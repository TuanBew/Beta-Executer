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
#include "CapcomDriver.h"

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
extern "C" NTSTATUS NTAPI NtAlertThread(HANDLE ThreadHandle);

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

    // 3a. Enable SeDebugPrivilege — needed when running as admin for
    //     VirtualAllocEx/WriteProcessMemory into a user-level target process.
    //     Without this, admin processes get ACCESS_DENIED (5) on cross-session calls.
    {
        HANDLE hToken;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
            LUID luidDebug;
            if (LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &luidDebug)) {
                TOKEN_PRIVILEGES tp{};
                tp.PrivilegeCount = 1;
                tp.Privileges[0].Luid = luidDebug;
                tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
                fprintf(stderr, "[UMM-DBG] SeDebugPrivilege enabled (err=%lu)\n", GetLastError());
            }
            CloseHandle(hToken);
        }
    }

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

    // 8. Execute DllMain — try thread hijack first, then APC, then code-cave
    bool entryOk = ExecuteEntryPoint(hProcess, pid);
    if (!entryOk) {
        fprintf(stderr, "[UMM-DBG] Thread hijack failed, trying APC injection...\n");
        entryOk = TryApcExecute(hProcess, pid);
    }
    if (!entryOk) {
        fprintf(stderr, "[UMM-DBG] APC injection failed, trying kernel code-cave injection...\n");
        entryOk = TryKernelCodeCaveExecute(hProcess, pid);
    }
    if (!entryOk) {
        fprintf(stderr, "[UMM-DBG] ERROR: all execution methods failed\n");
        VirtualFreeEx(hProcess, reinterpret_cast<LPVOID>(m_mappedBase), 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }
    fprintf(stderr, "[UMM-DBG] DllMain executed successfully\n");

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

    // Try VirtualAllocEx first (works in same-user, non-elevated context).
    m_mappedBase = reinterpret_cast<uintptr_t>(
        VirtualAllocEx(hProcess,
                       reinterpret_cast<LPVOID>(m_preferredBase),
                       allocSize,
                       MEM_COMMIT | MEM_RESERVE,
                       PAGE_EXECUTE_READWRITE));

    DWORD err1 = GetLastError();

    if (!m_mappedBase) {
        // Retry with no base preference.
        m_mappedBase = reinterpret_cast<uintptr_t>(
            VirtualAllocEx(hProcess, nullptr, allocSize,
                           MEM_COMMIT | MEM_RESERVE,
                           PAGE_EXECUTE_READWRITE));
        DWORD err2 = GetLastError();

        if (!m_mappedBase) {
            // When running as admin, VirtualAllocEx may fail with ACCESS_DENIED
            // on PPL or cross-session targets. Fall back to NtAllocateVirtualMemory
            // which is a direct syscall (bypasses some win32u hooks).
            using NtAllocFn = NTSTATUS(NTAPI*)(HANDLE, PVOID*, ULONG_PTR, PSIZE_T,
                                                ULONG, ULONG);
            static auto pNtAlloc = reinterpret_cast<NtAllocFn>(
                GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                               "NtAllocateVirtualMemory"));
            if (pNtAlloc) {
                SIZE_T regionSize = allocSize;
                PVOID baseAddr = nullptr;
                NTSTATUS st = pNtAlloc(hProcess, &baseAddr, 0, &regionSize,
                                       MEM_COMMIT | MEM_RESERVE,
                                       PAGE_EXECUTE_READWRITE);
                m_mappedBase = reinterpret_cast<uintptr_t>(baseAddr);
                fprintf(stderr, "[UMM-DBG] NtAllocateVirtualMemory fallback: "
                        "status=0x%08lX base=0x%llX size=%zu\n",
                        st, m_mappedBase, regionSize);
            }

            if (!m_mappedBase) {
                fprintf(stderr, "[UMM-DBG] AllocateTargetMemory: all attempts failed "
                        "(VirtualAllocEx preferred=0x%llX err=%lu, nullptr err=%lu, "
                        "NtAlloc also failed)\n",
                        m_preferredBase, err1, err2);
            }
        }
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

HANDLE UserModeMapper::SelectTargetThread(DWORD pid, HANDLE hProcess) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnap == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[UMM-DBG] SelectTargetThread: snapshot failed (err=%lu)\n",
                GetLastError());
        return NULL;
    }

    THREADENTRY32 te{sizeof(te)};
    HANDLE chosenHandle = NULL;
    HANDLE fallbackHandle = NULL;
    bool firstFound = false;

    if (Thread32First(hSnap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid)
                continue;

            // Skip the first thread (main/GUI thread)
            if (!firstFound) {
                firstFound = true;
                continue;
            }

            // THREAD_ALERT (0x0004) is needed for NtAlertThread to succeed.
            // Without it, NtAlertThread returns STATUS_ACCESS_DENIED (0xC0000022).
            HANDLE hThread = OpenThread(
                THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION |
                THREAD_SET_INFORMATION | 0x0004, // THREAD_ALERT
                FALSE, te.th32ThreadID);
            if (!hThread)
                continue;

            // Check for pending I/O
            ULONG isIoPending = 0;
            NTSTATUS st = NtQueryInformationThread(
                hThread, ThreadIsIoPending, &isIoPending,
                sizeof(isIoPending), nullptr);
            if (st >= 0 && isIoPending) {
                CloseHandle(hThread);
                continue;
            }

            // Suspend the thread to freeze it for inspection
            DWORD prevCount = SuspendThread(hThread);
            if (prevCount == (DWORD)-1 || prevCount > 0) {
                if (prevCount != (DWORD)-1 && prevCount > 0)
                    ResumeThread(hThread);
                CloseHandle(hThread);
                continue;
            }

            // Get context to inspect RIP while thread is frozen
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            bool gotContext = GetThreadContext(hThread, &ctx) != 0;

            if (!gotContext) {
                ResumeThread(hThread);
                CloseHandle(hThread);
                continue;
            }

            // Skip the thread we hijacked last time
            if (te.th32ThreadID == m_lastHijackedTid) {
                ResumeThread(hThread);
                CloseHandle(hThread);
                continue;
            }

            // Skip threads whose RIP falls within our own mapped region
            uintptr_t mappedEnd = m_mappedBase + m_imageSize + 0x2000;
            if (ctx.Rip >= m_mappedBase && ctx.Rip < mappedEnd) {
                ResumeThread(hThread);
                CloseHandle(hThread);
                fprintf(stderr, "[UMM-DBG] SelectTargetThread: skipping TID=%lu (RIP=0x%llX in our mapped range)\n",
                        te.th32ThreadID, ctx.Rip);
                continue;
            }

            // Skip threads whose RIP isn't in any known module (stale kernel-wait context)
            if (!IsRipInKnownModule(ctx.Rip, pid)) {
                ResumeThread(hThread);
                CloseHandle(hThread);
                fprintf(stderr, "[UMM-DBG] SelectTargetThread: skipping TID=%lu (RIP=0x%llX not in any known module)\n",
                        te.th32ThreadID, ctx.Rip);
                continue;
            }

            bool inSafeWait = IsRipInSafeWait(ctx.Rip, hProcess);
            bool inSystemDll = IsRipInSystemDll(ctx.Rip, pid);

            // Tier 1 (best): Thread executing application/user-mode DLL code.
            // These are genuinely in user mode — SetThreadContext takes effect
            // immediately on ResumeThread. Threads in ntdll/kernel32/kernelbase
            // are almost always in or about to enter a syscall, where the kernel
            // restores its own saved context, ignoring our RIP change.
            if (!inSystemDll && !chosenHandle) {
                chosenHandle = hThread; // KEEP SUSPENDED
                fprintf(stderr, "[UMM-DBG] SelectTargetThread: preferred TID=%lu (app-code, RIP=0x%llX)\n",
                        te.th32ThreadID, ctx.Rip);
                break; // take the first app-code thread
            }

            // Tier 2 (fallback): Thread in a known ntdll wait function.
            // We know it's in a kernel wait, but at least we know WHAT wait.
            // May execute shellcode if the wait is alertable.
            if (!chosenHandle && inSafeWait && !fallbackHandle) {
                fallbackHandle = hThread; // KEEP SUSPENDED
                fprintf(stderr, "[UMM-DBG] SelectTargetThread: fallback TID=%lu (wait-state, RIP=0x%llX)\n",
                        te.th32ThreadID, ctx.Rip);
                // Don't break — keep looking for an app-code thread
            } else if (!chosenHandle && inSystemDll && !inSafeWait && !fallbackHandle) {
                // Tier 3 (last resort): Thread in system DLL at unknown offset.
                // Likely in a syscall — very unlikely to be hijackable.
                fallbackHandle = hThread; // KEEP SUSPENDED
                fprintf(stderr, "[UMM-DBG] SelectTargetThread: last-resort TID=%lu (sys-dll-nonwait, RIP=0x%llX)\n",
                        te.th32ThreadID, ctx.Rip);
            } else {
                ResumeThread(hThread);
                CloseHandle(hThread);
            }
        } while (Thread32Next(hSnap, &te));
    }

    CloseHandle(hSnap);

    // Return the best option found (prefer active, fall back to wait-state)
    if (chosenHandle) {
        if (fallbackHandle) {
            ResumeThread(fallbackHandle);
            CloseHandle(fallbackHandle);
        }
        return chosenHandle;
    }
    if (fallbackHandle)
        return fallbackHandle;

    fprintf(stderr, "[UMM-DBG] SelectTargetThread: NO suitable thread found\n");
    return NULL;
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

bool UserModeMapper::IsRipInSystemDll(uintptr_t rip, DWORD pid) {
    // Check whether RIP falls within ntdll.dll, kernel32.dll, or kernelbase.dll.
    // Threads executing in these DLLs are almost always in or about to enter
    // a syscall — SetThreadContext won't take effect because the kernel saves
    // its own copy of the user context before entering kernel mode.
    //
    // We cache the module ranges on first call (they're static per boot session).
    static uintptr_t s_ntdllBase = 0, s_ntdllEnd = 0;
    static uintptr_t s_kernel32Base = 0, s_kernel32End = 0;
    static uintptr_t s_kernelbaseBase = 0, s_kernelbaseEnd = 0;
    static bool s_resolved = false;

    if (!s_resolved) {
        s_resolved = true;

        // Use CreateToolhelp32Snapshot on the TARGET to get accurate ranges.
        // (Within a session, system DLLs load at the same base in all processes,
        // but querying the target is more robust.)
        HANDLE hModSnap = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (hModSnap != INVALID_HANDLE_VALUE) {
            MODULEENTRY32 me{sizeof(me)};
            if (Module32First(hModSnap, &me)) {
                do {
                    uintptr_t base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                    uintptr_t end  = base + me.modBaseSize;
                    if (_stricmp(me.szModule, "ntdll.dll") == 0) {
                        s_ntdllBase = base; s_ntdllEnd = end;
                    } else if (_stricmp(me.szModule, "kernel32.dll") == 0) {
                        s_kernel32Base = base; s_kernel32End = end;
                    } else if (_stricmp(me.szModule, "kernelbase.dll") == 0) {
                        s_kernelbaseBase = base; s_kernelbaseEnd = end;
                    }
                } while (Module32Next(hModSnap, &me));
            }
            CloseHandle(hModSnap);
        }

        // Fallback: use our own process's module handles (same session = same base)
        if (!s_ntdllBase) {
            HMODULE h = GetModuleHandleA("ntdll.dll");
            if (h) {
                s_ntdllBase = reinterpret_cast<uintptr_t>(h);
                // Size: parse PE headers for exact size... approximate is fine
                // for the range check; ntdll is ~2MB.
                s_ntdllEnd = s_ntdllBase + 0x200000;
            }
        }
        if (!s_kernel32Base) {
            HMODULE h = GetModuleHandleA("kernel32.dll");
            if (h) {
                s_kernel32Base = reinterpret_cast<uintptr_t>(h);
                s_kernel32End = s_kernel32Base + 0x100000;
            }
        }
        if (!s_kernelbaseBase) {
            HMODULE h = GetModuleHandleA("kernelbase.dll");
            if (h) {
                s_kernelbaseBase = reinterpret_cast<uintptr_t>(h);
                s_kernelbaseEnd = s_kernelbaseBase + 0x400000;
            }
        }
    }

    if ((s_ntdllBase && rip >= s_ntdllBase && rip < s_ntdllEnd) ||
        (s_kernel32Base && rip >= s_kernel32Base && rip < s_kernel32End) ||
        (s_kernelbaseBase && rip >= s_kernelbaseBase && rip < s_kernelbaseEnd)) {
        return true;
    }
    return false;
}

bool UserModeMapper::IsRipInKnownModule(uintptr_t rip, DWORD pid) {
    // Enumerate the target's loaded modules. If RIP falls within any
    // module's address range, the thread is executing real code.
    // If not, the thread is likely stuck in a kernel wait and
    // GetThreadContext is returning a stale context (e.g., our own
    // previously-set shellcode RIP).
    HANDLE hModSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hModSnap == INVALID_HANDLE_VALUE)
        return false; // can't verify → assume unsafe, skip

    MODULEENTRY32 me{sizeof(me)};
    if (Module32First(hModSnap, &me)) {
        do {
            uintptr_t modBase = reinterpret_cast<uintptr_t>(me.modBaseAddr);
            uintptr_t modEnd  = modBase + me.modBaseSize;
            if (rip >= modBase && rip < modEnd) {
                CloseHandle(hModSnap);
                return true;
            }
        } while (Module32Next(hModSnap, &me));
    }

    CloseHandle(hModSnap);
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
    //   +0x38: heartbeat     (shellcode writes 1 here BEFORE calling DllMain)

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
        //    REX.WR (4C) needed: R=1 so ModRM.reg=011 → r11 (not rbx)
        0x4C, 0x8D, 0x1D,
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

        // ── 5.5. Heartbeat: write 1 BEFORE calling DllMain ──
        // If this is set but done marker is not → DllMain crashed.
        // mov qword [r11 + 0x38], 1
        0x49, 0xC7, 0x43, 0x38, 0x01, 0x00, 0x00, 0x00,

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
    // 0x38: heartbeat  — written by shellcode at runtime, leave 0

    fprintf(stderr, "[UMM-DBG] Shellcode: codeSize=%zu dataBase=0x%llX stackBase=0x%llX leaDisp=%d\n",
            CODE_SIZE, dataBase, stackBase, leaDisp);

    return page;
}

bool UserModeMapper::ExecuteEntryPoint(HANDLE hProcess, DWORD pid) {
    if (!m_entryPointRva)
        return true; // resource-only DLL, no DllMain to call

    uintptr_t entryAddr = m_mappedBase + m_entryPointRva;

    // 1. Select a target thread to hijack.
    //    Returns the thread ALREADY SUSPENDED — no race window between
    //    inspection and hijack where the thread could enter a kernel wait.
    HANDLE hThread = SelectTargetThread(pid, hProcess);
    if (!hThread) {
        fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: no suitable thread\n");
        return false;
    }

    DWORD targetTid = GetThreadId(hThread);
    m_lastHijackedTid = targetTid;
    fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: using pre-suspended TID=%lu\n", targetTid);

    // 2. Get current thread context (RIP and RSP) — thread is already frozen
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

    // Resolve which module the original RIP lives in (diagnostic)
    const char* ripModule = "unknown";
    HANDLE hModSnap2 = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hModSnap2 != INVALID_HANDLE_VALUE) {
        MODULEENTRY32 me2{sizeof(me2)};
        if (Module32First(hModSnap2, &me2)) {
            do {
                uintptr_t base = reinterpret_cast<uintptr_t>(me2.modBaseAddr);
                uintptr_t end  = base + me2.modBaseSize;
                if (originalRip >= base && originalRip < end) {
                    ripModule = me2.szModule;
                    break;
                }
            } while (Module32Next(hModSnap2, &me2));
        }
        CloseHandle(hModSnap2);
    }
    fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: original RIP=0x%llX (%s) RSP=0x%llX\n",
            originalRip, ripModule, originalRsp);

    // 3. Calculate shellcode location (aligned 16-byte after PE image)
    uintptr_t codePage = m_mappedBase + m_imageSize;
    codePage = (codePage + 15) & ~15ULL;

    // 4. Build and write shellcode
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

    // Verify: read back first 32 bytes from target to confirm WPM worked
    {
        uint8_t verifyBuf[32] = {};
        SIZE_T vbytes = 0;
        if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(codePage),
                              verifyBuf, sizeof(verifyBuf), &vbytes)) {
            fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: verify read back %zu bytes: ",
                    vbytes);
            for (SIZE_T vi = 0; vi < vbytes && vi < 32; vi++)
                fprintf(stderr, "%02X ", verifyBuf[vi]);
            fprintf(stderr, "\n");
            // Check first byte is 0x4C (REX.WR for lea r11)
            if (verifyBuf[0] != 0x4C) {
                fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: *** MISMATCH: first byte is 0x%02X, expected 0x4C ***\n",
                        verifyBuf[0]);
            }
        } else {
            fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: verify ReadProcessMemory FAILED (err=%lu)\n",
                    GetLastError());
        }

        // Query the actual page protection the target OS sees
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQueryEx(hProcess, reinterpret_cast<LPCVOID>(codePage),
                          &mbi, sizeof(mbi))) {
            fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: page protect=0x%lX state=0x%lX type=0x%lX\n",
                    mbi.Protect, mbi.State, mbi.Type);
        }

        // Write a minimal "loop forever" test shellcode at offset 0x200
        // to verify basic execution from our allocated page works.
        uint8_t testLoop[] = { 0xEB, 0xFE }; // jmp -2 (infinite loop)
        SIZE_T testWritten = 0;
        WriteProcessMemory(hProcess,
                          reinterpret_cast<LPVOID>(codePage + 0x200),
                          testLoop, sizeof(testLoop), &testWritten);
    }

    // 5. Redirect thread: set RIP to shellcode entry point
    ctx.Rip = codePage;
    // RSP is left as-is — the shellcode saves and restores it.

    if (!SetThreadContext(hThread, &ctx)) {
        fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: SetThreadContext failed (err=%lu)\n",
                GetLastError());
        ResumeThread(hThread);
        CloseHandle(hThread);
        return false;
    }

    // 6. Resume the thread — shellcode calls DllMain, then jumps back.
    //    The thread may still be blocked in a kernel wait. Use NtAlertThread
    //    to attempt breaking alertable waits so the shellcode runs immediately.
    DWORD resumeResult = ResumeThread(hThread);
    fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: ResumeThread returned %lu (0=resumed from 1)\n",
            resumeResult);

    // Attempt to wake the thread from alertable kernel wait.
    // If NtAlertThread succeeds, the thread returns from its wait immediately
    // with STATUS_ALERTED and executes our shellcode. If it fails (thread not
    // in alertable wait), our shellcode executes when the wait completes naturally.
    NTSTATUS alertStatus = NtAlertThread(hThread);
    fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: NtAlertThread returned 0x%08lX (%s)\n",
            alertStatus, alertStatus >= 0 ? "alerted" : "not alerted (non-alertable wait)");
    CloseHandle(hThread);

    // 7. Poll the 'done' marker to confirm DllMain returned.
    //    InitPayload is minimal (just a flag set), so this should be fast.
    //    If NtAlertThread succeeded, the shellcode runs within microseconds.
    //    If not, the thread executes when its wait completes naturally.
    uintptr_t doneAddr = codePage + 0x300 + 0x30; // data offset + done slot
    uint64_t doneVal = 0;
    for (int i = 0; i < 50; ++i) { // up to 5 seconds
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

    // ── Diagnostics ──
    // Always read heartbeat + done marker (survives thread termination).
    uintptr_t heartbeatAddr = codePage + 0x300 + 0x38;
    uint64_t heartbeatVal = 0, finalDoneVal = 0;
    {
        SIZE_T hbBytes = 0, dnBytes = 0;
        ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(heartbeatAddr),
                         &heartbeatVal, sizeof(heartbeatVal), &hbBytes);
        ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(doneAddr),
                         &finalDoneVal, sizeof(finalDoneVal), &dnBytes);
    }

    // Also try to read the thread's RIP
    HANDLE hDiagThread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                     FALSE, targetTid);
    if (hDiagThread) {
        CONTEXT diagCtx{};
        diagCtx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(hDiagThread, &diagCtx)) {
            fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: after 5s, thread RIP=0x%llX "
                    "(shellcode=0x%llX, original=0x%llX) heartbeat=%llu done=%llu\n",
                    diagCtx.Rip, static_cast<unsigned long long>(codePage),
                    static_cast<unsigned long long>(originalRip),
                    heartbeatVal, finalDoneVal);
            if (heartbeatVal == 1 && finalDoneVal == 0) {
                fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: *** HEARTBEAT=1, DONE=0 *** "
                        "→ shellcode executed, DllMain CRASHED\n");
            } else if (heartbeatVal == 0) {
                fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: heartbeat=0 → "
                        "shellcode NEVER executed past heartbeat write\n");
            }
        } else {
            fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: GetThreadContext(diag) failed (err=%lu) "
                    "heartbeat=%llu done=%llu\n", GetLastError(), heartbeatVal, finalDoneVal);
        }
        CloseHandle(hDiagThread);
    } else {
        fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: OpenThread(diag) for TID=%lu failed "
                "(err=%lu) heartbeat=%llu done=%llu — thread TERMINATED\n",
                targetTid, GetLastError(), heartbeatVal, finalDoneVal);
        if (heartbeatVal == 1 && finalDoneVal == 0) {
            fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: *** HEARTBEAT=1, DONE=0 *** "
                    "→ shellcode executed, DllMain CRASHED (thread killed)\n");
        } else if (heartbeatVal == 0) {
            fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: heartbeat=0 → "
                    "shellcode NEVER executed (SetThreadContext silently ignored)\n");
        }
    }

    fprintf(stderr, "[UMM-DBG] ExecuteEntryPoint: FAIL — done marker not set after 5s "
            "(thread still in kernel wait, DllMain crashed, or shellcode never executed)\n");
    return false;
}

// ---- APC-based Execution (bypasses anti-cheat RIP-range checks) -------------

std::vector<uint8_t> UserModeMapper::BuildApcShellcode(
    uintptr_t mappedBase, uintptr_t entryAddr)
{
    // APC shellcode: simpler than hijack shellcode — no stack switch,
    // no jmp-back. The kernel's APC dispatcher saves/restores context.
    //
    // Function signature: void CALLBACK ApcRoutine(ULONG_PTR dwParam)
    //   rcx = dwParam = pointer to data area
    //
    // Data slot layout (offset from data base):
    //   +0x00: hModule       (mapped DLL base)
    //   +0x08: dllMain       (mappedBase + entryPointRva)
    //   +0x10: done marker   (written by shellcode before ret)

    constexpr uint32_t DATA_OFFSET = 0x100;  // data at offset 0x100 within page
    uintptr_t codePage = mappedBase + m_imageSize;
    codePage = (codePage + 15) & ~15ULL;
    uintptr_t dataBase = codePage + DATA_OFFSET;

    // lea rax, [rip + data] — need REX.W for 64-bit
    int32_t leaDisp = static_cast<int32_t>(dataBase - (codePage + 7));

    uint8_t code[] = {
        // ── 1. Save volatile regs and dwParam (rcx) ──
        // push rcx                     ; dwParam = data pointer
        0x51,
        // push rdx
        0x52,
        // push r8
        0x41, 0x50,
        // push r9
        0x41, 0x51,
        // push r10
        0x41, 0x52,
        // push r11
        0x41, 0x53,
        // push rax
        0x50,
        // pushfq
        0x9C,

        // ── 2. Shadow space (32 bytes, maintains 16-byte alignment) ──
        // After 8 pushes (64 bytes) + sub rsp,0x20 (32 bytes) = 96 = 6*16 ✓
        // sub rsp, 0x20
        0x48, 0x83, 0xEC, 0x20,

        // ── 3. Reload data pointer (rcx may have been clobbered) ──
        // We saved it on the stack; read it back relative to RSP.
        // After 8 pushes (64) + sub rsp,0x20 (32) = 96 bytes below saved rcx.
        // Saved rcx is at RSP + 0x20 + 7*8 = RSP + 0x20 + 56 = RSP + 0x58
        // mov rax, [rsp + 0x58]       ; recover dwParam (saved rcx)
        0x48, 0x8B, 0x84, 0x24, 0x58, 0x00, 0x00, 0x00,

        // ── 4. Heartbeat: write 1 to [rax + 0x18] ──
        // mov qword [rax + 0x18], 1
        0x48, 0xC7, 0x40, 0x18, 0x01, 0x00, 0x00, 0x00,

        // ── 5. Call DllMain(hModule, DLL_PROCESS_ATTACH, NULL) ──
        // mov rcx, [rax + 0x00]       ; hModule
        0x48, 0x8B, 0x48, 0x00,
        // mov rdx, 1                   ; fdwReason
        0x48, 0xC7, 0xC2, 0x01, 0x00, 0x00, 0x00,
        // xor r8, r8                   ; lpReserved
        0x4D, 0x31, 0xC0,
        // push rax                     ; save data pointer before call
        0x50,
        // mov rax, [rax + 0x08]       ; DllMain address
        0x48, 0x8B, 0x40, 0x08,
        // call rax
        0xFF, 0xD0,
        // pop rax                      ; restore data pointer
        0x58,

        // ── 6. Set done marker ──
        // mov qword [rax + 0x10], 1
        0x48, 0xC7, 0x40, 0x10, 0x01, 0x00, 0x00, 0x00,

        // ── 7. Restore shadow space ──
        // add rsp, 0x20
        0x48, 0x83, 0xC4, 0x20,

        // ── 8. Restore volatile regs (reverse order) ──
        // popfq
        0x9D,
        // pop rax
        0x58,
        // pop r11
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

        // ── 9. Return to APC dispatcher ──
        // ret
        0xC3,
    };

    // Build the full page
    std::vector<uint8_t> page(0x1000, 0);
    std::memcpy(page.data(), code, sizeof(code));

    // Patch data slots at offset DATA_OFFSET
    uint8_t* data = page.data() + DATA_OFFSET;
    auto patchData = [&](size_t off, uintptr_t val) {
        std::memcpy(data + off, &val, sizeof(val));
    };
    patchData(0x00, mappedBase);   // hModule
    patchData(0x08, entryAddr);    // DllMain
    // 0x10: done      — written by shellcode at runtime
    // 0x18: heartbeat  — written by shellcode at runtime

    return page;
}

bool UserModeMapper::TryApcExecute(HANDLE hProcess, DWORD pid) {
    if (!m_entryPointRva)
        return true;

    uintptr_t entryAddr = m_mappedBase + m_entryPointRva;
    uintptr_t codePage = m_mappedBase + m_imageSize;
    codePage = (codePage + 15) & ~15ULL;
    uintptr_t dataBase = codePage + 0x100;

    fprintf(stderr, "[UMM-APC] TryApcExecute: entry=0x%llX codePage=0x%llX dataBase=0x%llX\n",
            entryAddr, codePage, dataBase);

    // 1. Build and write APC shellcode
    std::vector<uint8_t> apcShellcode = BuildApcShellcode(m_mappedBase, entryAddr);
    SIZE_T written = 0;
    if (!WriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(codePage),
                            apcShellcode.data(), apcShellcode.size(), &written)) {
        fprintf(stderr, "[UMM-APC] WriteProcessMemory failed (err=%lu)\n", GetLastError());
        return false;
    }
    fprintf(stderr, "[UMM-APC] wrote %zu bytes APC shellcode at 0x%llX\n", written, codePage);

    // 2. Enumerate threads — collect all candidates, main thread FIRST.
    //    The main/GUI thread runs a message loop (GetMessage/PeekMessage) which
    //    are alertable waits. When we PostThreadMessage + QueueUserAPC, the
    //    kernel delivers the APC before returning the message.
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnap == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[UMM-APC] CreateToolhelp32Snapshot failed (err=%lu)\n", GetLastError());
        return false;
    }

    THREADENTRY32 te{sizeof(te)};
    std::vector<DWORD> threadIds;
    DWORD mainTid = 0;
    bool hasMain = false;

    if (Thread32First(hSnap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            if (te.th32ThreadID == m_lastHijackedTid) continue;

            if (!hasMain) {
                // Main thread goes first — most likely alertable (GUI thread)
                mainTid = te.th32ThreadID;
                hasMain = true;
            } else {
                threadIds.push_back(te.th32ThreadID);
            }
        } while (Thread32Next(hSnap, &te));
    }
    CloseHandle(hSnap);

    // Build ordered list: main thread first, then all others
    std::vector<DWORD> orderedIds;
    if (mainTid) orderedIds.push_back(mainTid);
    orderedIds.insert(orderedIds.end(), threadIds.begin(), threadIds.end());

    fprintf(stderr, "[UMM-APC] %zu candidate threads (main TID=%lu first)\n",
            orderedIds.size(), mainTid);

    // 3. Try each thread — early exit after N consecutive "never delivered"
    bool success = false;
    int consecutiveNeverDelivered = 0;
    constexpr int kMaxConsecutiveNever = 5;
    for (DWORD tid : orderedIds) {
        // THREAD_ALERT (0x0004) is needed for NtAlertThread to succeed.
        // Without it, NtAlertThread returns STATUS_ACCESS_DENIED (0xC0000022).
        constexpr DWORD kThreadAccess = THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME
                                       | THREAD_QUERY_INFORMATION | 0x0004; // THREAD_ALERT
        HANDLE hThread = OpenThread(kThreadAccess, FALSE, tid);
        if (!hThread) {
            fprintf(stderr, "[UMM-APC] OpenThread TID=%lu failed (err=%lu)\n", tid, GetLastError());
            continue;
        }

        // Step A: Queue the APC. dwParam points to the data area (hModule, DllMain, etc.)
        DWORD queued = QueueUserAPC(
            reinterpret_cast<PAPCFUNC>(codePage),
            hThread,
            static_cast<ULONG_PTR>(dataBase));

        fprintf(stderr, "[UMM-APC] QueueUserAPC TID=%lu: %s (err=%lu)\n",
                tid, queued ? "QUEUED" : "FAILED",
                queued ? 0 : GetLastError());

        if (!queued) {
            CloseHandle(hThread);
            continue;
        }

        // Step B: PostThreadMessage(WM_NULL) — force alertable wait in message loop.
        // If the thread runs GetMessage/PeekMessage (GUI threads), posting a message
        // triggers an alertable wait where the kernel delivers pending APCs BEFORE
        // returning the message. Even if the thread lacks a message queue,
        // PostThreadMessage creates one.
        BOOL msgPosted = PostThreadMessageW(tid, WM_NULL, 0, 0);
        fprintf(stderr, "[UMM-APC] PostThreadMessage(WM_NULL) TID=%lu: %s (err=%lu)\n",
                tid, msgPosted ? "POSTED" : "FAILED",
                msgPosted ? 0 : GetLastError());

        // Step C: NtAlertThread — wake thread from alertable kernel wait.
        // If the thread is in NtWaitForSingleObject/NtWaitForMultipleObjects
        // with Alertable=TRUE, this forces an immediate return with STATUS_ALERTED.
        NTSTATUS alertStatus = NtAlertThread(hThread);
        fprintf(stderr, "[UMM-APC] NtAlertThread TID=%lu: 0x%08lX (%s)\n",
                tid, alertStatus,
                alertStatus >= 0 ? "ALERTED — APC should fire immediately"
                                 : "not alerted (non-alertable wait or no THREAD_ALERT access)");

        CloseHandle(hThread);

        // Step D: Poll the done marker for up to 5 seconds
        uintptr_t doneAddr = dataBase + 0x10;
        uint64_t doneVal = 0;
        for (int i = 0; i < 50; ++i) {
            Sleep(100);
            SIZE_T bytesRead = 0;
            if (ReadProcessMemory(hProcess,
                                 reinterpret_cast<LPCVOID>(doneAddr),
                                 &doneVal, sizeof(doneVal), &bytesRead)) {
                if (doneVal == 1) {
                    fprintf(stderr, "[UMM-APC] DONE via TID=%lu after %dms — SUCCESS\n",
                            tid, (i + 1) * 100);
                    success = true;
                    break;
                }
            }
        }
        if (success) break;

        // Check heartbeat to distinguish "never ran" from "ran but crashed"
        uintptr_t hbAddr = dataBase + 0x18;
        uint64_t hbVal = 0;
        ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(hbAddr),
                         &hbVal, sizeof(hbVal), nullptr);
        if (hbVal == 1) {
            fprintf(stderr, "[UMM-APC] TID=%lu: heartbeat=1 done=0 → APC delivered, DllMain CRASHED\n",
                    tid);
            consecutiveNeverDelivered = 0;
        } else {
            fprintf(stderr, "[UMM-APC] TID=%lu: heartbeat=0 → APC NEVER delivered (thread not alertable)\n",
                    tid);
            consecutiveNeverDelivered++;
            if (consecutiveNeverDelivered >= kMaxConsecutiveNever) {
                fprintf(stderr, "[UMM-APC] %d consecutive threads never delivered — "
                        "Hyperion is filtering APCs, skipping remaining threads\n",
                        kMaxConsecutiveNever);
                break;
            }
        }
    }

    if (!success)
        fprintf(stderr, "[UMM-APC] FAIL: no thread delivered APC. All threads non-alertable.\n");
    return success;
}

// ---- Code-Cave Execution (kernel R/W bypass for Hyperion RIP-range checks) ----

std::vector<uint8_t> UserModeMapper::BuildCaveApcShellcode() {
    // Compact APC-compatible shellcode that fits in ~80 bytes.
    // Designed for code-cave injection: the shellcode lives in module padding,
    // data (hModule, DllMain, done, heartbeat) lives in a separate RWX allocation.
    //
    // void CALLBACK ApcRoutine(ULONG_PTR dwParam)
    //   rcx = dwParam = pointer to data area:
    //     +0x00: hModule
    //     +0x08: DllMain
    //     +0x10: done marker
    //     +0x18: heartbeat marker
    //
    // Stack layout after sub rsp,0x20:
    //   rsp+0x20: pushfq → rsp+0x28: rax → ... → rsp+0x58: rcx (data ptr)
    //
    // 8 pushes (64 bytes) + sub rsp,0x20 (32 bytes) = 96 = 6×16 ✓ aligned

    uint8_t code[] = {
        // ── Save volatile registers (x64 calling convention) ──
        0x51,                         // push rcx    ; data pointer
        0x52,                         // push rdx
        0x41, 0x50,                   // push r8
        0x41, 0x51,                   // push r9
        0x41, 0x52,                   // push r10
        0x41, 0x53,                   // push r11
        0x50,                         // push rax
        0x9C,                         // pushfq

        // ── Shadow space (32 bytes) ──
        0x48, 0x83, 0xEC, 0x20,       // sub rsp, 0x20

        // ── Recover data pointer from stack ──
        // rcx was pushed first; after sub rsp,0x20 it's at rsp+0x20+7*8 = rsp+0x58
        0x48, 0x8B, 0x84, 0x24, 0x58, 0x00, 0x00, 0x00,
        // mov rax, [rsp + 0x58]

        // ── Heartbeat: write 1 before calling DllMain ──
        0x48, 0xC7, 0x40, 0x18, 0x01, 0x00, 0x00, 0x00,
        // mov qword [rax + 0x18], 1

        // ── Call DllMain(hModule, DLL_PROCESS_ATTACH, NULL) ──
        0x48, 0x8B, 0x48, 0x00,       // mov rcx, [rax + 0x00]  ; hModule
        0x48, 0xC7, 0xC2, 0x01, 0x00, 0x00, 0x00,
        // mov rdx, 1                ; DLL_PROCESS_ATTACH
        0x4D, 0x31, 0xC0,             // xor r8, r8              ; NULL
        0x50,                         // push rax               ; save data ptr
        0x48, 0x8B, 0x40, 0x08,       // mov rax, [rax + 0x08]  ; DllMain addr
        0xFF, 0xD0,                   // call rax
        0x58,                         // pop rax                ; restore data ptr

        // ── Done marker ──
        0x48, 0xC7, 0x40, 0x10, 0x01, 0x00, 0x00, 0x00,
        // mov qword [rax + 0x10], 1

        // ── Restore registers ──
        0x48, 0x83, 0xC4, 0x20,       // add rsp, 0x20
        0x9D,                         // popfq
        0x58,                         // pop rax
        0x41, 0x5B,                   // pop r11
        0x41, 0x5A,                   // pop r10
        0x41, 0x59,                   // pop r9
        0x41, 0x58,                   // pop r8
        0x5A,                         // pop rdx
        0x59,                         // pop rcx
        0xC3,                         // ret
    };

    return std::vector<uint8_t>(code, code + sizeof(code));
}

bool UserModeMapper::FindCodeCave(DWORD pid, uintptr_t* outCaveAddr,
                                   size_t* outCaveSize) {
    auto& capcom = CapcomDriver::GetInstance();
    if (!capcom.IsLoaded()) {
        fprintf(stderr, "[UMM-CAVE] CapcomDriver not loaded — cannot find code cave\n");
        return false;
    }

    // Try modules in order: main exe first, then common DLLs with lots of code
    const char* candidateModules[] = {
        "RobloxPlayerBeta.exe",
        "vcruntime140.dll",   // often has padding
        "msvcp140.dll",
        "d3d11.dll",
        "dxgi.dll",
    };

    for (const char* modName : candidateModules) {
        uintptr_t modBase = FindModuleInTarget(pid, modName);
        if (!modBase) {
            fprintf(stderr, "[UMM-CAVE] Module '%s' not found in target\n", modName);
            continue;
        }

        // Read PE headers via kernel R/W (bypasses Hyperion RPM hooks)
        IMAGE_DOS_HEADER dos = capcom.Read<IMAGE_DOS_HEADER>(pid, modBase);
        if (dos.e_magic != IMAGE_DOS_SIGNATURE) {
            fprintf(stderr, "[UMM-CAVE] '%s': invalid DOS signature at 0x%llX\n",
                    modName, modBase);
            continue;
        }

        IMAGE_NT_HEADERS64 nt = capcom.Read<IMAGE_NT_HEADERS64>(
            pid, modBase + dos.e_lfanew);
        if (nt.Signature != IMAGE_NT_SIGNATURE) {
            fprintf(stderr, "[UMM-CAVE] '%s': invalid NT signature\n", modName);
            continue;
        }

        uintptr_t secTable = modBase + dos.e_lfanew + sizeof(IMAGE_NT_HEADERS64);
        WORD numSections = nt.FileHeader.NumberOfSections;
        uint32_t secAlign = nt.OptionalHeader.SectionAlignment;

        fprintf(stderr, "[UMM-CAVE] '%s': base=0x%llX sections=%u secAlign=0x%X\n",
                modName, modBase, numSections, secAlign);

        // Scan each section for a gap between it and the NEXT section.
        // This catches both intra-section padding (VirtualSize < aligned size)
        // and inter-section gaps.
        uintptr_t prevSecEnd = modBase + dos.e_lfanew
                             + sizeof(IMAGE_NT_HEADERS64)
                             + numSections * sizeof(IMAGE_SECTION_HEADER);
        // ^ approximate: the section table itself sits in the header region

        IMAGE_SECTION_HEADER prevSec{};
        bool havePrev = false;
        uintptr_t bestCave = 0;
        size_t bestSize = 0;

        for (WORD i = 0; i < numSections; ++i) {
            IMAGE_SECTION_HEADER sec = capcom.Read<IMAGE_SECTION_HEADER>(
                pid, secTable + i * sizeof(IMAGE_SECTION_HEADER));

            uintptr_t secStart = modBase + sec.VirtualAddress;
            size_t alignedSize = (sec.Misc.VirtualSize + secAlign - 1) & ~(secAlign - 1ULL);
            uintptr_t secAlignedEnd = secStart + alignedSize;

            // Check padding at end of this section
            size_t padding = static_cast<size_t>(secAlignedEnd - (secStart + sec.Misc.VirtualSize));
            bool isExec = (sec.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;

            fprintf(stderr, "[UMM-CAVE]   '%s' VA=0x%X VSize=0x%X alignedEnd=0x%llX padding=%zu exec=%d\n",
                    sec.Name, sec.VirtualAddress, sec.Misc.VirtualSize,
                    secAlignedEnd, padding, isExec);

            // Prefer executable-padded sections (Hyperion may check NX)
            if (isExec && padding >= 128 && padding > bestSize) {
                bestCave = secStart + sec.Misc.VirtualSize;
                bestSize = padding;
                fprintf(stderr, "[UMM-CAVE]   → executable cave candidate: 0x%llX size=%zu\n",
                        bestCave, bestSize);
            }

            // Also check gap between this section and the next
            if (havePrev) {
                uintptr_t prevEnd = modBase + prevSec.VirtualAddress
                                  + ((prevSec.Misc.VirtualSize + secAlign - 1) & ~(secAlign - 1ULL));
                if (secStart > prevEnd) {
                    size_t gap = static_cast<size_t>(secStart - prevEnd);
                    bool prevExec = (prevSec.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
                    fprintf(stderr, "[UMM-CAVE]   inter-section gap after '%s': 0x%llX size=%zu exec=%d\n",
                            prevSec.Name, prevEnd, gap, prevExec);
                    // The gap inherits characteristics from neither section exactly,
                    // but it's within the module's mapped range — Hyperion accepts it.
                    if (gap >= 128 && gap > bestSize) {
                        bestCave = prevEnd;
                        bestSize = gap;
                    }
                }
            }

            prevSec = sec;
            havePrev = true;
        }

        // Also check padding after the last section (rarely useful but worth a shot)
        if (havePrev) {
            uintptr_t lastEnd = modBase + prevSec.VirtualAddress
                              + ((prevSec.Misc.VirtualSize + secAlign - 1) & ~(secAlign - 1ULL));
            uintptr_t moduleEnd = modBase + nt.OptionalHeader.SizeOfImage;
            if (moduleEnd > lastEnd) {
                size_t tailGap = static_cast<size_t>(moduleEnd - lastEnd);
                fprintf(stderr, "[UMM-CAVE]   tail gap after last section: 0x%llX size=%zu\n",
                        lastEnd, tailGap);
                if (tailGap >= 128 && tailGap > bestSize) {
                    bestCave = lastEnd;
                    bestSize = tailGap;
                }
            }
        }

        if (bestCave && bestSize >= 128) {
            *outCaveAddr = bestCave;
            *outCaveSize = bestSize;
            fprintf(stderr, "[UMM-CAVE] selected cave in '%s': 0x%llX size=%zu\n",
                    modName, bestCave, bestSize);
            return true;
        }
    }

    fprintf(stderr, "[UMM-CAVE] no suitable code cave found (need ≥128 bytes)\n");
    return false;
}

bool UserModeMapper::TryKernelCodeCaveExecute(HANDLE hProcess, DWORD pid) {
    if (!m_entryPointRva) {
        fprintf(stderr, "[UMM-CAVE] No entry point — resource-only DLL, success\n");
        return true;
    }

    auto& capcom = CapcomDriver::GetInstance();
    if (!capcom.IsLoaded()) {
        fprintf(stderr, "[UMM-CAVE] CapcomDriver not loaded — code cave requires kernel R/W\n");
        return false;
    }

    uintptr_t entryAddr = m_mappedBase + m_entryPointRva;

    // 1. Find a code cave in a loaded module
    uintptr_t caveAddr = 0;
    size_t caveSize = 0;
    if (!FindCodeCave(pid, &caveAddr, &caveSize)) {
        fprintf(stderr, "[UMM-CAVE] no code cave found\n");
        return false;
    }

    // 2. Build compact APC shellcode
    std::vector<uint8_t> shellcode = BuildCaveApcShellcode();
    fprintf(stderr, "[UMM-CAVE] built %zu-byte cave shellcode\n", shellcode.size());

    if (shellcode.size() > caveSize) {
        fprintf(stderr, "[UMM-CAVE] shellcode too large (%zu > cave %zu)\n",
                shellcode.size(), caveSize);
        return false;
    }

    // 3. Write shellcode to cave via kernel R/W (bypasses VirtualProtectEx hooks)
    if (!capcom.WriteMemory(pid, caveAddr, shellcode.data(), shellcode.size())) {
        fprintf(stderr, "[UMM-CAVE] WriteMemory to cave 0x%llX failed\n", caveAddr);
        return false;
    }
    fprintf(stderr, "[UMM-CAVE] wrote %zu bytes to cave 0x%llX via kernel R/W\n",
            shellcode.size(), caveAddr);

    // Verify: read back via kernel R/W
    {
        std::vector<uint8_t> verify(shellcode.size());
        capcom.ReadMemory(pid, caveAddr, verify.data(), verify.size());
        bool match = (memcmp(verify.data(), shellcode.data(), shellcode.size()) == 0);
        fprintf(stderr, "[UMM-CAVE] verify read-back: %s\n", match ? "MATCH" : "MISMATCH");
        if (!match) {
            fprintf(stderr, "[UMM-CAVE] first bytes: ");
            for (size_t j = 0; j < (std::min)(verify.size(), (size_t)16); j++)
                fprintf(stderr, "%02X ", verify[j]);
            fprintf(stderr, "\n");
        }
    }

    // 4. Set up data page in our existing RWX allocation
    //    (data page is just 4 qwords — doesn't trigger Hyperion because it's
    //     only read by the shellcode, not executed)
    uintptr_t codePage = m_mappedBase + m_imageSize;
    codePage = (codePage + 15) & ~15ULL;
    uintptr_t dataBase = codePage + 0x100;

    uint64_t dataSlots[4] = {
        m_mappedBase,  // +0x00: hModule
        entryAddr,     // +0x08: DllMain
        0,             // +0x10: done marker (shellcode writes 1)
        0,             // +0x18: heartbeat (shellcode writes 1)
    };

    SIZE_T written = 0;
    if (!WriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(dataBase),
                            dataSlots, sizeof(dataSlots), &written)) {
        fprintf(stderr, "[UMM-CAVE] WriteProcessMemory(data) failed (err=%lu)\n", GetLastError());
        return false;
    }
    fprintf(stderr, "[UMM-CAVE] wrote data slots at 0x%llX (hModule=0x%llX, DllMain=0x%llX)\n",
            dataBase, m_mappedBase, entryAddr);

    // 5. Enumerate threads — prefer main thread (GUI thread, most alertable)
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnap == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[UMM-CAVE] CreateToolhelp32Snapshot failed (err=%lu)\n", GetLastError());
        return false;
    }

    THREADENTRY32 te{sizeof(te)};
    std::vector<DWORD> threadIds;
    DWORD mainTid = 0;

    if (Thread32First(hSnap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            if (te.th32ThreadID == m_lastHijackedTid) continue;
            if (!mainTid) {
                mainTid = te.th32ThreadID;  // first thread = main, goes first
            } else {
                threadIds.push_back(te.th32ThreadID);
            }
        } while (Thread32Next(hSnap, &te));
    }
    CloseHandle(hSnap);

    // Main thread first (GUI thread, message loop = alertable)
    std::vector<DWORD> orderedIds;
    if (mainTid) orderedIds.push_back(mainTid);
    orderedIds.insert(orderedIds.end(), threadIds.begin(), threadIds.end());

    fprintf(stderr, "[UMM-CAVE] %zu candidate threads (main TID=%lu first)\n",
            orderedIds.size(), mainTid);

    // 6. Try APC delivery to each thread
    bool success = false;
    int consecutiveNeverDelivered = 0;
    constexpr int kMaxConsecutiveNever = 5;
    constexpr DWORD kThreadAccess = THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME
                                   | THREAD_QUERY_INFORMATION | 0x0004; // THREAD_ALERT

    for (DWORD tid : orderedIds) {
        HANDLE hThread = OpenThread(kThreadAccess, FALSE, tid);
        if (!hThread) {
            fprintf(stderr, "[UMM-CAVE] OpenThread TID=%lu failed (err=%lu)\n",
                    tid, GetLastError());
            continue;
        }

        // Queue APC: function at caveAddr (inside module), dwParam = dataBase
        DWORD queued = QueueUserAPC(
            reinterpret_cast<PAPCFUNC>(caveAddr),
            hThread,
            static_cast<ULONG_PTR>(dataBase));

        fprintf(stderr, "[UMM-CAVE] QueueUserAPC(cave=0x%llX, data=0x%llX) TID=%lu: %s (err=%lu)\n",
                caveAddr, dataBase, tid,
                queued ? "QUEUED" : "FAILED",
                queued ? 0 : GetLastError());

        if (!queued) {
            CloseHandle(hThread);
            continue;
        }

        // Force delivery: post message + alert thread
        BOOL msgPosted = PostThreadMessageW(tid, WM_NULL, 0, 0);
        fprintf(stderr, "[UMM-CAVE] PostThreadMessage(WM_NULL) TID=%lu: %s (err=%lu)\n",
                tid, msgPosted ? "POSTED" : "FAILED",
                msgPosted ? 0 : GetLastError());

        NTSTATUS alertStatus = NtAlertThread(hThread);
        fprintf(stderr, "[UMM-CAVE] NtAlertThread TID=%lu: 0x%08lX (%s)\n",
                tid, alertStatus,
                alertStatus >= 0 ? "ALERTED" : "not alerted");

        CloseHandle(hThread);

        // Poll done marker
        uintptr_t doneAddr = dataBase + 0x10;
        uint64_t doneVal = 0;
        for (int i = 0; i < 50; ++i) {
            Sleep(100);
            SIZE_T bytesRead = 0;
            if (ReadProcessMemory(hProcess,
                                 reinterpret_cast<LPCVOID>(doneAddr),
                                 &doneVal, sizeof(doneVal), &bytesRead)) {
                if (doneVal == 1) {
                    fprintf(stderr, "[UMM-CAVE] DONE via TID=%lu after %dms — SUCCESS\n",
                            tid, (i + 1) * 100);
                    success = true;
                    break;
                }
            }
        }
        if (success) break;

        // Check heartbeat
        uintptr_t hbAddr = dataBase + 0x18;
        uint64_t hbVal = 0;
        ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(hbAddr),
                         &hbVal, sizeof(hbVal), nullptr);
        if (hbVal == 1) {
            fprintf(stderr, "[UMM-CAVE] TID=%lu: heartbeat=1 done=0 → "
                    "APC delivered, DllMain CRASHED\n", tid);
            consecutiveNeverDelivered = 0;
        } else {
            fprintf(stderr, "[UMM-CAVE] TID=%lu: heartbeat=0 → "
                    "APC not delivered (thread not alertable, or Hyperion blocks)\n", tid);
            consecutiveNeverDelivered++;
            if (consecutiveNeverDelivered >= kMaxConsecutiveNever) {
                fprintf(stderr, "[UMM-CAVE] %d consecutive never-delivered — "
                        "Hyperion filtering, skipping remaining threads\n",
                        kMaxConsecutiveNever);
                break;
            }
        }
    }

    if (!success)
        fprintf(stderr, "[UMM-CAVE] FAIL: no thread delivered cave APC\n");
    return success;
}
