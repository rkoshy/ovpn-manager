#include "dashboard.h"
#include "widgets.h"
#include "icons.h"
#include "servers_tab.h"
#include "../dbus/session_client.h"
#include "../dbus/config_client.h"
#include "../monitoring/bandwidth_monitor.h"
#include "../utils/logger.h"
#include "../utils/file_chooser.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/**
 * Dashboard structure
 */
struct Dashboard {
    GtkWidget *window;
    GtkWidget *header_bar;
    GtkWidget *notebook;
    GtkWidget *main_box;
    GtkWidget *sessions_container;
    GtkWidget *configs_container;
    /* Tab containers */
    GtkWidget *connections_tab;
    GtkWidget *statistics_tab;
    GtkWidget *servers_tab;
    /* Statistics widgets - card-based view */
    GtkWidget *stats_flowbox;      /* FlowBox container for stat cards */
    GtkWidget *stats_empty_state;  /* Empty state widget (shown when disconnected) */
    /* Aggregate bandwidth graph */
    GtkWidget *aggregate_graph;
    GtkWidget *aggregate_graph_box;
    GtkWidget *aggregate_dl_label;
    GtkWidget *aggregate_ul_label;
    double aggregate_dl_history[120];
    double aggregate_ul_history[120];
    int aggregate_write_idx;
    int aggregate_sample_count;
    /* Status bar */
    GtkWidget *status_bar;
    GtkWidget *status_label;
    /* Bandwidth monitors - one per session (hash table: session_path -> BandwidthMonitor) */
    GHashTable *bandwidth_monitors;
    /* Servers tab instance */
    ServersTab *servers_tab_instance;
    sd_bus *bus;
};

/* Forward declarations */
static gboolean on_window_delete(GtkWidget *widget, GdkEvent *event, gpointer data);
static void on_disconnect_clicked(GtkButton *button, gpointer data);
static void on_connect_clicked(GtkButton *button, gpointer data);
static void on_import_clicked(GtkButton *button, gpointer data);
static void create_session_card(Dashboard *dashboard, VpnSession *session);
static void create_config_card(Dashboard *dashboard, VpnConfig *config);
static void create_import_config_row(Dashboard *dashboard);
static GtkWidget* create_vpn_stat_card(Dashboard *dashboard, VpnSession *session, BandwidthMonitor *monitor);

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
 * Draw sparkline graph for stat card (compact version without axes)
 */
static gboolean on_card_graph_draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
    BandwidthMonitor *monitor = (BandwidthMonitor *)data;
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    const int margin = 5;

    /* Background */
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.02);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);

    /* Horizontal grid lines at 25%, 50%, 75% */
    {
        GdkRGBA grid_clr;
        GtkStyleContext *sc = gtk_widget_get_style_context(widget);
        if (!gtk_style_context_lookup_color(sc, "text_tertiary", &grid_clr))
            grid_clr = (GdkRGBA){0.5, 0.5, 0.5, 1.0};
        grid_clr.alpha = 0.25;
        cairo_set_source_rgba(cr, grid_clr.red, grid_clr.green, grid_clr.blue, grid_clr.alpha);
        cairo_set_line_width(cr, 0.5);
        double dashes[] = {4.0, 4.0};
        cairo_set_dash(cr, dashes, 2, 0);
        for (int i = 1; i <= 3; i++) {
            double gy = margin + (height - 2 * margin) * (1.0 - i * 0.25);
            cairo_move_to(cr, margin, gy);
            cairo_line_to(cr, width - margin, gy);
            cairo_stroke(cr);
        }
        cairo_set_dash(cr, NULL, 0, 0);
    }

    if (!monitor) {
        return TRUE;
    }

    /* Get bandwidth samples (last 60 seconds for sparkline) */
    BandwidthSample samples[60];
    unsigned int sample_count = 0;
    if (bandwidth_monitor_get_samples(monitor, samples, 60, &sample_count) < 0 || sample_count == 0) {
        /* Draw a flat line at zero when no data */
        cairo_set_source_rgba(cr, 0.2, 0.8, 0.4, 0.3);
        cairo_move_to(cr, margin, margin + (height - 2 * margin) / 2);
        cairo_line_to(cr, width - margin, margin + (height - 2 * margin) / 2);
        cairo_stroke(cr);
        return TRUE;
    }

    /* Filter out samples with timestamp=0 */
    BandwidthSample valid_samples[60];
    unsigned int valid_count = 0;
    for (unsigned int i = 0; i < sample_count; i++) {
        if (samples[i].timestamp > 0) {
            valid_samples[valid_count++] = samples[i];
        }
    }

    if (valid_count == 0) {
        /* No valid samples */
        cairo_set_source_rgba(cr, 0.2, 0.8, 0.4, 0.3);
        cairo_move_to(cr, margin, margin + (height - 2 * margin) / 2);
        cairo_line_to(cr, width - margin, margin + (height - 2 * margin) / 2);
        cairo_stroke(cr);
        return TRUE;
    }

    /* Copy valid samples back */
    for (unsigned int i = 0; i < valid_count; i++) {
        samples[i] = valid_samples[i];
    }
    sample_count = valid_count;

    /* Find max rate for scaling */
    double max_rate = 0.0;
    for (unsigned int i = 0; i < sample_count - 1; i++) {
        time_t time_diff = samples[i].timestamp - samples[i+1].timestamp;
        if (time_diff <= 0) {
            continue;  /* Skip invalid samples with same/backwards timestamps */
        }

        double upload_rate = (double)(samples[i].bytes_out - samples[i+1].bytes_out) / (double)time_diff;
        double download_rate = (double)(samples[i].bytes_in - samples[i+1].bytes_in) / (double)time_diff;

        /* Absolute values for scaling (handle negative rates from counter resets) */
        if (upload_rate < 0) upload_rate = -upload_rate;
        if (download_rate < 0) download_rate = -download_rate;

        if (upload_rate > max_rate) max_rate = upload_rate;
        if (download_rate > max_rate) max_rate = download_rate;
    }

    /* Use adaptive minimum scale: if there's no traffic, use small scale; otherwise 1 KB/s */
    if (max_rate < 10.0) {
        max_rate = 10.0;  /* 10 B/s minimum for idle connections */
    } else if (max_rate < 1024.0) {
        max_rate = 1024.0;  /* 1 KB/s scale for active connections */
    }

    int graph_width = width - 2 * margin;
    int graph_height = height - 2 * margin;

    /* Special case: if only one sample, draw points instead of lines */
    if (sample_count == 1) {
        double x = margin + graph_width;  /* Right edge */
        double y = margin + graph_height;  /* Bottom (zero rate) */

        /* Draw download point (green) */
        cairo_set_source_rgba(cr, 0.2, 0.8, 0.4, 0.8);
        cairo_arc(cr, x, y, 4.0, 0, 2 * M_PI);
        cairo_fill(cr);

        /* Draw upload point (blue) */
        cairo_set_source_rgba(cr, 0.2, 0.5, 0.95, 0.8);
        cairo_arc(cr, x, y, 4.0, 0, 2 * M_PI);
        cairo_fill(cr);

        return TRUE;
    }

    /* Draw download line (green) with gradient fill */
    cairo_set_line_width(cr, 2.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    gboolean first_point = TRUE;
    double last_y = margin + graph_height;  /* Track last Y for extending to oldest sample */
    for (unsigned int i = 0; i < sample_count - 1; i++) {
        time_t time_diff = samples[i].timestamp - samples[i+1].timestamp;
        if (time_diff <= 0) {
            continue;  /* Skip invalid samples */
        }

        double rate = (samples[i].bytes_in - samples[i+1].bytes_in) / (double)time_diff;
        /* Use time-based positioning: newer samples on right, older scroll left */
        double time_offset = samples[0].timestamp - samples[i].timestamp;
        double x = margin + graph_width - (time_offset * graph_width / 60.0);
        double y = margin + graph_height - (rate / max_rate * graph_height);
        last_y = y;

        if (first_point) {
            cairo_move_to(cr, x, y);
            first_point = FALSE;
        } else {
            cairo_line_to(cr, x, y);
        }
    }

    /* Extend line to the oldest sample's position to complete the graph */
    double fill_left_x = margin;  /* Default: left margin */
    if (!first_point && sample_count >= 2) {
        double oldest_time_offset = samples[0].timestamp - samples[sample_count - 1].timestamp;
        double oldest_x = margin + graph_width - (oldest_time_offset * graph_width / 60.0);
        /* Clamp to visible area */
        if (oldest_x < margin) oldest_x = margin;
        cairo_line_to(cr, oldest_x, last_y);
        fill_left_x = oldest_x;
    }

    /* Complete the path to create a filled area - go down then right along bottom */
    cairo_line_to(cr, fill_left_x, margin + graph_height);  /* Down to bottom at current X */
    cairo_line_to(cr, margin + graph_width, margin + graph_height);  /* Right along bottom */
    cairo_close_path(cr);

    /* Create gradient fill from opaque to transparent */
    cairo_pattern_t *download_gradient = cairo_pattern_create_linear(0, margin, 0, margin + graph_height);
    cairo_pattern_add_color_stop_rgba(download_gradient, 0.0, 0.2, 0.8, 0.4, 0.3);
    cairo_pattern_add_color_stop_rgba(download_gradient, 1.0, 0.2, 0.8, 0.4, 0.0);
    cairo_set_source(cr, download_gradient);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(download_gradient);

    /* Stroke the line */
    cairo_set_source_rgba(cr, 0.2, 0.8, 0.4, 0.8);
    cairo_stroke(cr);

    /* Draw upload line (blue) with gradient fill */
    cairo_set_line_width(cr, 2.0);

    first_point = TRUE;
    last_y = margin + graph_height;  /* Reset for upload graph */
    for (unsigned int i = 0; i < sample_count - 1; i++) {
        time_t time_diff = samples[i].timestamp - samples[i+1].timestamp;
        if (time_diff <= 0) {
            continue;  /* Skip invalid samples */
        }

        double rate = (samples[i].bytes_out - samples[i+1].bytes_out) / (double)time_diff;
        /* Use time-based positioning: newer samples on right, older scroll left */
        double time_offset = samples[0].timestamp - samples[i].timestamp;
        double x = margin + graph_width - (time_offset * graph_width / 60.0);
        double y = margin + graph_height - (rate / max_rate * graph_height);
        last_y = y;

        if (first_point) {
            cairo_move_to(cr, x, y);
            first_point = FALSE;
        } else {
            cairo_line_to(cr, x, y);
        }
    }

    /* Extend line to the oldest sample's position to complete the graph */
    fill_left_x = margin;  /* Reset for upload graph */
    if (!first_point && sample_count >= 2) {
        double oldest_time_offset = samples[0].timestamp - samples[sample_count - 1].timestamp;
        double oldest_x = margin + graph_width - (oldest_time_offset * graph_width / 60.0);
        /* Clamp to visible area */
        if (oldest_x < margin) oldest_x = margin;
        cairo_line_to(cr, oldest_x, last_y);
        fill_left_x = oldest_x;
    }

    /* Complete the path to create a filled area - go down then right along bottom */
    cairo_line_to(cr, fill_left_x, margin + graph_height);  /* Down to bottom at current X */
    cairo_line_to(cr, margin + graph_width, margin + graph_height);  /* Right along bottom */
    cairo_close_path(cr);

    /* Create gradient fill from opaque to transparent */
    cairo_pattern_t *upload_gradient = cairo_pattern_create_linear(0, margin, 0, margin + graph_height);
    cairo_pattern_add_color_stop_rgba(upload_gradient, 0.0, 0.2, 0.5, 0.95, 0.3);
    cairo_pattern_add_color_stop_rgba(upload_gradient, 1.0, 0.2, 0.5, 0.95, 0.0);
    cairo_set_source(cr, upload_gradient);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(upload_gradient);

    /* Stroke the line */
    cairo_set_source_rgba(cr, 0.2, 0.5, 0.95, 0.8);
    cairo_stroke(cr);

    /* Overlay current rate text in graph corners */
    {
        BandwidthRate overlay_rate;
        if (bandwidth_monitor_get_rate(monitor, &overlay_rate) >= 0) {
            char dl_t[32], ul_t[32], dl_f[48], ul_f[48];
            format_bytes((uint64_t)overlay_rate.download_rate_bps, dl_t, sizeof(dl_t));
            format_bytes((uint64_t)overlay_rate.upload_rate_bps, ul_t, sizeof(ul_t));
            snprintf(dl_f, sizeof(dl_f), "↓ %s/s", dl_t);
            snprintf(ul_f, sizeof(ul_f), "↑ %s/s", ul_t);

            GtkStyleContext *sc = gtk_widget_get_style_context(widget);
            PangoLayout *layout = pango_cairo_create_layout(cr);
            PangoFontDescription *font = pango_font_description_from_string("Monospace Bold 8");
            pango_layout_set_font_description(layout, font);

            /* Download - top left (green) */
            GdkRGBA dl_clr;
            if (!gtk_style_context_lookup_color(sc, "success_green", &dl_clr))
                dl_clr = (GdkRGBA){0.2, 0.8, 0.4, 1.0};
            dl_clr.alpha = 0.9;
            cairo_set_source_rgba(cr, dl_clr.red, dl_clr.green, dl_clr.blue, dl_clr.alpha);
            pango_layout_set_text(layout, dl_f, -1);
            cairo_move_to(cr, margin + 4, margin + 2);
            pango_cairo_show_layout(cr, layout);

            /* Upload - top right (blue) */
            GdkRGBA ul_clr;
            if (!gtk_style_context_lookup_color(sc, "primary_blue", &ul_clr))
                ul_clr = (GdkRGBA){0.2, 0.5, 0.95, 1.0};
            ul_clr.alpha = 0.9;
            cairo_set_source_rgba(cr, ul_clr.red, ul_clr.green, ul_clr.blue, ul_clr.alpha);
            pango_layout_set_text(layout, ul_f, -1);
            PangoRectangle ink, logical;
            pango_layout_get_pixel_extents(layout, &ink, &logical);
            cairo_move_to(cr, width - margin - 4 - logical.width, margin + 2);
            pango_cairo_show_layout(cr, layout);

            pango_font_description_free(font);
            g_object_unref(layout);
        }
    }

    return TRUE;
}

/**
 * Draw aggregate bandwidth graph for all sessions combined
 */
static gboolean on_aggregate_graph_draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
    Dashboard *dashboard = (Dashboard *)data;
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);
    const int margin = 8;

    /* Background */
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.02);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);

    /* Grid lines */
    {
        GdkRGBA grid_clr;
        GtkStyleContext *sc = gtk_widget_get_style_context(widget);
        if (!gtk_style_context_lookup_color(sc, "text_tertiary", &grid_clr))
            grid_clr = (GdkRGBA){0.5, 0.5, 0.5, 1.0};
        grid_clr.alpha = 0.2;
        cairo_set_source_rgba(cr, grid_clr.red, grid_clr.green, grid_clr.blue, grid_clr.alpha);
        cairo_set_line_width(cr, 0.5);
        double dashes[] = {4.0, 4.0};
        cairo_set_dash(cr, dashes, 2, 0);
        for (int i = 1; i <= 3; i++) {
            double gy = margin + (height - 2 * margin) * (1.0 - i * 0.25);
            cairo_move_to(cr, margin, gy);
            cairo_line_to(cr, width - margin, gy);
            cairo_stroke(cr);
        }
        cairo_set_dash(cr, NULL, 0, 0);
    }

    int count = dashboard->aggregate_sample_count;
    if (count < 2) return TRUE;

    /* Find max rate for scaling */
    double max_rate = 1024.0;
    for (int i = 0; i < count; i++) {
        int idx = (dashboard->aggregate_write_idx - 1 - i + 120) % 120;
        if (dashboard->aggregate_dl_history[idx] > max_rate)
            max_rate = dashboard->aggregate_dl_history[idx];
        if (dashboard->aggregate_ul_history[idx] > max_rate)
            max_rate = dashboard->aggregate_ul_history[idx];
    }

    int gw = width - 2 * margin;
    int gh = height - 2 * margin;

    /* Draw download line (green) with gradient fill */
    cairo_set_line_width(cr, 2.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    gboolean first_point = TRUE;
    for (int i = 0; i < count; i++) {
        int idx = (dashboard->aggregate_write_idx - 1 - i + 120) % 120;
        double rate = dashboard->aggregate_dl_history[idx];
        double x = margin + gw - (i * gw / 120.0);
        double y = margin + gh - (rate / max_rate * gh);
        if (x < margin) break;

        if (first_point) {
            cairo_move_to(cr, x, y);
            first_point = FALSE;
        } else {
            cairo_line_to(cr, x, y);
        }
    }

    if (!first_point) {
        /* Save path for stroke, then fill */
        cairo_path_t *dl_path = cairo_copy_path(cr);

        /* Complete fill area */
        double cur_x, cur_y;
        cairo_get_current_point(cr, &cur_x, &cur_y);
        cairo_line_to(cr, cur_x, margin + gh);
        cairo_line_to(cr, margin + gw, margin + gh);
        cairo_close_path(cr);

        cairo_pattern_t *dl_grad = cairo_pattern_create_linear(0, margin, 0, margin + gh);
        cairo_pattern_add_color_stop_rgba(dl_grad, 0.0, 0.2, 0.8, 0.4, 0.3);
        cairo_pattern_add_color_stop_rgba(dl_grad, 1.0, 0.2, 0.8, 0.4, 0.0);
        cairo_set_source(cr, dl_grad);
        cairo_fill(cr);
        cairo_pattern_destroy(dl_grad);

        /* Stroke the line */
        cairo_new_path(cr);
        cairo_append_path(cr, dl_path);
        cairo_set_source_rgba(cr, 0.2, 0.8, 0.4, 0.8);
        cairo_stroke(cr);
        cairo_path_destroy(dl_path);
    }

    /* Draw upload line (blue) with gradient fill */
    cairo_set_line_width(cr, 2.0);
    first_point = TRUE;
    for (int i = 0; i < count; i++) {
        int idx = (dashboard->aggregate_write_idx - 1 - i + 120) % 120;
        double rate = dashboard->aggregate_ul_history[idx];
        double x = margin + gw - (i * gw / 120.0);
        double y = margin + gh - (rate / max_rate * gh);
        if (x < margin) break;

        if (first_point) {
            cairo_move_to(cr, x, y);
            first_point = FALSE;
        } else {
            cairo_line_to(cr, x, y);
        }
    }

    if (!first_point) {
        cairo_path_t *ul_path = cairo_copy_path(cr);

        double cur_x, cur_y;
        cairo_get_current_point(cr, &cur_x, &cur_y);
        cairo_line_to(cr, cur_x, margin + gh);
        cairo_line_to(cr, margin + gw, margin + gh);
        cairo_close_path(cr);

        cairo_pattern_t *ul_grad = cairo_pattern_create_linear(0, margin, 0, margin + gh);
        cairo_pattern_add_color_stop_rgba(ul_grad, 0.0, 0.2, 0.5, 0.95, 0.3);
        cairo_pattern_add_color_stop_rgba(ul_grad, 1.0, 0.2, 0.5, 0.95, 0.0);
        cairo_set_source(cr, ul_grad);
        cairo_fill(cr);
        cairo_pattern_destroy(ul_grad);

        cairo_new_path(cr);
        cairo_append_path(cr, ul_path);
        cairo_set_source_rgba(cr, 0.2, 0.5, 0.95, 0.8);
        cairo_stroke(cr);
        cairo_path_destroy(ul_path);
    }

    return TRUE;
}

/**
 * Toggle More Info revealer
 */
static void on_info_toggled(GtkButton *btn, gpointer data) {
    GtkRevealer *revealer = GTK_REVEALER(data);
    gboolean is_revealed = gtk_revealer_get_reveal_child(revealer);
    gtk_revealer_set_reveal_child(revealer, !is_revealed);

    /* Update button label */
    gtk_button_set_label(btn, is_revealed ? "More Info ▼" : "More Info ▲");
}

/**
 * Create a VPN statistics card
 */
static GtkWidget* create_vpn_stat_card(Dashboard *dashboard, VpnSession *session, BandwidthMonitor *monitor) {
    (void)dashboard;  /* May be used for callbacks later */

    /* Main card container */
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(card, 400, -1);
    GtkStyleContext *card_context = gtk_widget_get_style_context(card);
    gtk_style_context_add_class(card_context, "vpn-stat-card");

    /* Header row: [●] DEV-SERVER (UDP)                      [More Info] */
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(header), 0);

    /* Status indicator */
    const char *status_icon = "●";
    GtkWidget *status_label = gtk_label_new(status_icon);
    if (session->state == SESSION_STATE_CONNECTED) {
        gtk_label_set_markup(GTK_LABEL(status_label), "<span foreground='#34C759'>●</span>");
    } else if (session->state == SESSION_STATE_CONNECTING) {
        gtk_label_set_markup(GTK_LABEL(status_label), "<span foreground='#FF9500'>●</span>");
    } else {
        gtk_label_set_markup(GTK_LABEL(status_label), "<span foreground='#FF3B30'>●</span>");
    }
    gtk_box_pack_start(GTK_BOX(header), status_label, FALSE, FALSE, 0);

    /* Session name and protocol */
    char header_text[256];
    snprintf(header_text, sizeof(header_text), "<span weight='bold' size='14000'>%s</span> <span size='11000' foreground='#888888'>(UDP)</span>",
             session->config_name ? session->config_name : "Unknown");
    GtkWidget *name_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(name_label), header_text);
    gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
    gtk_box_pack_start(GTK_BOX(header), name_label, TRUE, TRUE, 0);

    /* Quality badge based on error ratio */
    {
        BandwidthSample q_sample;
        if (bandwidth_monitor_get_latest_sample(monitor, &q_sample) >= 0) {
            uint64_t total_pkts = q_sample.packets_in + q_sample.packets_out;
            uint64_t total_errs = q_sample.errors_in + q_sample.errors_out +
                                  q_sample.dropped_in + q_sample.dropped_out;
            const char *q_text = NULL, *q_class = NULL;
            if (total_pkts > 100) {
                double ratio = (double)total_errs / (double)total_pkts;
                if (ratio < 0.001)      { q_text = "Excellent"; q_class = "quality-excellent"; }
                else if (ratio < 0.01)  { q_text = "Good";      q_class = "quality-good"; }
                else if (ratio < 0.05)  { q_text = "Fair";      q_class = "quality-fair"; }
                else                    { q_text = "Poor";      q_class = "quality-poor"; }
            }
            if (q_text) {
                GtkWidget *badge = gtk_label_new(q_text);
                GtkStyleContext *qc = gtk_widget_get_style_context(badge);
                gtk_style_context_add_class(qc, "quality-badge");
                gtk_style_context_add_class(qc, q_class);
                gtk_box_pack_end(GTK_BOX(header), badge, FALSE, FALSE, 0);
            }
        }
    }

    gtk_box_pack_start(GTK_BOX(card), header, FALSE, FALSE, 0);

    /* Separator */
    gtk_box_pack_start(GTK_BOX(card), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 5);

    /* Real-time throughput row */
    GtkWidget *throughput_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
    gtk_widget_set_halign(throughput_box, GTK_ALIGN_CENTER);
    gtk_container_set_border_width(GTK_CONTAINER(throughput_box), 5);

    /* Download speed */
    GtkWidget *download_label = gtk_label_new("↓ 0 B/s");
    gtk_style_context_add_class(gtk_widget_get_style_context(download_label), "card-bandwidth-download");
    gtk_box_pack_start(GTK_BOX(throughput_box), download_label, FALSE, FALSE, 0);

    /* Upload speed */
    GtkWidget *upload_label = gtk_label_new("↑ 0 B/s");
    gtk_style_context_add_class(gtk_widget_get_style_context(upload_label), "card-bandwidth-upload");
    gtk_box_pack_start(GTK_BOX(throughput_box), upload_label, FALSE, FALSE, 0);

    /* Store labels as widget data for updates */
    g_object_set_data(G_OBJECT(card), "download-label", download_label);
    g_object_set_data(G_OBJECT(card), "upload-label", upload_label);

    gtk_box_pack_start(GTK_BOX(card), throughput_box, FALSE, FALSE, 0);

    /* Sparkline graph */
    GtkWidget *graph = gtk_drawing_area_new();
    gtk_widget_set_size_request(graph, -1, 140);
    gtk_style_context_add_class(gtk_widget_get_style_context(graph), "card-graph-area");
    g_signal_connect(graph, "draw", G_CALLBACK(on_card_graph_draw), monitor);
    g_object_set_data(G_OBJECT(card), "graph-area", graph);
    gtk_box_pack_start(GTK_BOX(card), graph, FALSE, FALSE, 5);

    /* Separator */
    gtk_box_pack_start(GTK_BOX(card), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 5);

    /* Detail grid: 2 columns */
    GtkWidget *detail_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(detail_grid), 40);
    gtk_grid_set_row_spacing(GTK_GRID(detail_grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(detail_grid), 10);

    /* Left column header */
    GtkWidget *packets_header = gtk_label_new("PACKETS");
    gtk_style_context_add_class(gtk_widget_get_style_context(packets_header), "card-section-header");
    gtk_label_set_xalign(GTK_LABEL(packets_header), 0.0);
    gtk_grid_attach(GTK_GRID(detail_grid), packets_header, 0, 0, 1, 1);

    /* Right column header */
    GtkWidget *connection_header = gtk_label_new("CONNECTION");
    gtk_style_context_add_class(gtk_widget_get_style_context(connection_header), "card-section-header");
    gtk_label_set_xalign(GTK_LABEL(connection_header), 0.0);
    gtk_grid_attach(GTK_GRID(detail_grid), connection_header, 1, 0, 1, 1);

    /* Packet stats - left column */
    GtkWidget *sent_label = gtk_label_new("Sent:     0");
    gtk_label_set_xalign(GTK_LABEL(sent_label), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(sent_label), "card-stats-label");
    gtk_grid_attach(GTK_GRID(detail_grid), sent_label, 0, 1, 1, 1);
    g_object_set_data(G_OBJECT(card), "sent-label", sent_label);

    GtkWidget *received_label = gtk_label_new("Received: 0");
    gtk_label_set_xalign(GTK_LABEL(received_label), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(received_label), "card-stats-label");
    gtk_grid_attach(GTK_GRID(detail_grid), received_label, 0, 2, 1, 1);
    g_object_set_data(G_OBJECT(card), "received-label", received_label);

    GtkWidget *errors_label = gtk_label_new("Errors:   0");
    gtk_label_set_xalign(GTK_LABEL(errors_label), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(errors_label), "card-stats-label");
    gtk_grid_attach(GTK_GRID(detail_grid), errors_label, 0, 3, 1, 1);
    g_object_set_data(G_OBJECT(card), "errors-label", errors_label);

    /* Connection details - right column */
    char local_ip[64];
    if (session->device_name && get_interface_ip(session->device_name, local_ip, sizeof(local_ip)) == 0) {
        char local_text[128];
        snprintf(local_text, sizeof(local_text), "Local:  %s", local_ip);
        GtkWidget *local_label = gtk_label_new(local_text);
        gtk_label_set_xalign(GTK_LABEL(local_label), 0.0);
        gtk_style_context_add_class(gtk_widget_get_style_context(local_label), "card-stats-label");
        gtk_grid_attach(GTK_GRID(detail_grid), local_label, 1, 1, 1, 1);
    }

    char gateway[64];
    if (session->device_name && get_interface_gateway(session->device_name, gateway, sizeof(gateway)) == 0) {
        char gw_text[128];
        snprintf(gw_text, sizeof(gw_text), "Gateway: %s", gateway);
        GtkWidget *gw_label = gtk_label_new(gw_text);
        gtk_label_set_xalign(GTK_LABEL(gw_label), 0.0);
        gtk_style_context_add_class(gtk_widget_get_style_context(gw_label), "card-stats-label");
        gtk_grid_attach(GTK_GRID(detail_grid), gw_label, 1, 2, 1, 1);
    }

    GtkWidget *cipher_label = gtk_label_new("Cipher:  AES-256-GCM");  /* TODO: Get from D-Bus */
    gtk_label_set_xalign(GTK_LABEL(cipher_label), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(cipher_label), "card-stats-label");
    gtk_grid_attach(GTK_GRID(detail_grid), cipher_label, 1, 3, 1, 1);

    gtk_box_pack_start(GTK_BOX(card), detail_grid, FALSE, FALSE, 0);

    /* More Info Revealer Section */
    GtkWidget *revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(revealer), 250);
    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), FALSE);

    /* More Info diagnostic grid */
    GtkWidget *more_info_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(more_info_grid), 30);
    gtk_grid_set_row_spacing(GTK_GRID(more_info_grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(more_info_grid), 15);
    gtk_style_context_add_class(gtk_widget_get_style_context(more_info_grid), "more-info-grid");

    int row = 0;

    /* Network Path Section */
    GtkWidget *net_header = gtk_label_new("NETWORK PATH");
    gtk_style_context_add_class(gtk_widget_get_style_context(net_header), "info-label");
    gtk_label_set_xalign(GTK_LABEL(net_header), 0.0);
    gtk_grid_attach(GTK_GRID(more_info_grid), net_header, 0, row++, 2, 1);

    GtkWidget *mtu_label = gtk_label_new("MTU Size:");
    gtk_label_set_xalign(GTK_LABEL(mtu_label), 0.0);
    gtk_grid_attach(GTK_GRID(more_info_grid), mtu_label, 0, row, 1, 1);
    GtkWidget *mtu_value = gtk_label_new("1500 bytes");  /* TODO: Get from system */
    gtk_label_set_xalign(GTK_LABEL(mtu_value), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(mtu_value), "info-value");
    gtk_grid_attach(GTK_GRID(more_info_grid), mtu_value, 1, row++, 1, 1);

    GtkWidget *endpoint_label = gtk_label_new("Remote Endpoint:");
    gtk_label_set_xalign(GTK_LABEL(endpoint_label), 0.0);
    gtk_grid_attach(GTK_GRID(more_info_grid), endpoint_label, 0, row, 1, 1);
    GtkWidget *endpoint_value = gtk_label_new("0.0.0.0:1194");  /* TODO: Get from session */
    gtk_label_set_xalign(GTK_LABEL(endpoint_value), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(endpoint_value), "info-value");
    gtk_grid_attach(GTK_GRID(more_info_grid), endpoint_value, 1, row++, 1, 1);

    GtkWidget *keepalive_label = gtk_label_new("Keepalive:");
    gtk_label_set_xalign(GTK_LABEL(keepalive_label), 0.0);
    gtk_grid_attach(GTK_GRID(more_info_grid), keepalive_label, 0, row, 1, 1);
    GtkWidget *keepalive_value = gtk_label_new("10s / 60s");  /* TODO: Get from config */
    gtk_label_set_xalign(GTK_LABEL(keepalive_value), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(keepalive_value), "info-value");
    gtk_grid_attach(GTK_GRID(more_info_grid), keepalive_value, 1, row++, 1, 1);

    row++;  /* Spacer */

    /* Security Section */
    GtkWidget *sec_header = gtk_label_new("SECURITY");
    gtk_style_context_add_class(gtk_widget_get_style_context(sec_header), "info-label");
    gtk_label_set_xalign(GTK_LABEL(sec_header), 0.0);
    gtk_grid_attach(GTK_GRID(more_info_grid), sec_header, 0, row++, 2, 1);

    GtkWidget *tls_label = gtk_label_new("TLS Version:");
    gtk_label_set_xalign(GTK_LABEL(tls_label), 0.0);
    gtk_grid_attach(GTK_GRID(more_info_grid), tls_label, 0, row, 1, 1);
    GtkWidget *tls_value = gtk_label_new("TLSv1.3");  /* TODO: Get from session */
    gtk_label_set_xalign(GTK_LABEL(tls_value), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(tls_value), "info-value");
    gtk_grid_attach(GTK_GRID(more_info_grid), tls_value, 1, row++, 1, 1);

    GtkWidget *ctrl_cipher_label = gtk_label_new("Control Cipher:");
    gtk_label_set_xalign(GTK_LABEL(ctrl_cipher_label), 0.0);
    gtk_grid_attach(GTK_GRID(more_info_grid), ctrl_cipher_label, 0, row, 1, 1);
    GtkWidget *ctrl_cipher_value = gtk_label_new("TLS-DHE-RSA-WITH-AES-256-GCM-SHA384");
    gtk_label_set_xalign(GTK_LABEL(ctrl_cipher_value), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(ctrl_cipher_value), "info-value");
    gtk_grid_attach(GTK_GRID(more_info_grid), ctrl_cipher_value, 1, row++, 1, 1);

    GtkWidget *cert_label = gtk_label_new("Certificate Expiry:");
    gtk_label_set_xalign(GTK_LABEL(cert_label), 0.0);
    gtk_grid_attach(GTK_GRID(more_info_grid), cert_label, 0, row, 1, 1);
    GtkWidget *cert_value = gtk_label_new("2025-12-31 (Valid)");  /* TODO: Get from session */
    gtk_label_set_xalign(GTK_LABEL(cert_value), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(cert_value), "info-value");
    gtk_grid_attach(GTK_GRID(more_info_grid), cert_value, 1, row++, 1, 1);

    row++;  /* Spacer */

    /* Quality Section */
    GtkWidget *qual_header = gtk_label_new("QUALITY");
    gtk_style_context_add_class(gtk_widget_get_style_context(qual_header), "info-label");
    gtk_label_set_xalign(GTK_LABEL(qual_header), 0.0);
    gtk_grid_attach(GTK_GRID(more_info_grid), qual_header, 0, row++, 2, 1);

    GtkWidget *latency_label = gtk_label_new("Current Latency:");
    gtk_label_set_xalign(GTK_LABEL(latency_label), 0.0);
    gtk_grid_attach(GTK_GRID(more_info_grid), latency_label, 0, row, 1, 1);
    GtkWidget *latency_value = gtk_label_new("12 ms");  /* TODO: Calculate */
    gtk_label_set_xalign(GTK_LABEL(latency_value), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(latency_value), "info-value");
    gtk_grid_attach(GTK_GRID(more_info_grid), latency_value, 1, row++, 1, 1);

    GtkWidget *jitter_label = gtk_label_new("Jitter:");
    gtk_label_set_xalign(GTK_LABEL(jitter_label), 0.0);
    gtk_grid_attach(GTK_GRID(more_info_grid), jitter_label, 0, row, 1, 1);
    GtkWidget *jitter_value = gtk_label_new("2 ms");  /* TODO: Calculate */
    gtk_label_set_xalign(GTK_LABEL(jitter_value), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(jitter_value), "info-value");
    gtk_grid_attach(GTK_GRID(more_info_grid), jitter_value, 1, row++, 1, 1);

    GtkWidget *loss_label = gtk_label_new("Packet Loss:");
    gtk_label_set_xalign(GTK_LABEL(loss_label), 0.0);
    gtk_grid_attach(GTK_GRID(more_info_grid), loss_label, 0, row, 1, 1);
    GtkWidget *loss_value = gtk_label_new("0.00%");  /* TODO: Calculate from stats */
    gtk_label_set_xalign(GTK_LABEL(loss_value), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(loss_value), "info-value");
    gtk_grid_attach(GTK_GRID(more_info_grid), loss_value, 1, row++, 1, 1);

    row++;  /* Spacer */

    /* Internal Section */
    GtkWidget *int_header = gtk_label_new("INTERNAL");
    gtk_style_context_add_class(gtk_widget_get_style_context(int_header), "info-label");
    gtk_label_set_xalign(GTK_LABEL(int_header), 0.0);
    gtk_grid_attach(GTK_GRID(more_info_grid), int_header, 0, row++, 2, 1);

    GtkWidget *dns_label = gtk_label_new("Virtual DNS:");
    gtk_label_set_xalign(GTK_LABEL(dns_label), 0.0);
    gtk_grid_attach(GTK_GRID(more_info_grid), dns_label, 0, row, 1, 1);
    char dns_text[128];
    if (session->device_name && get_interface_gateway(session->device_name, gateway, sizeof(gateway)) == 0) {
        snprintf(dns_text, sizeof(dns_text), "%s", gateway);
    } else {
        snprintf(dns_text, sizeof(dns_text), "N/A");
    }
    GtkWidget *dns_value = gtk_label_new(dns_text);
    gtk_label_set_xalign(GTK_LABEL(dns_value), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(dns_value), "info-value");
    gtk_grid_attach(GTK_GRID(more_info_grid), dns_value, 1, row++, 1, 1);

    GtkWidget *routing_label = gtk_label_new("Routing Flags:");
    gtk_label_set_xalign(GTK_LABEL(routing_label), 0.0);
    gtk_grid_attach(GTK_GRID(more_info_grid), routing_label, 0, row, 1, 1);
    GtkWidget *routing_value = gtk_label_new("UG (Gateway)");  /* TODO: Parse from route */
    gtk_label_set_xalign(GTK_LABEL(routing_value), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(routing_value), "info-value");
    gtk_grid_attach(GTK_GRID(more_info_grid), routing_value, 1, row++, 1, 1);

    GtkWidget *peer_label = gtk_label_new("Peer ID:");
    gtk_label_set_xalign(GTK_LABEL(peer_label), 0.0);
    gtk_grid_attach(GTK_GRID(more_info_grid), peer_label, 0, row, 1, 1);
    GtkWidget *peer_value = gtk_label_new("0");  /* TODO: Get from session */
    gtk_label_set_xalign(GTK_LABEL(peer_value), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(peer_value), "info-value");
    gtk_grid_attach(GTK_GRID(more_info_grid), peer_value, 1, row++, 1, 1);

    gtk_container_add(GTK_CONTAINER(revealer), more_info_grid);
    gtk_box_pack_start(GTK_BOX(card), revealer, FALSE, FALSE, 0);

    /* More Info button with revealer toggle */
    GtkWidget *info_btn = gtk_button_new_with_label("More Info ▼");
    g_signal_connect(info_btn, "clicked", G_CALLBACK(on_info_toggled), revealer);
    gtk_box_pack_start(GTK_BOX(header), info_btn, FALSE, FALSE, 0);

    return card;
}

/**
 * Create a tab label with icon and text
 */
static GtkWidget* create_tab_label(const char *icon_name, const char *text) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_MENU);
    GtkWidget *label = gtk_label_new(text);
    gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    gtk_widget_show_all(box);
    return box;
}

/**
 * Window delete event handler - hide instead of destroy
 */
static gboolean on_window_delete(GtkWidget *widget, GdkEvent *event, gpointer data) {
    (void)event;
    (void)data;

    /* Hide instead of destroy */
    gtk_widget_hide(widget);

    /* Return TRUE to prevent window destruction */
    return TRUE;
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

    logger_info("Dashboard: Disconnecting session %s", session_path);
    int r = session_disconnect(dashboard->bus, session_path);
    if (r < 0) {
        logger_error("Failed to disconnect session");
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

    logger_info("Dashboard: Connecting to config %s", config_path);
    char *session_path = NULL;
    int r = session_start(dashboard->bus, config_path, &session_path);
    if (r < 0) {
        logger_error("Failed to start VPN session");
    } else {
        logger_info("Started VPN session: %s", session_path);
        g_free(session_path);
        /* Update dashboard after connect */
        dashboard_update(dashboard, dashboard->bus);
    }
}

/**
 * Import button callback
 */
static void on_import_clicked(GtkButton *button, gpointer data) {
    (void)button;
    Dashboard *dashboard = (Dashboard *)data;

    if (!dashboard || !dashboard->bus) {
        logger_error("No D-Bus connection available");
        dialog_show_error("Import Error", "No D-Bus connection available");
        return;
    }

    /* Show file chooser */
    char *file_path = file_chooser_select_ovpn("Import OpenVPN Configuration");
    if (!file_path) {
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

    /* Remove .ovpn or .conf extension */
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
    r = config_import(dashboard->bus, config_name, contents, false, true, &config_path);

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
        /* Update dashboard to show new config */
        dashboard_update(dashboard, dashboard->bus);
    }

    g_free(config_name);
    g_free(contents);
    g_free(file_path);
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
 * Create the "Import Config" row with button at the end of the list
 */
static void create_import_config_row(Dashboard *dashboard) {
    /* Create list box row */
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);

    /* Row content box */
    GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(row_box), 12);

    /* Import button */
    GtkWidget *import_btn = gtk_button_new_with_label("+ Import");
    g_signal_connect(import_btn, "clicked", G_CALLBACK(on_import_clicked), dashboard);
    gtk_box_pack_start(GTK_BOX(row_box), import_btn, FALSE, FALSE, 0);

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
    gtk_window_set_default_size(GTK_WINDOW(dashboard->window), 780, 600);
    gtk_window_set_position(GTK_WINDOW(dashboard->window), GTK_WIN_POS_CENTER);
    gtk_container_set_border_width(GTK_CONTAINER(dashboard->window), 0);

    /* Header bar (CSD) */
    dashboard->header_bar = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(dashboard->header_bar), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(dashboard->header_bar), "OpenVPN Manager");
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(dashboard->header_bar), "No active connections");

    GtkWidget *settings_btn = gtk_button_new_from_icon_name("preferences-system-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(settings_btn, "Settings");
    gtk_widget_set_sensitive(settings_btn, FALSE);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(dashboard->header_bar), settings_btn);

    gtk_window_set_titlebar(GTK_WINDOW(dashboard->window), dashboard->header_bar);

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
                            create_tab_label("network-wired-symbolic", "Connections"));

    /* Tab 2: Statistics - Card-based view */
    GtkWidget *stats_scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(stats_scrolled),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);

    /* Main container for statistics tab */
    dashboard->statistics_tab = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* Aggregate bandwidth section (fixed above scrolling cards) */
    dashboard->aggregate_graph_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(dashboard->aggregate_graph_box, 20);
    gtk_widget_set_margin_end(dashboard->aggregate_graph_box, 20);
    gtk_widget_set_margin_top(dashboard->aggregate_graph_box, 16);
    gtk_widget_set_no_show_all(dashboard->aggregate_graph_box, TRUE);

    GtkWidget *agg_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *agg_title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(agg_title),
        "<span size='large' weight='600'>Total Bandwidth</span>");
    gtk_label_set_xalign(GTK_LABEL(agg_title), 0.0);
    gtk_box_pack_start(GTK_BOX(agg_header), agg_title, TRUE, TRUE, 0);

    dashboard->aggregate_dl_label = gtk_label_new("↓ 0 B/s");
    gtk_style_context_add_class(gtk_widget_get_style_context(dashboard->aggregate_dl_label), "card-bandwidth-download");
    gtk_box_pack_start(GTK_BOX(agg_header), dashboard->aggregate_dl_label, FALSE, FALSE, 0);

    dashboard->aggregate_ul_label = gtk_label_new("↑ 0 B/s");
    gtk_style_context_add_class(gtk_widget_get_style_context(dashboard->aggregate_ul_label), "card-bandwidth-upload");
    gtk_box_pack_start(GTK_BOX(agg_header), dashboard->aggregate_ul_label, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(dashboard->aggregate_graph_box), agg_header, FALSE, FALSE, 0);

    dashboard->aggregate_graph = gtk_drawing_area_new();
    gtk_widget_set_size_request(dashboard->aggregate_graph, -1, 180);
    gtk_style_context_add_class(gtk_widget_get_style_context(dashboard->aggregate_graph), "card-graph-area");
    g_signal_connect(dashboard->aggregate_graph, "draw", G_CALLBACK(on_aggregate_graph_draw), dashboard);
    gtk_box_pack_start(GTK_BOX(dashboard->aggregate_graph_box), dashboard->aggregate_graph, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(dashboard->statistics_tab), dashboard->aggregate_graph_box, FALSE, FALSE, 0);

    /* FlowBox for stat cards - wraps automatically */
    dashboard->stats_flowbox = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(dashboard->stats_flowbox), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(dashboard->stats_flowbox), FALSE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(dashboard->stats_flowbox), 4);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(dashboard->stats_flowbox), 10);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(dashboard->stats_flowbox), 10);
    gtk_widget_set_margin_start(dashboard->stats_flowbox, 20);
    gtk_widget_set_margin_end(dashboard->stats_flowbox, 20);
    gtk_widget_set_margin_top(dashboard->stats_flowbox, 20);
    gtk_widget_set_margin_bottom(dashboard->stats_flowbox, 20);

    gtk_container_add(GTK_CONTAINER(stats_scrolled), dashboard->stats_flowbox);
    gtk_box_pack_start(GTK_BOX(dashboard->statistics_tab), stats_scrolled, TRUE, TRUE, 0);

    /* Empty state (shown when no VPNs connected) */
    dashboard->stats_empty_state = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_widget_set_valign(dashboard->stats_empty_state, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(dashboard->stats_empty_state, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(dashboard->stats_empty_state, TRUE);

    GtkWidget *empty_icon = gtk_image_new_from_icon_name("network-offline-symbolic", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(empty_icon), 96);
    gtk_widget_set_opacity(empty_icon, 0.3);
    gtk_box_pack_start(GTK_BOX(dashboard->stats_empty_state), empty_icon, FALSE, FALSE, 0);

    GtkWidget *empty_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(empty_label),
                        "<span size='large' weight='600'>No active connections</span>\n"
                        "<span foreground='#888888'>Connect to a VPN to see statistics</span>");
    gtk_label_set_justify(GTK_LABEL(empty_label), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(dashboard->stats_empty_state), empty_label, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(dashboard->statistics_tab), dashboard->stats_empty_state, TRUE, TRUE, 0);
    gtk_widget_set_no_show_all(dashboard->stats_empty_state, TRUE);

    gtk_notebook_append_page(GTK_NOTEBOOK(dashboard->notebook), dashboard->statistics_tab,
                            create_tab_label("utilities-system-monitor-symbolic", "Statistics"));

    /* Tab 3: Servers */
    dashboard->servers_tab_instance = servers_tab_create(NULL);  /* bus will be set later */
    dashboard->servers_tab = servers_tab_get_widget(dashboard->servers_tab_instance);
    gtk_notebook_append_page(GTK_NOTEBOOK(dashboard->notebook), dashboard->servers_tab,
                            create_tab_label("network-server-symbolic", "Servers"));

    /* Initialize bandwidth monitors hash table */
    dashboard->bandwidth_monitors = g_hash_table_new_full(
        g_str_hash,
        g_str_equal,
        g_free,
        (GDestroyNotify)bandwidth_monitor_free
    );

    /* Main container: notebook + status bar */
    GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(main_vbox), dashboard->notebook, TRUE, TRUE, 0);

    /* Status bar */
    dashboard->status_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(dashboard->status_bar), "status-bar");
    dashboard->status_label = gtk_label_new("No active connections");
    gtk_label_set_xalign(GTK_LABEL(dashboard->status_label), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(dashboard->status_label), "status-bar");
    gtk_box_pack_start(GTK_BOX(dashboard->status_bar), dashboard->status_label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(main_vbox), dashboard->status_bar, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(dashboard->window), main_vbox);

    logger_info("Dashboard window created");

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

    /* Update Statistics Tab - Card-based view for multiple sessions */
    /* Clear existing stat cards */
    gtk_container_foreach(GTK_CONTAINER(dashboard->stats_flowbox),
                         (GtkCallback)gtk_widget_destroy, NULL);

    if (session_count > 0 && sessions) {
        /* Hide empty state */
        gtk_widget_hide(dashboard->stats_empty_state);

        /* Create/update stat cards for each session */
        for (unsigned int i = 0; i < session_count; i++) {
            VpnSession *session = sessions[i];

            if (!session->device_name || !session->session_path) {
                continue;  /* Skip sessions without device */
            }

            /* Get or create bandwidth monitor for this session */
            BandwidthMonitor *monitor = g_hash_table_lookup(
                dashboard->bandwidth_monitors,
                session->session_path
            );

            if (!monitor) {
                /* Create new monitor */
                logger_debug("Dashboard: Creating bandwidth monitor for session %s (device: %s)",
                       session->config_name ? session->config_name : "unknown",
                       session->device_name);

                monitor = bandwidth_monitor_create(
                    session->session_path,
                    session->device_name,
                    STATS_SOURCE_AUTO,
                    7200  /* 2-hour buffer (7200 seconds) */
                );

                if (monitor) {
                    g_hash_table_insert(
                        dashboard->bandwidth_monitors,
                        g_strdup(session->session_path),
                        monitor
                    );
                    logger_debug("Dashboard: Bandwidth monitor created successfully");
                } else {
                    logger_debug("Dashboard: Failed to create bandwidth monitor");
                    continue;
                }
            }

            /* Update the monitor */
            bandwidth_monitor_update(monitor, bus);

            /* Create stat card for this session */
            GtkWidget *card = create_vpn_stat_card(dashboard, session, monitor);
            if (card) {
                gtk_container_add(GTK_CONTAINER(dashboard->stats_flowbox), card);

                /* Update card with live data */
                BandwidthRate rate;
                if (bandwidth_monitor_get_rate(monitor, &rate) >= 0) {
                    /* Update throughput labels */
                    GtkWidget *download_label = g_object_get_data(G_OBJECT(card), "download-label");
                    GtkWidget *upload_label = g_object_get_data(G_OBJECT(card), "upload-label");

                    if (download_label && upload_label) {
                        char text[256];
                        format_bytes((uint64_t)rate.download_rate_bps, text, sizeof(text));
                        char label_text[256];
                        snprintf(label_text, sizeof(label_text), "↓ %s/s", text);
                        gtk_label_set_text(GTK_LABEL(download_label), label_text);

                        format_bytes((uint64_t)rate.upload_rate_bps, text, sizeof(text));
                        snprintf(label_text, sizeof(label_text), "↑ %s/s", text);
                        gtk_label_set_text(GTK_LABEL(upload_label), label_text);
                    }
                }

                /* Update packet statistics */
                BandwidthSample sample;
                if (bandwidth_monitor_get_latest_sample(monitor, &sample) >= 0) {
                    GtkWidget *sent_label = g_object_get_data(G_OBJECT(card), "sent-label");
                    GtkWidget *received_label = g_object_get_data(G_OBJECT(card), "received-label");
                    GtkWidget *errors_label = g_object_get_data(G_OBJECT(card), "errors-label");

                    if (sent_label) {
                        char label_text[256];
                        snprintf(label_text, sizeof(label_text), "Sent:     %lu", sample.packets_out);
                        gtk_label_set_text(GTK_LABEL(sent_label), label_text);
                    }
                    if (received_label) {
                        char label_text[256];
                        snprintf(label_text, sizeof(label_text), "Received: %lu", sample.packets_in);
                        gtk_label_set_text(GTK_LABEL(received_label), label_text);
                    }
                    if (errors_label) {
                        char label_text[256];
                        snprintf(label_text, sizeof(label_text), "Errors:   %lu",
                                sample.errors_in + sample.errors_out);
                        gtk_label_set_text(GTK_LABEL(errors_label), label_text);
                    }
                }

                /* Queue redraw for sparkline graph */
                GtkWidget *graph_area = g_object_get_data(G_OBJECT(card), "graph-area");
                if (graph_area) {
                    gtk_widget_queue_draw(graph_area);
                }
            }
        }

        gtk_widget_show_all(dashboard->stats_flowbox);

        /* Show aggregate graph */
        if (dashboard->aggregate_graph_box) {
            gtk_widget_show_all(dashboard->aggregate_graph_box);
        }
    } else {
        /* No active sessions - show empty state */
        gtk_widget_show(dashboard->stats_empty_state);

        /* Hide aggregate graph */
        if (dashboard->aggregate_graph_box) {
            gtk_widget_hide(dashboard->aggregate_graph_box);
        }
    }

    /* Update header bar subtitle */
    if (dashboard->header_bar) {
        char subtitle[128];
        if (session_count > 0)
            snprintf(subtitle, sizeof(subtitle), "%u active connection%s",
                     session_count, session_count > 1 ? "s" : "");
        else
            snprintf(subtitle, sizeof(subtitle), "No active connections");
        gtk_header_bar_set_subtitle(GTK_HEADER_BAR(dashboard->header_bar), subtitle);
    }

    /* Aggregate bandwidth data for status bar and aggregate graph */
    {
        double total_dl = 0, total_ul = 0;
        time_t longest_uptime = 0;
        GHashTableIter bw_iter;
        gpointer bw_key, bw_value;
        g_hash_table_iter_init(&bw_iter, dashboard->bandwidth_monitors);
        while (g_hash_table_iter_next(&bw_iter, &bw_key, &bw_value)) {
            BandwidthMonitor *mon = (BandwidthMonitor *)bw_value;
            BandwidthRate bw_rate;
            if (bandwidth_monitor_get_rate(mon, &bw_rate) >= 0) {
                total_dl += bw_rate.download_rate_bps;
                total_ul += bw_rate.upload_rate_bps;
            }
            time_t start = bandwidth_monitor_get_start_time(mon);
            if (start > 0) {
                time_t uptime = time(NULL) - start;
                if (uptime > longest_uptime) longest_uptime = uptime;
            }
        }

        /* Store in aggregate history buffer */
        dashboard->aggregate_dl_history[dashboard->aggregate_write_idx] = total_dl;
        dashboard->aggregate_ul_history[dashboard->aggregate_write_idx] = total_ul;
        dashboard->aggregate_write_idx = (dashboard->aggregate_write_idx + 1) % 120;
        if (dashboard->aggregate_sample_count < 120) dashboard->aggregate_sample_count++;

        /* Update aggregate graph labels */
        if (dashboard->aggregate_dl_label && dashboard->aggregate_ul_label) {
            char agg_text[64], agg_label[80];
            format_bytes((uint64_t)total_dl, agg_text, sizeof(agg_text));
            snprintf(agg_label, sizeof(agg_label), "↓ %s/s", agg_text);
            gtk_label_set_text(GTK_LABEL(dashboard->aggregate_dl_label), agg_label);

            format_bytes((uint64_t)total_ul, agg_text, sizeof(agg_text));
            snprintf(agg_label, sizeof(agg_label), "↑ %s/s", agg_text);
            gtk_label_set_text(GTK_LABEL(dashboard->aggregate_ul_label), agg_label);
        }

        /* Queue aggregate graph redraw */
        if (dashboard->aggregate_graph) {
            gtk_widget_queue_draw(dashboard->aggregate_graph);
        }

        /* Update status bar */
        if (dashboard->status_label) {
            if (session_count > 0) {
                char dl_s[32], ul_s[32], ut_s[32], status_text[256];
                format_bytes((uint64_t)total_dl, dl_s, sizeof(dl_s));
                format_bytes((uint64_t)total_ul, ul_s, sizeof(ul_s));
                format_elapsed_time(longest_uptime, ut_s, sizeof(ut_s));
                snprintf(status_text, sizeof(status_text),
                        "↓ %s/s  ↑ %s/s  ·  %u connection%s  ·  Uptime: %s",
                        dl_s, ul_s, session_count, session_count > 1 ? "s" : "", ut_s);
                gtk_label_set_text(GTK_LABEL(dashboard->status_label), status_text);
            } else {
                gtk_label_set_text(GTK_LABEL(dashboard->status_label), "No active connections");
            }
        }
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

    /* Clean up bandwidth monitors hash table */
    if (dashboard->bandwidth_monitors) {
        g_hash_table_destroy(dashboard->bandwidth_monitors);
        dashboard->bandwidth_monitors = NULL;
    }

    /* Clean up servers tab */
    if (dashboard->servers_tab_instance) {
        servers_tab_free(dashboard->servers_tab_instance);
    }

    if (dashboard->window) {
        gtk_widget_destroy(dashboard->window);
    }

    g_free(dashboard);
    logger_info("Dashboard destroyed");
}
