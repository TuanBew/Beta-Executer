#pragma once

#include <string>
#include <cstdint>

/**
 * Bootstrap module — demonstrates loading a DLL into the target process's
 * address space via CreateRemoteThread + LoadLibraryA.
 *
 * FOR EDUCATIONAL DEMONSTRATION ONLY — This technique is shown to illustrate
 * how operating systems support loading libraries across process boundaries.
 * The Universal Hub engine normally runs as a standalone process that accesses
 * the target externally via ReadProcessMemory/WriteProcessMemory.
 *
 * Usage requires:
 *   - Administrator privileges
 *   - The target process to have LoadLibraryA in its import table (kernel32.dll)
 *   - A valid DLL at the specified path (must be accessible by the target process)
 */
namespace Bootstrap {

    /**
     * Load a DLL into the target process by creating a remote thread that
     * calls LoadLibraryA with the given path.
     *
     * @param dllPath  Absolute path to the DLL file (must be readable by target).
     * @return true if the remote thread was created successfully.
     */
    bool LoadIntoProcess(const std::string& dllPath);

    /**
     * Manual-map a DLL into the target process via kernel R/W (Capcom.sys).
     * Does NOT call LoadLibraryA — the DLL is mapped manually with PE parsing,
     * import resolution, and relocation fixup through kernel-mode IOCTLs.
     *
     * @param pid       Target process ID.
     * @param dllPath   Absolute path to the DLL file.
     * @return true if mapped and entry point executed successfully.
     */
    bool ManualMapIntoProcess(uint32_t pid, const std::string& dllPath);

    /**
     * User-mode manual-map a DLL into the target process via
     * VirtualAllocEx/WriteProcessMemory/ReadProcessMemory + thread hijacking.
     * No kernel driver required — bypasses Roblox's CreateRemoteThread kill
     * by hijacking an existing wait-state thread to call DllMain.
     *
     * @param pid       Target process ID.
     * @param dllPath   Absolute path to the DLL file.
     * @return true if mapped and DllMain called successfully.
     */
    bool UserModeMapIntoProcess(uint32_t pid, const std::string& dllPath);

    /**
     * Diagnostic: test whether CreateRemoteThread works against the attached
     * target by calling Sleep(1) in the remote process. Returns true if the
     * remote thread completed successfully.
     */
    bool TestRemoteThread();

} // namespace Bootstrap
