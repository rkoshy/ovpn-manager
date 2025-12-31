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

/**
 * Show a dialog to get text input from user
 */
char* dialog_get_text_input(const char *title, const char *prompt, const char *default_value) {
    GtkWidget *dialog;
    GtkWidget *content_area;
    GtkWidget *entry;
    GtkWidget *label;
    GtkWidget *hbox;
    char *result = NULL;

    /* Create dialog */
    dialog = gtk_dialog_new_with_buttons(
        title ? title : "Input",
        NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_OK", GTK_RESPONSE_ACCEPT,
        NULL
    );

    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);

    content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content_area), 10);

    /* Create horizontal box with label and entry */
    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_container_add(GTK_CONTAINER(content_area), hbox);

    label = gtk_label_new(prompt ? prompt : "Value:");
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);

    entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 40);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    if (default_value) {
        gtk_entry_set_text(GTK_ENTRY(entry), default_value);
        gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
    }
    gtk_box_pack_start(GTK_BOX(hbox), entry, TRUE, TRUE, 0);

    gtk_widget_show_all(dialog);

    /* Run dialog */
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));

    if (response == GTK_RESPONSE_ACCEPT) {
        const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
        if (text && strlen(text) > 0) {
            result = g_strdup(text);
        }
    }

    gtk_widget_destroy(dialog);

    /* Process pending events */
    while (gtk_events_pending()) {
        gtk_main_iteration();
    }

    return result;
}

/**
 * Show an error message dialog
 */
void dialog_show_error(const char *title, const char *message) {
    GtkWidget *dialog = gtk_message_dialog_new(
        NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_ERROR,
        GTK_BUTTONS_OK,
        "%s",
        message ? message : "An error occurred"
    );

    gtk_window_set_title(GTK_WINDOW(dialog), title ? title : "Error");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    /* Process pending events */
    while (gtk_events_pending()) {
        gtk_main_iteration();
    }
}

/**
 * Show an info message dialog
 */
void dialog_show_info(const char *title, const char *message) {
    GtkWidget *dialog = gtk_message_dialog_new(
        NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_INFO,
        GTK_BUTTONS_OK,
        "%s",
        message ? message : "Operation completed"
    );

    gtk_window_set_title(GTK_WINDOW(dialog), title ? title : "Information");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    /* Process pending events */
    while (gtk_events_pending()) {
        gtk_main_iteration();
    }
}
