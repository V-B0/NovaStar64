/**
 * @file Logger.hpp
 * @brief Video-specific structured logging system
 *
 * Provides per-subsystem filtering (RDP, Shader, Texture, Renderer, etc.),
 * frame-tagged log entries, ANSI-colored console output, rotating file output,
 * and optional callback hooks. Integrates with the core Trace system but adds
 * video-specific subsystem granularity.
 */

#pragma once

#include <cstdint>
#include <string>
#include <array>
#include <atomic>
#include <mutex>
#include <fstream>
#include <chrono>
#include <functional>
#include <format>

namespace Nova64::Video {

// Diagnostics-only: monotonic swapchain present index, incremented once per
// presented frame by the VK backend. Lets the triangle dump (in GBIHandler)
// and the headless screenshot capture (in VKBackend) key off the SAME frame
// number, so a dumped triangle set can be correlated to a captured image.
extern std::atomic<long> g_presentIndex;

// ═══════════════════════════════════════════════════════════════════════════
//  Log Levels & Subsystems
// ═══════════════════════════════════════════════════════════════════════════

enum class LogLevel : uint8_t {
    Off      = 0,
    Fatal    = 1,
    Error    = 2,
    Warning  = 3,
    Info     = 4,
    Debug    = 5,
    Trace    = 6
};

enum class LogSubsystem : uint8_t {
    Core        = 0,
    RDP         = 1,
    Microcode   = 2,
    Renderer    = 3,
    Shader      = 4,
    Texture     = 5,
    Window      = 6,
    Math        = 7,
    Compat      = 8,
    Debug       = 9,
    Capture     = 10,
    Validation  = 11,
    Plugin      = 12,
    Config      = 13,
    Memory      = 14,
    Count       = 15
};

// ═══════════════════════════════════════════════════════════════════════════
//  Log Entry
// ═══════════════════════════════════════════════════════════════════════════

struct LogEntry {
    LogLevel        level       = LogLevel::Info;
    LogSubsystem    subsystem   = LogSubsystem::Core;
    std::string     message;
    std::string     file;
    int             line        = 0;
    std::string     function;
    uint32_t        frameNumber = 0;
    uint32_t        threadId    = 0;
    std::chrono::steady_clock::time_point timestamp;
};

// ═══════════════════════════════════════════════════════════════════════════
//  Logger Configuration
// ═══════════════════════════════════════════════════════════════════════════

struct LoggerConfig {
    LogLevel  globalLevel = LogLevel::Info;

    // Per-subsystem overrides (Off = use global)
    std::array<LogLevel, static_cast<size_t>(LogSubsystem::Count)> subsystemLevels{};

    // Output targets
    bool consoleOutput   = true;
    bool fileOutput      = false;
    std::string logFilePath;

    // Formatting
    bool colorConsole    = true;
    bool includeTimestamp = true;
    bool includeFrame    = true;
    bool includeThread   = false;
    bool includeFile     = true;

    // File rotation
    size_t maxFileSize   = 10 * 1024 * 1024;  // 10 MB
    size_t maxRotations  = 3;

    LoggerConfig() { subsystemLevels.fill(LogLevel::Off); }
};

// ═══════════════════════════════════════════════════════════════════════════
//  Logger Statistics
// ═══════════════════════════════════════════════════════════════════════════

struct LoggerStats {
    std::atomic<uint64_t> totalMessages{0};
    std::atomic<uint64_t> fatalCount{0};
    std::atomic<uint64_t> errorCount{0};
    std::atomic<uint64_t> warningCount{0};
    std::atomic<uint64_t> infoCount{0};
    std::atomic<uint64_t> debugCount{0};
    std::atomic<uint64_t> traceCount{0};
    std::atomic<uint64_t> droppedMessages{0};
    std::atomic<uint64_t> bytesWritten{0};
};

// ═══════════════════════════════════════════════════════════════════════════
//  Logger Class
// ═══════════════════════════════════════════════════════════════════════════

class Logger {
public:
    using Callback = std::function<void(const LogEntry&)>;

    Logger() = default;
    ~Logger();

    // Lifecycle
    bool Init(const LoggerConfig& config);
    void Shutdown();

    // Configuration
    void SetConfig(const LoggerConfig& config);
    void SetGlobalLevel(LogLevel level);
    void SetSubsystemLevel(LogSubsystem sub, LogLevel level);

    // Level checking
    bool IsEnabled(LogLevel level, LogSubsystem sub) const noexcept;
    LogLevel GetEffectiveLevel(LogSubsystem sub) const noexcept;

    // Core logging
    void Log(LogLevel level, LogSubsystem sub, const char* file, int line,
             const char* func, const char* fmt, ...);

    // Frame tracking
    void SetFrameNumber(uint32_t frame) { m_frameNumber.store(frame, std::memory_order_relaxed); }
    uint32_t GetFrameNumber() const { return m_frameNumber.load(std::memory_order_relaxed); }

    // Flush
    void Flush();

    // Statistics
    const LoggerStats& GetStats() const { return m_stats; }

    // Callback
    void SetCallback(Callback cb) { std::lock_guard<std::mutex> lk(m_mutex); m_callback = std::move(cb); }

    // Name helpers
    static const char* LevelName(LogLevel level) noexcept;
    static const char* LevelColor(LogLevel level) noexcept;
    static const char* SubsystemName(LogSubsystem sub) noexcept;

    // ── Static convenience methods ──
    // Accept format string + args and forward to global logger.
    template<typename... Args>
    static void Info(const char* fmt, Args&&...);
    template<typename... Args>
    static void Debug(const char* fmt, Args&&...);
    template<typename... Args>
    static void Warn(const char* fmt, Args&&...);
    template<typename... Args>
    static void Error(const char* fmt, Args&&...);

private:
    void WriteEntry(const LogEntry& entry);
    void WriteToConsole(const LogEntry& entry);
    void WriteToFile(const LogEntry& entry);
    void FormatEntry(std::string& out, const LogEntry& entry, bool color) const;
    void RotateLog();

    LoggerConfig  m_config;
    LoggerStats   m_stats;
    std::mutex    m_mutex;
    std::ofstream m_file;
    size_t        m_fileBytes = 0;
    bool          m_initialized = false;
    Callback      m_callback;

    std::atomic<uint32_t> m_frameNumber{0};
    std::chrono::steady_clock::time_point m_startTime;
};

/// Global video logger
extern Logger g_logger;

// ── Static method implementations (g_logger must be declared first) ──
template<typename... Args>
void Logger::Info(const char* fmt, Args&&... args) {
    if constexpr (sizeof...(Args) == 0) {
        g_logger.Log(LogLevel::Info, LogSubsystem::Core, "", 0, "", "%s", fmt);
    } else {
        std::string msg = std::vformat(fmt, std::make_format_args(args...));
        g_logger.Log(LogLevel::Info, LogSubsystem::Core, "", 0, "", "%s", msg.c_str());
    }
}
template<typename... Args>
void Logger::Debug(const char* fmt, Args&&... args) {
    if constexpr (sizeof...(Args) == 0) {
        g_logger.Log(LogLevel::Debug, LogSubsystem::Core, "", 0, "", "%s", fmt);
    } else {
        std::string msg = std::vformat(fmt, std::make_format_args(args...));
        g_logger.Log(LogLevel::Debug, LogSubsystem::Core, "", 0, "", "%s", msg.c_str());
    }
}
template<typename... Args>
void Logger::Warn(const char* fmt, Args&&... args) {
    if constexpr (sizeof...(Args) == 0) {
        g_logger.Log(LogLevel::Warning, LogSubsystem::Core, "", 0, "", "%s", fmt);
    } else {
        std::string msg = std::vformat(fmt, std::make_format_args(args...));
        g_logger.Log(LogLevel::Warning, LogSubsystem::Core, "", 0, "", "%s", msg.c_str());
    }
}
template<typename... Args>
void Logger::Error(const char* fmt, Args&&... args) {
    if constexpr (sizeof...(Args) == 0) {
        g_logger.Log(LogLevel::Error, LogSubsystem::Core, "", 0, "", "%s", fmt);
    } else {
        std::string msg = std::vformat(fmt, std::make_format_args(args...));
        g_logger.Log(LogLevel::Error, LogSubsystem::Core, "", 0, "", "%s", msg.c_str());
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Type-safe logging via std::format  (replaces printf-style NOVA_LOG)
// ═══════════════════════════════════════════════════════════════════════════

/// Template wrapper: formats with std::vformat ({}-style) then forwards to
/// the low-level Log() method.  Zero-arg calls pass the format string as-is.
template<typename... Args>
inline void LogFormatted(LogLevel level, LogSubsystem sub,
                         const char* file, int line, const char* func,
                         std::string_view fmt, Args&&... args) {
    if (!g_logger.IsEnabled(level, sub)) return;
    if constexpr (sizeof...(Args) == 0) {
        g_logger.Log(level, sub, file, line, func, "%s", fmt.data());
    } else {
        std::string msg = std::vformat(fmt, std::make_format_args(args...));
        g_logger.Log(level, sub, file, line, func, "%s", msg.c_str());
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Convenience Macros  (now route through LogFormatted — use {} placeholders)
// ═══════════════════════════════════════════════════════════════════════════

#define NOVA_LOG(level, sub, fmt, ...)                                          \
    do {                                                                        \
        if (::Nova64::Video::g_logger.IsEnabled(level, sub))                    \
            ::Nova64::Video::LogFormatted(level, sub, __FILE__, __LINE__,       \
                                          __FUNCTION__, fmt, ##__VA_ARGS__);    \
    } while(0)

#define NOVA_FATAL(sub, fmt, ...)  NOVA_LOG(::Nova64::Video::LogLevel::Fatal,   sub, fmt, ##__VA_ARGS__)
#define NOVA_ERROR(sub, fmt, ...)  NOVA_LOG(::Nova64::Video::LogLevel::Error,   sub, fmt, ##__VA_ARGS__)
#define NOVA_WARN(sub, fmt, ...)   NOVA_LOG(::Nova64::Video::LogLevel::Warning, sub, fmt, ##__VA_ARGS__)
#define NOVA_INFO(sub, fmt, ...)   NOVA_LOG(::Nova64::Video::LogLevel::Info,    sub, fmt, ##__VA_ARGS__)
#define NOVA_DEBUG(sub, fmt, ...)  NOVA_LOG(::Nova64::Video::LogLevel::Debug,   sub, fmt, ##__VA_ARGS__)
#define NOVA_TRACE(sub, fmt, ...)  NOVA_LOG(::Nova64::Video::LogLevel::Trace,   sub, fmt, ##__VA_ARGS__)

// Subsystem-specific shortcuts
#define LOG_RDP(fmt, ...)        NOVA_INFO(::Nova64::Video::LogSubsystem::RDP,       fmt, ##__VA_ARGS__)
#define LOG_SHADER(fmt, ...)     NOVA_INFO(::Nova64::Video::LogSubsystem::Shader,    fmt, ##__VA_ARGS__)
#define LOG_TEX(fmt, ...)        NOVA_INFO(::Nova64::Video::LogSubsystem::Texture,   fmt, ##__VA_ARGS__)
#define LOG_RENDERER(fmt, ...)   NOVA_INFO(::Nova64::Video::LogSubsystem::Renderer,  fmt, ##__VA_ARGS__)
#define LOG_PLUGIN(fmt, ...)     NOVA_INFO(::Nova64::Video::LogSubsystem::Plugin,    fmt, ##__VA_ARGS__)
#define LOG_COMPAT(fmt, ...)     NOVA_INFO(::Nova64::Video::LogSubsystem::Compat,    fmt, ##__VA_ARGS__)
#define LOG_UCODE(fmt, ...)      NOVA_INFO(::Nova64::Video::LogSubsystem::Microcode, fmt, ##__VA_ARGS__)

// ── Generic level macros (no subsystem arg) ──
#define LOG_INFO(fmt, ...)    NOVA_INFO(::Nova64::Video::LogSubsystem::Core,  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)   NOVA_ERROR(::Nova64::Video::LogSubsystem::Core, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...)   NOVA_DEBUG(::Nova64::Video::LogSubsystem::Core, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)    NOVA_WARN(::Nova64::Video::LogSubsystem::Core,  fmt, ##__VA_ARGS__)

// ── NOVA_LOG_* aliases ──
#define NOVA_LOG_FATAL(fmt, ...)  NOVA_FATAL(::Nova64::Video::LogSubsystem::Core, fmt, ##__VA_ARGS__)
#define NOVA_LOG_ERROR(fmt, ...)  NOVA_ERROR(::Nova64::Video::LogSubsystem::Core, fmt, ##__VA_ARGS__)
#define NOVA_LOG_WARN(fmt, ...)   NOVA_WARN(::Nova64::Video::LogSubsystem::Core,  fmt, ##__VA_ARGS__)
#define NOVA_LOG_INFO(fmt, ...)   NOVA_INFO(::Nova64::Video::LogSubsystem::Core,  fmt, ##__VA_ARGS__)
#define NOVA_LOG_DEBUG(fmt, ...)  NOVA_DEBUG(::Nova64::Video::LogSubsystem::Core, fmt, ##__VA_ARGS__)
#define NOVA_LOG_TRACE(fmt, ...)  NOVA_TRACE(::Nova64::Video::LogSubsystem::Core, fmt, ##__VA_ARGS__)

} // namespace Nova64::Video
