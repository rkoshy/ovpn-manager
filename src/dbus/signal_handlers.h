#ifndef SIGNAL_HANDLERS_H
#define SIGNAL_HANDLERS_H

#include <systemd/sd-bus.h>

/**
 * Signal Handlers
 *
 * D-Bus signal subscription and handling
 */

/**
 * Subscribe to AttentionRequired signals for OAuth detection
 *
 * @param bus D-Bus connection
 * @param session_path Session object path to monitor
 * @return 0 on success, negative on error
 */
int signals_subscribe_attention_required(sd_bus *bus, const char *session_path);

/**
 * AttentionRequired signal callback
 */
int attention_required_handler(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);

#endif /* SIGNAL_HANDLERS_H */
