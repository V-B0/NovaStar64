/**
 * @file log.hpp
 * @brief Logging system
 *
 * Provides a thread-safe logging system with file output and log level filtering.
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
        Debug,      ///< Debug information
        Info,       ///< General information
        Warning,    ///< Warning messages
        Error,      ///< Error conditions
        Critical    ///< Critical errors
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

    class Logger {
    public:
        template<typename... Args>
        void debug(const char*, Args&&...) {}
        template<typename... Args>
        void info(const char*, Args&&...) {}
        template<typename... Args>
        void warn(const char*, Args&&...) {}
        template<typename... Args>
        void error(const char*, Args&&...) {}
        template<typename... Args>
        void critical(const char*, Args&&...) {}
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
 * @brief Get the current minimum log level
 * @return Current minimum level
 */
Logging::Level get_min_level();

/**
 * @brief Enable or disable console output
 * @param enable True to enable console output
 */
void enable_console_output(bool enable);

/**
 * @brief Check if console output is enabled
 * @return True if console output is enabled
 */
bool is_console_output_enabled();

/**
 * @brief Open a log file for output
 * @param filename Path to the log file
 * @param append True to append to existing file, false to truncate
 * @return True on success
 */
bool open_log_file(const std::string& filename, bool append = false);

/**
 * @brief Close the log file
 */
void close_log_file();

/**
 * @brief Get the current log filename
 * @return Path to the log file
 */
const std::string& get_log_filename();

} // namespace LoggingConfig

/**
 * @brief Convenience logging functions
 */
namespace Log {

/**
 * @brief Log a debug message
 * @param message Message to log
 */
void debug(const std::string& message);

/**
 * @brief Log an info message
 * @param message Message to log
 */
void info(const std::string& message);

/**
 * @brief Log a warning message
 * @param message Message to log
 */
void warning(const std::string& message);

/**
 * @brief Log an error message
 * @param message Message to log
 */
void error(const std::string& message);

/**
 * @brief Log a critical message
 * @param message Message to log
 */
void critical(const std::string& message);

/**
 * @brief Log a debug message with printf-style formatting
 * @param format Format string
 * @param ... Format arguments
 */
void debugf(const char* format, ...);

/**
 * @brief Log an info message with printf-style formatting
 * @param format Format string
 * @param ... Format arguments
 */
void infof(const char* format, ...);

/**
 * @brief Log a warning message with printf-style formatting
 * @param format Format string
 * @param ... Format arguments
 */
void warningf(const char* format, ...);

/**
 * @brief Log an error message with printf-style formatting
 * @param format Format string
 * @param ... Format arguments
 */
void errorf(const char* format, ...);

/**
 * @brief Log a critical message with printf-style formatting
 * @param format Format string
 * @param ... Format arguments
 */
void criticalf(const char* format, ...);

} // namespace Log