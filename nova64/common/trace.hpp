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

// Forward declaration
class CLog;

namespace Trace {

// ================ C++17-compatible Source Location ================
struct SourceLocation {
    const char* m_file;
    int m_line;
    const char* m_function;

    constexpr SourceLocation(const char* file = __builtin_FILE(),
                             int line = __builtin_LINE(),
                             const char* function = __builtin_FUNCTION()) noexcept
        : m_file(file), m_line(line), m_function(function) {}

    [[nodiscard]] constexpr const char* file_name() const noexcept { return m_file; }
    [[nodiscard]] constexpr int line() const noexcept { return m_line; }
    [[nodiscard]] constexpr const char* function_name() const noexcept { return m_function; }
};

// ================ Severity Levels ================
enum class Severity : uint8_t {
    None      = 0,
    Error     = 1,
    Warning   = 2,
    Notice    = 3,
    Info      = 4,
    Debug     = 5,
    Verbose   = 6,
    
    // Aliases for backward compatibility
    TraceEmmitError    = Error,
    TraceEmmitWarning  = Warning,
    TraceEmmitNotice   = Notice,
    TraceEmmitInfo     = Info,
    TraceEmmitDebug    = Debug,
    TraceEmmitVerbose  = Verbose
};

// Severity level names
constexpr std::array<std::string_view, 7> SeverityNames = {
    "NONE", "ERROR", "WARNING", "NOTICE", "INFO", "DEBUG", "VERBOSE"
};

// ================ Module Management ================
using ModuleID = uint32_t;

// Default module IDs (can be extended by applications)
namespace Module {
    constexpr ModuleID None     = 0;
    constexpr ModuleID Core     = 1;
    constexpr ModuleID Video    = 2;
    constexpr ModuleID Audio    = 3;
    constexpr ModuleID Input    = 4;
    constexpr ModuleID RSP      = 5;
    constexpr ModuleID RDP      = 6;
    constexpr ModuleID CPU      = 7;
    constexpr ModuleID Plugin   = 8;
    constexpr ModuleID Config   = 9;
    constexpr ModuleID Debug    = 10;
    constexpr ModuleID Network  = 11;
    constexpr ModuleID Max      = 64;  // Maximum number of modules
}

// ================ Log Entry Structure ================
struct LogEntry {
    std::chrono::system_clock::time_point timestamp;
    ModuleID module;
    Severity severity;
    std::string_view file;
    int line;
    std::string_view function;
    std::string message;
    
    [[nodiscard]] std::string Format(bool includeTimestamp = true) const;
    [[nodiscard]] std::string FormatShort() const;
};

// ================ Base Trace Interface ================
class ITraceSink {
public:
    virtual ~ITraceSink() = default;
    
    virtual void Write(const LogEntry& entry) = 0;
    virtual void Flush() = 0;
    [[nodiscard]] virtual Severity GetMinSeverity() const noexcept = 0;
    virtual void SetMinSeverity(Severity severity) noexcept = 0;
};

// ================ File Trace Sink ================
class FileTraceSink final : public ITraceSink {
public:
    struct Options {
        bool flushImmediately = false;
        size_t maxFileSize = 5 * 1024 * 1024;  // 5MB default
        size_t maxArchiveFiles = 5;
        std::string archivePattern = "{name}.{index}.log";
    };
    
    explicit FileTraceSink(std::string_view filename, 
                          const Options& options = {});
    ~FileTraceSink() override;
    
    // Non-copyable, movable
    FileTraceSink(const FileTraceSink&) = delete;
    FileTraceSink& operator=(const FileTraceSink&) = delete;
    FileTraceSink(FileTraceSink&& other) noexcept;
    FileTraceSink& operator=(FileTraceSink&& other) noexcept;
    
    void Write(const LogEntry& entry) override;
    void Flush() override;
    
    [[nodiscard]] Severity GetMinSeverity() const noexcept override { 
        return m_MinSeverity.load(std::memory_order_relaxed); 
    }
    
    void SetMinSeverity(Severity severity) noexcept override {
        m_MinSeverity.store(severity, std::memory_order_relaxed);
    }
    
    [[nodiscard]] const std::string& GetFilename() const noexcept { 
        return m_Filename; 
    }
    
    bool RotateIfNeeded();
    
private:
    std::string m_Filename;
    Options m_Options;
    std::ofstream m_FileStream;
    std::atomic<Severity> m_MinSeverity{Severity::Info};
    std::mutex m_Mutex;
    
    void OpenFile();
    void RotateFile();
    [[nodiscard]] std::string GenerateArchiveName(size_t index) const;
};

// ================ Console Trace Sink ================
class ConsoleTraceSink final : public ITraceSink {
public:
    enum class OutputTarget {
        StdOut,
        StdErr,
        Both
    };
    
    struct Options {
        OutputTarget target = OutputTarget::StdOut;
        bool useColors = true;
        bool showTimestamp = true;
        bool showSeverity = true;
        bool showModule = true;
        bool showLocation = false;
    };
    
    explicit ConsoleTraceSink(const Options& options = {});
    
    void Write(const LogEntry& entry) override;
    void Flush() override;
    
    [[nodiscard]] Severity GetMinSeverity() const noexcept override { 
        return m_MinSeverity.load(std::memory_order_relaxed); 
    }
    
    void SetMinSeverity(Severity severity) noexcept override {
        m_MinSeverity.store(severity, std::memory_order_relaxed);
    }
    
    void SetOptions(const Options& options) noexcept;
    
private:
    Options m_Options;
    std::atomic<Severity> m_MinSeverity{Severity::Info};
    std::mutex m_Mutex;
    
    [[nodiscard]] std::string GetColorCode(Severity severity) const;
    void ResetColor() const;
};

// ================ Memory Trace Sink (Ring Buffer) ================
class MemoryTraceSink final : public ITraceSink {
public:
    explicit MemoryTraceSink(size_t capacity = 1000);
    
    void Write(const LogEntry& entry) override;
    void Flush() override {}  // No-op for memory sink
    
    [[nodiscard]] Severity GetMinSeverity() const noexcept override { 
        return m_MinSeverity.load(std::memory_order_relaxed); 
    }
    
    void SetMinSeverity(Severity severity) noexcept override {
        m_MinSeverity.store(severity, std::memory_order_relaxed);
    }
    
    [[nodiscard]] std::vector<LogEntry> GetEntries() const;
    [[nodiscard]] std::vector<LogEntry> GetEntries(Severity minSeverity) const;
    void Clear();
    [[nodiscard]] size_t GetCount() const noexcept;
    [[nodiscard]] size_t GetCapacity() const noexcept { return m_Capacity; }
    
private:
    mutable std::mutex m_Mutex;
    std::vector<LogEntry> m_Entries;
    size_t m_Capacity;
    size_t m_Index = 0;
    std::atomic<Severity> m_MinSeverity{Severity::Info};
};

// ================ Trace Manager ================
class TraceManager {
public:
    static TraceManager& Instance();
    
    // Management
    void AddSink(std::unique_ptr<ITraceSink> sink);
    void RemoveSink(ITraceSink* sink);
    void RemoveAllSinks();
    
    // Configuration
    void SetModuleSeverity(ModuleID module, Severity severity);
    [[nodiscard]] Severity GetModuleSeverity(ModuleID module) const;
    
    void SetModuleName(ModuleID module, std::string_view name);
    [[nodiscard]] std::string_view GetModuleName(ModuleID module) const;
    
    // Logging
    void Log(ModuleID module, Severity severity, 
             std::string_view message,
             const SourceLocation& location = SourceLocation{});
    
    template <typename... Args>
    void Log(ModuleID module, Severity severity,
             std::string_view format, Args&&... args,
             const SourceLocation& location = SourceLocation{}) {
        if (ShouldLog(module, severity)) {
            Log(module, severity, 
                std::vformat(format, std::make_format_args(std::forward<Args>(args)...)),
                location);
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
    
    [[nodiscard]] Statistics GetStatistics() const;
    void ResetStatistics();
    
private:
    TraceManager();
    ~TraceManager();
    
    mutable std::mutex m_Mutex;
    std::vector<std::unique_ptr<ITraceSink>> m_Sinks;
    std::array<std::atomic<Severity>, Module::Max> m_ModuleSeverities;
    std::array<std::string, Module::Max> m_ModuleNames;
    Statistics m_Stats;
    
    void InitializeDefaultModuleNames();
    [[nodiscard]] LogEntry CreateEntry(ModuleID module, Severity severity,
                                      std::string_view message,
                                      const SourceLocation& location) const;
};

// ================ Convenience Macros ================
// Main logging macro
#define TRACE_LOG(module, severity, ...) \
    do { \
        if (Trace::TraceManager::Instance().ShouldLog((module), (severity))) { \
            Trace::TraceManager::Instance().Log((module), (severity), __VA_ARGS__); \
        } \
    } while(0)

// Convenience macros for specific severities
#define TRACE_ERROR(module, ...)   TRACE_LOG((module), Trace::Severity::Error, __VA_ARGS__)
#define TRACE_WARN(module, ...)    TRACE_LOG((module), Trace::Severity::Warning, __VA_ARGS__)
#define TRACE_INFO(module, ...)    TRACE_LOG((module), Trace::Severity::Info, __VA_ARGS__)
#define TRACE_DEBUG(module, ...)   TRACE_LOG((module), Trace::Severity::Debug, __VA_ARGS__)
#define TRACE_VERBOSE(module, ...) TRACE_LOG((module), Trace::Severity::Verbose, __VA_ARGS__)

// Conditional logging macros
#define TRACE_IF(condition, module, severity, ...) \
    do { \
        if ((condition) && Trace::TraceManager::Instance().ShouldLog((module), (severity))) { \
            Trace::TraceManager::Instance().Log((module), (severity), __VA_ARGS__); \
        } \
    } while(0)

// Once-only logging
#define TRACE_ONCE(module, severity, ...) \
    do { \
        static bool TRACE_ONCE_flag = false; \
        if (!TRACE_ONCE_flag && Trace::TraceManager::Instance().ShouldLog((module), (severity))) { \
            TRACE_ONCE_flag = true; \
            Trace::TraceManager::Instance().Log((module), (severity), __VA_ARGS__); \
        } \
    } while(0)

// ================ Backward Compatibility ================
// Legacy function names
inline void WriteTraceFull(Trace::ModuleID module, Trace::Severity severity,
                          const char* file, int line, const char* function,
                          const char* format, ...) {
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

inline void TraceFlushLog() {
    Trace::TraceManager::Instance().FlushAll();
}

inline void TraceSetMaxModule(Trace::ModuleID maxModule, Trace::Severity defaultSeverity) {
    for (Trace::ModuleID i = 0; i < maxModule && i < Trace::Module::Max; ++i) {
        Trace::TraceManager::Instance().SetModuleSeverity(i, defaultSeverity);
    }
}

// Legacy macro for backward compatibility
#ifdef _WIN32
    #define WriteTrace(module, severity, format, ...) \
        do { \
            if (Trace::TraceManager::Instance().ShouldLog((module), (severity))) { \
                WriteTraceFull((module), (severity), __FILE__, __LINE__, __FUNCTION__, \
                              (format), ##__VA_ARGS__); \
            } \
        } while(0)
#else
    #define WriteTrace(module, severity, format, ...) \
        do { \
            if (Trace::TraceManager::Instance().ShouldLog((module), (severity))) { \
                WriteTraceFull((module), (severity), __FILE__, __LINE__, __PRETTY_FUNCTION__, \
                              (format), ##__VA_ARGS__); \
            } \
        } while(0)
#endif

} // namespace Trace

// ================ Global Convenience Functions ================
// These maintain compatibility with the original API
inline Trace::ITraceSink* TraceAddModule(std::unique_ptr<Trace::ITraceSink> sink) {
    auto* ptr = sink.get();
    Trace::TraceManager::Instance().AddSink(std::move(sink));
    return ptr;
}

inline void TraceRemoveModule(Trace::ITraceSink* sink) {
    Trace::TraceManager::Instance().RemoveSink(sink);
}

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

inline void CloseTrace() {
    Trace::TraceManager::Instance().RemoveAllSinks();
}