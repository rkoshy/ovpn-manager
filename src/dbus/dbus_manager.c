#include "dbus_manager.h"
#include "../utils/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

/* OpenVPN3 D-Bus service names */
#define OPENVPN3_SERVICE_CONFIG "net.openvpn.v3.configuration"
#define OPENVPN3_SERVICE_SESSIONS "net.openvpn.v3.sessions"
#define OPENVPN3_SERVICE_BACKENDS "net.openvpn.v3.backends"

/**
 * GLib I/O watch callback for D-Bus file descriptor
 */
static gboolean dbus_io_callback(GIOChannel *source, GIOCondition condition, gpointer data) {
    DbusManager *manager = (DbusManager *)data;
    int r;

    if (!manager || !manager->bus) {
        return FALSE;
    }

    /* Process D-Bus events */
    do {
        r = sd_bus_process(manager->bus, NULL);
        if (r < 0) {
            logger_error("Failed to process D-Bus: %s", strerror(-r));
            return FALSE;
        }
    } while (r > 0);

    return TRUE;
}

/**
 * Initialize D-Bus manager and connect to system bus
 */
DbusManager* dbus_manager_init(void) {
    DbusManager *manager = NULL;
    int r;
    int fd;

    /* Allocate manager structure */
    manager = g_malloc0(sizeof(DbusManager));
    if (!manager) {
        logger_error("Failed to allocate DbusManager");
        return NULL;
    }

    /* Connect to system bus */
    r = sd_bus_open_system(&manager->bus);
    if (r < 0) {
        logger_error("Failed to connect to system bus: %s", strerror(-r));
        g_free(manager);
        return NULL;
    }

    /* Get the file descriptor for the bus */
    fd = sd_bus_get_fd(manager->bus);
    if (fd < 0) {
        logger_error("Failed to get D-Bus file descriptor: %s", strerror(-fd));
        sd_bus_unref(manager->bus);
        g_free(manager);
        return NULL;
    }

    /* Create GLib I/O channel from the D-Bus fd */
    manager->bus_channel = g_io_channel_unix_new(fd);
    if (!manager->bus_channel) {
        logger_error("Failed to create GLib I/O channel");
        sd_bus_unref(manager->bus);
        g_free(manager);
        return NULL;
    }

    /* Add watch for D-Bus events */
    manager->bus_watch_id = g_io_add_watch(
        manager->bus_channel,
        G_IO_IN | G_IO_HUP | G_IO_ERR,
        dbus_io_callback,
        manager
    );

    manager->connected = true;

    logger_info("D-Bus manager initialized successfully");

    return manager;
}

/**
 * Check if a D-Bus service is available
 */
static bool check_service_available(sd_bus *bus, const char *service_name) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r;
    const char *owner = NULL;

    /* Call org.freedesktop.DBus.GetNameOwner to check if service exists */
    r = sd_bus_call_method(
        bus,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "GetNameOwner",
        &error,
        &reply,
        "s",
        service_name
    );

    if (r < 0) {
        /* Service not available */
        logger_error("D-Bus GetNameOwner failed for %s: %s",
                service_name, error.message ? error.message : "unknown error");
        sd_bus_error_free(&error);
        return false;
    }

    /* Read the owner (if exists, service is available) */
    r = sd_bus_message_read(reply, "s", &owner);
    sd_bus_message_unref(reply);

    return (r >= 0 && owner != NULL);
}

/**
 * Check if OpenVPN3 services are available on D-Bus
 */
bool dbus_manager_check_openvpn3(DbusManager *manager) {
    int r;

    if (!manager || !manager->bus) {
        logger_error("Invalid DbusManager");
        return false;
    }

    /* Process any pending D-Bus messages first */
    r = sd_bus_process(manager->bus, NULL);
    if (r < 0) {
        logger_error("Failed to process D-Bus: %s", strerror(-r));
    }

    /* Check for configuration service (optional - may be activatable) */
    if (!check_service_available(manager->bus, OPENVPN3_SERVICE_CONFIG)) {
        logger_info("Note: OpenVPN3 configuration service not running (will activate on demand)");
    }

    /* Check for sessions service (optional - only runs when there are active sessions) */
    if (!check_service_available(manager->bus, OPENVPN3_SERVICE_SESSIONS)) {
        logger_info("Note: OpenVPN3 sessions service not running (will activate on demand)");
    }

    logger_info("OpenVPN3 D-Bus services installed");
    return true;
}

/**
 * Get the sd-bus connection
 */
sd_bus* dbus_manager_get_bus(DbusManager *manager) {
    if (!manager) {
        return NULL;
    }
    return manager->bus;
}

/**
 * Clean up D-Bus manager and disconnect from bus
 */
void dbus_manager_cleanup(DbusManager *manager) {
    if (!manager) {
        return;
    }

    /* Remove GLib watch */
    if (manager->bus_watch_id > 0) {
        g_source_remove(manager->bus_watch_id);
        manager->bus_watch_id = 0;
    }

    /* Unref GLib I/O channel */
    if (manager->bus_channel) {
        g_io_channel_unref(manager->bus_channel);
        manager->bus_channel = NULL;
    }

    /* Close D-Bus connection */
    if (manager->bus) {
        sd_bus_flush_close_unref(manager->bus);
        manager->bus = NULL;
    }

    manager->connected = false;

    logger_info("D-Bus manager cleaned up");

    g_free(manager);
}
