/**
 * Logger.cpp — Structured Logging Implementation
 *
 * FOR EDUCATIONAL DEMONSTRATION ONLY
 */

#include "Logging/Logger.h"
#include <fstream>
#include <sstream>
#include <cstdio>
#include <iomanip>

// Windows headers for CreateDirectoryA, OutputDebugStringA
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace Logging {

// ============================================================
//  LogLevel to String
// ============================================================

const char* LogLevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "?????";
}

// ============================================================
//  FileSink — writes formatted log entries to a file with rotation
// ============================================================

class FileSink : public ILogSink {
public:
    FileSink(const std::string& logDir, size_t maxFileSize, size_t maxFiles)
        : m_logDir(logDir), m_maxFileSize(maxFileSize), m_maxFiles(maxFiles)
    {
        // Create log directory if it doesn't exist
        CreateDirectoryA(logDir.c_str(), nullptr);

        // Generate filename with current date
        m_baseName = logDir + "/universal_hub_";
        OpenNewFile();
    }

    void Write(const LogEntry& entry) override {
        if (!m_file.is_open()) return;

        // Format: [timestamp] [LEVEL] [file:line] [function] message
        char buf[32];
        auto tt = std::chrono::system_clock::to_time_t(entry.timestamp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            entry.timestamp.time_since_epoch()) % 1000;

        std::tm tm{};
        localtime_s(&tm, &tt);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);

        m_file << "[" << buf << "." << std::setfill('0') << std::setw(3)
               << ms.count() << "] "
               << "[" << LogLevelToString(entry.level) << "] "
               << "[" << entry.file << ":" << entry.line << "] "
               << "[" << entry.function << "] "
               << entry.message << "\n";

        // Flush on Fatal to ensure crash reports survive
        if (entry.level == LogLevel::Fatal) {
            m_file.flush();
        }

        // Check rotation (track actual bytes written)
        m_bytesWritten += entry.message.size() + 80; // ~80 chars for timestamp/level/location
        if (m_bytesWritten >= m_maxFileSize) {
            RotateFiles();
        }
    }

    void Flush() override {
        if (m_file.is_open()) {
            m_file.flush();
        }
    }

    void DirectWrite(const char* message) {
        if (!m_file.is_open()) return;
        m_file << message << "\n";
        m_file.flush();
    }

private:
    void OpenNewFile() {
        if (m_file.is_open()) {
            m_file.close();
        }

        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        localtime_s(&tm, &tt);

        char dateBuf[16];
        strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", &tm);

        m_currentFileName = m_baseName + dateBuf + ".log";
        m_file.open(m_currentFileName, std::ios::out | std::ios::app);
        m_bytesWritten = 0;
    }

    void RotateFiles() {
        m_file.close();

        // Rename existing rotated files: .4.log → .5.log, .3.log → .4.log, etc.
        for (size_t i = m_maxFiles; i >= 1; --i) {
            std::string oldName = m_currentFileName + "." + std::to_string(i);
            std::string newName = m_currentFileName + "." + std::to_string(i + 1);

            if (i == m_maxFiles) {
                // Delete oldest file
                DeleteFileA(oldName.c_str());
            } else {
                MoveFileA(oldName.c_str(), newName.c_str());
            }
        }

        // Rename current to .1
        MoveFileA(m_currentFileName.c_str(),
                  (m_currentFileName + ".1").c_str());

        // Open fresh file
        OpenNewFile();
    }

    std::string m_logDir;
    std::string m_baseName;
    std::string m_currentFileName;
    std::ofstream m_file;
    size_t m_maxFileSize;
    size_t m_maxFiles;
    size_t m_bytesWritten = 0;
};

// ============================================================
//  GuiSink — lock-free ring buffer for GUI consumption
// ============================================================

class GuiSink : public ILogSink {
public:
    explicit GuiSink(Logger* logger) : m_logger(logger) {}

    void Write(const LogEntry& entry) override {
        // Lock-free: atomic write index, no reader blocks
        size_t idx = m_logger->m_ringWriteIndex.fetch_add(1, std::memory_order_relaxed);
        m_logger->m_ringBuffer[idx % Logger::RING_BUFFER_SIZE] = entry;
    }

private:
    Logger* m_logger;
};

// ============================================================
//  DebugSink — OutputDebugStringA for Visual Studio / DebugView
// ============================================================

class DebugSink : public ILogSink {
public:
    void Write(const LogEntry& entry) override {
        // Format with minimal prefix for IDE output window
        char buf[2048];
        snprintf(buf, sizeof(buf), "[%s] [%s:%d] %s\n",
                 LogLevelToString(entry.level),
                 entry.file.c_str(),
                 entry.line,
                 entry.message.c_str());
        OutputDebugStringA(buf);
    }
};

// ============================================================
//  Logger Implementation
// ============================================================

Logger& Logger::GetInstance() {
    static Logger instance;
    return instance;
}

Logger::~Logger() {
    Shutdown();
}

bool Logger::Initialize(const std::string& logDir, LogLevel minLevel) {
    if (m_initialized) return true;

    m_logDir = logDir;
    m_minLevel = minLevel;

    InitializeCriticalSection(&m_cs);

    // Create sinks
    // FileSink: 10MB max per file, keep 5 rotated files
    m_sinks.push_back(std::make_unique<FileSink>(logDir, 10 * 1024 * 1024, 5));

    // GuiSink: lock-free ring buffer for GUI
    m_sinks.push_back(std::make_unique<GuiSink>(this));

    // DebugSink: Windows debug output
    m_sinks.push_back(std::make_unique<DebugSink>());

    m_initialized = true;

    LOG_INFO("Logger initialized — level=%s, dir=%s/",
             LogLevelToString(minLevel), logDir.c_str());
    return true;
}

void Logger::Shutdown() {
    if (!m_initialized) return;

    LOG_INFO("Logger shutting down");

    EnterCriticalSection(&m_cs);
    for (auto& sink : m_sinks) {
        sink->Flush();
    }
    m_sinks.clear();
    LeaveCriticalSection(&m_cs);

    DeleteCriticalSection(&m_cs);
    m_initialized = false;
}

void Logger::SetMinLevel(LogLevel level) {
    m_minLevel = level;
}

LogLevel Logger::GetMinLevel() const {
    return m_minLevel;
}

void Logger::AddSink(std::unique_ptr<ILogSink> sink) {
    EnterCriticalSection(&m_cs);
    m_sinks.push_back(std::move(sink));
    LeaveCriticalSection(&m_cs);
}

// ---- Main Log Entry Point ----

void Logger::Log(LogLevel level, const char* file, int line,
                 const char* func, const char* fmt, ...) {
    if (!m_initialized || level < m_minLevel) return;

    // Build the entry
    LogEntry entry;
    entry.level = level;
    entry.timestamp = std::chrono::system_clock::now();
    entry.file = file;
    entry.line = line;
    entry.function = func;

    // Format message
    va_list args;
    va_start(args, fmt);
    entry.message = FormatMessage(fmt, args);
    va_end(args);

    // Dispatch to all sinks under lock
    EnterCriticalSection(&m_cs);
    for (auto& sink : m_sinks) {
        sink->Write(entry);
    }
    LeaveCriticalSection(&m_cs);
}

// ---- GUI Ring Buffer Read ----

std::vector<LogEntry> Logger::GetEntries(size_t maxCount) const {
    std::vector<LogEntry> result;
    size_t writeIdx = m_ringWriteIndex.load(std::memory_order_relaxed);

    // Determine how many entries are available
    size_t count = (std::min)(writeIdx, RING_BUFFER_SIZE);
    count = (std::min)(count, maxCount);

    // Calculate the starting index and collect entries
    size_t startIdx = (writeIdx >= count) ? (writeIdx - count) : 0;

    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        size_t idx = (startIdx + i) % RING_BUFFER_SIZE;
        const auto& entry = m_ringBuffer[idx];
        if (!entry.message.empty()) {
            result.push_back(entry);
        }
    }

    return result;
}

// ---- Emergency Log (Crash Handler Interface) ----

void Logger::EmergencyLog(const char* message) {
    // Bypass the lock — we're in a crash handler, the process is dying.
    // Walk sinks manually — only FileSink handles direct writes.
    for (auto& sink : m_sinks) {
        auto* fileSink = dynamic_cast<FileSink*>(sink.get());
        if (fileSink) {
            fileSink->DirectWrite(message);
        }
    }
}

// ---- Recent Logs for Crash Report ----

std::string Logger::GetRecentLogs(size_t count) const {
    auto entries = GetEntries(count);
    std::ostringstream oss;

    for (const auto& entry : entries) {
        auto tt = std::chrono::system_clock::to_time_t(entry.timestamp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            entry.timestamp.time_since_epoch()) % 1000;

        char buf[32];
        std::tm tm{};
        localtime_s(&tm, &tt);
        strftime(buf, sizeof(buf), "%H:%M:%S", &tm);

        oss << "[" << buf << "." << std::setfill('0') << std::setw(3)
            << ms.count() << "] "
            << "[" << LogLevelToString(entry.level) << "] "
            << "[" << entry.file << ":" << entry.line << "] "
            << entry.message << "\n";
    }

    return oss.str();
}

// ---- Message Formatting (printf-style) ----

std::string Logger::FormatMessage(const char* fmt, va_list args) {
    // Use vsnprintf for standard C format specifiers (%s, %d, %lu, %f, etc.)
    // First call to get required buffer size, then format into buffer.
    va_list argsCopy;
    va_copy(argsCopy, args);
    int needed = vsnprintf(nullptr, 0, fmt, argsCopy);
    va_end(argsCopy);

    if (needed <= 0) {
        return std::string(fmt); // Return format string as-is on error
    }

    std::string result(needed + 1, '\0');
    vsnprintf(&result[0], result.size(), fmt, args);
    result.resize(needed); // Remove null terminator
    return result;
}

// ---- Helpers ----

void Logger::EnsureInitialized() {
    if (!m_initialized) {
        // Lazy-init with defaults if Initialize wasn't called
        Initialize();
    }
}

} // namespace Logging
