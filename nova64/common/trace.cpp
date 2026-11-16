/**
 * @file trace.cpp
 * @brief Trace manager, log entry formatting, and the file, console and memory sinks.
 */
#include "trace.hpp"
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <ctime>
#include <cstring>

#ifdef _WIN32
#include <Windows.h>
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace Trace {

// --- LogEntry ---

std::string LogEntry::Format(bool includeTimestamp) const {
    std::ostringstream oss;

    if (includeTimestamp) {
        auto time = std::chrono::system_clock::to_time_t(timestamp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch()) % 1000;

        std::tm tm_time;
#ifdef _WIN32
        localtime_s(&tm_time, &time);
#else
        localtime_r(&time, &tm_time);
#endif

        oss << std::put_time(&tm_time, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count()
            << ' ';
    }

    if (static_cast<size_t>(severity) < SeverityNames.size()) {
        oss << '[' << SeverityNames[static_cast<size_t>(severity)] << "] ";
    }

    oss << '[' << TraceModule(module) << "] ";

    if (!file.empty()) {
        // Only the file name, not the full path.
        size_t lastSlash = file.find_last_of("/\\");
        std::string_view filename = (lastSlash != std::string_view::npos) ? file.substr(lastSlash + 1) : file;

        oss << filename << ':' << line;

        if (!function.empty()) {
            oss << " (" << function << ")";
        }

        oss << ": ";
    }

    oss << message;

    return oss.str();
}

// --- FileTraceSink ---

// --- ConsoleTraceSink ---

// --- MemoryTraceSink ---

// --- TraceManager ---

TraceManager::TraceManager() {
    // Initialize module severities to Info by default
    for (auto& severity : m_ModuleSeverities) {
        severity.store(Severity::Info, std::memory_order_relaxed);
    }

    InitializeDefaultModuleNames();
}

TraceManager::~TraceManager() { RemoveAllSinks(); }

TraceManager& TraceManager::Instance() {
    static TraceManager instance;
    return instance;
}

void TraceManager::InitializeDefaultModuleNames() {
    m_ModuleNames[Module::None] = "None";
    m_ModuleNames[Module::Core] = "Core";
    m_ModuleNames[Module::Video] = "Video";
    m_ModuleNames[Module::Audio] = "Audio";
    m_ModuleNames[Module::Input] = "Input";
    m_ModuleNames[Module::RSP] = "RSP";
    m_ModuleNames[Module::RDP] = "RDP";
    m_ModuleNames[Module::CPU] = "CPU";
    m_ModuleNames[Module::Plugin] = "Plugin";
    m_ModuleNames[Module::Config] = "Config";
    m_ModuleNames[Module::Debug] = "Debug";
    m_ModuleNames[Module::Network] = "Network";

    // Fill remaining with generic names
    for (ModuleID i = 12; i < Module::Max; ++i) {
        m_ModuleNames[i] = "Module" + std::to_string(i);
    }
}

void TraceManager::AddSink(std::unique_ptr<ITraceSink> sink) {
    if (!sink) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Sinks.push_back(std::move(sink));
}

void TraceManager::RemoveSink(ITraceSink* sink) {
    if (!sink) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_Mutex);

    m_Sinks.erase(std::remove_if(m_Sinks.begin(), m_Sinks.end(),
                                 [sink](const std::unique_ptr<ITraceSink>& ptr) { return ptr.get() == sink; }),
                  m_Sinks.end());
}

void TraceManager::RemoveAllSinks() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Sinks.clear();
}

void TraceManager::SetModuleSeverity(ModuleID module, Severity severity) {
    if (module < Module::Max) {
        m_ModuleSeverities[module].store(severity, std::memory_order_relaxed);
    }
}

void TraceManager::SetModuleName(ModuleID module, std::string_view name) {
    if (module < Module::Max) {
        m_ModuleNames[module] = name;
    }
}

std::string_view TraceManager::GetModuleName(ModuleID module) const {
    if (module < Module::Max) {
        return m_ModuleNames[module];
    }
    return "Unknown";
}

bool TraceManager::ShouldLog(ModuleID module, Severity severity) const noexcept {
    if (module >= Module::Max) {
        return false;
    }

    Severity moduleSeverity = m_ModuleSeverities[module].load(std::memory_order_relaxed);
    return severity <= moduleSeverity;
}

LogEntry TraceManager::CreateEntry(ModuleID module, Severity severity, std::string_view message,
                                   const SourceLocation& location) const {
    LogEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.module = module;
    entry.severity = severity;
    entry.file = location.file_name();
    entry.line = location.line();
    entry.function = location.function_name();
    entry.message = message;

    return entry;
}

void TraceManager::Log(ModuleID module, Severity severity, std::string_view message, const SourceLocation& location) {
    if (!ShouldLog(module, severity)) {
        return;
    }

    LogEntry entry = CreateEntry(module, severity, message, location);

    // Update statistics
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Stats.totalEntries++;
        if (static_cast<size_t>(severity) < m_Stats.severityCounts.size()) {
            m_Stats.severityCounts[static_cast<size_t>(severity)]++;
        }
        if (module < Module::Max) {
            m_Stats.moduleCounts[module]++;
        }

        for (const auto& sink : m_Sinks) {
            if (sink) {
                sink->Write(entry);
            }
        }
    }
}

void TraceManager::FlushAll() {
    std::lock_guard<std::mutex> lock(m_Mutex);

    for (const auto& sink : m_Sinks) {
        if (sink) {
            sink->Flush();
        }
    }
}

} // namespace Trace
