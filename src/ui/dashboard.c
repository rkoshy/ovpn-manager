#include "dashboard.h"
#include "widgets.h"
#include "icons.h"
#include "servers_tab.h"
#include "../dbus/session_client.h"
#include "../dbus/config_client.h"
#include "../monitoring/bandwidth_monitor.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/**
 * Dashboard structure
 */
struct Dashboard {
    GtkWidget *window;
    GtkWidget *notebook;
    GtkWidget *main_box;
    GtkWidget *sessions_container;
    GtkWidget *configs_container;
    /* Tab containers */
    GtkWidget *connections_tab;
    GtkWidget *statistics_tab;
    GtkWidget *servers_tab;
    GtkWidget *routing_tab;
    GtkWidget *security_tab;
    /* Statistics widgets */
    GtkWidget *stats_session_combo;    /* Dropdown to select which session to monitor */
    GtkWidget *stats_duration_label;
    GtkWidget *stats_upload_label;
    GtkWidget *stats_download_label;
    GtkWidget *stats_total_uploaded_label;
    GtkWidget *stats_total_downloaded_label;
    GtkWidget *stats_total_combined_label;
    GtkWidget *stats_packets_sent_label;
    GtkWidget *stats_packets_received_label;
    GtkWidget *stats_packets_errors_label;
    GtkWidget *stats_packets_dropped_label;
    GtkWidget *stats_protocol_label;
    GtkWidget *stats_cipher_label;
    GtkWidget *stats_server_label;
    GtkWidget *stats_local_ip_label;
    GtkWidget *stats_gateway_label;
    GtkWidget *stats_graph_area;  /* Drawing area for bandwidth graph */
    GtkWidget *stats_content_box;  /* Container for all statistics content (shown when connected) */
    GtkWidget *stats_empty_state;  /* Empty state widget (shown when disconnected) */
    /* Bandwidth monitor (for first active session) */
    BandwidthMonitor *bandwidth_monitor;
    char *current_device;
    /* Servers tab instance */
    ServersTab *servers_tab_instance;
    sd_bus *bus;
};

/* Forward declarations */
static void on_window_delete(GtkWidget *widget, GdkEvent *event, gpointer data);
static void on_disconnect_clicked(GtkButton *button, gpointer data);
static void on_connect_clicked(GtkButton *button, gpointer data);
static void on_export_stats_clicked(GtkButton *button, gpointer data);
static void on_reset_stats_clicked(GtkButton *button, gpointer data);
static void on_session_combo_changed(GtkComboBox *combo, gpointer data);
static void create_session_card(Dashboard *dashboard, VpnSession *session);
static void create_config_card(Dashboard *dashboard, VpnConfig *config);
static void create_import_config_row(Dashboard *dashboard);

/**
 * Format elapsed time
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
 * Format bytes to human-readable format
 */
static void format_bytes(uint64_t bytes, char *buffer, size_t buf_size) {
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_index = 0;
    double size = (double)bytes;

    while (size >= 1024.0 && unit_index < 4) {
        size /= 1024.0;
        unit_index++;
    }

    if (unit_index == 0) {
        snprintf(buffer, buf_size, "%.0f %s", size, units[unit_index]);
    } else {
        snprintf(buffer, buf_size, "%.2f %s", size, units[unit_index]);
    }
}

/**
 * Get IP address for a network interface
 */
static int get_interface_ip(const char *device_name, char *ip_buffer, size_t buf_size) {
    struct ifaddrs *ifaddr, *ifa;
    int result = -1;

    if (!device_name || !ip_buffer || buf_size == 0) {
        return -1;
    }

    if (getifaddrs(&ifaddr) == -1) {
        return -1;
    }

    /* Walk through linked list, looking for our device */
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) {
            continue;
        }

        /* Check if this is our device and it has an IPv4 address */
        if (strcmp(ifa->ifa_name, device_name) == 0 &&
            ifa->ifa_addr->sa_family == AF_INET) {

            struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
            const char *ip = inet_ntoa(addr->sin_addr);
            if (ip) {
                snprintf(ip_buffer, buf_size, "%s", ip);
                result = 0;
                break;
            }
        }
    }

    freeifaddrs(ifaddr);
    return result;
}

/**
 * Get gateway IP for a network interface
 */
static int get_interface_gateway(const char *device_name, char *gw_buffer, size_t buf_size) {
    FILE *fp;
    char line[256];
    char iface[32], dest[32], gateway[32];
    unsigned int gw_addr;
    struct in_addr gw_in;

    if (!device_name || !gw_buffer || buf_size == 0) {
        return -1;
    }

    fp = fopen("/proc/net/route", "r");
    if (!fp) {
        return -1;
    }

    /* Skip header line */
    if (fgets(line, sizeof(line), fp) == NULL) {
        fclose(fp);
        return -1;
    }

    /* Find route entry for our interface */
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (sscanf(line, "%31s %31s %31s", iface, dest, gateway) == 3) {
            if (strcmp(iface, device_name) == 0 && strcmp(dest, "00000000") != 0) {
                /* Found a route for this interface - parse gateway */
                if (sscanf(gateway, "%X", &gw_addr) == 1) {
                    gw_in.s_addr = gw_addr;
                    const char *gw_str = inet_ntoa(gw_in);
                    if (gw_str) {
                        snprintf(gw_buffer, buf_size, "%s", gw_str);
                        fclose(fp);
                        return 0;
                    }
                }
            }
        }
    }

    fclose(fp);
    return -1;
}

/**
 * Draw bandwidth graph
 */
static gboolean on_stats_graph_draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
    Dashboard *dashboard = (Dashboard *)data;
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    const int left_margin = 60;  /* Space for Y-axis labels */
    const int top_margin = 20;   /* Space for legend */
    const int bottom_margin = 5;
    const int right_margin = 5;

    /* Background - white for clean look */
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);

    /* Draw legend at top */
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10);

    /* Download legend (vibrant green) */
    cairo_set_source_rgb(cr, 0.2, 0.8, 0.4);
    cairo_rectangle(cr, left_margin, 5, 15, 10);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_move_to(cr, left_margin + 20, 13);
    cairo_show_text(cr, "Download");

    /* Upload legend (vibrant blue) */
    cairo_set_source_rgb(cr, 0.2, 0.5, 0.95);
    cairo_rectangle(cr, left_margin + 100, 5, 15, 10);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_move_to(cr, left_margin + 120, 13);
    cairo_show_text(cr, "Upload");

    /* Get bandwidth samples */
    if (!dashboard->bandwidth_monitor) {
        return TRUE;
    }

    BandwidthSample samples[60];
    unsigned int sample_count = 0;
    if (bandwidth_monitor_get_samples(dashboard->bandwidth_monitor, samples, 60, &sample_count) < 0) {
        return TRUE;
    }

    if (sample_count < 2) {
        return TRUE;
    }

    /* Find max rate for scaling */
    double max_rate = 0.0;
    for (unsigned int i = 0; i < sample_count - 1; i++) {
        double upload_rate = (samples[i].bytes_out - samples[i+1].bytes_out) /
                            (double)(samples[i].timestamp - samples[i+1].timestamp);
        double download_rate = (samples[i].bytes_in - samples[i+1].bytes_in) /
                              (double)(samples[i].timestamp - samples[i+1].timestamp);
        if (upload_rate > max_rate) max_rate = upload_rate;
        if (download_rate > max_rate) max_rate = download_rate;
    }

    if (max_rate < 1024.0) max_rate = 1024.0;  /* Minimum 1 KB/s scale */

    /* Calculate graph area */
    int graph_width = width - left_margin - right_margin;
    int graph_height = height - top_margin - bottom_margin;
    int graph_x = left_margin;
    int graph_y = top_margin;

    /* Draw Y-axis labels */
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_set_font_size(cr, 9);

    for (int i = 0; i <= 4; i++) {
        double rate_value = max_rate * (4 - i) / 4.0;
        char label[32];
        format_bytes((uint64_t)rate_value, label, sizeof(label));
        strcat(label, "/s");

        double y = graph_y + (graph_height / 4.0) * i;

        /* Draw label */
        cairo_text_extents_t extents;
        cairo_text_extents(cr, label, &extents);
        cairo_move_to(cr, left_margin - extents.width - 5, y + 3);
        cairo_show_text(cr, label);
    }

    /* Draw grid lines (lighter color) */
    cairo_set_source_rgba(cr, 0.9, 0.9, 0.9, 0.5);
    cairo_set_line_width(cr, 1.0);
    for (int i = 0; i <= 4; i++) {
        double y = graph_y + (graph_height / 4.0) * i;
        cairo_move_to(cr, graph_x, y);
        cairo_line_to(cr, graph_x + graph_width, y);
    }
    cairo_stroke(cr);

    /* Draw download area with gradient fill (green) */
    cairo_pattern_t *download_gradient = cairo_pattern_create_linear(
        0, graph_y, 0, graph_y + graph_height);
    cairo_pattern_add_color_stop_rgba(download_gradient, 0.0, 0.2, 0.8, 0.4, 0.3);  /* Green with 30% opacity at top */
    cairo_pattern_add_color_stop_rgba(download_gradient, 1.0, 0.2, 0.8, 0.4, 0.05); /* Green with 5% opacity at bottom */

    cairo_move_to(cr, graph_x + graph_width, graph_y + graph_height);

    /* Build points array for spline */
    double points[sample_count][2];
    for (unsigned int i = 0; i < sample_count - 1; i++) {
        double rate = (samples[i].bytes_in - samples[i+1].bytes_in) /
                     (double)(samples[i].timestamp - samples[i+1].timestamp);
        points[i][0] = graph_x + graph_width - (i * graph_width / 60.0);
        points[i][1] = graph_y + graph_height - (rate / max_rate * graph_height);
    }

    /* Draw smooth spline through points */
    for (unsigned int i = 0; i < sample_count - 2; i++) {
        double x0 = (i == 0) ? points[i][0] : points[i-1][0];
        double y0 = (i == 0) ? points[i][1] : points[i-1][1];
        double x1 = points[i][0];
        double y1 = points[i][1];
        double x2 = points[i+1][0];
        double y2 = points[i+1][1];
        double x3 = (i == sample_count - 3) ? points[i+1][0] : points[i+2][0];
        double y3 = (i == sample_count - 3) ? points[i+1][1] : points[i+2][1];

        /* Catmull-Rom to Bezier conversion */
        double cp1x = x1 + (x2 - x0) / 6.0;
        double cp1y = y1 + (y2 - y0) / 6.0;
        double cp2x = x2 - (x3 - x1) / 6.0;
        double cp2y = y2 - (y3 - y1) / 6.0;

        if (i == 0) cairo_move_to(cr, x1, y1);
        cairo_curve_to(cr, cp1x, cp1y, cp2x, cp2y, x2, y2);
    }

    /* Close the path to baseline */
    double last_x = graph_x + graph_width - ((sample_count - 2) * graph_width / 60.0);
    cairo_line_to(cr, last_x, graph_y + graph_height);
    cairo_close_path(cr);
    cairo_set_source(cr, download_gradient);
    cairo_fill(cr);
    cairo_pattern_destroy(download_gradient);

    /* Draw download line on top (vibrant green) */
    cairo_set_source_rgb(cr, 0.2, 0.8, 0.4);
    cairo_set_line_width(cr, 2.5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    for (unsigned int i = 0; i < sample_count - 2; i++) {
        double x0 = (i == 0) ? points[i][0] : points[i-1][0];
        double y0 = (i == 0) ? points[i][1] : points[i-1][1];
        double x1 = points[i][0];
        double y1 = points[i][1];
        double x2 = points[i+1][0];
        double y2 = points[i+1][1];
        double x3 = (i == sample_count - 3) ? points[i+1][0] : points[i+2][0];
        double y3 = (i == sample_count - 3) ? points[i+1][1] : points[i+2][1];

        double cp1x = x1 + (x2 - x0) / 6.0;
        double cp1y = y1 + (y2 - y0) / 6.0;
        double cp2x = x2 - (x3 - x1) / 6.0;
        double cp2y = y2 - (y3 - y1) / 6.0;

        if (i == 0) cairo_move_to(cr, x1, y1);
        cairo_curve_to(cr, cp1x, cp1y, cp2x, cp2y, x2, y2);
    }
    cairo_stroke(cr);

    /* Draw upload area with gradient fill (blue) */
    cairo_pattern_t *upload_gradient = cairo_pattern_create_linear(
        0, graph_y, 0, graph_y + graph_height);
    cairo_pattern_add_color_stop_rgba(upload_gradient, 0.0, 0.2, 0.5, 0.95, 0.3);  /* Blue with 30% opacity at top */
    cairo_pattern_add_color_stop_rgba(upload_gradient, 1.0, 0.2, 0.5, 0.95, 0.05); /* Blue with 5% opacity at bottom */

    cairo_move_to(cr, graph_x + graph_width, graph_y + graph_height);

    /* Build points array for upload spline */
    double upload_points[sample_count][2];
    for (unsigned int i = 0; i < sample_count - 1; i++) {
        double rate = (samples[i].bytes_out - samples[i+1].bytes_out) /
                     (double)(samples[i].timestamp - samples[i+1].timestamp);
        upload_points[i][0] = graph_x + graph_width - (i * graph_width / 60.0);
        upload_points[i][1] = graph_y + graph_height - (rate / max_rate * graph_height);
    }

    /* Draw smooth spline through points */
    for (unsigned int i = 0; i < sample_count - 2; i++) {
        double x0 = (i == 0) ? upload_points[i][0] : upload_points[i-1][0];
        double y0 = (i == 0) ? upload_points[i][1] : upload_points[i-1][1];
        double x1 = upload_points[i][0];
        double y1 = upload_points[i][1];
        double x2 = upload_points[i+1][0];
        double y2 = upload_points[i+1][1];
        double x3 = (i == sample_count - 3) ? upload_points[i+1][0] : upload_points[i+2][0];
        double y3 = (i == sample_count - 3) ? upload_points[i+1][1] : upload_points[i+2][1];

        double cp1x = x1 + (x2 - x0) / 6.0;
        double cp1y = y1 + (y2 - y0) / 6.0;
        double cp2x = x2 - (x3 - x1) / 6.0;
        double cp2y = y2 - (y3 - y1) / 6.0;

        if (i == 0) cairo_move_to(cr, x1, y1);
        cairo_curve_to(cr, cp1x, cp1y, cp2x, cp2y, x2, y2);
    }

    /* Close the path to baseline */
    last_x = graph_x + graph_width - ((sample_count - 2) * graph_width / 60.0);
    cairo_line_to(cr, last_x, graph_y + graph_height);
    cairo_close_path(cr);
    cairo_set_source(cr, upload_gradient);
    cairo_fill(cr);
    cairo_pattern_destroy(upload_gradient);

    /* Draw upload line on top (vibrant blue) */
    cairo_set_source_rgb(cr, 0.2, 0.5, 0.95);
    cairo_set_line_width(cr, 2.5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    for (unsigned int i = 0; i < sample_count - 2; i++) {
        double x0 = (i == 0) ? upload_points[i][0] : upload_points[i-1][0];
        double y0 = (i == 0) ? upload_points[i][1] : upload_points[i-1][1];
        double x1 = upload_points[i][0];
        double y1 = upload_points[i][1];
        double x2 = upload_points[i+1][0];
        double y2 = upload_points[i+1][1];
        double x3 = (i == sample_count - 3) ? upload_points[i+1][0] : upload_points[i+2][0];
        double y3 = (i == sample_count - 3) ? upload_points[i+1][1] : upload_points[i+2][1];

        double cp1x = x1 + (x2 - x0) / 6.0;
        double cp1y = y1 + (y2 - y0) / 6.0;
        double cp2x = x2 - (x3 - x1) / 6.0;
        double cp2y = y2 - (y3 - y1) / 6.0;

        if (i == 0) cairo_move_to(cr, x1, y1);
        cairo_curve_to(cr, cp1x, cp1y, cp2x, cp2y, x2, y2);
    }
    cairo_stroke(cr);

    return TRUE;
}

/**
 * Window delete event handler
 */
static void on_window_delete(GtkWidget *widget, GdkEvent *event, gpointer data) {
    (void)event;
    (void)data;  /* Dashboard pointer not needed for hide */

    /* Hide instead of destroy */
    gtk_widget_hide(widget);

    /* Return TRUE to prevent default destroy */
    g_signal_stop_emission_by_name(widget, "delete-event");
}

/**
 * Disconnect button callback
 */
static void on_disconnect_clicked(GtkButton *button, gpointer data) {
    char *session_path = (char *)data;
    Dashboard *dashboard = g_object_get_data(G_OBJECT(button), "dashboard");

    if (!dashboard || !dashboard->bus || !session_path) {
        return;
    }

    printf("Dashboard: Disconnecting session %s\n", session_path);
    int r = session_disconnect(dashboard->bus, session_path);
    if (r < 0) {
        fprintf(stderr, "Failed to disconnect session\n");
    } else {
        /* Update dashboard after disconnect */
        dashboard_update(dashboard, dashboard->bus);
    }
}

/**
 * Connect button callback
 */
static void on_connect_clicked(GtkButton *button, gpointer data) {
    char *config_path = (char *)data;
    Dashboard *dashboard = g_object_get_data(G_OBJECT(button), "dashboard");

    if (!dashboard || !dashboard->bus || !config_path) {
        return;
    }

    printf("Dashboard: Connecting to config %s\n", config_path);
    char *session_path = NULL;
    int r = session_start(dashboard->bus, config_path, &session_path);
    if (r < 0) {
        fprintf(stderr, "Failed to start VPN session\n");
    } else {
        printf("Started VPN session: %s\n", session_path);
        g_free(session_path);
        /* Update dashboard after connect */
        dashboard_update(dashboard, dashboard->bus);
    }
}

/**
 * Export statistics button callback
 */
static void on_export_stats_clicked(GtkButton *button, gpointer data) {
    Dashboard *dashboard = (Dashboard *)data;

    if (!dashboard || !dashboard->bandwidth_monitor) {
        return;
    }

    /* Create filename with timestamp */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char filename[256];
    strftime(filename, sizeof(filename), "vpn-stats-%Y%m%d-%H%M%S.csv", tm_info);

    /* Open file for writing */
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        fprintf(stderr, "Failed to create statistics file: %s\n", filename);
        return;
    }

    /* Write CSV header */
    fprintf(fp, "Timestamp,Bytes In,Bytes Out,Packets In,Packets Out,Errors In,Errors Out,Dropped In,Dropped Out\n");

    /* Get all samples */
    BandwidthSample samples[60];
    unsigned int sample_count = 0;
    if (bandwidth_monitor_get_samples(dashboard->bandwidth_monitor, samples, 60, &sample_count) >= 0) {
        /* Write samples in chronological order (oldest first) */
        for (int i = sample_count - 1; i >= 0; i--) {
            char timestamp[32];
            struct tm *sample_tm = localtime(&samples[i].timestamp);
            strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", sample_tm);

            fprintf(fp, "%s,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
                   timestamp,
                   samples[i].bytes_in,
                   samples[i].bytes_out,
                   samples[i].packets_in,
                   samples[i].packets_out,
                   samples[i].errors_in,
                   samples[i].errors_out,
                   samples[i].dropped_in,
                   samples[i].dropped_out);
        }
    }

    fclose(fp);
    printf("Statistics exported to: %s\n", filename);
}

/**
 * Reset statistics button callback
 */
static void on_reset_stats_clicked(GtkButton *button, gpointer data) {
    Dashboard *dashboard = (Dashboard *)data;

    if (!dashboard || !dashboard->bandwidth_monitor) {
        return;
    }

    /* Reset the bandwidth monitor */
    bandwidth_monitor_reset(dashboard->bandwidth_monitor);
    printf("Statistics counters reset\n");

    /* Update dashboard to reflect reset values */
    if (dashboard->bus) {
        dashboard_update(dashboard, dashboard->bus);
    }
}

/**
 * Session combo box changed callback
 */
static void on_session_combo_changed(GtkComboBox *combo, gpointer data) {
    Dashboard *dashboard = (Dashboard *)data;

    if (!dashboard || !dashboard->bus) {
        return;
    }

    gint active_index = gtk_combo_box_get_active(combo);
    if (active_index < 0) {
        return;
    }

    /* Build keys for data lookup */
    char key_path[32];
    char key_device[32];
    snprintf(key_path, sizeof(key_path), "session-path-%d", active_index);
    snprintf(key_device, sizeof(key_device), "device-name-%d", active_index);

    /* Get the session path stored with this combo box item */
    char *session_path = g_object_get_data(G_OBJECT(combo), key_path);
    char *device_name = g_object_get_data(G_OBJECT(combo), key_device);

    if (!session_path || !device_name) {
        printf("No session data for index %d\n", active_index);
        return;
    }

    printf("Dashboard: Switching to monitor session %s (device: %s)\n", session_path, device_name);

    /* Recreate bandwidth monitor for the selected session */
    if (dashboard->bandwidth_monitor) {
        bandwidth_monitor_free(dashboard->bandwidth_monitor);
        dashboard->bandwidth_monitor = NULL;
    }

    free(dashboard->current_device);
    dashboard->current_device = NULL;

    /* Create new monitor */
    dashboard->bandwidth_monitor = bandwidth_monitor_create(
        session_path,
        device_name,
        STATS_SOURCE_AUTO,  /* Try D-Bus first, fallback to sysfs */
        60  /* 60-second buffer */
    );
    dashboard->current_device = strdup(device_name);

    if (dashboard->bandwidth_monitor) {
        printf("Dashboard: Switched to monitoring session: %s\n", session_path);
        /* Trigger immediate update */
        dashboard_update(dashboard, dashboard->bus);
    } else {
        printf("Dashboard: Failed to create bandwidth monitor for session: %s\n", session_path);
    }
}

/**
 * Create a session card widget (Active Connection "Hero Row")
 */
static void create_session_card(Dashboard *dashboard, VpnSession *session) {
    if (!session) return;

    /* Create card frame with visible border */
    GtkWidget *card = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(card), GTK_SHADOW_ETCHED_OUT);

    /* Add CSS class for distinct "hero row" styling */
    GtkStyleContext *card_context = gtk_widget_get_style_context(card);
    gtk_style_context_add_class(card_context, "active-connection-card");

    /* Card content */
    GtkWidget *card_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(card_box), 16);

    /* Get state emoji */
    const char *emoji = "🔵";  /* Default: blue circle */
    switch (session->state) {
        case SESSION_STATE_CONNECTED:
            emoji = "🟢";  /* Green circle */
            break;
        case SESSION_STATE_AUTH_REQUIRED:
            emoji = "🟡";  /* Yellow circle */
            break;
        case SESSION_STATE_PAUSED:
            emoji = "⏸️";   /* Pause symbol */
            break;
        case SESSION_STATE_ERROR:
            emoji = "🔴";  /* Red circle */
            break;
        default:
            emoji = "🔵";  /* Blue circle for connecting/other */
            break;
    }

    /* Header line: emoji + name · state · duration */
    const char *state_text = widget_get_state_text(session->state);
    char header_markup[512];

    if (session->state == SESSION_STATE_CONNECTED) {
        time_t now = time(NULL);
        time_t elapsed = now - (time_t)session->session_created;
        char elapsed_str[32];
        format_elapsed_time(elapsed, elapsed_str, sizeof(elapsed_str));

        snprintf(header_markup, sizeof(header_markup),
                "%s <b>%s</b> · %s · %s",
                emoji,
                session->config_name ? session->config_name : "Unknown",
                state_text,
                elapsed_str);
    } else {
        snprintf(header_markup, sizeof(header_markup),
                "%s <b>%s</b> · %s",
                emoji,
                session->config_name ? session->config_name : "Unknown",
                state_text);
    }

    GtkWidget *header_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(header_label), header_markup);
    gtk_label_set_xalign(GTK_LABEL(header_label), 0.0);
    gtk_box_pack_start(GTK_BOX(card_box), header_label, FALSE, FALSE, 0);

    /* Device info with IP */
    if (session->device_name && session->device_name[0]) {
        char ip_address[64];
        char gateway[64];
        char device_markup[256];

        /* Get IP address and gateway from network interface */
        int has_ip = get_interface_ip(session->device_name, ip_address, sizeof(ip_address)) == 0;
        int has_gw = get_interface_gateway(session->device_name, gateway, sizeof(gateway)) == 0;

        if (has_ip && has_gw) {
            snprintf(device_markup, sizeof(device_markup),
                    "<span size='small' foreground='#888888'>%s: %s (remote: %s)</span>",
                    session->device_name, ip_address, gateway);
        } else if (has_ip) {
            snprintf(device_markup, sizeof(device_markup),
                    "<span size='small' foreground='#888888'>%s: %s</span>",
                    session->device_name, ip_address);
        } else {
            snprintf(device_markup, sizeof(device_markup),
                    "<span size='small' foreground='#888888'>%s: No IP</span>",
                    session->device_name);
        }

        GtkWidget *device_label = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(device_label), device_markup);
        gtk_label_set_xalign(GTK_LABEL(device_label), 0.0);
        gtk_box_pack_start(GTK_BOX(card_box), device_label, FALSE, FALSE, 0);
    }

    /* Action buttons */
    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(button_box, 8);

    if (session->state == SESSION_STATE_AUTH_REQUIRED) {
        /* Auth Required: [Authenticate] [Disconnect] */
        GtkWidget *auth_btn = gtk_button_new_with_label("Authenticate");
        g_object_set_data(G_OBJECT(auth_btn), "dashboard", dashboard);
        /* TODO: Implement on_authenticate_clicked */
        gtk_widget_set_sensitive(auth_btn, FALSE);
        gtk_style_context_add_class(gtk_widget_get_style_context(auth_btn), "suggested-action");
        gtk_box_pack_start(GTK_BOX(button_box), auth_btn, FALSE, FALSE, 0);

        GtkWidget *disconnect_btn = gtk_button_new_with_label("Disconnect");
        g_object_set_data(G_OBJECT(disconnect_btn), "dashboard", dashboard);
        g_signal_connect(disconnect_btn, "clicked",
                        G_CALLBACK(on_disconnect_clicked),
                        g_strdup(session->session_path));
        gtk_style_context_add_class(gtk_widget_get_style_context(disconnect_btn), "destructive-action");
        gtk_box_pack_start(GTK_BOX(button_box), disconnect_btn, FALSE, FALSE, 0);

    } else if (session->state == SESSION_STATE_CONNECTED) {
        /* Connected: [Disconnect] [Statistics] [Pause] */
        GtkWidget *disconnect_btn = gtk_button_new_with_label("Disconnect");
        g_object_set_data(G_OBJECT(disconnect_btn), "dashboard", dashboard);
        g_signal_connect(disconnect_btn, "clicked",
                        G_CALLBACK(on_disconnect_clicked),
                        g_strdup(session->session_path));
        gtk_style_context_add_class(gtk_widget_get_style_context(disconnect_btn), "destructive-action");
        gtk_box_pack_start(GTK_BOX(button_box), disconnect_btn, FALSE, FALSE, 0);

        GtkWidget *stats_btn = gtk_button_new_with_label("Statistics");
        g_object_set_data(G_OBJECT(stats_btn), "dashboard", dashboard);
        /* TODO: Implement on_statistics_clicked - switch to Statistics tab */
        gtk_widget_set_sensitive(stats_btn, FALSE);
        gtk_box_pack_start(GTK_BOX(button_box), stats_btn, FALSE, FALSE, 0);

        GtkWidget *pause_btn = gtk_button_new_with_label("Pause");
        g_object_set_data(G_OBJECT(pause_btn), "dashboard", dashboard);
        /* TODO: Implement on_pause_clicked */
        gtk_widget_set_sensitive(pause_btn, FALSE);
        gtk_box_pack_start(GTK_BOX(button_box), pause_btn, FALSE, FALSE, 0);

    } else if (session->state == SESSION_STATE_PAUSED) {
        /* Paused: [Disconnect] [Resume] */
        GtkWidget *disconnect_btn = gtk_button_new_with_label("Disconnect");
        g_object_set_data(G_OBJECT(disconnect_btn), "dashboard", dashboard);
        g_signal_connect(disconnect_btn, "clicked",
                        G_CALLBACK(on_disconnect_clicked),
                        g_strdup(session->session_path));
        gtk_style_context_add_class(gtk_widget_get_style_context(disconnect_btn), "destructive-action");
        gtk_box_pack_start(GTK_BOX(button_box), disconnect_btn, FALSE, FALSE, 0);

        GtkWidget *resume_btn = gtk_button_new_with_label("Resume");
        g_object_set_data(G_OBJECT(resume_btn), "dashboard", dashboard);
        /* TODO: Implement on_resume_clicked */
        gtk_widget_set_sensitive(resume_btn, FALSE);
        gtk_style_context_add_class(gtk_widget_get_style_context(resume_btn), "suggested-action");
        gtk_box_pack_start(GTK_BOX(button_box), resume_btn, FALSE, FALSE, 0);
    }

    gtk_box_pack_start(GTK_BOX(card_box), button_box, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(card), card_box);
    gtk_box_pack_start(GTK_BOX(dashboard->sessions_container), card, FALSE, FALSE, 0);
}

/**
 * Create a configuration list item widget (Action Row style)
 */
static void create_config_card(Dashboard *dashboard, VpnConfig *config) {
    if (!config) return;

    /* Create list box row */
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);

    /* Row content box */
    GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(row_box), 12);

    /* Name label */
    GtkWidget *name_label = gtk_label_new(config->config_name ? config->config_name : "Unknown");
    gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
    gtk_box_pack_start(GTK_BOX(row_box), name_label, TRUE, TRUE, 0);

    /* Connect button */
    GtkWidget *connect_btn = gtk_button_new_with_label("Connect");
    g_object_set_data(G_OBJECT(connect_btn), "dashboard", dashboard);
    g_signal_connect(connect_btn, "clicked",
                    G_CALLBACK(on_connect_clicked),
                    g_strdup(config->config_path));
    gtk_style_context_add_class(gtk_widget_get_style_context(connect_btn), "suggested-action");
    gtk_box_pack_start(GTK_BOX(row_box), connect_btn, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(row), row_box);
    gtk_container_add(GTK_CONTAINER(dashboard->configs_container), row);
}

/**
 * Create the "Import Config" special row at the end of the list
 */
static void create_import_config_row(Dashboard *dashboard) {
    /* Create list box row */
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);

    /* Add CSS class for special styling */
    GtkStyleContext *row_context = gtk_widget_get_style_context(row);
    gtk_style_context_add_class(row_context, "import-config-row");

    /* Row content box */
    GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(row_box), 12);

    /* Plus icon + label */
    GtkWidget *icon_label = gtk_label_new("+");
    gtk_widget_set_size_request(icon_label, 20, 20);
    gtk_box_pack_start(GTK_BOX(row_box), icon_label, FALSE, FALSE, 0);

    GtkWidget *import_label = gtk_label_new("Import new configuration...");
    gtk_label_set_xalign(GTK_LABEL(import_label), 0.0);
    GtkStyleContext *label_context = gtk_widget_get_style_context(import_label);
    gtk_style_context_add_class(label_context, "dim-label");
    gtk_box_pack_start(GTK_BOX(row_box), import_label, TRUE, TRUE, 0);

    gtk_container_add(GTK_CONTAINER(row), row_box);
    gtk_container_add(GTK_CONTAINER(dashboard->configs_container), row);
}

/**
 * Create dashboard window
 */
Dashboard* dashboard_create(void) {
    Dashboard *dashboard = g_malloc0(sizeof(Dashboard));
    if (!dashboard) {
        return NULL;
    }

    /* Create window */
    dashboard->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(dashboard->window), "OpenVPN Manager");
    gtk_window_set_default_size(GTK_WINDOW(dashboard->window), 780, 600);
    gtk_window_set_position(GTK_WINDOW(dashboard->window), GTK_WIN_POS_CENTER);
    gtk_container_set_border_width(GTK_CONTAINER(dashboard->window), 0);

    /* Apply window styling */
    GtkStyleContext *window_context = gtk_widget_get_style_context(dashboard->window);
    gtk_style_context_add_class(window_context, "dashboard-window");

    /* Connect delete event */
    g_signal_connect(dashboard->window, "delete-event",
                    G_CALLBACK(on_window_delete), dashboard);

    /* Create notebook for tabs */
    dashboard->notebook = gtk_notebook_new();
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(dashboard->notebook), GTK_POS_TOP);

    /* Tab 1: Connections (existing functionality) */
    GtkWidget *connections_scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(connections_scrolled),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);

    dashboard->connections_tab = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* Sessions section */
    GtkWidget *sessions_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(sessions_header), 20);
    gtk_widget_set_margin_top(sessions_header, 12);
    gtk_widget_set_margin_bottom(sessions_header, 8);

    GtkWidget *sessions_title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(sessions_title),
                        "<span size='large' weight='600'>Active Connections</span>");
    gtk_label_set_xalign(GTK_LABEL(sessions_title), 0.0);
    gtk_box_pack_start(GTK_BOX(sessions_header), sessions_title, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(dashboard->connections_tab), sessions_header, FALSE, FALSE, 0);

    /* Sessions container */
    dashboard->sessions_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(dashboard->sessions_container), 20);
    gtk_widget_set_margin_top(dashboard->sessions_container, 0);
    gtk_box_pack_start(GTK_BOX(dashboard->connections_tab), dashboard->sessions_container, FALSE, FALSE, 0);

    /* Configurations section */
    GtkWidget *configs_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(configs_header), 20);
    gtk_widget_set_margin_top(configs_header, 12);
    gtk_widget_set_margin_bottom(configs_header, 8);

    GtkWidget *configs_title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(configs_title),
                        "<span size='large' weight='600'>Available Configurations</span>");
    gtk_label_set_xalign(GTK_LABEL(configs_title), 0.0);
    gtk_box_pack_start(GTK_BOX(configs_header), configs_title, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(dashboard->connections_tab), configs_header, FALSE, FALSE, 0);

    /* Configurations container - create boxed list frame */
    GtkWidget *configs_frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(configs_frame), GTK_SHADOW_IN);
    gtk_container_set_border_width(GTK_CONTAINER(configs_frame), 20);
    gtk_widget_set_margin_top(configs_frame, 0);

    /* Apply boxed-list CSS class for modern styling */
    GtkStyleContext *configs_frame_context = gtk_widget_get_style_context(configs_frame);
    gtk_style_context_add_class(configs_frame_context, "boxed-list");

    /* Create list box for configurations */
    dashboard->configs_container = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(dashboard->configs_container), GTK_SELECTION_NONE);

    gtk_container_add(GTK_CONTAINER(configs_frame), dashboard->configs_container);
    gtk_box_pack_start(GTK_BOX(dashboard->connections_tab), configs_frame, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(connections_scrolled), dashboard->connections_tab);
    gtk_notebook_append_page(GTK_NOTEBOOK(dashboard->notebook), connections_scrolled,
                            gtk_label_new("Connections"));

    /* Tab 2: Statistics */
    dashboard->statistics_tab = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(dashboard->statistics_tab), 20);

    /* Header row: Session selector + Duration */
    GtkWidget *stats_header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);

    /* Session selector combo box */
    GtkWidget *session_selector_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *session_label = gtk_label_new("Session:");
    gtk_box_pack_start(GTK_BOX(session_selector_box), session_label, FALSE, FALSE, 0);

    dashboard->stats_session_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(dashboard->stats_session_combo), "No active sessions");
    gtk_combo_box_set_active(GTK_COMBO_BOX(dashboard->stats_session_combo), 0);
    g_signal_connect(dashboard->stats_session_combo, "changed",
                    G_CALLBACK(on_session_combo_changed), dashboard);
    gtk_box_pack_start(GTK_BOX(session_selector_box), dashboard->stats_session_combo, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(stats_header_box), session_selector_box, FALSE, FALSE, 0);

    /* Duration label */
    dashboard->stats_duration_label = gtk_label_new("Duration: --");
    gtk_label_set_xalign(GTK_LABEL(dashboard->stats_duration_label), 1.0);
    gtk_box_pack_start(GTK_BOX(stats_header_box), dashboard->stats_duration_label, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(dashboard->statistics_tab), stats_header_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(dashboard->statistics_tab), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);

    /* Create content container (shown when connected) */
    dashboard->stats_content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_box_pack_start(GTK_BOX(dashboard->statistics_tab), dashboard->stats_content_box, TRUE, TRUE, 0);

    /* Create empty state (shown when disconnected) */
    dashboard->stats_empty_state = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_widget_set_valign(dashboard->stats_empty_state, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(dashboard->stats_empty_state, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(dashboard->stats_empty_state, TRUE);

    /* Empty state icon */
    GtkWidget *empty_icon = gtk_image_new_from_icon_name("network-offline-symbolic", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(empty_icon), 96);
    gtk_widget_set_opacity(empty_icon, 0.3);
    gtk_box_pack_start(GTK_BOX(dashboard->stats_empty_state), empty_icon, FALSE, FALSE, 0);

    /* Empty state message */
    GtkWidget *empty_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(empty_label),
                        "<span size='large' weight='600'>No active connection</span>\n"
                        "<span foreground='#888888'>Connect to a VPN to see statistics</span>");
    gtk_label_set_justify(GTK_LABEL(empty_label), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(dashboard->stats_empty_state), empty_label, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(dashboard->statistics_tab), dashboard->stats_empty_state, TRUE, TRUE, 0);

    /* Initially hide empty state (will be shown/hidden in dashboard_update) */
    gtk_widget_set_no_show_all(dashboard->stats_empty_state, TRUE);

    /* Bandwidth section */
    GtkWidget *bandwidth_header = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(bandwidth_header),
                        "<span size='large' weight='600'>Bandwidth (Live - 2s refresh)</span>");
    gtk_label_set_xalign(GTK_LABEL(bandwidth_header), 0.0);
    gtk_widget_set_margin_top(bandwidth_header, 8);
    gtk_widget_set_margin_bottom(bandwidth_header, 8);
    gtk_box_pack_start(GTK_BOX(dashboard->stats_content_box), bandwidth_header, FALSE, FALSE, 0);

    /* Bandwidth frame - no visible border for clean look */
    GtkWidget *bandwidth_frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(bandwidth_frame), GTK_SHADOW_NONE);
    GtkWidget *bandwidth_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(bandwidth_box), 12);

    /* Upload rate */
    dashboard->stats_upload_label = gtk_label_new("  ▲ Upload:   0 B/s");
    gtk_label_set_xalign(GTK_LABEL(dashboard->stats_upload_label), 0.0);
    gtk_box_pack_start(GTK_BOX(bandwidth_box), dashboard->stats_upload_label, FALSE, FALSE, 0);

    /* Download rate */
    dashboard->stats_download_label = gtk_label_new("  ▼ Download: 0 B/s");
    gtk_label_set_xalign(GTK_LABEL(dashboard->stats_download_label), 0.0);
    gtk_box_pack_start(GTK_BOX(bandwidth_box), dashboard->stats_download_label, FALSE, FALSE, 0);

    /* Bandwidth graph */
    dashboard->stats_graph_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(dashboard->stats_graph_area, -1, 120);
    gtk_widget_set_margin_top(dashboard->stats_graph_area, 8);
    g_signal_connect(dashboard->stats_graph_area, "draw",
                    G_CALLBACK(on_stats_graph_draw), dashboard);
    gtk_box_pack_start(GTK_BOX(bandwidth_box), dashboard->stats_graph_area, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(bandwidth_frame), bandwidth_box);
    gtk_box_pack_start(GTK_BOX(dashboard->stats_content_box), bandwidth_frame, FALSE, FALSE, 0);

    /* Separator */
    gtk_box_pack_start(GTK_BOX(dashboard->stats_content_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 8);

    /* 4-column grid for Total Transfer + Packet Statistics */
    GtkWidget *stats_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(stats_grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(stats_grid), 8);
    gtk_grid_set_column_homogeneous(GTK_GRID(stats_grid), TRUE);  /* Equal width columns (25% each) */
    gtk_widget_set_hexpand(stats_grid, TRUE);  /* Expand to fill 100% width */
    gtk_widget_set_margin_top(stats_grid, 8);

    /* Row 0: Section headers */
    GtkWidget *total_header = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(total_header),
                        "<span size='large' weight='600'>Total Transfer</span>");
    gtk_label_set_xalign(GTK_LABEL(total_header), 0.0);
    gtk_grid_attach(GTK_GRID(stats_grid), total_header, 0, 0, 2, 1);  /* Span 2 columns */

    GtkWidget *packets_header = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(packets_header),
                        "<span size='large' weight='600'>Packet Statistics</span>");
    gtk_label_set_xalign(GTK_LABEL(packets_header), 0.0);
    gtk_grid_attach(GTK_GRID(stats_grid), packets_header, 2, 0, 2, 1);  /* Span 2 columns */

    /* Row 1: Uploaded / Sent */
    GtkWidget *uploaded_label = gtk_label_new("Uploaded:");
    gtk_label_set_xalign(GTK_LABEL(uploaded_label), 0.0);
    gtk_grid_attach(GTK_GRID(stats_grid), uploaded_label, 0, 1, 1, 1);

    dashboard->stats_total_uploaded_label = gtk_label_new("0 B");
    gtk_label_set_xalign(GTK_LABEL(dashboard->stats_total_uploaded_label), 1.0);  /* Right-align */
    gtk_grid_attach(GTK_GRID(stats_grid), dashboard->stats_total_uploaded_label, 1, 1, 1, 1);

    GtkWidget *sent_label = gtk_label_new("Sent:");
    gtk_label_set_xalign(GTK_LABEL(sent_label), 0.0);
    gtk_grid_attach(GTK_GRID(stats_grid), sent_label, 2, 1, 1, 1);

    dashboard->stats_packets_sent_label = gtk_label_new("0 packets");
    gtk_label_set_xalign(GTK_LABEL(dashboard->stats_packets_sent_label), 1.0);  /* Right-align */
    gtk_grid_attach(GTK_GRID(stats_grid), dashboard->stats_packets_sent_label, 3, 1, 1, 1);

    /* Row 2: Downloaded / Received */
    GtkWidget *downloaded_label = gtk_label_new("Downloaded:");
    gtk_label_set_xalign(GTK_LABEL(downloaded_label), 0.0);
    gtk_grid_attach(GTK_GRID(stats_grid), downloaded_label, 0, 2, 1, 1);

    dashboard->stats_total_downloaded_label = gtk_label_new("0 B");
    gtk_label_set_xalign(GTK_LABEL(dashboard->stats_total_downloaded_label), 1.0);  /* Right-align */
    gtk_grid_attach(GTK_GRID(stats_grid), dashboard->stats_total_downloaded_label, 1, 2, 1, 1);

    GtkWidget *received_label = gtk_label_new("Received:");
    gtk_label_set_xalign(GTK_LABEL(received_label), 0.0);
    gtk_grid_attach(GTK_GRID(stats_grid), received_label, 2, 2, 1, 1);

    dashboard->stats_packets_received_label = gtk_label_new("0 packets");
    gtk_label_set_xalign(GTK_LABEL(dashboard->stats_packets_received_label), 1.0);  /* Right-align */
    gtk_grid_attach(GTK_GRID(stats_grid), dashboard->stats_packets_received_label, 3, 2, 1, 1);

    /* Row 3: Total / Errors */
    GtkWidget *total_label = gtk_label_new("Total:");
    gtk_label_set_xalign(GTK_LABEL(total_label), 0.0);
    gtk_grid_attach(GTK_GRID(stats_grid), total_label, 0, 3, 1, 1);

    dashboard->stats_total_combined_label = gtk_label_new("0 B");
    gtk_label_set_xalign(GTK_LABEL(dashboard->stats_total_combined_label), 1.0);  /* Right-align */
    gtk_grid_attach(GTK_GRID(stats_grid), dashboard->stats_total_combined_label, 1, 3, 1, 1);

    GtkWidget *errors_label = gtk_label_new("Errors:");
    gtk_label_set_xalign(GTK_LABEL(errors_label), 0.0);
    gtk_grid_attach(GTK_GRID(stats_grid), errors_label, 2, 3, 1, 1);

    dashboard->stats_packets_errors_label = gtk_label_new("0");
    gtk_label_set_xalign(GTK_LABEL(dashboard->stats_packets_errors_label), 1.0);  /* Right-align */
    gtk_grid_attach(GTK_GRID(stats_grid), dashboard->stats_packets_errors_label, 3, 3, 1, 1);

    /* Row 4: Empty / Dropped */
    GtkWidget *dropped_label = gtk_label_new("Dropped:");
    gtk_label_set_xalign(GTK_LABEL(dropped_label), 0.0);
    gtk_grid_attach(GTK_GRID(stats_grid), dropped_label, 2, 4, 1, 1);

    dashboard->stats_packets_dropped_label = gtk_label_new("0");
    gtk_label_set_xalign(GTK_LABEL(dashboard->stats_packets_dropped_label), 1.0);  /* Right-align */
    gtk_grid_attach(GTK_GRID(stats_grid), dashboard->stats_packets_dropped_label, 3, 4, 1, 1);

    gtk_box_pack_start(GTK_BOX(dashboard->stats_content_box), stats_grid, FALSE, FALSE, 0);

    /* Separator */
    gtk_box_pack_start(GTK_BOX(dashboard->stats_content_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 8);

    /* Connection Details section */
    GtkWidget *details_header = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(details_header),
                        "<span size='large' weight='600'>Connection Details</span>");
    gtk_label_set_xalign(GTK_LABEL(details_header), 0.0);
    gtk_widget_set_margin_bottom(details_header, 8);
    gtk_box_pack_start(GTK_BOX(dashboard->stats_content_box), details_header, FALSE, FALSE, 0);

    /* Protocol - with icon */
    GtkWidget *protocol_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *protocol_icon = gtk_image_new_from_icon_name("network-wired-symbolic", GTK_ICON_SIZE_MENU);
    gtk_box_pack_start(GTK_BOX(protocol_box), protocol_icon, FALSE, FALSE, 0);
    dashboard->stats_protocol_label = gtk_label_new("Protocol: --");
    gtk_label_set_xalign(GTK_LABEL(dashboard->stats_protocol_label), 0.0);
    gtk_box_pack_start(GTK_BOX(protocol_box), dashboard->stats_protocol_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(dashboard->stats_content_box), protocol_box, FALSE, FALSE, 0);

    /* Cipher - with icon */
    GtkWidget *cipher_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *cipher_icon = gtk_image_new_from_icon_name("channel-secure-symbolic", GTK_ICON_SIZE_MENU);
    gtk_box_pack_start(GTK_BOX(cipher_box), cipher_icon, FALSE, FALSE, 0);
    dashboard->stats_cipher_label = gtk_label_new("Cipher:   --");
    gtk_label_set_xalign(GTK_LABEL(dashboard->stats_cipher_label), 0.0);
    gtk_box_pack_start(GTK_BOX(cipher_box), dashboard->stats_cipher_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(dashboard->stats_content_box), cipher_box, FALSE, FALSE, 0);

    /* Server - with icon */
    GtkWidget *server_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *server_icon = gtk_image_new_from_icon_name("network-server-symbolic", GTK_ICON_SIZE_MENU);
    gtk_box_pack_start(GTK_BOX(server_box), server_icon, FALSE, FALSE, 0);
    dashboard->stats_server_label = gtk_label_new("Server:   --");
    gtk_label_set_xalign(GTK_LABEL(dashboard->stats_server_label), 0.0);
    gtk_box_pack_start(GTK_BOX(server_box), dashboard->stats_server_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(dashboard->stats_content_box), server_box, FALSE, FALSE, 0);

    /* Local IP - with icon */
    GtkWidget *localip_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *localip_icon = gtk_image_new_from_icon_name("network-workgroup-symbolic", GTK_ICON_SIZE_MENU);
    gtk_box_pack_start(GTK_BOX(localip_box), localip_icon, FALSE, FALSE, 0);
    dashboard->stats_local_ip_label = gtk_label_new("Local IP: --");
    gtk_label_set_xalign(GTK_LABEL(dashboard->stats_local_ip_label), 0.0);
    gtk_box_pack_start(GTK_BOX(localip_box), dashboard->stats_local_ip_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(dashboard->stats_content_box), localip_box, FALSE, FALSE, 0);

    /* Gateway - with icon */
    GtkWidget *gateway_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *gateway_icon = gtk_image_new_from_icon_name("network-transmit-symbolic", GTK_ICON_SIZE_MENU);
    gtk_box_pack_start(GTK_BOX(gateway_box), gateway_icon, FALSE, FALSE, 0);
    dashboard->stats_gateway_label = gtk_label_new("Gateway:  --");
    gtk_label_set_xalign(GTK_LABEL(dashboard->stats_gateway_label), 0.0);
    gtk_box_pack_start(GTK_BOX(gateway_box), dashboard->stats_gateway_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(dashboard->stats_content_box), gateway_box, FALSE, FALSE, 0);

    /* Separator */
    gtk_box_pack_start(GTK_BOX(dashboard->stats_content_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 8);

    /* Buttons row */
    GtkWidget *stats_buttons_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(stats_buttons_box, 8);

    GtkWidget *export_btn = gtk_button_new_with_label("Export Statistics");
    g_signal_connect(export_btn, "clicked", G_CALLBACK(on_export_stats_clicked), dashboard);
    gtk_box_pack_start(GTK_BOX(stats_buttons_box), export_btn, FALSE, FALSE, 0);

    GtkWidget *reset_btn = gtk_button_new_with_label("Reset Counters");
    g_signal_connect(reset_btn, "clicked", G_CALLBACK(on_reset_stats_clicked), dashboard);
    gtk_box_pack_start(GTK_BOX(stats_buttons_box), reset_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(dashboard->stats_content_box), stats_buttons_box, FALSE, FALSE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(dashboard->notebook), dashboard->statistics_tab,
                            gtk_label_new("Statistics"));

    /* Tab 3: Servers */
    dashboard->servers_tab_instance = servers_tab_create(NULL);  /* bus will be set later */
    dashboard->servers_tab = servers_tab_get_widget(dashboard->servers_tab_instance);
    gtk_notebook_append_page(GTK_NOTEBOOK(dashboard->notebook), dashboard->servers_tab,
                            gtk_label_new("Servers"));

    /* Tab 4: Routing (placeholder) */
    dashboard->routing_tab = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(dashboard->routing_tab), 20);
    GtkWidget *routing_label = gtk_label_new("Routing/Split Tunneling tab - Coming in Phase 4");
    gtk_box_pack_start(GTK_BOX(dashboard->routing_tab), routing_label, TRUE, TRUE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(dashboard->notebook), dashboard->routing_tab,
                            gtk_label_new("Routing"));

    /* Tab 5: Security (placeholder) */
    dashboard->security_tab = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(dashboard->security_tab), 20);
    GtkWidget *security_label = gtk_label_new("Security/Kill Switch tab - Coming in Phase 5");
    gtk_box_pack_start(GTK_BOX(dashboard->security_tab), security_label, TRUE, TRUE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(dashboard->notebook), dashboard->security_tab,
                            gtk_label_new("Security"));

    /* Add notebook to window */
    gtk_container_add(GTK_CONTAINER(dashboard->window), dashboard->notebook);

    printf("Dashboard window created\n");

    return dashboard;
}

/**
 * Show dashboard
 */
void dashboard_show(Dashboard *dashboard) {
    if (!dashboard || !dashboard->window) {
        return;
    }

    /* Ensure window is realized before showing */
    if (!gtk_widget_get_realized(dashboard->window)) {
        gtk_widget_realize(dashboard->window);

        /* Update dashboard with current data on first show */
        if (dashboard->bus) {
            dashboard_update(dashboard, dashboard->bus);
        }
    }

    gtk_widget_show_all(dashboard->window);
    gtk_window_present(GTK_WINDOW(dashboard->window));
}

/**
 * Hide dashboard
 */
void dashboard_hide(Dashboard *dashboard) {
    if (!dashboard || !dashboard->window) {
        return;
    }

    gtk_widget_hide(dashboard->window);
}

/**
 * Toggle dashboard visibility
 */
void dashboard_toggle(Dashboard *dashboard) {
    if (!dashboard || !dashboard->window) {
        return;
    }

    if (gtk_widget_get_visible(dashboard->window)) {
        dashboard_hide(dashboard);
    } else {
        dashboard_show(dashboard);
    }
}

/**
 * Update dashboard with current data
 */
void dashboard_update(Dashboard *dashboard, sd_bus *bus) {
    if (!dashboard || !bus) {
        return;
    }

    dashboard->bus = bus;

    /* Clear existing content */
    gtk_container_foreach(GTK_CONTAINER(dashboard->sessions_container),
                         (GtkCallback)gtk_widget_destroy, NULL);
    gtk_container_foreach(GTK_CONTAINER(dashboard->configs_container),
                         (GtkCallback)gtk_widget_destroy, NULL);

    /* Get active sessions */
    VpnSession **sessions = NULL;
    unsigned int session_count = 0;
    int r = session_list(bus, &sessions, &session_count);

    if (r >= 0 && session_count > 0) {
        for (unsigned int i = 0; i < session_count; i++) {
            create_session_card(dashboard, sessions[i]);
        }
    } else {
        /* No active sessions */
        GtkWidget *no_sessions = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(no_sessions),
                           "<span foreground='#888888'>No active VPN connections</span>");
        gtk_box_pack_start(GTK_BOX(dashboard->sessions_container), no_sessions, FALSE, FALSE, 0);
    }

    /* Get configurations */
    VpnConfig **configs = NULL;
    unsigned int config_count = 0;
    r = config_list(bus, &configs, &config_count);

    if (r >= 0 && config_count > 0) {
        for (unsigned int i = 0; i < config_count; i++) {
            /* Check if config is in use */
            gboolean in_use = FALSE;
            if (sessions && session_count > 0) {
                for (unsigned int j = 0; j < session_count; j++) {
                    if (sessions[j]->config_name && configs[i]->config_name &&
                        strcmp(sessions[j]->config_name, configs[i]->config_name) == 0) {
                        in_use = TRUE;
                        break;
                    }
                }
            }
            /* Skip configs that are already in use (shown in Active Connections) */
            if (in_use) {
                continue;
            }
            create_config_card(dashboard, configs[i]);
        }
        config_list_free(configs, config_count);
    } else {
        /* No configurations */
        GtkWidget *no_configs = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(no_configs),
                           "<span foreground='#888888'>No configurations available</span>");
        gtk_box_pack_start(GTK_BOX(dashboard->configs_container), no_configs, FALSE, FALSE, 0);
    }

    /* Add Import Config row at the end of the list */
    create_import_config_row(dashboard);

    /* Update bandwidth statistics */
    if (session_count > 0 && sessions) {
        /* Show statistics content, hide empty state */
        gtk_widget_show(dashboard->stats_content_box);
        gtk_widget_hide(dashboard->stats_empty_state);

        /* Clear and repopulate session combo box */
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(dashboard->stats_session_combo));

        /* Store current selection to restore it if possible */
        gint previous_index = gtk_combo_box_get_active(GTK_COMBO_BOX(dashboard->stats_session_combo));

        /* Add all active sessions to combo box */
        for (unsigned int i = 0; i < session_count; i++) {
            VpnSession *session = sessions[i];
            char item_text[256];
            snprintf(item_text, sizeof(item_text), "%s",
                    session->config_name ? session->config_name : "Unknown");
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(dashboard->stats_session_combo), item_text);

            /* Store session path and device name with this index */
            char key_path[32];
            char key_device[32];
            snprintf(key_path, sizeof(key_path), "session-path-%u", i);
            snprintf(key_device, sizeof(key_device), "device-name-%u", i);

            g_object_set_data_full(G_OBJECT(dashboard->stats_session_combo), key_path,
                                  g_strdup(session->session_path), g_free);
            g_object_set_data_full(G_OBJECT(dashboard->stats_session_combo), key_device,
                                  g_strdup(session->device_name ? session->device_name : ""), g_free);
        }

        /* Restore previous selection or default to first session */
        if (previous_index >= 0 && (unsigned int)previous_index < session_count) {
            gtk_combo_box_set_active(GTK_COMBO_BOX(dashboard->stats_session_combo), previous_index);
        } else {
            gtk_combo_box_set_active(GTK_COMBO_BOX(dashboard->stats_session_combo), 0);
        }

        /* Get the currently selected session */
        gint selected_index = gtk_combo_box_get_active(GTK_COMBO_BOX(dashboard->stats_session_combo));
        if (selected_index < 0 || (unsigned int)selected_index >= session_count) {
            selected_index = 0;
        }
        VpnSession *active_session = sessions[selected_index];

        /* Update duration */
        time_t now = time(NULL);
        time_t elapsed = now - (time_t)active_session->session_created;
        char elapsed_str[32];
        format_elapsed_time(elapsed, elapsed_str, sizeof(elapsed_str));
        char duration_text[64];
        snprintf(duration_text, sizeof(duration_text), "Duration: %s", elapsed_str);
        gtk_label_set_text(GTK_LABEL(dashboard->stats_duration_label), duration_text);

        /* Create or recreate bandwidth monitor if device changed */
        if (!dashboard->bandwidth_monitor ||
            !dashboard->current_device ||
            strcmp(dashboard->current_device, active_session->device_name) != 0) {

            /* Free old monitor */
            if (dashboard->bandwidth_monitor) {
                bandwidth_monitor_free(dashboard->bandwidth_monitor);
            }
            free(dashboard->current_device);

            /* Create new monitor */
            printf("Dashboard: Creating bandwidth monitor for device '%s'\n", active_session->device_name);
            dashboard->bandwidth_monitor = bandwidth_monitor_create(
                active_session->session_path,
                active_session->device_name,
                STATS_SOURCE_AUTO,  /* Try D-Bus first, fallback to sysfs */
                60  /* 60-second buffer */
            );
            dashboard->current_device = strdup(active_session->device_name);
            if (dashboard->bandwidth_monitor) {
                printf("Dashboard: Bandwidth monitor created successfully\n");
            } else {
                printf("Dashboard: Failed to create bandwidth monitor\n");
            }
        }

        /* Update bandwidth monitor */
        if (dashboard->bandwidth_monitor) {
            int r = bandwidth_monitor_update(dashboard->bandwidth_monitor, bus);
            if (r < 0) {
                printf("Dashboard: Failed to update bandwidth monitor: %d\n", r);
            }

            /* Get current rate */
            BandwidthRate rate;
            BandwidthSample sample;
            char text[256];

            int rate_result = bandwidth_monitor_get_rate(dashboard->bandwidth_monitor, &rate);
            if (rate_result >= 0) {
                printf("Dashboard: Rates - Upload: %.0f B/s, Download: %.0f B/s, Total Up: %lu, Total Down: %lu\n",
                       rate.upload_rate_bps, rate.download_rate_bps,
                       rate.total_uploaded, rate.total_downloaded);

                /* Update upload rate */
                format_bytes((uint64_t)rate.upload_rate_bps, text, sizeof(text));
                char label_text[256];
                snprintf(label_text, sizeof(label_text), "  ▲ Upload:   %s/s", text);
                gtk_label_set_text(GTK_LABEL(dashboard->stats_upload_label), label_text);

                /* Update download rate */
                format_bytes((uint64_t)rate.download_rate_bps, text, sizeof(text));
                snprintf(label_text, sizeof(label_text), "  ▼ Download: %s/s", text);
                gtk_label_set_text(GTK_LABEL(dashboard->stats_download_label), label_text);

                /* Update total uploaded */
                format_bytes(rate.total_uploaded, text, sizeof(text));
                snprintf(label_text, sizeof(label_text), "<b>%s</b>", text);
                gtk_label_set_markup(GTK_LABEL(dashboard->stats_total_uploaded_label), label_text);

                /* Update total downloaded */
                format_bytes(rate.total_downloaded, text, sizeof(text));
                snprintf(label_text, sizeof(label_text), "<b>%s</b>", text);
                gtk_label_set_markup(GTK_LABEL(dashboard->stats_total_downloaded_label), label_text);

                /* Update total combined */
                format_bytes(rate.total_uploaded + rate.total_downloaded, text, sizeof(text));
                snprintf(label_text, sizeof(label_text), "<b>%s</b>", text);
                gtk_label_set_markup(GTK_LABEL(dashboard->stats_total_combined_label), label_text);
            } else {
                printf("Dashboard: Not enough samples yet to calculate rate (result: %d)\n", rate_result);
            }

            if (bandwidth_monitor_get_latest_sample(dashboard->bandwidth_monitor, &sample) >= 0) {
                /* Update packet statistics */
                char label_text[256];
                snprintf(label_text, sizeof(label_text), "<b>%lu packets</b>", sample.packets_out);
                gtk_label_set_markup(GTK_LABEL(dashboard->stats_packets_sent_label), label_text);

                snprintf(label_text, sizeof(label_text), "<b>%lu packets</b>", sample.packets_in);
                gtk_label_set_markup(GTK_LABEL(dashboard->stats_packets_received_label), label_text);

                snprintf(label_text, sizeof(label_text), "<b>%lu</b>", sample.errors_in + sample.errors_out);
                gtk_label_set_markup(GTK_LABEL(dashboard->stats_packets_errors_label), label_text);

                snprintf(label_text, sizeof(label_text), "<b>%lu</b>", sample.dropped_in + sample.dropped_out);
                gtk_label_set_markup(GTK_LABEL(dashboard->stats_packets_dropped_label), label_text);
            }

            /* Redraw bandwidth graph */
            if (dashboard->stats_graph_area) {
                gtk_widget_queue_draw(dashboard->stats_graph_area);
            }
        }

        /* Update connection details */
        char detail_text[256];

        /* Protocol - extract from session if available */
        snprintf(detail_text, sizeof(detail_text), "Protocol: %s",
                active_session->device_name ? "UDP" : "--");  /* TODO: Get actual protocol from D-Bus */
        gtk_label_set_text(GTK_LABEL(dashboard->stats_protocol_label), detail_text);

        /* Cipher */
        snprintf(detail_text, sizeof(detail_text), "Cipher:   AES-256-GCM");  /* TODO: Get actual cipher from D-Bus */
        gtk_label_set_text(GTK_LABEL(dashboard->stats_cipher_label), detail_text);

        /* Server */
        snprintf(detail_text, sizeof(detail_text), "Server:   %s",
                active_session->config_name ? active_session->config_name : "--");
        gtk_label_set_text(GTK_LABEL(dashboard->stats_server_label), detail_text);

        /* Local IP */
        char local_ip[64];
        if (active_session->device_name &&
            get_interface_ip(active_session->device_name, local_ip, sizeof(local_ip)) == 0) {
            snprintf(detail_text, sizeof(detail_text), "Local IP: %s", local_ip);
        } else {
            snprintf(detail_text, sizeof(detail_text), "Local IP: --");
        }
        gtk_label_set_text(GTK_LABEL(dashboard->stats_local_ip_label), detail_text);

        /* Gateway */
        char gateway[64];
        if (active_session->device_name &&
            get_interface_gateway(active_session->device_name, gateway, sizeof(gateway)) == 0) {
            snprintf(detail_text, sizeof(detail_text), "Gateway:  %s", gateway);
        } else {
            snprintf(detail_text, sizeof(detail_text), "Gateway:  --");
        }
        gtk_label_set_text(GTK_LABEL(dashboard->stats_gateway_label), detail_text);

    } else {
        /* No active sessions - show empty state */
        gtk_widget_hide(dashboard->stats_content_box);
        gtk_widget_show(dashboard->stats_empty_state);

        /* Reset session combo box */
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(dashboard->stats_session_combo));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(dashboard->stats_session_combo), "No active sessions");
        gtk_combo_box_set_active(GTK_COMBO_BOX(dashboard->stats_session_combo), 0);

        /* Reset duration label */
        gtk_label_set_text(GTK_LABEL(dashboard->stats_duration_label), "Duration: --");

        if (dashboard->bandwidth_monitor) {
            bandwidth_monitor_free(dashboard->bandwidth_monitor);
            dashboard->bandwidth_monitor = NULL;
        }
        free(dashboard->current_device);
        dashboard->current_device = NULL;

        gtk_label_set_text(GTK_LABEL(dashboard->stats_upload_label), "  ▲ Upload:   0 B/s");
        gtk_label_set_text(GTK_LABEL(dashboard->stats_download_label), "  ▼ Download: 0 B/s");
        gtk_label_set_text(GTK_LABEL(dashboard->stats_total_uploaded_label), "0 B");
        gtk_label_set_text(GTK_LABEL(dashboard->stats_total_downloaded_label), "0 B");
        gtk_label_set_text(GTK_LABEL(dashboard->stats_total_combined_label), "0 B");
        gtk_label_set_text(GTK_LABEL(dashboard->stats_packets_sent_label), "0 packets");
        gtk_label_set_text(GTK_LABEL(dashboard->stats_packets_received_label), "0 packets");
        gtk_label_set_text(GTK_LABEL(dashboard->stats_packets_errors_label), "0");
        gtk_label_set_text(GTK_LABEL(dashboard->stats_packets_dropped_label), "0");
        gtk_label_set_text(GTK_LABEL(dashboard->stats_protocol_label), "Protocol: --");
        gtk_label_set_text(GTK_LABEL(dashboard->stats_cipher_label), "Cipher:   --");
        gtk_label_set_text(GTK_LABEL(dashboard->stats_server_label), "Server:   --");
        gtk_label_set_text(GTK_LABEL(dashboard->stats_local_ip_label), "Local IP: --");
        gtk_label_set_text(GTK_LABEL(dashboard->stats_gateway_label), "Gateway:  --");
    }

    /* Free sessions after we're done using them */
    if (sessions) {
        session_list_free(sessions, session_count);
    }

    /* Update servers tab */
    if (dashboard->servers_tab_instance) {
        servers_tab_refresh(dashboard->servers_tab_instance, bus);
    }

    gtk_widget_show_all(dashboard->sessions_container);
    gtk_widget_show_all(dashboard->configs_container);
}

/**
 * Destroy dashboard
 */
void dashboard_destroy(Dashboard *dashboard) {
    if (!dashboard) {
        return;
    }

    /* Clean up bandwidth monitor */
    if (dashboard->bandwidth_monitor) {
        bandwidth_monitor_free(dashboard->bandwidth_monitor);
    }
    free(dashboard->current_device);

    /* Clean up servers tab */
    if (dashboard->servers_tab_instance) {
        servers_tab_free(dashboard->servers_tab_instance);
    }

    if (dashboard->window) {
        gtk_widget_destroy(dashboard->window);
    }

    g_free(dashboard);
    printf("Dashboard destroyed\n");
}
