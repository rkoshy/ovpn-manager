#ifndef THEME_H
#define THEME_H

#include <gtk/gtk.h>
#include <stdbool.h>

/**
 * Theme mode enumeration
 */
typedef enum {
    THEME_MODE_LIGHT,
    THEME_MODE_DARK
} ThemeMode;

/**
 * Theme callback function type
 * Called when theme changes (light/dark mode switch)
 */
typedef void (*ThemeChangeCallback)(ThemeMode mode, gpointer user_data);

/**
 * Initialize theme system
 * Sets up CSS providers and theme detection
 * Returns 0 on success, -1 on failure
 */
int theme_init(void);

/**
 * Get current theme mode
 * Returns THEME_MODE_LIGHT or THEME_MODE_DARK
 */
ThemeMode theme_get_current_mode(void);

/**
 * Register callback for theme changes
 * Callback will be invoked when system theme switches between light/dark
 */
void theme_register_callback(ThemeChangeCallback callback, gpointer user_data);

/**
 * Reload CSS for current theme mode
 * Useful for forcing a refresh after CSS updates
 */
void theme_reload_css(void);

/**
 * Cleanup theme system
 * Unregister callbacks and free resources
 */
void theme_cleanup(void);

/**
 * Get color value for current theme
 * Returns color string (e.g., "#007AFF" or "rgba(0, 122, 255, 0.8)")
 */
const char* theme_get_color(const char *color_name);

#endif /* THEME_H */
