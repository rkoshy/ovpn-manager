#ifndef ICONS_H
#define ICONS_H

/**
 * Icon System for OpenVPN3 Manager
 *
 * Uses GTK's icon theme system with freedesktop.org Icon Naming Specification.
 * All icons use -symbolic suffix for proper theme color adaptation.
 *
 * Reference: https://specifications.freedesktop.org/icon-naming-spec/latest/
 */

/* Connection State Icons (used in menu items to show connection status) */
#define ICON_CONNECTED "network-transmit-receive-symbolic"  /* Active connection */
#define ICON_CONNECTING "network-idle-symbolic"             /* Establishing connection */
#define ICON_PAUSED "media-playback-pause-symbolic"        /* Paused connection */
#define ICON_AUTH_REQUIRED "dialog-password-symbolic"      /* Authentication needed */
#define ICON_ERROR "network-error-symbolic"                /* Connection error */
#define ICON_DISCONNECTED "network-offline-symbolic"       /* No connection */
#define ICON_RECONNECTING "network-idle-symbolic"          /* Reconnecting */

/* Action Icons (for submenu buttons) */
#define ICON_CONNECT "network-wired-symbolic"              /* Connect action */
#define ICON_DISCONNECT "network-offline-symbolic"         /* Disconnect action */
#define ICON_PAUSE "media-playback-pause-symbolic"         /* Pause connection */
#define ICON_RESUME "media-playback-start-symbolic"        /* Resume connection */
#define ICON_AUTHENTICATE "dialog-password-symbolic"       /* Authenticate action */

/* Main Menu Feature Icons */
#define ICON_DASHBOARD "view-grid-symbolic"                /* Show Dashboard */
#define ICON_IMPORT "document-open-symbolic"               /* Import Config */
#define ICON_SETTINGS "preferences-system-symbolic"        /* Settings */
#define ICON_STATISTICS "utilities-system-monitor"         /* Statistics (no symbolic variant) */
#define ICON_QUIT "application-exit-symbolic"              /* Quit application */

/* Config Management Icons */
#define ICON_CONFIG "text-x-generic-symbolic"              /* Generic config file */
#define ICON_CONFIG_IN_USE "network-transmit-receive-symbolic"  /* Config actively in use */

/* Status Indicators (for list/table views) */
#define ICON_STATUS_ACTIVE "emblem-default-symbolic"       /* Active/selected item indicator */

/* Unicode Status Symbols (embedded in label text for connection state) */
#define STATUS_SYMBOL_CONNECTED      "●"   /* U+25CF Filled circle */
#define STATUS_SYMBOL_DISCONNECTED   "○"   /* U+25CB Empty circle */
#define STATUS_SYMBOL_CONNECTING     "◐"   /* U+25D0 Half-filled circle */
#define STATUS_SYMBOL_AUTHENTICATING "◉"   /* U+25C9 Fisheye (circle in circle) */
#define STATUS_SYMBOL_PAUSED         "◫"   /* U+25EB Square with vertical line */
#define STATUS_SYMBOL_ERROR          "⊗"   /* U+2297 Circle with X */
#define STATUS_SYMBOL_RECONNECTING   "◐"   /* U+25D0 Half-filled circle (same as connecting) */

/* App Indicator Tray Icons (for system tray status) */
#define ICON_TRAY_ACTIVE "network-vpn-symbolic"                 /* Active VPN connection */
#define ICON_TRAY_IDLE "network-vpn-symbolic"                   /* VPN available but idle */
#define ICON_TRAY_DISCONNECTED "network-vpn-symbolic"           /* No VPN connections */
#define ICON_TRAY_ATTENTION "network-error-symbolic"            /* Error/needs attention */

/* Per-connection VPN tray indicator icons (one AppIndicator per connection) */
#define ICON_TRAY_VPN_CONNECTED   "network-vpn-symbolic"           /* Connected */
#define ICON_TRAY_VPN_DISCONNECTED "network-vpn-disabled-symbolic" /* Disconnected */
#define ICON_TRAY_VPN_ACQUIRING   "network-vpn-acquiring-symbolic" /* Connecting/reconnecting */
#define ICON_TRAY_APP             "preferences-system-symbolic"    /* App gear icon */

#endif /* ICONS_H */
