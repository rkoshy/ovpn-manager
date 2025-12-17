#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <glib.h>
#include <gio/gio.h>
#include "dbus/dbus_manager.h"
#include "tray.h"
#include "ui/theme.h"
#include "ui/dashboard.h"

/* Application ID for single-instance support */
#define APP_ID "com.github.rennykoshy.ovpntool"

/* Global variables */
GMainLoop *main_loop = NULL;  /* Non-static so tray.c can access it */
Dashboard *dashboard = NULL;  /* Non-static so tray.c can access it */
static GApplication *app = NULL;
static DbusManager *dbus_manager = NULL;
static TrayIcon *tray_icon = NULL;
static guint tray_timer_id = 0;
static guint session_timer_id = 0;
static guint timer_update_id = 0;
static guint dashboard_timer_id = 0;

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
    printf("\nReceived %s, shutting down gracefully...\n", signal_name);

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
        fprintf(stderr, "Failed to setup SIGINT handler\n");
    }

    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        fprintf(stderr, "Failed to setup SIGTERM handler\n");
    }
}

/**
 * Cleanup function called on exit
 */
static void cleanup(void) {
    printf("Cleaning up resources...\n");

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

    printf("Cleanup complete\n");
}

/**
 * Application activation callback
 * Called when app is launched or when a second instance tries to launch
 */
static void on_app_activate(GApplication *application, gpointer user_data) {
    (void)user_data;

    /* If already initialized, just show the dashboard */
    if (dashboard) {
        printf("Application already running - showing dashboard\n");
        dashboard_show(dashboard);
        return;
    }

    printf("OpenVPN3 Manager v0.1.0\n");
    printf("========================\n\n");

    /* Setup signal handlers */
    setup_signal_handlers();

    /* Initialize theme system */
    printf("Initializing theme system...\n");
    if (theme_init() < 0) {
        fprintf(stderr, "Failed to initialize theme system\n");
        g_application_quit(application);
        return;
    }

    /* Initialize D-Bus manager */
    printf("Initializing D-Bus manager...\n");
    dbus_manager = dbus_manager_init();
    if (!dbus_manager) {
        fprintf(stderr, "Failed to initialize D-Bus manager\n");
        g_application_quit(application);
        return;
    }

    /* Check if OpenVPN3 is available (non-fatal warning) */
    printf("Checking for OpenVPN3 services...\n");
    if (!dbus_manager_check_openvpn3(dbus_manager)) {
        fprintf(stderr, "WARNING: OpenVPN3 services not available. Some features may not work.\n");
        fprintf(stderr, "Install openvpn3-linux if you need VPN functionality.\n\n");
    }

    /* Initialize dashboard window */
    printf("Initializing dashboard window...\n");
    dashboard = dashboard_create();
    if (!dashboard) {
        fprintf(stderr, "Failed to initialize dashboard window\n");
        g_application_quit(application);
        return;
    }

    /* Initialize system tray icon */
    printf("Initializing system tray icon...\n");
    tray_icon = tray_icon_init("OpenVPN3 Manager");
    if (!tray_icon) {
        fprintf(stderr, "Failed to initialize system tray icon\n");
        g_application_quit(application);
        return;
    }

    /* Add timer to process GTK events (50ms = 20 times per second) */
    tray_timer_id = g_timeout_add(50, tray_update_callback, tray_icon);

    /* Update session list initially */
    if (dbus_manager_check_openvpn3(dbus_manager)) {
        sd_bus *bus = dbus_manager_get_bus(dbus_manager);
        if (bus) {
            printf("Loading active VPN sessions...\n");
            tray_icon_update_sessions(tray_icon, bus);
        }
    }

    /* Add timer to check for session changes every 5 seconds */
    session_timer_id = g_timeout_add_seconds(5, session_update_callback, NULL);

    /* Add timer to update timer labels every 1 second (efficient, no rebuild) */
    timer_update_id = g_timeout_add_seconds(1, timer_update_callback, NULL);

    /* Add timer to update dashboard every 2 seconds */
    dashboard_timer_id = g_timeout_add_seconds(2, dashboard_update_callback, NULL);

    printf("OpenVPN3 Manager started successfully\n");
    printf("System tray icon should be visible\n");
    printf("Press Ctrl+C or use tray menu to quit\n\n");
}

/**
 * Application shutdown callback
 */
static void on_app_shutdown(GApplication *application, gpointer user_data) {
    (void)application;
    (void)user_data;

    printf("\nApplication shutting down\n");

    /* Cleanup resources */
    cleanup();
}

/**
 * Main entry point
 */
int main(int argc, char *argv[]) {
    int status;

    /* Create GApplication for single-instance support */
    app = g_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    if (!app) {
        fprintf(stderr, "Failed to create GApplication\n");
        return EXIT_FAILURE;
    }

    /* Connect signals */
    g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_app_shutdown), NULL);

    /* Run the application */
    status = g_application_run(app, argc, argv);

    /* Cleanup GApplication */
    g_object_unref(app);

    return status;
}
