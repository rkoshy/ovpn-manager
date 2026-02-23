#include "session_client.h"
#include "config_client.h"
#include "signal_handlers.h"
#include "../utils/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>

#define OPENVPN3_SERVICE_SESSIONS "net.openvpn.v3.sessions"
#define OPENVPN3_INTERFACE_SESSIONS "net.openvpn.v3.sessions"
#define OPENVPN3_INTERFACE_SESSION "net.openvpn.v3.sessions"

/**
 * Free a VPN session structure
 */
void session_free(VpnSession *session) {
    if (!session) {
        return;
    }

    g_free(session->session_path);
    g_free(session->config_name);
    g_free(session->device_name);
    g_free(session->remote_host);
    g_free(session->status_message);
    g_free(session);
}

/**
 * Free an array of VPN sessions
 */
void session_list_free(VpnSession **sessions, unsigned int count) {
    if (!sessions) {
        return;
    }

    for (unsigned int i = 0; i < count; i++) {
        session_free(sessions[i]);
    }

    g_free(sessions);
}

/**
 * Get a string property from D-Bus object
 */
static char* get_string_property(sd_bus *bus, const char *path,
                                   const char *interface, const char *property) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *value = NULL;
    char *result = NULL;
    int r;

    r = sd_bus_get_property(
        bus,
        OPENVPN3_SERVICE_SESSIONS,
        path,
        interface,
        property,
        &error,
        &reply,
        "s"
    );

    if (r < 0) {
        sd_bus_error_free(&error);
        return NULL;
    }

    r = sd_bus_message_read(reply, "s", &value);
    if (r >= 0 && value) {
        result = g_strdup(value);
    }

    sd_bus_message_unref(reply);
    return result;
}

/**
 * Get status property (struct with major, minor, message)
 */
static int get_status_property(sd_bus *bus, const char *path,
                                const char *interface,
                                unsigned int *major, unsigned int *minor,
                                char **message) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *msg = NULL;
    int r;

    r = sd_bus_get_property(
        bus,
        OPENVPN3_SERVICE_SESSIONS,
        path,
        interface,
        "status",
        &error,
        &reply,
        "(uus)"
    );

    if (r < 0) {
        logger_error("Failed to get status property: %s",
                error.message ? error.message : strerror(-r));
        sd_bus_error_free(&error);
        return r;
    }

    r = sd_bus_message_read(reply, "(uus)", major, minor, &msg);
    if (r >= 0 && msg) {
        *message = g_strdup(msg);
    } else {
        *message = NULL;
    }

    sd_bus_message_unref(reply);
    return r;
}

/**
 * Check if session has connected_to property (indicates active connection)
 */
static bool is_session_connected(sd_bus *bus, const char *path,
                                  const char *interface) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *protocol = NULL;
    const char *host = NULL;
    unsigned int port = 0;
    int r;

    r = sd_bus_get_property(
        bus,
        OPENVPN3_SERVICE_SESSIONS,
        path,
        interface,
        "connected_to",
        &error,
        &reply,
        "(ssu)"
    );

    if (r < 0) {
        sd_bus_error_free(&error);
        return false;
    }

    r = sd_bus_message_read(reply, "(ssu)", &protocol, &host, &port);
    sd_bus_message_unref(reply);

    /* If we got valid connection info, session is connected */
    return (r >= 0 && protocol && host && port > 0);
}

/**
 * Get an unsigned integer property from D-Bus object
 */
static unsigned int get_uint_property(sd_bus *bus, const char *path,
                                       const char *interface, const char *property) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    unsigned int value = 0;
    int r;

    r = sd_bus_get_property(
        bus,
        OPENVPN3_SERVICE_SESSIONS,
        path,
        interface,
        property,
        &error,
        &reply,
        "u"
    );

    if (r < 0) {
        sd_bus_error_free(&error);
        return 0;
    }

    sd_bus_message_read(reply, "u", &value);
    sd_bus_message_unref(reply);

    return value;
}

/**
 * Get a uint64 property from D-Bus object (for timestamps)
 */
static uint64_t get_uint64_property(sd_bus *bus, const char *path,
                                     const char *interface, const char *property) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    uint64_t value = 0;
    int r;

    r = sd_bus_get_property(
        bus,
        OPENVPN3_SERVICE_SESSIONS,
        path,
        interface,
        property,
        &error,
        &reply,
        "t"
    );

    if (r < 0) {
        sd_bus_error_free(&error);
        return 0;
    }

    sd_bus_message_read(reply, "t", &value);
    sd_bus_message_unref(reply);

    return value;
}

/**
 * Get detailed session information
 */
VpnSession* session_get_info(sd_bus *bus, const char *session_path) {
    VpnSession *session = NULL;

    if (!bus || !session_path) {
        return NULL;
    }

    session = g_malloc0(sizeof(VpnSession));
    if (!session) {
        return NULL;
    }

    /* Store session path */
    session->session_path = g_strdup(session_path);

    /* Get properties */
    session->config_name = get_string_property(
        bus, session_path, OPENVPN3_INTERFACE_SESSION, "config_name"
    );

    session->device_name = get_string_property(
        bus, session_path, OPENVPN3_INTERFACE_SESSION, "device_name"
    );

    /* Get backend PID */
    session->backend_pid = get_uint_property(
        bus, session_path, OPENVPN3_INTERFACE_SESSION, "backend_pid"
    );

    /* Get session creation timestamp */
    session->session_created = get_uint64_property(
        bus, session_path, OPENVPN3_INTERFACE_SESSION, "session_created"
    );

    /* Get status (major, minor, message) */
    unsigned int major = 0, minor = 0;
    get_status_property(bus, session_path, OPENVPN3_INTERFACE_SESSION,
                        &major, &minor, &session->status_message);

    /* Check if session is actually connected using connected_to property */
    bool connected = is_session_connected(bus, session_path, OPENVPN3_INTERFACE_SESSION);

    /* Verbose logging (optional) */
    #ifdef VERBOSE_DEBUG
    logger_debug("Session %s - major=%u, minor=%u, connected=%s, msg='%s'",
           session->config_name ? session->config_name : "unknown",
           major, minor, connected ? "yes" : "no",
           session->status_message ? session->status_message : "(null)");
    #endif

    /* Check if session requires authentication */
    char *auth_url = NULL;
    int auth_check = session_get_auth_url(bus, session_path, &auth_url);
    bool needs_auth = (auth_check == 0);

    if (logger_get_verbosity() >= 1) {
        logger_debug("Session %s", session->config_name ? session->config_name : "unknown");
        logger_debug("  Status message: '%s'", session->status_message ? session->status_message : "(null)");
        logger_debug("  Auth check result: %d (0=has auth, negative=no auth)", auth_check);
        logger_debug("  Auth URL: %s", auth_url ? auth_url : "(null)");
        logger_debug("  Connected: %s", connected ? "yes" : "no");
        logger_debug("  Status codes: major=%u, minor=%u", major, minor);
    }

    if (auth_url) {
        g_free(auth_url);  /* We just needed to check if auth is required */
    }

    /* Determine session state
     * Priority: 1) auth required, 2) paused, 3) connected_to property,
     *           4) error messages, 5) status codes, 6) other messages
     *
     * IMPORTANT: Paused must be checked before connected_to because a paused
     * session still has valid connection metadata (protocol, host, port) in
     * the connected_to D-Bus property — checking connected first would
     * incorrectly report CONNECTED instead of PAUSED.
     *
     * OpenVPN3 reports paused sessions as major=2 ("Connection") with
     * minor=13 (CONN_PAUSING) or minor=14 (CONN_PAUSED). The status
     * message text is often empty, so we must check the minor code.
     *
     * OpenVPN3 StatusMinor codes (major=2 CONNECTION):
     *   7  = CONN_CONNECTED
     *  13  = CONN_PAUSING
     *  14  = CONN_PAUSED
     *  15  = CONN_RESUMING
     */
    bool is_paused = (major == 4) ||
                     (major == 2 && (minor == 13 || minor == 14));

    if (needs_auth) {
        /* Session is waiting for authentication */
        if (logger_get_verbosity() >= 1) {
            logger_debug("  -> Setting state: AUTH_REQUIRED");
        }
        session->state = SESSION_STATE_AUTH_REQUIRED;
    } else if (is_paused) {
        /* PAUSED — must be before connected check */
        if (logger_get_verbosity() >= 1) {
            logger_debug("  -> Setting state: PAUSED (major=%u, msg='%s')",
                         major, session->status_message ? session->status_message : "");
        }
        session->state = SESSION_STATE_PAUSED;
    } else if (connected) {
        if (logger_get_verbosity() >= 1) {
            logger_debug("  -> Setting state: CONNECTED");
        }
        /* Session has active connection */
        session->state = SESSION_STATE_CONNECTED;
    } else if (session->status_message &&
               (strstr(session->status_message, "failed") ||
                strstr(session->status_message, "Failed") ||
                strstr(session->status_message, "Error"))) {
        /* Check for failure/error messages FIRST, before status codes */
        if (logger_get_verbosity() >= 1) {
            logger_debug("  -> Setting state: ERROR (failed/error in message)");
        }
        session->state = SESSION_STATE_ERROR;
    } else if (major == 2) {
        /* CONNECTING status */
        if (logger_get_verbosity() >= 1) {
            logger_debug("  -> Setting state: CONNECTING (major=2)");
        }
        session->state = SESSION_STATE_CONNECTING;
    } else if (session->status_message) {
        /* Fallback to message parsing for other states */
        if (strstr(session->status_message, "authentication required") ||
            strstr(session->status_message, "Web authentication") ||
            strstr(session->status_message, "https://")) {
            if (logger_get_verbosity() >= 1) {
                logger_debug("  -> Setting state: AUTH_REQUIRED (from message)");
            }
            session->state = SESSION_STATE_AUTH_REQUIRED;
        } else if (strstr(session->status_message, "Connecting")) {
            if (logger_get_verbosity() >= 1) {
                logger_debug("  -> Setting state: CONNECTING (from message)");
            }
            session->state = SESSION_STATE_CONNECTING;
        } else if (strstr(session->status_message, "Reconnecting")) {
            if (logger_get_verbosity() >= 1) {
                logger_debug("  -> Setting state: RECONNECTING (from message)");
            }
            session->state = SESSION_STATE_RECONNECTING;
        } else if (strstr(session->status_message, "Paused")) {
            if (logger_get_verbosity() >= 1) {
                logger_debug("  -> Setting state: PAUSED (from message)");
            }
            session->state = SESSION_STATE_PAUSED;
        } else {
            if (logger_get_verbosity() >= 1) {
                logger_debug("  -> Setting state: DISCONNECTED (default from message)");
            }
            session->state = SESSION_STATE_DISCONNECTED;
        }
    } else {
        if (logger_get_verbosity() >= 1) {
            logger_debug("  -> Setting state: DISCONNECTED (no message)");
        }
        session->state = SESSION_STATE_DISCONNECTED;
    }

    return session;
}

/**
 * List all active VPN sessions
 */
int session_list(sd_bus *bus, VpnSession ***sessions, unsigned int *count) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r;
    const char *path;
    GPtrArray *session_array = NULL;

    if (!bus || !sessions || !count) {
        return -EINVAL;
    }

    *sessions = NULL;
    *count = 0;

    /* Create temporary array */
    session_array = g_ptr_array_new();

    /* Call FetchAvailableSessions method */
    r = sd_bus_call_method(
        bus,
        OPENVPN3_SERVICE_SESSIONS,
        "/net/openvpn/v3/sessions",
        OPENVPN3_INTERFACE_SESSIONS,
        "FetchAvailableSessions",
        &error,
        &reply,
        ""
    );

    if (r < 0) {
        logger_error("Failed to fetch sessions: %s",
                error.message ? error.message : strerror(-r));
        sd_bus_error_free(&error);
        g_ptr_array_free(session_array, TRUE);
        return r;
    }

    /* Parse array of object paths */
    r = sd_bus_message_enter_container(reply, 'a', "o");
    if (r < 0) {
        logger_error("Failed to enter container: %s", strerror(-r));
        sd_bus_message_unref(reply);
        g_ptr_array_free(session_array, TRUE);
        return r;
    }

    /* Read each session path */
    while ((r = sd_bus_message_read(reply, "o", &path)) > 0) {
        VpnSession *session = session_get_info(bus, path);
        if (session) {
            g_ptr_array_add(session_array, session);
        }
    }

    sd_bus_message_exit_container(reply);
    sd_bus_message_unref(reply);

    /* Convert to array */
    *count = session_array->len;
    if (*count > 0) {
        *sessions = g_malloc(sizeof(VpnSession*) * (*count));
        for (unsigned int i = 0; i < *count; i++) {
            (*sessions)[i] = g_ptr_array_index(session_array, i);
        }
    }

    g_ptr_array_free(session_array, TRUE);

    return 0;
}

/**
 * Disconnect a VPN session
 */
int session_disconnect(sd_bus *bus, const char *session_path) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int r;

    if (!bus || !session_path) {
        return -EINVAL;
    }

    r = sd_bus_call_method(
        bus,
        OPENVPN3_SERVICE_SESSIONS,
        session_path,
        OPENVPN3_INTERFACE_SESSION,
        "Disconnect",
        &error,
        NULL,
        ""
    );

    if (r < 0) {
        logger_error("Failed to disconnect session: %s",
                error.message ? error.message : strerror(-r));
        sd_bus_error_free(&error);
        return r;
    }

    return 0;
}

/**
 * Pause a VPN session
 */
int session_pause(sd_bus *bus, const char *session_path, const char *reason) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int r;

    if (!bus || !session_path) {
        return -EINVAL;
    }

    r = sd_bus_call_method(
        bus,
        OPENVPN3_SERVICE_SESSIONS,
        session_path,
        OPENVPN3_INTERFACE_SESSION,
        "Pause",
        &error,
        NULL,
        "s",
        reason ? reason : "User requested"
    );

    if (r < 0) {
        logger_error("Failed to pause session: %s",
                error.message ? error.message : strerror(-r));
        sd_bus_error_free(&error);
        return r;
    }

    return 0;
}

/**
 * Resume a paused VPN session
 */
int session_resume(sd_bus *bus, const char *session_path) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int r;

    if (!bus || !session_path) {
        return -EINVAL;
    }

    r = sd_bus_call_method(
        bus,
        OPENVPN3_SERVICE_SESSIONS,
        session_path,
        OPENVPN3_INTERFACE_SESSION,
        "Resume",
        &error,
        NULL,
        ""
    );

    if (r < 0) {
        logger_error("Failed to resume session: %s",
                error.message ? error.message : strerror(-r));
        sd_bus_error_free(&error);
        return r;
    }

    return 0;
}

/**
 * Check if session requires authentication and get auth URL
 */
int session_get_auth_url(sd_bus *bus, const char *session_path, char **auth_url) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r;

    if (logger_get_verbosity() >= 1) {
        logger_debug("session_get_auth_url called for %s", session_path);
    }

    if (!bus || !session_path || !auth_url) {
        return -EINVAL;
    }

    *auth_url = NULL;

    /* Check if there are pending user input requests */
    r = sd_bus_call_method(
        bus,
        OPENVPN3_SERVICE_SESSIONS,
        session_path,
        OPENVPN3_INTERFACE_SESSION,
        "UserInputQueueGetTypeGroup",
        &error,
        &reply,
        ""
    );

    if (r < 0) {
        logger_debug("session_get_auth_url: UserInputQueueGetTypeGroup failed: %s",
               error.message ? error.message : strerror(-r));
        sd_bus_error_free(&error);
        return r;
    }

    /* Parse response - array of (type, group) tuples */
    r = sd_bus_message_enter_container(reply, 'a', "(uu)");
    if (r < 0) {
        sd_bus_message_unref(reply);
        return r;
    }

    unsigned int type, group;
    bool found_auth = false;

    while ((r = sd_bus_message_read(reply, "(uu)", &type, &group)) > 0) {
        /* Type 1 = Web authentication */
        if (type == 1) {
            found_auth = true;
            break;
        }
    }

    sd_bus_message_exit_container(reply);
    sd_bus_message_unref(reply);

    if (!found_auth) {
        if (logger_get_verbosity() >= 1) {
            logger_debug("session_get_auth_url: No auth requests found in queue");
        }
        return -ENOENT;
    }

    if (logger_get_verbosity() >= 1) {
        logger_debug("session_get_auth_url: Found auth request type=%u, group=%u", type, group);
    }

    /* Get the list of request IDs for this type/group */
    sd_bus_error_free(&error);
    reply = NULL;

    r = sd_bus_call_method(
        bus,
        OPENVPN3_SERVICE_SESSIONS,
        session_path,
        OPENVPN3_INTERFACE_SESSION,
        "UserInputQueueCheck",
        &error,
        &reply,
        "uu",
        type, group
    );

    if (r < 0) {
        logger_debug("UserInputQueueCheck failed: %s",
               error.message ? error.message : strerror(-r));
        sd_bus_error_free(&error);
        return r;
    }

    /* Parse response - array of IDs */
    r = sd_bus_message_enter_container(reply, 'a', "u");
    if (r < 0) {
        sd_bus_message_unref(reply);
        return r;
    }

    unsigned int req_id;
    bool found_id = false;

    /* Get the first ID */
    if ((r = sd_bus_message_read(reply, "u", &req_id)) > 0) {
        found_id = true;
        logger_debug("Found request ID: %u", req_id);
    }

    sd_bus_message_exit_container(reply);
    sd_bus_message_unref(reply);

    if (!found_id) {
        logger_debug("No request IDs found");
        return -ENOENT;
    }

    /* Now fetch the authentication details with the ID */
    sd_bus_error_free(&error);
    reply = NULL;

    r = sd_bus_call_method(
        bus,
        OPENVPN3_SERVICE_SESSIONS,
        session_path,
        OPENVPN3_INTERFACE_SESSION,
        "UserInputQueueFetch",
        &error,
        &reply,
        "uuu",
        type, group, req_id
    );

    if (r < 0) {
        logger_debug("UserInputQueueFetch failed: %s",
               error.message ? error.message : strerror(-r));
        sd_bus_error_free(&error);
        return r;
    }

    /* Parse response: (type, group, id, name, description, hidden_input, masked_input) */
    unsigned int ret_type, ret_group, ret_id;
    const char *name, *description, *hidden_input;
    int masked_input;

    r = sd_bus_message_read(reply, "uuusssb",
                            &ret_type, &ret_group, &ret_id,
                            &name, &description, &hidden_input,
                            &masked_input);

    logger_debug("UserInputQueueFetch returned: type=%u, group=%u, id=%u, description='%s'",
           ret_type, ret_group, ret_id, description ? description : "(null)");

    if (r >= 0 && description && strstr(description, "http") == description) {
        *auth_url = g_strdup(description);
        logger_debug("Got auth URL: %s", *auth_url);
        sd_bus_message_unref(reply);
        return 0;
    }

    sd_bus_message_unref(reply);
    return -ENODATA;
}

/**
 * Disconnect any existing sessions for the same config.
 * Prevents duplicate connections on the same profile.
 */
static void disconnect_existing_sessions(sd_bus *bus, const char *config_path) {
    if (!bus || !config_path) {
        return;
    }

    /* Get config name from config path */
    VpnConfig *config = config_get_info(bus, config_path);
    if (!config || !config->config_name) {
        config_free(config);
        return;
    }

    /* List active sessions */
    VpnSession **sessions = NULL;
    unsigned int count = 0;
    int r = session_list(bus, &sessions, &count);
    if (r < 0 || !sessions || count == 0) {
        config_free(config);
        return;
    }

    /* Disconnect any session whose config_name matches */
    for (unsigned int i = 0; i < count; i++) {
        if (sessions[i]->config_name &&
            strcmp(sessions[i]->config_name, config->config_name) == 0) {
            logger_warn("Disconnected existing session for '%s' (path=%s) before reconnect",
                        config->config_name, sessions[i]->session_path);
            session_disconnect(bus, sessions[i]->session_path);
        }
    }

    session_list_free(sessions, count);
    config_free(config);
}

/**
 * Start a new VPN session from a configuration
 */
int session_start(sd_bus *bus, const char *config_path, char **session_path) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *path = NULL;
    int r;

    if (!bus || !config_path || !session_path) {
        return -EINVAL;
    }

    *session_path = NULL;

    /* Disconnect any existing sessions for this config to prevent duplicates */
    disconnect_existing_sessions(bus, config_path);

    /* Call NewTunnel method to create session from config */
    r = sd_bus_call_method(
        bus,
        OPENVPN3_SERVICE_SESSIONS,
        "/net/openvpn/v3/sessions",
        OPENVPN3_INTERFACE_SESSIONS,
        "NewTunnel",
        &error,
        &reply,
        "o",
        config_path
    );

    if (r < 0) {
        logger_error("Failed to create session: %s",
                error.message ? error.message : strerror(-r));
        sd_bus_error_free(&error);
        return r;
    }

    /* Parse session path from reply */
    r = sd_bus_message_read(reply, "o", &path);
    if (r < 0 || !path) {
        logger_error("Failed to read session path: %s", strerror(-r));
        sd_bus_message_unref(reply);
        return -EIO;
    }

    *session_path = g_strdup(path);
    sd_bus_message_unref(reply);

    logger_info("Created session: %s", *session_path);

    /* Now connect the session */
    sd_bus_error_free(&error);
    r = sd_bus_call_method(
        bus,
        OPENVPN3_SERVICE_SESSIONS,
        *session_path,
        OPENVPN3_INTERFACE_SESSION,
        "Connect",
        &error,
        NULL,
        ""
    );

    if (r < 0) {
        logger_error("Failed to connect session: %s",
                error.message ? error.message : strerror(-r));
        sd_bus_error_free(&error);
        return r;
    }

    logger_info("Connected session: %s", *session_path);

    /* Subscribe to AttentionRequired signals for OAuth detection */
    r = signals_subscribe_attention_required(bus, *session_path);
    if (r < 0) {
        logger_warn("Failed to subscribe to authentication signals");
    }

    return 0;
}

/**
 * Get session statistics
 */
int session_get_statistics(sd_bus *bus, const char *session_path,
                          uint64_t *bytes_in, uint64_t *bytes_out,
                          uint64_t *packets_in, uint64_t *packets_out) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r;

    if (!bus || !session_path) {
        return -EINVAL;
    }

    /* Initialize outputs */
    if (bytes_in) *bytes_in = 0;
    if (bytes_out) *bytes_out = 0;
    if (packets_in) *packets_in = 0;
    if (packets_out) *packets_out = 0;

    /* Get statistics property - it's a dictionary a{sx} where:
     * s = key name (e.g., "BYTES_IN", "BYTES_OUT", "PACKETS_IN", "PACKETS_OUT",
     *                     "TUN_BYTES_IN", "TUN_BYTES_OUT", "TUN_PACKETS_IN", "TUN_PACKETS_OUT")
     * x = int64 value
     */
    r = sd_bus_get_property(
        bus,
        OPENVPN3_SERVICE_SESSIONS,
        session_path,
        OPENVPN3_INTERFACE_SESSION,
        "statistics",
        &error,
        &reply,
        "a{sx}"
    );

    if (r < 0) {
        /* Statistics property may not be available on all OpenVPN3 versions */
        sd_bus_error_free(&error);
        return -ENOTSUP;
    }

    /* Parse the dictionary of statistics */
    r = sd_bus_message_enter_container(reply, 'a', "{sx}");
    if (r < 0) {
        sd_bus_message_unref(reply);
        return r;
    }

    /* Iterate through statistics */
    while (sd_bus_message_enter_container(reply, 'e', "sx") > 0) {
        const char *key;
        int64_t value;

        r = sd_bus_message_read(reply, "sx", &key, &value);
        if (r < 0) {
            sd_bus_message_exit_container(reply);
            sd_bus_message_exit_container(reply);
            sd_bus_message_unref(reply);
            return r;
        }

        /* Match known statistics keys */
        if (strcmp(key, "BYTES_IN") == 0 && bytes_in) {
            *bytes_in = (uint64_t)value;
        } else if (strcmp(key, "BYTES_OUT") == 0 && bytes_out) {
            *bytes_out = (uint64_t)value;
        } else if (strcmp(key, "PACKETS_IN") == 0 && packets_in) {
            *packets_in = (uint64_t)value;
        } else if (strcmp(key, "PACKETS_OUT") == 0 && packets_out) {
            *packets_out = (uint64_t)value;
        }

        sd_bus_message_exit_container(reply);
    }

    sd_bus_message_exit_container(reply);
    sd_bus_message_unref(reply);

    return 0;
}

/**
 * Force-disconnect all active sessions (cleanup for stuck sessions)
 */
int session_cleanup_all(sd_bus *bus, unsigned int *out_total, unsigned int *out_cleaned) {
    if (!bus || !out_total || !out_cleaned) {
        return -1;
    }

    *out_total = 0;
    *out_cleaned = 0;

    VpnSession **sessions = NULL;
    unsigned int count = 0;

    int r = session_list(bus, &sessions, &count);
    if (r < 0 || !sessions || count == 0) {
        return 0;  /* Nothing to clean up */
    }

    *out_total = count;

    for (unsigned int i = 0; i < count; i++) {
        if (!sessions[i] || !sessions[i]->session_path) {
            continue;
        }

        logger_info("Cleanup: disconnecting session '%s' (%s)",
                     sessions[i]->config_name ? sessions[i]->config_name : "unknown",
                     sessions[i]->session_path);

        r = session_disconnect(bus, sessions[i]->session_path);
        if (r >= 0) {
            (*out_cleaned)++;
        } else {
            logger_error("Cleanup: failed to disconnect session '%s'",
                         sessions[i]->config_name ? sessions[i]->config_name : "unknown");
        }
    }

    session_list_free(sessions, count);
    return 0;
}
