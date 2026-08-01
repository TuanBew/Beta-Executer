#include "Engine.h"
#include <iostream>

// ---- Singleton Access ----

Engine& Engine::GetInstance() {
    static Engine instance;
    return instance;
}

// ---- Destructor ----

Engine::~Engine() {
    Detach();
}

// ---- Attach by Process Name ----

bool Engine::AttachToProcess(const std::string& processName) {
    // If already attached, detach first
    if (m_hProcess) {
        Detach();
    }

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        std::cerr << "[Engine] Failed to create process snapshot" << std::endl;
        return false;
    }

    PROCESSENTRY32 pe32{};
    pe32.dwSize = sizeof(PROCESSENTRY32);

    DWORD pid = 0;
    if (Process32First(hSnapshot, &pe32)) {
        do {
            if (_stricmp(pe32.szExeFile, processName.c_str()) == 0) {
                pid = pe32.th32ProcessID;
                break;
            }
        } while (Process32Next(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);

    if (pid == 0) {
        std::cerr << "[Engine] Process '" << processName << "' not found" << std::endl;
        return false;
    }

    return AttachToProcess(pid);
}

// ---- Attach by PID ----

bool Engine::AttachToProcess(DWORD pid) {
    if (m_hProcess) {
        Detach();
    }

    // FOR EDUCATIONAL DEMONSTRATION ONLY — PROCESS_ALL_ACCESS is used
    // to enable full memory read/write capabilities for testing in a
    // controlled offline environment.
    m_hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!m_hProcess) {
        std::cerr << "[Engine] OpenProcess failed for PID " << pid
                  << " (error: " << GetLastError()
                  << "). Try running as Administrator." << std::endl;
        return false;
    }

    m_Pid = pid;
    ResolveModuleBase();

    std::cout << "[Engine] Attached to process '" << m_Pid
              << "' base: 0x" << std::hex << m_ModuleBase << std::dec << std::endl;
    return true;
}

// ---- Detach ----

void Engine::Detach() {
    if (m_hProcess) {
        CloseHandle(m_hProcess);
        m_hProcess = nullptr;
        std::cout << "[Engine] Detached from process " << m_Pid << std::endl;
    }
    m_Pid = 0;
    m_ModuleBase = 0;
}

// ---- State Queries ----

bool Engine::IsAttached() const {
    return m_hProcess != nullptr;
}

HANDLE Engine::GetProcessHandle() const {
    return m_hProcess;
}

DWORD Engine::GetPid() const {
    return m_Pid;
}

uintptr_t Engine::GetModuleBase() const {
    return m_ModuleBase;
}

// ---- Internal: Resolve Main Module Base ----

void Engine::ResolveModuleBase() {
    if (!m_hProcess || m_Pid == 0) return;

    HANDLE hSnapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, m_Pid);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;

    MODULEENTRY32 me32{};
    me32.dwSize = sizeof(MODULEENTRY32);

    // The first module in the list is the main executable
    if (Module32First(hSnapshot, &me32)) {
        m_ModuleBase = reinterpret_cast<uintptr_t>(me32.modBaseAddr);
    }

    CloseHandle(hSnapshot);
}
