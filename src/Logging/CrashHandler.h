/**
 * CrashHandler.h — Crash & Exception Capture
 *
 * Installs SEH, C++, and signal handlers at startup. On crash:
 *   1. Writes a human-readable crash log (crash_YYYY-MM-DD_HH-MM-SS.log)
 *   2. Writes a Win32 minidump (crash_YYYY-MM-DD_HH-MM-SS.dmp)
 *   3. Terminates the process
 *
 * The crash log includes exception info, register dump, stack trace with
 * symbol resolution via dbghelp.dll, and recent Logger entries.
 *
 * FOR EDUCATIONAL DEMONSTRATION ONLY
 */

#pragma once

#include <string>
#include <windows.h>

namespace Logging {

class CrashHandler {
public:
    static CrashHandler& GetInstance();

    /**
     * Install all crash handlers. Call once at startup before any work.
     * Loads dbghelp.dll dynamically for symbol resolution.
     * @param logDir  Directory for crash artifacts (default: "logs")
     * @return true if installation succeeded.
     */
    bool Install(const std::string& logDir = "logs");

    /**
     * Remove crash handlers. Call before clean shutdown.
     */
    void Uninstall();

    /**
     * Trigger a crash programmatically (for testing the handler).
     * Writes crash artifacts and terminates the process.
     */
    [[noreturn]] void TriggerCrash(const char* reason);

    /**
     * Check if crash handlers are currently installed.
     */
    bool IsInstalled() const;

private:
    CrashHandler() = default;
    ~CrashHandler();
    CrashHandler(const CrashHandler&) = delete;
    CrashHandler& operator=(const CrashHandler&) = delete;

    // Native handler callbacks
    static LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* ep);
    static void __cdecl OnTerminate();
    static void OnAbortSignal(int signum);

    // Core crash processing
    void OnCrash(const char* exceptionType, DWORD exceptionCode,
                 uintptr_t faultAddr, EXCEPTION_POINTERS* ep);

    // Artifact writers (kernel32-only, no CRT heap)
    void WriteCrashLog(const char* exceptionType, DWORD exceptionCode,
                       uintptr_t faultAddr, EXCEPTION_POINTERS* ep);
    void WriteMinidump(EXCEPTION_POINTERS* ep);

    // Stack walking
    void WriteStackTrace(HANDLE hProcess, HANDLE hThread,
                         CONTEXT* context, FILE* file);
    void WriteRegisterDump(CONTEXT* context, FILE* file);

    // Dynamic dbghelp function pointers
    using SymInitialize_t      = BOOL (WINAPI *)(HANDLE, PCSTR, BOOL);
    using SymCleanup_t         = BOOL (WINAPI *)(HANDLE);
    using SymFromAddr_t        = BOOL (WINAPI *)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
    using SymGetLineFromAddr64_t = BOOL (WINAPI *)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);
    using StackWalk64_t        = BOOL (WINAPI *)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID,
                                                 PREAD_PROCESS_MEMORY_ROUTINE64,
                                                 PFUNCTION_TABLE_ACCESS_ROUTINE64,
                                                 PGET_MODULE_BASE_ROUTINE64,
                                                 PTRANSLATE_ADDRESS_ROUTINE64);
    using MiniDumpWriteDump_t  = BOOL (WINAPI *)(HANDLE, DWORD, HANDLE,
                                                  MINIDUMP_TYPE,
                                                  PMINIDUMP_EXCEPTION_INFORMATION,
                                                  PMINIDUMP_USER_STREAM_INFORMATION,
                                                  PMINIDUMP_CALLBACK_INFORMATION);

    // State
    bool m_installed = false;
    std::string m_logDir;
    HMODULE m_dbghelpDll = nullptr;
    SymInitialize_t       m_pSymInitialize = nullptr;
    SymCleanup_t          m_pSymCleanup = nullptr;
    SymFromAddr_t         m_pSymFromAddr = nullptr;
    SymGetLineFromAddr64_t m_pSymGetLineFromAddr64 = nullptr;
    StackWalk64_t         m_pStackWalk64 = nullptr;
    MiniDumpWriteDump_t   m_pMiniDumpWriteDump = nullptr;

    // Previous handlers (for restore on Uninstall)
    LPTOP_LEVEL_EXCEPTION_FILTER m_prevSEHFilter = nullptr;
    std::terminate_handler m_prevTerminateHandler = nullptr;
};

} // namespace Logging
