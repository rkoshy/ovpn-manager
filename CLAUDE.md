# OVPN-Manager Project Guide

## Project Overview
OpenVPN3 Manager is a professional, lightweight GTK3-based VPN client for Linux designed for power users. It provides comprehensive VPN connection management with advanced monitoring and security features.

## Technical Stack
- **Language**: C99 (modern C standard)
- **Build System**: Meson + Ninja
- **GUI Framework**: GTK3
- **Graphics**: Cairo
- **System Integration**:
  - D-Bus (systemd sd-bus) for OpenVPN3 communication
  - libayatana-appindicator3 for system tray
- **Event Loop**: GLib
- **JSON Parsing**: cJSON

## Project Structure
```
ovpn-manager/
├── src/
│   ├── main.c               # Entry point and GLib main loop
│   ├── dbus/                # D-Bus interaction with OpenVPN3
│   │   ├── config_client.c  # Configuration management
│   │   ├── session_client.c # Session management
│   │   ├── signal_handlers.c # D-Bus signal handling
│   │   └── dbus_manager.c   # D-Bus connection management
│   ├── ui/                  # User interface components
│   │   ├── dashboard.c      # Multi-tab dashboard window
│   │   ├── servers_tab.c    # Server selection UI
│   │   ├── widgets.c        # Custom GTK widgets
│   │   └── theme.c          # Light/dark theme support
│   ├── tray.c               # System tray icon and menu
│   ├── monitoring/          # Network monitoring
│   │   ├── bandwidth_monitor.c # Real-time bandwidth tracking
│   │   └── ping_util.c      # Async server latency testing
│   ├── storage/             # Configuration persistence
│   │   └── config_storage.c # Config file management
│   └── utils/               # Utility functions
│       ├── logger.c/h       # Logging system
│       └── file_chooser.c/h # File selection dialogs
├── data/
│   ├── ovpn-manager.desktop # Desktop file for app launcher
│   ├── icons/               # Application icons
│   └── css/                 # GTK theme styles
├── vendor/                  # Third-party libraries (cJSON)
└── meson.build              # Build configuration
```

## Build System
### Dependencies
- GLib 2.0 (>= 2.58)
- GTK+ 3.0 (>= 3.22)
- libsystemd (>= 239)
- libayatana-appindicator3
- Cairo
- OpenVPN3 (runtime dependency)

### Build Instructions
```bash
# Configure build (creates builddir/)
meson setup builddir --wipe

# Compile
ninja -C builddir
# OR using the configured PATH
PATH=$HOME/.local/bin:$PATH meson compile -C builddir

# Run from build directory
./builddir/src/ovpn-manager

# Install (optional)
sudo ninja -C builddir install
```

### Debian Package Build
The project includes debian packaging:
```bash
# Build .deb package
./debian-package/build-deb.sh

# Install package
sudo dpkg -i ovpn-manager_*.deb
```

## Development Guidelines

### Code Style
- **Standard**: C99
- **Indentation**: 4 spaces (K&R style)
- **Naming**:
  - Functions: `module_action()` (e.g., `session_connect()`, `tray_icon_update()`)
  - Structs: PascalCase (e.g., `TrayIcon`, `VpnSession`)
  - Variables: snake_case
- **No compiler warnings**: Code must compile cleanly
- **Resource cleanup**: Always free allocated memory and unreference GObject resources

### Logging
Structured logging system in `src/utils/logger.c`:
```c
logger_info("Starting VPN session: %s", session_path);
logger_error("Failed to connect: %s", error_msg);
logger_debug("D-Bus signal received: %s", signal_name);
```

### Error Handling
- Return negative values on error (following systemd conventions)
- Use descriptive error messages
- Log errors with context
- Clean up resources on error paths

### GTK/GLib Patterns
- Use `g_malloc()` / `g_free()` for memory allocation
- Use `g_signal_connect()` for event handlers
- Store callback data with `g_object_set_data_full()` for automatic cleanup
- Always call `gtk_widget_show()` or `gtk_widget_show_all()` after creating widgets

## Key Components

### System Tray (tray.c)
- Creates app indicator with menu
- **Current Issue**: Menu is completely rebuilt on each refresh, causing UI state reset
- **TODO**: Refactor to use persistent menu structure with visibility toggles
- Manages VPN session actions (connect, disconnect, pause, resume)
- Auto-launches browser for OAuth authentication

### Dashboard (ui/dashboard.c)
- Multi-tab interface with:
  - Connections: Active session monitoring
  - Statistics: Real-time bandwidth graphs with Catmull-Rom spline smoothing
  - Servers: Server selection with latency testing
  - Routing: Route table visualization
  - Security: Security status monitoring

### D-Bus Integration (dbus/)
- Communicates with OpenVPN3 via D-Bus
- Session management: start, stop, pause, resume
- Configuration import and management
- Real-time signal handling for status updates

### Bandwidth Monitoring (monitoring/bandwidth_monitor.c)
- Fallback approach: D-Bus statistics → sysfs network counters
- Circular buffer for historical data
- Efficient delta calculation for rate computation

## Testing
### Manual Testing
- Test with light and dark GTK themes
- Verify no GLib runtime errors or warnings
- Test all session states: connecting, connected, paused, auth required, error
- Test menu interactions during refresh cycles

### Known Issues
- **Menu rebuild on refresh**: Tray menu is reconstructed on each cycle, resetting expansion state
- **Settings not implemented**: Settings menu item is disabled

## Release Process
Automated via GitHub Actions (`.github/workflows/release.yml`):
1. Tag version: `git tag v0.1.x`
2. Push tag: `git push origin v0.1.x`
3. GitHub Action builds .deb package
4. Release created with package attached

## Future Enhancements
- [ ] Fix tray menu persistence (in progress)
- [ ] Connection profiles with auto-connect
- [ ] Historical bandwidth graphs in dashboard
- [ ] Desktop notifications for connection events
- [ ] Settings dialog
- [ ] Scripting API for automation

## Performance Characteristics
- CPU Usage: <2% idle, <5% active
- Memory: ~15-20 MB
- Binary Size: ~100 KB (stripped)
- Refresh Rate: Tray updates every 2s, statistics every 1s

## Git Workflow
- **Main branch**: `main` (for open source project)
- **Commit messages**: Clear, descriptive (no AI attributions)
- **Before commits**: Always compile to ensure build succeeds

## License
MIT License

## Contributing
1. Fork the repository
2. Create a feature branch
3. Follow existing code style
4. Ensure no compiler warnings
5. Test with both light and dark themes
6. Submit pull request with clear description
