#include "signal_handlers.h"
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
        fprintf(stderr, "Failed to parse AttentionRequired signal: %s\n", strerror(-r));
        return 0;
    }

    const char *session_path = sd_bus_message_get_path(m);
    printf("\n");
    printf("⚠️  Attention required for session: %s\n", session_path);
    printf("   Type: %u, Group: %u\n", type, group);
    printf("   Message: %s\n", message ? message : "(none)");

    /* Type 1 = Web authentication (OAuth) */
    if (type == 1 && message && strstr(message, "http") == message) {
        printf("\n🔐 Web authentication required!\n");
        printf("🌐 Opening browser for authentication...\n");
        printf("   URL: %s\n", message);

        /* Launch browser with xdg-open */
        char cmd[2048];
        snprintf(cmd, sizeof(cmd), "xdg-open '%s' >/dev/null 2>&1 &", message);
        int ret = system(cmd);
        (void)ret;

        printf("\n");
        printf("ℹ️  Please complete authentication in your browser.\n");
        printf("   The VPN will connect automatically after you authenticate.\n");
        printf("\n");
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
        fprintf(stderr, "Failed to subscribe to AttentionRequired: %s\n", strerror(-r));
        return r;
    }

    printf("Subscribed to authentication signals for session\n");

    /* Note: We don't unref the slot because we want to keep the subscription active */

    return 0;
}
