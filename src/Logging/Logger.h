/**
 * Logger.h — Structured Logging System
 *
 * Provides leveled logging with multiple output sinks:
 *   - FileSink   — timestamped log files with rotation
 *   - GuiSink    — lock-free ring buffer for GUI consumption
 *   - DebugSink  — OutputDebugStringA for IDE integration
 *
 * Thread safety: CRITICAL_SECTION on sink dispatch.
 * Ring buffer uses atomic index for lock-free reads from GUI thread.
 *
 * FOR EDUCATIONAL DEMONSTRATION ONLY
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <array>
#include <atomic>
#include <cstdarg>
#include <windows.h>

namespace Logging {

// ---- Log Levels ----

enum class LogLevel {
    Trace,   // Extremely verbose — function entry/exit
    Debug,   // Development diagnostics
    Info,    // Normal operational messages
    Warn,    // Recoverable issues, degraded functionality
    Error,   // Operation failures, exceptions caught
    Fatal    // Unrecoverable — application must exit
};

const char* LogLevelToString(LogLevel level);

// ---- Log Entry ----

struct LogEntry {
    LogLevel level = LogLevel::Info;
    std::chrono::system_clock::time_point timestamp;
    std::string file;
    int line = 0;
    std::string function;
    std::string message;
};

// ---- Sink Interface ----

struct ILogSink {
    virtual ~ILogSink() = default;
    virtual void Write(const LogEntry& entry) = 0;
    virtual void Flush() {}
};

// ---- Logger Singleton ----

class Logger {
public:
    static Logger& GetInstance();

    /**
     * Initialize the logger. Creates the logs/ directory and sets up sinks
     * based on config. Call once at startup before any LOG_* macro.
     * @param logDir    Directory for log files (default: "logs")
     * @param minLevel  Minimum level to record (default: Debug in debug, Info in release)
     * @return true if initialization succeeded.
     */
    bool Initialize(const std::string& logDir = "logs",
                    LogLevel minLevel =
#ifdef _DEBUG
                        LogLevel::Debug
#else
                        LogLevel::Info
#endif
    );

    /**
     * Flush all sinks and close file handles. Call before application exit.
     */
    void Shutdown();

    /**
     * Set the minimum log level. Messages below this level are dropped.
     */
    void SetMinLevel(LogLevel level);
    LogLevel GetMinLevel() const;

    /**
     * Add a custom sink. Ownership transfers to Logger.
     */
    void AddSink(std::unique_ptr<ILogSink> sink);

    /**
     * Main log entry point — called by LOG_* macros.
     * Uses printf-style format specifiers (%s, %d, %lu, %f, %p, etc.).
     */
    void Log(LogLevel level, const char* file, int line, const char* func,
             const char* fmt, ...);

    /**
     * Get recent log entries from the ring buffer for GUI display.
     * @param maxCount  Maximum entries to return.
     * @return Snapshot of recent log entries (newest last).
     */
    std::vector<LogEntry> GetEntries(size_t maxCount = 200) const;

    /**
     * Direct-write for crash handler — bypasses locks and formatting.
     * Appends a raw line to the crash log file using only kernel32 APIs.
     */
    void EmergencyLog(const char* message);

    /**
     * Get recent log entries as a pre-formatted string for crash reports.
     */
    std::string GetRecentLogs(size_t count = 20) const;

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void EnsureInitialized();
    std::string FormatMessage(const char* fmt, va_list args);
    std::string FormatTimestamp(const std::chrono::system_clock::time_point& tp);

    // Ring buffer for GUI consumption (lock-free reads)
    static constexpr size_t RING_BUFFER_SIZE = 500;
    std::array<LogEntry, RING_BUFFER_SIZE> m_ringBuffer;
    std::atomic<size_t> m_ringWriteIndex{0};

    // Sinks
    std::vector<std::unique_ptr<ILogSink>> m_sinks;

    // State
    bool m_initialized = false;
    LogLevel m_minLevel = LogLevel::Debug;
    std::string m_logDir;
    CRITICAL_SECTION m_cs;
};

} // namespace Logging

// ---- Logging Macros ----

#define LOG_TRACE(fmt, ...) \
    ::Logging::Logger::GetInstance().Log(::Logging::LogLevel::Trace, \
        __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)

#define LOG_DEBUG(fmt, ...) \
    ::Logging::Logger::GetInstance().Log(::Logging::LogLevel::Debug, \
        __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)

#define LOG_INFO(fmt, ...) \
    ::Logging::Logger::GetInstance().Log(::Logging::LogLevel::Info, \
        __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)

#define LOG_WARN(fmt, ...) \
    ::Logging::Logger::GetInstance().Log(::Logging::LogLevel::Warn, \
        __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)

#define LOG_ERROR(fmt, ...) \
    ::Logging::Logger::GetInstance().Log(::Logging::LogLevel::Error, \
        __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)

#define LOG_FATAL(fmt, ...) \
    ::Logging::Logger::GetInstance().Log(::Logging::LogLevel::Fatal, \
        __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)
