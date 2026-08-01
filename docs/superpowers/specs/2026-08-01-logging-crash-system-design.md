# Universal Hub — Logging & Crash System Design

**Date:** 2026-08-01
**Status:** Approved — ready for implementation
**Depends on:** Existing Universal Hub codebase (Phases 1-4)

---

## Context

The Universal Hub codebase currently has zero structured logging. All output goes through `std::cout`/`std::cerr` with ad-hoc `[ModuleName]` prefixes. There is no file persistence, no log levels, no crash handler, and no stack trace capture. A crash in the field leaves zero diagnostic artifacts.

This design adds a structured Logger with level filtering, multi-sink output (file, GUI, debug), and a CrashHandler that captures SEH exceptions, C++ exceptions, and signals — producing both a human-readable crash log and a Win32 minidump for Visual Studio/WinDbg analysis.

---

## Architecture

```
┌──────────────────────────────────────────────────────┐
│  main.cpp — Init / Shutdown                          │
│    Logger::Init(config)                              │
│    CrashHandler::Install()                           │
│    ... app runs ...                                  │
│    CrashHandler::Uninstall()                         │
│    Logger::Shutdown()                                │
└──────────────────┬───────────────────────────────────┘
                   │
    ┌──────────────┴──────────────┐
    │                             │
    ▼                             ▼
┌───────────┐              ┌──────────────┐
│  Logger   │              │ CrashHandler │
│ singleton │              │  singleton   │
├───────────┤              ├──────────────┤
│ LogLevel  │              │ Install()    │
│ Sinks[]   │◄─────────────│ Uninstall()  │
│ RingBuf   │ EmergencyLog │ OnCrash()    │
│ Macros    │              │ StackWalk64  │
└──┬──┬──┬──┘              │ MiniDump     │
   │  │  │                 └──────┬───────┘
   │  │  │                        │
   ▼  ▼  ▼                        ▼
FileSink GuiSink DebugSink   crash.log + crash.dmp
(.log)  (ring buf) (ODS)     (logs/ directory)
```

Two independent singletons with one coupling: CrashHandler calls `Logger::EmergencyLog()` to dump recent log history into the crash report. The Logger never calls CrashHandler.

---

## Component 1: Logger

### Log Levels

```cpp
enum class LogLevel {
    Trace,   // Extremely verbose — function entry/exit
    Debug,   // Development diagnostics
    Info,    // Normal operational messages
    Warn,    // Recoverable issues, degraded functionality
    Error,   // Operation failures, exceptions caught
    Fatal    // Unrecoverable — application must exit
};
```

Runtime minimum level configurable. Default: `Debug` in Debug builds, `Info` in Release.

### Log Entry Format

```
[2026-08-01 14:51:03.123] [ERROR] [Engine.cpp:65] [AttachToProcess] OpenProcess failed (error: 5)
```

Fields: timestamp (ms precision), level, source file:line, function, message.

### Sink Pattern

Logger holds `std::vector<std::unique_ptr<ILogSink>>`. Each log call iterates all sinks. Sinks filter by their own minimum level.

```cpp
struct ILogSink {
    virtual ~ILogSink() = default;
    virtual void Write(const LogEntry& entry) = 0;
    virtual void Flush() {}
};
```

Three sinks:

**FileSink** — `std::ofstream` to `logs/universal_hub_YYYY-MM-DD.log`. Opens on first write, flushes on every Fatal. Rotation: max 5 files, 10MB each. When current file exceeds limit, renames `file.log` → `file.1.log`, `file.1.log` → `file.2.log`, etc., drops `file.5.log`.

**GuiSink** — lock-free ring buffer. Fixed array of 500 `LogEntry` slots with atomic write index. GUI reads via `Logger::GetEntries(count)` which returns a `std::vector<LogEntry>` snapshot by copying the ring buffer. The copy is cheap (500 entries ≈ ~50KB).

**DebugSink** — calls `OutputDebugStringA` with formatted entry. Appears in Visual Studio Output window and SysInternals DebugView.

### Thread Safety

`CRITICAL_SECTION` on the sink dispatch loop. The ring buffer uses `std::atomic<size_t>` write index — GUI reads are lock-free (readers may see slightly stale data during a concurrent write, which is acceptable for a log viewer).

### Logging Macros

```cpp
#define LOG_TRACE(fmt, ...) Logger::GetInstance().Log(LogLevel::Trace, __FILE__, __LINE__, __FUNCTION__, fmt, __VA_ARGS__)
#define LOG_DEBUG(fmt, ...) Logger::GetInstance().Log(LogLevel::Debug, __FILE__, __LINE__, __FUNCTION__, fmt, __VA_ARGS__)
#define LOG_INFO(fmt, ...)  Logger::GetInstance().Log(LogLevel::Info,  __FILE__, __LINE__, __FUNCTION__, fmt, __VA_ARGS__)
#define LOG_WARN(fmt, ...)  Logger::GetInstance().Log(LogLevel::Warn,  __FILE__, __LINE__, __FUNCTION__, fmt, __VA_ARGS__)
#define LOG_ERROR(fmt, ...) Logger::GetInstance().Log(LogLevel::Error, __FILE__, __LINE__, __FUNCTION__, fmt, __VA_ARGS__)
#define LOG_FATAL(fmt, ...) Logger::GetInstance().Log(LogLevel::Fatal, __FILE__, __LINE__, __FUNCTION__, fmt, __VA_ARGS__)
```

Use `{}` placeholder syntax. The `Log()` method does simple `snprintf`-style replacement by scanning for `{}` tokens — no external formatting dependency. Supports up to 8 arguments per call.

### Public API

```cpp
class Logger {
public:
    static Logger& GetInstance();

    bool Initialize(const std::string& configPath = "");
    void Shutdown();
    void SetMinLevel(LogLevel level);
    void AddSink(std::unique_ptr<ILogSink> sink);

    // Main log entry point — called by macros
    void Log(LogLevel level, const char* file, int line, const char* func,
             const char* fmt, ...);

    // GUI consumption — returns recent entries from ring buffer
    std::vector<LogEntry> GetEntries(size_t maxCount = 200) const;

    // Crash handler interface — bypasses locks, writes directly
    void EmergencyLog(const char* message);
    std::string GetRecentLogs(size_t count = 20) const;

private:
    // ...
};
```

---

## Component 2: CrashHandler

### Installation

`CrashHandler::Install()` called once at startup, before any other work:

1. `SetUnhandledExceptionFilter(OnUnhandledException)` — SEH exceptions (access violations, stack overflows, illegal instructions, divide by zero)
2. `std::set_terminate(OnTerminate)` — uncaught C++ exceptions
3. `signal(SIGABRT, OnSignal)` — `abort()` calls
4. `signal(SIGSEGV, OnSignal)` — segfaults (POSIX-emulated on Windows)

All four callbacks funnel into `OnCrash(EXCEPTION_POINTERS*)` with appropriate exception info.

### Crash Artifacts

Both written to `logs/` directory with timestamped filenames:

**`crash_YYYY-MM-DD_HH-MM-SS.log`** — human-readable:
```
=== UNIVERSAL HUB CRASH REPORT ===
Timestamp:   2026-08-01 14:51:03.456
Exception:   0xC0000005 — ACCESS_VIOLATION
Fault addr:  0x00007FF8A2B31000
Thread ID:   12345
Build:       UniversalHub v1.0.0 (Debug)

--- Stack Trace ---
  [0] UniversalHub.exe!Engine::AttachToProcess+0x42 (Engine.cpp:65)
  [1] UniversalHub.exe!main+0x1A3 (main.cpp:82)
  [2] kernel32.dll!BaseThreadInitThunk+0x14
  [3] ntdll.dll!RtlUserThreadStart+0x21

--- Register Dump ---
RAX=0x0000000000000000 RBX=0x0000000000000001
RCX=0x00007FF8A2B31000 RDX=0x0000000000000030
RIP=0x00007FF8A2B31000 RSP=0x0000000000B5F800
RBP=0x0000000000B5F880 ...

--- Recent Logs ---
[14:51:03.100] [INFO ] [Engine.cpp:74] Attached to PID 12345
[14:51:03.200] [DEBUG] [LuaBridge.cpp:175] LuaU VM initialized
[14:51:03.380] [ERROR] [Memory.h:42] ReadProcessMemory failed (error: 299)
```

**`crash_YYYY-MM-DD_HH-MM-SS.dmp`** — `MiniDumpWriteDump` with `MiniDumpWithFullMemory` flag. Open in Visual Studio: File → Open → Dump File → Debug with Managed/Managed Only/Native Only.

### Stack Walking

Uses `dbghelp.dll` via dynamic loading (`LoadLibraryA` + `GetProcAddress`):
- `SymInitialize` with `SYMOPT_LOAD_LINES | SYMOPT_UNDNAME`
- `StackWalk64` with `GetCurrentThread()` context from exception pointers
- `SymFromAddr` for symbol name resolution
- `SymGetLineFromAddr64` for source file + line number

Falls back gracefully: if PDB not found → shows `module+offset` format. If dbghelp.dll unavailable → stack frames show raw addresses only.

### Safety Rules

```
1. Static guard:  static bool s_inCrashHandler — if true on entry, call TerminateProcess immediately
2. No heap:       EmergencyLog uses CreateFileA + WriteFile (kernel32, no CRT dependency)
3. No locks:      Bypasses Logger's CRITICAL_SECTION entirely
4. Bounded work:  Stack depth capped at 32 frames, log dump capped at 20 entries
5. Clean exit:    After writing crash artifacts, calls TerminateProcess(GetCurrentProcess(), code)
```

### Public API

```cpp
class CrashHandler {
public:
    static CrashHandler& GetInstance();

    // Install all handlers. Call once at startup.
    bool Install();

    // Remove handlers. Call before clean shutdown.
    void Uninstall();

    // Programmatic crash trigger (for testing, or from LOG_FATAL)
    void TriggerCrash(const char* reason);

private:
    static LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* ep);
    static void __cdecl OnTerminate();
    static void OnSignal(int signum);
    void OnCrash(const char* exceptionType, DWORD exceptionCode,
                 uintptr_t faultAddr, EXCEPTION_POINTERS* ep);
};
```

---

## Component 3: Integration

### New Files

| File | Lines (est.) | Purpose |
|------|-------------|---------|
| `src/Logging/Logger.h` | ~120 | Logger class, LogLevel enum, LogEntry struct, ILogSink, macros |
| `src/Logging/Logger.cpp` | ~250 | FileSink, GuiSink, DebugSink, rotation, format, ring buffer |
| `src/Logging/CrashHandler.h` | ~40 | CrashHandler class declaration |
| `src/Logging/CrashHandler.cpp` | ~190 | SEH/terminate/signal handlers, StackWalk64, MiniDump, EmergencyLog |

### Modified Files

| File | Changes |
|------|---------|
| `src/main.cpp` | Include Logger + CrashHandler. Init at top, Shutdown at exit. Replace all `std::cerr`/`std::cout`. Add `LOG_FATAL` + `CrashHandler::TriggerCrash` for unrecoverable errors. |
| `src/Core/Engine.cpp` | Replace 5 `std::cerr`/`std::cout` → `LOG_ERROR`/`LOG_INFO` |
| `src/Core/Bootstrap.cpp` | Replace `std::cerr`/`std::cout` → `LOG_ERROR`/`LOG_INFO` |
| `src/Lua/LuaBridge.cpp` | `ReportError` → `LOG_ERROR`. Bridge exceptions → `LOG_WARN`. Init/shutdown → `LOG_INFO`. `lua_pcall` errors → `LOG_ERROR`. `on_frame` error → `LOG_ERROR`. Add `l_log` bridge function. Override Lua `print()` → `LOG_INFO`. |
| `src/GUI/GUI.h` | Remove `m_logLines`. Add `m_logSnapshot` cache for frame display. Remove `m_scrollToBottom`/`m_reclaimFocus` (move to local). |
| `src/GUI/GUI.cpp` | `RenderConsoleTab`/`RenderLogPanel` → read from `Logger::GetEntries()`. Remove all `m_logLines.push_back` calls. `ExecuteLuaInput` → `LOG_INFO`. `ProcessWidgetQueue` → log widget data via macros. |
| `src/CMakeLists.txt` | Add `Logging/Logger.cpp` + `Logging/CrashHandler.cpp` to `UniversalHub` sources. Link `dbghelp.lib`. |

### Config Addition

```json
"logging": {
    "level": "debug",
    "file_enabled": true,
    "max_file_size_mb": 10,
    "max_files": 5,
    "gui_buffer_size": 500,
    "debug_output": true
}
```

### Lua Bridge Addition

```lua
-- New: structured logging from Lua
log("info", "Module loaded successfully")
log("error", "Failed to read memory at 0x{:X}", address)

-- Existing print() redirected through Logger
print("Hello")  -- becomes LOG_INFO
```

---

## Build & Linkage

`dbghelp.lib` is a standard Windows SDK library — no additional FetchContent dependency. Added to `target_link_libraries` in `src/CMakeLists.txt`.

The `logs/` directory is created on first write by `FileSink` via `CreateDirectoryA`. Not tracked in git (added to `.gitignore` as `logs/`).

---

## Verification

1. Build with logging sources → compiles without errors
2. Launch UniversalHub → `logs/universal_hub_YYYY-MM-DD.log` created, contains init messages
3. Trigger a known error (attach to nonexistent PID) → log file shows `[ERROR]` entry with source location
4. Press INSERT → GUI Console tab shows log entries from ring buffer
5. Visual Studio Output window shows `[DEBUG]` entries during development
6. Call `CrashHandler::TriggerCrash("test")` from console → `crash_*.log` + `crash_*.dmp` written to `logs/`
7. Open `.dmp` in Visual Studio → call stack visible with symbol names
8. Lua `print("test")` → appears in log file, GUI console, and debug output
9. Lua `log("warn", "message")` → logged with WARN level
10. Close UniversalHub cleanly → `Logger::Shutdown()` flushes and closes log file
