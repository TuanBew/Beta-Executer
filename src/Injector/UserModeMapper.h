// src/Injector/UserModeMapper.h
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <map>

// Reuse ImportEntry from ManualMapInjector.h (shared PE import representation)
#include "ManualMapInjector.h"

/**
 * UserModeMapper — Pure user-mode PE manual mapper with thread hijacking.
 *
 * Maps a DLL into a target process without CreateRemoteThread or kernel drivers:
 *   1. Read DLL from disk, parse PE headers / sections / imports / relocations
 *   2. VirtualAllocEx → allocate RWX memory in target (image + shellcode page)
 *   3. WriteProcessMemory → copy headers, sections, and shellcode
 *   4. ReadProcessMemory → walk target module list, resolve exports, apply relocs
 *   5. Hijack a target thread → redirect RIP to shellcode that calls DllMain
 *
 * Thread-safety: singleton. Not thread-safe for concurrent Inject() calls.
 *
 * FOR EDUCATIONAL DEMONSTRATION ONLY — thread hijacking bypasses the OS
 * thread-creation path and may be detected by advanced anti-tamper systems.
 */
class UserModeMapper {
public:
    static UserModeMapper& GetInstance();

    /**
     * Full user-mode manual map pipeline.
     * @param pid       Target process ID.
     * @param dllPath   Path to the DLL file on disk (absolute).
     * @return true if injection succeeded and DllMain was called.
     */
    bool Inject(DWORD pid, const std::string& dllPath);

    /**
     * @return The base address the DLL was mapped at in the target (valid after Inject).
     */
    uintptr_t GetMappedBase() const { return m_mappedBase; }

private:
    UserModeMapper() = default;
    ~UserModeMapper() = default;
    UserModeMapper(const UserModeMapper&) = delete;
    UserModeMapper& operator=(const UserModeMapper&) = delete;

    // ---- PE Parsing (host-side, from raw file bytes) ----
    bool ParsePE(const std::vector<uint8_t>& dllBytes);

    // ---- Memory Operations (against target process) ----
    bool AllocateTargetMemory(HANDLE hProcess);
    bool CopyHeadersAndSections(HANDLE hProcess);
    bool ApplyRelocations(HANDLE hProcess);
    bool ResolveImports(HANDLE hProcess, DWORD pid);

    // Module/export resolution (user-mode: Toolhelp + RPM)
    uintptr_t FindModuleInTarget(DWORD pid, const std::string& moduleName);
    uintptr_t FindExportInTarget(HANDLE hProcess, uintptr_t moduleBase,
                                  const std::string& funcName);
    uintptr_t ResolveForwardedExport(HANDLE hProcess, DWORD pid,
                                      const std::string& forwarder);

    // ---- Thread Hijacking ----
    bool ExecuteEntryPoint(HANDLE hProcess, DWORD pid);

    // Select a suitable target thread (skip main thread, prefer wait-state).
    // Returns thread ID, or 0 if no suitable thread found.
    DWORD SelectTargetThread(DWORD pid, HANDLE hProcess);

    // Check whether a RIP points inside an ntdll wait function (safe to hijack).
    bool IsRipInSafeWait(uintptr_t rip, HANDLE hProcess);

    // Build the thread-hijack shellcode. Returns the full shellcode + data + stack
    // page (0x1000 bytes). Patches in hModule, DllMain addr, original RIP, original
    // RSP, and stack top.
    std::vector<uint8_t> BuildHijackShellcode(uintptr_t mappedBase,
                                                uintptr_t entryAddr,
                                                uintptr_t originalRip,
                                                uintptr_t originalRsp);

    // ---- Parsed PE Data ----
    std::vector<uint8_t> m_rawDll;

    IMAGE_DOS_HEADER           m_dosHeader{};
    IMAGE_NT_HEADERS64         m_ntHeaders{};
    std::vector<IMAGE_SECTION_HEADER> m_sections;

    uintptr_t m_preferredBase  = 0;
    uintptr_t m_mappedBase     = 0;
    size_t    m_imageSize      = 0;
    uintptr_t m_entryPointRva  = 0;

    // Parsed imports: dllName → [ImportEntry]
    std::map<std::string, std::vector<ImportEntry>> m_imports;

    // Cache of ntdll wait function addresses (target-space, resolved once).
    // Populated by IsRipInSafeWait on first call.
    std::vector<uintptr_t> m_safeWaitAddrs;
    bool m_safeWaitResolved = false;
};
