#ifndef WIDGETS_H
#define WIDGETS_H

#include <gtk/gtk.h>
#include "../dbus/session_client.h"

/**
 * Create a styled menu item with optional icon and CSS class
 *
 * @param label Text label for the menu item
 * @param icon_name Optional icon name (NULL for no icon)
 * @param css_class Optional CSS class for styling (NULL for default)
 * @return GtkWidget* - The created menu item
 */
GtkWidget* widget_create_menu_item(const char *label,
                                    const char *icon_name,
                                    const char *css_class);

/**
 * Create a session menu item with state-based styling
 *
 * @param session VPN session information
 * @param with_timer If true, includes connection duration
 * @return GtkWidget* - The created menu item
 */
GtkWidget* widget_create_session_item(VpnSession *session, gboolean with_timer);

/**
 * Create a configuration menu item
 *
 * @param config_name Configuration name
 * @param is_in_use Whether config is currently in use
 * @return GtkWidget* - The created menu item
 */
GtkWidget* widget_create_config_item(const char *config_name, gboolean is_in_use);

/**
 * Create a section header menu item
 *
 * @param label Header text
 * @return GtkWidget* - The created header item (disabled)
 */
GtkWidget* widget_create_section_header(const char *label);

/**
 * Create a metadata/info menu item (small, gray text)
 *
 * @param text Metadata text
 * @return GtkWidget* - The created metadata item (disabled)
 */
GtkWidget* widget_create_metadata_item(const char *text);

/**
 * Update a label widget with markup
 *
 * @param label GtkLabel widget
 * @param text New text
 * @param css_class Optional CSS class to apply
 */
void widget_update_label(GtkWidget *label, const char *text, const char *css_class);

/**
 * Get CSS class for session state
 *
 * @param state Session state
 * @return const char* - CSS class name
 */
const char* widget_get_state_css_class(SessionState state);

/**
 * Get icon name for session state
 *
 * @param state Session state
 * @return const char* - Icon name or NULL
 */
const char* widget_get_state_icon(SessionState state);

/**
 * Get human-readable state text
 *
 * @param state Session state
 * @return const char* - State text
 */
const char* widget_get_state_text(SessionState state);

#endif /* WIDGETS_H */
