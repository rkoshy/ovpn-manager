#ifndef TRAY_WRAPPER_H
#define TRAY_WRAPPER_H

#include <stdbool.h>
#include <systemd/sd-bus.h>

/**
 * Tray Icon Manager
 *
 * Wrapper around zserge/tray for system tray integration
 */

typedef struct TrayIcon TrayIcon;

/**
 * Initialize the system tray icon
 *
 * @param tooltip Initial tooltip text
 * @return Pointer to TrayIcon on success, NULL on failure
 */
TrayIcon* tray_icon_init(const char *tooltip);

/**
 * Update the tray icon tooltip
 *
 * @param tray TrayIcon instance
 * @param tooltip New tooltip text
 */
void tray_icon_set_tooltip(TrayIcon *tray, const char *tooltip);

/**
 * Run the tray icon event loop (blocking)
 *
 * @param tray TrayIcon instance
 * @return 0 on success, negative on error
 */
int tray_icon_run(TrayIcon *tray);

/**
 * Signal the tray to quit
 *
 * @param tray TrayIcon instance
 */
void tray_icon_quit(TrayIcon *tray);

/**
 * Update tray menu with active VPN sessions
 *
 * @param tray TrayIcon instance
 * @param bus D-Bus connection
 */
void tray_icon_update_sessions(TrayIcon *tray, sd_bus *bus);

/**
 * Update only the timer labels for connected sessions (efficient, no rebuild)
 *
 * @param tray TrayIcon instance
 * @param bus D-Bus connection
 */
void tray_icon_update_timers(TrayIcon *tray, sd_bus *bus);

/**
 * Clean up tray icon and free resources
 *
 * @param tray TrayIcon instance
 */
void tray_icon_cleanup(TrayIcon *tray);

#endif /* TRAY_WRAPPER_H */
