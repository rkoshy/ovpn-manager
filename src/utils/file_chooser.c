#include "file_chooser.h"
#include <gtk/gtk.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

/**
 * Show a file chooser dialog for selecting an OVPN file
 */
char* file_chooser_select_ovpn(const char *title) {
    GtkWidget *dialog;
    GtkFileChooser *chooser;
    GtkFileFilter *filter;
    char *filename = NULL;

    /* Create file chooser dialog */
    dialog = gtk_file_chooser_dialog_new(
        title ? title : "Select OpenVPN Configuration",
        NULL,
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open", GTK_RESPONSE_ACCEPT,
        NULL
    );

    chooser = GTK_FILE_CHOOSER(dialog);

    /* Add file filter for .ovpn files */
    filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "OpenVPN Config Files");
    gtk_file_filter_add_pattern(filter, "*.ovpn");
    gtk_file_filter_add_pattern(filter, "*.conf");
    gtk_file_chooser_add_filter(chooser, filter);

    /* Add "All Files" filter */
    filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "All Files");
    gtk_file_filter_add_pattern(filter, "*");
    gtk_file_chooser_add_filter(chooser, filter);

    /* Show dialog and get response */
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));

    if (response == GTK_RESPONSE_ACCEPT) {
        filename = gtk_file_chooser_get_filename(chooser);
    }

    gtk_widget_destroy(dialog);

    /* Process pending events to clean up dialog */
    while (gtk_events_pending()) {
        gtk_main_iteration();
    }

    return filename;
}

/**
 * Read entire file contents into a string
 */
int file_read_contents(const char *file_path, char **contents, char **error) {
    GError *gerror = NULL;
    gchar *file_contents = NULL;
    gsize length = 0;

    if (!file_path || !contents) {
        if (error) {
            *error = g_strdup("Invalid parameters");
        }
        return -EINVAL;
    }

    *contents = NULL;

    /* Read file using GLib */
    if (!g_file_get_contents(file_path, &file_contents, &length, &gerror)) {
        if (error) {
            *error = g_strdup(gerror ? gerror->message : "Failed to read file");
        }
        if (gerror) {
            g_error_free(gerror);
        }
        return -EIO;
    }

    /* Validate it's not empty */
    if (length == 0 || !file_contents) {
        if (error) {
            *error = g_strdup("File is empty");
        }
        g_free(file_contents);
        return -EINVAL;
    }

    /* Validate it looks like an OVPN file (basic check) */
    if (!g_strstr_len(file_contents, length, "client") &&
        !g_strstr_len(file_contents, length, "remote")) {
        if (error) {
            *error = g_strdup("File does not appear to be a valid OpenVPN configuration");
        }
        g_free(file_contents);
        return -EINVAL;
    }

    *contents = file_contents;
    return 0;
}
