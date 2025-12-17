#ifndef ICONS_H
#define ICONS_H

/**
 * Icon System for OpenVPN3 Manager
 *
 * Phase 1 uses GTK's built-in icon theme system.
 * Icons are referenced by standard icon names from the freedesktop.org
 * icon naming specification.
 *
 * Future phases may add custom SVG icons for enhanced visuals.
 */

/* Session State Icons */
#define ICON_CONNECTED "emblem-default"
#define ICON_CONNECTING "emblem-synchronizing"
#define ICON_PAUSED "media-playback-pause"
#define ICON_AUTH_REQUIRED "dialog-password"
#define ICON_ERROR "dialog-error"
#define ICON_DISCONNECTED "network-vpn"

/* Action Icons */
#define ICON_DISCONNECT "process-stop"
#define ICON_PAUSE "media-playback-pause"
#define ICON_RESUME "media-playback-start"
#define ICON_AUTHENTICATE "dialog-password"
#define ICON_CONNECT "media-playback-start"

/* Feature Icons */
#define ICON_IMPORT "document-open"
#define ICON_SETTINGS "preferences-system"
#define ICON_STATISTICS "utilities-system-monitor"
#define ICON_QUIT "application-exit"

/* Config Icons */
#define ICON_CONFIG "text-x-generic"
#define ICON_CONFIG_IN_USE "emblem-default"

#endif /* ICONS_H */
