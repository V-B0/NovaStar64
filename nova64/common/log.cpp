/**
 * @file log.cpp
 * @brief Logging system implementation
 *
 * Provides a thread-safe logging system with file output and log level filtering.
 * Based on Project64's logging system with modern C++ improvements.
 */

#include "log.hpp"
#include "DateTime.hpp"
#include "LockGuard.hpp"
#include "platform.hpp"
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>
#include <sstream>

// =============================================================================
// Static Member Definitions
// =============================================================================

FileSystem::File Logging::log_file;

// =============================================================================
// Internal State
// =============================================================================

namespace {

// Thread synchronization
RecursiveMutex g_log_mutex;

// Logging configuration state
struct LoggingState {
    Logging::Level min_level = Logging::Level::Info;
    bool console_output_enabled = true;
    bool file_output_enabled = false;
    std::string log_filename;
};

LoggingState& get_state() {
    static LoggingState state;
    return state;
}

// Level to string conversion
const char* level_to_string(Logging::Level level) {
    switch (level) {
        case Logging::Level::Debug:    return "DEBUG";
        case Logging::Level::Info:     return "INFO";
        case Logging::Level::Warning:  return "WARNING";
        case Logging::Level::Error:    return "ERROR";
        case Logging::Level::Critical: return "CRITICAL";
        default:                       return "UNKNOWN";
    }
}

// Get timestamp string in format: [YYYY-MM-DD HH:MM:SS]
std::string get_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time_t_now = std::chrono::system_clock::to_time_t(now);
    
    std::tm local_time;
#if PLATFORM_WINDOWS
    localtime_s(&local_time, &time_t_now);
#else
    localtime_r(&time_t_now, &local_time);
#endif

    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "[%Y-%m-%d %H:%M:%S]", &local_time);
    return std::string(buffer);
}

// Format message with timestamp and level
std::string format_log_message(Logging::Level level, const std::string& message) {
    std::ostringstream oss;
    oss << get_timestamp() << " [" << level_to_string(level) << "] " << message;
    if (!message.empty() && message.back() != '\n') {
        oss << '\n';
    }
    return oss.str();
}

// Write to console with color support (Windows only for now)
void write_to_console(Logging::Level level, const std::string& formatted_message) {
#if PLATFORM_WINDOWS
    HANDLE console = GetStdHandle(STD_ERROR_HANDLE);
    if (console != INVALID_HANDLE_VALUE) {
        WORD color = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE; // Default white
        
        switch (level) {
            case Logging::Level::Debug:
                color = FOREGROUND_INTENSITY; // Gray
                break;
            case Logging::Level::Info:
                color = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY; // Cyan
                break;
            case Logging::Level::Warning:
                color = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY; // Yellow
                break;
            case Logging::Level::Error:
                color = FOREGROUND_RED | FOREGROUND_INTENSITY; // Bright red
                break;
            case Logging::Level::Critical:
                color = FOREGROUND_RED | FOREGROUND_INTENSITY | BACKGROUND_RED; // Red on red background
                break;
        }
        
        SetConsoleTextAttribute(console, color);
        std::cerr << formatted_message;
        SetConsoleTextAttribute(console, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE); // Reset
    } else {
        std::cerr << formatted_message;
    }
#else
    // ANSI color codes for POSIX systems
    const char* color_code = "";
    const char* reset_code = "\033[0m";
    
    switch (level) {
        case Logging::Level::Debug:
            color_code = "\033[90m"; // Gray
            break;
        case Logging::Level::Info:
            color_code = "\033[36m"; // Cyan
            break;
        case Logging::Level::Warning:
            color_code = "\033[33m"; // Yellow
            break;
        case Logging::Level::Error:
            color_code = "\033[31m"; // Red
            break;
        case Logging::Level::Critical:
            color_code = "\033[1;31m"; // Bold red
            break;
    }
    
    std::cerr << color_code << formatted_message << reset_code;
#endif
    std::cerr.flush();
}

// Write to file
void write_to_file(const std::string& formatted_message) {
    auto& state = get_state();
    
    if (!state.file_output_enabled || !Logging::log_file.is_open()) {
        return;
    }
    
    // Write to file
    const size_t bytes_written = Logging::log_file.write(
        formatted_message.c_str(), 
        formatted_message.size()
    );
    
    if (bytes_written != formatted_message.size()) {
        // Failed to write complete message - try to report error but avoid recursion
        if (state.console_output_enabled) {
            std::cerr << "[LOG ERROR] Failed to write complete log message to file\n";
        }
    }
    
    // Always flush for log files to ensure messages are written immediately
    Logging::log_file.flush();
}

} // anonymous namespace

// =============================================================================
// Logging Implementation
// =============================================================================

void Logging::log(Level level, const std::string& message) {
    LockGuard lock(g_log_mutex);
    
    auto& state = get_state();
    
    // Check if message should be logged based on level
    if (level < state.min_level) {
        return;
    }
    
    // Format the message
    const std::string formatted_message = format_log_message(level, message);
    
    // Output to console if enabled
    if (state.console_output_enabled) {
        write_to_console(level, formatted_message);
    }
    
    // Output to file if enabled
    if (state.file_output_enabled) {
        write_to_file(formatted_message);
    }
}

void Logging::stop_logging() {
    LockGuard lock(g_log_mutex);
    
    auto& state = get_state();
    
    if (log_file.is_open()) {
        log_file.close();
    }
    
    state.file_output_enabled = false;
}

// =============================================================================
// LoggingConfig Implementation
// =============================================================================

void LoggingConfig::set_min_level(Logging::Level level) {
    LockGuard lock(g_log_mutex);
    get_state().min_level = level;
}

Logging::Level LoggingConfig::get_min_level() {
    LockGuard lock(g_log_mutex);
    return get_state().min_level;
}

void LoggingConfig::enable_console_output(bool enable) {
    LockGuard lock(g_log_mutex);
    get_state().console_output_enabled = enable;
}

bool LoggingConfig::is_console_output_enabled() {
    LockGuard lock(g_log_mutex);
    return get_state().console_output_enabled;
}

bool LoggingConfig::open_log_file(const std::string& filename, bool append) {
    LockGuard lock(g_log_mutex);
    
    auto& state = get_state();
    
    // Close existing file if open
    if (Logging::log_file.is_open()) {
        Logging::log_file.close();
    }
    
    // Open new file
    const auto open_mode = append 
        ? (FileSystem::OpenMode::ReadWrite | FileSystem::OpenMode::Create)
        : (FileSystem::OpenMode::Write | FileSystem::OpenMode::Create | FileSystem::OpenMode::Truncate);
    
    try {
        Logging::log_file.open(filename, open_mode);
    } catch (const FileSystem::FileError&) {
        state.file_output_enabled = false;
        state.log_filename.clear();
        return false;
    }
    
    // If appending, seek to end
    if (append) {
        Logging::log_file.seek(0, FileSystem::SeekOrigin::End);
    }
    
    state.file_output_enabled = true;
    state.log_filename = filename;
    
    return true;
}

void LoggingConfig::close_log_file() {
    LockGuard lock(g_log_mutex);
    
    auto& state = get_state();
    
    if (Logging::log_file.is_open()) {
        Logging::log_file.close();
    }
    
    state.file_output_enabled = false;
}

const std::string& LoggingConfig::get_log_filename() {
    LockGuard lock(g_log_mutex);
    return get_state().log_filename;
}

// =============================================================================
// Log Namespace - Convenience Functions
// =============================================================================

void Log::debug(const std::string& message) {
    Logging::log(Logging::Level::Debug, message);
}

void Log::info(const std::string& message) {
    Logging::log(Logging::Level::Info, message);
}

void Log::warning(const std::string& message) {
    Logging::log(Logging::Level::Warning, message);
}

void Log::error(const std::string& message) {
    Logging::log(Logging::Level::Error, message);
}

void Log::critical(const std::string& message) {
    Logging::log(Logging::Level::Critical, message);
}

// =============================================================================
// Printf-Style Functions
// =============================================================================

namespace {
    // Helper for printf-style formatting
    std::string format_printf_style(const char* format, va_list args) {
        // Determine required buffer size
        va_list args_copy;
        va_copy(args_copy, args);
        const int size = std::vsnprintf(nullptr, 0, format, args_copy);
        va_end(args_copy);
        
        if (size < 0) {
            return "[LOG ERROR: Invalid format string]";
        }
        
        // Format the string
        std::string result(static_cast<size_t>(size) + 1, '\0');
        std::vsnprintf(&result[0], result.size(), format, args);
        result.resize(static_cast<size_t>(size)); // Remove null terminator
        
        return result;
    }
}

void Log::debugf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const std::string message = format_printf_style(format, args);
    va_end(args);
    
    Logging::log(Logging::Level::Debug, message);
}

void Log::infof(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const std::string message = format_printf_style(format, args);
    va_end(args);
    
    Logging::log(Logging::Level::Info, message);
}

void Log::warningf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const std::string message = format_printf_style(format, args);
    va_end(args);
    
    Logging::log(Logging::Level::Warning, message);
}

void Log::errorf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const std::string message = format_printf_style(format, args);
    va_end(args);
    
    Logging::log(Logging::Level::Error, message);
}

void Log::criticalf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const std::string message = format_printf_style(format, args);
    va_end(args);
    
    Logging::log(Logging::Level::Critical, message);
}
