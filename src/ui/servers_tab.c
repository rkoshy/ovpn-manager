#include "servers_tab.h"
#include "../utils/logger.h"
#include "../monitoring/ping_util.h"
#include "../dbus/session_client.h"
#include <string.h>

/**
 * TreeView column indices
 */
enum {
    COL_SERVER_INFO,      /* ServerInfo pointer (hidden) */
    COL_STATUS_ICON,      /* Status icon name */
    COL_CONFIG_NAME,      /* Configuration name */
    COL_SERVER,           /* Server hostname/IP */
    COL_PORT,             /* Port number */
    COL_PROTOCOL,         /* Protocol (UDP/TCP) */
    COL_LATENCY,          /* Latency display string */
    COL_NUM_COLUMNS
};

/**
 * Servers tab structure
 */
struct ServersTab {
    GtkWidget *container;        /* Main container widget */
    GtkWidget *search_entry;     /* Search/filter entry */
    GtkWidget *tree_view;        /* Server list tree view */
    GtkListStore *list_store;    /* Data model */
    GtkWidget *refresh_latency_button; /* Refresh latency button */
    GtkWidget *connect_button;   /* Connect button */
    GtkWidget *disconnect_button; /* Disconnect button */
    GtkWidget *refresh_button;   /* Refresh button */

    GPtrArray *servers;          /* Array of ServerInfo */
    sd_bus *bus;                 /* D-Bus connection */

    int ping_in_progress;        /* Number of pings in progress */
};

/* Forward declarations */
static void on_refresh_latency_clicked(GtkButton *button, gpointer data);
static void on_connect_clicked(GtkButton *button, gpointer data);
static void on_disconnect_clicked(GtkButton *button, gpointer data);
static void on_refresh_clicked(GtkButton *button, gpointer data);
static void on_selection_changed(GtkTreeSelection *selection, gpointer data);
static void on_search_changed(GtkSearchEntry *entry, gpointer data);
static void ping_callback(const char *hostname, int latency_ms, void *user_data);
static void test_all_servers_latency(ServersTab *tab);

/**
 * Free server info structure
 */
static void server_info_free(ServerInfo *info) {
    if (!info) return;

    if (info->config) {
        config_free(info->config);
    }
    g_free(info);
}

/**
 * Create servers tab widget
 */
ServersTab* servers_tab_create(sd_bus *bus) {
    ServersTab *tab = g_malloc0(sizeof(ServersTab));
    if (!tab) return NULL;

    tab->bus = bus;
    tab->servers = g_ptr_array_new_with_free_func((GDestroyNotify)server_info_free);

    /* Main container */
    tab->container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(tab->container), 20);

    /* Header with search and refresh */
    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_bottom(header_box, 12);

    /* Search entry */
    tab->search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(tab->search_entry), "Search servers...");
    gtk_widget_set_hexpand(tab->search_entry, TRUE);
    g_signal_connect(tab->search_entry, "search-changed",
                    G_CALLBACK(on_search_changed), tab);
    gtk_box_pack_start(GTK_BOX(header_box), tab->search_entry, TRUE, TRUE, 0);

    /* Refresh button */
    tab->refresh_button = gtk_button_new_with_label("Refresh");
    g_signal_connect(tab->refresh_button, "clicked",
                    G_CALLBACK(on_refresh_clicked), tab);
    gtk_box_pack_start(GTK_BOX(header_box), tab->refresh_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(tab->container), header_box, FALSE, FALSE, 0);

    /* Scrolled window for tree view */
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scrolled, TRUE);

    /* Create list store */
    tab->list_store = gtk_list_store_new(COL_NUM_COLUMNS,
                                         G_TYPE_POINTER,   /* ServerInfo */
                                         G_TYPE_STRING,    /* Status icon name */
                                         G_TYPE_STRING,    /* Config name */
                                         G_TYPE_STRING,    /* Server hostname/IP */
                                         G_TYPE_INT,       /* Port */
                                         G_TYPE_STRING,    /* Protocol */
                                         G_TYPE_STRING);   /* Latency */

    /* Create tree view */
    tab->tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(tab->list_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tab->tree_view), TRUE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(tab->tree_view), TRUE);
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(tab->tree_view), COL_CONFIG_NAME);

    /* Config Name column with status icon */
    GtkTreeViewColumn *column = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(column, "Configuration Name");
    gtk_tree_view_column_set_sort_column_id(column, COL_CONFIG_NAME);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_column_set_fixed_width(column, 150);

    /* Add icon renderer */
    GtkCellRenderer *icon_renderer = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(column, icon_renderer, FALSE);
    gtk_tree_view_column_add_attribute(column, icon_renderer, "icon-name", COL_STATUS_ICON);

    /* Add text renderer */
    GtkCellRenderer *text_renderer = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(column, text_renderer, TRUE);
    gtk_tree_view_column_add_attribute(column, text_renderer, "text", COL_CONFIG_NAME);

    gtk_tree_view_append_column(GTK_TREE_VIEW(tab->tree_view), column);

    /* Server column (hostname only) */
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes(
        "Server", renderer,
        "text", COL_SERVER,
        NULL);
    gtk_tree_view_column_set_sort_column_id(column, COL_SERVER);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tab->tree_view), column);

    /* Port column */
    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes(
        "Port", renderer,
        "text", COL_PORT,
        NULL);
    gtk_tree_view_column_set_sort_column_id(column, COL_PORT);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tab->tree_view), column);

    /* Protocol column */
    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes(
        "Protocol", renderer,
        "text", COL_PROTOCOL,
        NULL);
    gtk_tree_view_column_set_sort_column_id(column, COL_PROTOCOL);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tab->tree_view), column);

    /* Latency column */
    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes(
        "Latency", renderer,
        "text", COL_LATENCY,
        NULL);
    gtk_tree_view_column_set_sort_column_id(column, COL_LATENCY);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tab->tree_view), column);

    /* Connect selection changed signal */
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tab->tree_view));
    gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);
    g_signal_connect(selection, "changed",
                    G_CALLBACK(on_selection_changed), tab);

    gtk_container_add(GTK_CONTAINER(scrolled), tab->tree_view);
    gtk_box_pack_start(GTK_BOX(tab->container), scrolled, TRUE, TRUE, 0);

    /* Button bar */
    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(button_box, 12);

    /* Refresh Latency button */
    GtkWidget *refresh_icon = gtk_image_new_from_icon_name("view-refresh", GTK_ICON_SIZE_BUTTON);
    tab->refresh_latency_button = gtk_button_new_with_label("Refresh Latency");
    gtk_button_set_image(GTK_BUTTON(tab->refresh_latency_button), refresh_icon);
    gtk_button_set_always_show_image(GTK_BUTTON(tab->refresh_latency_button), TRUE);
    g_signal_connect(tab->refresh_latency_button, "clicked",
                    G_CALLBACK(on_refresh_latency_clicked), tab);
    gtk_box_pack_start(GTK_BOX(button_box), tab->refresh_latency_button, FALSE, FALSE, 0);

    /* Spacer */
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_pack_start(GTK_BOX(button_box), spacer, TRUE, TRUE, 0);

    /* Connect button */
    tab->connect_button = gtk_button_new_with_label("Connect");
    gtk_widget_set_sensitive(tab->connect_button, FALSE);
    g_signal_connect(tab->connect_button, "clicked",
                    G_CALLBACK(on_connect_clicked), tab);
    GtkStyleContext *ctx = gtk_widget_get_style_context(tab->connect_button);
    gtk_style_context_add_class(ctx, "suggested-action");
    gtk_box_pack_start(GTK_BOX(button_box), tab->connect_button, FALSE, FALSE, 0);

    /* Disconnect button */
    tab->disconnect_button = gtk_button_new_with_label("Disconnect");
    gtk_widget_set_sensitive(tab->disconnect_button, FALSE);
    g_signal_connect(tab->disconnect_button, "clicked",
                    G_CALLBACK(on_disconnect_clicked), tab);
    ctx = gtk_widget_get_style_context(tab->disconnect_button);
    gtk_style_context_add_class(ctx, "destructive-action");
    gtk_box_pack_start(GTK_BOX(button_box), tab->disconnect_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(tab->container), button_box, FALSE, FALSE, 0);

    return tab;
}

/**
 * Get the tab widget
 */
GtkWidget* servers_tab_get_widget(ServersTab *tab) {
    return tab ? tab->container : NULL;
}

/**
 * Update tree view row for a server
 */
static void update_server_row(ServersTab *tab, ServerInfo *server, GtkTreeIter *iter) {
    const char *status_icon = NULL;
    if (server->connected) {
        status_icon = "emblem-default";  /* Green checkmark icon */
    }

    char latency_text[32];
    if (server->testing) {
        snprintf(latency_text, sizeof(latency_text), "Testing...");
    } else if (server->latency_ms < 0) {
        snprintf(latency_text, sizeof(latency_text), "--");
    } else {
        snprintf(latency_text, sizeof(latency_text), "%d ms", server->latency_ms);
    }

    gtk_list_store_set(tab->list_store, iter,
                      COL_SERVER_INFO, server,
                      COL_STATUS_ICON, status_icon,
                      COL_CONFIG_NAME, server->config->config_name ? server->config->config_name : "Unknown",
                      COL_SERVER, server->config->server_hostname ? server->config->server_hostname : "--",
                      COL_PORT, server->config->server_port,
                      COL_PROTOCOL, server->config->protocol ? server->config->protocol : "--",
                      COL_LATENCY, latency_text,
                      -1);
}

/**
 * Callback data for ping updates
 */
typedef struct {
    ServersTab *tab;
    ServerInfo *server;
    int latency_ms;
} PingCallbackData;

/**
 * Idle callback to update GUI with ping results
 */
static gboolean ping_update_idle(gpointer user_data) {
    PingCallbackData *data = (PingCallbackData *)user_data;
    ServersTab *tab = data->tab;
    ServerInfo *server = data->server;
    int latency_ms = data->latency_ms;

    /* Update server info */
    server->testing = FALSE;
    server->latency_ms = latency_ms;

    /* Find the server in the list and update the row */
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(tab->list_store), &iter)) {
        do {
            ServerInfo *row_server = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(tab->list_store), &iter,
                             COL_SERVER_INFO, &row_server, -1);
            if (row_server == server) {
                update_server_row(tab, server, &iter);
                break;
            }
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(tab->list_store), &iter));
    }

    /* Decrement ping counter */
    if (tab->ping_in_progress > 0) {
        tab->ping_in_progress--;
    }

    g_free(data);
    return FALSE;  /* Don't call again */
}

/**
 * Ping callback - called when ping completes
 */
static void ping_callback(const char *hostname, int latency_ms, void *user_data) {
    PingCallbackData *data = (PingCallbackData *)user_data;

    /* Update latency result */
    data->latency_ms = latency_ms;

    /* Schedule GUI update on main thread */
    g_idle_add(ping_update_idle, data);
}

/**
 * Test latency for a specific server
 */
static void test_server_latency(ServersTab *tab, ServerInfo *server) {
    if (!server->config->server_hostname) {
        return;
    }

    server->testing = TRUE;
    server->latency_ms = -1;

    /* Update UI to show "Testing..." */
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(tab->list_store), &iter)) {
        do {
            ServerInfo *row_server = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(tab->list_store), &iter,
                             COL_SERVER_INFO, &row_server, -1);
            if (row_server == server) {
                update_server_row(tab, server, &iter);
                break;
            }
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(tab->list_store), &iter));
    }

    /* Create callback data */
    PingCallbackData *callback_data = g_malloc0(sizeof(PingCallbackData));
    callback_data->tab = tab;
    callback_data->server = server;
    callback_data->latency_ms = -1;  /* Will be updated by callback */

    /* Start async ping */
    tab->ping_in_progress++;
    ping_host_async(server->config->server_hostname, 2000, ping_callback, callback_data);
}

/**
 * Test latency for all servers (rate-limited)
 */
static void test_all_servers_latency(ServersTab *tab) {
    /* Rate limiting: max 5 concurrent pings */
    const int max_concurrent = 5;
    int started = 0;

    for (guint i = 0; i < tab->servers->len && started < max_concurrent; i++) {
        ServerInfo *server = g_ptr_array_index(tab->servers, i);
        if (server->config->server_hostname) {
            test_server_latency(tab, server);
            started++;
        }
    }
}

/**
 * Selection changed callback
 */
static void on_selection_changed(GtkTreeSelection *selection, gpointer data) {
    ServersTab *tab = (ServersTab *)data;
    GtkTreeIter iter;
    GtkTreeModel *model;

    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        ServerInfo *server = NULL;
        gtk_tree_model_get(model, &iter, COL_SERVER_INFO, &server, -1);

        if (server) {
            /* Enable/disable buttons based on server state */
            gtk_widget_set_sensitive(tab->connect_button, !server->connected);
            gtk_widget_set_sensitive(tab->disconnect_button, server->connected);
        }
    } else {
        /* No selection */
        gtk_widget_set_sensitive(tab->connect_button, FALSE);
        gtk_widget_set_sensitive(tab->disconnect_button, FALSE);
    }
}

/**
 * Refresh latency button clicked - tests all servers
 */
static void on_refresh_latency_clicked(GtkButton *button, gpointer data) {
    ServersTab *tab = (ServersTab *)data;
    test_all_servers_latency(tab);
}

/**
 * Connect button clicked
 */
static void on_connect_clicked(GtkButton *button, gpointer data) {
    ServersTab *tab = (ServersTab *)data;
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tab->tree_view));
    GtkTreeIter iter;
    GtkTreeModel *model;

    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        ServerInfo *server = NULL;
        gtk_tree_model_get(model, &iter, COL_SERVER_INFO, &server, -1);

        if (server && server->config->config_path) {
            logger_info("Connecting to server: %s (%s)",
                   server->config->config_name,
                   server->config->server_address);

            char *session_path = NULL;
            int r = session_start(tab->bus, server->config->config_path, &session_path);
            if (r < 0) {
                logger_error("Failed to start VPN session");
            } else {
                logger_info("Started VPN session: %s", session_path);
                g_free(session_path);

                /* Update status */
                server->connected = TRUE;
                update_server_row(tab, server, &iter);
                on_selection_changed(selection, tab);
            }
        }
    }
}

/**
 * Disconnect button clicked
 */
static void on_disconnect_clicked(GtkButton *button, gpointer data) {
    ServersTab *tab = (ServersTab *)data;
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tab->tree_view));
    GtkTreeIter iter;
    GtkTreeModel *model;

    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        ServerInfo *server = NULL;
        gtk_tree_model_get(model, &iter, COL_SERVER_INFO, &server, -1);

        if (server && server->connected) {
            /* Find active session for this config */
            VpnSession **sessions = NULL;
            unsigned int session_count = 0;
            int r = session_list(tab->bus, &sessions, &session_count);

            if (r >= 0) {
                for (unsigned int i = 0; i < session_count; i++) {
                    if (sessions[i]->config_name &&
                        server->config->config_name &&
                        strcmp(sessions[i]->config_name, server->config->config_name) == 0) {

                        logger_info("Disconnecting session: %s", sessions[i]->session_path);
                        r = session_disconnect(tab->bus, sessions[i]->session_path);
                        if (r < 0) {
                            logger_error("Failed to disconnect session");
                        } else {
                            server->connected = FALSE;
                            update_server_row(tab, server, &iter);
                            on_selection_changed(selection, tab);
                        }
                        break;
                    }
                }
                session_list_free(sessions, session_count);
            }
        }
    }
}

/**
 * Refresh button clicked
 */
static void on_refresh_clicked(GtkButton *button, gpointer data) {
    ServersTab *tab = (ServersTab *)data;
    servers_tab_refresh(tab, tab->bus);
}

/**
 * Search changed callback
 */
static void on_search_changed(GtkSearchEntry *entry, gpointer data) {
    ServersTab *tab = (ServersTab *)data;
    const char *search_text = gtk_entry_get_text(GTK_ENTRY(entry));

    /* Simple search implementation - rebuild list with filter */
    gtk_list_store_clear(tab->list_store);

    for (guint i = 0; i < tab->servers->len; i++) {
        ServerInfo *server = g_ptr_array_index(tab->servers, i);

        /* Filter by search text */
        if (search_text && strlen(search_text) > 0) {
            gboolean match = FALSE;

            if (server->config->config_name &&
                g_strstr_len(server->config->config_name, -1, search_text)) {
                match = TRUE;
            }
            if (server->config->server_hostname &&
                g_strstr_len(server->config->server_hostname, -1, search_text)) {
                match = TRUE;
            }

            if (!match) continue;
        }

        /* Add to list */
        GtkTreeIter iter;
        gtk_list_store_append(tab->list_store, &iter);
        update_server_row(tab, server, &iter);
    }
}

/**
 * Refresh server list from configurations
 */
void servers_tab_refresh(ServersTab *tab, sd_bus *bus) {
    if (!tab || !bus) return;

    tab->bus = bus;

    /* Get configurations */
    VpnConfig **configs = NULL;
    unsigned int config_count = 0;
    int r = config_list(bus, &configs, &config_count);

    if (r < 0 || config_count == 0) {
        if (configs) {
            config_list_free(configs, config_count);
        }
        return;
    }

    /* Get active sessions to determine connection status */
    VpnSession **sessions = NULL;
    unsigned int session_count = 0;
    session_list(bus, &sessions, &session_count);

    /* First time initialization - create all servers */
    if (tab->servers->len == 0) {
        logger_info("ServersTab: Initial load (found %u configs)", config_count);

        for (unsigned int i = 0; i < config_count; i++) {
            ServerInfo *server = g_malloc0(sizeof(ServerInfo));
            server->config = configs[i];  /* Transfer ownership */
            server->latency_ms = -1;
            server->testing = FALSE;
            server->connected = FALSE;

            /* Check if this config is connected */
            if (sessions && session_count > 0) {
                for (unsigned int j = 0; j < session_count; j++) {
                    if (sessions[j]->config_name &&
                        server->config->config_name &&
                        strcmp(sessions[j]->config_name, server->config->config_name) == 0) {
                        server->connected = TRUE;
                        break;
                    }
                }
            }

            g_ptr_array_add(tab->servers, server);

            logger_info("ServersTab: Added server '%s' (address=%s, connected=%d)",
                   server->config->config_name ? server->config->config_name : "Unknown",
                   server->config->server_address ? server->config->server_address : "N/A",
                   server->connected);

            /* Add to tree view */
            GtkTreeIter iter;
            gtk_list_store_append(tab->list_store, &iter);
            update_server_row(tab, server, &iter);
        }
        g_free(configs);  /* Free array but not configs themselves */

        /* Automatically test latency on initial load */
        test_all_servers_latency(tab);
    } else {
        /* Update existing servers - only update connection status */
        for (guint i = 0; i < tab->servers->len; i++) {
            ServerInfo *server = g_ptr_array_index(tab->servers, i);
            gboolean was_connected = server->connected;
            server->connected = FALSE;

            /* Check if this config is connected */
            if (sessions && session_count > 0) {
                for (unsigned int j = 0; j < session_count; j++) {
                    if (sessions[j]->config_name &&
                        server->config->config_name &&
                        strcmp(sessions[j]->config_name, server->config->config_name) == 0) {
                        server->connected = TRUE;
                        break;
                    }
                }
            }

            /* Only update GUI if connection status changed */
            if (was_connected != server->connected) {
                /* Find and update this row in the tree view */
                GtkTreeIter iter;
                if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(tab->list_store), &iter)) {
                    do {
                        ServerInfo *row_server = NULL;
                        gtk_tree_model_get(GTK_TREE_MODEL(tab->list_store), &iter,
                                         COL_SERVER_INFO, &row_server, -1);
                        if (row_server == server) {
                            update_server_row(tab, server, &iter);
                            break;
                        }
                    } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(tab->list_store), &iter));
                }
            }
        }

        /* Free configs since we didn't use them */
        config_list_free(configs, config_count);
    }

    /* Free session list */
    if (sessions) {
        session_list_free(sessions, session_count);
    }
}

/**
 * Update connection status for servers
 */
void servers_tab_update_status(ServersTab *tab, sd_bus *bus) {
    if (!tab || !bus) return;

    /* Get active sessions */
    VpnSession **sessions = NULL;
    unsigned int session_count = 0;
    int r = session_list(bus, &sessions, &session_count);

    if (r < 0) return;

    /* Update connection status for all servers */
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(tab->list_store), &iter)) {
        do {
            ServerInfo *server = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(tab->list_store), &iter,
                             COL_SERVER_INFO, &server, -1);

            if (server) {
                gboolean was_connected = server->connected;
                server->connected = FALSE;

                /* Check if this config is connected */
                if (sessions && session_count > 0) {
                    for (unsigned int j = 0; j < session_count; j++) {
                        if (sessions[j]->config_name &&
                            server->config->config_name &&
                            strcmp(sessions[j]->config_name, server->config->config_name) == 0) {
                            server->connected = TRUE;
                            break;
                        }
                    }
                }

                /* Update row if status changed */
                if (was_connected != server->connected) {
                    update_server_row(tab, server, &iter);
                }
            }
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(tab->list_store), &iter));
    }

    if (sessions) {
        session_list_free(sessions, session_count);
    }

    /* Update button states based on current selection */
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tab->tree_view));
    on_selection_changed(selection, tab);
}

/**
 * Free servers tab
 */
void servers_tab_free(ServersTab *tab) {
    if (!tab) return;

    if (tab->servers) {
        g_ptr_array_free(tab->servers, TRUE);
    }

    if (tab->list_store) {
        g_object_unref(tab->list_store);
    }

    g_free(tab);
}
