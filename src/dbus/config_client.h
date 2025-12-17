#ifndef CONFIG_CLIENT_H
#define CONFIG_CLIENT_H

#include <systemd/sd-bus.h>
#include <stdbool.h>
#include <stdint.h>
#include <glib.h>

/**
 * Config Client
 *
 * Manages OpenVPN3 configuration profiles via D-Bus
 */

/* VPN configuration information */
typedef struct {
    char *config_path;       /* D-Bus object path */
    char *config_name;       /* Configuration name */
    bool locked_down;        /* Is config locked */
    bool persistent;         /* Is config persistent */
    char *server_address;    /* Server address (hostname:port) */
    char *server_hostname;   /* Server hostname only */
    int server_port;         /* Server port number */
    char *protocol;          /* Protocol (udp/tcp) */
} VpnConfig;

/**
 * Import an OVPN configuration file
 *
 * @param bus D-Bus connection
 * @param name Configuration name
 * @param config_content OVPN file contents
 * @param single_use Whether config should be single-use
 * @param persistent Whether config should persist across reboots
 * @param config_path Output: D-Bus object path of imported config
 * @return 0 on success, negative on error
 */
int config_import(sd_bus *bus, const char *name, const char *config_content,
                  bool single_use, bool persistent, char **config_path);

/**
 * List all available VPN configurations
 *
 * @param bus D-Bus connection
 * @param configs Output array of VpnConfig pointers
 * @param count Output count of configs
 * @return 0 on success, negative on error
 */
int config_list(sd_bus *bus, VpnConfig ***configs, unsigned int *count);

/**
 * Get detailed configuration information
 *
 * @param bus D-Bus connection
 * @param config_path D-Bus object path of configuration
 * @return VpnConfig structure on success, NULL on failure
 */
VpnConfig* config_get_info(sd_bus *bus, const char *config_path);

/**
 * Delete a VPN configuration
 *
 * @param bus D-Bus connection
 * @param config_path D-Bus object path of configuration
 * @return 0 on success, negative on error
 */
int config_delete(sd_bus *bus, const char *config_path);

/**
 * Free a VPN configuration structure
 *
 * @param config VpnConfig to free
 */
void config_free(VpnConfig *config);

/**
 * Free an array of VPN configurations
 *
 * @param configs Array of VpnConfig pointers
 * @param count Number of configurations
 */
void config_list_free(VpnConfig **configs, unsigned int count);

#endif /* CONFIG_CLIENT_H */
