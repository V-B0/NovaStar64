/**
 * @file log.cpp
 * @brief Thread-safe logging with level filtering, console colour, and file output.
 */

#include "log.hpp"
#include "platform.hpp"

#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#if PLATFORM_WINDOWS
#include <windows.h>
#endif

FileSystem::File Logging::log_file;

namespace {

std::recursive_mutex g_logMutex;

struct LoggingState {
    Logging::Level minLevel      = Logging::Level::Info;
    bool           consoleOutput = true;
    bool           fileOutput    = false;
    std::string    filename;
};

LoggingState& State()
{
    static LoggingState state;
    return state;
}

const char* LevelName(Logging::Level level)
{
    switch (level) {
        case Logging::Level::Debug:    return "DEBUG";
        case Logging::Level::Info:     return "INFO";
        case Logging::Level::Warning:  return "WARNING";
        case Logging::Level::Error:    return "ERROR";
        case Logging::Level::Critical: return "CRITICAL";
    }
    return "UNKNOWN";
}

/// "[YYYY-MM-DD HH:MM:SS]"
std::string Timestamp()
{
    const std::time_t now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());

    std::tm local{};
#if PLATFORM_WINDOWS
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif

    char buffer[32];
    if (std::strftime(buffer, sizeof(buffer), "[%Y-%m-%d %H:%M:%S]", &local) == 0) {
        return "[????-??-?? ??:??:??]";
    }
    return buffer;
}

/// Always newline-terminated so callers need not supply one.
std::string FormatLine(Logging::Level level, const std::string& message)
{
    std::string line = Timestamp();
    line += " [";
    line += LevelName(level);
    line += "] ";
    line += message;
    if (line.empty() || line.back() != '\n') line += '\n';
    return line;
}

void WriteConsole(Logging::Level level, const std::string& line)
{
#if PLATFORM_WINDOWS
    HANDLE console = GetStdHandle(STD_ERROR_HANDLE);
    if (console != INVALID_HANDLE_VALUE) {
        WORD colour = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        switch (level) {
            case Logging::Level::Debug:    colour = FOREGROUND_INTENSITY; break;
            case Logging::Level::Info:     colour = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY; break;
            case Logging::Level::Warning:  colour = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
            case Logging::Level::Error:    colour = FOREGROUND_RED | FOREGROUND_INTENSITY; break;
            case Logging::Level::Critical: colour = FOREGROUND_RED | FOREGROUND_INTENSITY | BACKGROUND_RED; break;
        }
        SetConsoleTextAttribute(console, colour);
        std::cerr << line;
        SetConsoleTextAttribute(console, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        std::cerr.flush();
        return;
    }
    std::cerr << line;
#else
    const char* colour = "";
    switch (level) {
        case Logging::Level::Debug:    colour = "\033[90m";   break;
        case Logging::Level::Info:     colour = "\033[36m";   break;
        case Logging::Level::Warning:  colour = "\033[33m";   break;
        case Logging::Level::Error:    colour = "\033[31m";   break;
        case Logging::Level::Critical: colour = "\033[1;31m"; break;
    }
    std::cerr << colour << line << "\033[0m";
#endif
    std::cerr.flush();
}

void WriteFile(const std::string& line)
{
    if (!Logging::log_file.is_open()) return;

    const size_t written = Logging::log_file.write(line.c_str(), line.size());
    if (written != line.size() && State().consoleOutput) {
        // Reported directly rather than through the logger, to avoid recursing.
        std::cerr << "[LOG ERROR] Incomplete write to log file\n";
    }
    Logging::log_file.flush();
}

/// Renders a printf-style message, growing the buffer if the first try truncates.
std::string FormatVa(const char* format, std::va_list args)
{
    if (!format) return {};

    std::va_list copy;
    va_copy(copy, args);
    const int needed = std::vsnprintf(nullptr, 0, format, copy);
    va_end(copy);

    if (needed <= 0) return {};

    std::vector<char> buffer(static_cast<size_t>(needed) + 1);
    std::vsnprintf(buffer.data(), buffer.size(), format, args);
    return std::string(buffer.data(), static_cast<size_t>(needed));
}

void LogFormatted(Logging::Level level, const char* format, std::va_list args)
{
    Logging::log(level, FormatVa(format, args));
}

} // namespace

void Logging::log(Level level, const std::string& message)
{
    std::lock_guard<std::recursive_mutex> lock(g_logMutex);

    LoggingState& state = State();
    if (level < state.minLevel) return;

    const std::string line = FormatLine(level, message);
    if (state.consoleOutput) WriteConsole(level, line);
    if (state.fileOutput)    WriteFile(line);
}

void Logging::stop_logging()
{
    std::lock_guard<std::recursive_mutex> lock(g_logMutex);

    if (log_file.is_open()) log_file.close();
    State().fileOutput = false;
}

namespace LoggingConfig {

void set_min_level(Logging::Level level)
{
    std::lock_guard<std::recursive_mutex> lock(g_logMutex);
    State().minLevel = level;
}

Logging::Level get_min_level()
{
    std::lock_guard<std::recursive_mutex> lock(g_logMutex);
    return State().minLevel;
}

void enable_console_output(bool enable)
{
    std::lock_guard<std::recursive_mutex> lock(g_logMutex);
    State().consoleOutput = enable;
}

bool is_console_output_enabled()
{
    std::lock_guard<std::recursive_mutex> lock(g_logMutex);
    return State().consoleOutput;
}

bool open_log_file(const std::string& filename, bool append)
{
    std::lock_guard<std::recursive_mutex> lock(g_logMutex);

    LoggingState& state = State();
    if (Logging::log_file.is_open()) Logging::log_file.close();

    const FileSystem::OpenMode mode =
        append ? (FileSystem::OpenMode::ReadWrite | FileSystem::OpenMode::Create)
               : (FileSystem::OpenMode::Write | FileSystem::OpenMode::Create |
                  FileSystem::OpenMode::Truncate);

    try {
        Logging::log_file.open(filename, mode);
    } catch (const FileSystem::FileError&) {
        state.fileOutput = false;
        state.filename.clear();
        return false;
    }

    // Opening ReadWrite starts at offset 0, so appending has to seek past
    // whatever is already in the file.
    if (append) {
        Logging::log_file.seek(0, FileSystem::SeekOrigin::End);
    }

    state.fileOutput = true;
    state.filename   = filename;
    return true;
}

void close_log_file()
{
    std::lock_guard<std::recursive_mutex> lock(g_logMutex);

    if (Logging::log_file.is_open()) Logging::log_file.close();
    State().fileOutput = false;
    State().filename.clear();
}

const std::string& get_log_filename()
{
    std::lock_guard<std::recursive_mutex> lock(g_logMutex);
    return State().filename;
}

} // namespace LoggingConfig

namespace Log {

void debug   (const std::string& m) { Logging::log(Logging::Level::Debug,    m); }
void info    (const std::string& m) { Logging::log(Logging::Level::Info,     m); }
void warning (const std::string& m) { Logging::log(Logging::Level::Warning,  m); }
void error   (const std::string& m) { Logging::log(Logging::Level::Error,    m); }
void critical(const std::string& m) { Logging::log(Logging::Level::Critical, m); }

#define NOVA_LOG_VARIADIC(fnName, levelName)                    \
    void fnName(const char* format, ...)                        \
    {                                                           \
        std::va_list args;                                      \
        va_start(args, format);                                 \
        LogFormatted(Logging::Level::levelName, format, args);  \
        va_end(args);                                           \
    }

NOVA_LOG_VARIADIC(debugf,    Debug)
NOVA_LOG_VARIADIC(infof,     Info)
NOVA_LOG_VARIADIC(warningf,  Warning)
NOVA_LOG_VARIADIC(errorf,    Error)
NOVA_LOG_VARIADIC(criticalf, Critical)

#undef NOVA_LOG_VARIADIC

} // namespace Log
