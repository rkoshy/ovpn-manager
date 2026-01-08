#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <glib.h>
#include <gio/gio.h>
#include "dbus/dbus_manager.h"
#include "tray.h"
#include "ui/theme.h"
#include "ui/dashboard.h"
#include "utils/logger.h"

/* Application ID for single-instance support */
#define APP_ID "com.github.rennykoshy.ovpntool"

/* Global variables */
GMainLoop *main_loop = NULL;  /* Non-static so tray.c can access it */
Dashboard *dashboard = NULL;  /* Non-static so tray.c can access it */
GApplication *app = NULL;     /* Non-static so tray.c can access it */
static DbusManager *dbus_manager = NULL;
static TrayIcon *tray_icon = NULL;
static guint tray_timer_id = 0;
static guint session_timer_id = 0;
static guint timer_update_id = 0;
static guint dashboard_timer_id = 0;
static gboolean app_held = FALSE;  /* Track if g_application_hold was called */

/* Command-line options */
static gchar *log_level_str = NULL;
static gint verbosity = 0;

/* Command-line option entries */
static GOptionEntry option_entries[] = {
    { "log-level", 'l', 0, G_OPTION_ARG_STRING, &log_level_str,
      "Set log level (debug, info, warn, error). Default: warn", "LEVEL" },
    { "verbose", 'v', 0, G_OPTION_ARG_INT, &verbosity,
      "Set verbosity level (0=quiet, 1=changes only, 2=detailed, 3=debug). Default: 0", "LEVEL" },
    { NULL }
};

/**
 * Parse log level string to LogLevel enum
 */
static LogLevel parse_log_level(const char *level_str) {
    if (!level_str) {
        return LOG_LEVEL_WARN;  /* Default */
    }

    if (g_ascii_strcasecmp(level_str, "debug") == 0) {
        return LOG_LEVEL_DEBUG;
    } else if (g_ascii_strcasecmp(level_str, "info") == 0) {
        return LOG_LEVEL_INFO;
    } else if (g_ascii_strcasecmp(level_str, "warn") == 0 ||
               g_ascii_strcasecmp(level_str, "warning") == 0) {
        return LOG_LEVEL_WARN;
    } else if (g_ascii_strcasecmp(level_str, "error") == 0) {
        return LOG_LEVEL_ERROR;
    } else {
        fprintf(stderr, "Invalid log level '%s'. Using default 'warn'.\n", level_str);
        fprintf(stderr, "Valid levels: debug, info, warn, error\n");
        return LOG_LEVEL_WARN;
    }
}

/**
 * Command-line handler
 */
static gint on_command_line(GApplication *application,
                            GApplicationCommandLine *cmdline,
                            gpointer user_data) {
    (void)user_data;

    /* Get the option context */
    GVariantDict *options = g_application_command_line_get_options_dict(cmdline);

    /* Extract log-level option if provided */
    const gchar *level = NULL;
    if (g_variant_dict_lookup(options, "log-level", "&s", &level)) {
        g_free(log_level_str);
        log_level_str = g_strdup(level);
    }

    /* Extract verbosity option if provided */
    gint verb = 0;
    if (g_variant_dict_lookup(options, "verbose", "i", &verb)) {
        verbosity = verb;
    }

    /* Activate the application (which will initialize everything) */
    g_application_activate(application);

    return 0;  /* Success */
}

/**
 * GTK/Tray event processing callback
 */
static gboolean tray_update_callback(gpointer user_data) {
    TrayIcon *tray = (TrayIcon *)user_data;
    if (tray) {
        tray_icon_run(tray);
    }
    return TRUE;  /* Continue calling */
}

/**
 * Session update callback - checks for session changes every 5 seconds
 * (only rebuilds menu if sessions actually changed)
 */
static gboolean session_update_callback(gpointer user_data) {
    (void)user_data;

    if (tray_icon && dbus_manager) {
        sd_bus *bus = dbus_manager_get_bus(dbus_manager);
        if (bus) {
            tray_icon_update_sessions(tray_icon, bus);
        }
    }

    return TRUE;  /* Continue calling */
}

/**
 * Timer update callback - updates timer labels every 1 second
 * (efficient label updates, no menu rebuild)
 */
static gboolean timer_update_callback(gpointer user_data) {
    (void)user_data;

    if (tray_icon && dbus_manager) {
        sd_bus *bus = dbus_manager_get_bus(dbus_manager);
        if (bus) {
            tray_icon_update_timers(tray_icon, bus);
        }
    }

    return TRUE;  /* Continue calling */
}

/**
 * Dashboard update callback - updates dashboard data every 2 seconds
 */
static gboolean dashboard_update_callback(gpointer user_data) {
    (void)user_data;

    if (dashboard && dbus_manager) {
        sd_bus *bus = dbus_manager_get_bus(dbus_manager);
        if (bus) {
            dashboard_update(dashboard, bus);
        }
    }

    return TRUE;  /* Continue calling */
}

/**
 * Signal handler for SIGINT and SIGTERM
 */
static void signal_handler(int signum) {
    const char *signal_name = (signum == SIGINT) ? "SIGINT" : "SIGTERM";
    logger_info("Received %s, shutting down gracefully...", signal_name);

    if (app) {
        g_application_quit(app);
    }
}

/**
 * Setup signal handlers for graceful shutdown
 */
static void setup_signal_handlers(void) {
    struct sigaction sa;

    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        logger_error("Failed to setup SIGINT handler");
    }

    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        logger_error("Failed to setup SIGTERM handler");
    }
}

/**
 * Cleanup function called on exit
 */
static void cleanup(void) {
    logger_info("Cleaning up resources...");

    /* Remove dashboard update timer */
    if (dashboard_timer_id > 0) {
        g_source_remove(dashboard_timer_id);
        dashboard_timer_id = 0;
    }

    /* Remove timer update timer */
    if (timer_update_id > 0) {
        g_source_remove(timer_update_id);
        timer_update_id = 0;
    }

    /* Remove session update timer */
    if (session_timer_id > 0) {
        g_source_remove(session_timer_id);
        session_timer_id = 0;
    }

    /* Remove tray update timer */
    if (tray_timer_id > 0) {
        g_source_remove(tray_timer_id);
        tray_timer_id = 0;
    }

    /* Cleanup dashboard */
    if (dashboard) {
        dashboard_destroy(dashboard);
        dashboard = NULL;
    }

    /* Cleanup tray icon */
    if (tray_icon) {
        tray_icon_cleanup(tray_icon);
        tray_icon = NULL;
    }

    /* Cleanup theme system */
    theme_cleanup();

    /* Cleanup D-Bus manager */
    if (dbus_manager) {
        dbus_manager_cleanup(dbus_manager);
        dbus_manager = NULL;
    }

    /* Cleanup main loop */
    if (main_loop) {
        g_main_loop_unref(main_loop);
        main_loop = NULL;
    }

    logger_info("Cleanup complete");

    /* Cleanup logger system (must be last) */
    logger_cleanup();
}

/**
 * Application activation callback
 * Called when app is launched or when a second instance tries to launch
 */
static void on_app_activate(GApplication *application, gpointer user_data) {
    (void)user_data;

    /* If already initialized, just show the dashboard */
    if (dashboard) {
        logger_info("Application already running - showing dashboard");
        dashboard_show(dashboard);
        return;
    }

    /* Setup signal handlers */
    setup_signal_handlers();

    /* Initialize logger system (must be early) */
    LogLevel log_level = parse_log_level(log_level_str);
    /* Enable file logging for debugging (logs to ~/.local/share/ovpn-manager/app.log) */
    logger_init(true, NULL, log_level, true);
    logger_set_verbosity(verbosity);

    logger_info("=== OpenVPN3 Manager Starting ===");
    logger_info("Log level: %d, Verbosity: %d", log_level, verbosity);

    /* Print banner to terminal (keep as printf for direct user output) */
    printf("OpenVPN3 Manager v0.1.0\n");
    printf("========================\n");
    printf("Logs: ~/.local/share/ovpn-manager/app.log\n\n");

    /* Initialize theme system */
    logger_info("Initializing theme system...");
    if (theme_init() < 0) {
        logger_error("Failed to initialize theme system");
        g_application_quit(application);
        return;
    }

    /* Initialize D-Bus manager */
    logger_info("Initializing D-Bus manager...");
    dbus_manager = dbus_manager_init();
    if (!dbus_manager) {
        logger_error("Failed to initialize D-Bus manager");
        g_application_quit(application);
        return;
    }

    /* Check if OpenVPN3 is available (non-fatal warning) */
    logger_info("Checking for OpenVPN3 services...");
    if (!dbus_manager_check_openvpn3(dbus_manager)) {
        logger_warn("OpenVPN3 services not available. Some features may not work.");
        logger_warn("Install openvpn3-linux if you need VPN functionality.");
    }

    /* Initialize dashboard window */
    logger_info("Initializing dashboard window...");
    dashboard = dashboard_create();
    if (!dashboard) {
        logger_error("Failed to initialize dashboard window");
        g_application_quit(application);
        return;
    }

    /* Initialize system tray icon */
    logger_info("Initializing system tray icon...");
    tray_icon = tray_icon_init("OpenVPN3 Manager");
    if (!tray_icon) {
        logger_error("Failed to initialize system tray icon");
        g_application_quit(application);
        return;
    }

    /* Add timer to process GTK events (50ms = 20 times per second) */
    tray_timer_id = g_timeout_add(50, tray_update_callback, tray_icon);

    /* Update session list initially */
    if (dbus_manager_check_openvpn3(dbus_manager)) {
        sd_bus *bus = dbus_manager_get_bus(dbus_manager);
        if (bus) {
            logger_info("Loading active VPN sessions...");
            tray_icon_update_sessions(tray_icon, bus);
        }
    }

    /* Add timer to check for session changes every 5 seconds */
    session_timer_id = g_timeout_add_seconds(5, session_update_callback, NULL);

    /* Add timer to update timer labels every 1 second (efficient, no rebuild) */
    timer_update_id = g_timeout_add_seconds(1, timer_update_callback, NULL);

    /* Add timer to update dashboard every 2 seconds */
    dashboard_timer_id = g_timeout_add_seconds(2, dashboard_update_callback, NULL);

    /* Hold the application - prevent it from exiting (we use tray icon, not windows) */
    g_application_hold(application);
    app_held = TRUE;

    /* Print user instructions to terminal (keep as printf for direct user output) */
    printf("OpenVPN3 Manager started successfully\n");
    printf("System tray icon should be visible\n");
    printf("Press Ctrl+C or use tray menu to quit\n\n");
}

/**
 * Application shutdown callback
 */
static void on_app_shutdown(GApplication *application, gpointer user_data) {
    (void)user_data;

    logger_info("Application shutting down");

    /* Release the application hold only if it was acquired */
    if (app_held) {
        g_application_release(application);
        app_held = FALSE;
    }

    /* Cleanup resources */
    cleanup();
}

/**
 * Main entry point
 */
int main(int argc, char *argv[]) {
    int status;

    /* Create GApplication for single-instance support */
    app = g_application_new(APP_ID, G_APPLICATION_HANDLES_COMMAND_LINE);
    if (!app) {
        /* Logger not initialized yet, use fprintf */
        fprintf(stderr, "Failed to create GApplication\n");
        return EXIT_FAILURE;
    }

    /* Add command-line options */
    g_application_add_main_option_entries(G_APPLICATION(app), option_entries);

    /* Connect signals */
    g_signal_connect(app, "command-line", G_CALLBACK(on_command_line), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_app_shutdown), NULL);

    /* Run the application */
    status = g_application_run(app, argc, argv);

    /* Cleanup GApplication */
    g_object_unref(app);

    return status;
}
