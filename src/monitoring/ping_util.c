#include "ping_util.h"
#include "../utils/logger.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/**
 * Parse ping output to extract average latency
 *
 * Expected format (Linux):
 * "rtt min/avg/max/mdev = 12.345/23.456/34.567/5.678 ms"
 */
static int parse_ping_output(const char *output, int *latency_ms) {
    if (!output || !latency_ms) {
        return PING_PARSE_ERROR;
    }

    /* Look for "time=" pattern (each ping line) or "rtt" pattern (summary) */
    const char *time_str = strstr(output, "time=");
    const char *rtt_str = strstr(output, "rtt min/avg/max/mdev = ");

    if (rtt_str) {
        /* Parse summary line: "rtt min/avg/max/mdev = 12.345/23.456/34.567/5.678 ms" */
        rtt_str += strlen("rtt min/avg/max/mdev = ");

        /* Skip min, extract avg */
        const char *avg_start = strchr(rtt_str, '/');
        if (!avg_start) {
            return PING_PARSE_ERROR;
        }
        avg_start++; /* Move past '/' */

        float avg_ms = 0.0f;
        if (sscanf(avg_start, "%f", &avg_ms) == 1) {
            *latency_ms = (int)(avg_ms + 0.5); /* Round to nearest int */
            return PING_SUCCESS;
        }
    } else if (time_str) {
        /* Parse individual ping line: "64 bytes from ...: icmp_seq=1 ttl=64 time=12.3 ms" */
        time_str += 5; /* Skip "time=" */

        float time_val = 0.0f;
        if (sscanf(time_str, "%f", &time_val) == 1) {
            *latency_ms = (int)(time_val + 0.5);
            return PING_SUCCESS;
        }
    }

    /* Check for common error messages */
    if (strstr(output, "Destination Host Unreachable") ||
        strstr(output, "100% packet loss")) {
        return PING_TIMEOUT;
    }

    if (strstr(output, "unknown host") ||
        strstr(output, "Name or service not known")) {
        return PING_DNS_ERROR;
    }

    return PING_PARSE_ERROR;
}

/**
 * Execute ping command synchronously
 */
int ping_host(const char *hostname, int timeout_ms, int *latency_ms) {
    if (!hostname || !latency_ms) {
        return PING_PARSE_ERROR;
    }

    /* Convert timeout from ms to seconds (ping uses seconds) */
    int timeout_sec = (timeout_ms + 999) / 1000; /* Round up */
    if (timeout_sec < 1) timeout_sec = 1;

    /* Build ping command: ping -c 1 -W <timeout> <hostname> */
    char timeout_str[16];
    snprintf(timeout_str, sizeof(timeout_str), "%d", timeout_sec);

    char *argv[] = {
        "ping",
        "-c", "1",           /* Count: send 1 packet */
        "-W", timeout_str,   /* Timeout in seconds */
        (char *)hostname,
        NULL
    };

    /* Execute ping command */
    gchar *standard_output = NULL;
    gchar *standard_error = NULL;
    gint exit_status = 0;
    GError *error = NULL;

    gboolean success = g_spawn_sync(
        NULL,                /* working directory */
        argv,                /* argv */
        NULL,                /* envp */
        G_SPAWN_SEARCH_PATH, /* flags */
        NULL,                /* child_setup */
        NULL,                /* user_data */
        &standard_output,    /* standard_output */
        &standard_error,     /* standard_error */
        &exit_status,        /* exit_status */
        &error               /* error */
    );

    if (!success) {
        if (error) {
            logger_error("Ping exec error for %s: %s", hostname, error->message);
            g_error_free(error);
        }
        g_free(standard_output);
        g_free(standard_error);

        /* Check if it's a permission error */
        if (error && strstr(error->message, "Permission denied")) {
            return PING_PERMISSION_ERR;
        }
        return PING_EXEC_ERROR;
    }

    /* Parse output */
    int result = PING_PARSE_ERROR;
    if (standard_output) {
        result = parse_ping_output(standard_output, latency_ms);
    }

    /* Check exit status */
    if (exit_status != 0 && result == PING_PARSE_ERROR) {
        /* Ping failed - could be timeout or unreachable */
        result = PING_TIMEOUT;
    }

    g_free(standard_output);
    g_free(standard_error);

    return result;
}

/**
 * Async ping context
 */
typedef struct {
    char *hostname;
    PingCallback callback;
    void *user_data;
    GPid pid;
    gint stdout_fd;
    GIOChannel *stdout_channel;
    guint stdout_watch_id;
    GString *output;
} AsyncPingContext;

/**
 * Free async ping context
 */
static void free_async_ping_context(AsyncPingContext *ctx) {
    if (!ctx) return;

    if (ctx->stdout_watch_id > 0) {
        g_source_remove(ctx->stdout_watch_id);
    }
    if (ctx->stdout_channel) {
        g_io_channel_shutdown(ctx->stdout_channel, FALSE, NULL);
        g_io_channel_unref(ctx->stdout_channel);
    }
    if (ctx->output) {
        g_string_free(ctx->output, TRUE);
    }

    g_free(ctx->hostname);
    g_free(ctx);
}

/**
 * Child watch callback - called when ping process exits
 */
static void on_ping_child_exit(GPid pid, gint status, gpointer user_data) {
    AsyncPingContext *ctx = (AsyncPingContext *)user_data;

    int latency_ms = 0;
    int result = parse_ping_output(ctx->output->str, &latency_ms);

    /* Check exit status */
    if (status != 0 && result == PING_PARSE_ERROR) {
        result = PING_TIMEOUT;
    }

    /* Call user callback */
    if (ctx->callback) {
        ctx->callback(ctx->hostname,
                     result == PING_SUCCESS ? latency_ms : result,
                     ctx->user_data);
    }

    /* Cleanup */
    g_spawn_close_pid(pid);
    free_async_ping_context(ctx);
}

/**
 * IO channel callback - read ping output
 */
static gboolean on_stdout_data(GIOChannel *channel, GIOCondition condition, gpointer user_data) {
    AsyncPingContext *ctx = (AsyncPingContext *)user_data;

    if (condition & G_IO_IN) {
        gchar buffer[256];
        gsize bytes_read = 0;
        GError *error = NULL;

        GIOStatus status = g_io_channel_read_chars(channel, buffer, sizeof(buffer) - 1,
                                                   &bytes_read, &error);

        if (status == G_IO_STATUS_NORMAL && bytes_read > 0) {
            buffer[bytes_read] = '\0';
            g_string_append(ctx->output, buffer);
        }

        if (error) {
            g_error_free(error);
        }
    }

    if (condition & (G_IO_HUP | G_IO_ERR)) {
        ctx->stdout_watch_id = 0;  /* Watch will be auto-removed */
        return FALSE; /* Remove this watch */
    }

    return TRUE; /* Keep watching */
}

/**
 * Ping a host asynchronously
 */
int ping_host_async(const char *hostname, int timeout_ms,
                    PingCallback callback, void *user_data) {
    if (!hostname || !callback) {
        return PING_PARSE_ERROR;
    }

    /* Convert timeout from ms to seconds */
    int timeout_sec = (timeout_ms + 999) / 1000;
    if (timeout_sec < 1) timeout_sec = 1;

    /* Build ping command */
    char timeout_str[16];
    snprintf(timeout_str, sizeof(timeout_str), "%d", timeout_sec);

    char *argv[] = {
        "ping",
        "-c", "1",
        "-W", timeout_str,
        (char *)hostname,
        NULL
    };

    /* Create context */
    AsyncPingContext *ctx = g_malloc0(sizeof(AsyncPingContext));
    ctx->hostname = g_strdup(hostname);
    ctx->callback = callback;
    ctx->user_data = user_data;
    ctx->output = g_string_new("");

    /* Spawn process */
    GError *error = NULL;
    gboolean success = g_spawn_async_with_pipes(
        NULL,                /* working directory */
        argv,                /* argv */
        NULL,                /* envp */
        G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, /* flags */
        NULL,                /* child_setup */
        NULL,                /* user_data */
        &ctx->pid,           /* child_pid */
        NULL,                /* stdin */
        &ctx->stdout_fd,     /* stdout */
        NULL,                /* stderr */
        &error               /* error */
    );

    if (!success) {
        if (error) {
            logger_error("Async ping spawn error for %s: %s", hostname, error->message);
            g_error_free(error);
        }
        free_async_ping_context(ctx);
        return PING_EXEC_ERROR;
    }

    /* Set up IO channel to read stdout */
    ctx->stdout_channel = g_io_channel_unix_new(ctx->stdout_fd);
    g_io_channel_set_flags(ctx->stdout_channel, G_IO_FLAG_NONBLOCK, NULL);
    ctx->stdout_watch_id = g_io_add_watch(ctx->stdout_channel,
                                         G_IO_IN | G_IO_HUP | G_IO_ERR,
                                         on_stdout_data,
                                         ctx);

    /* Watch for process exit */
    g_child_watch_add(ctx->pid, on_ping_child_exit, ctx);

    return PING_SUCCESS;
}

/**
 * Extract hostname from "host:port" format
 */
char* extract_hostname(const char *server_address) {
    if (!server_address) {
        return NULL;
    }

    /* Find colon separator */
    const char *colon = strchr(server_address, ':');

    if (colon) {
        /* Extract hostname part before colon */
        size_t hostname_len = colon - server_address;
        char *hostname = g_malloc(hostname_len + 1);
        strncpy(hostname, server_address, hostname_len);
        hostname[hostname_len] = '\0';
        return hostname;
    } else {
        /* No port, return copy of full address */
        return g_strdup(server_address);
    }
}

/**
 * Get human-readable error message for ping error code
 */
const char* ping_error_string(int error_code) {
    switch (error_code) {
        case PING_SUCCESS:
            return "Success";
        case PING_TIMEOUT:
            return "Timeout";
        case PING_DNS_ERROR:
            return "DNS Error";
        case PING_PERMISSION_ERR:
            return "Permission Denied";
        case PING_PARSE_ERROR:
            return "Parse Error";
        case PING_EXEC_ERROR:
            return "Execution Error";
        default:
            return "Unknown Error";
    }
}
