#ifndef PING_UTIL_H
#define PING_UTIL_H

#include <glib.h>

/**
 * Ping result codes
 */
#define PING_SUCCESS          0
#define PING_TIMEOUT         -1
#define PING_DNS_ERROR       -2
#define PING_PERMISSION_ERR  -3
#define PING_PARSE_ERROR     -4
#define PING_EXEC_ERROR      -5

/**
 * Ping a host and return the latency
 *
 * @param hostname Hostname or IP address to ping
 * @param timeout_ms Timeout in milliseconds (typically 1000-5000)
 * @param latency_ms Output parameter for latency in milliseconds
 * @return PING_SUCCESS (0) on success, negative error code on failure
 */
int ping_host(const char *hostname, int timeout_ms, int *latency_ms);

/**
 * Callback for async ping operations
 *
 * @param hostname The hostname that was pinged
 * @param latency_ms Latency in milliseconds, or negative error code
 * @param user_data User-provided data passed to ping_host_async
 */
typedef void (*PingCallback)(const char *hostname, int latency_ms, void *user_data);

/**
 * Ping a host asynchronously
 *
 * @param hostname Hostname or IP address to ping
 * @param timeout_ms Timeout in milliseconds
 * @param callback Function to call when ping completes
 * @param user_data Data to pass to callback
 * @return PING_SUCCESS if ping was started, negative error code on failure
 */
int ping_host_async(const char *hostname, int timeout_ms,
                    PingCallback callback, void *user_data);

/**
 * Extract hostname from "host:port" format
 *
 * @param server_address Address in format "hostname:port" or just "hostname"
 * @return Newly allocated string with hostname only (caller must free), or NULL on error
 */
char* extract_hostname(const char *server_address);

/**
 * Get human-readable error message for ping error code
 *
 * @param error_code Error code from ping_host
 * @return Static error message string
 */
const char* ping_error_string(int error_code);

#endif /* PING_UTIL_H */
