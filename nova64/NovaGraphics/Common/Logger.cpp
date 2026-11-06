/**
 * @file Logger.cpp
 * @brief Video logger implementation
 *
 * Thread-safe logging with ANSI console coloring, rotating file output,
 * per-subsystem filtering, frame-tagged entries, and callback hooks.
 */

#include "Logger.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <algorithm>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#else
    #include <pthread.h>
    #include <unistd.h>
    #include <sys/syscall.h>
#endif

namespace Nova64::Video {

Logger g_logger;

// Diagnostics-only present index (declared in Logger.hpp).
std::atomic<long> g_presentIndex{0};

// ═══════════════════════════════════════════════════════════════════════════
//  Static Name/Color Tables
// ═══════════════════════════════════════════════════════════════════════════

const char* Logger::LevelName(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Fatal:   return "FATAL";
    case LogLevel::Error:   return "ERROR";
    case LogLevel::Warning: return "WARN ";
    case LogLevel::Info:    return "INFO ";
    case LogLevel::Debug:   return "DEBUG";
    case LogLevel::Trace:   return "TRACE";
    default:                return "?????";
    }
}

const char* Logger::LevelColor(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Fatal:   return "\033[1;31m"; // Bold Red
    case LogLevel::Error:   return "\033[31m";   // Red
    case LogLevel::Warning: return "\033[33m";   // Yellow
    case LogLevel::Info:    return "\033[32m";    // Green
    case LogLevel::Debug:   return "\033[36m";    // Cyan
    case LogLevel::Trace:   return "\033[37m";    // Gray
    default:                return "\033[0m";
    }
}

const char* Logger::SubsystemName(LogSubsystem sub) noexcept {
    switch (sub) {
    case LogSubsystem::Core:       return "Core      ";
    case LogSubsystem::RDP:        return "RDP       ";
    case LogSubsystem::Microcode:  return "Microcode ";
    case LogSubsystem::Renderer:   return "Renderer  ";
    case LogSubsystem::Shader:     return "Shader    ";
    case LogSubsystem::Texture:    return "Texture   ";
    case LogSubsystem::Window:     return "Window    ";
    case LogSubsystem::Math:       return "Math      ";
    case LogSubsystem::Compat:     return "Compat    ";
    case LogSubsystem::Debug:      return "Debug     ";
    case LogSubsystem::Capture:    return "Capture   ";
    case LogSubsystem::Validation: return "Validation";
    case LogSubsystem::Plugin:     return "Plugin    ";
    case LogSubsystem::Config:     return "Config    ";
    case LogSubsystem::Memory:     return "Memory    ";
    default:                       return "Unknown   ";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Lifecycle
// ═══════════════════════════════════════════════════════════════════════════

Logger::~Logger() {
    Shutdown();
}

bool Logger::Init(const LoggerConfig& config) {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_initialized) return true;

    m_config = config;
    m_startTime = std::chrono::steady_clock::now();

    // Diagnostics: NOVA64_LOG_LEVEL=0..6 overrides the global level so heavy
    // per-draw INFO spam can be silenced for fast headless capture runs.
    // (Off=0, Fatal=1, Error=2, Warning=3, Info=4, Debug=5, Trace=6)
    if (const char* lvl = std::getenv("NOVA64_LOG_LEVEL")) {
        int v = std::atoi(lvl);
        if (v >= 0 && v <= 6) m_config.globalLevel = static_cast<LogLevel>(v);
    }

    // Enable ANSI escape codes on Windows
#ifdef _WIN32
    if (m_config.colorConsole) {
        HANDLE hOut = GetStdHandle(STD_ERROR_HANDLE);
        if (hOut != INVALID_HANDLE_VALUE) {
            DWORD mode = 0;
            if (GetConsoleMode(hOut, &mode)) {
                SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
            }
        }
    }
#endif

    // Open log file
    if (m_config.fileOutput && !m_config.logFilePath.empty()) {
        m_file.open(m_config.logFilePath, std::ios::out | std::ios::trunc);
        if (!m_file.is_open()) {
            std::fprintf(stderr, "[NovaGraphics Logger] Failed to open log file: %s\n",
                         m_config.logFilePath.c_str());
            m_config.fileOutput = false;
        }
        m_fileBytes = 0;
    }

    m_initialized = true;
    return true;
}

void Logger::Shutdown() {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_initialized) return;

    if (m_file.is_open()) {
        m_file.flush();
        m_file.close();
    }
    m_initialized = false;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Configuration
// ═══════════════════════════════════════════════════════════════════════════

void Logger::SetConfig(const LoggerConfig& config) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_config = config;
}

void Logger::SetGlobalLevel(LogLevel level) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_config.globalLevel = level;
}

void Logger::SetSubsystemLevel(LogSubsystem sub, LogLevel level) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto idx = static_cast<size_t>(sub);
    if (idx < m_config.subsystemLevels.size()) {
        m_config.subsystemLevels[idx] = level;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Level Checks
// ═══════════════════════════════════════════════════════════════════════════

LogLevel Logger::GetEffectiveLevel(LogSubsystem sub) const noexcept {
    auto idx = static_cast<size_t>(sub);
    if (idx < m_config.subsystemLevels.size()) {
        LogLevel subLevel = m_config.subsystemLevels[idx];
        if (subLevel != LogLevel::Off) return subLevel;
    }
    return m_config.globalLevel;
}

bool Logger::IsEnabled(LogLevel level, LogSubsystem sub) const noexcept {
    if (!m_initialized) return false;
    if (level == LogLevel::Off) return false;
    return static_cast<uint8_t>(level) <= static_cast<uint8_t>(GetEffectiveLevel(sub));
}

// ═══════════════════════════════════════════════════════════════════════════
//  Core Logging
// ═══════════════════════════════════════════════════════════════════════════

void Logger::Log(LogLevel level, LogSubsystem sub, const char* file, int line,
                 const char* func, const char* fmt, ...) {
    if (!IsEnabled(level, sub)) return;

    // Format message via va_list
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    int written = std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (written < 0) return;
    if (written >= static_cast<int>(sizeof(buf)))
        buf[sizeof(buf) - 1] = '\0';

    // Build entry
    LogEntry entry;
    entry.level       = level;
    entry.subsystem   = sub;
    entry.message     = buf;
    entry.file        = file ? file : "";
    entry.line        = line;
    entry.function    = func ? func : "";
    entry.timestamp   = std::chrono::steady_clock::now();
    entry.frameNumber = m_frameNumber.load(std::memory_order_relaxed);

#if defined(_WIN32)
    entry.threadId = GetCurrentThreadId();
#elif defined(__APPLE__)
    {
        uint64_t tid = 0;   // syscall(SYS_gettid) is deprecated on macOS; this is the supported call
        pthread_threadid_np(nullptr, &tid);
        entry.threadId = static_cast<uint32_t>(tid);
    }
#else
    entry.threadId = static_cast<uint32_t>(syscall(SYS_gettid));
#endif

    // Update stats
    m_stats.totalMessages.fetch_add(1, std::memory_order_relaxed);
    switch (level) {
    case LogLevel::Fatal:   m_stats.fatalCount.fetch_add(1, std::memory_order_relaxed); break;
    case LogLevel::Error:   m_stats.errorCount.fetch_add(1, std::memory_order_relaxed); break;
    case LogLevel::Warning: m_stats.warningCount.fetch_add(1, std::memory_order_relaxed); break;
    case LogLevel::Info:    m_stats.infoCount.fetch_add(1, std::memory_order_relaxed); break;
    case LogLevel::Debug:   m_stats.debugCount.fetch_add(1, std::memory_order_relaxed); break;
    case LogLevel::Trace:   m_stats.traceCount.fetch_add(1, std::memory_order_relaxed); break;
    default: break;
    }

    WriteEntry(entry);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Output
// ═══════════════════════════════════════════════════════════════════════════

void Logger::WriteEntry(const LogEntry& entry) {
    std::lock_guard<std::mutex> lk(m_mutex);

    if (m_config.consoleOutput) WriteToConsole(entry);
    if (m_config.fileOutput && m_file.is_open()) WriteToFile(entry);
    if (m_callback) m_callback(entry);
}

void Logger::WriteToConsole(const LogEntry& entry) {
    std::string formatted;
    FormatEntry(formatted, entry, m_config.colorConsole);
    std::fputs(formatted.c_str(), stderr);
    std::fputc('\n', stderr);
}

void Logger::WriteToFile(const LogEntry& entry) {
    std::string formatted;
    FormatEntry(formatted, entry, false);
    m_file << formatted << '\n';
    m_fileBytes += formatted.size() + 1;
    m_stats.bytesWritten.fetch_add(formatted.size() + 1, std::memory_order_relaxed);

    if (m_config.maxFileSize > 0 && m_fileBytes >= m_config.maxFileSize) {
        RotateLog();
    }
}

void Logger::FormatEntry(std::string& out, const LogEntry& entry, bool color) const {
    std::ostringstream ss;

    if (m_config.includeTimestamp) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(entry.timestamp - m_startTime);
        auto totalMs = elapsed.count();
        auto sec = totalMs / 1000;
        auto ms  = totalMs % 1000;
        ss << '[' << std::setw(6) << sec << '.' << std::setfill('0')
           << std::setw(3) << ms << "] ";
        ss << std::setfill(' ');
    }

    if (m_config.includeFrame) {
        ss << 'F' << std::setw(6) << entry.frameNumber << ' ';
    }

    if (m_config.includeThread) {
        ss << 'T' << std::setw(5) << entry.threadId << ' ';
    }

    if (color) ss << LevelColor(entry.level);
    ss << LevelName(entry.level);
    if (color) ss << "\033[0m";
    ss << ' ';

    ss << '[' << SubsystemName(entry.subsystem) << "] ";
    ss << entry.message;

    if (m_config.includeFile && !entry.file.empty()) {
        auto pos = entry.file.find_last_of("/\\");
        std::string_view fname = (pos != std::string::npos)
                                 ? std::string_view(entry.file).substr(pos + 1)
                                 : entry.file;
        ss << " (" << fname << ':' << entry.line << ')';
    }

    out = ss.str();
}

// ═══════════════════════════════════════════════════════════════════════════
//  Log Rotation
// ═══════════════════════════════════════════════════════════════════════════

void Logger::RotateLog() {
    if (!m_file.is_open()) return;
    m_file.close();

    namespace fs = std::filesystem;

    for (int i = static_cast<int>(m_config.maxRotations) - 1; i >= 0; --i) {
        std::string oldPath = m_config.logFilePath;
        if (i > 0) oldPath += "." + std::to_string(i);

        std::string newPath = m_config.logFilePath + "." + std::to_string(i + 1);

        std::error_code ec;
        if (fs::exists(oldPath, ec)) {
            if (i + 1 >= static_cast<int>(m_config.maxRotations))
                fs::remove(oldPath, ec);
            else
                fs::rename(oldPath, newPath, ec);
        }
    }

    m_file.open(m_config.logFilePath, std::ios::out | std::ios::trunc);
    m_fileBytes = 0;
}

void Logger::Flush() {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_file.is_open()) m_file.flush();
    std::fflush(stderr);
}

} // namespace Nova64::Video
