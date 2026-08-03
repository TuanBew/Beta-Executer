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

    // Alternative: APC-based execution via QueueUserAPC.
    // When thread hijacking is blocked by anti-cheat (RIP range checks),
    // APC injection routes execution through the kernel's APC dispatcher
    // in ntdll, which may bypass RIP-based detection.
    bool TryApcExecute(HANDLE hProcess, DWORD pid);

    // Code-cave execution: write compact shellcode into a loaded module's
    // section padding via kernel R/W (CapcomDriver), then deliver via APC.
    // Since RIP stays within the module's address range, Hyperion's
    // non-module-RIP detection does not trigger.
    bool TryKernelCodeCaveExecute(HANDLE hProcess, DWORD pid);

    // Find unused executable padding in a loaded module's .text section.
    // Uses CapcomDriver to read remote PE headers (bypasses Hyperion hooks).
    // Returns cave address + size via out params. Requires CapcomDriver loaded.
    bool FindCodeCave(DWORD pid, uintptr_t* outCaveAddr, size_t* outCaveSize);

    // Build compact APC shellcode for code-cave injection.
    // Unlike BuildApcShellcode (0x1000 byte page), this fits in ~80 bytes.
    // The data page (hModule, DllMain, done, heartbeat) is allocated separately;
    // the shellcode receives its address via rcx (APC dwParam).
    std::vector<uint8_t> BuildCaveApcShellcode();

    // Select a suitable target thread (skip main thread, prefer active user-mode).
    // Returns a SUSPENDED thread handle, or NULL. Caller owns the handle and
    // must ResumeThread + CloseHandle when done.
    HANDLE SelectTargetThread(DWORD pid, HANDLE hProcess);

    // Check whether a RIP points inside an ntdll wait function (safe to hijack).
    bool IsRipInSafeWait(uintptr_t rip, HANDLE hProcess);

    // Check whether a RIP falls within any loaded module in the target process.
    // Returns false for kernel-wait threads returning stale context.
    bool IsRipInKnownModule(uintptr_t rip, DWORD pid);

    // Check whether a RIP falls within ntdll.dll, kernel32.dll, or kernelbase.dll.
    // Threads in these DLLs are likely in or about to enter a syscall — their
    // SetThreadContext changes will be overwritten when the kernel restores context.
    bool IsRipInSystemDll(uintptr_t rip, DWORD pid);

    // Build the thread-hijack shellcode. Returns the full shellcode + data + stack
    // page (0x1000 bytes). Patches in hModule, DllMain addr, original RIP, original
    // RSP, and stack top.
    std::vector<uint8_t> BuildHijackShellcode(uintptr_t mappedBase,
                                                uintptr_t entryAddr,
                                                uintptr_t originalRip,
                                                uintptr_t originalRsp);

    // Build the APC shellcode (QueueUserAPC-compatible). Returns 0x1000 byte page.
    // Simpler than hijack: takes dwParam as data pointer, calls DllMain, returns.
    std::vector<uint8_t> BuildApcShellcode(uintptr_t mappedBase,
                                             uintptr_t entryAddr);

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

    // Track last hijacked TID to avoid re-hijacking stuck threads.
    DWORD m_lastHijackedTid = 0;
};
