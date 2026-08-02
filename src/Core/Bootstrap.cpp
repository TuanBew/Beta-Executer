#include "Bootstrap.h"
#include "Engine.h"
#include "Logging/Logger.h"
#include <windows.h>

/**
 * FOR EDUCATIONAL DEMONSTRATION ONLY
 *
 * This function demonstrates the classic CreateRemoteThread + LoadLibraryA
 * technique for loading a DLL into another process's address space.
 *
 * How it works:
 * 1. Allocate memory in the target process (VirtualAllocEx) to hold the DLL path string
 * 2. Write the DLL path into that allocated memory (WriteProcessMemory)
 * 3. Find the address of LoadLibraryA (same in all processes due to kernel32 base)
 * 4. Create a remote thread in the target that calls LoadLibraryA with the DLL path
 * 5. Wait for the thread to complete
 *
 * This is a well-documented OS-level mechanism. The engine uses this only as
 * an optional bootstrap; normally it runs standalone using external memory I/O.
 */

namespace Bootstrap {

    bool LoadIntoProcess(const std::string& dllPath) {
        auto& engine = Engine::GetInstance();
        if (!engine.IsAttached()) {
            LOG_ERROR("[Bootstrap] Not attached to a process");
            return false;
        }

        HANDLE hProcess = engine.GetProcessHandle();

        // Step 1: Allocate memory in the target for the DLL path string
        size_t pathSize = dllPath.size() + 1;
        LPVOID pRemoteMem = VirtualAllocEx(hProcess, nullptr, pathSize,
                                            MEM_COMMIT | MEM_RESERVE,
                                            PAGE_READWRITE);
        if (!pRemoteMem) {
            LOG_ERROR("[Bootstrap] VirtualAllocEx failed (error: %lu)", GetLastError());
            return false;
        }

        // Step 2: Write the DLL path into the allocated remote memory
        if (!WriteProcessMemory(hProcess, pRemoteMem, dllPath.c_str(),
                                pathSize, nullptr)) {
            LOG_ERROR("[Bootstrap] WriteProcessMemory failed (error: %lu)", GetLastError());
            VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);
            return false;
        }

        // Step 3: Get the address of LoadLibraryA from kernel32.dll
        // LoadLibraryA has the same address in all processes (kernel32 is
        // loaded at the same base address system-wide per session)
        LPTHREAD_START_ROUTINE pLoadLibraryA = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA"));
        if (!pLoadLibraryA) {
            LOG_ERROR("[Bootstrap] GetProcAddress(LoadLibraryA) failed");
            VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);
            return false;
        }

        // Step 4: Create a remote thread that calls LoadLibraryA(dllPath)
        HANDLE hRemoteThread = CreateRemoteThread(hProcess, nullptr, 0,
                                                    pLoadLibraryA,
                                                    pRemoteMem, 0, nullptr);
        if (!hRemoteThread) {
            LOG_ERROR("[Bootstrap] CreateRemoteThread failed (error: %lu)", GetLastError());
            VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);
            return false;
        }

        // Step 5: Wait for the remote thread to complete
        LOG_INFO("[Bootstrap] Waiting for remote LoadLibraryA to complete...");
        WaitForSingleObject(hRemoteThread, INFINITE);

        // Get the thread's exit code (return value of LoadLibraryA = HMODULE base)
        DWORD exitCode = 0;
        GetExitCodeThread(hRemoteThread, &exitCode);

        // Clean up
        CloseHandle(hRemoteThread);
        VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);

        if (exitCode == 0) {
            LOG_ERROR("[Bootstrap] LoadLibraryA returned NULL — DLL may have failed to load or path not found");
            return false;
        }

        LOG_INFO("[Bootstrap] DLL loaded at remote base 0x%lX", exitCode);
        return true;
    }

} // namespace Bootstrap

// ---- Phase 2: Manual-map injection through kernel R/W ----

#include "../Injector/ManualMapInjector.h"
#include "../Injector/CapcomDriver.h"

namespace Bootstrap {

bool ManualMapIntoProcess(DWORD pid, const std::string& dllPath) {
    return ManualMapInjector::GetInstance().Inject(pid, dllPath);
}

} // namespace Bootstrap
