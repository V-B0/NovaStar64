/**
 * @file trace.hpp
 * @brief Trace manager with severity and module filtering and pluggable sinks (file, console,
 *        memory), plus the CLog file logger.
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <memory>
#include <vector>
#include <array>
#include <format>
#include <chrono>
#include <mutex>
#include <atomic>
#include <fstream>
#include <optional>
#include <functional>
#include <cstdarg>
#include <cstdio>

/// Line-oriented file logger: Open() truncates the file and each Log() call
/// (printf-style, at most 2047 characters) appends one line.
class CLog {
public:
    CLog() = default;
    ~CLog() { Close(); }

    CLog(const CLog&) = delete;
    CLog& operator=(const CLog&) = delete;

    bool Open(const char* path) {
        Close();
        m_Stream.open(path, std::ios::out | std::ios::trunc);
        if (m_Stream.is_open()) {
            m_Path = path;
            return true;
        }
        return false;
    }

    void Close() {
        if (m_Stream.is_open()) {
            m_Stream.close();
        }
    }

    bool IsOpen() const { return m_Stream.is_open(); }
    const std::string& GetPath() const { return m_Path; }

    void Log(const char* fmt, ...) {
        if (!m_Stream.is_open()) return;
        char buf[2048];
        std::va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        m_Stream << buf << '\n';
    }

    void Flush() {
        if (m_Stream.is_open()) m_Stream.flush();
    }

private:
    std::ofstream m_Stream;
    std::string m_Path;
};

namespace Trace {

// --- C++17-compatible source location ---
struct SourceLocation {
    const char* m_file;
    int m_line;
    const char* m_function;

    constexpr SourceLocation(const char* file = __builtin_FILE(), int line = __builtin_LINE(),
                             const char* function = __builtin_FUNCTION()) noexcept :
        m_file(file), m_line(line), m_function(function) {}

    [[nodiscard]] constexpr const char* file_name() const noexcept { return m_file; }
    [[nodiscard]] constexpr int line() const noexcept { return m_line; }
    [[nodiscard]] constexpr const char* function_name() const noexcept { return m_function; }
};

// --- Severity levels ---
enum class Severity : uint8_t {
    None = 0,
    Error = 1,
    Warning = 2,
    Notice = 3,
    Info = 4,
    Debug = 5,
    Verbose = 6,

    // Legacy names for the severities above
    TraceEmmitError = Error,
    TraceEmmitWarning = Warning,
    TraceEmmitNotice = Notice,
    TraceEmmitInfo = Info,
    TraceEmmitDebug = Debug,
    TraceEmmitVerbose = Verbose
};

// Indexed by Severity
constexpr std::array<std::string_view, 7> SeverityNames = {"NONE", "ERROR", "WARNING", "NOTICE",
                                                           "INFO", "DEBUG", "VERBOSE"};

// --- Module management ---
using ModuleID = uint32_t;

// Default module IDs (can be extended by applications)
namespace Module {
constexpr ModuleID None = 0;
constexpr ModuleID Core = 1;
constexpr ModuleID Video = 2;
constexpr ModuleID Audio = 3;
constexpr ModuleID Input = 4;
constexpr ModuleID RSP = 5;
constexpr ModuleID RDP = 6;
constexpr ModuleID CPU = 7;
constexpr ModuleID Plugin = 8;
constexpr ModuleID Config = 9;
constexpr ModuleID Debug = 10;
constexpr ModuleID Network = 11;
constexpr ModuleID Max = 64; // Size of the per-module tables; valid IDs are below this
} // namespace Module

// --- Log entry ---
struct LogEntry {
    std::chrono::system_clock::time_point timestamp;
    ModuleID module;
    Severity severity;
    std::string_view file;
    int line;
    std::string_view function;
    std::string message;

    [[nodiscard]] std::string Format(bool includeTimestamp = true) const;
};

// --- Sink interface ---
class ITraceSink {
public:
    virtual ~ITraceSink() = default;

    virtual void Write(const LogEntry& entry) = 0;
    virtual void Flush() = 0;
    [[nodiscard]] virtual Severity GetMinSeverity() const noexcept = 0;
    virtual void SetMinSeverity(Severity severity) noexcept = 0;
};

// --- File sink ---

// --- Console sink ---

// --- Memory sink (ring buffer) ---

// --- Trace manager ---
class TraceManager {
public:
    static TraceManager& Instance();

    // Management
    void AddSink(std::unique_ptr<ITraceSink> sink);
    void RemoveSink(ITraceSink* sink);
    void RemoveAllSinks();

    // Configuration
    void SetModuleSeverity(ModuleID module, Severity severity);

    void SetModuleName(ModuleID module, std::string_view name);
    [[nodiscard]] std::string_view GetModuleName(ModuleID module) const;

    // Logging
    void Log(ModuleID module, Severity severity, std::string_view message,
             const SourceLocation& location = SourceLocation{});

    template <typename... Args>
    void Log(ModuleID module, Severity severity, std::string_view format, Args&&... args,
             const SourceLocation& location = SourceLocation{}) {
        if (ShouldLog(module, severity)) {
            Log(module, severity, std::vformat(format, std::make_format_args(std::forward<Args>(args)...)), location);
        }
    }

    void FlushAll();

    // Query
    [[nodiscard]] bool ShouldLog(ModuleID module, Severity severity) const noexcept;

    // Statistics
    struct Statistics {
        size_t totalEntries = 0;
        std::array<size_t, 7> severityCounts{};
        std::array<size_t, Module::Max> moduleCounts{};
    };

private:
    TraceManager();
    ~TraceManager();

    mutable std::mutex m_Mutex;
    std::vector<std::unique_ptr<ITraceSink>> m_Sinks;
    std::array<std::atomic<Severity>, Module::Max> m_ModuleSeverities;
    std::array<std::string, Module::Max> m_ModuleNames;
    Statistics m_Stats;

    void InitializeDefaultModuleNames();
    [[nodiscard]] LogEntry CreateEntry(ModuleID module, Severity severity, std::string_view message,
                                       const SourceLocation& location) const;
};

// --- Convenience macros ---
#define TRACE_LOG(module, severity, ...)                                                                               \
    do {                                                                                                               \
        if (Trace::TraceManager::Instance().ShouldLog((module), (severity))) {                                         \
            Trace::TraceManager::Instance().Log((module), (severity), __VA_ARGS__);                                    \
        }                                                                                                              \
    } while (0)

// Convenience macros for specific severities
#define TRACE_ERROR(module, ...)   TRACE_LOG((module), Trace::Severity::Error, __VA_ARGS__)
#define TRACE_WARN(module, ...)    TRACE_LOG((module), Trace::Severity::Warning, __VA_ARGS__)
#define TRACE_INFO(module, ...)    TRACE_LOG((module), Trace::Severity::Info, __VA_ARGS__)
#define TRACE_DEBUG(module, ...)   TRACE_LOG((module), Trace::Severity::Debug, __VA_ARGS__)
#define TRACE_VERBOSE(module, ...) TRACE_LOG((module), Trace::Severity::Verbose, __VA_ARGS__)

// Conditional logging macros
#define TRACE_IF(condition, module, severity, ...)                                                                     \
    do {                                                                                                               \
        if ((condition) && Trace::TraceManager::Instance().ShouldLog((module), (severity))) {                          \
            Trace::TraceManager::Instance().Log((module), (severity), __VA_ARGS__);                                    \
        }                                                                                                              \
    } while (0)

// Once-only logging
#define TRACE_ONCE(module, severity, ...)                                                                              \
    do {                                                                                                               \
        static bool TRACE_ONCE_flag = false;                                                                           \
        if (!TRACE_ONCE_flag && Trace::TraceManager::Instance().ShouldLog((module), (severity))) {                     \
            TRACE_ONCE_flag = true;                                                                                    \
            Trace::TraceManager::Instance().Log((module), (severity), __VA_ARGS__);                                    \
        }                                                                                                              \
    } while (0)

// --- Legacy API ---
inline void WriteTraceFull(Trace::ModuleID module, Trace::Severity severity, const char* file, int line,
                           const char* function, const char* format, ...) {
    if (Trace::TraceManager::Instance().ShouldLog(module, severity)) {
        std::va_list args;
        va_start(args, format);
        char buffer[4096];
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);

        Trace::SourceLocation location(file, line, function);

        Trace::TraceManager::Instance().Log(module, severity, buffer, location);
    }
}

inline void TraceFlushLog() { Trace::TraceManager::Instance().FlushAll(); }

inline void TraceSetMaxModule(Trace::ModuleID maxModule, Trace::Severity defaultSeverity) {
    for (Trace::ModuleID i = 0; i < maxModule && i < Trace::Module::Max; ++i) {
        Trace::TraceManager::Instance().SetModuleSeverity(i, defaultSeverity);
    }
}

// Legacy printf-style macro
#ifdef _WIN32
#define WriteTrace(module, severity, format, ...)                                                                      \
    do {                                                                                                               \
        if (Trace::TraceManager::Instance().ShouldLog((module), (severity))) {                                         \
            WriteTraceFull((module), (severity), __FILE__, __LINE__, __FUNCTION__, (format), ##__VA_ARGS__);           \
        }                                                                                                              \
    } while (0)
#else
#define WriteTrace(module, severity, format, ...)                                                                      \
    do {                                                                                                               \
        if (Trace::TraceManager::Instance().ShouldLog((module), (severity))) {                                         \
            WriteTraceFull((module), (severity), __FILE__, __LINE__, __PRETTY_FUNCTION__, (format), ##__VA_ARGS__);    \
        }                                                                                                              \
    } while (0)
#endif

} // namespace Trace

// --- Global convenience functions ---
// Free-function wrappers around TraceManager, matching the original trace API.
inline Trace::ITraceSink* TraceAddModule(std::unique_ptr<Trace::ITraceSink> sink) {
    auto* ptr = sink.get();
    Trace::TraceManager::Instance().AddSink(std::move(sink));
    return ptr;
}

inline void TraceRemoveModule(Trace::ITraceSink* sink) { Trace::TraceManager::Instance().RemoveSink(sink); }

inline const char* TraceSeverity(Trace::Severity severity) {
    if (static_cast<size_t>(severity) < Trace::SeverityNames.size()) {
        return Trace::SeverityNames[static_cast<size_t>(severity)].data();
    }
    return "UNKNOWN";
}

inline const char* TraceModule(Trace::ModuleID module) {
    return Trace::TraceManager::Instance().GetModuleName(module).data();
}

inline void TraceSetModuleName(Trace::ModuleID module, const char* name) {
    Trace::TraceManager::Instance().SetModuleName(module, name);
}

inline void CloseTrace() { Trace::TraceManager::Instance().RemoveAllSinks(); }
