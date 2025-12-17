#ifndef CONFIG_STORAGE_H
#define CONFIG_STORAGE_H

#include "config_schema.h"

/**
 * Configuration Storage
 *
 * JSON-based configuration persistence
 */

/**
 * Load configuration from file
 *
 * @param config_path Path to config file (NULL for default: ~/.config/ovpn-manager/config.json)
 * @return AppConfig structure on success, NULL on failure
 */
AppConfig* config_load(const char *config_path);

/**
 * Save configuration to file
 *
 * @param config AppConfig structure to save
 * @param config_path Path to config file (NULL for default: ~/.config/ovpn-manager/config.json)
 * @return 0 on success, negative on failure
 */
int config_save(const AppConfig *config, const char *config_path);

/**
 * Create default configuration
 *
 * @return AppConfig structure with default values
 */
AppConfig* config_create_default(void);

/**
 * Add a VPN configuration
 *
 * @param config AppConfig structure
 * @param vpn_config VPN configuration to add
 * @return 0 on success, negative on failure
 */
int config_add_vpn(AppConfig *config, VpnConfig *vpn_config);

/**
 * Remove a VPN configuration by name
 *
 * @param config AppConfig structure
 * @param name Name of VPN to remove
 * @return 0 on success, negative on failure
 */
int config_remove_vpn(AppConfig *config, const char *name);

/**
 * Find a VPN configuration by name
 *
 * @param config AppConfig structure
 * @param name Name of VPN to find
 * @return VpnConfig pointer on success, NULL if not found
 */
VpnConfig* config_find_vpn(const AppConfig *config, const char *name);

#endif /* CONFIG_STORAGE_H */
