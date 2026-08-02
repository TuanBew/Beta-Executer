#include "PayloadDLL.h"
#include "PipeProtocol.h"
#include "../Core/offsets.h"   // shared offset definitions
#include <vector>
#include <string>
#include <cstdio>

// ---- Global State ----
static HMODULE      g_hModule       = nullptr;
static HANDLE       g_hPipe         = INVALID_HANDLE_VALUE;
static HANDLE       g_hPipeThread   = nullptr;
static volatile bool g_running      = true;
static volatile uint32_t g_stateFlags = 0;

// These are populated by Tasks 3-4
static uintptr_t    g_luaState          = 0;   // VEH capture (Task 3)
static uintptr_t    g_scriptContext     = 0;   // Route H scan (Task 4)
static uintptr_t    g_heartbeatOrigAddr = 0;   // Heartbeat trampoline (Task 4)
static uint8_t      g_heartbeatOrigBytes[5] = {0};

// ---- Forward Declares ----
static DWORD WINAPI PipeClientThread(LPVOID param);
static bool InstallVEH();           // Task 3
static void RemoveVEH();            // Task 3
static bool CaptureLuaState();      // Task 3
static bool ElevatePrivilege();     // Task 4
static bool HijackHeartbeat();      // Task 4
static void RestoreHeartbeat();     // Task 4

// ---- DllMain ----
BOOL WINAPI DllMain(HINSTANCE hModule, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        return InitPayload(hModule);
    case DLL_PROCESS_DETACH:
        ShutdownPayload();
        return TRUE;
    }
    return TRUE;
}

// ---- InitPayload ----
BOOL InitPayload(HMODULE hModule) {
    g_hModule = hModule;

    // Spawn pipe client thread — it handles connection retries internally
    g_hPipeThread = CreateThread(
        nullptr, 0, PipeClientThread, nullptr, 0, nullptr);

    if (!g_hPipeThread) {
        return FALSE;
    }
    return TRUE;
}

// ---- ShutdownPayload ----
void ShutdownPayload() {
    g_running = false;

    // Wake the pipe thread if it's blocked on ReadFile.
    // Benign race: pipe thread may close g_hPipe between check and CancelIoEx.
    // If already closed, the thread is exiting naturally and WaitForSingleObject
    // returns quickly.
    if (g_hPipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(g_hPipe, nullptr);
    }

    if (g_hPipeThread) {
        WaitForSingleObject(g_hPipeThread, 3000);
        CloseHandle(g_hPipeThread);
    }

    RemoveVEH();
    // Don't restore heartbeat on detach — the process is exiting anyway
}

// ---- PipeClientThread ----
static DWORD WINAPI PipeClientThread(LPVOID) {
    // Retry loop: wait for server to be ready
    while (g_running) {
        g_hPipe = CreateFileA(
            "\\\\.\\pipe\\UniversalHub",
            GENERIC_READ | GENERIC_WRITE,
            0,                      // no sharing
            nullptr,
            OPEN_EXISTING,
            0,                      // synchronous I/O is fine for Phase 1
            nullptr);

        if (g_hPipe != INVALID_HANDLE_VALUE) break;

        DWORD err = GetLastError();
        if (err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND) {
            return 1; // unrecoverable
        }
        // Wait for server to create another instance
        if (!WaitNamedPipeA("\\\\.\\pipe\\UniversalHub", 5000)) {
            Sleep(1000); // backoff
            continue;
        }
    }

    if (g_hPipe == INVALID_HANDLE_VALUE) {
        return 1;
    }

    // Set to message mode for framed reads
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(g_hPipe, &mode, nullptr, nullptr);

    g_stateFlags |= PipeProtocol::STATE_READY;

    // ---- Command Read Loop ----
    uint8_t headerBuf[PipeProtocol::FRAME_HEADER_SIZE];
    std::vector<uint8_t> payloadBuf;

    while (g_running) {
        // Read header
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(g_hPipe, headerBuf, sizeof(headerBuf),
                           &bytesRead, nullptr);
        if (!ok || bytesRead != sizeof(headerBuf)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) break;
            continue;
        }

        PipeProtocol::Command cmd;
        uint32_t payloadLen = 0;
        if (!PipeProtocol::DeserializeFrameHeader(headerBuf, sizeof(headerBuf),
                                                   cmd, payloadLen)) {
            continue; // bad frame, skip
        }

        // Read payload if present
        if (payloadLen > 0) {
            payloadBuf.resize(payloadLen);
            ok = ReadFile(g_hPipe, payloadBuf.data(), payloadLen,
                          &bytesRead, nullptr);
            if (!ok) break;
        }

        // Dispatch
        switch (cmd) {
        case PipeProtocol::Command::EXECUTE_SCRIPT:
            // Task 6 will handle this via the Script Manager callback
            // For now: stub — we need the Script Manager running first
            break;

        case PipeProtocol::Command::PING: {
            auto pong = PipeProtocol::MakeSimpleFrame(
                PipeProtocol::Command::PONG);
            // For PONG we want state flags; Task 6 will add proper payload
            WriteFile(g_hPipe, pong.data(),
                      static_cast<DWORD>(pong.size()), &bytesRead, nullptr);
            break;
        }

        case PipeProtocol::Command::SHUTDOWN:
            g_running = false;
            break;

        default:
            break;
        }
    }

    CloseHandle(g_hPipe);
    g_hPipe = INVALID_HANDLE_VALUE;
    return 0;
}

// ---- Stubs (populated by Tasks 3-4) ----
static bool InstallVEH() {
    // Task 3: install vectored exception handler
    return false;
}

static void RemoveVEH() {
    // Task 3: remove vectored exception handler
}

static bool CaptureLuaState() {
    // Task 3: capture Lua state via VEH + intentional access violation
    return false;
}

static bool ElevatePrivilege() {
    // Task 4: elevate Roblox script context privilege
    return false;
}

static bool HijackHeartbeat() {
    // Task 4: install heartbeat trampoline for script execution
    return false;
}

static void RestoreHeartbeat() {
    // Task 4: restore original heartbeat bytes
}
