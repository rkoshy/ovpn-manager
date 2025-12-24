#include "config_storage.h"
#include "../utils/logger.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <glib.h>

/* Default config file path */
#define DEFAULT_CONFIG_PATH "~/.config/ovpn-manager/config.json"

/**
 * Free a VPN config structure
 */
void vpn_config_free(VpnConfig *config) {
    if (!config) {
        return;
    }

    g_free(config->name);
    g_free(config->config_path);
    g_free(config->ovpn_file_path);
    g_free(config);
}

/**
 * Free application config structure
 */
void app_config_free(AppConfig *config) {
    if (!config) {
        return;
    }

    /* Free VPN configs */
    if (config->vpn_configs) {
        for (unsigned int i = 0; i < config->vpn_config_count; i++) {
            vpn_config_free(config->vpn_configs[i]);
        }
        g_free(config->vpn_configs);
    }

    g_free(config->last_connected_vpn);
    g_free(config);
}

/**
 * Create default configuration
 */
AppConfig* config_create_default(void) {
    AppConfig *config = g_malloc0(sizeof(AppConfig));
    if (!config) {
        return NULL;
    }

    /* Default settings */
    config->enable_notifications = true;
    config->enable_dns_leak_check = true;
    config->enable_bandwidth_stats = true;
    config->enable_logging = false;
    config->log_level = 1;  /* INFO */

    /* Auto-reconnect defaults */
    config->auto_reconnect.enabled = true;
    config->auto_reconnect.max_attempts = 5;

    /* No VPN configs by default */
    config->vpn_configs = NULL;
    config->vpn_config_count = 0;
    config->last_connected_vpn = NULL;

    return config;
}

/**
 * Ensure config directory exists
 */
static int ensure_config_directory(const char *config_path) {
    char *dir_path = g_path_get_dirname(config_path);
    int ret = 0;

    /* Try to create directory (recursively) */
    if (g_mkdir_with_parents(dir_path, 0700) != 0 && errno != EEXIST) {
        logger_error("Failed to create config directory %s: %s",
                dir_path, strerror(errno));
        ret = -1;
    }

    g_free(dir_path);
    return ret;
}

/**
 * Expand tilde in path
 */
static char* expand_path(const char *path) {
    if (path && path[0] == '~') {
        const char *home = g_get_home_dir();
        return g_build_filename(home, path + 2, NULL);  /* Skip "~/" */
    }
    return g_strdup(path);
}

/**
 * Parse VPN config from JSON
 */
static VpnConfig* parse_vpn_config_json(cJSON *json) {
    VpnConfig *config = g_malloc0(sizeof(VpnConfig));
    if (!config) {
        return NULL;
    }

    cJSON *item;

    item = cJSON_GetObjectItem(json, "name");
    if (item && cJSON_IsString(item)) {
        config->name = g_strdup(item->valuestring);
    }

    item = cJSON_GetObjectItem(json, "config_path");
    if (item && cJSON_IsString(item)) {
        config->config_path = g_strdup(item->valuestring);
    }

    item = cJSON_GetObjectItem(json, "ovpn_file_path");
    if (item && cJSON_IsString(item)) {
        config->ovpn_file_path = g_strdup(item->valuestring);
    }

    item = cJSON_GetObjectItem(json, "auto_connect");
    if (item && cJSON_IsBool(item)) {
        config->auto_connect = cJSON_IsTrue(item);
    }

    return config;
}

/**
 * Serialize VPN config to JSON
 */
static cJSON* vpn_config_to_json(const VpnConfig *config) {
    cJSON *json = cJSON_CreateObject();
    if (!json) {
        return NULL;
    }

    if (config->name) {
        cJSON_AddStringToObject(json, "name", config->name);
    }

    if (config->config_path) {
        cJSON_AddStringToObject(json, "config_path", config->config_path);
    }

    if (config->ovpn_file_path) {
        cJSON_AddStringToObject(json, "ovpn_file_path", config->ovpn_file_path);
    }

    cJSON_AddBoolToObject(json, "auto_connect", config->auto_connect);

    return json;
}

/**
 * Load configuration from file
 */
AppConfig* config_load(const char *config_path) {
    char *path = NULL;
    char *file_content = NULL;
    gsize file_size;
    cJSON *json = NULL;
    AppConfig *config = NULL;
    GError *error = NULL;

    /* Expand path */
    if (config_path) {
        path = expand_path(config_path);
    } else {
        path = expand_path(DEFAULT_CONFIG_PATH);
    }

    /* Read file */
    if (!g_file_get_contents(path, &file_content, &file_size, &error)) {
        /* File doesn't exist - create default config */
        if (error->code == G_FILE_ERROR_NOENT) {
            logger_info("Config file not found, creating default: %s", path);
            config = config_create_default();
            g_free(path);
            g_error_free(error);
            return config;
        }

        logger_error("Failed to read config file %s: %s", path, error->message);
        g_free(path);
        g_error_free(error);
        return NULL;
    }

    /* Parse JSON */
    json = cJSON_Parse(file_content);
    g_free(file_content);

    if (!json) {
        logger_error("Failed to parse config JSON: %s", cJSON_GetErrorPtr());
        g_free(path);
        return NULL;
    }

    /* Create config structure */
    config = g_malloc0(sizeof(AppConfig));

    /* Parse settings */
    cJSON *item;

    item = cJSON_GetObjectItem(json, "enable_notifications");
    config->enable_notifications = item && cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(json, "enable_dns_leak_check");
    config->enable_dns_leak_check = item && cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(json, "enable_bandwidth_stats");
    config->enable_bandwidth_stats = item && cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(json, "enable_logging");
    config->enable_logging = item && cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(json, "log_level");
    if (item && cJSON_IsNumber(item)) {
        config->log_level = item->valueint;
    } else {
        config->log_level = 1;  /* Default: INFO */
    }

    /* Parse auto-reconnect */
    cJSON *auto_reconnect = cJSON_GetObjectItem(json, "auto_reconnect");
    if (auto_reconnect) {
        item = cJSON_GetObjectItem(auto_reconnect, "enabled");
        config->auto_reconnect.enabled = item && cJSON_IsTrue(item);

        item = cJSON_GetObjectItem(auto_reconnect, "max_attempts");
        if (item && cJSON_IsNumber(item)) {
            config->auto_reconnect.max_attempts = item->valueint;
        } else {
            config->auto_reconnect.max_attempts = 5;
        }
    }

    /* Parse VPN configs */
    cJSON *vpn_configs = cJSON_GetObjectItem(json, "vpn_configs");
    if (vpn_configs && cJSON_IsArray(vpn_configs)) {
        int count = cJSON_GetArraySize(vpn_configs);
        if (count > 0) {
            config->vpn_configs = g_malloc0(sizeof(VpnConfig*) * count);
            config->vpn_config_count = 0;

            for (int i = 0; i < count; i++) {
                cJSON *vpn_json = cJSON_GetArrayItem(vpn_configs, i);
                if (vpn_json) {
                    VpnConfig *vpn = parse_vpn_config_json(vpn_json);
                    if (vpn) {
                        config->vpn_configs[config->vpn_config_count++] = vpn;
                    }
                }
            }
        }
    }

    /* Parse last connected VPN */
    item = cJSON_GetObjectItem(json, "last_connected_vpn");
    if (item && cJSON_IsString(item)) {
        config->last_connected_vpn = g_strdup(item->valuestring);
    }

    cJSON_Delete(json);
    g_free(path);

    return config;
}

/**
 * Save configuration to file
 */
int config_save(const AppConfig *config, const char *config_path) {
    char *path = NULL;
    char *json_string = NULL;
    FILE *file = NULL;
    cJSON *json = NULL;
    int ret = 0;

    if (!config) {
        return -1;
    }

    /* Expand path */
    if (config_path) {
        path = expand_path(config_path);
    } else {
        path = expand_path(DEFAULT_CONFIG_PATH);
    }

    /* Ensure directory exists */
    if (ensure_config_directory(path) != 0) {
        g_free(path);
        return -1;
    }

    /* Create JSON */
    json = cJSON_CreateObject();
    if (!json) {
        g_free(path);
        return -1;
    }

    /* Add settings */
    cJSON_AddBoolToObject(json, "enable_notifications", config->enable_notifications);
    cJSON_AddBoolToObject(json, "enable_dns_leak_check", config->enable_dns_leak_check);
    cJSON_AddBoolToObject(json, "enable_bandwidth_stats", config->enable_bandwidth_stats);
    cJSON_AddBoolToObject(json, "enable_logging", config->enable_logging);
    cJSON_AddNumberToObject(json, "log_level", config->log_level);

    /* Add auto-reconnect */
    cJSON *auto_reconnect = cJSON_CreateObject();
    cJSON_AddBoolToObject(auto_reconnect, "enabled", config->auto_reconnect.enabled);
    cJSON_AddNumberToObject(auto_reconnect, "max_attempts", config->auto_reconnect.max_attempts);
    cJSON_AddItemToObject(json, "auto_reconnect", auto_reconnect);

    /* Add VPN configs */
    cJSON *vpn_configs = cJSON_CreateArray();
    for (unsigned int i = 0; i < config->vpn_config_count; i++) {
        cJSON *vpn_json = vpn_config_to_json(config->vpn_configs[i]);
        if (vpn_json) {
            cJSON_AddItemToArray(vpn_configs, vpn_json);
        }
    }
    cJSON_AddItemToObject(json, "vpn_configs", vpn_configs);

    /* Add last connected VPN */
    if (config->last_connected_vpn) {
        cJSON_AddStringToObject(json, "last_connected_vpn", config->last_connected_vpn);
    } else {
        cJSON_AddNullToObject(json, "last_connected_vpn");
    }

    /* Convert to string */
    json_string = cJSON_Print(json);
    cJSON_Delete(json);

    if (!json_string) {
        g_free(path);
        return -1;
    }

    /* Write to file with 0600 permissions */
    file = fopen(path, "w");
    if (!file) {
        logger_error("Failed to open config file for writing %s: %s",
                path, strerror(errno));
        free(json_string);
        g_free(path);
        return -1;
    }

    /* Write JSON */
    if (fputs(json_string, file) == EOF) {
        logger_error("Failed to write config file %s: %s",
                path, strerror(errno));
        ret = -1;
    }

    fclose(file);
    free(json_string);

    /* Set permissions to 0600 */
    if (chmod(path, S_IRUSR | S_IWUSR) != 0) {
        logger_error("Failed to set config file permissions %s: %s",
                path, strerror(errno));
        ret = -1;
    }

    g_free(path);
    return ret;
}

/**
 * Add a VPN configuration
 */
int config_add_vpn(AppConfig *config, VpnConfig *vpn_config) {
    if (!config || !vpn_config) {
        return -1;
    }

    /* Reallocate array */
    config->vpn_configs = g_realloc(
        config->vpn_configs,
        sizeof(VpnConfig*) * (config->vpn_config_count + 1)
    );

    config->vpn_configs[config->vpn_config_count++] = vpn_config;

    return 0;
}

/**
 * Remove a VPN configuration by name
 */
int config_remove_vpn(AppConfig *config, const char *name) {
    if (!config || !name) {
        return -1;
    }

    /* Find VPN */
    for (unsigned int i = 0; i < config->vpn_config_count; i++) {
        if (config->vpn_configs[i]->name &&
            strcmp(config->vpn_configs[i]->name, name) == 0) {

            /* Free the VPN config */
            vpn_config_free(config->vpn_configs[i]);

            /* Shift remaining configs */
            for (unsigned int j = i; j < config->vpn_config_count - 1; j++) {
                config->vpn_configs[j] = config->vpn_configs[j + 1];
            }

            config->vpn_config_count--;

            return 0;
        }
    }

    return -1;  /* Not found */
}

/**
 * Find a VPN configuration by name
 */
VpnConfig* config_find_vpn(const AppConfig *config, const char *name) {
    if (!config || !name) {
        return NULL;
    }

    for (unsigned int i = 0; i < config->vpn_config_count; i++) {
        if (config->vpn_configs[i]->name &&
            strcmp(config->vpn_configs[i]->name, name) == 0) {
            return config->vpn_configs[i];
        }
    }

    return NULL;
}
