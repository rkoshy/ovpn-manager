#include "tray.h"
#include "dbus/session_client.h"
#include "dbus/config_client.h"
#include "utils/file_chooser.h"
#include "utils/logger.h"
#include "utils/connection_fsm.h"
#include "ui/widgets.h"
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

/**
 * TrayIcon structure
 */
struct TrayIcon {
    AppIndicator *indicator;
    GtkWidget *menu;
    char *tooltip;
};

/* Callback data for session menu items */
typedef struct {
    sd_bus *bus;
    char *session_path;
} SessionCallbackData;

/* Callback data for config menu items */
typedef struct {
    sd_bus *bus;
    char *config_path;
} ConfigCallbackData;

/* Session timing data */
typedef struct {
    char *session_path;
    time_t connect_time;
} SessionTiming;

/* Global session timing array */
static GHashTable *session_timings = NULL;

/* Global hash table to store timer label widgets for efficient updates */
static GHashTable *timer_labels = NULL;  /* session_path -> GtkWidget* (label) */

/* Global hash table to track which sessions have had browser launched for auth */
static GHashTable *auth_launched = NULL;  /* session_path -> gboolean */

/* Menu item tracking for persistent menu structure */
typedef struct {
    GtkWidget *parent_item;      /* The main menu item (parent) */
    GtkWidget *submenu;          /* The submenu container */
    GtkWidget *device_item;      /* Device name metadata item */
    GtkWidget *disconnect_item;  /* Disconnect button */
    GtkWidget *pause_item;       /* Pause button */
    GtkWidget *resume_item;      /* Resume button */
    GtkWidget *auth_item;        /* Authenticate button */
    SessionCallbackData *disconnect_data;
    SessionCallbackData *pause_data;
    SessionCallbackData *resume_data;
    SessionCallbackData *auth_data;
} SessionMenuItem;

typedef struct {
    GtkWidget *parent_item;      /* The main config menu item */
    GtkWidget *submenu;          /* The submenu container */
    GtkWidget *connect_item;     /* Connect button */
    GtkWidget *status_item;      /* "(In use)" status item */
    ConfigCallbackData *connect_data;
} ConfigMenuItem;

/* === NEW UNIFIED CONNECTION STRUCTURES === */

/* ConnectionState enum now defined in connection_fsm.h */

/* Unified callback data for connections */
typedef struct {
    sd_bus *bus;
    char *config_path;           /* Always set */
    char *session_path;          /* NULL if disconnected */
} ConnectionCallbackData;

/* Unified connection menu item */
typedef struct {
    GtkWidget *parent_item;      /* Main menu item with state label */
    GtkWidget *submenu;          /* Submenu container */

    /* All action items (always present) */
    GtkWidget *connect_item;
    GtkWidget *disconnect_item;
    GtkWidget *pause_item;
    GtkWidget *resume_item;
    GtkWidget *auth_item;
    GtkWidget *metadata_item;    /* Device/status info */

    /* Callback data */
    ConnectionCallbackData *callback_data;

    /* State tracking */
    char *config_path;           /* Stable identifier (hash key) */
    char *session_path;          /* NULL if disconnected */
    char *config_name;           /* Display name */
    ConnectionState state;
    time_t connect_time;

    /* FSM for state management */
    ConnectionFsm *fsm;          /* State machine instance */
} ConnectionMenuItem;

/* Merged connection data (temporary struct for building menu) */
typedef struct {
    char *config_path;
    char *config_name;
    char *session_path;          /* NULL if no active session */
    ConnectionState state;
    time_t connect_time;
} ConnectionInfo;

/* === END NEW STRUCTURES === */

/* Hash tables for persistent menu items */
static GHashTable *session_menu_items = NULL;  /* session_path -> SessionMenuItem* */
static GHashTable *config_menu_items = NULL;   /* config_path -> ConfigMenuItem* */
static GHashTable *connection_menu_items = NULL;  /* config_path -> ConnectionMenuItem* (NEW) */

/* Static menu items that never change */
static GtkWidget *static_section_sep = NULL;
static GtkWidget *dashboard_item = NULL;
static GtkWidget *import_item = NULL;
static GtkWidget *settings_item = NULL;
static GtkWidget *quit_item = NULL;

/* Dynamic section widgets */
static GtkWidget *sessions_separator = NULL;
static GtkWidget *no_sessions_item = NULL;
static GtkWidget *configs_separator = NULL;
static GtkWidget *configs_header = NULL;
static GtkWidget *no_configs_item = NULL;
static GtkWidget *loading_configs_item = NULL;

/* NEW: Unified connections section */
static GtkWidget *connections_header = NULL;
static GtkWidget *no_connections_item = NULL;

/* Forward declarations */
static void quit_callback(GtkMenuItem *item, gpointer user_data);
static void show_dashboard_callback(GtkMenuItem *item, gpointer user_data);
static void disconnect_callback(GtkMenuItem *item, gpointer user_data);
static void pause_callback(GtkMenuItem *item, gpointer user_data);
static void resume_callback(GtkMenuItem *item, gpointer user_data);
static void authenticate_callback(GtkMenuItem *item, gpointer user_data);
static void import_config_callback(GtkMenuItem *item, gpointer user_data);
static void connect_config_callback(GtkMenuItem *item, gpointer user_data);

/**
 * Initialize the system tray icon
 */
TrayIcon* tray_icon_init(const char *tooltip) {
    TrayIcon *tray = NULL;

    /* Initialize GTK if not already initialized */
    if (!gtk_init_check(NULL, NULL)) {
        logger_error("Failed to initialize GTK");
        return NULL;
    }

    /* Allocate tray structure */
    tray = g_malloc0(sizeof(TrayIcon));
    if (!tray) {
        logger_error("Failed to allocate TrayIcon");
        return NULL;
    }

    /* Create app indicator */
    tray->indicator = app_indicator_new(
        "ovpn-manager",
        ICON_TRAY_IDLE,  /* Default tray icon */
        APP_INDICATOR_CATEGORY_APPLICATION_STATUS
    );

    if (!tray->indicator) {
        logger_error("Failed to create app indicator");
        g_free(tray);
        return NULL;
    }

    /* Set indicator status to active */
    app_indicator_set_status(tray->indicator, APP_INDICATOR_STATUS_ACTIVE);

    /* Set tooltip */
    if (tooltip) {
        tray->tooltip = g_strdup(tooltip);
        app_indicator_set_title(tray->indicator, tooltip);
    }

    /* Create menu */
    tray->menu = gtk_menu_new();

    /* Set the menu */
    app_indicator_set_menu(tray->indicator, GTK_MENU(tray->menu));

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

    if (tray->tooltip) {
        g_free(tray->tooltip);
    }

    tray->tooltip = g_strdup(tooltip);
    app_indicator_set_title(tray->indicator, tooltip);
}

/**
 * Run the tray icon event loop (non-blocking)
 * This is called periodically by the GLib main loop
 */
int tray_icon_run(TrayIcon *tray) {
    if (!tray) {
        return -1;
    }

    /* Process pending GTK events */
    while (gtk_events_pending()) {
        gtk_main_iteration_do(FALSE);
    }

    return 0;
}

/**
 * Free config callback data
 */
static void free_config_callback_data(gpointer data) {
    ConfigCallbackData *cbd = (ConfigCallbackData *)data;
    if (cbd) {
        g_free(cbd->config_path);
        g_free(cbd);
    }
}

/**
 * Quit callback
 */
static void quit_callback(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;

    logger_info("Quit menu item clicked");

    /* Quit the GApplication */
    extern GApplication *app;  /* Defined in main.c */
    if (app) {
        g_application_quit(app);
    }
}

/**
 * Show dashboard callback
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
 * Free SessionMenuItem structure and its widgets
 */
static void free_session_menu_item(SessionMenuItem *item) {
    if (!item) {
        return;
    }

    /* Destroy the parent widget (this destroys the submenu and all children) */
    if (item->parent_item) {
        gtk_widget_destroy(item->parent_item);
    }

    /* Free callback data */
    if (item->disconnect_data) {
        g_free(item->disconnect_data->session_path);
        g_free(item->disconnect_data);
    }
    if (item->pause_data) {
        g_free(item->pause_data->session_path);
        g_free(item->pause_data);
    }
    if (item->resume_data) {
        g_free(item->resume_data->session_path);
        g_free(item->resume_data);
    }
    if (item->auth_data) {
        g_free(item->auth_data->session_path);
        g_free(item->auth_data);
    }

    g_free(item);
}

/**
 * Free ConfigMenuItem structure and its widgets
 */
static void free_config_menu_item(ConfigMenuItem *item) {
    if (!item) {
        return;
    }

    /* Destroy the parent widget (this destroys the submenu and all children) */
    if (item->parent_item) {
        gtk_widget_destroy(item->parent_item);
    }

    /* Free callback data */
    if (item->connect_data) {
        free_config_callback_data(item->connect_data);
    }

    g_free(item);
}

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

    /* New session - use the actual session_created timestamp from D-Bus */
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
 * Disconnect callback
 */
static void disconnect_callback(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    /* NEW: Support both old SessionCallbackData and new ConnectionCallbackData */
    ConnectionCallbackData *data = (ConnectionCallbackData *)user_data;

    if (!data || !data->bus || !data->session_path) {
        return;
    }

    logger_info("Disconnecting session: %s", data->session_path);

    int r = session_disconnect(data->bus, data->session_path);
    if (r < 0) {
        logger_error("Failed to disconnect session");
    } else {
        /* Remove timing entry on successful disconnect */
        remove_session_timing(data->session_path);
    }
}

/**
 * Pause callback
 */
static void pause_callback(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    /* NEW: Support both old SessionCallbackData and new ConnectionCallbackData */
    ConnectionCallbackData *data = (ConnectionCallbackData *)user_data;

    if (!data || !data->bus || !data->session_path) {
        return;
    }

    logger_info("Pausing session: %s", data->session_path);

    int r = session_pause(data->bus, data->session_path, "User requested");
    if (r < 0) {
        logger_error("Failed to pause session");
    }
}

/**
 * Resume callback
 */
static void resume_callback(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    /* NEW: Support both old SessionCallbackData and new ConnectionCallbackData */
    ConnectionCallbackData *data = (ConnectionCallbackData *)user_data;

    if (!data || !data->bus || !data->session_path) {
        return;
    }

    logger_info("Resuming session: %s", data->session_path);

    int r = session_resume(data->bus, data->session_path);
    if (r < 0) {
        logger_error("Failed to resume session");
    }
}

/**
 * Helper function to launch browser for authentication
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

    /* Launch browser */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "xdg-open '%s' >/dev/null 2>&1 &", auth_url);
    int ret = system(cmd);
    (void)ret;

    g_free(auth_url);
}

/**
 * Authenticate callback - launch browser with OAuth URL
 */
static void authenticate_callback(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    /* NEW: Support both old SessionCallbackData and new ConnectionCallbackData */
    ConnectionCallbackData *data = (ConnectionCallbackData *)user_data;

    if (!data || !data->bus || !data->session_path) {
        return;
    }

    /* Use the helper function */
    launch_auth_browser(data->bus, data->session_path);
}

/* === NEW CONNECTION MANAGEMENT FUNCTIONS === */

/**
 * Map VPN session state to connection state
 */
static ConnectionState get_state_from_session(VpnSession *session) {
    if (!session) {
        return CONN_STATE_DISCONNECTED;
    }

    ConnectionState state;
    const char *session_state_name = NULL;

    switch (session->state) {
        case SESSION_STATE_CONNECTING:
            state = CONN_STATE_CONNECTING;
            session_state_name = "SESSION_STATE_CONNECTING";
            break;
        case SESSION_STATE_CONNECTED:
            state = CONN_STATE_CONNECTED;
            session_state_name = "SESSION_STATE_CONNECTED";
            break;
        case SESSION_STATE_PAUSED:
            state = CONN_STATE_PAUSED;
            session_state_name = "SESSION_STATE_PAUSED";
            break;
        case SESSION_STATE_AUTH_REQUIRED:
            state = CONN_STATE_AUTH_REQUIRED;
            session_state_name = "SESSION_STATE_AUTH_REQUIRED";
            break;
        case SESSION_STATE_ERROR:
            state = CONN_STATE_ERROR;
            session_state_name = "SESSION_STATE_ERROR";
            break;
        case SESSION_STATE_RECONNECTING:
            state = CONN_STATE_RECONNECTING;
            session_state_name = "SESSION_STATE_RECONNECTING";
            break;
        case SESSION_STATE_DISCONNECTED:
        default:
            state = CONN_STATE_DISCONNECTED;
            session_state_name = "SESSION_STATE_DISCONNECTED";
            break;
    }

    if (logger_get_verbosity() >= 2) {
        logger_info("D-Bus session state: %s (%d) -> Connection state: %s, session_path=%s, config_name=%s",
                    session_state_name, session->state,
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
 * Returns array of ConnectionInfo (caller must free)
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
        return NULL;  /* No configs available */
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
                /* Found matching session */
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
                break;  /* One session per config for now */
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

/**
 * Get status symbol for connection state
 */
static const char* get_status_symbol(ConnectionState state) {
    switch (state) {
        case CONN_STATE_DISCONNECTED:
            return STATUS_SYMBOL_DISCONNECTED;
        case CONN_STATE_CONNECTING:
            return STATUS_SYMBOL_CONNECTING;
        case CONN_STATE_RECONNECTING:
            return STATUS_SYMBOL_RECONNECTING;
        case CONN_STATE_CONNECTED:
            return STATUS_SYMBOL_CONNECTED;
        case CONN_STATE_PAUSED:
            return STATUS_SYMBOL_PAUSED;
        case CONN_STATE_AUTH_REQUIRED:
            return STATUS_SYMBOL_AUTHENTICATING;
        case CONN_STATE_ERROR:
            return STATUS_SYMBOL_ERROR;
        default:
            return STATUS_SYMBOL_DISCONNECTED;
    }
}

/**
 * Format connection label with status symbol and state info
 */
static void format_connection_label(ConnectionInfo *conn, char *buffer, size_t size) {
    const char *symbol = get_status_symbol(conn->state);

    switch (conn->state) {
        case CONN_STATE_DISCONNECTED:
            snprintf(buffer, size, "%s %s", symbol, conn->config_name);
            break;

        case CONN_STATE_CONNECTING:
            snprintf(buffer, size, "%s %s: Connecting...", symbol, conn->config_name);
            break;

        case CONN_STATE_CONNECTED: {
            time_t now = time(NULL);
            time_t elapsed = now - conn->connect_time;
            char elapsed_str[32];
            format_elapsed_time(elapsed, elapsed_str, sizeof(elapsed_str));
            snprintf(buffer, size, "%s %s: Connected · %s", symbol, conn->config_name, elapsed_str);
            break;
        }

        case CONN_STATE_PAUSED:
            snprintf(buffer, size, "%s %s: Paused", symbol, conn->config_name);
            break;

        case CONN_STATE_AUTH_REQUIRED:
            snprintf(buffer, size, "%s %s: Auth Required", symbol, conn->config_name);
            break;

        case CONN_STATE_ERROR:
            snprintf(buffer, size, "%s %s: Error", symbol, conn->config_name);
            break;

        case CONN_STATE_RECONNECTING:
            snprintf(buffer, size, "%s %s: Reconnecting...", symbol, conn->config_name);
            break;

        default:
            snprintf(buffer, size, "%s %s", symbol, conn->config_name);
            break;
    }
}

/**
 * Set action button states based on connection FSM state
 */
static void set_action_states(ConnectionMenuItem *item) {
    if (!item || !item->fsm) {
        logger_error("set_action_states called with NULL item or FSM");
        return;
    }

    /* Get button states from FSM */
    ConnectionButtonStates button_states = connection_fsm_get_button_states(item->fsm);
    ConnectionState current_state = connection_fsm_get_state(item->fsm);

    if (logger_get_verbosity() >= 3) {
        logger_debug("set_action_states called: config=%s, FSM state=%s",
                     item->config_name ? item->config_name : "unknown",
                     connection_fsm_state_name(current_state));
        logger_debug("  Button states from FSM: connect=%d, disconnect=%d, pause=%d, resume=%d, auth=%d",
                     button_states.connect_enabled, button_states.disconnect_enabled,
                     button_states.pause_enabled, button_states.resume_enabled,
                     button_states.auth_enabled);
    }

    /* Apply states to widgets */
    gtk_widget_set_sensitive(item->connect_item, button_states.connect_enabled);
    gtk_widget_set_sensitive(item->disconnect_item, button_states.disconnect_enabled);
    gtk_widget_set_sensitive(item->pause_item, button_states.pause_enabled);
    gtk_widget_set_sensitive(item->resume_item, button_states.resume_enabled);
    gtk_widget_set_sensitive(item->auth_item, button_states.auth_enabled);

    if (logger_get_verbosity() >= 3) {
        logger_debug("  Applied sensitivity - disconnect is now: %s",
                     gtk_widget_get_sensitive(item->disconnect_item) ? "SENSITIVE" : "INSENSITIVE");
    }
}

/**
 * Free a ConnectionMenuItem
 */
static void free_connection_menu_item(ConnectionMenuItem *item) {
    if (!item) {
        return;
    }

    /* Destroy FSM */
    if (item->fsm) {
        connection_fsm_destroy(item->fsm);
        item->fsm = NULL;
    }

    /* Free callback data */
    if (item->callback_data) {
        g_free(item->callback_data->config_path);
        g_free(item->callback_data->session_path);
        g_free(item->callback_data);
    }

    /* Free tracked strings */
    g_free(item->config_path);
    g_free(item->session_path);
    g_free(item->config_name);

    /* GTK widgets are freed automatically when removed from menu */

    g_free(item);
}

/**
 * Create a new connection menu item
 */
static ConnectionMenuItem* create_connection_menu_item(TrayIcon *tray, sd_bus *bus, ConnectionInfo *conn) {
    ConnectionMenuItem *item = g_malloc0(sizeof(ConnectionMenuItem));

    /* Store connection info */
    item->config_path = g_strdup(conn->config_path);
    item->session_path = conn->session_path ? g_strdup(conn->session_path) : NULL;
    item->config_name = g_strdup(conn->config_name);
    item->state = conn->state;
    item->connect_time = conn->connect_time;

    /* Create FSM for this connection */
    item->fsm = connection_fsm_create(conn->config_name);
    if (!item->fsm) {
        logger_error("Failed to create FSM for connection '%s'", conn->config_name);
        g_free(item->config_path);
        g_free(item->session_path);
        g_free(item->config_name);
        g_free(item);
        return NULL;
    }

    /* Initialize FSM state based on current connection state */
    ConnectionFsmEvent initial_event;
    switch (conn->state) {
        case CONN_STATE_CONNECTING:
            initial_event = FSM_EVENT_SESSION_CONNECTING;
            connection_fsm_process_event(item->fsm, initial_event);
            break;
        case CONN_STATE_CONNECTED:
            initial_event = FSM_EVENT_SESSION_CONNECTED;
            connection_fsm_process_event(item->fsm, initial_event);
            break;
        case CONN_STATE_PAUSED:
            initial_event = FSM_EVENT_SESSION_PAUSED;
            connection_fsm_process_event(item->fsm, initial_event);
            break;
        case CONN_STATE_AUTH_REQUIRED:
            initial_event = FSM_EVENT_SESSION_AUTH_REQUIRED;
            connection_fsm_process_event(item->fsm, initial_event);
            break;
        case CONN_STATE_ERROR:
            initial_event = FSM_EVENT_SESSION_ERROR;
            connection_fsm_process_event(item->fsm, initial_event);
            break;
        case CONN_STATE_RECONNECTING:
            initial_event = FSM_EVENT_SESSION_RECONNECTING;
            connection_fsm_process_event(item->fsm, initial_event);
            break;
        case CONN_STATE_DISCONNECTED:
        default:
            /* Already in DISCONNECTED state - no event needed */
            break;
    }

    /* Format initial label (includes status symbol) */
    char label[256];
    format_connection_label(conn, label, sizeof(label));

    /* Create parent menu item (no icon - status is in label text) */
    item->parent_item = gtk_menu_item_new_with_label(label);

    /* Create submenu */
    item->submenu = gtk_menu_new();

    /* Create callback data */
    item->callback_data = g_malloc0(sizeof(ConnectionCallbackData));
    item->callback_data->bus = bus;
    item->callback_data->config_path = g_strdup(conn->config_path);
    item->callback_data->session_path = conn->session_path ? g_strdup(conn->session_path) : NULL;

    /* Create all action buttons (always present) */
    item->connect_item = widget_create_menu_item("Connect", ICON_CONNECT, NULL);
    g_signal_connect(item->connect_item, "activate", G_CALLBACK(connect_config_callback), item->callback_data);
    gtk_menu_shell_append(GTK_MENU_SHELL(item->submenu), item->connect_item);
    gtk_widget_show(item->connect_item);

    item->disconnect_item = widget_create_menu_item("Disconnect", ICON_DISCONNECT, NULL);
    g_signal_connect(item->disconnect_item, "activate", G_CALLBACK(disconnect_callback), item->callback_data);
    gtk_menu_shell_append(GTK_MENU_SHELL(item->submenu), item->disconnect_item);
    gtk_widget_show(item->disconnect_item);

    item->pause_item = widget_create_menu_item("Pause", ICON_PAUSE, NULL);
    g_signal_connect(item->pause_item, "activate", G_CALLBACK(pause_callback), item->callback_data);
    gtk_menu_shell_append(GTK_MENU_SHELL(item->submenu), item->pause_item);
    gtk_widget_show(item->pause_item);

    item->resume_item = widget_create_menu_item("Resume", ICON_RESUME, NULL);
    g_signal_connect(item->resume_item, "activate", G_CALLBACK(resume_callback), item->callback_data);
    gtk_menu_shell_append(GTK_MENU_SHELL(item->submenu), item->resume_item);
    gtk_widget_show(item->resume_item);

    item->auth_item = widget_create_menu_item("Authenticate", ICON_AUTHENTICATE, NULL);
    g_signal_connect(item->auth_item, "activate", G_CALLBACK(authenticate_callback), item->callback_data);
    gtk_menu_shell_append(GTK_MENU_SHELL(item->submenu), item->auth_item);
    gtk_widget_show(item->auth_item);

    /* Attach submenu to parent */
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item->parent_item), item->submenu);

    /* Set initial action states from FSM */
    set_action_states(item);

    /* Insert at alphabetically sorted position before static section */
    GList *children = gtk_container_get_children(GTK_CONTAINER(tray->menu));
    int insert_pos = g_list_index(children, static_section_sep);
    if (insert_pos >= 0) {
        gtk_menu_shell_insert(GTK_MENU_SHELL(tray->menu), item->parent_item, insert_pos);
    } else {
        gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), item->parent_item);
    }
    g_list_free(children);

    gtk_widget_show(item->parent_item);

    return item;
}

/**
 * Map connection state to FSM event
 * This determines which event to send to the FSM based on observed state
 */
static ConnectionFsmEvent map_state_to_event(ConnectionState new_state) {
    switch (new_state) {
        case CONN_STATE_CONNECTING:
            return FSM_EVENT_SESSION_CONNECTING;
        case CONN_STATE_CONNECTED:
            return FSM_EVENT_SESSION_CONNECTED;
        case CONN_STATE_PAUSED:
            return FSM_EVENT_SESSION_PAUSED;
        case CONN_STATE_AUTH_REQUIRED:
            return FSM_EVENT_SESSION_AUTH_REQUIRED;
        case CONN_STATE_ERROR:
            return FSM_EVENT_SESSION_ERROR;
        case CONN_STATE_RECONNECTING:
            return FSM_EVENT_SESSION_RECONNECTING;
        case CONN_STATE_DISCONNECTED:
        default:
            return FSM_EVENT_SESSION_DISCONNECTED;
    }
}

/**
 * Update an existing connection menu item
 */
static void update_connection_menu_item(ConnectionMenuItem *item, ConnectionInfo *conn, sd_bus *bus) {
    if (!item || !conn || !item->fsm) {
        logger_error("update_connection_menu_item called with NULL item, conn, or FSM");
        return;
    }

    ConnectionState old_state = item->state;
    ConnectionState new_state = conn->state;

    /* Process state change through FSM */
    if (old_state != new_state) {
        logger_info("Connection '%s' state: %s -> %s, session_path: %s -> %s",
                    conn->config_name ? conn->config_name : "unknown",
                    connection_fsm_state_name(old_state),
                    connection_fsm_state_name(new_state),
                    item->session_path ? item->session_path : "(null)",
                    conn->session_path ? conn->session_path : "(null)");

        /* Determine FSM event from new state */
        ConnectionFsmEvent event = map_state_to_event(new_state);

        /* Process event through FSM */
        ConnectionState fsm_result = connection_fsm_process_event(item->fsm, event);

        /* Update our tracked state to match FSM state */
        item->state = fsm_result;
    } else if (logger_get_verbosity() >= 2) {
        logger_info("Connection '%s' state: %s (no change), session_path: %s -> %s",
                    conn->config_name ? conn->config_name : "unknown",
                    connection_fsm_state_name(old_state),
                    item->session_path ? item->session_path : "(null)",
                    conn->session_path ? conn->session_path : "(null)");
    }

    /* Update connect time */
    item->connect_time = conn->connect_time;

    /* Update session path if changed */
    if (item->session_path != conn->session_path) {
        g_free(item->session_path);
        item->session_path = conn->session_path ? g_strdup(conn->session_path) : NULL;

        /* Update callback data */
        g_free(item->callback_data->session_path);
        item->callback_data->session_path = conn->session_path ? g_strdup(conn->session_path) : NULL;
    }

    /* Update label (includes status symbol) */
    char label[256];
    format_connection_label(conn, label, sizeof(label));
    gtk_menu_item_set_label(GTK_MENU_ITEM(item->parent_item), label);

    /* Update action states from FSM */
    set_action_states(item);

    /* Auto-launch browser for authentication if required */
    if (conn->state == CONN_STATE_AUTH_REQUIRED && conn->session_path && bus) {
        if (!auth_launched) {
            auth_launched = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        }
        if (!g_hash_table_contains(auth_launched, conn->session_path)) {
            launch_auth_browser(bus, conn->session_path);
            g_hash_table_insert(auth_launched, g_strdup(conn->session_path), GINT_TO_POINTER(1));
        }
    }

    /* Show parent item */
    gtk_widget_show(item->parent_item);
}

/* === END NEW CONNECTION FUNCTIONS === */

/**
 * Create a new session menu item with all sub-items
 */
static SessionMenuItem* create_session_menu_item(TrayIcon *tray, sd_bus *bus, VpnSession *session) {
    SessionMenuItem *item = g_malloc0(sizeof(SessionMenuItem));

    /* Create main session menu item (parent) */
    item->parent_item = widget_create_session_item(session, TRUE);

    /* Create submenu */
    item->submenu = gtk_menu_new();

    /* Add device name to submenu */
    char device_text[128];
    snprintf(device_text, sizeof(device_text), "%s",
            session->device_name && session->device_name[0] ? session->device_name : "no device");
    item->device_item = widget_create_metadata_item(device_text);
    gtk_menu_shell_append(GTK_MENU_SHELL(item->submenu), item->device_item);

    /* Add separator */
    GtkWidget *sep = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(item->submenu), sep);
    gtk_widget_show(sep);

    /* Create all possible action buttons (we'll show/hide based on state) */

    /* Disconnect button - always present */
    item->disconnect_data = g_malloc(sizeof(SessionCallbackData));
    item->disconnect_data->bus = bus;
    item->disconnect_data->session_path = g_strdup(session->session_path);
    item->disconnect_item = widget_create_menu_item("Disconnect", ICON_DISCONNECT, NULL);
    g_signal_connect(item->disconnect_item, "activate",
                   G_CALLBACK(disconnect_callback), item->disconnect_data);
    gtk_menu_shell_append(GTK_MENU_SHELL(item->submenu), item->disconnect_item);

    /* Pause button - show when connected */
    item->pause_data = g_malloc(sizeof(SessionCallbackData));
    item->pause_data->bus = bus;
    item->pause_data->session_path = g_strdup(session->session_path);
    item->pause_item = widget_create_menu_item("Pause", ICON_PAUSE, NULL);
    g_signal_connect(item->pause_item, "activate",
                   G_CALLBACK(pause_callback), item->pause_data);
    gtk_menu_shell_append(GTK_MENU_SHELL(item->submenu), item->pause_item);

    /* Resume button - show when paused */
    item->resume_data = g_malloc(sizeof(SessionCallbackData));
    item->resume_data->bus = bus;
    item->resume_data->session_path = g_strdup(session->session_path);
    item->resume_item = widget_create_menu_item("Resume", ICON_RESUME, NULL);
    g_signal_connect(item->resume_item, "activate",
                   G_CALLBACK(resume_callback), item->resume_data);
    gtk_menu_shell_append(GTK_MENU_SHELL(item->submenu), item->resume_item);

    /* Authenticate button - show when auth required */
    item->auth_data = g_malloc(sizeof(SessionCallbackData));
    item->auth_data->bus = bus;
    item->auth_data->session_path = g_strdup(session->session_path);
    item->auth_item = widget_create_menu_item("Authenticate", ICON_AUTHENTICATE, NULL);
    g_signal_connect(item->auth_item, "activate",
                   G_CALLBACK(authenticate_callback), item->auth_data);
    gtk_menu_shell_append(GTK_MENU_SHELL(item->submenu), item->auth_item);

    /* Attach submenu to parent */
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item->parent_item), item->submenu);

    /* Add parent to main menu */
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), item->parent_item);

    return item;
}

/**
 * Update an existing session menu item based on session state
 */
static void update_session_menu_item(SessionMenuItem *item, VpnSession *session, sd_bus *bus) {
    if (!item || !session) {
        return;
    }

    /* Update parent label */
    if (session->state == SESSION_STATE_CONNECTED) {
        time_t start_time = get_session_start_time(session->session_path, session->session_created);
        time_t now = time(NULL);
        time_t elapsed = now - start_time;

        char elapsed_str[32];
        format_elapsed_time(elapsed, elapsed_str, sizeof(elapsed_str));

        char label_text[256];
        snprintf(label_text, sizeof(label_text), "%s: Connected · %s",
                session->config_name ? session->config_name : "Unknown",
                elapsed_str);
        gtk_menu_item_set_label(GTK_MENU_ITEM(item->parent_item), label_text);
    } else {
        /* For other states, use widget_create_session_item logic */
        const char *state_text = "";
        switch (session->state) {
            case SESSION_STATE_CONNECTING:
                state_text = ": Connecting...";
                break;
            case SESSION_STATE_RECONNECTING:
                state_text = ": Reconnecting...";
                break;
            case SESSION_STATE_PAUSED:
                state_text = ": Paused";
                break;
            case SESSION_STATE_AUTH_REQUIRED:
                state_text = ": Auth Required";
                break;
            case SESSION_STATE_ERROR:
                state_text = ": Error";
                break;
            case SESSION_STATE_DISCONNECTED:
                state_text = ": Disconnected";
                break;
            default:
                state_text = "";
                break;
        }

        char label_text[256];
        snprintf(label_text, sizeof(label_text), "%s%s",
                session->config_name ? session->config_name : "Unknown",
                state_text);
        gtk_menu_item_set_label(GTK_MENU_ITEM(item->parent_item), label_text);
    }

    /* Update device name */
    if (item->device_item) {
        char device_text[128];
        snprintf(device_text, sizeof(device_text), "%s",
                session->device_name && session->device_name[0] ? session->device_name : "no device");
        gtk_menu_item_set_label(GTK_MENU_ITEM(item->device_item), device_text);
    }

    /* Update button visibility based on state */
    if (item->disconnect_item) {
        gtk_widget_show(item->disconnect_item);  /* Always show disconnect */
    }

    if (item->pause_item) {
        if (session->state == SESSION_STATE_CONNECTED) {
            gtk_widget_show(item->pause_item);
        } else {
            gtk_widget_hide(item->pause_item);
        }
    }

    if (item->resume_item) {
        if (session->state == SESSION_STATE_PAUSED) {
            gtk_widget_show(item->resume_item);
        } else {
            gtk_widget_hide(item->resume_item);
        }
    }

    if (item->auth_item) {
        if (session->state == SESSION_STATE_AUTH_REQUIRED) {
            gtk_widget_show(item->auth_item);

            /* Auto-launch browser for authentication if required */
            if (!auth_launched) {
                auth_launched = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
            }
            if (!g_hash_table_contains(auth_launched, session->session_path)) {
                launch_auth_browser(bus, session->session_path);
                g_hash_table_insert(auth_launched, g_strdup(session->session_path), GINT_TO_POINTER(1));
            }
        } else {
            gtk_widget_hide(item->auth_item);
        }
    }

    /* Show parent item */
    gtk_widget_show_all(item->parent_item);
}

/**
 * Create a new config menu item
 */
static ConfigMenuItem* create_config_menu_item(TrayIcon *tray, sd_bus *bus, VpnConfig *config, bool in_use) {
    ConfigMenuItem *item = g_malloc0(sizeof(ConfigMenuItem));

    /* Create parent item */
    item->parent_item = widget_create_config_item(
        config->config_name ? config->config_name : "Unknown",
        in_use);

    /* Create submenu */
    item->submenu = gtk_menu_new();

    /* Create status item (for in_use state) */
    item->status_item = gtk_menu_item_new_with_label("(In use)");
    gtk_widget_set_sensitive(item->status_item, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(item->submenu), item->status_item);

    /* Create connect button */
    item->connect_data = g_malloc(sizeof(ConfigCallbackData));
    item->connect_data->bus = bus;
    item->connect_data->config_path = g_strdup(config->config_path);
    item->connect_item = widget_create_menu_item("Connect", ICON_CONNECT, NULL);
    g_signal_connect(item->connect_item, "activate",
                   G_CALLBACK(connect_config_callback), item->connect_data);
    gtk_menu_shell_append(GTK_MENU_SHELL(item->submenu), item->connect_item);

    /* Attach submenu to parent */
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item->parent_item), item->submenu);

    /* Add parent to main menu - insert before static section separator */
    GList *children = gtk_container_get_children(GTK_CONTAINER(tray->menu));
    int insert_pos = g_list_index(children, static_section_sep);
    if (insert_pos >= 0) {
        gtk_menu_shell_insert(GTK_MENU_SHELL(tray->menu), item->parent_item, insert_pos);
    } else {
        /* Fallback: append if separator not found yet */
        gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), item->parent_item);
    }
    g_list_free(children);

    return item;
}

/**
 * Update an existing config menu item
 */
static void update_config_menu_item(ConfigMenuItem *item, VpnConfig *config, bool in_use) {
    if (!item || !config) {
        return;
    }

    /* Update visibility based on in_use state */
    if (in_use) {
        if (item->status_item) {
            gtk_widget_show(item->status_item);
        }
        if (item->connect_item) {
            gtk_widget_hide(item->connect_item);
        }
    } else {
        if (item->status_item) {
            gtk_widget_hide(item->status_item);
        }
        if (item->connect_item) {
            gtk_widget_show(item->connect_item);
        }
    }

    /* Show parent item */
    gtk_widget_show_all(item->parent_item);
}

/**
 * Import config callback
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
        /* User cancelled */
        return;
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
        /* User cancelled */
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
        snprintf(msg, sizeof(msg), "Failed to import configuration '%s'.\n\nCheck if the configuration already exists.", config_name);
        dialog_show_error("Import Error", msg);
    } else {
        logger_info("Successfully imported persistent configuration: %s -> %s", config_name, config_path);
        char msg[256];
        snprintf(msg, sizeof(msg), "Configuration '%s' imported successfully.", config_name);
        dialog_show_info("Import Successful", msg);
        g_free(config_path);
    }

    /* Cleanup */
    g_free(config_name);
    g_free(contents);
    g_free(file_path);
}

/**
 * Connect to config callback
 */
static void connect_config_callback(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    /* NEW: Support both old ConfigCallbackData and new ConnectionCallbackData */
    ConnectionCallbackData *data = (ConnectionCallbackData *)user_data;

    if (!data || !data->bus || !data->config_path) {
        return;
    }

    logger_info("Connecting to config: %s", data->config_path);

    char *session_path = NULL;
    int r = session_start(data->bus, data->config_path, &session_path);
    if (r < 0) {
        logger_error("Failed to start VPN session");
    } else {
        logger_info("Started VPN session: %s", session_path);
        g_free(session_path);
    }
}

/**
 * Initialize static menu items (one-time setup)
 */
static void init_static_menu_items(TrayIcon *tray, sd_bus *bus) {
    if (static_section_sep) {
        return;  /* Already initialized */
    }

    /* Create static separator */
    static_section_sep = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), static_section_sep);
    gtk_widget_show(static_section_sep);

    /* Create "Show Dashboard" menu item */
    dashboard_item = widget_create_menu_item("Show Dashboard", ICON_DASHBOARD, NULL);
    g_signal_connect(dashboard_item, "activate", G_CALLBACK(show_dashboard_callback), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), dashboard_item);
    gtk_widget_show(dashboard_item);

    /* Create "Import Config..." menu item */
    import_item = widget_create_menu_item("Import Config...", ICON_IMPORT, NULL);
    g_signal_connect(import_item, "activate", G_CALLBACK(import_config_callback), bus);
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), import_item);
    gtk_widget_show(import_item);

    /* Create "Settings" menu item (placeholder) */
    settings_item = widget_create_menu_item("Settings", ICON_SETTINGS, NULL);
    gtk_widget_set_sensitive(settings_item, FALSE); /* Not implemented yet */
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), settings_item);
    gtk_widget_show(settings_item);

    /* Create final separator */
    GtkWidget *separator2 = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), separator2);
    gtk_widget_show(separator2);

    /* Create "Quit" menu item */
    quit_item = widget_create_menu_item("Quit", ICON_QUIT, NULL);
    g_signal_connect(quit_item, "activate", G_CALLBACK(quit_callback), tray);
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), quit_item);
    gtk_widget_show(quit_item);
}

/**
 * Update only the timer labels for connected sessions (efficient, no rebuild)
 */
void tray_icon_update_timers(TrayIcon *tray, sd_bus *bus) {
    (void)tray;

    if (!bus || !timer_labels) {
        return;
    }

    /* Get current sessions */
    VpnSession **sessions = NULL;
    unsigned int count = 0;

    int r = session_list(bus, &sessions, &count);
    if (r < 0) {
        return;
    }

    /* Update timer labels for connected sessions */
    for (unsigned int i = 0; i < count; i++) {
        VpnSession *session = sessions[i];

        if (session->state == SESSION_STATE_CONNECTED) {
            GtkWidget *menu_item = g_hash_table_lookup(timer_labels, session->session_path);
            if (menu_item && GTK_IS_MENU_ITEM(menu_item)) {
                time_t start_time = get_session_start_time(session->session_path, session->session_created);
                time_t now = time(NULL);
                time_t elapsed = now - start_time;

                char elapsed_str[32];
                format_elapsed_time(elapsed, elapsed_str, sizeof(elapsed_str));

                /* Update parent menu item label with status symbol: "● config_name: Connected · 2m" */
                char label_text[256];
                snprintf(label_text, sizeof(label_text), "%s %s: Connected · %s",
                        STATUS_SYMBOL_CONNECTED,
                        session->config_name ? session->config_name : "Unknown",
                        elapsed_str);

                gtk_menu_item_set_label(GTK_MENU_ITEM(menu_item), label_text);
            }
        }
    }

    session_list_free(sessions, count);
}
/**
 * Update tray menu with active VPN sessions
 */
void tray_icon_update_sessions(TrayIcon *tray, sd_bus *bus) {
    if (!tray || !bus) {
        return;
    }

    /* Initialize hash tables on first call */
    if (!session_menu_items) {
        session_menu_items = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                   (GDestroyNotify)free_session_menu_item);
    }
    if (!config_menu_items) {
        config_menu_items = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                  (GDestroyNotify)free_config_menu_item);
    }
    /* NEW: Initialize unified connection menu items hash table */
    if (!connection_menu_items) {
        connection_menu_items = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                      (GDestroyNotify)free_connection_menu_item);
    }
    if (!timer_labels) {
        timer_labels = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    }

    /* Initialize static menu items (one-time) */
    init_static_menu_items(tray, bus);

    /* === NEW UNIFIED CONNECTION LOGIC === */

    /* Get merged connection data */
    unsigned int connection_count = 0;
    ConnectionInfo *connections = merge_connections_data(bus, &connection_count);

    /* Track which connections exist now */
    GHashTable *current_connections = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (connections) {
        for (unsigned int i = 0; i < connection_count; i++) {
            g_hash_table_insert(current_connections, g_strdup(connections[i].config_path), GINT_TO_POINTER(1));
        }
    }

    /* Show/hide CONNECTIONS header */
    if (connection_count > 0) {
        if (!connections_header) {
            connections_header = widget_create_section_header("CONNECTIONS");
            /* Insert before static section */
            GList *children = gtk_container_get_children(GTK_CONTAINER(tray->menu));
            int insert_pos = g_list_index(children, static_section_sep);
            if (insert_pos >= 0) {
                gtk_menu_shell_insert(GTK_MENU_SHELL(tray->menu), connections_header, insert_pos);
            }
            g_list_free(children);
        }
        gtk_widget_show(connections_header);

        /* Hide no connections item */
        if (no_connections_item) {
            gtk_widget_hide(no_connections_item);
        }

        /* Update or create connection menu items */
        for (unsigned int i = 0; i < connection_count; i++) {
            ConnectionInfo *conn = &connections[i];

            ConnectionMenuItem *menu_item = g_hash_table_lookup(connection_menu_items, conn->config_path);
            if (menu_item) {
                /* Update existing item */
                update_connection_menu_item(menu_item, conn, bus);
            } else {
                /* Create new item */
                menu_item = create_connection_menu_item(tray, bus, conn);
                g_hash_table_insert(connection_menu_items, g_strdup(conn->config_path), menu_item);
            }

            /* Update timer labels tracking for connected sessions */
            if (conn->state == CONN_STATE_CONNECTED && conn->session_path) {
                if (!g_hash_table_contains(timer_labels, conn->session_path)) {
                    g_hash_table_insert(timer_labels, g_strdup(conn->session_path), menu_item->parent_item);
                }
            } else if (conn->session_path) {
                g_hash_table_remove(timer_labels, conn->session_path);
            }
        }
    } else {
        /* No connections */
        if (connections_header) {
            gtk_widget_hide(connections_header);
        }
        if (!no_connections_item) {
            no_connections_item = gtk_menu_item_new_with_label("No configurations available");
            gtk_widget_set_sensitive(no_connections_item, FALSE);
            GList *children = gtk_container_get_children(GTK_CONTAINER(tray->menu));
            int insert_pos = g_list_index(children, static_section_sep);
            if (insert_pos >= 0) {
                gtk_menu_shell_insert(GTK_MENU_SHELL(tray->menu), no_connections_item, insert_pos);
            }
            g_list_free(children);
        }
        gtk_widget_show(no_connections_item);
    }

    /* Remove connections that no longer exist */
    GHashTableIter iter;
    gpointer key, value;
    GList *to_remove = NULL;

    g_hash_table_iter_init(&iter, connection_menu_items);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        const char *config_path = (const char *)key;
        if (!g_hash_table_contains(current_connections, config_path)) {
            to_remove = g_list_prepend(to_remove, g_strdup(config_path));
        }
    }

    for (GList *l = to_remove; l != NULL; l = l->next) {
        g_hash_table_remove(connection_menu_items, l->data);
        g_free(l->data);
    }
    g_list_free(to_remove);

    g_hash_table_destroy(current_connections);

    /* Update tooltip - count active connections */
    unsigned int active_count = 0;
    if (connections) {
        for (unsigned int i = 0; i < connection_count; i++) {
            if (connections[i].state != CONN_STATE_DISCONNECTED) {
                active_count++;
            }
        }
    }

    if (active_count > 0) {
        char tooltip[256];
        snprintf(tooltip, sizeof(tooltip), "OpenVPN3 Manager - %u active connection%s",
                active_count, active_count == 1 ? "" : "s");
        tray_icon_set_tooltip(tray, tooltip);
    } else {
        tray_icon_set_tooltip(tray, "OpenVPN3 Manager - No active connections");
    }

    /* Clean up auth_launched entries for sessions that no longer exist */
    if (auth_launched && active_count == 0) {
        /* No active sessions - clear all auth tracking */
        g_hash_table_remove_all(auth_launched);
    } else if (auth_launched && connections) {
        /* Remove entries for sessions that are no longer active */
        to_remove = NULL;

        g_hash_table_iter_init(&iter, auth_launched);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            const char *tracked_path = (const char *)key;
            bool found = false;
            for (unsigned int i = 0; i < connection_count; i++) {
                if (connections[i].session_path &&
                    strcmp(connections[i].session_path, tracked_path) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                to_remove = g_list_prepend(to_remove, g_strdup(tracked_path));
            }
        }

        /* Remove stale entries */
        for (GList *l = to_remove; l != NULL; l = l->next) {
            g_hash_table_remove(auth_launched, l->data);
            g_free(l->data);
        }
        g_list_free(to_remove);
    }

    /* Free merged connection data */
    free_connection_info_array(connections, connection_count);

    /* === END NEW UNIFIED CONNECTION LOGIC === */
}


void tray_icon_quit(TrayIcon *tray) {
    if (!tray) {
        return;
    }

    /* Nothing special needed - just stop updating */
}

/**
 * Clean up tray icon and free resources
 */
void tray_icon_cleanup(TrayIcon *tray) {
    if (!tray) {
        return;
    }

    /* Free tooltip */
    if (tray->tooltip) {
        g_free(tray->tooltip);
        tray->tooltip = NULL;
    }

    /* Cleanup GTK menu */
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

    /* Cleanup timer labels */
    if (timer_labels) {
        g_hash_table_destroy(timer_labels);
        timer_labels = NULL;
    }

    /* Cleanup auth launched tracking */
    if (auth_launched) {
        g_hash_table_destroy(auth_launched);
        auth_launched = NULL;
    }

    /* Cleanup session menu items */
    if (session_menu_items) {
        g_hash_table_destroy(session_menu_items);
        session_menu_items = NULL;
    }

    /* Cleanup config menu items */
    if (config_menu_items) {
        g_hash_table_destroy(config_menu_items);
        config_menu_items = NULL;
    }

    /* Null out static menu item pointers (they're destroyed with tray->menu) */
    static_section_sep = NULL;
    dashboard_item = NULL;
    import_item = NULL;
    settings_item = NULL;
    quit_item = NULL;
    sessions_separator = NULL;
    no_sessions_item = NULL;
    configs_separator = NULL;
    configs_header = NULL;
    no_configs_item = NULL;
    loading_configs_item = NULL;

    logger_info("System tray icon cleaned up");

    g_free(tray);
}
