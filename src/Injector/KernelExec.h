// src/Injector/KernelExec.h
#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>
#include <string>

/**
 * KernelExec — Kernel Function Execution Primitive
 *
 * Leverages Capcom's kernel R/W to hijack a rarely-called kernel export
 * function pointer, redirect it to our shellcode, trigger execution from
 * user mode, and restore the original pointer — all within one operation.
 *
 * Primary use: call ZwAllocateVirtualMemory in the target process's
 * context when user-mode VirtualAllocEx may be hooked.
 *
 * Technique:
 *   1. Locate ntoskrnl.exe base + HalDispatchTable via export walking
 *   2. Read HalDispatchTable[1] original pointer
 *   3. Allocate shellcode buffer in kernel pool (or writable .data region)
 *   4. Write shellcode that calls ZwAllocateVirtualMemory
 *   5. Overwrite HalDispatchTable[1] → shellcode address
 *   6. Call NtQuerySystemInformation(0, ...) which triggers HalQuerySystemInformation
 *   7. Shellcode runs in kernel context, allocates memory, stores result
 *   8. Restore HalDispatchTable[1] → original pointer
 *
 * FOR EDUCATIONAL DEMONSTRATION ONLY — PatchGuard may detect HalDispatchTable
 * modification on retail builds. Use in offline VM environments.
 */
class KernelExec {
public:
    static KernelExec& GetInstance();

    bool Initialize();
    void Shutdown();
    bool IsInitialized() const { return m_initialized; }

    /**
     * Allocate `size` bytes of MEM_COMMIT | MEM_RESERVE memory in the
     * target process identified by `targetPid`.
     *
     * @param targetPid  The target process ID.
     * @param outBase    Receives the allocated base address (virtual, in target).
     * @param size       Allocation size in bytes.
     * @param protect    Page protection (default PAGE_EXECUTE_READWRITE).
     * @param outHandle  Optional: receives the kernel handle for the allocation.
     * @return NTSTATUS (0 = STATUS_SUCCESS).
     */
    uint64_t AllocateRemoteMemory(DWORD targetPid, uintptr_t& outBase,
                                  size_t size, DWORD protect = PAGE_EXECUTE_READWRITE,
                                  HANDLE* outHandle = nullptr);

    /**
     * Free memory previously allocated via AllocateRemoteMemory.
     * @param targetPid  The target process ID.
     * @param base       Base address to free.
     * @param size       Size to free (0 for entire region).
     * @return NTSTATUS.
     */
    uint64_t FreeRemoteMemory(DWORD targetPid, uintptr_t base, size_t size = 0);

    /**
     * Get the kernel base address of ntoskrnl.exe discovered during Initialize().
     */
    uintptr_t GetKernelBase() const { return m_kernelBase; }

private:
    KernelExec() = default;
    ~KernelExec() { Shutdown(); }
    KernelExec(const KernelExec&) = delete;
    KernelExec& operator=(const KernelExec&) = delete;

    bool LocateHalDispatchTable();
    uintptr_t FindKernelExport(const std::string& funcName);

    bool     m_initialized    = false;
    uintptr_t m_kernelBase    = 0;
    uintptr_t m_halDispatch   = 0; // address of HalDispatchTable
    uintptr_t m_originalPtr   = 0; // original HalDispatchTable[1] value
    uintptr_t m_shellcodeAddr = 0; // kernel address of our shellcode buffer
};
