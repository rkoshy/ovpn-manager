#include "signal_handlers.h"
#include "../utils/logger.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#define OPENVPN3_SERVICE_SESSIONS "net.openvpn.v3.sessions"
#define OPENVPN3_INTERFACE_SESSION "net.openvpn.v3.sessions"

/**
 * AttentionRequired signal callback
 */
int attention_required_handler(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)userdata;
    (void)ret_error;

    unsigned int type, group;
    const char *message;
    int r;

    /* Parse AttentionRequired signal: uus (three separate args, not a struct) */
    r = sd_bus_message_read(m, "uus", &type, &group, &message);
    if (r < 0) {
        logger_error("Failed to parse AttentionRequired signal: %s", strerror(-r));
        return 0;
    }

    const char *session_path = sd_bus_message_get_path(m);

    logger_info("AttentionRequired signal: session=%s, type=%u, group=%u",
                session_path, type, group);

    /* Type 1 = Web authentication (OAuth) */
    if (type == 1 && message && strstr(message, "http") == message) {
        logger_info("Web authentication required, URL: %s", message);
        /* Browser launch is handled by tray_icon_update_sessions to avoid duplicates */
    } else if (message) {
        logger_info("Attention message: %s", message);
    }

    return 0;
}

/**
 * Subscribe to AttentionRequired signals for OAuth detection
 */
int signals_subscribe_attention_required(sd_bus *bus, const char *session_path) {
    sd_bus_slot *slot = NULL;
    int r;

    if (!bus || !session_path) {
        return -EINVAL;
    }

    /* Subscribe to AttentionRequired signal for this specific session */
    char match[512];
    snprintf(match, sizeof(match),
            "type='signal',"
            "sender='net.openvpn.v3.sessions',"
            "path='%s',"
            "interface='net.openvpn.v3.sessions',"
            "member='AttentionRequired'",
            session_path);

    r = sd_bus_add_match(bus, &slot, match, attention_required_handler, NULL);
    if (r < 0) {
        logger_error("Failed to subscribe to AttentionRequired: %s", strerror(-r));
        return r;
    }

    logger_info("Subscribed to authentication signals for session");

    /* Note: We don't unref the slot because we want to keep the subscription active */

    return 0;
}
