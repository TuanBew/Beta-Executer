#include "Bootstrap.h"
#include "Engine.h"
#include "Logging/Logger.h"
#include <windows.h>
#include <cstdio>   // fprintf(stderr) for Phase 1 injection diagnostics

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
        fprintf(stderr, "[BS-DBG] LoadIntoProcess('%s') entered\n", dllPath.c_str());
        fflush(stderr);

        auto& engine = Engine::GetInstance();
        if (!engine.IsAttached()) {
            fprintf(stderr, "[BS-DBG] ERROR: not attached\n");
            LOG_ERROR("[Bootstrap] Not attached to a process");
            return false;
        }

        HANDLE hProcess = engine.GetProcessHandle();
        fprintf(stderr, "[BS-DBG] hProcess=0x%p, pathSize=%zu\n", hProcess, dllPath.size() + 1);
        fflush(stderr);

        // Step 1: Allocate memory in the target for the DLL path string
        size_t pathSize = dllPath.size() + 1;
        LPVOID pRemoteMem = VirtualAllocEx(hProcess, nullptr, pathSize,
                                            MEM_COMMIT | MEM_RESERVE,
                                            PAGE_READWRITE);
        if (!pRemoteMem) {
            fprintf(stderr, "[BS-DBG] VirtualAllocEx FAILED error=%lu\n", GetLastError());
            LOG_ERROR("[Bootstrap] VirtualAllocEx failed (error: %lu)", GetLastError());
            return false;
        }
        fprintf(stderr, "[BS-DBG] VirtualAllocEx OK: pRemoteMem=0x%p\n", pRemoteMem);
        fflush(stderr);

        // Step 2: Write the DLL path into the allocated remote memory
        if (!WriteProcessMemory(hProcess, pRemoteMem, dllPath.c_str(),
                                pathSize, nullptr)) {
            fprintf(stderr, "[BS-DBG] WriteProcessMemory FAILED error=%lu\n", GetLastError());
            LOG_ERROR("[Bootstrap] WriteProcessMemory failed (error: %lu)", GetLastError());
            VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);
            return false;
        }
        fprintf(stderr, "[BS-DBG] WriteProcessMemory OK\n");
        fflush(stderr);

        // Step 3: Get the address of LoadLibraryA from kernel32.dll
        // LoadLibraryA has the same address in all processes (kernel32 is
        // loaded at the same base address system-wide per session)
        LPTHREAD_START_ROUTINE pLoadLibraryA = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA"));
        if (!pLoadLibraryA) {
            fprintf(stderr, "[BS-DBG] GetProcAddress(LoadLibraryA) FAILED error=%lu\n", GetLastError());
            LOG_ERROR("[Bootstrap] GetProcAddress(LoadLibraryA) failed");
            VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);
            return false;
        }
        fprintf(stderr, "[BS-DBG] LoadLibraryA addr = 0x%p\n", pLoadLibraryA);
        fflush(stderr);

        // Step 4: Create a remote thread that calls LoadLibraryA(dllPath)
        fprintf(stderr, "[BS-DBG] Calling CreateRemoteThread(h=0x%p, fn=0x%p, arg=0x%p)...\n",
                hProcess, pLoadLibraryA, pRemoteMem);
        fflush(stderr);

        HANDLE hRemoteThread = CreateRemoteThread(hProcess, nullptr, 0,
                                                    pLoadLibraryA,
                                                    pRemoteMem, 0, nullptr);
        if (!hRemoteThread) {
            fprintf(stderr, "[BS-DBG] CreateRemoteThread FAILED error=%lu\n", GetLastError());
            LOG_ERROR("[Bootstrap] CreateRemoteThread failed (error: %lu)", GetLastError());
            VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);
            return false;
        }

        DWORD remoteTid = GetThreadId(hRemoteThread);
        fprintf(stderr, "[BS-DBG] CreateRemoteThread OK: hThread=0x%p tid=%lu\n", hRemoteThread, remoteTid);
        fflush(stderr);

        // Step 5: Wait for the remote thread to complete (with diagnostic timeout)
        fprintf(stderr, "[BS-DBG] WaitForSingleObject(hThread=0x%p, 5000ms)...\n", hRemoteThread);
        fflush(stderr);

        DWORD waitResult = WaitForSingleObject(hRemoteThread, 5000);

        fprintf(stderr, "[BS-DBG] WaitForSingleObject returned: %lu (0=OK, 258=TIMEOUT, 0xFFFFFFFF=ERROR)\n",
                waitResult);
        fflush(stderr);

        if (waitResult == WAIT_TIMEOUT) {
            // Diagnostic: check if thread is still alive
            DWORD exitCode = STILL_ACTIVE;
            GetExitCodeThread(hRemoteThread, &exitCode);

            fprintf(stderr, "[BS-DBG] TIMEOUT — exitCode=%lu (259=STILL_ACTIVE)\n", exitCode);

            LOG_ERROR("[Bootstrap] TIMEOUT after 5s — remote thread TID=%lu exitCode=%lu (STILL_ACTIVE=%s)",
                      remoteTid, exitCode,
                      exitCode == STILL_ACTIVE ? "YES" : "NO, exited with code above");

            // Thread is stuck — clean up but leave the remote memory since
            // the thread may still be referencing it (we can't safely free it)
            CloseHandle(hRemoteThread);
            return false;
        }

        // Thread completed (WAIT_OBJECT_0) or error
        LOG_INFO("[Bootstrap] Remote thread signaled: waitResult=%lu", waitResult);

        // Get the thread's exit code (return value of LoadLibraryA = HMODULE base)
        DWORD exitCode = 0;
        GetExitCodeThread(hRemoteThread, &exitCode);

        fprintf(stderr, "[BS-DBG] Thread exitCode=%lu (0=LoadLibraryA returned NULL, non-zero=DLL base)\n",
                exitCode);

        // Clean up
        CloseHandle(hRemoteThread);
        VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);

        if (exitCode == 0 || exitCode > 0x7FFFFFFF) {
            // exitCode == 0: LoadLibraryA returned NULL — DLL not found or failed.
            // exitCode > 0x7FFFFFFF: NTSTATUS error (e.g. STATUS_INVALID_THREAD = 0xC000071C)
            //   — the remote thread crashed, LoadLibraryA never executed.
            fprintf(stderr, "[BS-DBG] FAIL: exitCode=0x%08lX (%s)\n",
                    exitCode,
                    exitCode == 0 ? "NULL" : "NTSTATUS error — thread killed by target");
            LOG_ERROR("[Bootstrap] LoadLibraryA failed (exitCode=0x%08lX)", exitCode);
            return false;
        }

        fprintf(stderr, "[BS-DBG] SUCCESS: DLL loaded at remote base 0x%lX\n", exitCode);
        LOG_INFO("[Bootstrap] DLL loaded at remote base 0x%lX", exitCode);
        return true;
    }

    // Diagnostic test: does CreateRemoteThread work at all against this target?
    // Calls Sleep(1) instead of LoadLibraryA — if this returns true, the remote
    // thread mechanism is working and Roblox is specifically blocking LoadLibraryA.
    // If this also returns STATUS_INVALID_THREAD, Roblox blocks ALL remote threads.
    bool TestRemoteThread() {
        fprintf(stderr, "[BS-DBG] TestRemoteThread() — calling Sleep(1) in target\n");
        fflush(stderr);

        auto& engine = Engine::GetInstance();
        if (!engine.IsAttached()) {
            fprintf(stderr, "[BS-DBG] TestRemoteThread: not attached\n");
            return false;
        }

        HANDLE hProcess = engine.GetProcessHandle();
        LPTHREAD_START_ROUTINE pSleep = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleA("kernel32.dll"), "Sleep"));
        if (!pSleep) {
            fprintf(stderr, "[BS-DBG] TestRemoteThread: GetProcAddress(Sleep) failed\n");
            return false;
        }
        fprintf(stderr, "[BS-DBG] TestRemoteThread: Sleep addr = 0x%p\n", pSleep);

        HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0, pSleep,
                                             reinterpret_cast<LPVOID>(1), // Sleep(1)
                                             0, nullptr);
        if (!hThread) {
            fprintf(stderr, "[BS-DBG] TestRemoteThread: CreateRemoteThread FAILED error=%lu\n",
                    GetLastError());
            return false;
        }

        DWORD tid = GetThreadId(hThread);
        fprintf(stderr, "[BS-DBG] TestRemoteThread: hThread=0x%p tid=%lu — waiting 3s...\n",
                hThread, tid);
        fflush(stderr);

        DWORD wr = WaitForSingleObject(hThread, 3000);
        DWORD exitCode = 0xFFFFFFFF;
        GetExitCodeThread(hThread, &exitCode);
        CloseHandle(hThread);

        fprintf(stderr, "[BS-DBG] TestRemoteThread: waitResult=%lu exitCode=%lu (0x%08lX)\n",
                wr, exitCode, exitCode);
        return wr == WAIT_OBJECT_0;
    }

} // namespace Bootstrap

// ---- Phase 2: Manual-map injection through kernel R/W ----

#include "../Injector/ManualMapInjector.h"
#include "../Injector/CapcomDriver.h"
#include "../Injector/UserModeMapper.h"

namespace Bootstrap {

bool ManualMapIntoProcess(uint32_t pid, const std::string& dllPath) {
    return ManualMapInjector::GetInstance().Inject(pid, dllPath);
}

bool UserModeMapIntoProcess(uint32_t pid, const std::string& dllPath) {
    return UserModeMapper::GetInstance().Inject(pid, dllPath);
}

} // namespace Bootstrap
