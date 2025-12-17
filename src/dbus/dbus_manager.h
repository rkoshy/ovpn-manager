#ifndef DBUS_MANAGER_H
#define DBUS_MANAGER_H

#include <systemd/sd-bus.h>
#include <glib.h>
#include <stdbool.h>

/**
 * D-Bus Manager
 *
 * Manages the connection to the system D-Bus and integrates
 * with the GLib main loop for event-driven processing.
 */

typedef struct {
    sd_bus *bus;
    GIOChannel *bus_channel;
    guint bus_watch_id;
    bool connected;
} DbusManager;

/**
 * Initialize D-Bus manager and connect to system bus
 *
 * @return Pointer to DbusManager on success, NULL on failure
 */
DbusManager* dbus_manager_init(void);

/**
 * Check if OpenVPN3 services are available on D-Bus
 *
 * @param manager DbusManager instance
 * @return true if OpenVPN3 services are available, false otherwise
 */
bool dbus_manager_check_openvpn3(DbusManager *manager);

/**
 * Get the sd-bus connection
 *
 * @param manager DbusManager instance
 * @return sd_bus pointer
 */
sd_bus* dbus_manager_get_bus(DbusManager *manager);

/**
 * Clean up D-Bus manager and disconnect from bus
 *
 * @param manager DbusManager instance to clean up
 */
void dbus_manager_cleanup(DbusManager *manager);

#endif /* DBUS_MANAGER_H */
