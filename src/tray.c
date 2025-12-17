#include "tray.h"
#include "dbus/session_client.h"
#include "dbus/config_client.h"
#include "utils/file_chooser.h"
#include "ui/widgets.h"
#include "ui/icons.h"
#include "ui/dashboard.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

/* Previous session state for change detection */
typedef struct {
    unsigned int count;
    char **session_paths;
    SessionState *states;
} SessionSnapshot;

static SessionSnapshot prev_snapshot = {0, NULL, NULL};

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
        fprintf(stderr, "Failed to initialize GTK\n");
        return NULL;
    }

    /* Allocate tray structure */
    tray = g_malloc0(sizeof(TrayIcon));
    if (!tray) {
        fprintf(stderr, "Failed to allocate TrayIcon\n");
        return NULL;
    }

    /* Create app indicator */
    tray->indicator = app_indicator_new(
        "ovpn-manager",
        "network-vpn",  /* Icon name from theme */
        APP_INDICATOR_CATEGORY_APPLICATION_STATUS
    );

    if (!tray->indicator) {
        fprintf(stderr, "Failed to create app indicator\n");
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

    /* Add "Quit" menu item */
    GtkWidget *quit_item = widget_create_menu_item("Quit", ICON_QUIT, NULL);
    g_signal_connect(quit_item, "activate", G_CALLBACK(quit_callback), tray);
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), quit_item);
    gtk_widget_show(quit_item);

    /* Set the menu */
    app_indicator_set_menu(tray->indicator, GTK_MENU(tray->menu));

    printf("System tray icon initialized\n");

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

    printf("Quit menu item clicked\n");

    /* Quit the GLib main loop */
    extern GMainLoop *main_loop;  /* Defined in main.c */
    if (main_loop) {
        g_main_loop_quit(main_loop);
    }
}

/**
 * Show dashboard callback
 */
static void show_dashboard_callback(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;

    printf("Show Dashboard menu item clicked\n");

    if (dashboard) {
        dashboard_show(dashboard);
    }
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
    SessionCallbackData *data = (SessionCallbackData *)user_data;

    if (!data || !data->bus || !data->session_path) {
        return;
    }

    printf("Disconnecting session: %s\n", data->session_path);

    int r = session_disconnect(data->bus, data->session_path);
    if (r < 0) {
        fprintf(stderr, "Failed to disconnect session\n");
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
    SessionCallbackData *data = (SessionCallbackData *)user_data;

    if (!data || !data->bus || !data->session_path) {
        return;
    }

    printf("Pausing session: %s\n", data->session_path);

    int r = session_pause(data->bus, data->session_path, "User requested");
    if (r < 0) {
        fprintf(stderr, "Failed to pause session\n");
    }
}

/**
 * Resume callback
 */
static void resume_callback(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    SessionCallbackData *data = (SessionCallbackData *)user_data;

    if (!data || !data->bus || !data->session_path) {
        return;
    }

    printf("Resuming session: %s\n", data->session_path);

    int r = session_resume(data->bus, data->session_path);
    if (r < 0) {
        fprintf(stderr, "Failed to resume session\n");
    }
}

/**
 * Helper function to launch browser for authentication
 */
static void launch_auth_browser(sd_bus *bus, const char *session_path) {
    if (!bus || !session_path) {
        return;
    }

    printf("Auto-launching browser for authentication: %s\n", session_path);

    char *auth_url = NULL;
    int r = session_get_auth_url(bus, session_path, &auth_url);

    /* If queue is empty, try to get URL from session status message */
    if (r < 0 || !auth_url) {
        VpnSession *session = session_get_info(bus, session_path);
        if (session && session->status_message && strstr(session->status_message, "https://")) {
            auth_url = g_strdup(session->status_message);
            printf("Got auth URL from status message: %s\n", auth_url);
        }
        session_free(session);
    }

    if (!auth_url) {
        fprintf(stderr, "Failed to get authentication URL\n");
        return;
    }

    printf("Opening browser for authentication: %s\n", auth_url);

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
    SessionCallbackData *data = (SessionCallbackData *)user_data;

    if (!data || !data->bus || !data->session_path) {
        return;
    }

    /* Use the helper function */
    launch_auth_browser(data->bus, data->session_path);
}

/**
 * Import config callback
 */
static void import_config_callback(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    sd_bus *bus = (sd_bus *)user_data;

    if (!bus) {
        fprintf(stderr, "No D-Bus connection available\n");
        return;
    }

    /* Show file chooser */
    char *file_path = file_chooser_select_ovpn("Import OpenVPN Configuration");
    if (!file_path) {
        /* User cancelled */
        return;
    }

    printf("Selected file: %s\n", file_path);

    /* Read file contents */
    char *contents = NULL;
    char *error = NULL;
    int r = file_read_contents(file_path, &contents, &error);

    if (r < 0) {
        fprintf(stderr, "Failed to read file: %s\n", error ? error : "Unknown error");
        g_free(error);
        g_free(file_path);
        return;
    }

    /* Extract config name from filename */
    char *basename = g_path_get_basename(file_path);
    char *config_name = g_strdup(basename);

    /* Remove .ovpn or .conf extension */
    char *dot = strrchr(config_name, '.');
    if (dot && (strcmp(dot, ".ovpn") == 0 || strcmp(dot, ".conf") == 0)) {
        *dot = '\0';
    }

    /* Import configuration */
    char *config_path = NULL;
    r = config_import(bus, config_name, contents, false, true, &config_path);

    if (r < 0) {
        fprintf(stderr, "Failed to import configuration\n");
    } else {
        printf("Successfully imported configuration: %s\n", config_name);
        g_free(config_path);
    }

    /* Cleanup */
    g_free(config_name);
    g_free(basename);
    g_free(contents);
    g_free(file_path);
}

/**
 * Connect to config callback
 */
static void connect_config_callback(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    ConfigCallbackData *data = (ConfigCallbackData *)user_data;

    if (!data || !data->bus || !data->config_path) {
        return;
    }

    printf("Connecting to config: %s\n", data->config_path);

    char *session_path = NULL;
    int r = session_start(data->bus, data->config_path, &session_path);
    if (r < 0) {
        fprintf(stderr, "Failed to start VPN session\n");
    } else {
        printf("Started VPN session: %s\n", session_path);
        g_free(session_path);
    }
}

/**
 * Check if sessions have changed since last update
 */
static bool sessions_changed(VpnSession **sessions, unsigned int count) {
    /* First update - menu never built before */
    if (prev_snapshot.session_paths == NULL) {
        return true;
    }

    /* Count changed */
    if (prev_snapshot.count != count) {
        return true;
    }

    /* Check if any session paths or states changed */
    for (unsigned int i = 0; i < count; i++) {
        bool found = false;
        for (unsigned int j = 0; j < prev_snapshot.count; j++) {
            if (strcmp(sessions[i]->session_path, prev_snapshot.session_paths[j]) == 0) {
                found = true;
                /* Same session - check if state changed */
                if (sessions[i]->state != prev_snapshot.states[j]) {
                    return true;
                }
                break;
            }
        }
        /* New session */
        if (!found) {
            return true;
        }
    }

    return false;
}

/**
 * Update session snapshot for change detection
 */
static void update_session_snapshot(VpnSession **sessions, unsigned int count) {
    /* Free previous snapshot */
    if (prev_snapshot.session_paths) {
        for (unsigned int i = 0; i < prev_snapshot.count; i++) {
            g_free(prev_snapshot.session_paths[i]);
        }
        g_free(prev_snapshot.session_paths);
        g_free(prev_snapshot.states);
    }

    /* Store new snapshot */
    prev_snapshot.count = count;
    if (count > 0) {
        prev_snapshot.session_paths = g_malloc(sizeof(char*) * count);
        prev_snapshot.states = g_malloc(sizeof(SessionState) * count);
        for (unsigned int i = 0; i < count; i++) {
            prev_snapshot.session_paths[i] = g_strdup(sessions[i]->session_path);
            prev_snapshot.states[i] = sessions[i]->state;
        }
    } else {
        prev_snapshot.session_paths = NULL;
        prev_snapshot.states = NULL;
    }
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
            if (menu_item && GTK_IS_IMAGE_MENU_ITEM(menu_item)) {
                time_t start_time = get_session_start_time(session->session_path, session->session_created);
                time_t now = time(NULL);
                time_t elapsed = now - start_time;

                char elapsed_str[32];
                format_elapsed_time(elapsed, elapsed_str, sizeof(elapsed_str));

                /* Update parent menu item label: "config_name: Connected · 2m" */
                char label_text[256];
                snprintf(label_text, sizeof(label_text), "%s: Connected · %s",
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

    VpnSession **sessions = NULL;
    unsigned int count = 0;

    /* Get active sessions (may fail if service not running - treat as no sessions) */
    int r = session_list(bus, &sessions, &count);
    if (r < 0) {
        /* Service not running or no sessions - treat as empty list */
        sessions = NULL;
        count = 0;
    }

    /* Check if anything changed - if not, skip menu rebuild */
    if (!sessions_changed(sessions, count)) {
        session_list_free(sessions, count);
        return;
    }

    /* Update snapshot */
    update_session_snapshot(sessions, count);

    /* Clear existing menu */
    if (tray->menu) {
        gtk_widget_destroy(tray->menu);
    }

    /* Clear timer labels hash table - menu rebuild will populate it */
    if (timer_labels) {
        g_hash_table_remove_all(timer_labels);
    } else {
        timer_labels = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    }

    /* Create new menu */
    tray->menu = gtk_menu_new();

    /* Add session menu items */
    if (count > 0) {
        for (unsigned int i = 0; i < count; i++) {
            VpnSession *session = sessions[i];

            /* State text is now handled by widget_create_session_item() */

            /* Auto-launch browser for authentication if required */
            if (session->state == SESSION_STATE_AUTH_REQUIRED) {
                /* Initialize auth tracking hash table if needed */
                if (!auth_launched) {
                    auth_launched = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
                }

                /* Check if we haven't already launched browser for this session */
                if (!g_hash_table_contains(auth_launched, session->session_path)) {
                    /* Launch browser automatically */
                    launch_auth_browser(bus, session->session_path);
                    /* Mark as launched */
                    g_hash_table_insert(auth_launched, g_strdup(session->session_path), GINT_TO_POINTER(1));
                }
            } else {
                /* Clear auth launched flag if session is no longer in AUTH_REQUIRED state */
                if (auth_launched) {
                    g_hash_table_remove(auth_launched, session->session_path);
                }
            }

            /* Create main session menu item (parent) with status using custom widget */
            GtkWidget *session_parent = widget_create_session_item(session, TRUE);

            /* Store the parent menu item for efficient timer updates */
            if (session->state == SESSION_STATE_CONNECTED) {
                /* For GtkImageMenuItem, we need to update the whole label */
                g_hash_table_insert(timer_labels, g_strdup(session->session_path), session_parent);
            }

            /* Create submenu for this session */
            GtkWidget *session_submenu = gtk_menu_new();

            /* Add device name to submenu using metadata widget */
            char device_text[128];
            snprintf(device_text, sizeof(device_text), "%s",
                    session->device_name && session->device_name[0] ? session->device_name : "no device");

            GtkWidget *device_item = widget_create_metadata_item(device_text);
            gtk_menu_shell_append(GTK_MENU_SHELL(session_submenu), device_item);

            /* Add separator */
            GtkWidget *sep = gtk_separator_menu_item_new();
            gtk_menu_shell_append(GTK_MENU_SHELL(session_submenu), sep);

            /* Add action buttons based on state */
            if (session->state == SESSION_STATE_CONNECTED) {
                /* Disconnect button */
                SessionCallbackData *disconnect_data = g_malloc(sizeof(SessionCallbackData));
                disconnect_data->bus = bus;
                disconnect_data->session_path = g_strdup(session->session_path);

                GtkWidget *disconnect_item = widget_create_menu_item("Disconnect", ICON_DISCONNECT, NULL);
                g_signal_connect(disconnect_item, "activate",
                               G_CALLBACK(disconnect_callback), disconnect_data);
                g_object_set_data_full(G_OBJECT(disconnect_item), "callback-data",
                                      disconnect_data,
                                      (GDestroyNotify)g_free);
                gtk_menu_shell_append(GTK_MENU_SHELL(session_submenu), disconnect_item);

                /* Pause button */
                SessionCallbackData *pause_data = g_malloc(sizeof(SessionCallbackData));
                pause_data->bus = bus;
                pause_data->session_path = g_strdup(session->session_path);

                GtkWidget *pause_item = widget_create_menu_item("Pause", ICON_PAUSE, NULL);
                g_signal_connect(pause_item, "activate",
                               G_CALLBACK(pause_callback), pause_data);
                g_object_set_data_full(G_OBJECT(pause_item), "callback-data",
                                      pause_data,
                                      (GDestroyNotify)g_free);
                gtk_menu_shell_append(GTK_MENU_SHELL(session_submenu), pause_item);
            } else if (session->state == SESSION_STATE_AUTH_REQUIRED) {
                /* Authenticate button */
                SessionCallbackData *auth_data = g_malloc(sizeof(SessionCallbackData));
                auth_data->bus = bus;
                auth_data->session_path = g_strdup(session->session_path);

                GtkWidget *auth_item = widget_create_menu_item("Authenticate", ICON_AUTHENTICATE, NULL);
                g_signal_connect(auth_item, "activate",
                               G_CALLBACK(authenticate_callback), auth_data);
                g_object_set_data_full(G_OBJECT(auth_item), "callback-data",
                                      auth_data,
                                      (GDestroyNotify)g_free);
                gtk_menu_shell_append(GTK_MENU_SHELL(session_submenu), auth_item);

                /* Disconnect button */
                SessionCallbackData *disconnect_data = g_malloc(sizeof(SessionCallbackData));
                disconnect_data->bus = bus;
                disconnect_data->session_path = g_strdup(session->session_path);

                GtkWidget *disconnect_item = widget_create_menu_item("Disconnect", ICON_DISCONNECT, NULL);
                g_signal_connect(disconnect_item, "activate",
                               G_CALLBACK(disconnect_callback), disconnect_data);
                g_object_set_data_full(G_OBJECT(disconnect_item), "callback-data",
                                      disconnect_data,
                                      (GDestroyNotify)g_free);
                gtk_menu_shell_append(GTK_MENU_SHELL(session_submenu), disconnect_item);
            } else if (session->state == SESSION_STATE_PAUSED) {
                /* Resume button */
                SessionCallbackData *resume_data = g_malloc(sizeof(SessionCallbackData));
                resume_data->bus = bus;
                resume_data->session_path = g_strdup(session->session_path);

                GtkWidget *resume_item = widget_create_menu_item("Resume", ICON_RESUME, NULL);
                g_signal_connect(resume_item, "activate",
                               G_CALLBACK(resume_callback), resume_data);
                g_object_set_data_full(G_OBJECT(resume_item), "callback-data",
                                      resume_data,
                                      (GDestroyNotify)g_free);
                gtk_menu_shell_append(GTK_MENU_SHELL(session_submenu), resume_item);

                /* Disconnect button */
                SessionCallbackData *disconnect_data = g_malloc(sizeof(SessionCallbackData));
                disconnect_data->bus = bus;
                disconnect_data->session_path = g_strdup(session->session_path);

                GtkWidget *disconnect_item = widget_create_menu_item("Disconnect", ICON_DISCONNECT, NULL);
                g_signal_connect(disconnect_item, "activate",
                               G_CALLBACK(disconnect_callback), disconnect_data);
                g_object_set_data_full(G_OBJECT(disconnect_item), "callback-data",
                                      disconnect_data,
                                      (GDestroyNotify)g_free);
                gtk_menu_shell_append(GTK_MENU_SHELL(session_submenu), disconnect_item);
            } else {
                /* For all other states (CONNECTING, RECONNECTING, ERROR, DISCONNECTED):
                 * Just show Disconnect button */
                SessionCallbackData *disconnect_data = g_malloc(sizeof(SessionCallbackData));
                disconnect_data->bus = bus;
                disconnect_data->session_path = g_strdup(session->session_path);

                GtkWidget *disconnect_item = widget_create_menu_item("Disconnect", ICON_DISCONNECT, NULL);
                g_signal_connect(disconnect_item, "activate",
                               G_CALLBACK(disconnect_callback), disconnect_data);
                g_object_set_data_full(G_OBJECT(disconnect_item), "callback-data",
                                      disconnect_data,
                                      (GDestroyNotify)g_free);
                gtk_menu_shell_append(GTK_MENU_SHELL(session_submenu), disconnect_item);
            }

            /* Attach submenu to parent item */
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(session_parent), session_submenu);

            /* Add parent to main menu */
            gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), session_parent);
            gtk_widget_show_all(session_parent);
        }

        /* Add separator */
        GtkWidget *separator = gtk_separator_menu_item_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), separator);
        gtk_widget_show(separator);
    } else {
        /* No active sessions */
        GtkWidget *no_sessions_item = gtk_menu_item_new_with_label("No active VPN sessions");
        gtk_widget_set_sensitive(no_sessions_item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), no_sessions_item);
        gtk_widget_show(no_sessions_item);

        GtkWidget *separator = gtk_separator_menu_item_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), separator);
        gtk_widget_show(separator);
    }

    /* Get available configurations */
    VpnConfig **configs = NULL;
    unsigned int config_count = 0;
    r = config_list(bus, &configs, &config_count);

    printf("DEBUG: config_list returned r=%d, count=%u\n", r, config_count);

    /* Add available configurations section */
    if (r >= 0 && config_count > 0) {
        printf("DEBUG: Adding %u configurations to menu\n", config_count);
        /* Add separator if there were sessions */
        if (count > 0) {
            GtkWidget *sep = gtk_separator_menu_item_new();
            gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), sep);
            gtk_widget_show(sep);
        }

        /* Add header using section header widget */
        GtkWidget *header_item = widget_create_section_header("AVAILABLE CONFIGURATIONS");
        gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), header_item);
        gtk_widget_show(header_item);

        /* Add each configuration */
        for (unsigned int i = 0; i < config_count; i++) {
            VpnConfig *config = configs[i];

            printf("DEBUG: Processing config %u: %s\n", i,
                   config->config_name ? config->config_name : "NULL");

            /* Check if this config is currently in use by any session */
            bool in_use = false;
            for (unsigned int j = 0; j < count; j++) {
                if (sessions[j]->config_name && config->config_name &&
                    strcmp(sessions[j]->config_name, config->config_name) == 0) {
                    in_use = true;
                    printf("DEBUG: Config '%s' is in use by session '%s'\n",
                           config->config_name, sessions[j]->config_name);
                    break;
                }
            }

            /* Create config parent item using custom widget */
            GtkWidget *config_parent = widget_create_config_item(
                config->config_name ? config->config_name : "Unknown",
                in_use);

            printf("DEBUG: Created config menu item for '%s', in_use=%d\n",
                   config->config_name ? config->config_name : "NULL", in_use);

            /* Create submenu for this config */
            GtkWidget *config_submenu = gtk_menu_new();

            /* Add status indicator if in use */
            if (in_use) {
                GtkWidget *status_item = gtk_menu_item_new_with_label("(In use)");
                gtk_widget_set_sensitive(status_item, FALSE);
                gtk_menu_shell_append(GTK_MENU_SHELL(config_submenu), status_item);
            } else {
                /* Add Connect button */
                ConfigCallbackData *connect_data = g_malloc(sizeof(ConfigCallbackData));
                connect_data->bus = bus;
                connect_data->config_path = g_strdup(config->config_path);

                GtkWidget *connect_item = widget_create_menu_item("Connect", ICON_CONNECT, NULL);
                g_signal_connect(connect_item, "activate",
                               G_CALLBACK(connect_config_callback), connect_data);
                g_object_set_data_full(G_OBJECT(connect_item), "callback-data",
                                      connect_data,
                                      free_config_callback_data);
                gtk_menu_shell_append(GTK_MENU_SHELL(config_submenu), connect_item);
            }

            /* Attach submenu to parent */
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(config_parent), config_submenu);

            /* Add to main menu */
            gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), config_parent);
            gtk_widget_show_all(config_parent);
        }

        config_list_free(configs, config_count);

        /* Add separator after configs */
        GtkWidget *sep = gtk_separator_menu_item_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), sep);
        gtk_widget_show(sep);
    } else if (r < 0) {
        /* Config loading failed - show loading indicator */
        if (count > 0) {
            GtkWidget *sep = gtk_separator_menu_item_new();
            gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), sep);
            gtk_widget_show(sep);
        }

        GtkWidget *loading_item = gtk_menu_item_new_with_label("Loading configurations...");
        gtk_widget_set_sensitive(loading_item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), loading_item);
        gtk_widget_show(loading_item);

        GtkWidget *sep = gtk_separator_menu_item_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), sep);
        gtk_widget_show(sep);
    } else if (config_count == 0) {
        /* No configurations available */
        if (count > 0) {
            GtkWidget *sep = gtk_separator_menu_item_new();
            gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), sep);
            gtk_widget_show(sep);
        }

        GtkWidget *no_configs_item = gtk_menu_item_new_with_label("No configurations available");
        gtk_widget_set_sensitive(no_configs_item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), no_configs_item);
        gtk_widget_show(no_configs_item);

        GtkWidget *sep = gtk_separator_menu_item_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), sep);
        gtk_widget_show(sep);
    }

    /* Add "Show Dashboard" menu item */
    GtkWidget *dashboard_item = widget_create_menu_item("Show Dashboard", ICON_CONFIG, NULL);
    g_signal_connect(dashboard_item, "activate", G_CALLBACK(show_dashboard_callback), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), dashboard_item);
    gtk_widget_show(dashboard_item);

    /* Add "Import Config..." menu item */
    GtkWidget *import_item = widget_create_menu_item("Import Config...", ICON_IMPORT, NULL);
    g_signal_connect(import_item, "activate", G_CALLBACK(import_config_callback), bus);
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), import_item);
    gtk_widget_show(import_item);

    /* Add "Settings" menu item (placeholder) */
    GtkWidget *settings_item = widget_create_menu_item("Settings", ICON_SETTINGS, NULL);
    gtk_widget_set_sensitive(settings_item, FALSE); /* Not implemented yet */
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), settings_item);
    gtk_widget_show(settings_item);

    /* Add separator */
    GtkWidget *separator2 = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), separator2);
    gtk_widget_show(separator2);

    /* Add "Quit" menu item */
    GtkWidget *quit_item = widget_create_menu_item("Quit", ICON_QUIT, NULL);
    g_signal_connect(quit_item, "activate", G_CALLBACK(quit_callback), tray);
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->menu), quit_item);
    gtk_widget_show(quit_item);

    /* Set the new menu */
    app_indicator_set_menu(tray->indicator, GTK_MENU(tray->menu));

    /* Update tooltip */
    if (count > 0) {
        char tooltip[256];
        snprintf(tooltip, sizeof(tooltip), "OpenVPN3 Manager - %u active session%s",
                count, count == 1 ? "" : "s");
        tray_icon_set_tooltip(tray, tooltip);
    } else {
        tray_icon_set_tooltip(tray, "OpenVPN3 Manager - No active sessions");
    }

    /* Free session list */
    session_list_free(sessions, count);
}

/**
 * Signal the tray to quit
 */
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

    printf("System tray icon cleaned up\n");

    g_free(tray);
}
