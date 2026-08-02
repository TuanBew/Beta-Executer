#include "PayloadDLL.h"
#include "PipeProtocol.h"
#include "../Core/offsets.h"   // shared offset definitions
#include <vector>
#include <string>
#include <cstdio>
#include <TlHelp32.h>
#include <cstring>

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
extern "C" void HeartbeatTrampolineCallback(); // Task 4 trampoline

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

    // Wait for lua_State capture (VEH fires asynchronously on the main thread).
    // The target's Lua scheduler calls lua_pcall every frame, so the breakpoint
    // should hit within a few seconds at most.
    for (int i = 0; i < 200 && g_luaState == 0; i++) {
        Sleep(50);  // 10 second timeout total
    }

    // Task 4: elevate privilege, then hijack the Heartbeat scheduler job.
    // Elevation MUST happen before hijack — the injected Script Manager needs
    // level 10 identity to execute privileged APIs.
    if (g_luaState && ElevatePrivilege() && HijackHeartbeat()) {
        // Heartbeat trampoline will fire on the next scheduler tick,
        // injecting the Script Manager. The trampoline restores itself.
        g_stateFlags |= PipeProtocol::STATE_READY;
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

// ---- Task 5 stub: Script Manager source (populated by Task 5) ----
// These are temporary empty placeholders. Task 5 will replace them with
// the embedded Lua bytecode for the Script Manager. The Heartbeat trampoline
// callback references these symbols so the injection site is pre-wired.
static const char  g_ScriptManagerSource[]   = "-- Task 5 placeholder\n";
static const size_t g_ScriptManagerSourceLen  = 0;

// ================================================================
//  Task 4: Privilege Elevation & Heartbeat Hijack
// ================================================================

// ---- ElevatePrivilege: Local Route H heap scan + identity write ----
//
// Since we are inside the target process, all memory access is via direct
// pointer dereference (no RPM/WPM). We walk every committed, private,
// readable memory region and test each 8-byte-aligned value as a potential
// Roblox object pointer by checking ClassDescriptor->ClassName == "ScriptContext".
//
// On success, writes identity level 10 to ScriptContext+0x2C0 and populates
// g_scriptContext. This MUST complete before the Heartbeat hijack fires.
static bool ElevatePrivilege() {
    // Step 1: Locate ScriptContext via Route H logic (local version).
    // Walk memory regions directly, test each 8-byte aligned value as a
    // potential object pointer — check ClassDescriptor->ClassName.

    SYSTEM_INFO si;
    GetSystemInfo(&si);

    uintptr_t start = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    uintptr_t end   = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);

    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = start;

    while (addr < end) {
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) {
            addr += si.dwPageSize;
            continue;
        }

        // Only scan committed, private, readable memory (heap regions).
        // Exclude guard pages, no-access regions, and mapped images.
        if (mbi.State != MEM_COMMIT ||
            mbi.Type  != MEM_PRIVATE ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            continue;
        }

        // Scan the region for potential ScriptContext pointers.
        // Every 8-byte aligned value is a candidate.
        uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        size_t regionSize = mbi.RegionSize;
        size_t scanSize = regionSize - (regionSize % 8);

        for (size_t off = 0; off < scanSize; off += 8) {
            __try {
                uintptr_t candidate = *reinterpret_cast<uintptr_t*>(regionStart + off);

                // Quick bounds check — discard obviously invalid pointers
                if (candidate < reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress) ||
                    candidate > reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress)) {
                    continue;
                }
                // Must be 8-byte aligned (Roblox objects are heap allocated)
                if ((candidate & 0x7) != 0) continue;

                // Check ClassDescriptor at candidate+0x18
                uintptr_t classDesc = *reinterpret_cast<uintptr_t*>(candidate + offsets::ClassDescriptor);
                if (!classDesc) continue;
                if (classDesc < reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress) ||
                    classDesc > reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress)) {
                    continue;
                }

                // Check ClassName at ClassDescriptor+0x8
                uintptr_t classNamePtr = *reinterpret_cast<uintptr_t*>(classDesc + offsets::ClassDescriptorToClassName);
                if (!classNamePtr) continue;

                // Read class name string and compare
                const char* className = reinterpret_cast<const char*>(classNamePtr);
                if (className && strcmp(className, "ScriptContext") == 0) {
                    g_scriptContext = candidate;
                    break;
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }
        }

        if (g_scriptContext) break;
        addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }

    if (!g_scriptContext) return false;

    // Step 2: Write identity level 10 to ScriptContext+0x2C0.
    // Use VirtualProtect to ensure writability, then restore.
    int* pIdentityLevel = reinterpret_cast<int*>(g_scriptContext + offsets::ScriptContextIdentityLevel);
    DWORD oldProtect;
    VirtualProtect(pIdentityLevel, sizeof(int), PAGE_READWRITE, &oldProtect);
    *pIdentityLevel = 10;
    VirtualProtect(pIdentityLevel, sizeof(int), oldProtect, &oldProtect);

    // Step 3: Verify the write took effect.
    int verifyLevel = *reinterpret_cast<int*>(g_scriptContext + offsets::ScriptContextIdentityLevel);
    return verifyLevel == 10;
}

// ---- HeartbeatTrampolineCallback (extern "C" — called from raw assembly) ----
//
// The trampoline (written in raw bytes by HijackHeartbeat) calls this C
// function. It executes in the Heartbeat job's thread context, within the
// target's Lua scheduler frame.
//
// ONE-SHOT contract: the FIRST thing this callback does is restore the
// original Heartbeat function bytes. After that, no persistent C++ hooks
// remain. The Script Manager (injected here) takes over from Lua.
extern "C" void HeartbeatTrampolineCallback() {
    if (!g_luaState) return;

    // ONE-SHOT: Restore original Heartbeat bytes BEFORE any risk of crash.
    // If we crash during Script Manager injection, the Heartbeat is already
    // restored — the target continues normally on the next tick.
    RestoreHeartbeat();

    // ---- Script Manager Injection (stub — filled by Task 5/6) ----
    //
    // The Script Manager source (g_ScriptManagerSource) will be embedded as
    // a static const char array in Task 5. Task 6 will wire the actual
    // luau_compile + luau_load + lua_pcall sequence.
    //
    // For now, this is a call site that references the Task 5 symbols:
    //   size_t bytecodeSize = 0;
    //   char* bytecode = luau_compile(g_ScriptManagerSource,
    //                                  g_ScriptManagerSourceLen,
    //                                  nullptr, &bytecodeSize);
    //   if (bytecode) {
    //       int status = luau_load(g_luaState, "=ScriptManager",
    //                              bytecode, bytecodeSize, 0);
    //       free(bytecode);
    //       if (status == 0) {
    //           lua_pcall(g_luaState, 0, 0, 0);
    //       }
    //   }

    // After the Script Manager is loaded, it takes over —
    // it registers with the target's Heartbeat scheduler and
    // polls the pipe on each tick. No further C++ involvement.
    (void)g_ScriptManagerSource;    // suppress unused-variable warning
    (void)g_ScriptManagerSourceLen;
}

// ---- HijackHeartbeat: locate Heartbeat job and install one-shot trampoline ----
//
// 1. Read TaskScheduler pointer from the absolute address.
// 2. Walk the job array to find the job whose name contains "Heartbeat".
// 3. Scan candidate offsets within the job struct for a function pointer
//    (validated by checking that it points to executable memory).
// 4. Allocate a trampoline page, build the x64 call stub:
//      sub rsp, 0x28        ; shadow space
//      call HeartbeatTrampolineCallback
//      add rsp, 0x28
//      <original 5 bytes>
//      jmp <heartbeat + 5>
// 5. Save original 5 bytes and write jmp <trampoline> at Heartbeat entry.
static bool HijackHeartbeat() {
    if (!g_luaState) return false;

    // Step 1: Locate TaskScheduler via absolute pointer.
    uintptr_t tsAddr = *reinterpret_cast<uintptr_t*>(offsets::TaskSchedulerPointer);
    if (!tsAddr) return false;

    // Step 2: Walk job array to find the Heartbeat job.
    uintptr_t jobStart = 0, jobEnd = 0;
    __try {
        jobStart = *reinterpret_cast<uintptr_t*>(tsAddr + offsets::JobStart);
        jobEnd   = *reinterpret_cast<uintptr_t*>(tsAddr + offsets::JobEnd);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    if (!jobStart || !jobEnd || jobEnd <= jobStart) return false;

    uintptr_t heartbeatJob = 0;
    size_t maxJobs = 256;
    size_t count = 0;

    for (uintptr_t cur = jobStart; cur < jobEnd && count < maxJobs;
         cur += sizeof(uintptr_t), ++count) {
        __try {
            uintptr_t jobPtr = *reinterpret_cast<uintptr_t*>(cur);
            if (!jobPtr) continue;

            // Check job name at Job_Name offset
            uintptr_t namePtr = *reinterpret_cast<uintptr_t*>(jobPtr + offsets::Job_Name);
            if (!namePtr) continue;

            const char* name = reinterpret_cast<const char*>(namePtr);
            if (name && strstr(name, "Heartbeat")) {
                heartbeatJob = jobPtr;
                break;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
    }

    if (!heartbeatJob) return false;

    // Step 3: Scan the job struct for the callback function pointer.
    // The exact offset varies by build; try a list of known candidates.
    // Validate by checking that the pointer targets executable memory.
    uintptr_t hbFuncPtr = 0;
    const uintptr_t candidateOffsets[] = { 0x10, 0x28, 0x38, 0x48, 0x58 };

    for (uintptr_t candidateOff : candidateOffsets) {
        __try {
            uintptr_t candidate = *reinterpret_cast<uintptr_t*>(heartbeatJob + candidateOff);
            if (!candidate) continue;

            // Validate: does this address fall in executable memory?
            MEMORY_BASIC_INFORMATION mbi2;
            if (VirtualQuery(reinterpret_cast<LPCVOID>(candidate), &mbi2, sizeof(mbi2))) {
                if (mbi2.State == MEM_COMMIT &&
                    (mbi2.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
                    hbFuncPtr = candidate;
                    break;
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
    }

    if (!hbFuncPtr) return false;
    g_heartbeatOrigAddr = hbFuncPtr;

    // Step 4: Save the original 5 bytes at the Heartbeat entry point.
    __try {
        memcpy(g_heartbeatOrigBytes, reinterpret_cast<void*>(hbFuncPtr), 5);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    // Step 5: Allocate a trampoline page and build the call stub.
    //
    // Layout (total 23 bytes, well within one page):
    //   Offset  Size  Instruction
    //   0       4     sub rsp, 0x28          ; x64 shadow space
    //   4       5     call rel32 HeartbeatTrampolineCallback
    //   9       4     add rsp, 0x28
    //   13      5     <original 5 bytes>     ; stolen from Heartbeat entry
    //   18      5     jmp rel32 <heartbeat + 5>  ; continue original function
    void* trampPage = VirtualAlloc(nullptr, 0x1000,
                                    MEM_COMMIT | MEM_RESERVE,
                                    PAGE_EXECUTE_READWRITE);
    if (!trampPage) return false;

    uint8_t* t = static_cast<uint8_t*>(trampPage);
    size_t pos = 0;

    // sub rsp, 0x28
    t[pos++] = 0x48; t[pos++] = 0x83; t[pos++] = 0xEC; t[pos++] = 0x28;

    // call rel32 HeartbeatTrampolineCallback
    // rel32 = target - (trampPage + pos + 5)  [RIP = instr_addr + 5 for E8]
    t[pos] = 0xE8;
    int32_t callRel = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(&HeartbeatTrampolineCallback) -
        (reinterpret_cast<uintptr_t>(t) + pos + 5));
    memcpy(&t[pos + 1], &callRel, 4);
    pos += 5;

    // add rsp, 0x28
    t[pos++] = 0x48; t[pos++] = 0x83; t[pos++] = 0xC4; t[pos++] = 0x28;

    // Original 5 bytes from Heartbeat entry
    memcpy(&t[pos], g_heartbeatOrigBytes, 5);
    pos += 5;

    // jmp rel32 to heartbeat + 5 (continue after the patched 5 bytes)
    // rel32 = (heartbeat + 5) - (trampPage + pos + 5)  [RIP = instr_addr + 5 for E9]
    t[pos] = 0xE9;
    int32_t jmpBackRel = static_cast<int32_t>(
        (hbFuncPtr + 5) - (reinterpret_cast<uintptr_t>(t) + pos + 5));
    memcpy(&t[pos + 1], &jmpBackRel, 4);
    pos += 5;

    // Step 6: Write jmp to trampoline at the Heartbeat entry point.
    DWORD oldProtect;
    VirtualProtect(reinterpret_cast<void*>(hbFuncPtr), 5,
                   PAGE_EXECUTE_READWRITE, &oldProtect);

    uint8_t jmpPatch[5];
    jmpPatch[0] = 0xE9;  // jmp rel32
    int32_t jmpRel = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(trampPage) - (hbFuncPtr + 5));
    memcpy(&jmpPatch[1], &jmpRel, 4);
    memcpy(reinterpret_cast<void*>(hbFuncPtr), jmpPatch, 5);

    VirtualProtect(reinterpret_cast<void*>(hbFuncPtr), 5, oldProtect, &oldProtect);

    return true;
}

// ---- RestoreHeartbeat: restore original 5 bytes at Heartbeat entry ----
//
// Called by HeartbeatTrampolineCallback on its first (and only) invocation.
// This is the ONE-SHOT mechanism: the trampoline fires once, restores the
// original Heartbeat code, and then returns. Subsequent Heartbeat ticks
// execute the original function with no C++ involvement.
static void RestoreHeartbeat() {
    if (!g_heartbeatOrigAddr) return;

    DWORD oldProtect;
    VirtualProtect(reinterpret_cast<void*>(g_heartbeatOrigAddr), 5,
                   PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(reinterpret_cast<void*>(g_heartbeatOrigAddr),
           g_heartbeatOrigBytes, 5);
    VirtualProtect(reinterpret_cast<void*>(g_heartbeatOrigAddr), 5,
                   oldProtect, &oldProtect);

    // Clear the saved state so RestoreHeartbeat is idempotent
    g_heartbeatOrigAddr = 0;
    memset(g_heartbeatOrigBytes, 0, sizeof(g_heartbeatOrigBytes));
}
