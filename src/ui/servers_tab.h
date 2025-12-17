#ifndef SERVERS_TAB_H
#define SERVERS_TAB_H

#include <gtk/gtk.h>
#include <systemd/sd-bus.h>
#include "../dbus/config_client.h"

/**
 * Servers Tab
 *
 * Displays available VPN configurations with server information and latency
 */

/* Server information structure */
typedef struct {
    VpnConfig *config;       /* Configuration details */
    int latency_ms;          /* Ping latency in milliseconds (-1 = not tested) */
    gboolean testing;        /* Currently testing latency */
    gboolean connected;      /* Currently connected */
} ServerInfo;

/* Servers tab structure */
typedef struct ServersTab ServersTab;

/**
 * Create servers tab widget
 *
 * @param bus D-Bus connection
 * @return ServersTab instance or NULL on error
 */
ServersTab* servers_tab_create(sd_bus *bus);

/**
 * Get the tab widget
 *
 * @param tab ServersTab instance
 * @return GtkWidget containing the tab content
 */
GtkWidget* servers_tab_get_widget(ServersTab *tab);

/**
 * Refresh server list from configurations
 *
 * @param tab ServersTab instance
 * @param bus D-Bus connection
 */
void servers_tab_refresh(ServersTab *tab, sd_bus *bus);

/**
 * Update connection status for servers
 *
 * @param tab ServersTab instance
 * @param bus D-Bus connection
 */
void servers_tab_update_status(ServersTab *tab, sd_bus *bus);

/**
 * Free servers tab
 *
 * @param tab ServersTab instance
 */
void servers_tab_free(ServersTab *tab);

#endif /* SERVERS_TAB_H */
