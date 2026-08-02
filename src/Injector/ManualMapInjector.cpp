// src/Injector/ManualMapInjector.cpp
// Full manual-map pipeline: parse PE, allocate in target, copy sections,
// apply base relocations, resolve imports, execute entry point via APC.
//
// FOR EDUCATIONAL DEMONSTRATION ONLY.
#include "ManualMapInjector.h"
#include "CapcomDriver.h"
#include "KernelExec.h"

#include <windows.h>
#include <TlHelp32.h>
#include <algorithm>
#include <cstring>
#include <fstream>

// ---- Singleton ------------------------------------------------------------

ManualMapInjector& ManualMapInjector::GetInstance() {
    static ManualMapInjector instance;
    return instance;
}

// ---- Main Pipeline --------------------------------------------------------

bool ManualMapInjector::Inject(DWORD pid, const std::string& dllPath) {
    auto& capcom   = CapcomDriver::GetInstance();
    auto& kernExec = KernelExec::GetInstance();

    if (!capcom.IsLoaded() || !kernExec.IsInitialized())
        return false;

    // 1. Read DLL from disk
    std::ifstream file(dllPath, std::ios::binary | std::ios::ate);
    if (!file)
        return false;

    size_t fileSize = static_cast<size_t>(file.tellg());
    file.seekg(0);

    m_rawDll.resize(fileSize);
    file.read(reinterpret_cast<char*>(m_rawDll.data()), fileSize);
    file.close();

    // 2. Parse PE headers, sections, imports
    if (!ParsePE(m_rawDll))
        return false;

    // 3. Allocate memory in target (with extra slack for the APC stub)
    if (!AllocateTargetMemory(pid, m_preferredBase))
        return false;

    // 4. Copy headers and section data
    if (!CopyHeadersAndSections(pid))
        return false;

    // 5. Apply base relocations (delta = actualBase - preferredBase)
    if (!ApplyRelocations(pid))
        return false;

    // 6. Resolve IAT — walk target module list, parse export tables, write IAT
    if (!ResolveImports(pid))
        return false;

    // 7. Set per-section page protection (stub for MVP — all RWX)
    if (!SetSectionProtection(pid))
        return false;

    // 8. Execute DllMain via APC on a target thread
    if (!ExecuteEntryPoint(pid))
        return false;

    return true;
}

// ---- PE Parsing -----------------------------------------------------------

bool ManualMapInjector::ParsePE(const std::vector<uint8_t>& dllBytes) {
    if (dllBytes.size() < sizeof(IMAGE_DOS_HEADER))
        return false;

    std::memcpy(&m_dosHeader, dllBytes.data(), sizeof(IMAGE_DOS_HEADER));
    if (m_dosHeader.e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    if (dllBytes.size() < m_dosHeader.e_lfanew + sizeof(IMAGE_NT_HEADERS64))
        return false;

    std::memcpy(&m_ntHeaders,
                dllBytes.data() + m_dosHeader.e_lfanew,
                sizeof(IMAGE_NT_HEADERS64));

    if (m_ntHeaders.Signature != IMAGE_NT_SIGNATURE)
        return false;
    if (m_ntHeaders.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
        return false;

    // Parse section headers
    size_t sectionCount = m_ntHeaders.FileHeader.NumberOfSections;
    m_sections.resize(sectionCount);

    size_t sectionTableOffset = m_dosHeader.e_lfanew
                              + sizeof(IMAGE_NT_HEADERS64);

    for (size_t i = 0; i < sectionCount; ++i) {
        std::memcpy(&m_sections[i],
                    dllBytes.data() + sectionTableOffset
                        + i * sizeof(IMAGE_SECTION_HEADER),
                    sizeof(IMAGE_SECTION_HEADER));
    }

    m_preferredBase = m_ntHeaders.OptionalHeader.ImageBase;
    m_imageSize     = m_ntHeaders.OptionalHeader.SizeOfImage;
    m_entryPointRva = m_ntHeaders.OptionalHeader.AddressOfEntryPoint;

    // Helper: map an RVA to a pointer within the raw file bytes.
    // Uses the larger of SizeOfRawData / Misc.VirtualSize as the valid range
    // for the section — covers sections where VirtualSize > SizeOfRawData.
    auto rvaToRaw = [&](uint32_t rva) -> const uint8_t* {
        for (const auto& sec : m_sections) {
            uint32_t secEnd = sec.VirtualAddress
                            + std::max(sec.SizeOfRawData, sec.Misc.VirtualSize);
            if (rva >= sec.VirtualAddress && rva < secEnd) {
                // If rva falls past the raw data (e.g., .bss), there is
                // nothing in the file — return nullptr.
                if (rva - sec.VirtualAddress >= sec.SizeOfRawData)
                    return nullptr;
                uint32_t fileOff = sec.PointerToRawData
                                 + (rva - sec.VirtualAddress);
                return dllBytes.data() + fileOff;
            }
        }
        return nullptr;
    };

    // Parse import table
    auto& importDir = m_ntHeaders.OptionalHeader
                          .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

    if (importDir.VirtualAddress && importDir.Size) {
        const uint8_t* descCursor = rvaToRaw(importDir.VirtualAddress);
        if (descCursor) {
            while (true) {
                auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
                    descCursor);
                if (!desc->Name)
                    break;

                const char* dllNameRaw =
                    reinterpret_cast<const char*>(rvaToRaw(desc->Name));
                if (!dllNameRaw)
                    break;

                std::string dllName(dllNameRaw);

                // Use OriginalFirstThunk (INT) for names when available;
                // fall back to FirstThunk (IAT) if the INT is missing.
                uint32_t thunkRva = desc->OriginalFirstThunk
                                        ? desc->OriginalFirstThunk
                                        : desc->FirstThunk;
                const IMAGE_THUNK_DATA64* thunk =
                    reinterpret_cast<const IMAGE_THUNK_DATA64*>(
                        rvaToRaw(thunkRva));
                if (!thunk)
                    break;

                while (thunk->u1.AddressOfData) {
                    ImportEntry entry;
                    entry.dllName = dllName;

                    if (thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG64) {
                        entry.byOrdinal = true;
                        entry.hint = static_cast<uint16_t>(
                            thunk->u1.Ordinal & 0xFFFF);
                    } else {
                        auto* nameEntry =
                            reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                                rvaToRaw(static_cast<uint32_t>(
                                    thunk->u1.AddressOfData & 0x7FFFFFFF)));
                        if (nameEntry) {
                            entry.hint     = nameEntry->Hint;
                            entry.funcName = std::string(nameEntry->Name);
                        }
                    }

                    m_imports[dllName].push_back(entry);
                    ++thunk;
                }

                descCursor += sizeof(IMAGE_IMPORT_DESCRIPTOR);
            }
        }
    }

    return true;
}

// ---- Memory Allocation ----------------------------------------------------

bool ManualMapInjector::AllocateTargetMemory(DWORD pid,
                                              uintptr_t preferredBase) {
    auto& kernExec = KernelExec::GetInstance();

    // Allocate enough for the full PE image plus one extra page for the APC
    // stub that must reside in the *target* address space.
    const size_t allocSize = m_imageSize + 0x1000;

    m_mappedBase = 0;
    uint64_t status = kernExec.AllocateRemoteMemory(
        pid, m_mappedBase, allocSize, PAGE_EXECUTE_READWRITE);

    if (status != 0) {
        // Retry with no base preference (let the kernel choose the VA).
        m_mappedBase = 0;
        status = kernExec.AllocateRemoteMemory(
            pid, m_mappedBase, allocSize, PAGE_EXECUTE_READWRITE);
    }

    return (status == 0) && (m_mappedBase != 0);
}

// ---- Copy Headers & Sections ----------------------------------------------

bool ManualMapInjector::CopyHeadersAndSections(DWORD pid) {
    auto& capcom = CapcomDriver::GetInstance();

    // Write PE headers (first SizeOfHeaders bytes)
    if (!capcom.WriteMemory(pid, m_mappedBase, m_rawDll.data(),
                            m_ntHeaders.OptionalHeader.SizeOfHeaders))
        return false;

    // Write each section's raw data
    for (const auto& sec : m_sections) {
        if (sec.SizeOfRawData == 0)
            continue;

        uintptr_t destAddr = m_mappedBase + sec.VirtualAddress;
        const uint8_t* srcData = m_rawDll.data() + sec.PointerToRawData;

        if (!capcom.WriteMemory(pid, destAddr, srcData, sec.SizeOfRawData))
            return false;
    }

    return true;
}

// ---- Relocations ----------------------------------------------------------

bool ManualMapInjector::ApplyRelocations(DWORD pid) {
    auto& capcom = CapcomDriver::GetInstance();

    int64_t delta = static_cast<int64_t>(m_mappedBase)
                  - static_cast<int64_t>(m_preferredBase);
    if (delta == 0)
        return true; // loaded at the preferred base — nothing to fix up

    auto& relocDir =
        m_ntHeaders.OptionalHeader
            .DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (!relocDir.VirtualAddress || !relocDir.Size)
        return true;

    // Find the raw-file base of the relocation data
    const uint8_t* relocRaw = nullptr;
    for (const auto& sec : m_sections) {
        uint32_t secEnd = sec.VirtualAddress + sec.SizeOfRawData;
        if (relocDir.VirtualAddress >= sec.VirtualAddress &&
            relocDir.VirtualAddress < secEnd) {
            uint32_t off = relocDir.VirtualAddress - sec.VirtualAddress;
            relocRaw = m_rawDll.data() + sec.PointerToRawData + off;
            break;
        }
    }
    if (!relocRaw)
        return true;

    size_t processed = 0;
    size_t relocSize = relocDir.Size;

    while (processed < relocSize) {
        auto* block =
            reinterpret_cast<const IMAGE_BASE_RELOCATION*>(relocRaw + processed);
        if (!block->SizeOfBlock)
            break;

        size_t entryCount =
            (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
        const WORD* entries = reinterpret_cast<const WORD*>(block + 1);

        for (size_t i = 0; i < entryCount; ++i) {
            WORD entry  = entries[i];
            WORD type   = entry >> 12;
            WORD offset = entry & 0x0FFF;

            if (type == IMAGE_REL_BASED_DIR64) {
                uintptr_t patchAddr =
                    m_mappedBase + block->VirtualAddress + offset;
                uint64_t original = capcom.Read<uint64_t>(pid, patchAddr);
                capcom.Write<uint64_t>(pid, patchAddr, original + delta);
            }
            // IMAGE_REL_BASED_HIGH / LOW not relevant for AMD64.
        }

        processed += block->SizeOfBlock;
    }

    return true;
}

// ---- Import Resolution ----------------------------------------------------

bool ManualMapInjector::ResolveImports(DWORD pid) {
    auto& capcom = CapcomDriver::GetInstance();

    // Resolve every imported function address for every DLL.
    for (auto& [dllName, entries] : m_imports) {
        uintptr_t moduleBase = FindModuleInTarget(pid, dllName);
        if (!moduleBase)
            return false;

        for (auto& entry : entries) {
            const std::string& lookupName = entry.byOrdinal
                ? std::string("#") + std::to_string(entry.hint)
                : entry.funcName;

            entry.resolvedAddr = FindExportInTarget(pid, moduleBase, lookupName);
            if (!entry.resolvedAddr)
                return false;
        }
    }

    // Now write the resolved addresses into the IAT in the target.
    // We walk the PE import descriptors directly (NOT a flattened vector) so
    // that DLL-specific entries match the IAT layout in the mapped image.
    auto& importDir =
        m_ntHeaders.OptionalHeader
            .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!importDir.VirtualAddress)
        return true;

    // Locate the import descriptors in the raw file bytes.
    const uint8_t* importSectionBase = nullptr;
    uintptr_t importSectionRva = 0;
    for (const auto& sec : m_sections) {
        if (importDir.VirtualAddress >= sec.VirtualAddress &&
            importDir.VirtualAddress < sec.VirtualAddress + sec.SizeOfRawData) {
            importSectionBase = m_rawDll.data() + sec.PointerToRawData;
            importSectionRva  = sec.VirtualAddress;
            break;
        }
    }
    if (!importSectionBase)
        return true;

    size_t descFileOff = importDir.VirtualAddress - importSectionRva;
    auto* importDesc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
        importSectionBase + descFileOff);

    // Walk each import descriptor, match its DLL name, and patch the IAT.
    while (importDesc->Name != 0) {
        const char* dllNameRaw = reinterpret_cast<const char*>(
            importSectionBase + (importDesc->Name - importSectionRva));
        std::string dllName(dllNameRaw);

        auto it = m_imports.find(dllName);
        if (it == m_imports.end()) {
            ++importDesc;
            continue;
        }

        const auto& entries = it->second;
        uintptr_t iatAddr = m_mappedBase + importDesc->FirstThunk;

        for (size_t i = 0; i < entries.size(); ++i) {
            uintptr_t thunkAddr = iatAddr + i * sizeof(uint64_t);
            capcom.Write<uint64_t>(pid, thunkAddr, entries[i].resolvedAddr);
        }

        ++importDesc;
    }

    return true;
}

uintptr_t ManualMapInjector::FindModuleInTarget(DWORD pid,
                                                 const std::string& moduleName) {
    // Use a host-side toolhelp snapshot to enumerate modules in the target
    // process.  Module32First/Next report the module base address in the
    // *target's* virtual address space, which is exactly what we need.
    HANDLE hSnapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnapshot == INVALID_HANDLE_VALUE)
        return 0;

    uintptr_t base = 0;
    MODULEENTRY32 me32{};
    me32.dwSize = sizeof(MODULEENTRY32);

    if (Module32First(hSnapshot, &me32)) {
        do {
            if (_stricmp(me32.szModule, moduleName.c_str()) == 0) {
                base = reinterpret_cast<uintptr_t>(me32.modBaseAddr);
                break;
            }
        } while (Module32Next(hSnapshot, &me32));
    }

    CloseHandle(hSnapshot);
    return base;
}

uintptr_t ManualMapInjector::FindExportInTarget(DWORD pid,
                                                  uintptr_t moduleBase,
                                                  const std::string& funcName) {
    auto& capcom = CapcomDriver::GetInstance();

    IMAGE_DOS_HEADER dos = capcom.Read<IMAGE_DOS_HEADER>(pid, moduleBase);
    if (dos.e_magic != IMAGE_DOS_SIGNATURE)
        return 0;

    IMAGE_NT_HEADERS64 nt =
        capcom.Read<IMAGE_NT_HEADERS64>(pid, moduleBase + dos.e_lfanew);
    if (nt.Signature != IMAGE_NT_SIGNATURE)
        return 0;

    auto& expDir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!expDir.VirtualAddress || !expDir.Size)
        return 0;

    IMAGE_EXPORT_DIRECTORY exp =
        capcom.Read<IMAGE_EXPORT_DIRECTORY>(pid,
                                             moduleBase + expDir.VirtualAddress);

    uintptr_t namesBase = moduleBase + exp.AddressOfNames;
    uintptr_t ordsBase  = moduleBase + exp.AddressOfNameOrdinals;
    uintptr_t funcsBase = moduleBase + exp.AddressOfFunctions;

    // --- Ordinal lookup ----------------------------------------------------
    if (funcName[0] == '#') {
        DWORD ordinal = std::stoul(funcName.substr(1));
        DWORD funcRva = capcom.Read<DWORD>(
            pid, funcsBase + (ordinal - exp.Base) * sizeof(DWORD));

        if (funcRva >= expDir.VirtualAddress &&
            funcRva < expDir.VirtualAddress + expDir.Size) {
            // Forwarded export — resolve recursively.
            char fwdBuf[256] = {};
            capcom.ReadMemory(pid, moduleBase + funcRva,
                              fwdBuf, sizeof(fwdBuf) - 1);
            return ResolveForwardedExport(pid, std::string(fwdBuf));
        }

        return moduleBase + funcRva;
    }

    // --- Name lookup -------------------------------------------------------
    for (DWORD i = 0; i < exp.NumberOfNames; ++i) {
        DWORD nameRva =
            capcom.Read<DWORD>(pid, namesBase + i * sizeof(DWORD));
        char nameBuf[256] = {};
        capcom.ReadMemory(pid, moduleBase + nameRva,
                          nameBuf, sizeof(nameBuf) - 1);

        if (funcName == nameBuf) {
            WORD ordinal =
                capcom.Read<WORD>(pid, ordsBase + i * sizeof(WORD));
            DWORD funcRva =
                capcom.Read<DWORD>(pid, funcsBase + ordinal * sizeof(DWORD));

            // Forwarded export check:
            // If the function RVA lies inside the export directory range,
            // the "function" is actually a forwarder string like
            // "ntdll.RtlAllocateHeap" — resolve it recursively.
            if (funcRva >= expDir.VirtualAddress &&
                funcRva < expDir.VirtualAddress + expDir.Size) {
                char fwdBuf[256] = {};
                capcom.ReadMemory(pid, moduleBase + funcRva,
                                  fwdBuf, sizeof(fwdBuf) - 1);
                return ResolveForwardedExport(pid, std::string(fwdBuf));
            }

            return moduleBase + funcRva;
        }
    }

    return 0;
}

uintptr_t ManualMapInjector::ResolveForwardedExport(
    DWORD pid, const std::string& forwarder) {
    // Format: "ModuleName.FunctionName"  (e.g. "ntdll.RtlAllocateHeap")
    size_t dot = forwarder.find('.');
    if (dot == std::string::npos)
        return 0;

    std::string dllName = forwarder.substr(0, dot) + ".dll";
    std::string funcName = forwarder.substr(dot + 1);

    uintptr_t fwdModule = FindModuleInTarget(pid, dllName);
    if (!fwdModule)
        return 0;

    return FindExportInTarget(pid, fwdModule, funcName);
}

// ---- Section Protection ---------------------------------------------------

bool ManualMapInjector::SetSectionProtection(DWORD pid) {
    // Phase 2 MVP: leave all sections as PAGE_EXECUTE_READWRITE (the default
    // set during AllocateTargetMemory).
    //
    // Phase 3 will implement per-section protection:
    //   .text  → PAGE_EXECUTE_READ
    //   .rdata → PAGE_READONLY
    //   .data  → PAGE_READWRITE
    //   etc.
    (void)pid;
    return true;
}

// ---- Entry Point Execution ------------------------------------------------

bool ManualMapInjector::ExecuteEntryPoint(DWORD pid) {
    if (!m_entryPointRva)
        return true; // resource-only DLL, no DllMain to call

    auto& capcom  = CapcomDriver::GetInstance();
    uintptr_t entryAddr = m_mappedBase + m_entryPointRva;

    // ------------------------------------------------------------------
    //  APC stub shellcode (41 bytes):
    //    +0x00  48 83 EC 28                     sub  rsp, 0x28
    //    +0x04  48 B9 <hModule>                  mov  rcx, imm64      ; hModule
    //    +0x0E  48 C7 C2 01 00 00 00            mov  rdx, 1          ; fdwReason
    //    +0x15  4D 31 C0                        xor  r8, r8           ; lpReserved
    //    +0x18  48 B8 <entryAddr>               mov  rax, imm64      ; DllMain
    //    +0x22  FF D0                           call rax
    //    +0x24  48 83 C4 28                     add  rsp, 0x28
    //    +0x28  C3                              ret
    //
    //  Patch points:
    //    offset  6 → 8-byte hModule   (after opcode 48 B9)
    //    offset 26 → 8-byte entryAddr (after opcode 48 B8)
    // ------------------------------------------------------------------
    uint8_t apcStub[] = {
        0x48, 0x83, 0xEC, 0x28,                   // sub  rsp, 0x28
        0x48, 0xB9, 0x00, 0x00, 0x00, 0x00,       // mov  rcx, <hModule>
        0x00, 0x00, 0x00, 0x00,
        0x48, 0xC7, 0xC2, 0x01, 0x00, 0x00, 0x00, // mov  rdx, 1
        0x4D, 0x31, 0xC0,                         // xor  r8,  r8
        0x48, 0xB8, 0x00, 0x00, 0x00, 0x00,       // mov  rax, <entryAddr>
        0x00, 0x00, 0x00, 0x00,
        0xFF, 0xD0,                               // call rax
        0x48, 0x83, 0xC4, 0x28,                   // add  rsp, 0x28
        0xC3                                        // ret
    };
    static_assert(sizeof(apcStub) == 41,
                  "APC stub size mismatch — offsets need re-verification");

    // Poke the runtime values into the shellcode.
    std::memcpy(apcStub + 6,  &m_mappedBase, sizeof(m_mappedBase));
    std::memcpy(apcStub + 26, &entryAddr,    sizeof(entryAddr));

    // Place the stub inside the already-allocated target region, right
    // after the PE image.  This guarantees the stub is accessible in the
    // target process when QueueUserAPC fires.
    //
    // NOTE: KernelExec::AllocateRemoteMemory currently allocates in the
    // calling-process context (pseudo-handle -1).  We deliberately reuse
    // the PE-region allocation (which was written to the target via
    // CapcomDriver) instead of making a separate allocation that would
    // only be valid in the injector process.
    constexpr size_t kStubAlign = 16;
    uintptr_t stubAddr = m_mappedBase + m_imageSize;
    stubAddr = (stubAddr + kStubAlign - 1) & ~(kStubAlign - 1ULL);

    capcom.WriteMemory(pid, stubAddr, apcStub, sizeof(apcStub));

    // Queue the APC to a target thread.
    return QueueUserApcToTarget(pid, stubAddr, 0);
}

bool ManualMapInjector::QueueUserApcToTarget(DWORD pid,
                                               uintptr_t apcRoutine,
                                               uintptr_t argument) {
    // Enumerate threads belonging to the target process and queue an APC
    // to the first thread we can open with sufficient rights.
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
        return false;

    THREADENTRY32 te32{};
    te32.dwSize = sizeof(THREADENTRY32);

    bool queued = false;

    if (Thread32First(hSnapshot, &te32)) {
        do {
            if (te32.th32OwnerProcessID != pid)
                continue;

            HANDLE hThread = OpenThread(
                THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                FALSE, te32.th32ThreadID);
            if (!hThread)
                continue;

            if (QueueUserAPC(reinterpret_cast<PAPCFUNC>(apcRoutine),
                             hThread, argument)) {
                queued = true;
            }

            CloseHandle(hThread);
            if (queued)
                break;
        } while (Thread32Next(hSnapshot, &te32));
    }

    CloseHandle(hSnapshot);
    return queued;
}
