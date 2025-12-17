#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <glib.h>

/* Logger state */
static struct {
    bool initialized;
    bool log_to_file;
    FILE *log_file;
    LogLevel min_level;
    GMutex mutex;  /* For thread safety */
} logger_state = {
    .initialized = false,
    .log_to_file = false,
    .log_file = NULL,
    .min_level = LOG_LEVEL_INFO,
};

/* Log level names */
static const char *log_level_names[] = {
    "DEBUG",
    "INFO",
    "WARN",
    "ERROR"
};

/* Log level colors (ANSI escape codes) */
static const char *log_level_colors[] = {
    "\033[0;36m",  /* Cyan for DEBUG */
    "\033[0;32m",  /* Green for INFO */
    "\033[0;33m",  /* Yellow for WARN */
    "\033[0;31m"   /* Red for ERROR */
};

#define COLOR_RESET "\033[0m"

/**
 * Get current timestamp as string
 */
static void get_timestamp(char *buffer, size_t buffer_size) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buffer, buffer_size, "%Y-%m-%d %H:%M:%S", tm_info);
}

/**
 * Ensure log directory exists
 */
static int ensure_log_directory(const char *log_path) {
    char *dir_path = g_path_get_dirname(log_path);
    int ret = 0;

    /* Try to create directory (recursively) */
    if (g_mkdir_with_parents(dir_path, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "Failed to create log directory %s: %s\n",
                dir_path, strerror(errno));
        ret = -1;
    }

    g_free(dir_path);
    return ret;
}

/**
 * Initialize the logger
 */
int logger_init(bool log_to_file, const char *log_file_path, LogLevel min_level) {
    char *default_log_path = NULL;

    if (logger_state.initialized) {
        fprintf(stderr, "Logger already initialized\n");
        return -1;
    }

    /* Initialize mutex */
    g_mutex_init(&logger_state.mutex);

    logger_state.min_level = min_level;
    logger_state.log_to_file = log_to_file;

    if (log_to_file) {
        /* Determine log file path */
        if (log_file_path) {
            default_log_path = g_strdup(log_file_path);
        } else {
            /* Default: ~/.local/share/ovpn-manager/app.log */
            const char *home = g_get_home_dir();
            default_log_path = g_build_filename(
                home, ".local", "share", "ovpn-manager", "app.log", NULL
            );
        }

        /* Ensure directory exists */
        if (ensure_log_directory(default_log_path) != 0) {
            g_free(default_log_path);
            g_mutex_clear(&logger_state.mutex);
            return -1;
        }

        /* Open log file */
        logger_state.log_file = fopen(default_log_path, "a");
        if (!logger_state.log_file) {
            fprintf(stderr, "Failed to open log file %s: %s\n",
                    default_log_path, strerror(errno));
            g_free(default_log_path);
            g_mutex_clear(&logger_state.mutex);
            return -1;
        }

        fprintf(stderr, "Logging to file: %s\n", default_log_path);
        g_free(default_log_path);
    }

    logger_state.initialized = true;

    logger_info("Logger initialized (min_level=%s, log_to_file=%s)",
                log_level_names[min_level],
                log_to_file ? "yes" : "no");

    return 0;
}

/**
 * Set the minimum log level
 */
void logger_set_level(LogLevel level) {
    g_mutex_lock(&logger_state.mutex);
    logger_state.min_level = level;
    g_mutex_unlock(&logger_state.mutex);
}

/**
 * Log a message (generic)
 */
void logger_log(LogLevel level, const char *format, ...) {
    va_list args;
    char timestamp[32];
    bool use_color = true;

    if (!logger_state.initialized) {
        return;
    }

    /* Check log level */
    if (level < logger_state.min_level) {
        return;
    }

    g_mutex_lock(&logger_state.mutex);

    /* Get timestamp */
    get_timestamp(timestamp, sizeof(timestamp));

    /* Log to stderr (with color) */
    if (use_color) {
        fprintf(stderr, "%s[%s] [%s%s%s] ",
                timestamp,
                "ovpn-manager",
                log_level_colors[level],
                log_level_names[level],
                COLOR_RESET);
    } else {
        fprintf(stderr, "%s [%s] [%s] ",
                timestamp,
                "ovpn-manager",
                log_level_names[level]);
    }

    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    fprintf(stderr, "\n");

    /* Log to file (without color) */
    if (logger_state.log_to_file && logger_state.log_file) {
        fprintf(logger_state.log_file, "%s [%s] [%s] ",
                timestamp,
                "ovpn-manager",
                log_level_names[level]);

        va_start(args, format);
        vfprintf(logger_state.log_file, format, args);
        va_end(args);

        fprintf(logger_state.log_file, "\n");
        fflush(logger_state.log_file);
    }

    g_mutex_unlock(&logger_state.mutex);
}

/**
 * Log a debug message
 */
void logger_debug(const char *format, ...) {
    va_list args;
    va_start(args, format);

    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    logger_log(LOG_LEVEL_DEBUG, "%s", buffer);

    va_end(args);
}

/**
 * Log an info message
 */
void logger_info(const char *format, ...) {
    va_list args;
    va_start(args, format);

    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    logger_log(LOG_LEVEL_INFO, "%s", buffer);

    va_end(args);
}

/**
 * Log a warning message
 */
void logger_warn(const char *format, ...) {
    va_list args;
    va_start(args, format);

    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    logger_log(LOG_LEVEL_WARN, "%s", buffer);

    va_end(args);
}

/**
 * Log an error message
 */
void logger_error(const char *format, ...) {
    va_list args;
    va_start(args, format);

    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    logger_log(LOG_LEVEL_ERROR, "%s", buffer);

    va_end(args);
}

/**
 * Clean up logger and close log file
 */
void logger_cleanup(void) {
    if (!logger_state.initialized) {
        return;
    }

    g_mutex_lock(&logger_state.mutex);

    logger_info("Logger shutting down");

    /* Close log file */
    if (logger_state.log_file) {
        fclose(logger_state.log_file);
        logger_state.log_file = NULL;
    }

    logger_state.initialized = false;
    logger_state.log_to_file = false;

    g_mutex_unlock(&logger_state.mutex);
    g_mutex_clear(&logger_state.mutex);
}
