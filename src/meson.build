# Vendor sources (cJSON)
vendor_sources = files(
  '../vendor/cJSON.c',
)

# Utility sources
util_sources = files(
  'utils/logger.c',
  'utils/file_chooser.c',
)
# Will add: string_utils.c, validation.c

# Storage sources
storage_sources = files(
  'storage/config_storage.c',
)

# D-Bus sources
dbus_sources = files(
  'dbus/dbus_manager.c',
  'dbus/session_client.c',
  'dbus/config_client.c',
  'dbus/signal_handlers.c',
)
# Will add: log_client.c, netcfg_client.c

# UI sources
ui_sources = files(
  'ui/theme.c',
  'ui/widgets.c',
  'ui/dashboard.c',
  'ui/servers_tab.c',
)

# Monitoring sources
monitoring_sources = files(
  'monitoring/bandwidth_monitor.c',
  'monitoring/ping_util.c',
)

# Feature sources
feature_sources = []
# Will add: dns_leak_monitor.c, notifications.c, log_viewer.c, auto_reconnect.c

# OAuth sources
oauth_sources = []
# Will add: oauth_handler.c, http_listener.c

# Tray sources
tray_sources = files(
  'tray.c',
)

# Main source
main_sources = files(
  'main.c',
)

# Combine all sources
all_sources = [
  vendor_sources,
  util_sources,
  storage_sources,
  dbus_sources,
  ui_sources,
  monitoring_sources,
  feature_sources,
  oauth_sources,
  tray_sources,
  main_sources,
]

# Build executable
executable(
  'ovpn-manager',
  sources: all_sources,
  include_directories: inc,
  dependencies: deps,
  install: true,
  install_dir: get_option('bindir')
)
