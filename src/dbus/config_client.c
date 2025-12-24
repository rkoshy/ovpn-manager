#include "config_client.h"
#include "../utils/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define OPENVPN3_SERVICE_CONFIG "net.openvpn.v3.configuration"
#define OPENVPN3_INTERFACE_CONFIG "net.openvpn.v3.configuration"
#define OPENVPN3_ROOT_PATH "/net/openvpn/v3/configuration"

/**
 * Free a VPN configuration structure
 */
void config_free(VpnConfig *config) {
    if (!config) {
        return;
    }

    g_free(config->config_path);
    g_free(config->config_name);
    g_free(config->server_address);
    g_free(config->server_hostname);
    g_free(config->protocol);
    g_free(config);
}

/**
 * Free an array of VPN configurations
 */
void config_list_free(VpnConfig **configs, unsigned int count) {
    if (!configs) {
        return;
    }

    for (unsigned int i = 0; i < count; i++) {
        config_free(configs[i]);
    }

    g_free(configs);
}

/**
 * Get a string property from D-Bus object
 */
static char* get_string_property(sd_bus *bus, const char *path,
                                   const char *interface, const char *property) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *value = NULL;
    char *result = NULL;
    int r;

    r = sd_bus_get_property(
        bus,
        OPENVPN3_SERVICE_CONFIG,
        path,
        interface,
        property,
        &error,
        &reply,
        "s"
    );

    if (r < 0) {
        sd_bus_error_free(&error);
        return NULL;
    }

    r = sd_bus_message_read(reply, "s", &value);
    if (r >= 0 && value) {
        result = g_strdup(value);
    }

    sd_bus_message_unref(reply);
    return result;
}

/**
 * Get a boolean property from D-Bus object
 */
static bool get_bool_property(sd_bus *bus, const char *path,
                               const char *interface, const char *property) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int value = 0;
    int r;

    r = sd_bus_get_property(
        bus,
        OPENVPN3_SERVICE_CONFIG,
        path,
        interface,
        property,
        &error,
        &reply,
        "b"
    );

    if (r < 0) {
        sd_bus_error_free(&error);
        return false;
    }

    sd_bus_message_read(reply, "b", &value);
    sd_bus_message_unref(reply);

    return value != 0;
}

/**
 * Fetch configuration content from D-Bus
 */
static char* fetch_config_content(sd_bus *bus, const char *config_path) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *content = NULL;
    char *result = NULL;
    int r;

    r = sd_bus_call_method(
        bus,
        OPENVPN3_SERVICE_CONFIG,
        config_path,
        OPENVPN3_INTERFACE_CONFIG,
        "Fetch",
        &error,
        &reply,
        ""
    );

    if (r < 0) {
        sd_bus_error_free(&error);
        return NULL;
    }

    r = sd_bus_message_read(reply, "s", &content);
    if (r >= 0 && content) {
        result = g_strdup(content);
    }

    sd_bus_message_unref(reply);
    return result;
}

/**
 * Parse server details from OpenVPN config content
 * Looks for: remote <hostname> <port> [protocol]
 */
static void parse_server_details(const char *config_content, VpnConfig *config) {
    if (!config_content || !config) {
        return;
    }

    /* Initialize to NULL/default values */
    config->server_address = NULL;
    config->server_hostname = NULL;
    config->server_port = 1194;  /* Default OpenVPN port */
    config->protocol = NULL;

    /* Split config into lines */
    char **lines = g_strsplit(config_content, "\n", -1);
    if (!lines) {
        return;
    }

    /* Look for "remote" directive */
    for (int i = 0; lines[i] != NULL; i++) {
        char *line = g_strstrip(lines[i]);

        /* Skip comments and empty lines */
        if (line[0] == '#' || line[0] == ';' || line[0] == '\0') {
            continue;
        }

        /* Check if line starts with "remote" */
        if (g_str_has_prefix(line, "remote ")) {
            /* Parse: remote <hostname> <port> [protocol] */
            char **parts = g_strsplit_set(line, " \t", -1);
            int part_count = 0;

            /* Count non-empty parts */
            for (int j = 0; parts[j] != NULL; j++) {
                if (strlen(g_strstrip(parts[j])) > 0) {
                    part_count++;
                }
            }

            /* We need at least: remote hostname port */
            if (part_count >= 3) {
                int part_idx = 0;
                char *hostname = NULL;
                char *port_str = NULL;
                char *protocol = NULL;

                /* Extract non-empty parts */
                for (int j = 0; parts[j] != NULL && part_idx < 4; j++) {
                    char *part = g_strstrip(parts[j]);
                    if (strlen(part) == 0) continue;

                    if (part_idx == 0) {
                        /* "remote" keyword - skip */
                    } else if (part_idx == 1) {
                        hostname = part;
                    } else if (part_idx == 2) {
                        port_str = part;
                    } else if (part_idx == 3) {
                        protocol = part;
                    }
                    part_idx++;
                }

                /* Store results */
                if (hostname && port_str) {
                    config->server_hostname = g_strdup(hostname);
                    config->server_port = atoi(port_str);

                    /* Build server_address as "hostname:port" */
                    config->server_address = g_strdup_printf("%s:%d",
                                                             hostname,
                                                             config->server_port);

                    if (protocol) {
                        config->protocol = g_strdup(protocol);
                    } else {
                        config->protocol = g_strdup("udp");  /* Default */
                    }

                    g_strfreev(parts);
                    break;  /* Use first remote directive found */
                }
            }

            g_strfreev(parts);
        }
    }

    g_strfreev(lines);
}

/**
 * Get detailed configuration information
 */
VpnConfig* config_get_info(sd_bus *bus, const char *config_path) {
    VpnConfig *config = NULL;

    if (!bus || !config_path) {
        return NULL;
    }

    config = g_malloc0(sizeof(VpnConfig));
    if (!config) {
        return NULL;
    }

    /* Store config path */
    config->config_path = g_strdup(config_path);

    /* Get properties */
    config->config_name = get_string_property(
        bus, config_path, OPENVPN3_INTERFACE_CONFIG, "name"
    );

    config->locked_down = get_bool_property(
        bus, config_path, OPENVPN3_INTERFACE_CONFIG, "locked_down"
    );

    config->persistent = get_bool_property(
        bus, config_path, OPENVPN3_INTERFACE_CONFIG, "persistent"
    );

    /* Fetch config content and extract server details */
    char *config_content = fetch_config_content(bus, config_path);
    if (config_content) {
        parse_server_details(config_content, config);
        g_free(config_content);
    }

    return config;
}

/**
 * Import an OVPN configuration file
 */
int config_import(sd_bus *bus, const char *name, const char *config_content,
                  bool single_use, bool persistent, char **config_path) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *path = NULL;
    int r;

    if (!bus || !name || !config_content || !config_path) {
        return -EINVAL;
    }

    *config_path = NULL;

    /* Call Import method */
    r = sd_bus_call_method(
        bus,
        OPENVPN3_SERVICE_CONFIG,
        OPENVPN3_ROOT_PATH,
        OPENVPN3_INTERFACE_CONFIG,
        "Import",
        &error,
        &reply,
        "ssbb",
        name,
        config_content,
        single_use ? 1 : 0,
        persistent ? 1 : 0
    );

    if (r < 0) {
        logger_error("Failed to import config '%s': %s",
                name, error.message ? error.message : strerror(-r));
        sd_bus_error_free(&error);
        return r;
    }

    /* Parse object path from reply */
    r = sd_bus_message_read(reply, "o", &path);
    if (r < 0 || !path) {
        logger_error("Failed to read import reply: %s", strerror(-r));
        sd_bus_message_unref(reply);
        return -EIO;
    }

    *config_path = g_strdup(path);
    sd_bus_message_unref(reply);

    logger_info("Imported config '%s' -> %s", name, *config_path);

    return 0;
}

/**
 * List all available VPN configurations
 */
int config_list(sd_bus *bus, VpnConfig ***configs, unsigned int *count) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r;
    const char *path;
    GPtrArray *config_array = NULL;

    if (!bus || !configs || !count) {
        return -EINVAL;
    }

    *configs = NULL;
    *count = 0;

    /* Create temporary array */
    config_array = g_ptr_array_new();

    /* Call FetchAvailableConfigs method (with retry for service activation) */
    for (int retry = 0; retry < 6; retry++) {
        r = sd_bus_call_method(
            bus,
            OPENVPN3_SERVICE_CONFIG,
            OPENVPN3_ROOT_PATH,
            OPENVPN3_INTERFACE_CONFIG,
            "FetchAvailableConfigs",
            &error,
            &reply,
            ""
        );

        if (r >= 0) {
            /* Success */
            break;
        }

        /* If service-related failure and not last retry, wait and retry */
        if (retry < 5 && (r == -ENODATA || r == -53)) {
            if (retry == 0) {
                logger_error("Configuration service not ready, waiting for startup...");
            } else {
                logger_error("Still waiting for configuration service (attempt %d/6)...", retry + 1);
            }
            sd_bus_error_free(&error);
            /* Give the service time to activate (1 second) */
            g_usleep(1000000);
            continue;
        }

        /* Other error or final retry failed */
        logger_error("Failed to fetch configs after %d attempts: %s",
                retry + 1, error.message ? error.message : strerror(-r));
        sd_bus_error_free(&error);
        g_ptr_array_free(config_array, TRUE);
        return r;
    }

    /* Parse array of object paths */
    r = sd_bus_message_enter_container(reply, 'a', "o");
    if (r < 0) {
        logger_error("Failed to enter container: %s", strerror(-r));
        sd_bus_message_unref(reply);
        g_ptr_array_free(config_array, TRUE);
        return r;
    }

    /* Read each config path */
    while ((r = sd_bus_message_read(reply, "o", &path)) > 0) {
        VpnConfig *config = config_get_info(bus, path);
        if (config) {
            g_ptr_array_add(config_array, config);
        }
    }

    sd_bus_message_exit_container(reply);
    sd_bus_message_unref(reply);

    /* Convert to array */
    *count = config_array->len;
    if (*count > 0) {
        *configs = g_malloc(sizeof(VpnConfig*) * (*count));
        for (unsigned int i = 0; i < *count; i++) {
            (*configs)[i] = g_ptr_array_index(config_array, i);
        }
    }

    g_ptr_array_free(config_array, TRUE);

    return 0;
}

/**
 * Delete a VPN configuration
 */
int config_delete(sd_bus *bus, const char *config_path) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int r;

    if (!bus || !config_path) {
        return -EINVAL;
    }

    r = sd_bus_call_method(
        bus,
        OPENVPN3_SERVICE_CONFIG,
        config_path,
        OPENVPN3_INTERFACE_CONFIG,
        "Remove",
        &error,
        NULL,
        ""
    );

    if (r < 0) {
        logger_error("Failed to delete config: %s",
                error.message ? error.message : strerror(-r));
        sd_bus_error_free(&error);
        return r;
    }

    logger_info("Deleted config: %s", config_path);

    return 0;
}
