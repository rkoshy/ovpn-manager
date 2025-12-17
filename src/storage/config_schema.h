#ifndef CONFIG_SCHEMA_H
#define CONFIG_SCHEMA_H

#include <stdbool.h>
#include <glib.h>

/**
 * Configuration Schema
 *
 * Data structures for application and VPN configurations
 */

/**
 * VPN configuration entry
 */
typedef struct {
    char *name;              /* User-friendly name */
    char *config_path;       /* D-Bus object path to OpenVPN3 config */
    char *ovpn_file_path;    /* Original .ovpn file path (optional) */
    bool auto_connect;       /* Auto-connect on startup */
} VpnConfig;

/**
 * Auto-reconnect settings
 */
typedef struct {
    bool enabled;
    unsigned int max_attempts;
} AutoReconnectConfig;

/**
 * Application configuration
 */
typedef struct {
    /* VPN configurations */
    VpnConfig **vpn_configs;
    unsigned int vpn_config_count;

    /* Application settings */
    bool enable_notifications;
    bool enable_dns_leak_check;
    bool enable_bandwidth_stats;
    bool enable_logging;
    int log_level;  /* 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR */

    /* Auto-reconnect */
    AutoReconnectConfig auto_reconnect;

    /* Last connected VPN */
    char *last_connected_vpn;
} AppConfig;

/**
 * Free a VPN config structure
 */
void vpn_config_free(VpnConfig *config);

/**
 * Free application config structure
 */
void app_config_free(AppConfig *config);

#endif /* CONFIG_SCHEMA_H */
