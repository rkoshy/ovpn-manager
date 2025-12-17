#include "widgets.h"
#include <string.h>
#include <time.h>

/**
 * Get CSS class for session state
 */
const char* widget_get_state_css_class(SessionState state) {
    switch (state) {
        case SESSION_STATE_CONNECTED:
            return "session-connected";
        case SESSION_STATE_CONNECTING:
        case SESSION_STATE_RECONNECTING:
            return "session-connecting";
        case SESSION_STATE_PAUSED:
            return "session-paused";
        case SESSION_STATE_AUTH_REQUIRED:
            return "session-auth-required";
        case SESSION_STATE_ERROR:
            return "session-error";
        default:
            return NULL;
    }
}

/**
 * Get icon name for session state
 */
const char* widget_get_state_icon(SessionState state) {
    switch (state) {
        case SESSION_STATE_CONNECTED:
            return "emblem-default";  /* Green checkmark */
        case SESSION_STATE_CONNECTING:
        case SESSION_STATE_RECONNECTING:
            return "emblem-synchronizing";  /* Orange sync */
        case SESSION_STATE_PAUSED:
            return "media-playback-pause";  /* Pause */
        case SESSION_STATE_AUTH_REQUIRED:
            return "dialog-password";  /* Key/password */
        case SESSION_STATE_ERROR:
            return "dialog-error";  /* Red X */
        default:
            return "network-vpn";  /* Generic VPN icon */
    }
}

/**
 * Get human-readable state text
 */
const char* widget_get_state_text(SessionState state) {
    switch (state) {
        case SESSION_STATE_CONNECTED:
            return "Connected";
        case SESSION_STATE_CONNECTING:
            return "Connecting...";
        case SESSION_STATE_RECONNECTING:
            return "Reconnecting...";
        case SESSION_STATE_PAUSED:
            return "Paused";
        case SESSION_STATE_AUTH_REQUIRED:
            return "Auth Required";
        case SESSION_STATE_ERROR:
            return "Error";
        default:
            return "Disconnected";
    }
}

/**
 * Create a styled menu item with optional icon and CSS class
 */
GtkWidget* widget_create_menu_item(const char *label,
                                    const char *icon_name,
                                    const char *css_class) {
    printf("DEBUG: Creating menu item: label='%s', icon='%s', css='%s'\n",
           label ? label : "NULL",
           icon_name ? icon_name : "NULL",
           css_class ? css_class : "NULL");

    GtkWidget *menu_item;

    /* Use GtkImageMenuItem if icon is provided (for AppIndicator compatibility) */
    if (icon_name) {
        GtkWidget *icon = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_MENU);
        menu_item = gtk_image_menu_item_new_with_label(label);
        gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(menu_item), icon);
        gtk_image_menu_item_set_always_show_image(GTK_IMAGE_MENU_ITEM(menu_item), TRUE);
        printf("DEBUG:   -> Created ImageMenuItem with icon '%s'\n", icon_name);
    } else {
        menu_item = gtk_menu_item_new_with_label(label);
    }

    /* Apply CSS class if provided */
    if (css_class) {
        GtkStyleContext *context = gtk_widget_get_style_context(menu_item);
        gtk_style_context_add_class(context, css_class);
        printf("DEBUG:   -> Applied CSS class '%s'\n", css_class);
    }

    return menu_item;
}

/**
 * Create a session menu item with state-based styling
 */
GtkWidget* widget_create_session_item(VpnSession *session, gboolean with_timer) {
    if (!session) {
        return NULL;
    }

    const char *state_text = widget_get_state_text(session->state);
    const char *icon_name = widget_get_state_icon(session->state);
    const char *css_class = widget_get_state_css_class(session->state);

    /* Build label with session name and state */
    char label[256];
    if (with_timer && session->state == SESSION_STATE_CONNECTED) {
        /* Timer will be added separately - just show state */
        snprintf(label, sizeof(label), "%s: %s",
                session->config_name ? session->config_name : "Unknown",
                state_text);
    } else {
        snprintf(label, sizeof(label), "%s: %s",
                session->config_name ? session->config_name : "Unknown",
                state_text);
    }

    return widget_create_menu_item(label, icon_name, css_class);
}

/**
 * Create a configuration menu item
 */
GtkWidget* widget_create_config_item(const char *config_name, gboolean is_in_use) {
    const char *icon_name = is_in_use ? "emblem-default" : "network-vpn";
    const char *css_class = is_in_use ? "config-item" : "config-item";

    GtkWidget *item = widget_create_menu_item(config_name, icon_name, css_class);

    /* Disable if in use */
    if (is_in_use) {
        gtk_widget_set_sensitive(item, FALSE);
    }

    return item;
}

/**
 * Create a section header menu item
 */
GtkWidget* widget_create_section_header(const char *label) {
    GtkWidget *menu_item = gtk_menu_item_new();
    GtkWidget *label_widget = gtk_label_new(label);

    /* Apply header styling */
    GtkStyleContext *context = gtk_widget_get_style_context(label_widget);
    gtk_style_context_add_class(context, "section-header");

    gtk_label_set_xalign(GTK_LABEL(label_widget), 0.0);
    gtk_container_add(GTK_CONTAINER(menu_item), label_widget);
    gtk_widget_show(label_widget);

    /* Disable interaction */
    gtk_widget_set_sensitive(menu_item, FALSE);

    return menu_item;
}

/**
 * Create a metadata/info menu item (small, gray text)
 */
GtkWidget* widget_create_metadata_item(const char *text) {
    GtkWidget *menu_item = gtk_menu_item_new();

    /* Create label with markup for small size and gray color */
    char markup[256];
    snprintf(markup, sizeof(markup),
            "<span size='x-small' foreground='#888888'>%s</span>",
            text);

    GtkWidget *label_widget = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label_widget), markup);
    gtk_label_set_xalign(GTK_LABEL(label_widget), 0.0);

    /* Apply metadata styling */
    GtkStyleContext *context = gtk_widget_get_style_context(label_widget);
    gtk_style_context_add_class(context, "metadata");

    gtk_container_add(GTK_CONTAINER(menu_item), label_widget);
    gtk_widget_show(label_widget);

    /* Disable interaction */
    gtk_widget_set_sensitive(menu_item, FALSE);

    return menu_item;
}

/**
 * Update a label widget with markup
 */
void widget_update_label(GtkWidget *label, const char *text, const char *css_class) {
    if (!GTK_IS_LABEL(label) || !text) {
        return;
    }

    gtk_label_set_text(GTK_LABEL(label), text);

    if (css_class) {
        GtkStyleContext *context = gtk_widget_get_style_context(label);
        gtk_style_context_add_class(context, css_class);
    }
}
