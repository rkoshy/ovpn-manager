#include "tray.h"
#include "dbus/session_client.h"
#include "dbus/config_client.h"
#include "utils/file_chooser.h"
#include "utils/logger.h"
#include "utils/connection_fsm.h"
#include "ui/icons.h"
#include "ui/dashboard.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>

/* External global variables from main.c */
extern Dashboard *dashboard;

/* ──────────────────────────────────────────────────────────────
 * Data structures
 * ────────────────────────────────────────────────────────────── */

/* Per-connection tray indicator */
typedef struct {
    AppIndicator *indicator;       /* Separate AppIndicator per connection */
    GtkWidget *menu;               /* Flat menu, rebuilt on state change */
    char *config_path;             /* Stable identifier */
    char *config_name;             /* Display name */
    char *session_path;            /* NULL if disconnected */
    ConnectionState state;         /* Current state */
    time_t connect_time;           /* For elapsed time display */
    sd_bus *bus;                   /* D-Bus connection (borrowed) */
} ConnectionIndicator;

/* App-level tray indicator (global actions) */
struct TrayIcon {
    AppIndicator *indicator;       /* The [gear] icon */
    GtkWidget *menu;               /* Dashboard, Import, Settings, Quit */
    char *tooltip;
    GHashTable *connections;       /* config_path -> ConnectionIndicator* */
    sd_bus *bus;                   /* D-Bus connection (borrowed, set on first update) */
    gboolean app_menu_built;       /* Whether app menu has been built */
};

/* Merged connection data (temporary struct for building/updating) */
typedef struct {
    char *config_path;
    char *config_name;
    char *session_path;            /* NULL if no active session */
    ConnectionState state;
    time_t connect_time;
} ConnectionInfo;

/* ──────────────────────────────────────────────────────────────
 * Static state
 * ────────────────────────────────────────────────────────────── */

static GHashTable *session_timings = NULL;  /* session_path -> time_t */
static GHashTable *auth_launched = NULL;    /* session_path -> TRUE */

/* ──────────────────────────────────────────────────────────────
 * Forward declarations
 * ────────────────────────────────────────────────────────────── */

static void on_connect(GtkMenuItem *item, gpointer data);
static void on_disconnect(GtkMenuItem *item, gpointer data);
static void on_pause(GtkMenuItem *item, gpointer data);
static void on_resume(GtkMenuItem *item, gpointer data);
static void on_cancel(GtkMenuItem *item, gpointer data);
static void on_authenticate(GtkMenuItem *item, gpointer data);
static void quit_callback(GtkMenuItem *item, gpointer user_data);
static void show_dashboard_callback(GtkMenuItem *item, gpointer user_data);
static void import_config_callback(GtkMenuItem *item, gpointer user_data);

/* ──────────────────────────────────────────────────────────────
 * Utility functions
 * ────────────────────────────────────────────────────────────── */

/**
 * Format elapsed time as a human-readable string
 */
static void format_elapsed_time(time_t seconds, char *buffer, size_t buf_size) {
    if (seconds < 60) {
        snprintf(buffer, buf_size, "%lds", seconds);
    } else if (seconds < 3600) {
        int mins = seconds / 60;
        snprintf(buffer, buf_size, "%dm", mins);
    } else if (seconds < 86400) {
        int hours = seconds / 3600;
        int mins = (seconds % 3600) / 60;
        if (mins > 0) {
            snprintf(buffer, buf_size, "%dh %dm", hours, mins);
        } else {
            snprintf(buffer, buf_size, "%dh", hours);
        }
    } else {
        int days = seconds / 86400;
        int hours = (seconds % 86400) / 3600;
        if (hours > 0) {
            snprintf(buffer, buf_size, "%dd %dh", days, hours);
        } else {
            snprintf(buffer, buf_size, "%dd", days);
        }
    }
}

/**
 * Get or create session timing entry
 */
static time_t get_session_start_time(const char *session_path, uint64_t session_created) {
    if (!session_timings) {
        session_timings = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    }

    gpointer value = g_hash_table_lookup(session_timings, session_path);
    if (value) {
        return (time_t)(intptr_t)value;
    }

    time_t start_time = (time_t)session_created;
    g_hash_table_insert(session_timings, g_strdup(session_path), (gpointer)(intptr_t)start_time);
    return start_time;
}

/**
 * Remove session timing entry
 */
static void remove_session_timing(const char *session_path) {
    if (session_timings) {
        g_hash_table_remove(session_timings, session_path);
    }
}

/**
 * Launch browser for OAuth authentication
 */
static void launch_auth_browser(sd_bus *bus, const char *session_path) {
    if (!bus || !session_path) {
        return;
    }

    logger_info("Auto-launching browser for authentication: %s", session_path);

    char *auth_url = NULL;
    int r = session_get_auth_url(bus, session_path, &auth_url);

    /* If queue is empty, try to get URL from session status message */
    if (r < 0 || !auth_url) {
        VpnSession *session = session_get_info(bus, session_path);
        if (session && session->status_message && strstr(session->status_message, "https://")) {
            auth_url = g_strdup(session->status_message);
            logger_info("Got auth URL from status message: %s", auth_url);
        }
        session_free(session);
    }

    if (!auth_url) {
        logger_error("Failed to get authentication URL");
        return;
    }

    logger_info("Opening browser for authentication: %s", auth_url);

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "xdg-open '%s' >/dev/null 2>&1 &", auth_url);
    int ret = system(cmd);
    (void)ret;

    g_free(auth_url);
}

/**
 * Map VPN session state to connection state
 */
static ConnectionState get_state_from_session(VpnSession *session) {
    if (!session) {
        return CONN_STATE_DISCONNECTED;
    }

    ConnectionState state;

    switch (session->state) {
        case SESSION_STATE_CONNECTING:
            state = CONN_STATE_CONNECTING;
            break;
        case SESSION_STATE_CONNECTED:
            state = CONN_STATE_CONNECTED;
            break;
        case SESSION_STATE_PAUSED:
            state = CONN_STATE_PAUSED;
            break;
        case SESSION_STATE_AUTH_REQUIRED:
            state = CONN_STATE_AUTH_REQUIRED;
            break;
        case SESSION_STATE_ERROR:
            state = CONN_STATE_ERROR;
            break;
        case SESSION_STATE_RECONNECTING:
            state = CONN_STATE_RECONNECTING;
            break;
        case SESSION_STATE_DISCONNECTED:
        default:
            state = CONN_STATE_DISCONNECTED;
            break;
    }

    if (logger_get_verbosity() >= 2) {
        logger_info("D-Bus session state: %d -> Connection state: %s, session_path=%s, config_name=%s",
                    session->state,
                    connection_fsm_state_name(state),
                    session->session_path ? session->session_path : "NULL",
                    session->config_name ? session->config_name : "NULL");
    }

    return state;
}

/**
 * Comparison function for sorting connections alphabetically
 */
static int compare_connections(const void *a, const void *b) {
    const ConnectionInfo *conn_a = (const ConnectionInfo *)a;
    const ConnectionInfo *conn_b = (const ConnectionInfo *)b;

    if (!conn_a->config_name) return 1;
    if (!conn_b->config_name) return -1;

    return strcmp(conn_a->config_name, conn_b->config_name);
}

/**
 * Merge configs and sessions into a unified connection list
 * Returns array of ConnectionInfo (caller must free with free_connection_info_array)
 */
static ConnectionInfo* merge_connections_data(sd_bus *bus, unsigned int *out_count) {
    if (!bus || !out_count) {
        return NULL;
    }

    *out_count = 0;

    /* Get all configurations */
    VpnConfig **configs = NULL;
    unsigned int config_count = 0;
    int r = config_list(bus, &configs, &config_count);

    if (r < 0 || !configs) {
        return NULL;
    }

    /* Get all active sessions */
    VpnSession **sessions = NULL;
    unsigned int session_count = 0;
    r = session_list(bus, &sessions, &session_count);
    if (r < 0) {
        sessions = NULL;
        session_count = 0;
    }

    /* Allocate array for merged connections */
    ConnectionInfo *connections = g_malloc0(sizeof(ConnectionInfo) * config_count);

    if (logger_get_verbosity() >= 2) {
        logger_info("Merging connections: %u configs, %u active sessions", config_count, session_count);
    }

    /* Build connection list from configs */
    for (unsigned int i = 0; i < config_count; i++) {
        VpnConfig *config = configs[i];

        connections[i].config_path = g_strdup(config->config_path);
        connections[i].config_name = g_strdup(config->config_name ? config->config_name : "Unknown");
        connections[i].session_path = NULL;
        connections[i].state = CONN_STATE_DISCONNECTED;
        connections[i].connect_time = 0;

        /* Try to match this config to an active session */
        bool found_session = false;
        for (unsigned int j = 0; j < session_count; j++) {
            VpnSession *session = sessions[j];

            if (session->config_name && config->config_name &&
                strcmp(session->config_name, config->config_name) == 0) {
                connections[i].session_path = g_strdup(session->session_path);
                connections[i].state = get_state_from_session(session);
                connections[i].connect_time = get_session_start_time(
                    session->session_path,
                    session->session_created
                );
                found_session = true;

                if (logger_get_verbosity() >= 2) {
                    logger_info("  Config '%s' matched to session (state=%s, session_path=%s)",
                               config->config_name,
                               connection_fsm_state_name(connections[i].state),
                               session->session_path ? session->session_path : "NULL");
                }
                break;
            }
        }

        if (!found_session && logger_get_verbosity() >= 2) {
            logger_info("  Config '%s' has no active session (state=DISCONNECTED)",
                       config->config_name);
        }
    }

    *out_count = config_count;

    /* Sort alphabetically */
    qsort(connections, config_count, sizeof(ConnectionInfo), compare_connections);

    /* Free session and config lists */
    if (sessions) {
        session_list_free(sessions, session_count);
    }
    if (configs) {
        config_list_free(configs, config_count);
    }

    return connections;
}

/**
 * Free a ConnectionInfo array
 */
static void free_connection_info_array(ConnectionInfo *connections, unsigned int count) {
    if (!connections) {
        return;
    }

    for (unsigned int i = 0; i < count; i++) {
        g_free(connections[i].config_path);
        g_free(connections[i].config_name);
        g_free(connections[i].session_path);
    }

    g_free(connections);
}

/* ──────────────────────────────────────────────────────────────
 * Connection indicator helpers
 * ────────────────────────────────────────────────────────────── */

/**
 * Get the tray icon name for a connection state
 */
static const char* get_indicator_icon(ConnectionState state) {
    switch (state) {
        case CONN_STATE_DISCONNECTED: return ICON_TRAY_VPN_DISCONNECTED;
        case CONN_STATE_CONNECTING:   return ICON_TRAY_VPN_ACQUIRING;
        case CONN_STATE_CONNECTED:    return ICON_TRAY_VPN_CONNECTED;
        case CONN_STATE_PAUSED:       return ICON_PAUSED;
        case CONN_STATE_AUTH_REQUIRED: return ICON_AUTH_REQUIRED;
        case CONN_STATE_ERROR:        return ICON_TRAY_ATTENTION;
        case CONN_STATE_RECONNECTING: return ICON_TRAY_VPN_ACQUIRING;
        default:                      return ICON_TRAY_VPN_DISCONNECTED;
    }
}

/**
 * Create a sanitized AppIndicator ID from a config name
 */
static char* make_indicator_id(const char *config_name) {
    char *id = g_strdup_printf("ovpn-%s", config_name);
    for (char *p = id; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != '-') {
            *p = '-';
        } else {
            *p = g_ascii_tolower(*p);
        }
    }
    return id;
}

/**
 * Format the status label shown at the top of a connection's menu
 */
static void format_status_label(const char *name, ConnectionState state,
                                 time_t connect_time, char *buffer, size_t size) {
    switch (state) {
        case CONN_STATE_DISCONNECTED:
            snprintf(buffer, size, "%s: Disconnected", name);
            break;
        case CONN_STATE_CONNECTING:
            snprintf(buffer, size, "%s: Connecting...", name);
            break;
        case CONN_STATE_CONNECTED: {
            time_t now = time(NULL);
            time_t elapsed = now - connect_time;
            char elapsed_str[32];
            format_elapsed_time(elapsed, elapsed_str, sizeof(elapsed_str));
            snprintf(buffer, size, "%s: Connected \xC2\xB7 %s", name, elapsed_str);
            break;
        }
        case CONN_STATE_PAUSED:
            snprintf(buffer, size, "%s: Paused", name);
            break;
        case CONN_STATE_AUTH_REQUIRED:
            snprintf(buffer, size, "%s: Auth Required", name);
            break;
        case CONN_STATE_ERROR:
            snprintf(buffer, size, "%s: Error", name);
            break;
        case CONN_STATE_RECONNECTING:
            snprintf(buffer, size, "%s: Reconnecting...", name);
            break;
        default:
            snprintf(buffer, size, "%s", name);
            break;
    }
}

/**
 * Append a plain menu item with a callback to a menu
 */
static void add_action(GtkWidget *menu, const char *label,
                        GCallback callback, gpointer data) {
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    g_signal_connect(item, "activate", callback, data);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    gtk_widget_show(item);
}

/* ──────────────────────────────────────────────────────────────
 * Connection indicator lifecycle
 * ────────────────────────────────────────────────────────────── */

/**
 * Rebuild a connection indicator's menu from scratch.
 * This is the core fix: calling app_indicator_set_menu() with a new menu
 * forces dbusmenu to re-serialize the entire tree, avoiding the property
 * propagation issues that plague visibility/sensitivity changes.
 */
static void connection_indicator_rebuild_menu(ConnectionIndicator *ci) {
    GtkWidget *new_menu = gtk_menu_new();

    /* Status label (disabled) */
    char label[256];
    format_status_label(ci->config_name, ci->state, ci->connect_time,
                        label, sizeof(label));
    GtkWidget *status = gtk_menu_item_new_with_label(label);
    gtk_widget_set_sensitive(status, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(new_menu), status);
    gtk_widget_show(status);

    /* Separator */
    GtkWidget *sep = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(new_menu), sep);
    gtk_widget_show(sep);

    /* State-dependent actions */
    switch (ci->state) {
        case CONN_STATE_DISCONNECTED:
        case CONN_STATE_ERROR:
            add_action(new_menu, "Connect", G_CALLBACK(on_connect), ci);
            break;

        case CONN_STATE_CONNECTING:
        case CONN_STATE_RECONNECTING:
            add_action(new_menu, "Cancel", G_CALLBACK(on_cancel), ci);
            break;

        case CONN_STATE_CONNECTED:
            add_action(new_menu, "Disconnect", G_CALLBACK(on_disconnect), ci);
            add_action(new_menu, "Pause", G_CALLBACK(on_pause), ci);
            break;

        case CONN_STATE_PAUSED:
            add_action(new_menu, "Resume", G_CALLBACK(on_resume), ci);
            add_action(new_menu, "Disconnect", G_CALLBACK(on_disconnect), ci);
            break;

        case CONN_STATE_AUTH_REQUIRED:
            add_action(new_menu, "Authenticate", G_CALLBACK(on_authenticate), ci);
            add_action(new_menu, "Cancel", G_CALLBACK(on_cancel), ci);
            break;
    }

    /* Swap menu — AppIndicator re-serializes on set_menu */
    app_indicator_set_menu(ci->indicator, GTK_MENU(new_menu));
    ci->menu = new_menu;
}

/**
 * Create a new connection indicator with its own AppIndicator
 */
static ConnectionIndicator* connection_indicator_create(sd_bus *bus, ConnectionInfo *conn) {
    ConnectionIndicator *ci = g_malloc0(sizeof(ConnectionIndicator));

    ci->config_path = g_strdup(conn->config_path);
    ci->config_name = g_strdup(conn->config_name);
    ci->session_path = conn->session_path ? g_strdup(conn->session_path) : NULL;
    ci->state = conn->state;
    ci->connect_time = conn->connect_time;
    ci->bus = bus;

    /* Create unique AppIndicator */
    char *id = make_indicator_id(conn->config_name);
    const char *icon = get_indicator_icon(conn->state);

    ci->indicator = app_indicator_new(id, icon,
                                       APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    g_free(id);

    if (!ci->indicator) {
        logger_error("Failed to create indicator for '%s'", conn->config_name);
        g_free(ci->config_path);
        g_free(ci->config_name);
        g_free(ci->session_path);
        g_free(ci);
        return NULL;
    }

    app_indicator_set_status(ci->indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_title(ci->indicator, conn->config_name);

    /* Build initial menu */
    connection_indicator_rebuild_menu(ci);

    logger_info("Created tray indicator for '%s' (state=%s)",
                conn->config_name, connection_fsm_state_name(conn->state));

    return ci;
}

/**
 * Destroy a connection indicator
 */
static void connection_indicator_free(ConnectionIndicator *ci) {
    if (!ci) {
        return;
    }

    logger_info("Destroying tray indicator for '%s'", ci->config_name);

    if (ci->indicator) {
        app_indicator_set_status(ci->indicator, APP_INDICATOR_STATUS_PASSIVE);
        g_object_unref(ci->indicator);
    }

    g_free(ci->config_path);
    g_free(ci->config_name);
    g_free(ci->session_path);
    g_free(ci);
}

/**
 * Update a connection indicator with new state from D-Bus
 */
static void connection_indicator_update(ConnectionIndicator *ci, ConnectionInfo *conn) {
    if (!ci || !conn) {
        return;
    }

    gboolean changed = FALSE;

    /* Check for state change */
    if (ci->state != conn->state) {
        logger_info("Connection '%s' state: %s -> %s",
                    ci->config_name,
                    connection_fsm_state_name(ci->state),
                    connection_fsm_state_name(conn->state));
        ci->state = conn->state;
        changed = TRUE;

        /* Update icon */
        app_indicator_set_icon(ci->indicator, get_indicator_icon(ci->state));
    }

    /* Update connect time */
    ci->connect_time = conn->connect_time;

    /* Update session path if changed */
    if (g_strcmp0(ci->session_path, conn->session_path) != 0) {
        g_free(ci->session_path);
        ci->session_path = conn->session_path ? g_strdup(conn->session_path) : NULL;
        changed = TRUE;
    }

    /* Rebuild menu if anything changed */
    if (changed) {
        connection_indicator_rebuild_menu(ci);
    }

    /* Auto-launch browser for authentication */
    if (conn->state == CONN_STATE_AUTH_REQUIRED && conn->session_path && ci->bus) {
        if (!auth_launched) {
            auth_launched = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        }
        if (!g_hash_table_contains(auth_launched, conn->session_path)) {
            launch_auth_browser(ci->bus, conn->session_path);
            g_hash_table_insert(auth_launched, g_strdup(conn->session_path),
                                GINT_TO_POINTER(1));
        }
    }
}

/* ──────────────────────────────────────────────────────────────
 * Connection action callbacks
 * ────────────────────────────────────────────────────────────── */

/**
 * Connect to a VPN configuration
 */
static void on_connect(GtkMenuItem *item, gpointer data) {
    (void)item;
    ConnectionIndicator *ci = (ConnectionIndicator *)data;
    if (!ci || !ci->bus || !ci->config_path) {
        return;
    }

    logger_info("Connecting: %s", ci->config_name);

    char *session_path = NULL;
    int r = session_start(ci->bus, ci->config_path, &session_path);
    if (r < 0) {
        logger_error("Failed to start VPN session for '%s'", ci->config_name);
    } else {
        logger_info("Started VPN session: %s", session_path);
        g_free(session_path);
    }
}

/**
 * Disconnect with confirmation dialog
 */
static void on_disconnect(GtkMenuItem *item, gpointer data) {
    (void)item;
    ConnectionIndicator *ci = (ConnectionIndicator *)data;
    if (!ci || !ci->bus || !ci->session_path) {
        return;
    }

    /* Show confirmation dialog */
    GtkWidget *dialog = gtk_message_dialog_new(
        NULL,
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_NONE,
        "Disconnect from %s?",
        ci->config_name
    );
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Disconnect", GTK_RESPONSE_ACCEPT);

    int response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response == GTK_RESPONSE_ACCEPT) {
        logger_info("Disconnecting: %s", ci->config_name);
        int r = session_disconnect(ci->bus, ci->session_path);
        if (r < 0) {
            logger_error("Failed to disconnect session");
        } else {
            remove_session_timing(ci->session_path);
        }
    }
}

/**
 * Pause a connected VPN session
 */
static void on_pause(GtkMenuItem *item, gpointer data) {
    (void)item;
    ConnectionIndicator *ci = (ConnectionIndicator *)data;
    if (!ci || !ci->bus || !ci->session_path) {
        return;
    }

    logger_info("Pausing: %s", ci->config_name);
    int r = session_pause(ci->bus, ci->session_path, "User requested");
    if (r < 0) {
        logger_error("Failed to pause session");
    }
}

/**
 * Resume a paused VPN session
 */
static void on_resume(GtkMenuItem *item, gpointer data) {
    (void)item;
    ConnectionIndicator *ci = (ConnectionIndicator *)data;
    if (!ci || !ci->bus || !ci->session_path) {
        return;
    }

    logger_info("Resuming: %s", ci->config_name);
    int r = session_resume(ci->bus, ci->session_path);
    if (r < 0) {
        logger_error("Failed to resume session");
    }
}

/**
 * Cancel a connecting/reconnecting session
 */
static void on_cancel(GtkMenuItem *item, gpointer data) {
    (void)item;
    ConnectionIndicator *ci = (ConnectionIndicator *)data;
    if (!ci || !ci->bus || !ci->session_path) {
        return;
    }

    logger_info("Cancelling: %s", ci->config_name);
    int r = session_disconnect(ci->bus, ci->session_path);
    if (r < 0) {
        logger_error("Failed to cancel session");
    } else {
        remove_session_timing(ci->session_path);
    }
}

/**
 * Launch browser for OAuth authentication
 */
static void on_authenticate(GtkMenuItem *item, gpointer data) {
    (void)item;
    ConnectionIndicator *ci = (ConnectionIndicator *)data;
    if (!ci || !ci->bus || !ci->session_path) {
        return;
    }

    launch_auth_browser(ci->bus, ci->session_path);
}

/* ──────────────────────────────────────────────────────────────
 * App menu callbacks
 * ────────────────────────────────────────────────────────────── */

/**
 * Quit the application
 */
static void quit_callback(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;

    logger_info("Quit menu item clicked");

    extern GApplication *app;
    if (app) {
        g_application_quit(app);
    }
}

/**
 * Show the dashboard window
 */
static void show_dashboard_callback(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;

    logger_info("Show Dashboard menu item clicked");

    if (dashboard) {
        dashboard_show(dashboard);
    }
}

/**
 * Import a VPN configuration file
 */
static void import_config_callback(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    sd_bus *bus = (sd_bus *)user_data;

    if (!bus) {
        logger_error("No D-Bus connection available");
        dialog_show_error("Import Error", "No D-Bus connection available");
        return;
    }

    /* Show file chooser */
    char *file_path = file_chooser_select_ovpn("Import OpenVPN Configuration");
    if (!file_path) {
        return;  /* User cancelled */
    }

    logger_info("Selected file: %s", file_path);

    /* Read file contents */
    char *contents = NULL;
    char *error = NULL;
    int r = file_read_contents(file_path, &contents, &error);

    if (r < 0) {
        logger_error("Failed to read file: %s", error ? error : "Unknown error");
        dialog_show_error("Import Error", error ? error : "Failed to read file");
        g_free(error);
        g_free(file_path);
        return;
    }

    /* Extract default config name from filename */
    char *basename = g_path_get_basename(file_path);
    char *default_name = g_strdup(basename);

    /* Remove .ovpn or .conf extension for default name */
    char *dot = strrchr(default_name, '.');
    if (dot && (strcmp(dot, ".ovpn") == 0 || strcmp(dot, ".conf") == 0)) {
        *dot = '\0';
    }

    /* Prompt user for config name */
    char *config_name = dialog_get_text_input(
        "Import Configuration",
        "Configuration name:",
        default_name
    );

    g_free(default_name);
    g_free(basename);

    if (!config_name) {
        logger_info("Import cancelled by user");
        g_free(contents);
        g_free(file_path);
        return;
    }

    /* Import configuration with persistent=true */
    char *config_path = NULL;
    r = config_import(bus, config_name, contents, false, true, &config_path);

    if (r < 0) {
        logger_error("Failed to import configuration: %s", config_name);
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Failed to import configuration '%s'.\n\n"
                 "Check if the configuration already exists.", config_name);
        dialog_show_error("Import Error", msg);
    } else {
        logger_info("Successfully imported persistent configuration: %s -> %s",
                     config_name, config_path);
        char msg[256];
        snprintf(msg, sizeof(msg), "Configuration '%s' imported successfully.",
                 config_name);
        dialog_show_info("Import Successful", msg);
        g_free(config_path);
    }

    g_free(config_name);
    g_free(contents);
    g_free(file_path);
}

/**
 * Force cleanup all VPN sessions via D-Bus (no sudo required)
 */
static void on_force_cleanup(GtkMenuItem *item, gpointer data) {
    (void)item;
    TrayIcon *tray = (TrayIcon *)data;
    if (!tray || !tray->bus) {
        return;
    }

    /* Confirmation dialog */
    GtkWidget *dialog = gtk_message_dialog_new(
        NULL,
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING,
        GTK_BUTTONS_NONE,
        "Force cleanup all VPN sessions?\n\n"
        "This will disconnect all active VPN connections."
    );
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Cleanup", GTK_RESPONSE_ACCEPT);

    int response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response != GTK_RESPONSE_ACCEPT) {
        return;
    }

    logger_info("Force cleanup: disconnecting all sessions");

    unsigned int total = 0, cleaned = 0;
    session_cleanup_all(tray->bus, &total, &cleaned);

    char msg[256];
    if (total == 0) {
        snprintf(msg, sizeof(msg), "No active sessions found.");
    } else if (cleaned == total) {
        snprintf(msg, sizeof(msg), "Successfully disconnected %u session%s.",
                 cleaned, cleaned == 1 ? "" : "s");
    } else {
        snprintf(msg, sizeof(msg),
                 "Disconnected %u of %u sessions.\n\n"
                 "%u session%s could not be disconnected.\n"
                 "Try \"Restart VPN Service\" if sessions are still stuck.",
                 cleaned, total,
                 total - cleaned, (total - cleaned) == 1 ? "" : "s");
    }

    dialog_show_info("Force Cleanup", msg);
}

/**
 * Restart VPN backend service (requires sudo via pkexec)
 */
static void on_restart_vpn_service(GtkMenuItem *item, gpointer data) {
    (void)item;
    (void)data;

    /* Confirmation dialog */
    GtkWidget *dialog = gtk_message_dialog_new(
        NULL,
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING,
        GTK_BUTTONS_NONE,
        "Restart VPN Service?\n\n"
        "This will kill all VPN backend processes and\n"
        "disconnect all active sessions.\n\n"
        "Administrative privileges are required."
    );
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Restart", GTK_RESPONSE_ACCEPT);

    int response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response != GTK_RESPONSE_ACCEPT) {
        return;
    }

    logger_info("Restarting VPN backend service via pkexec");

    GError *error = NULL;
    gboolean ok = g_spawn_command_line_async(
        "pkexec bash -c '"
        "killall openvpn3-service-backend 2>/dev/null; "
        "sleep 1; "
        "echo VPN backend processes terminated"
        "'",
        &error
    );

    if (!ok) {
        logger_error("Failed to restart VPN service: %s",
                     error ? error->message : "Unknown error");
        char err_msg[512];
        snprintf(err_msg, sizeof(err_msg),
                 "Failed to restart VPN service:\n%s",
                 error ? error->message : "Unknown error");
        dialog_show_error("Restart Failed", err_msg);
        if (error) {
            g_error_free(error);
        }
    } else {
        dialog_show_info("VPN Service",
                         "VPN backend processes are being restarted.\n\n"
                         "Reconnect your VPN sessions when ready.");
    }
}

/* ──────────────────────────────────────────────────────────────
 * App menu building
 * ────────────────────────────────────────────────────────────── */

/**
 * Build the app indicator's static menu (Dashboard, Import, Settings, Quit)
 */
static void build_app_menu(TrayIcon *tray) {
    if (tray->app_menu_built) {
        return;
    }

    GtkWidget *menu = gtk_menu_new();

    /* Header (disabled) */
    GtkWidget *header = gtk_menu_item_new_with_label("OpenVPN Manager");
    gtk_widget_set_sensitive(header, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), header);
    gtk_widget_show(header);

    /* Separator */
    GtkWidget *sep1 = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep1);
    gtk_widget_show(sep1);

    /* Show Dashboard */
    GtkWidget *dash = gtk_menu_item_new_with_label("Show Dashboard");
    g_signal_connect(dash, "activate", G_CALLBACK(show_dashboard_callback), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), dash);
    gtk_widget_show(dash);

    /* Import Config... */
    GtkWidget *import_item = gtk_menu_item_new_with_label("Import Config...");
    g_signal_connect(import_item, "activate", G_CALLBACK(import_config_callback), tray->bus);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), import_item);
    gtk_widget_show(import_item);

    /* Settings (not implemented) */
    GtkWidget *settings = gtk_menu_item_new_with_label("Settings");
    gtk_widget_set_sensitive(settings, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), settings);
    gtk_widget_show(settings);

    /* Separator — Troubleshooting section */
    GtkWidget *sep_trouble = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep_trouble);
    gtk_widget_show(sep_trouble);

    /* Force Cleanup Sessions */
    GtkWidget *cleanup = gtk_menu_item_new_with_label("Force Cleanup Sessions");
    g_signal_connect(cleanup, "activate", G_CALLBACK(on_force_cleanup), tray);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), cleanup);
    gtk_widget_show(cleanup);

    /* Restart VPN Service... */
    GtkWidget *restart = gtk_menu_item_new_with_label("Restart VPN Service...");
    g_signal_connect(restart, "activate", G_CALLBACK(on_restart_vpn_service), tray);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), restart);
    gtk_widget_show(restart);

    /* Separator */
    GtkWidget *sep2 = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep2);
    gtk_widget_show(sep2);

    /* Quit */
    GtkWidget *quit = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(quit, "activate", G_CALLBACK(quit_callback), tray);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit);
    gtk_widget_show(quit);

    app_indicator_set_menu(tray->indicator, GTK_MENU(menu));
    tray->menu = menu;
    tray->app_menu_built = TRUE;
}

/* ──────────────────────────────────────────────────────────────
 * Public API
 * ────────────────────────────────────────────────────────────── */

/**
 * Initialize the system tray icon (app indicator only)
 */
TrayIcon* tray_icon_init(const char *tooltip) {
    /* Initialize GTK if not already initialized */
    if (!gtk_init_check(NULL, NULL)) {
        logger_error("Failed to initialize GTK");
        return NULL;
    }

    TrayIcon *tray = g_malloc0(sizeof(TrayIcon));

    /* Create app indicator (gear icon) */
    tray->indicator = app_indicator_new(
        "ovpn-manager",
        ICON_TRAY_APP,
        APP_INDICATOR_CATEGORY_APPLICATION_STATUS
    );

    if (!tray->indicator) {
        logger_error("Failed to create app indicator");
        g_free(tray);
        return NULL;
    }

    app_indicator_set_status(tray->indicator, APP_INDICATOR_STATUS_ACTIVE);

    if (tooltip) {
        tray->tooltip = g_strdup(tooltip);
        app_indicator_set_title(tray->indicator, tooltip);
    }

    /* Placeholder menu (AppIndicator requires a menu to show) */
    tray->menu = gtk_menu_new();
    GtkWidget *placeholder = gtk_menu_item_new_with_label("Loading...");
    gtk_widget_set_sensitive(placeholder, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), placeholder);
    gtk_widget_show(placeholder);
    app_indicator_set_menu(tray->indicator, GTK_MENU(tray->menu));

    /* Initialize connection hash table */
    tray->connections = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free,
        (GDestroyNotify)connection_indicator_free
    );

    tray->app_menu_built = FALSE;
    tray->bus = NULL;

    logger_info("System tray icon initialized");

    return tray;
}

/**
 * Update the tray icon tooltip
 */
void tray_icon_set_tooltip(TrayIcon *tray, const char *tooltip) {
    if (!tray || !tooltip) {
        return;
    }

    g_free(tray->tooltip);
    tray->tooltip = g_strdup(tooltip);
    app_indicator_set_title(tray->indicator, tooltip);
}

/**
 * Process pending GTK events (called by GLib timer)
 */
int tray_icon_run(TrayIcon *tray) {
    if (!tray) {
        return -1;
    }

    while (gtk_events_pending()) {
        gtk_main_iteration_do(FALSE);
    }

    return 0;
}

/**
 * Update tray with active VPN sessions.
 * Creates/updates/removes per-connection AppIndicators.
 */
void tray_icon_update_sessions(TrayIcon *tray, sd_bus *bus) {
    if (!tray || !bus) {
        return;
    }

    /* Store bus reference and build app menu on first call */
    if (!tray->bus) {
        tray->bus = bus;
    }
    if (!tray->app_menu_built) {
        build_app_menu(tray);
    }

    /* Get merged connection data */
    unsigned int count = 0;
    ConnectionInfo *connections = merge_connections_data(bus, &count);

    /* Track which config_paths exist in current data */
    GHashTable *current = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* Create or update connection indicators */
    if (connections) {
        for (unsigned int i = 0; i < count; i++) {
            ConnectionInfo *conn = &connections[i];
            g_hash_table_insert(current, g_strdup(conn->config_path), GINT_TO_POINTER(1));

            ConnectionIndicator *ci = g_hash_table_lookup(tray->connections,
                                                           conn->config_path);
            if (ci) {
                connection_indicator_update(ci, conn);
            } else {
                ci = connection_indicator_create(bus, conn);
                if (ci) {
                    g_hash_table_insert(tray->connections,
                                        g_strdup(conn->config_path), ci);
                }
            }
        }
    }

    /* Remove indicators for deleted configs */
    GList *to_remove = NULL;
    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init(&iter, tray->connections);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        if (!g_hash_table_contains(current, (const char *)key)) {
            to_remove = g_list_prepend(to_remove, g_strdup((const char *)key));
        }
    }

    for (GList *l = to_remove; l != NULL; l = l->next) {
        g_hash_table_remove(tray->connections, l->data);
        g_free(l->data);
    }
    g_list_free(to_remove);
    g_hash_table_destroy(current);

    /* Update app indicator tooltip */
    unsigned int active = 0;
    if (connections) {
        for (unsigned int i = 0; i < count; i++) {
            if (connections[i].state != CONN_STATE_DISCONNECTED) {
                active++;
            }
        }
    }

    char tooltip[256];
    if (active > 0) {
        snprintf(tooltip, sizeof(tooltip), "OpenVPN3 Manager - %u active connection%s",
                 active, active == 1 ? "" : "s");
    } else {
        snprintf(tooltip, sizeof(tooltip), "OpenVPN3 Manager - No active connections");
    }
    tray_icon_set_tooltip(tray, tooltip);

    /* Clean up stale auth tracking entries */
    if (auth_launched) {
        if (active == 0) {
            g_hash_table_remove_all(auth_launched);
        } else if (connections) {
            to_remove = NULL;

            g_hash_table_iter_init(&iter, auth_launched);
            while (g_hash_table_iter_next(&iter, &key, &value)) {
                const char *tracked = (const char *)key;
                gboolean found = FALSE;
                for (unsigned int i = 0; i < count; i++) {
                    if (connections[i].session_path &&
                        strcmp(connections[i].session_path, tracked) == 0) {
                        found = TRUE;
                        break;
                    }
                }
                if (!found) {
                    to_remove = g_list_prepend(to_remove, g_strdup(tracked));
                }
            }

            for (GList *l = to_remove; l != NULL; l = l->next) {
                g_hash_table_remove(auth_launched, l->data);
                g_free(l->data);
            }
            g_list_free(to_remove);
        }
    }

    free_connection_info_array(connections, count);
}

/**
 * Update elapsed time labels for connected sessions.
 * Rebuilds menus for CONNECTED indicators to refresh the elapsed time.
 */
void tray_icon_update_timers(TrayIcon *tray, sd_bus *bus) {
    (void)bus;

    if (!tray || !tray->connections) {
        return;
    }

    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init(&iter, tray->connections);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        ConnectionIndicator *ci = (ConnectionIndicator *)value;
        if (ci->state == CONN_STATE_CONNECTED) {
            connection_indicator_rebuild_menu(ci);
        }
    }
}

/**
 * Signal the tray to quit
 */
void tray_icon_quit(TrayIcon *tray) {
    if (!tray) {
        return;
    }
}

/**
 * Clean up tray icon and free all resources
 */
void tray_icon_cleanup(TrayIcon *tray) {
    if (!tray) {
        return;
    }

    /* Destroy all connection indicators */
    if (tray->connections) {
        g_hash_table_destroy(tray->connections);
        tray->connections = NULL;
    }

    /* Free tooltip */
    g_free(tray->tooltip);
    tray->tooltip = NULL;

    /* Cleanup app menu */
    if (tray->menu) {
        gtk_widget_destroy(tray->menu);
        tray->menu = NULL;
    }

    /* Cleanup app indicator */
    if (tray->indicator) {
        g_object_unref(tray->indicator);
        tray->indicator = NULL;
    }

    /* Cleanup session timings */
    if (session_timings) {
        g_hash_table_destroy(session_timings);
        session_timings = NULL;
    }

    /* Cleanup auth launched tracking */
    if (auth_launched) {
        g_hash_table_destroy(auth_launched);
        auth_launched = NULL;
    }

    logger_info("System tray icon cleaned up");

    g_free(tray);
}
