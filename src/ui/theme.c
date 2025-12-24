#include "theme.h"
#include "../utils/logger.h"
#include <stdio.h>
#include <string.h>
#include <glib.h>

/* CSS content for light and dark modes */
static const char *CSS_LIGHT_MODE =
"@define-color primary_blue #007AFF;\n"
"@define-color success_green #34C759;\n"
"@define-color warning_orange #FF9500;\n"
"@define-color danger_red #FF3B30;\n"
"@define-color pause_purple #AF52DE;\n"
"\n"
"@define-color bg_primary rgba(255, 255, 255, 0.92);\n"
"@define-color bg_secondary rgba(246, 246, 246, 0.85);\n"
"@define-color bg_hover rgba(0, 122, 255, 0.08);\n"
"@define-color bg_active rgba(0, 122, 255, 0.12);\n"
"\n"
"@define-color text_primary #1C1C1E;\n"
"@define-color text_secondary #8E8E93;\n"
"@define-color text_tertiary #C7C7CC;\n"
"\n"
"@define-color border_light rgba(0, 0, 0, 0.06);\n"
"@define-color separator rgba(0, 0, 0, 0.04);\n"
"\n"
"/* Menu styling */\n"
"menu {\n"
"    background-color: @bg_primary;\n"
"    border-radius: 8px;\n"
"    border: 1px solid @border_light;\n"
"    padding: 4px;\n"
"}\n"
"\n"
"menuitem {\n"
"    border-radius: 4px;\n"
"    padding: 6px 12px;\n"
"    margin: 2px 0;\n"
"    color: @text_primary;\n"
"    font-size: 13px;\n"
"}\n"
"\n"
"menuitem:hover {\n"
"    background-color: @bg_hover;\n"
"    transition: background-color 100ms ease-out;\n"
"}\n"
"\n"
"menuitem:active {\n"
"    background-color: @bg_active;\n"
"}\n"
"\n"
"menuitem:disabled {\n"
"    color: @text_tertiary;\n"
"}\n"
"\n"
"separator {\n"
"    background-color: @separator;\n"
"    min-height: 1px;\n"
"    margin: 4px 8px;\n"
"}\n"
"\n"
"/* Custom widget classes */\n"
".session-connected {\n"
"    color: @success_green;\n"
"    font-weight: 600;\n"
"}\n"
"\n"
".session-connecting {\n"
"    color: @warning_orange;\n"
"    font-weight: 500;\n"
"}\n"
"\n"
".session-paused {\n"
"    color: @pause_purple;\n"
"    font-weight: 500;\n"
"}\n"
"\n"
".session-error {\n"
"    color: @danger_red;\n"
"    font-weight: 500;\n"
"}\n"
"\n"
".session-auth-required {\n"
"    color: @warning_orange;\n"
"    font-weight: 500;\n"
"}\n"
"\n"
".config-item {\n"
"    color: @text_primary;\n"
"}\n"
"\n"
".config-item:disabled {\n"
"    color: @text_tertiary;\n"
"}\n"
"\n"
".section-header {\n"
"    color: @text_secondary;\n"
"    font-size: 11px;\n"
"    font-weight: 600;\n"
"}\n"
"\n"
".metadata {\n"
"    color: @text_secondary;\n"
"    font-size: 11px;\n"
"}\n"
"\n"
"/* Dashboard window styling */\n"
".dashboard-window {\n"
"    background-color: @bg_secondary;\n"
"}\n"
"\n"
".session-card {\n"
"    background-color: @bg_primary;\n"
"    border-radius: 12px;\n"
"    border: 1px solid @border_light;\n"
"    padding: 16px;\n"
"}\n"
"\n"
".session-card.session-connected {\n"
"    border-left: 4px solid @success_green;\n"
"}\n"
"\n"
".session-card.session-connecting {\n"
"    border-left: 4px solid @warning_orange;\n"
"}\n"
"\n"
".session-card.session-paused {\n"
"    border-left: 4px solid @pause_purple;\n"
"}\n"
"\n"
".session-card.session-auth-required {\n"
"    border-left: 4px solid @warning_orange;\n"
"}\n"
"\n"
".session-card.session-error {\n"
"    border-left: 4px solid @danger_red;\n"
"}\n"
"\n"
".config-card {\n"
"    background-color: @bg_primary;\n"
"    border-radius: 8px;\n"
"    border: 1px solid @border_light;\n"
"    padding: 12px;\n"
"}\n"
"\n"
"/* Statistics card styling */\n"
".vpn-stat-card {\n"
"    background-color: @bg_primary;\n"
"    border: 1px solid @border_light;\n"
"    border-radius: 12px;\n"
"    padding: 15px;\n"
"    margin: 10px;\n"
"    box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1);\n"
"}\n"
"\n"
".card-title {\n"
"    font-weight: bold;\n"
"    font-size: 14px;\n"
"    color: @text_primary;\n"
"}\n"
"\n"
".card-protocol {\n"
"    font-size: 11px;\n"
"    color: @text_secondary;\n"
"    font-weight: 500;\n"
"}\n"
"\n"
".card-bandwidth {\n"
"    font-family: monospace;\n"
"    font-size: 12px;\n"
"    color: @text_primary;\n"
"}\n"
"\n"
".card-stats-label {\n"
"    font-family: monospace;\n"
"    font-size: 10px;\n"
"    color: @text_secondary;\n"
"}\n"
"\n"
".card-graph-area {\n"
"    background: rgba(0, 0, 0, 0.02);\n"
"    border-radius: 6px;\n"
"    min-height: 80px;\n"
"}\n"
"\n"
"/* More Info grid styling */\n"
".more-info-grid {\n"
"    background: rgba(0, 0, 0, 0.03);\n"
"    border-top: 1px solid @border_light;\n"
"    border-radius: 0 0 12px 12px;\n"
"}\n"
"\n"
".info-label {\n"
"    color: @text_secondary;\n"
"    font-size: 9px;\n"
"    font-weight: 700;\n"
"    text-transform: uppercase;\n"
"    letter-spacing: 0.5px;\n"
"}\n"
"\n"
".info-value {\n"
"    font-family: monospace;\n"
"    font-size: 11px;\n"
"    color: @text_primary;\n"
"    font-weight: 500;\n"
"}\n";

static const char *CSS_DARK_MODE =
"@define-color primary_blue #0A84FF;\n"
"@define-color success_green #30D158;\n"
"@define-color warning_orange #FF9F0A;\n"
"@define-color danger_red #FF453A;\n"
"@define-color pause_purple #BF5AF2;\n"
"\n"
"@define-color bg_primary rgba(30, 30, 30, 0.92);\n"
"@define-color bg_secondary rgba(44, 44, 44, 0.85);\n"
"@define-color bg_hover rgba(10, 132, 255, 0.12);\n"
"@define-color bg_active rgba(10, 132, 255, 0.18);\n"
"\n"
"@define-color text_primary #FFFFFF;\n"
"@define-color text_secondary #98989D;\n"
"@define-color text_tertiary #48484A;\n"
"\n"
"@define-color border_light rgba(255, 255, 255, 0.08);\n"
"@define-color separator rgba(255, 255, 255, 0.06);\n"
"\n"
"/* Menu styling */\n"
"menu {\n"
"    background-color: @bg_primary;\n"
"    border-radius: 8px;\n"
"    border: 1px solid @border_light;\n"
"    padding: 4px;\n"
"}\n"
"\n"
"menuitem {\n"
"    border-radius: 4px;\n"
"    padding: 6px 12px;\n"
"    margin: 2px 0;\n"
"    color: @text_primary;\n"
"    font-size: 13px;\n"
"}\n"
"\n"
"menuitem:hover {\n"
"    background-color: @bg_hover;\n"
"    transition: background-color 100ms ease-out;\n"
"}\n"
"\n"
"menuitem:active {\n"
"    background-color: @bg_active;\n"
"}\n"
"\n"
"menuitem:disabled {\n"
"    color: @text_tertiary;\n"
"}\n"
"\n"
"separator {\n"
"    background-color: @separator;\n"
"    min-height: 1px;\n"
"    margin: 4px 8px;\n"
"}\n"
"\n"
"/* Custom widget classes */\n"
".session-connected {\n"
"    color: @success_green;\n"
"    font-weight: 600;\n"
"}\n"
"\n"
".session-connecting {\n"
"    color: @warning_orange;\n"
"    font-weight: 500;\n"
"}\n"
"\n"
".session-paused {\n"
"    color: @pause_purple;\n"
"    font-weight: 500;\n"
"}\n"
"\n"
".session-error {\n"
"    color: @danger_red;\n"
"    font-weight: 500;\n"
"}\n"
"\n"
".session-auth-required {\n"
"    color: @warning_orange;\n"
"    font-weight: 500;\n"
"}\n"
"\n"
".config-item {\n"
"    color: @text_primary;\n"
"}\n"
"\n"
".config-item:disabled {\n"
"    color: @text_tertiary;\n"
"}\n"
"\n"
".section-header {\n"
"    color: @text_secondary;\n"
"    font-size: 11px;\n"
"    font-weight: 600;\n"
"}\n"
"\n"
".metadata {\n"
"    color: @text_secondary;\n"
"    font-size: 11px;\n"
"}\n"
"\n"
"/* Dashboard window styling */\n"
".dashboard-window {\n"
"    background-color: @bg_secondary;\n"
"}\n"
"\n"
".session-card {\n"
"    background-color: @bg_primary;\n"
"    border-radius: 12px;\n"
"    border: 1px solid @border_light;\n"
"    padding: 16px;\n"
"}\n"
"\n"
".session-card.session-connected {\n"
"    border-left: 4px solid @success_green;\n"
"}\n"
"\n"
".session-card.session-connecting {\n"
"    border-left: 4px solid @warning_orange;\n"
"}\n"
"\n"
".session-card.session-paused {\n"
"    border-left: 4px solid @pause_purple;\n"
"}\n"
"\n"
".session-card.session-auth-required {\n"
"    border-left: 4px solid @warning_orange;\n"
"}\n"
"\n"
".session-card.session-error {\n"
"    border-left: 4px solid @danger_red;\n"
"}\n"
"\n"
".config-card {\n"
"    background-color: @bg_primary;\n"
"    border-radius: 8px;\n"
"    border: 1px solid @border_light;\n"
"    padding: 12px;\n"
"}\n"
"\n"
"/* Statistics card styling */\n"
".vpn-stat-card {\n"
"    background-color: @bg_primary;\n"
"    border: 1px solid @border_light;\n"
"    border-radius: 12px;\n"
"    padding: 15px;\n"
"    margin: 10px;\n"
"    box-shadow: 0 4px 6px rgba(0, 0, 0, 0.3);\n"
"}\n"
"\n"
".card-title {\n"
"    font-weight: bold;\n"
"    font-size: 14px;\n"
"    color: @text_primary;\n"
"}\n"
"\n"
".card-protocol {\n"
"    font-size: 11px;\n"
"    color: @text_secondary;\n"
"    font-weight: 500;\n"
"}\n"
"\n"
".card-bandwidth {\n"
"    font-family: monospace;\n"
"    font-size: 12px;\n"
"    color: @text_primary;\n"
"}\n"
"\n"
".card-stats-label {\n"
"    font-family: monospace;\n"
"    font-size: 10px;\n"
"    color: @text_secondary;\n"
"}\n"
"\n"
".card-graph-area {\n"
"    background: rgba(255, 255, 255, 0.03);\n"
"    border-radius: 6px;\n"
"    min-height: 80px;\n"
"}\n"
"\n"
"/* More Info grid styling */\n"
".more-info-grid {\n"
"    background: rgba(255, 255, 255, 0.04);\n"
"    border-top: 1px solid @border_light;\n"
"    border-radius: 0 0 12px 12px;\n"
"}\n"
"\n"
".info-label {\n"
"    color: @text_secondary;\n"
"    font-size: 9px;\n"
"    font-weight: 700;\n"
"    text-transform: uppercase;\n"
"    letter-spacing: 0.5px;\n"
"}\n"
"\n"
".info-value {\n"
"    font-family: monospace;\n"
"    font-size: 11px;\n"
"    color: @text_primary;\n"
"    font-weight: 500;\n"
"}\n";

/* Global state */
static GtkCssProvider *css_provider = NULL;
static ThemeMode current_mode = THEME_MODE_LIGHT;
static GtkSettings *gtk_settings = NULL;
static GPtrArray *callbacks = NULL;

/* Callback data structure */
typedef struct {
    ThemeChangeCallback callback;
    gpointer user_data;
} CallbackData;

/**
 * Detect if system is using dark theme
 */
static gboolean detect_dark_theme(void) {
    if (!gtk_settings) {
        gtk_settings = gtk_settings_get_default();
    }

    gchar *theme_name = NULL;
    g_object_get(gtk_settings, "gtk-theme-name", &theme_name, NULL);

    gboolean is_dark = FALSE;
    if (theme_name) {
        /* Check for common dark theme indicators */
        is_dark = (strstr(theme_name, "dark") != NULL ||
                  strstr(theme_name, "Dark") != NULL ||
                  strstr(theme_name, "DARK") != NULL);
        g_free(theme_name);
    }

    /* Also check gtk-application-prefer-dark-theme setting */
    gboolean prefer_dark = FALSE;
    g_object_get(gtk_settings, "gtk-application-prefer-dark-theme", &prefer_dark, NULL);

    return is_dark || prefer_dark;
}

/**
 * Load CSS for current theme mode
 */
static void load_theme_css(void) {
    const char *css_content = (current_mode == THEME_MODE_DARK) ? CSS_DARK_MODE : CSS_LIGHT_MODE;

    GError *error = NULL;
    gtk_css_provider_load_from_data(css_provider, css_content, -1, &error);

    if (error) {
        logger_error("Failed to load CSS: %s", error->message);
        g_error_free(error);
    } else {
        logger_info("Loaded CSS for %s mode",
               current_mode == THEME_MODE_DARK ? "dark" : "light");
    }
}

/**
 * Notify all registered callbacks of theme change
 */
static void notify_theme_changed(void) {
    if (!callbacks) {
        return;
    }

    for (guint i = 0; i < callbacks->len; i++) {
        CallbackData *data = g_ptr_array_index(callbacks, i);
        if (data && data->callback) {
            data->callback(current_mode, data->user_data);
        }
    }
}

/**
 * Handle GTK theme changes
 */
static void on_theme_changed(GObject *object, GParamSpec *pspec, gpointer user_data) {
    (void)object;
    (void)pspec;
    (void)user_data;

    ThemeMode old_mode = current_mode;
    current_mode = detect_dark_theme() ? THEME_MODE_DARK : THEME_MODE_LIGHT;

    if (old_mode != current_mode) {
        logger_info("Theme changed: %s -> %s",
               old_mode == THEME_MODE_DARK ? "dark" : "light",
               current_mode == THEME_MODE_DARK ? "dark" : "light");

        load_theme_css();
        notify_theme_changed();
    }
}

/**
 * Initialize theme system
 */
int theme_init(void) {
    /* Initialize GTK if not already done */
    if (!gtk_init_check(NULL, NULL)) {
        logger_error("Failed to initialize GTK for theme system");
        return -1;
    }

    /* Get GTK settings */
    gtk_settings = gtk_settings_get_default();
    if (!gtk_settings) {
        logger_error("Failed to get GTK settings");
        return -1;
    }

    /* Detect initial theme mode */
    current_mode = detect_dark_theme() ? THEME_MODE_DARK : THEME_MODE_LIGHT;
    logger_info("Initial theme mode: %s", current_mode == THEME_MODE_DARK ? "dark" : "light");

    /* Create CSS provider */
    css_provider = gtk_css_provider_new();
    if (!css_provider) {
        logger_error("Failed to create CSS provider");
        return -1;
    }

    /* Load initial CSS */
    load_theme_css();

    /* Apply CSS to all screens */
    GdkScreen *screen = gdk_screen_get_default();
    if (screen) {
        gtk_style_context_add_provider_for_screen(
            screen,
            GTK_STYLE_PROVIDER(css_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
        );
    }

    /* Monitor theme changes */
    g_signal_connect(gtk_settings, "notify::gtk-theme-name",
                    G_CALLBACK(on_theme_changed), NULL);
    g_signal_connect(gtk_settings, "notify::gtk-application-prefer-dark-theme",
                    G_CALLBACK(on_theme_changed), NULL);

    /* Initialize callbacks array */
    callbacks = g_ptr_array_new();

    logger_info("Theme system initialized successfully");
    return 0;
}

/**
 * Get current theme mode
 */
ThemeMode theme_get_current_mode(void) {
    return current_mode;
}

/**
 * Register callback for theme changes
 */
void theme_register_callback(ThemeChangeCallback callback, gpointer user_data) {
    if (!callbacks) {
        callbacks = g_ptr_array_new();
    }

    CallbackData *data = g_malloc(sizeof(CallbackData));
    data->callback = callback;
    data->user_data = user_data;

    g_ptr_array_add(callbacks, data);
}

/**
 * Reload CSS for current theme mode
 */
void theme_reload_css(void) {
    load_theme_css();
}

/**
 * Cleanup theme system
 */
void theme_cleanup(void) {
    /* Disconnect signals */
    if (gtk_settings) {
        g_signal_handlers_disconnect_by_func(gtk_settings,
                                            G_CALLBACK(on_theme_changed),
                                            NULL);
    }

    /* Free callbacks */
    if (callbacks) {
        for (guint i = 0; i < callbacks->len; i++) {
            CallbackData *data = g_ptr_array_index(callbacks, i);
            g_free(data);
        }
        g_ptr_array_free(callbacks, TRUE);
        callbacks = NULL;
    }

    /* Cleanup CSS provider */
    if (css_provider) {
        g_object_unref(css_provider);
        css_provider = NULL;
    }

    logger_info("Theme system cleaned up");
}

/**
 * Get color value for current theme
 */
const char* theme_get_color(const char *color_name) {
    /* Simple color lookup - returns CSS color variable names */
    static char color_var[64];
    snprintf(color_var, sizeof(color_var), "@%s", color_name);
    return color_var;
}
