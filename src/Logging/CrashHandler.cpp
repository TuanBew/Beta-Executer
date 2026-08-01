/**
 * CrashHandler.cpp — Crash & Exception Capture Implementation
 *
 * Installs four handlers that funnel into a single OnCrash() path:
 *   1. SEH (SetUnhandledExceptionFilter) — access violations, stack overflow, etc.
 *   2. C++ (std::set_terminate)          — uncaught C++ exceptions
 *   3. SIGABRT (signal)                  — abort() calls
 *   4. SIGSEGV (signal)                  — segfaults
 *
 * Safety: static re-entry guard, kernel32-only file I/O in crash path,
 * bounded stack depth (32 frames), bounded log dump (20 entries).
 *
 * FOR EDUCATIONAL DEMONSTRATION ONLY
 */

#include "Logging/CrashHandler.h"
#include "Logging/Logger.h"

#include <cstdio>
#include <ctime>
#include <csignal>
#include <io.h>
#include <fcntl.h>
#include <dbghelp.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace Logging {

// ---- Re-entry Guard ----
// Prevent infinite recursion if the crash handler itself crashes.
// Set to true on entry, checked before doing any work.
static volatile LONG s_inCrashHandler = 0;

// ---- Forward Declarations ----
static void WriteHeader(FILE* f, const char* exceptionType, DWORD code, uintptr_t addr);

// ============================================================
//  Singleton
// ============================================================

CrashHandler& CrashHandler::GetInstance() {
    static CrashHandler instance;
    return instance;
}

CrashHandler::~CrashHandler() {
    Uninstall();
}

// ============================================================
//  Install / Uninstall
// ============================================================

bool CrashHandler::Install(const std::string& logDir) {
    if (m_installed) return true;

    m_logDir = logDir;
    CreateDirectoryA(logDir.c_str(), nullptr);

    // ---- Load dbghelp.dll dynamically ----
    m_dbghelpDll = LoadLibraryA("dbghelp.dll");
    if (m_dbghelpDll) {
        m_pSymInitialize       = (SymInitialize_t)      GetProcAddress(m_dbghelpDll, "SymInitialize");
        m_pSymCleanup          = (SymCleanup_t)         GetProcAddress(m_dbghelpDll, "SymCleanup");
        m_pSymFromAddr         = (SymFromAddr_t)        GetProcAddress(m_dbghelpDll, "SymFromAddr");
        m_pSymGetLineFromAddr64 = (SymGetLineFromAddr64_t)GetProcAddress(m_dbghelpDll, "SymGetLineFromAddr64");
        m_pStackWalk64         = (StackWalk64_t)        GetProcAddress(m_dbghelpDll, "StackWalk64");
        m_pMiniDumpWriteDump   = (MiniDumpWriteDump_t)  GetProcAddress(m_dbghelpDll, "MiniDumpWriteDump");

        // Initialize symbol handler for the current process
        if (m_pSymInitialize) {
            m_pSymInitialize(GetCurrentProcess(), nullptr, TRUE);
        }
    }

    // ---- Install handlers ----

    // SEH: Unhandled exceptions (access violations, stack overflow, etc.)
    m_prevSEHFilter = SetUnhandledExceptionFilter(OnUnhandledException);

    // C++: Uncaught exceptions
    m_prevTerminateHandler = std::set_terminate(OnTerminate);

    // Signals: abort(), segfault
    signal(SIGABRT, OnAbortSignal);
    signal(SIGSEGV, OnAbortSignal);

    m_installed = true;

    LOG_INFO("CrashHandler installed — crash artifacts will be written to %s/", logDir.c_str());
    return true;
}

void CrashHandler::Uninstall() {
    if (!m_installed) return;

    // Restore previous handlers
    SetUnhandledExceptionFilter(m_prevSEHFilter);
    std::set_terminate(m_prevTerminateHandler);
    signal(SIGABRT, SIG_DFL);
    signal(SIGSEGV, SIG_DFL);

    // Cleanup symbol handler
    if (m_pSymCleanup) {
        m_pSymCleanup(GetCurrentProcess());
    }

    if (m_dbghelpDll) {
        FreeLibrary(m_dbghelpDll);
        m_dbghelpDll = nullptr;
    }

    m_installed = false;
}

bool CrashHandler::IsInstalled() const {
    return m_installed;
}

// ============================================================
//  Test Trigger
// ============================================================

void CrashHandler::TriggerCrash(const char* reason) {
    LOG_FATAL("CrashHandler::TriggerCrash called: %s", reason);

    // Generate a controlled crash by raising an access violation
    // The SEH handler will catch this and produce the crash artifacts
    RaiseException(EXCEPTION_ACCESS_VIOLATION,
                   EXCEPTION_NONCONTINUABLE,
                   0, nullptr);
}

// ============================================================
//  Handler Callbacks
// ============================================================

LONG WINAPI CrashHandler::OnUnhandledException(EXCEPTION_POINTERS* ep) {
    // Re-entry guard
    if (InterlockedExchange(&s_inCrashHandler, 1) != 0) {
        // Already in crash handler — terminate immediately
        TerminateProcess(GetCurrentProcess(), 0xC0000005);
    }

    DWORD code = ep->ExceptionRecord->ExceptionCode;
    uintptr_t addr = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);

    GetInstance().OnCrash("SEH Exception", code, addr, ep);

    // OnCrash should terminate, but just in case:
    TerminateProcess(GetCurrentProcess(), code);
    return EXCEPTION_CONTINUE_SEARCH; // unreachable
}

void __cdecl CrashHandler::OnTerminate() {
    if (InterlockedExchange(&s_inCrashHandler, 1) != 0) {
        TerminateProcess(GetCurrentProcess(), 0xE0000000);
    }

    GetInstance().OnCrash("Uncaught C++ Exception", 0xE0000000, 0, nullptr);
    TerminateProcess(GetCurrentProcess(), 0xE0000000);
}

void CrashHandler::OnAbortSignal(int signum) {
    if (InterlockedExchange(&s_inCrashHandler, 1) != 0) {
        TerminateProcess(GetCurrentProcess(), 0x80000000);
    }

    DWORD code = (signum == SIGABRT) ? 0x80000001 : 0x80000002;
    GetInstance().OnCrash(signum == SIGABRT ? "SIGABRT" : "SIGSEGV", code, 0, nullptr);
    TerminateProcess(GetCurrentProcess(), code);
}

// ============================================================
//  Core Crash Processing
// ============================================================

void CrashHandler::OnCrash(const char* exceptionType, DWORD exceptionCode,
                            uintptr_t faultAddr, EXCEPTION_POINTERS* ep) {
    // 1. Write human-readable crash log
    WriteCrashLog(exceptionType, exceptionCode, faultAddr, ep);

    // 2. Write minidump (if we have exception pointers)
    if (ep) {
        WriteMinidump(ep);
    }
}

// ============================================================
//  Crash Log Writer (kernel32-only — no CRT heap)
// ============================================================

void CrashHandler::WriteCrashLog(const char* exceptionType, DWORD exceptionCode,
                                  uintptr_t faultAddr, EXCEPTION_POINTERS* ep) {
    // Build timestamped filename
    auto now = time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    char filename[256];
    snprintf(filename, sizeof(filename), "%s\\crash_%04d-%02d-%02d_%02d-%02d-%02d.log",
             m_logDir.c_str(),
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);

    // Open with CreateFileA — no CRT, safe in crash context
    HANDLE hFile = CreateFileA(filename, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;

    // Wrap in CRT FILE* for fprintf convenience (safe here since we checked re-entry)
    int fd = _open_osfhandle((intptr_t)hFile, 0);
    FILE* f = _fdopen(fd, "w");
    if (!f) { CloseHandle(hFile); return; }

    // --- Header ---
    char timeBuf[64];
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tm);

    fprintf(f, "=== UNIVERSAL HUB CRASH REPORT ===\n");
    fprintf(f, "Timestamp:   %s\n", timeBuf);
    fprintf(f, "Exception:   0x%08lX — %s\n", exceptionCode, exceptionType);
    fprintf(f, "Fault addr:  0x%p\n", (void*)faultAddr);
    fprintf(f, "Thread ID:   %lu\n", GetCurrentThreadId());
#ifdef _DEBUG
    fprintf(f, "Build:       UniversalHub v1.0.0 (Debug)\n");
#else
    fprintf(f, "Build:       UniversalHub v1.0.0 (Release)\n");
#endif
    fprintf(f, "\n");

    // --- Register Dump ---
    if (ep && ep->ContextRecord) {
        WriteRegisterDump(ep->ContextRecord, f);
    }

    // --- Stack Trace ---
    fprintf(f, "--- Stack Trace ---\n");
    if (ep && ep->ContextRecord) {
        WriteStackTrace(GetCurrentProcess(), GetCurrentThread(),
                        ep->ContextRecord, f);
    } else {
        fprintf(f, "  (no context available)\n");
    }
    fprintf(f, "\n");

    // --- Recent Logs ---
    fprintf(f, "--- Recent Logs ---\n");
    std::string recentLogs = Logger::GetInstance().GetRecentLogs(20);
    if (!recentLogs.empty()) {
        fprintf(f, "%s", recentLogs.c_str());
    } else {
        fprintf(f, "  (no log entries available)\n");
    }

    fclose(f);
    // hFile is closed by fclose
}

// ============================================================
//  Minidump Writer
// ============================================================

void CrashHandler::WriteMinidump(EXCEPTION_POINTERS* ep) {
    if (!m_pMiniDumpWriteDump) return;

    auto now = time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    char filename[256];
    snprintf(filename, sizeof(filename), "%s\\crash_%04d-%02d-%02d_%02d-%02d-%02d.dmp",
             m_logDir.c_str(),
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);

    HANDLE hFile = CreateFileA(filename, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;

    m_pMiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        hFile,
        (MINIDUMP_TYPE)(MiniDumpWithFullMemory | MiniDumpWithHandleData),
        &mei,
        nullptr,
        nullptr);

    CloseHandle(hFile);
}

// ============================================================
//  Register Dump (x64)
// ============================================================

void CrashHandler::WriteRegisterDump(CONTEXT* ctx, FILE* f) {
    fprintf(f, "--- Register Dump ---\n");
#ifdef _M_X64
    fprintf(f, "RAX=0x%016llX RBX=0x%016llX RCX=0x%016llX RDX=0x%016llX\n",
            ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx);
    fprintf(f, "RIP=0x%016llX RSP=0x%016llX RBP=0x%016llX\n",
            ctx->Rip, ctx->Rsp, ctx->Rbp);
    fprintf(f, "RSI=0x%016llX RDI=0x%016llX\n",
            ctx->Rsi, ctx->Rdi);
    fprintf(f, "R8 =0x%016llX R9 =0x%016llX R10=0x%016llX R11=0x%016llX\n",
            ctx->R8, ctx->R9, ctx->R10, ctx->R11);
    fprintf(f, "R12=0x%016llX R13=0x%016llX R14=0x%016llX R15=0x%016llX\n",
            ctx->R12, ctx->R13, ctx->R14, ctx->R15);
#else
    fprintf(f, "EAX=0x%08lX EBX=0x%08lX ECX=0x%08lX EDX=0x%08lX\n",
            ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx);
    fprintf(f, "EIP=0x%08lX ESP=0x%08lX EBP=0x%08lX\n",
            ctx->Eip, ctx->Esp, ctx->Ebp);
#endif
    fprintf(f, "\n");
}

// ============================================================
//  Stack Walking
// ============================================================

void CrashHandler::WriteStackTrace(HANDLE hProcess, HANDLE hThread,
                                    CONTEXT* context, FILE* f) {
    if (!m_pStackWalk64 || !m_pSymFromAddr) {
        fprintf(f, "  (dbghelp.dll not available — no stack trace)\n");
        return;
    }

    // Initialize stack frame
    STACKFRAME64 sf{};
#ifdef _M_X64
    sf.AddrPC.Offset    = context->Rip;
    sf.AddrPC.Mode      = AddrModeFlat;
    sf.AddrFrame.Offset = context->Rbp;
    sf.AddrFrame.Mode   = AddrModeFlat;
    sf.AddrStack.Offset = context->Rsp;
    sf.AddrStack.Mode   = AddrModeFlat;
    DWORD machineType = IMAGE_FILE_MACHINE_AMD64;
#else
    sf.AddrPC.Offset    = context->Eip;
    sf.AddrPC.Mode      = AddrModeFlat;
    sf.AddrFrame.Offset = context->Ebp;
    sf.AddrFrame.Mode   = AddrModeFlat;
    sf.AddrStack.Offset = context->Esp;
    sf.AddrStack.Mode   = AddrModeFlat;
    DWORD machineType = IMAGE_FILE_MACHINE_I386;
#endif

    // Symbol buffer — SYMBOL_INFO is variable-length
    char symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symBuf);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

    for (int frameNum = 0; frameNum < 32; ++frameNum) {
        if (!m_pStackWalk64(machineType, hProcess, hThread, &sf,
                            context, nullptr,
                            SymFunctionTableAccess64,
                            SymGetModuleBase64, nullptr)) {
            break;
        }

        if (sf.AddrPC.Offset == 0) break;

        DWORD64 addr = sf.AddrPC.Offset;
        DWORD64 displacement = 0;

        // Try to get symbol name
        if (m_pSymFromAddr(hProcess, addr, &displacement, symbol)) {
            DWORD lineDisplacement = 0;

            // Try to get source file and line
            if (m_pSymGetLineFromAddr64 &&
                m_pSymGetLineFromAddr64(hProcess, addr, &lineDisplacement, &line)) {
                fprintf(f, "  [%d] %s!%s+0x%llX (%s:%lu)\n",
                        frameNum,
                        line.FileName ? line.FileName : "???",
                        symbol->Name,
                        displacement,
                        line.FileName ? line.FileName : "???",
                        line.LineNumber);
            } else {
                // Get module name
                IMAGEHLP_MODULE64 modInfo{};
                modInfo.SizeOfStruct = sizeof(modInfo);
                if (SymGetModuleInfo64(hProcess, addr, &modInfo)) {
                    fprintf(f, "  [%d] %s!%s+0x%llX\n",
                            frameNum, modInfo.ModuleName, symbol->Name, displacement);
                } else {
                    fprintf(f, "  [%d] %s+0x%llX\n",
                            frameNum, symbol->Name, displacement);
                }
            }
        } else {
            // No symbol — show module+offset
            IMAGEHLP_MODULE64 modInfo{};
            modInfo.SizeOfStruct = sizeof(modInfo);
            if (SymGetModuleInfo64(hProcess, addr, &modInfo)) {
                fprintf(f, "  [%d] %s+0x%llX\n",
                        frameNum, modInfo.ModuleName,
                        addr - modInfo.BaseOfImage);
            } else {
                fprintf(f, "  [%d] 0x%p\n", frameNum, (void*)addr);
            }
        }
    }
}

} // namespace Logging
