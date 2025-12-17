#ifndef DASHBOARD_H
#define DASHBOARD_H

#include <gtk/gtk.h>
#include <systemd/sd-bus.h>

/**
 * Dashboard window for OpenVPN Manager
 *
 * Shows a beautiful, modern UI with:
 * - Color-coded VPN session cards
 * - Connection statistics and timers
 * - Configuration management
 * - Modern macOS-inspired design
 */

typedef struct Dashboard Dashboard;

/**
 * Create and initialize the dashboard window
 *
 * @return Dashboard* - The dashboard instance, or NULL on failure
 */
Dashboard* dashboard_create(void);

/**
 * Show the dashboard window
 * If already visible, brings it to front
 *
 * @param dashboard The dashboard instance
 */
void dashboard_show(Dashboard *dashboard);

/**
 * Hide the dashboard window
 *
 * @param dashboard The dashboard instance
 */
void dashboard_hide(Dashboard *dashboard);

/**
 * Toggle dashboard visibility
 *
 * @param dashboard The dashboard instance
 */
void dashboard_toggle(Dashboard *dashboard);

/**
 * Update dashboard with current VPN sessions and configurations
 *
 * @param dashboard The dashboard instance
 * @param bus D-Bus connection for fetching data
 */
void dashboard_update(Dashboard *dashboard, sd_bus *bus);

/**
 * Cleanup and free dashboard resources
 *
 * @param dashboard The dashboard instance
 */
void dashboard_destroy(Dashboard *dashboard);

#endif /* DASHBOARD_H */
