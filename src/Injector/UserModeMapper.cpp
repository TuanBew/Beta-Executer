// src/Injector/UserModeMapper.cpp
// Pure user-mode PE manual mapper with thread hijacking.
//
// Pipeline:
//   ParsePE → VirtualAllocEx → WriteProcessMemory (headers+sections) →
//   ApplyRelocations (RPM/WPM) → ResolveImports (Toolhelp+RPM/WPM) →
//   BuildHijackShellcode → SelectTargetThread → Suspend→SetContext→Resume
//
// FOR EDUCATIONAL DEMONSTRATION ONLY.
#include "UserModeMapper.h"

#include <windows.h>
#include <TlHelp32.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

// NtQueryInformationThread — used to skip threads with pending I/O.
// Declared locally; ntdll is already linked by the project.
#ifndef NTSTATUS
using NTSTATUS = LONG;
#endif
#ifndef ThreadIsIoPending
#define ThreadIsIoPending 0x10
#endif
extern "C" NTSTATUS NTAPI NtQueryInformationThread(
    HANDLE ThreadHandle, ULONG ThreadInformationClass,
    PVOID ThreadInformation, ULONG ThreadInformationLength,
    PULONG ReturnLength);

// ---- Singleton ------------------------------------------------------------

UserModeMapper& UserModeMapper::GetInstance() {
    static UserModeMapper instance;
    return instance;
}

// ---- Main Pipeline --------------------------------------------------------

bool UserModeMapper::Inject(DWORD pid, const std::string& dllPath) {
    fprintf(stderr, "[UMM-DBG] Inject(pid=%lu, '%s')\n", pid, dllPath.c_str());
    fflush(stderr);

    // 1. Read DLL from disk
    std::ifstream file(dllPath, std::ios::binary | std::ios::ate);
    if (!file) {
        fprintf(stderr, "[UMM-DBG] ERROR: cannot open file\n");
        return false;
    }

    size_t fileSize = static_cast<size_t>(file.tellg());
    file.seekg(0);
    m_rawDll.resize(fileSize);
    file.read(reinterpret_cast<char*>(m_rawDll.data()), fileSize);
    file.close();
    fprintf(stderr, "[UMM-DBG] Read %zu bytes from file\n", fileSize);

    // 2. Open target process
    HANDLE hProcess = OpenProcess(
        PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
        PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if (!hProcess) {
        fprintf(stderr, "[UMM-DBG] ERROR: OpenProcess failed (error=%lu)\n",
                GetLastError());
        return false;
    }
    fprintf(stderr, "[UMM-DBG] OpenProcess OK: hProcess=0x%p\n", hProcess);

    // 3. Parse PE
    if (!ParsePE(m_rawDll)) {
        fprintf(stderr, "[UMM-DBG] ERROR: ParsePE failed\n");
        CloseHandle(hProcess);
        return false;
    }
    fprintf(stderr, "[UMM-DBG] ParsePE OK: preferredBase=0x%llX imageSize=%zu entryRva=0x%llX sections=%zu imports=%zu\n",
            m_preferredBase, m_imageSize, m_entryPointRva, m_sections.size(), m_imports.size());

    // 4. Allocate memory in target (image + one page for shellcode/stack)
    if (!AllocateTargetMemory(hProcess)) {
        fprintf(stderr, "[UMM-DBG] ERROR: AllocateTargetMemory failed\n");
        CloseHandle(hProcess);
        return false;
    }
    fprintf(stderr, "[UMM-DBG] Allocate OK: mappedBase=0x%llX\n", m_mappedBase);

    // 5. Copy headers and sections
    if (!CopyHeadersAndSections(hProcess)) {
        fprintf(stderr, "[UMM-DBG] ERROR: CopyHeadersAndSections failed\n");
        VirtualFreeEx(hProcess, reinterpret_cast<LPVOID>(m_mappedBase), 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }
    fprintf(stderr, "[UMM-DBG] CopyHeadersAndSections OK\n");

    // 6. Apply base relocations
    if (!ApplyRelocations(hProcess)) {
        fprintf(stderr, "[UMM-DBG] ERROR: ApplyRelocations failed\n");
        VirtualFreeEx(hProcess, reinterpret_cast<LPVOID>(m_mappedBase), 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }
    fprintf(stderr, "[UMM-DBG] ApplyRelocations OK\n");

    // 7. Resolve imports
    if (!ResolveImports(hProcess, pid)) {
        fprintf(stderr, "[UMM-DBG] ERROR: ResolveImports failed\n");
        VirtualFreeEx(hProcess, reinterpret_cast<LPVOID>(m_mappedBase), 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }
    fprintf(stderr, "[UMM-DBG] ResolveImports OK\n");

    // 8. Hijack thread to execute DllMain
    if (!ExecuteEntryPoint(hProcess, pid)) {
        fprintf(stderr, "[UMM-DBG] ERROR: ExecuteEntryPoint failed\n");
        VirtualFreeEx(hProcess, reinterpret_cast<LPVOID>(m_mappedBase), 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }
    fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint OK — DllMain called\n");

    CloseHandle(hProcess);
    fprintf(stderr, "[UMM-DBG] Inject SUCCESS\n");
    return true;
}

// ---- PE Parsing -----------------------------------------------------------
// Adapted from ManualMapInjector::ParsePE — identical logic, different class.

bool UserModeMapper::ParsePE(const std::vector<uint8_t>& dllBytes) {
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

    // Helper: map RVA → pointer within raw file bytes.
    auto rvaToRaw = [&](uint32_t rva) -> const uint8_t* {
        for (const auto& sec : m_sections) {
            uint32_t secEnd = sec.VirtualAddress
                            + (std::max)(sec.SizeOfRawData, sec.Misc.VirtualSize);
            if (rva >= sec.VirtualAddress && rva < secEnd) {
                if (rva - sec.VirtualAddress >= sec.SizeOfRawData)
                    return nullptr; // BSS / uninitialized data
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

bool UserModeMapper::AllocateTargetMemory(HANDLE hProcess) {
    // Allocate enough for the PE image plus one extra page for shellcode/stack.
    const size_t allocSize = m_imageSize + 0x1000;

    m_mappedBase = reinterpret_cast<uintptr_t>(
        VirtualAllocEx(hProcess,
                       reinterpret_cast<LPVOID>(m_preferredBase),
                       allocSize,
                       MEM_COMMIT | MEM_RESERVE,
                       PAGE_EXECUTE_READWRITE));

    if (!m_mappedBase) {
        // Retry with no base preference (let the kernel choose).
        m_mappedBase = reinterpret_cast<uintptr_t>(
            VirtualAllocEx(hProcess, nullptr, allocSize,
                           MEM_COMMIT | MEM_RESERVE,
                           PAGE_EXECUTE_READWRITE));
    }

    return m_mappedBase != 0;
}

// ---- Copy Headers & Sections ----------------------------------------------

bool UserModeMapper::CopyHeadersAndSections(HANDLE hProcess) {
    SIZE_T written = 0;

    // Write PE headers (first SizeOfHeaders bytes)
    if (!WriteProcessMemory(hProcess,
                            reinterpret_cast<LPVOID>(m_mappedBase),
                            m_rawDll.data(),
                            m_ntHeaders.OptionalHeader.SizeOfHeaders,
                            &written))
        return false;

    // Write each section's raw data
    for (const auto& sec : m_sections) {
        if (sec.SizeOfRawData == 0)
            continue;

        uintptr_t destAddr = m_mappedBase + sec.VirtualAddress;
        const uint8_t* srcData = m_rawDll.data() + sec.PointerToRawData;

        if (!WriteProcessMemory(hProcess,
                                reinterpret_cast<LPVOID>(destAddr),
                                srcData, sec.SizeOfRawData, &written))
            return false;
    }

    return true;
}

// ---- Relocations ----------------------------------------------------------

bool UserModeMapper::ApplyRelocations(HANDLE hProcess) {
    int64_t delta = static_cast<int64_t>(m_mappedBase)
                  - static_cast<int64_t>(m_preferredBase);
    if (delta == 0)
        return true; // loaded at preferred base — nothing to fix up

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

                // Read current value, add delta, write back
                uint64_t original = 0;
                SIZE_T bytesRead = 0;
                if (ReadProcessMemory(hProcess,
                                      reinterpret_cast<LPCVOID>(patchAddr),
                                      &original, sizeof(original), &bytesRead)) {
                    uint64_t fixed = original + delta;
                    WriteProcessMemory(hProcess,
                                       reinterpret_cast<LPVOID>(patchAddr),
                                       &fixed, sizeof(fixed), &bytesRead);
                }
            }
        }

        processed += block->SizeOfBlock;
    }

    return true;
}

// ---- Import Resolution ----------------------------------------------------
// FindModuleInTarget uses CreateToolhelp32Snapshot (user-mode, no kernel needed).
// FindExportInTarget uses ReadProcessMemory to walk the target's export table.

bool UserModeMapper::ResolveImports(HANDLE hProcess, DWORD pid) {
    // Resolve every imported function address for every DLL.
    for (auto& [dllName, entries] : m_imports) {
        uintptr_t moduleBase = FindModuleInTarget(pid, dllName);
        if (!moduleBase) {
            fprintf(stderr, "[UMM-DBG] ResolveImports: module '%s' not found in target\n",
                    dllName.c_str());
            return false;
        }

        for (auto& entry : entries) {
            const std::string& lookupName = entry.byOrdinal
                ? std::string("#") + std::to_string(entry.hint)
                : entry.funcName;

            entry.resolvedAddr = FindExportInTarget(hProcess, moduleBase, lookupName);
            if (!entry.resolvedAddr) {
                fprintf(stderr, "[UMM-DBG] ResolveImports: '%s'!%s NOT FOUND\n",
                        dllName.c_str(), lookupName.c_str());
                return false;
            }
        }
    }

    // Write resolved addresses into the IAT in the target.
    auto& importDir =
        m_ntHeaders.OptionalHeader
            .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!importDir.VirtualAddress)
        return true;

    // Locate the import descriptors in the raw file to walk DLL order.
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
            SIZE_T written = 0;
            uint64_t resolved = entries[i].resolvedAddr;
            WriteProcessMemory(hProcess,
                               reinterpret_cast<LPVOID>(thunkAddr),
                               &resolved, sizeof(resolved), &written);
        }

        ++importDesc;
    }

    return true;
}

uintptr_t UserModeMapper::FindModuleInTarget(DWORD pid,
                                               const std::string& moduleName) {
    // Toolhelp module snapshot — reports base addresses in the target's VA space.
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

uintptr_t UserModeMapper::FindExportInTarget(HANDLE hProcess,
                                                uintptr_t moduleBase,
                                                const std::string& funcName) {
    // Read DOS header
    IMAGE_DOS_HEADER dos{};
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(moduleBase),
                           &dos, sizeof(dos), &bytesRead))
        return 0;
    if (dos.e_magic != IMAGE_DOS_SIGNATURE)
        return 0;

    // Read NT headers
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadProcessMemory(hProcess,
                           reinterpret_cast<LPCVOID>(moduleBase + dos.e_lfanew),
                           &nt, sizeof(nt), &bytesRead))
        return 0;
    if (nt.Signature != IMAGE_NT_SIGNATURE)
        return 0;

    auto& expDir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!expDir.VirtualAddress || !expDir.Size)
        return 0;

    // Read export directory
    IMAGE_EXPORT_DIRECTORY exp{};
    if (!ReadProcessMemory(hProcess,
                           reinterpret_cast<LPCVOID>(moduleBase + expDir.VirtualAddress),
                           &exp, sizeof(exp), &bytesRead))
        return 0;

    uintptr_t namesBase = moduleBase + exp.AddressOfNames;
    uintptr_t ordsBase  = moduleBase + exp.AddressOfNameOrdinals;
    uintptr_t funcsBase = moduleBase + exp.AddressOfFunctions;

    // Ordinal lookup
    if (funcName[0] == '#') {
        DWORD ordinal = std::stoul(funcName.substr(1));
        DWORD funcRva = 0;
        if (!ReadProcessMemory(hProcess,
                               reinterpret_cast<LPCVOID>(
                                   funcsBase + (ordinal - exp.Base) * sizeof(DWORD)),
                               &funcRva, sizeof(funcRva), &bytesRead))
            return 0;

        if (funcRva >= expDir.VirtualAddress &&
            funcRva < expDir.VirtualAddress + expDir.Size) {
            // Forwarded export
            char fwdBuf[256] = {};
            ReadProcessMemory(hProcess,
                              reinterpret_cast<LPCVOID>(moduleBase + funcRva),
                              fwdBuf, sizeof(fwdBuf) - 1, &bytesRead);
            return ResolveForwardedExport(hProcess, 0, std::string(fwdBuf));
        }

        return moduleBase + funcRva;
    }

    // Name lookup — binary search is possible but linear is fine for our IAT sizes.
    for (DWORD i = 0; i < exp.NumberOfNames; ++i) {
        DWORD nameRva = 0;
        if (!ReadProcessMemory(hProcess,
                               reinterpret_cast<LPCVOID>(namesBase + i * sizeof(DWORD)),
                               &nameRva, sizeof(nameRva), &bytesRead))
            continue;

        char nameBuf[256] = {};
        ReadProcessMemory(hProcess,
                          reinterpret_cast<LPCVOID>(moduleBase + nameRva),
                          nameBuf, sizeof(nameBuf) - 1, &bytesRead);

        if (funcName == nameBuf) {
            WORD ordinal = 0;
            if (!ReadProcessMemory(hProcess,
                                   reinterpret_cast<LPCVOID>(ordsBase + i * sizeof(WORD)),
                                   &ordinal, sizeof(ordinal), &bytesRead))
                continue;

            DWORD funcRva = 0;
            if (!ReadProcessMemory(hProcess,
                                   reinterpret_cast<LPCVOID>(funcsBase + ordinal * sizeof(DWORD)),
                                   &funcRva, sizeof(funcRva), &bytesRead))
                continue;

            // Forwarded export check
            if (funcRva >= expDir.VirtualAddress &&
                funcRva < expDir.VirtualAddress + expDir.Size) {
                char fwdBuf[256] = {};
                ReadProcessMemory(hProcess,
                                  reinterpret_cast<LPCVOID>(moduleBase + funcRva),
                                  fwdBuf, sizeof(fwdBuf) - 1, &bytesRead);
                return ResolveForwardedExport(hProcess, 0, std::string(fwdBuf));
            }

            return moduleBase + funcRva;
        }
    }

    return 0;
}

uintptr_t UserModeMapper::ResolveForwardedExport(
    HANDLE hProcess, DWORD pid, const std::string& forwarder) {
    // Format: "ModuleName.FunctionName" (e.g. "ntdll.RtlAllocateHeap")
    size_t dot = forwarder.find('.');
    if (dot == std::string::npos)
        return 0;

    std::string dllName = forwarder.substr(0, dot) + ".dll";
    std::string funcName = forwarder.substr(dot + 1);

    // Need pid for FindModuleInTarget — the second arg is unused in
    // ResolveForwardedExport's current signature but needed here.
    // We resolve via Toolhelp which needs the pid.
    // Workaround: enumerate processes to find pid from hProcess... but that's
    // fragile. Instead, we take a different approach: pass pid through.
    //
    // For now: we don't have pid in this function signature. The pid parameter
    // is currently 0 (unused). Forwarded exports are rare in the DLLs PayloadDLL
    // imports (kernel32, ntdll, user32). If we hit one, we need the pid.
    //
    // Quick fix: resolve using local GetProcAddress since forwarded exports
    // have the same address in the host process (same kernel32/ntdll session).
    (void)pid; // currently unused — see below
    HMODULE hLocalMod = GetModuleHandleA(dllName.c_str());
    if (hLocalMod) {
        if (funcName[0] == '#') {
            // Ordinal — can't easily resolve locally; fall back.
            return 0;
        }
        FARPROC proc = GetProcAddress(hLocalMod, funcName.c_str());
        if (proc) {
            // GetProcAddress returns host address. Since kernel32/ntdll are
            // loaded at the same base in all processes per session, this IS
            // the target address.
            return reinterpret_cast<uintptr_t>(proc);
        }
    }

    return 0;
}

// ---- Thread Hijacking -----------------------------------------------------

DWORD UserModeMapper::SelectTargetThread(DWORD pid, HANDLE hProcess) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnap == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[UMM-DBG] SelectTargetThread: snapshot failed (err=%lu)\n",
                GetLastError());
        return 0;
    }

    THREADENTRY32 te{sizeof(te)};
    DWORD chosenTid = 0;
    DWORD fallbackTid = 0;
    bool firstFound = false;

    if (Thread32First(hSnap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid)
                continue;

            // Skip the first thread (main/GUI thread) — hijacking it could
            // cause visible stutter if DllMain takes any time at all.
            if (!firstFound) {
                firstFound = true;
                continue;
            }

            HANDLE hThread = OpenThread(
                THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
                FALSE, te.th32ThreadID);
            if (!hThread)
                continue;

            // Check for pending I/O — hijacking a thread mid-I/O is risky.
            ULONG isIoPending = 0;
            NTSTATUS st = NtQueryInformationThread(
                hThread, ThreadIsIoPending, &isIoPending,
                sizeof(isIoPending), nullptr);
            if (st >= 0 && isIoPending) {
                CloseHandle(hThread);
                continue;
            }

            // Check if already suspended — must be able to resume it exactly once.
            DWORD prevCount = SuspendThread(hThread);
            if (prevCount == (DWORD)-1 || prevCount > 0) {
                // prevCount > 0 means it was already suspended — skip.
                if (prevCount != (DWORD)-1 && prevCount > 0)
                    ResumeThread(hThread); // undo our SuspendThread
                CloseHandle(hThread);
                continue;
            }

            // Get context to inspect RIP
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            bool gotContext = GetThreadContext(hThread, &ctx) != 0;

            // Resume immediately — we'll re-suspend when we choose the winner.
            ResumeThread(hThread);

            if (!gotContext) {
                CloseHandle(hThread);
                continue;
            }

            // Check if RIP is in a safe wait function (preferred).
            if (IsRipInSafeWait(ctx.Rip, hProcess)) {
                chosenTid = te.th32ThreadID;
                CloseHandle(hThread);
                fprintf(stderr, "[UMM-DBG] SelectTargetThread: preferred TID=%lu (in safe wait)\n",
                        chosenTid);
                break;
            }

            // Fallback: accept any thread outside the main module.
            if (!fallbackTid) {
                fallbackTid = te.th32ThreadID;
                fprintf(stderr, "[UMM-DBG] SelectTargetThread: fallback TID=%lu (not in safe wait)\n",
                        fallbackTid);
            }

            CloseHandle(hThread);
        } while (Thread32Next(hSnap, &te));
    }

    CloseHandle(hSnap);

    if (chosenTid)
        return chosenTid;
    if (fallbackTid)
        return fallbackTid;

    fprintf(stderr, "[UMM-DBG] SelectTargetThread: NO suitable thread found\n");
    return 0;
}

bool UserModeMapper::IsRipInSafeWait(uintptr_t rip, HANDLE hProcess) {
    // Resolve known ntdll wait functions on first call.
    // These are safe to hijack because the thread is idle in kernel-mode wait.
    if (!m_safeWaitResolved) {
        m_safeWaitResolved = true;

        // ntdll is loaded at the same base in all processes per session.
        // We can use our own GetModuleHandle + GetProcAddress.
        HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
        if (!hNtdll)
            return false;

        const char* waitFuncs[] = {
            "NtWaitForSingleObject",
            "NtWaitForMultipleObjects",
            "NtDelayExecution",
            "NtRemoveIoCompletion",
            "NtRemoveIoCompletionEx",
            "NtWaitForAlertByThreadId",
            "NtAlpcSendWaitReceivePort",
            "NtRequestWaitReplyPort",
        };

        for (const char* name : waitFuncs) {
            FARPROC addr = GetProcAddress(hNtdll, name);
            if (addr) {
                m_safeWaitAddrs.push_back(reinterpret_cast<uintptr_t>(addr));
            }
        }
    }

    // Check if RIP is within ~0x40 bytes of any known wait function entry.
    // The thread may be at the entry point or a few instructions in.
    for (uintptr_t waitAddr : m_safeWaitAddrs) {
        if (rip >= waitAddr && rip < waitAddr + 0x40)
            return true;
    }

    return false;
}

std::vector<uint8_t> UserModeMapper::BuildHijackShellcode(
    uintptr_t mappedBase, uintptr_t entryAddr,
    uintptr_t originalRip, uintptr_t originalRsp) {

    // Shellcode page layout:
    //   Offset 0x000: code (~75 bytes)
    //   Offset 0x300: data slots (6 × 8 = 48 bytes)
    //   Offset 0x800-0x1000: stack space (grows down from top)
    //
    // Data slot layout (offset from data base = r11 after lea r11,[rip+data]):
    //   +0x00: hModule       (mapped DLL base)
    //   +0x08: originalRip   (thread's original RIP)
    //   +0x10: dllMain       (mappedBase + entryPointRva)
    //   +0x18: originalRsp   (thread's original RSP)
    //   +0x20: savedRsp      (shellcode stores original RSP here at runtime)
    //   +0x28: stackTop      (top of our stack page)
    //   +0x30: done marker   (shellcode writes 1 here before jmp back)

    constexpr uint32_t DATA_OFFSET = 0x300;  // data region within the shellcode page
    constexpr uint32_t STACK_TOP  = 0x1000; // stack grows down from page top

    uintptr_t codePage = mappedBase + m_imageSize;
    // Align code page
    codePage = (codePage + 15) & ~15ULL;
    uintptr_t dataBase = codePage + DATA_OFFSET;
    uintptr_t stackBase = codePage + STACK_TOP - 8; // -8 for alignment safety

    // Build the code section.
    // lea r11, [rip + data] needs: offset = dataBase - (codePage + 7)
    // Encoding: 48 8D 1D <disp32>
    int32_t leaDisp = static_cast<int32_t>(dataBase - (codePage + 7));

    // After all 7 pushes (56 bytes) + sub rsp,0x20 (32 bytes) = 88 bytes
    // from entry RSP. If entry RSP was 16-byte aligned (standard), then
    // 88 % 16 = 8 → NOT aligned. Need one more adjustment.
    // Fix: after pushfq, do `sub rsp, 8` to add 8 bytes → total 96 = 6*16.
    // Wait, I already have sub rsp,0x20 earlier. Let me restructure:
    //
    // Before pushes: RSP is 16-byte aligned at call site (standard)
    // sub rsp, 0x20         : -32 bytes
    // 7 pushes              : -56 bytes
    // Total                 : -88 bytes, RSP & 0xF == 8 (misaligned)
    //
    // Fix: make shadow space 0x28 instead of 0x20:
    // sub rsp, 0x28         : -40 bytes
    // 7 pushes              : -56 bytes
    // Total                 : -96 bytes, RSP & 0xF == 0 (aligned!) ✓

    uint8_t code[] = {
        // ── 1. Data base pointer ──
        // lea r11, [rip + data] — RIP-relative, patched with leaDisp
        0x48, 0x8D, 0x1D,
        static_cast<uint8_t>(leaDisp & 0xFF),
        static_cast<uint8_t>((leaDisp >> 8) & 0xFF),
        static_cast<uint8_t>((leaDisp >> 16) & 0xFF),
        static_cast<uint8_t>((leaDisp >> 24) & 0xFF),

        // ── 2. Save original RSP → data+0x20 ──
        // mov [r11 + 0x20], rsp
        0x49, 0x89, 0x63, 0x20,

        // ── 3. Switch to our stack ──
        // mov rsp, [r11 + 0x28]
        0x49, 0x8B, 0x63, 0x28,

        // ── 4. Shadow space (40 bytes — maintains 16-byte alignment) ──
        // sub rsp, 0x28
        0x48, 0x83, 0xEC, 0x28,

        // ── 5. Save volatile registers ──
        // push rax
        0x50,
        // push rcx
        0x51,
        // push rdx
        0x52,
        // push r8
        0x41, 0x50,
        // push r9
        0x41, 0x51,
        // push r10
        0x41, 0x52,
        // push r11  (save data pointer — call may clobber it)
        0x41, 0x53,
        // pushfq
        0x9C,

        // ── 6. Call DllMain(hModule, DLL_PROCESS_ATTACH, NULL) ──
        // mov rcx, [r11 + 0x00]  — hModule
        0x49, 0x8B, 0x4B, 0x00,
        // mov rdx, 1  — fdwReason = DLL_PROCESS_ATTACH
        0x48, 0xC7, 0xC2, 0x01, 0x00, 0x00, 0x00,
        // xor r8, r8  — lpReserved = NULL
        0x4D, 0x31, 0xC0,
        // mov rax, [r11 + 0x10]  — DllMain address
        0x49, 0x8B, 0x43, 0x10,
        // call rax
        0xFF, 0xD0,

        // ── 7. Restore volatile registers (reverse order) ──
        // popfq
        0x9D,
        // pop r11  (restore data pointer)
        0x41, 0x5B,
        // pop r10
        0x41, 0x5A,
        // pop r9
        0x41, 0x59,
        // pop r8
        0x41, 0x58,
        // pop rdx
        0x5A,
        // pop rcx
        0x59,
        // pop rax
        0x58,

        // ── 8. Restore shadow space ──
        // add rsp, 0x28
        0x48, 0x83, 0xC4, 0x28,

        // ── 9. Restore original RSP ──
        // mov rsp, [r11 + 0x20]
        0x49, 0x8B, 0x63, 0x20,

        // ── 10. Set done marker ──
        // mov qword [r11 + 0x30], 1
        0x49, 0xC7, 0x43, 0x30, 0x01, 0x00, 0x00, 0x00,

        // ── 11. Jump back to original RIP ──
        // jmp [r11 + 0x08]
        0x49, 0xFF, 0x63, 0x08,
    };

    constexpr size_t CODE_SIZE = sizeof(code);
    static_assert(CODE_SIZE < DATA_OFFSET,
                  "Shellcode code exceeds DATA_OFFSET — increase DATA_OFFSET or shrink code");

    // Build the full page: code at offset 0, data at DATA_OFFSET, zero otherwise.
    std::vector<uint8_t> page(0x1000, 0);

    // Copy code
    std::memcpy(page.data(), code, CODE_SIZE);

    // Copy data slots at offset DATA_OFFSET
    uint8_t* data = page.data() + DATA_OFFSET;
    auto patchData = [&](size_t off, uintptr_t val) {
        std::memcpy(data + off, &val, sizeof(val));
    };

    patchData(0x00, mappedBase);   // hModule
    patchData(0x08, originalRip);  // original RIP
    patchData(0x10, entryAddr);    // DllMain (mappedBase + entryRva)
    patchData(0x18, originalRsp);  // original RSP
    // 0x20: savedRsp  — written by shellcode at runtime, leave 0
    patchData(0x28, stackBase);    // stack top
    // 0x30: done      — written by shellcode at runtime, leave 0

    fprintf(stderr, "[UMM-DBG] Shellcode: codeSize=%zu dataBase=0x%llX stackBase=0x%llX leaDisp=%d\n",
            CODE_SIZE, dataBase, stackBase, leaDisp);

    return page;
}

bool UserModeMapper::ExecuteEntryPoint(HANDLE hProcess, DWORD pid) {
    if (!m_entryPointRva)
        return true; // resource-only DLL, no DllMain to call

    uintptr_t entryAddr = m_mappedBase + m_entryPointRva;

    // 1. Select a target thread to hijack
    DWORD targetTid = SelectTargetThread(pid, hProcess);
    if (!targetTid) {
        fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: no suitable thread\n");
        return false;
    }

    // 2. Open the thread and suspend it
    HANDLE hThread = OpenThread(
        THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
        FALSE, targetTid);
    if (!hThread) {
        fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: OpenThread(TID=%lu) failed (err=%lu)\n",
                targetTid, GetLastError());
        return false;
    }

    DWORD prevCount = SuspendThread(hThread);
    if (prevCount == (DWORD)-1) {
        fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: SuspendThread failed (err=%lu)\n",
                GetLastError());
        CloseHandle(hThread);
        return false;
    }
    fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: TID=%lu suspended (prevCount=%lu)\n",
            targetTid, prevCount);

    // 3. Get current thread context (RIP and RSP)
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (!GetThreadContext(hThread, &ctx)) {
        fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: GetThreadContext failed (err=%lu)\n",
                GetLastError());
        ResumeThread(hThread);
        CloseHandle(hThread);
        return false;
    }

    uintptr_t originalRip = ctx.Rip;
    uintptr_t originalRsp = ctx.Rsp;
    fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: original RIP=0x%llX RSP=0x%llX\n",
            originalRip, originalRsp);

    // 4. Calculate shellcode location (aligned 16-byte after PE image)
    uintptr_t codePage = m_mappedBase + m_imageSize;
    codePage = (codePage + 15) & ~15ULL;

    // 5. Build and write shellcode
    std::vector<uint8_t> shellcode = BuildHijackShellcode(
        m_mappedBase, entryAddr, originalRip, originalRsp);

    SIZE_T written = 0;
    if (!WriteProcessMemory(hProcess,
                            reinterpret_cast<LPVOID>(codePage),
                            shellcode.data(), shellcode.size(), &written)) {
        fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: WriteProcessMemory(shellcode) failed (err=%lu)\n",
                GetLastError());
        ResumeThread(hThread);
        CloseHandle(hThread);
        return false;
    }
    fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: wrote %zu bytes shellcode at 0x%llX\n",
            written, codePage);

    // 6. Redirect thread: set RIP to shellcode entry point
    ctx.Rip = codePage;
    // RSP is left as-is — the shellcode saves and restores it.

    if (!SetThreadContext(hThread, &ctx)) {
        fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: SetThreadContext failed (err=%lu)\n",
                GetLastError());
        ResumeThread(hThread);
        CloseHandle(hThread);
        return false;
    }

    // 7. Resume the thread — shellcode calls DllMain, then jumps back
    DWORD resumeResult = ResumeThread(hThread);
    fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: ResumeThread returned %lu (0=resumed from 1)\n",
            resumeResult);
    CloseHandle(hThread);

    // 8. Poll the 'done' marker to confirm DllMain returned
    //    InitPayload is minimal (just a flag set), so this should be fast.
    uintptr_t doneAddr = codePage + 0x300 + 0x30; // data offset + done slot
    uint64_t doneVal = 0;
    for (int i = 0; i < 30; ++i) { // up to 3 seconds
        Sleep(100);
        SIZE_T bytesRead = 0;
        if (ReadProcessMemory(hProcess,
                              reinterpret_cast<LPCVOID>(doneAddr),
                              &doneVal, sizeof(doneVal), &bytesRead)) {
            if (doneVal == 1) {
                fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: done marker set after %dms\n",
                        (i + 1) * 100);
                return true;
            }
        }
    }

    fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: WARNING — done marker not set after 3s "
            "(DllMain may still be running or thread may have crashed)\n");
    return true; // Thread was resumed — consider it a success even if marker didn't flip
}
