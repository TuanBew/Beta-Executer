// src/Injector/CapcomDriver.cpp
#include "CapcomDriver.h"
#include <string>
#include <stdexcept>

CapcomDriver& CapcomDriver::GetInstance() {
    static CapcomDriver instance;
    return instance;
}

CapcomDriver::~CapcomDriver() {
    UnloadDriver();
}

// ---- Driver Lifecycle ----

bool CapcomDriver::LoadDriver() {
    if (m_hDevice != INVALID_HANDLE_VALUE) return true; // already loaded

    m_ownPid = GetCurrentProcessId();

    // 1. Enable SeLoadDriverPrivilege
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return false;

    LUID luid;
    if (!LookupPrivilegeValueW(nullptr, L"SeLoadDriverPrivilege", &luid)) {
        CloseHandle(hToken);
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    CloseHandle(hToken);

    // 2. Get full driver path (in same directory as exe)
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring driverDir(exePath);
    driverDir = driverDir.substr(0, driverDir.find_last_of(L"\\/") + 1);
    std::wstring driverPath = driverDir + CAPCOM_DRIVER_PATH;

    // 3. Open SCM and create service (or open existing)
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!hSCM) return false;

    SC_HANDLE hService = CreateServiceW(
        hSCM, CAPCOM_SERVICE_NAME, CAPCOM_SERVICE_NAME,
        SERVICE_START | SERVICE_STOP | DELETE,
        SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START, SERVICE_ERROR_IGNORE,
        driverPath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);

    if (!hService) {
        // Service may already exist — try to open it
        if (GetLastError() == ERROR_SERVICE_EXISTS) {
            hService = OpenServiceW(hSCM, CAPCOM_SERVICE_NAME, SERVICE_START | SERVICE_STOP | DELETE);
        }
        if (!hService) {
            CloseServiceHandle(hSCM);
            return false;
        }
    }

    // 4. Start the driver
    if (!StartServiceW(hService, 0, nullptr)) {
        if (GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
            DeleteService(hService);
            CloseServiceHandle(hService);
            CloseServiceHandle(hSCM);
            return false;
        }
    }

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);

    // 5. Open device handle
    m_hDevice = CreateFileW(
        L"\\\\.\\Capcom", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    return m_hDevice != INVALID_HANDLE_VALUE;
}

void CapcomDriver::UnloadDriver() {
    if (m_hDevice != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hDevice);
        m_hDevice = INVALID_HANDLE_VALUE;
    }

    // Stop and delete the service
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hSCM) return;

    SC_HANDLE hService = OpenServiceW(hSCM, CAPCOM_SERVICE_NAME, SERVICE_STOP | DELETE);
    if (hService) {
        SERVICE_STATUS status{};
        ControlService(hService, SERVICE_CONTROL_STOP, &status);
        DeleteService(hService);
        CloseServiceHandle(hService);
    }
    CloseServiceHandle(hSCM);
}

// ---- Kernel R/W Primitives ----

bool CapcomDriver::ReadMemory(DWORD pid, uintptr_t addr, void* outBuf, size_t size) {
    if (m_hDevice == INVALID_HANDLE_VALUE) return false;

    CapcomRequest req{};
    req.srcPid  = pid;
    req.dstPid  = m_ownPid;
    req.srcAddr = addr;
    req.dstAddr = reinterpret_cast<uint64_t>(outBuf);
    req.size    = size;
    req.flags   = 0;

    DWORD bytesReturned = 0;
    return DeviceIoControl(m_hDevice, CAPCOM_IOCTL_READWRITE,
                           &req, sizeof(req),
                           &req, sizeof(req),
                           &bytesReturned, nullptr) != FALSE;
}

bool CapcomDriver::WriteMemory(DWORD pid, uintptr_t addr, const void* inBuf, size_t size) {
    if (m_hDevice == INVALID_HANDLE_VALUE) return false;

    CapcomRequest req{};
    req.srcPid  = m_ownPid;
    req.dstPid  = pid;
    req.srcAddr = reinterpret_cast<uint64_t>(inBuf);
    req.dstAddr = addr;
    req.size    = size;
    req.flags   = 0;

    DWORD bytesReturned = 0;
    return DeviceIoControl(m_hDevice, CAPCOM_IOCTL_READWRITE,
                           &req, sizeof(req),
                           &req, sizeof(req),
                           &bytesReturned, nullptr) != FALSE;
}
