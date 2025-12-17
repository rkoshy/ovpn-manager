#ifndef SESSION_CLIENT_H
#define SESSION_CLIENT_H

#include <systemd/sd-bus.h>
#include <stdbool.h>
#include <stdint.h>
#include <glib.h>

/**
 * Session Client
 *
 * Manages OpenVPN3 VPN sessions via D-Bus
 */

/* VPN session states */
typedef enum {
    SESSION_STATE_DISCONNECTED = 0,
    SESSION_STATE_CONNECTING = 1,
    SESSION_STATE_CONNECTED = 2,
    SESSION_STATE_RECONNECTING = 3,
    SESSION_STATE_PAUSED = 4,
    SESSION_STATE_ERROR = 5,
    SESSION_STATE_AUTH_REQUIRED = 6
} SessionState;

/* VPN session information */
typedef struct {
    char *session_path;      /* D-Bus object path */
    char *config_name;       /* VPN config name */
    char *device_name;       /* Network device (e.g., tun0) */
    char *remote_host;       /* Connected to host:port */
    SessionState state;      /* Connection state */
    char *status_message;    /* Human-readable status */
    pid_t backend_pid;       /* Backend process PID */
    uint64_t session_created; /* Unix timestamp when session was created */
} VpnSession;

/**
 * List all active VPN sessions
 *
 * @param bus D-Bus connection
 * @param sessions Output array of VpnSession pointers
 * @param count Output count of sessions
 * @return 0 on success, negative on error
 */
int session_list(sd_bus *bus, VpnSession ***sessions, unsigned int *count);

/**
 * Get detailed session information
 *
 * @param bus D-Bus connection
 * @param session_path D-Bus object path of session
 * @return VpnSession structure on success, NULL on failure
 */
VpnSession* session_get_info(sd_bus *bus, const char *session_path);

/**
 * Disconnect a VPN session
 *
 * @param bus D-Bus connection
 * @param session_path D-Bus object path of session
 * @return 0 on success, negative on error
 */
int session_disconnect(sd_bus *bus, const char *session_path);

/**
 * Pause a VPN session
 *
 * @param bus D-Bus connection
 * @param session_path D-Bus object path of session
 * @param reason Reason for pausing
 * @return 0 on success, negative on error
 */
int session_pause(sd_bus *bus, const char *session_path, const char *reason);

/**
 * Resume a paused VPN session
 *
 * @param bus D-Bus connection
 * @param session_path D-Bus object path of session
 * @return 0 on success, negative on error
 */
int session_resume(sd_bus *bus, const char *session_path);

/**
 * Start a new VPN session from a configuration
 *
 * @param bus D-Bus connection
 * @param config_path D-Bus object path of configuration
 * @param session_path Output: D-Bus object path of created session
 * @return 0 on success, negative on error
 */
int session_start(sd_bus *bus, const char *config_path, char **session_path);

/**
 * Free a VPN session structure
 *
 * @param session VpnSession to free
 */
void session_free(VpnSession *session);

/**
 * Free an array of VPN sessions
 *
 * @param sessions Array of VpnSession pointers
 * @param count Number of sessions
 */
void session_list_free(VpnSession **sessions, unsigned int count);

/**
 * Check if session requires authentication and get auth URL
 *
 * @param bus D-Bus connection
 * @param session_path D-Bus object path of session
 * @param auth_url Output: Authentication URL (caller must free)
 * @return 0 if auth required and URL retrieved, negative otherwise
 */
int session_get_auth_url(sd_bus *bus, const char *session_path, char **auth_url);

/**
 * Get session statistics (bytes in/out, packets, etc.)
 *
 * @param bus D-Bus connection
 * @param session_path D-Bus object path of session
 * @param bytes_in Output: Bytes received
 * @param bytes_out Output: Bytes sent
 * @param packets_in Output: Packets received
 * @param packets_out Output: Packets sent
 * @return 0 on success, negative on error
 */
int session_get_statistics(sd_bus *bus, const char *session_path,
                          uint64_t *bytes_in, uint64_t *bytes_out,
                          uint64_t *packets_in, uint64_t *packets_out);

#endif /* SESSION_CLIENT_H */
