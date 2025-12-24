#ifndef LOGGER_H
#define LOGGER_H

#include <stdbool.h>
#include <stdarg.h>

/**
 * Logger Utility
 *
 * Simple logging system with multiple log levels and optional file output
 */

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO = 1,
    LOG_LEVEL_WARN = 2,
    LOG_LEVEL_ERROR = 3
} LogLevel;

/**
 * Initialize the logger
 *
 * @param log_to_file If true, also log to file
 * @param log_file_path Path to log file (NULL for default: ~/.local/share/ovpn-manager/app.log)
 * @param min_level Minimum log level to output
 * @param use_syslog If true, send WARN and ERROR messages to syslog
 * @return 0 on success, negative on error
 */
int logger_init(bool log_to_file, const char *log_file_path, LogLevel min_level, bool use_syslog);

/**
 * Set the minimum log level
 *
 * @param level New minimum log level
 */
void logger_set_level(LogLevel level);

/**
 * Log a message (generic)
 *
 * @param level Log level
 * @param format Printf-style format string
 * @param ... Format arguments
 */
void logger_log(LogLevel level, const char *format, ...) __attribute__((format(printf, 2, 3)));

/**
 * Log a debug message
 */
void logger_debug(const char *format, ...) __attribute__((format(printf, 1, 2)));

/**
 * Log an info message
 */
void logger_info(const char *format, ...) __attribute__((format(printf, 1, 2)));

/**
 * Log a warning message
 */
void logger_warn(const char *format, ...) __attribute__((format(printf, 1, 2)));

/**
 * Log an error message
 */
void logger_error(const char *format, ...) __attribute__((format(printf, 1, 2)));

/**
 * Clean up logger and close log file
 */
void logger_cleanup(void);

#endif /* LOGGER_H */
