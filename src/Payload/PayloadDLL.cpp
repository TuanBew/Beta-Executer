#include "PayloadDLL.h"
#include "PipeProtocol.h"
#include "../Core/offsets.h"   // shared offset definitions
#include <vector>
#include <string>
#include <cstdio>
#include <TlHelp32.h>

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

// ---- VEH State ----
static PVOID    g_vehHandle     = nullptr;
static uintptr_t g_luaPcallAddr = 0;
static DWORD     g_mainThreadId = 0;

// ---- Forward Declares ----
static DWORD WINAPI PipeClientThread(LPVOID param);
static bool InstallVEH();           // Task 3
static void RemoveVEH();            // Task 3
static bool CaptureLuaState();      // Task 3
static LONG CALLBACK VehHandler(PEXCEPTION_POINTERS ex); // Task 3
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

    if (!InstallVEH()) {
        // VEH failure is non-fatal — we can fall back to the jmp trampoline
        // method (CaptureLuaStateViaTrampoline) if needed.
    }

    // Trigger lua_State capture on the main thread.
    // This will fire on the next lua_pcall execution via hardware breakpoint.
    CaptureLuaState();

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

// ---- VEH Handler (Task 3) ----
static LONG CALLBACK VehHandler(PEXCEPTION_POINTERS ex) {
    // Only handle single-step (hardware breakpoint) exceptions
    if (ex->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Verify it's OUR breakpoint — the one we set on lua_pcall
    if (reinterpret_cast<uintptr_t>(ex->ExceptionRecord->ExceptionAddress)
        != g_luaPcallAddr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Capture lua_State* from RCX (x64 calling convention, first argument)
    g_luaState = ex->ContextRecord->Rcx;

    // Disable the hardware breakpoint so it fires only once.
    // Clear DR0 and DR7 L0 bit.
    ex->ContextRecord->Dr0 = 0;
    ex->ContextRecord->Dr7 &= ~0x1ULL;

    return EXCEPTION_CONTINUE_EXECUTION;
}

// ---- VEH Install / Remove (Task 3) ----
static bool InstallVEH() {
    g_vehHandle = AddVectoredExceptionHandler(1, VehHandler);
    return g_vehHandle != nullptr;
}

static void RemoveVEH() {
    if (g_vehHandle) {
        RemoveVectoredExceptionHandler(g_vehHandle);
        g_vehHandle = nullptr;
    }

    // Also clear the hardware breakpoint if it is still armed
    if (g_mainThreadId && g_luaPcallAddr) {
        HANDLE hThread = OpenThread(
            THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
            FALSE, g_mainThreadId);
        if (hThread) {
            SuspendThread(hThread);
            CONTEXT ctx = {};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(hThread, &ctx)) {
                ctx.Dr0 = 0;
                ctx.Dr7 &= ~0x1ULL;
                SetThreadContext(hThread, &ctx);
            }
            ResumeThread(hThread);
            CloseHandle(hThread);
        }
    }
}

// ---- lua_State Capture via Hardware Breakpoint (Task 3) ----
static bool CaptureLuaState() {
    // Resolve lua_pcall from the Luau DLL loaded in this process
    HMODULE hLuau = GetModuleHandleA("Luau");
    if (!hLuau) {
        // Luau might be statically linked or named differently in the target.
        // Fallback: could enumerate modules and search for "luau" substring.
        return false;
    }

    g_luaPcallAddr = reinterpret_cast<uintptr_t>(
        GetProcAddress(hLuau, "lua_pcall"));
    if (!g_luaPcallAddr) return false;

    // Enumerate threads to find the main thread (the one running the Lua
    // scheduler). Strategy: pick the first thread belonging to this process
    // for which we can open with the required access rights.
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return false;

    DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te = { sizeof(te) };
    DWORD targetThreadId = 0;

    if (Thread32First(hSnapshot, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) {
                // Skip threads we cannot open — keep looking
                HANDLE hThread = OpenThread(
                    THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                    THREAD_SUSPEND_RESUME,
                    FALSE, te.th32ThreadID);
                if (hThread) {
                    targetThreadId = te.th32ThreadID;
                    CloseHandle(hThread);
                    break;
                }
            }
        } while (Thread32Next(hSnapshot, &te));
    }
    CloseHandle(hSnapshot);

    if (!targetThreadId) return false;
    g_mainThreadId = targetThreadId;

    // Suspend the main thread and set DR0/DR7
    HANDLE hMainThread = OpenThread(
        THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
        FALSE, g_mainThreadId);
    if (!hMainThread) return false;

    SuspendThread(hMainThread);

    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(hMainThread, &ctx)) {
        ResumeThread(hMainThread);
        CloseHandle(hMainThread);
        return false;
    }

    // Set DR0 = lua_pcall address, enable local execute breakpoint in DR7
    ctx.Dr0 = g_luaPcallAddr;
    // DR7: L0=1 (local enable), RW0=00 (execute), LEN0=00 (1 byte)
    // Keep other bits as-is; only touch L0 and G0
    ctx.Dr7 |= 0x1ULL;   // L0 enable (bit 0)
    ctx.Dr7 &= ~0x2ULL;  // G0 disable — local breakpoint only (bit 1)

    if (!SetThreadContext(hMainThread, &ctx)) {
        ResumeThread(hMainThread);
        CloseHandle(hMainThread);
        return false;
    }

    ResumeThread(hMainThread);
    CloseHandle(hMainThread);
    return true;
}

// ---- Fallback: jmp Trampoline at lua_pcall (Task 3) ----
//
// If hardware breakpoints are monitored by the target's anti-tamper, this
// provides a byte-patching fallback. It writes a 14-byte jmp at lua_pcall
// that redirects to a trampoline, captures lua_State* from RCX, restores
// the original bytes, and returns.
static bool CaptureLuaStateViaTrampoline() {
    if (!g_luaPcallAddr) return false;

    // Allocate a RWX trampoline page
    void* trampPage = VirtualAlloc(
        nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampPage) return false;

    uint8_t* tramp = static_cast<uint8_t*>(trampPage);

    // Read the original 14 bytes at lua_pcall entry
    uint8_t origBytes[14];
    memcpy(origBytes, reinterpret_cast<void*>(g_luaPcallAddr), 14);

    // Build trampoline:
    //   mov rax, <&g_luaState>     ; 10 bytes
    //   mov [rax], rcx             ; 3 bytes  — capture lua_State*
    //   <original 14 bytes>        ; 14 bytes — execute stolen preamble
    //   jmp [rip+0]                ; 6 bytes
    //   <lua_pcall + 14>           ; 8 bytes  — absolute return address
    size_t off = 0;

    // mov rax, imm64  (0x48 0xB8 <8-byte address>)
    tramp[off++] = 0x48; tramp[off++] = 0xB8;
    *reinterpret_cast<uintptr_t*>(&tramp[off]) =
        reinterpret_cast<uintptr_t>(&g_luaState);
    off += 8;

    // mov [rax], rcx  (0x48 0x89 0x08)
    tramp[off++] = 0x48; tramp[off++] = 0x89; tramp[off++] = 0x08;

    // Copy original 14 bytes
    memcpy(&tramp[off], origBytes, 14);
    off += 14;

    // jmp [rip+0]  (0xFF 0x25 0x00 0x00 0x00 0x00 <8-byte target>)
    tramp[off++] = 0xFF; tramp[off++] = 0x25;
    tramp[off++] = 0x00; tramp[off++] = 0x00;
    tramp[off++] = 0x00; tramp[off++] = 0x00;
    *reinterpret_cast<uintptr_t*>(&tramp[off]) = g_luaPcallAddr + 14;
    off += 8;

    // Write jmp to trampoline at lua_pcall entry point (14-byte patch)
    DWORD oldProtect;
    VirtualProtect(reinterpret_cast<void*>(g_luaPcallAddr), 14,
                   PAGE_EXECUTE_READWRITE, &oldProtect);

    // jmp [rip+0] targeting tramp (14 bytes)
    uint8_t patch[14];
    patch[0] = 0xFF; patch[1] = 0x25;
    patch[2] = 0x00; patch[3] = 0x00;
    patch[4] = 0x00; patch[5] = 0x00;
    *reinterpret_cast<uintptr_t*>(&patch[6]) =
        reinterpret_cast<uintptr_t>(tramp);
    // Remaining 0 bytes — jmp [mem] is exactly 14 bytes

    memcpy(reinterpret_cast<void*>(g_luaPcallAddr), patch, 14);
    VirtualProtect(reinterpret_cast<void*>(g_luaPcallAddr), 14,
                   oldProtect, &oldProtect);

    // Poll for capture (wait for the next lua_pcall call)
    for (int i = 0; i < 100 && g_luaState == 0; i++) {
        Sleep(100);
    }

    // Restore original bytes at lua_pcall
    VirtualProtect(reinterpret_cast<void*>(g_luaPcallAddr), 14,
                   PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(reinterpret_cast<void*>(g_luaPcallAddr), origBytes, 14);
    VirtualProtect(reinterpret_cast<void*>(g_luaPcallAddr), 14,
                   oldProtect, &oldProtect);

    // Free the trampoline page (it won't be called again)
    VirtualFree(trampPage, 0, MEM_RELEASE);

    return g_luaState != 0;
}

// ---- Task 4 stubs (to be filled by Task 4) ----
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
