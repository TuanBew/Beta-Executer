#pragma once

#include <string>
#include <cstdint>
#include <windows.h>
#include <TlHelp32.h>

/**
 * Engine singleton — manages process attachment and detachment.
 *
 * Provides the foundation for all memory operations by opening a handle
 * to the target process with full access rights. All subsequent reads
 * and writes go through this handle.
 *
 * FOR EDUCATIONAL DEMONSTRATION ONLY — intended for use with test.exe
 * in a controlled offline environment.
 */
class Engine {
public:
    static Engine& GetInstance();

    /**
     * Attach to a running process by its executable name (e.g., "test.exe").
     * Uses CreateToolhelp32Snapshot to enumerate processes and OpenProcess
     * with PROCESS_ALL_ACCESS to obtain a full-access handle.
     *
     * @param processName  The executable filename (case-insensitive).
     * @return true if the process was found and a handle was obtained.
     */
    bool AttachToProcess(const std::string& processName);

    /**
     * Attach to a process by its PID directly.
     *
     * @param pid  The process ID.
     * @return true if a handle was obtained.
     */
    bool AttachToProcess(DWORD pid);

    /**
     * Close the process handle and reset internal state.
     */
    void Detach();

    /**
     * @return true if currently attached to a process.
     */
    bool IsAttached() const;

    /**
     * @return The process handle (HANDLE) for the target, or nullptr.
     */
    HANDLE GetProcessHandle() const;

    /**
     * @return The PID of the attached process, or 0.
     */
    DWORD GetPid() const;

    /**
     * @return The base address of the target's main module, or 0.
     */
    uintptr_t GetModuleBase() const;

private:
    Engine() = default;
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /**
     * Enumerate all modules of the target process and find the base address
     * of the main executable module. Called after successful attachment.
     */
    void ResolveModuleBase();

public:
    /**
     * Skip auto-elevation on attach (set via --no-elevate CLI flag).
     * Must be set before calling AttachToProcess().
     */
    static bool s_skipAutoElevate;

    HANDLE m_hProcess = nullptr;
    DWORD  m_Pid = 0;
    uintptr_t m_ModuleBase = 0;
};
