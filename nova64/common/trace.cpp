/**
 * @file trace.cpp
 * @brief trace implementation.
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

// ================ LogEntry Implementation ================

std::string LogEntry::Format(bool includeTimestamp) const
{
    std::ostringstream oss;
    
    if (includeTimestamp)
    {
        // Format timestamp
        auto time = std::chrono::system_clock::to_time_t(timestamp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            timestamp.time_since_epoch()
        ) % 1000;
        
        std::tm tm_time;
#ifdef _WIN32
        localtime_s(&tm_time, &time);
#else
        localtime_r(&time, &tm_time);
#endif
        
        oss << std::put_time(&tm_time, "%Y-%m-%d %H:%M:%S")
            << '.' << std::setfill('0') << std::setw(3) << ms.count() << ' ';
    }
    
    // Severity
    if (static_cast<size_t>(severity) < SeverityNames.size())
    {
        oss << '[' << SeverityNames[static_cast<size_t>(severity)] << "] ";
    }
    
    // Module name
    oss << '[' << TraceModule(module) << "] ";
    
    // Location info
    if (!file.empty())
    {
        // Extract just the filename from the path
        size_t lastSlash = file.find_last_of("/\\");
        std::string_view filename = (lastSlash != std::string_view::npos) 
            ? file.substr(lastSlash + 1) 
            : file;
        
        oss << filename << ':' << line;
        
        if (!function.empty())
        {
            oss << " (" << function << ")";
        }
        
        oss << ": ";
    }
    
    // Message
    oss << message;
    
    return oss.str();
}

std::string LogEntry::FormatShort() const
{
    std::ostringstream oss;
    
    // Just severity and message for compact output
    if (static_cast<size_t>(severity) < SeverityNames.size())
    {
        oss << '[' << SeverityNames[static_cast<size_t>(severity)] << "] ";
    }
    
    oss << message;
    
    return oss.str();
}

// ================ FileTraceSink Implementation ================

FileTraceSink::FileTraceSink(std::string_view filename, const Options& options)
    : m_Filename(filename)
    , m_Options(options)
{
    OpenFile();
}

FileTraceSink::~FileTraceSink()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_FileStream.is_open())
    {
        m_FileStream.flush();
        m_FileStream.close();
    }
}

FileTraceSink::FileTraceSink(FileTraceSink&& other) noexcept
    : m_Filename(std::move(other.m_Filename))
    , m_Options(std::move(other.m_Options))
    , m_FileStream(std::move(other.m_FileStream))
{
    m_MinSeverity.store(other.m_MinSeverity.load(), std::memory_order_relaxed);
}

FileTraceSink& FileTraceSink::operator=(FileTraceSink&& other) noexcept
{
    if (this != &other)
    {
        std::lock_guard<std::mutex> lock1(m_Mutex);
        std::lock_guard<std::mutex> lock2(other.m_Mutex);
        
        m_Filename = std::move(other.m_Filename);
        m_Options = std::move(other.m_Options);
        m_FileStream = std::move(other.m_FileStream);
        m_MinSeverity.store(other.m_MinSeverity.load(), std::memory_order_relaxed);
    }
    return *this;
}

void FileTraceSink::OpenFile()
{
    m_FileStream.open(m_Filename, std::ios::out | std::ios::app);
    
    if (!m_FileStream.is_open())
    {
        // Failed to open file - try creating directory
        size_t lastSlash = m_Filename.find_last_of("/\\");
        if (lastSlash != std::string::npos)
        {
            std::string dir = m_Filename.substr(0, lastSlash);
            
#ifdef _WIN32
            CreateDirectoryA(dir.c_str(), nullptr);
#else
            mkdir(dir.c_str(), 0755);
#endif
            
            // Try again
            m_FileStream.open(m_Filename, std::ios::out | std::ios::app);
        }
    }
}

void FileTraceSink::Write(const LogEntry& entry)
{
    if (entry.severity > m_MinSeverity.load(std::memory_order_relaxed))
    {
        return;
    }
    
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    if (!m_FileStream.is_open())
    {
        return;
    }
    
    // Write formatted entry
    std::string formatted = entry.Format(true);
    m_FileStream << formatted << '\n';
    
    if (m_Options.flushImmediately)
    {
        m_FileStream.flush();
    }
    
    // Check if rotation is needed
    if (m_Options.maxFileSize > 0)
    {
        RotateIfNeeded();
    }
}

void FileTraceSink::Flush()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_FileStream.is_open())
    {
        m_FileStream.flush();
    }
}

bool FileTraceSink::RotateIfNeeded()
{
    if (!m_FileStream.is_open())
    {
        return false;
    }
    
    // Check current file size
    auto currentPos = m_FileStream.tellp();
    if (currentPos < 0 || static_cast<size_t>(currentPos) < m_Options.maxFileSize)
    {
        return false;
    }
    
    // Close current file
    m_FileStream.close();
    
    // Rotate archived logs
    RotateFile();
    
    // Reopen file
    m_FileStream.open(m_Filename, std::ios::out | std::ios::trunc);
    
    return true;
}

void FileTraceSink::RotateFile()
{
    // Delete oldest archive if it exists
    if (m_Options.maxArchiveFiles > 0)
    {
        std::string oldestArchive = GenerateArchiveName(m_Options.maxArchiveFiles);
        std::remove(oldestArchive.c_str());
    }
    
    // Rotate existing archives
    for (size_t i = m_Options.maxArchiveFiles; i > 0; --i)
    {
        std::string from = GenerateArchiveName(i - 1);
        std::string to = GenerateArchiveName(i);
        
        std::rename(from.c_str(), to.c_str());
    }
    
    // Move current log to first archive
    if (m_Options.maxArchiveFiles > 0)
    {
        std::string archive = GenerateArchiveName(0);
        std::rename(m_Filename.c_str(), archive.c_str());
    }
}

std::string FileTraceSink::GenerateArchiveName(size_t index) const
{
    std::string result = m_Options.archivePattern;
    
    // Replace {name} with base filename
    size_t lastSlash = m_Filename.find_last_of("/\\");
    size_t lastDot = m_Filename.find_last_of('.');
    
    std::string baseName = (lastDot != std::string::npos) 
        ? m_Filename.substr((lastSlash != std::string::npos ? lastSlash + 1 : 0), lastDot - (lastSlash != std::string::npos ? lastSlash + 1 : 0))
        : m_Filename.substr(lastSlash != std::string::npos ? lastSlash + 1 : 0);
    
    size_t namePos = result.find("{name}");
    if (namePos != std::string::npos)
    {
        result.replace(namePos, 6, baseName);
    }
    
    // Replace {index} with archive index
    size_t indexPos = result.find("{index}");
    if (indexPos != std::string::npos)
    {
        result.replace(indexPos, 7, std::to_string(index));
    }
    
    // If result doesn't have a directory, use the same directory as original file
    if (result.find_first_of("/\\") == std::string::npos && lastSlash != std::string::npos)
    {
        result = m_Filename.substr(0, lastSlash + 1) + result;
    }
    
    return result;
}

// ================ ConsoleTraceSink Implementation ================

ConsoleTraceSink::ConsoleTraceSink(const Options& options)
    : m_Options(options)
{
}

std::string ConsoleTraceSink::GetColorCode(Severity severity) const
{
#ifdef _WIN32
    // Windows console colors are set differently
    return "";
#else
    if (!m_Options.useColors)
    {
        return "";
    }
    
    // ANSI color codes
    switch (severity)
    {
    case Severity::Error:   return "\033[1;31m"; // Bold Red
    case Severity::Warning: return "\033[1;33m"; // Bold Yellow
    case Severity::Notice:  return "\033[1;36m"; // Bold Cyan
    case Severity::Info:    return "\033[1;37m"; // Bold White
    case Severity::Debug:   return "\033[0;36m"; // Cyan
    case Severity::Verbose: return "\033[0;37m"; // White
    default:                return "";
    }
#endif
}

void ConsoleTraceSink::ResetColor() const
{
#ifndef _WIN32
    if (m_Options.useColors)
    {
        std::cout << "\033[0m";
    }
#endif
}

void ConsoleTraceSink::Write(const LogEntry& entry)
{
    if (entry.severity > m_MinSeverity.load(std::memory_order_relaxed))
    {
        return;
    }
    
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    std::ostringstream oss;
    
    // Color
    if (m_Options.useColors)
    {
        oss << GetColorCode(entry.severity);
    }
    
    // Timestamp
    if (m_Options.showTimestamp)
    {
        auto time = std::chrono::system_clock::to_time_t(entry.timestamp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            entry.timestamp.time_since_epoch()
        ) % 1000;
        
        std::tm tm_time;
#ifdef _WIN32
        localtime_s(&tm_time, &time);
#else
        localtime_r(&time, &tm_time);
#endif
        
        oss << std::put_time(&tm_time, "%H:%M:%S")
            << '.' << std::setfill('0') << std::setw(3) << ms.count() << ' ';
    }
    
    // Severity
    if (m_Options.showSeverity)
    {
        if (static_cast<size_t>(entry.severity) < SeverityNames.size())
        {
            oss << '[' << SeverityNames[static_cast<size_t>(entry.severity)] << "] ";
        }
    }
    
    // Module
    if (m_Options.showModule)
    {
        oss << '[' << TraceModule(entry.module) << "] ";
    }
    
    // Location
    if (m_Options.showLocation && !entry.file.empty())
    {
        size_t lastSlash = entry.file.find_last_of("/\\");
        std::string_view filename = (lastSlash != std::string_view::npos) 
            ? entry.file.substr(lastSlash + 1) 
            : entry.file;
        
        oss << filename << ':' << entry.line << ' ';
    }
    
    // Message
    oss << entry.message;
    
    // Reset color
    if (m_Options.useColors)
    {
        oss << "\033[0m";
    }
    
    oss << '\n';
    
    // Output to appropriate stream
    std::string output = oss.str();
    
    switch (m_Options.target)
    {
    case OutputTarget::StdOut:
        std::cout << output;
        std::cout.flush();
        break;
        
    case OutputTarget::StdErr:
        std::cerr << output;
        std::cerr.flush();
        break;
        
    case OutputTarget::Both:
        std::cout << output;
        std::cout.flush();
        std::cerr << output;
        std::cerr.flush();
        break;
    }
}

void ConsoleTraceSink::Flush()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    std::cout.flush();
    std::cerr.flush();
}

void ConsoleTraceSink::SetOptions(const Options& options) noexcept
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Options = options;
}

// ================ MemoryTraceSink Implementation ================

MemoryTraceSink::MemoryTraceSink(size_t capacity)
    : m_Capacity(capacity)
{
    m_Entries.reserve(capacity);
}

void MemoryTraceSink::Write(const LogEntry& entry)
{
    if (entry.severity > m_MinSeverity.load(std::memory_order_relaxed))
    {
        return;
    }
    
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    if (m_Entries.size() < m_Capacity)
    {
        m_Entries.push_back(entry);
    }
    else
    {
        // Ring buffer: overwrite oldest entry
        m_Entries[m_Index] = entry;
        m_Index = (m_Index + 1) % m_Capacity;
    }
}

std::vector<LogEntry> MemoryTraceSink::GetEntries() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Entries;
}

std::vector<LogEntry> MemoryTraceSink::GetEntries(Severity minSeverity) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    std::vector<LogEntry> result;
    std::copy_if(m_Entries.begin(), m_Entries.end(), std::back_inserter(result),
                 [minSeverity](const LogEntry& entry) {
                     return entry.severity <= minSeverity;
                 });
    
    return result;
}

void MemoryTraceSink::Clear()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Entries.clear();
    m_Index = 0;
}

size_t MemoryTraceSink::GetCount() const noexcept
{
    // No lock needed for reading size (atomic operation on most platforms)
    return m_Entries.size();
}

// ================ TraceManager Implementation ================

TraceManager::TraceManager()
{
    // Initialize module severities to Info by default
    for (auto& severity : m_ModuleSeverities)
    {
        severity.store(Severity::Info, std::memory_order_relaxed);
    }
    
    InitializeDefaultModuleNames();
}

TraceManager::~TraceManager()
{
    RemoveAllSinks();
}

TraceManager& TraceManager::Instance()
{
    static TraceManager instance;
    return instance;
}

void TraceManager::InitializeDefaultModuleNames()
{
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
    for (ModuleID i = 12; i < Module::Max; ++i)
    {
        m_ModuleNames[i] = "Module" + std::to_string(i);
    }
}

void TraceManager::AddSink(std::unique_ptr<ITraceSink> sink)
{
    if (!sink)
    {
        return;
    }
    
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Sinks.push_back(std::move(sink));
}

void TraceManager::RemoveSink(ITraceSink* sink)
{
    if (!sink)
    {
        return;
    }
    
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    m_Sinks.erase(
        std::remove_if(m_Sinks.begin(), m_Sinks.end(),
                      [sink](const std::unique_ptr<ITraceSink>& ptr) {
                          return ptr.get() == sink;
                      }),
        m_Sinks.end()
    );
}

void TraceManager::RemoveAllSinks()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Sinks.clear();
}

void TraceManager::SetModuleSeverity(ModuleID module, Severity severity)
{
    if (module < Module::Max)
    {
        m_ModuleSeverities[module].store(severity, std::memory_order_relaxed);
    }
}

Severity TraceManager::GetModuleSeverity(ModuleID module) const
{
    if (module < Module::Max)
    {
        return m_ModuleSeverities[module].load(std::memory_order_relaxed);
    }
    return Severity::None;
}

void TraceManager::SetModuleName(ModuleID module, std::string_view name)
{
    if (module < Module::Max)
    {
        m_ModuleNames[module] = name;
    }
}

std::string_view TraceManager::GetModuleName(ModuleID module) const
{
    if (module < Module::Max)
    {
        return m_ModuleNames[module];
    }
    return "Unknown";
}

bool TraceManager::ShouldLog(ModuleID module, Severity severity) const noexcept
{
    if (module >= Module::Max)
    {
        return false;
    }
    
    Severity moduleSeverity = m_ModuleSeverities[module].load(std::memory_order_relaxed);
    return severity <= moduleSeverity;
}

LogEntry TraceManager::CreateEntry(ModuleID module, Severity severity,
                                   std::string_view message,
                                   const SourceLocation& location) const
{
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

void TraceManager::Log(ModuleID module, Severity severity,
                      std::string_view message,
                      const SourceLocation& location)
{
    if (!ShouldLog(module, severity))
    {
        return;
    }
    
    LogEntry entry = CreateEntry(module, severity, message, location);
    
    // Update statistics
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Stats.totalEntries++;
        if (static_cast<size_t>(severity) < m_Stats.severityCounts.size())
        {
            m_Stats.severityCounts[static_cast<size_t>(severity)]++;
        }
        if (module < Module::Max)
        {
            m_Stats.moduleCounts[module]++;
        }
        
        // Write to all sinks
        for (const auto& sink : m_Sinks)
        {
            if (sink)
            {
                sink->Write(entry);
            }
        }
    }
}

void TraceManager::FlushAll()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    for (const auto& sink : m_Sinks)
    {
        if (sink)
        {
            sink->Flush();
        }
    }
}

TraceManager::Statistics TraceManager::GetStatistics() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Stats;
}

void TraceManager::ResetStatistics()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Stats = Statistics{};
}

} // namespace Trace
