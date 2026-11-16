/**
 * @file log.hpp
 * @brief Thread-safe logging with level filtering and console and file output.
 */

#pragma once
#include "file.hpp"
#include <cstdint>

/**
 * @brief Main logging class
 *
 * Provides static methods for logging messages with different severity levels.
 * Supports both console and file output.
 */
class Logging {
public:
    /**
     * @brief Log severity levels
     */
    enum class Level : uint8_t {
        Debug,   ///< Debug information
        Info,    ///< General information
        Warning, ///< Warning messages
        Error,   ///< Error conditions
        Critical ///< Critical errors
    };

    /**
     * @brief Log a message with the specified level
     * @param level Severity level
     * @param message Message to log
     */
    static void log(Level level, const std::string& message);

    /**
     * @brief Stop logging and close the log file
     */
    static void stop_logging();

    /**
     * @brief The log file (public for internal use by LoggingConfig)
     */
    static FileSystem::File log_file;

    /// A logger that discards everything: every method is empty.
    class Logger {
    public:
        template <typename... Args> void debug(const char*, Args&&...) {}
        template <typename... Args> void info(const char*, Args&&...) {}
        template <typename... Args> void warn(const char*, Args&&...) {}
        template <typename... Args> void error(const char*, Args&&...) {}
        template <typename... Args> void critical(const char*, Args&&...) {}
    };
};

/**
 * @brief Logging configuration namespace
 */
namespace LoggingConfig {

/**
 * @brief Set the minimum log level
 * @param level Minimum level to output
 */
void set_min_level(Logging::Level level);

/**
 * @brief Enable or disable console output
 * @param enable True to enable console output
 */
void enable_console_output(bool enable);

/**
 * @brief Open a log file for output
 * @param filename Path to the log file
 * @param append True to append to existing file, false to truncate
 * @return True on success
 */
bool open_log_file(const std::string& filename, bool append = false);

} // namespace LoggingConfig

/// Convenience wrappers around Logging::log, one per level. The `...f` variants
/// take a printf-style format.
namespace Log {

void debug(const std::string& message);
void info(const std::string& message);
void warning(const std::string& message);
void error(const std::string& message);
void critical(const std::string& message);

void debugf(const char* format, ...);
void infof(const char* format, ...);
void warningf(const char* format, ...);
void errorf(const char* format, ...);

} // namespace Log
