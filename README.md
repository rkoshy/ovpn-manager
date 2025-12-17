# OpenVPN3 Manager

Professional GTK3-based VPN client for managing OpenVPN3 connections on Linux. Designed for power users with comprehensive monitoring, server selection, and security features.

## Features

### ✅ Implemented
- **Multi-tab Dashboard**: Connections, Statistics, Servers, Routing (placeholder), Security (placeholder)
- **VPN Connection Management**: Connect/disconnect/pause/resume sessions via tray menu and dashboard
- **Real-time Bandwidth Monitoring**: Live upload/download speeds with smooth spline graphs
- **Connection Statistics**: Packet counts, errors, dropped packets, total transfer
- **Server Selection**: Browse available configs with latency testing (ping)
- **OAuth Support**: Automatic browser-based authentication flow
- **Theme Support**: Light/dark mode with CSS styling
- **System Tray Integration**: Quick access menu with session status
- **Multi-session Support**: Manage multiple concurrent VPN connections

### 🔄 Planned (Phases 4-5)
- **Split Tunneling**: Per-application and per-domain routing bypass
- **Kill Switch**: Block all non-VPN traffic with iptables
- **DNS Leak Protection**: Force DNS through VPN tunnel
- **IPv6 Leak Protection**: Block or tunnel IPv6 traffic
- **Auto-reconnect**: Automatic reconnection on unexpected disconnects
- **Desktop Notifications**: Connection status notifications
- **Connection Logs**: Capture and display OpenVPN3 logs

## Current Status

**Phase 1 - Foundation** (✅ COMPLETE)
- ✅ Git repository initialized
- ✅ Meson build system configured
- ✅ Vendor libraries (cJSON)
- ✅ D-Bus manager (sd-bus with GLib integration)
- ✅ Main loop with signal handlers and cleanup
- ✅ System tray icon (libayatana-appindicator)
- ✅ Theme system (light/dark mode CSS)

**Phase 2 - Statistics & Visualization** (✅ COMPLETE)
- ✅ Dashboard window with tabbed interface (GtkNotebook)
- ✅ Bandwidth monitor backend (D-Bus + sysfs fallback)
- ✅ Statistics tab with live graphs (Cairo rendering)
- ✅ Smooth spline graphs (Catmull-Rom to Bezier conversion)
- ✅ Auto-scaling Y-axis with grid lines
- ✅ Packet statistics display
- ✅ Connection details (protocol, cipher, IP addresses)
- ✅ Export statistics to CSV
- ✅ Reset counters functionality

**Phase 3 - Server Selection** (✅ COMPLETE)
- ✅ Servers tab with configuration list
- ✅ Async ping utility for latency testing
- ✅ Server status indicators (connected, latency, load)
- ✅ One-click connect/disconnect
- ✅ Configuration import support

**Phase 4 - Split Tunneling** (⏳ PENDING)
- ⏳ Per-application routing (cgroups + iptables)
- ⏳ Per-domain routing (DNS-based)
- ⏳ Rule persistence (JSON storage)
- ⏳ Routing tab UI

**Phase 5 - Security Features** (⏳ PENDING)
- ⏳ Kill switch (iptables-based)
- ⏳ DNS leak protection
- ⏳ IPv6 leak protection
- ⏳ Leak testing utilities
- ⏳ Security tab UI
- ⏳ PolicyKit integration for root access

**Binary Size:** ~100 KB stripped
**Memory Usage:** ~15-20 MB (with dashboard and monitoring active)

## Build Dependencies

### Required
```bash
sudo apt-get install -y \
  build-essential \
  pkg-config \
  libglib2.0-dev \
  libsystemd-dev \
  libgtk-3-dev \
  libayatana-appindicator3-dev \
  libcairo2-dev \
  openvpn3

# Install meson and ninja (if not already installed)
pip3 install --user meson ninja
```

## Build Instructions

```bash
# Add meson/ninja to PATH (if installed via pip)
export PATH="$HOME/.local/bin:$PATH"

# Configure the build
meson setup builddir

# Compile
ninja -C builddir

# Run (requires a desktop environment with system tray)
./builddir/src/ovpn-manager
```

**Note:** The application requires:
- Desktop environment with system tray support
- OpenVPN3 installed and configured
- D-Bus session bus

## CI/CD Workflow

This project uses GitHub Actions to automatically build for multiple Debian versions:
- **Debian 12 (Bookworm)**: GTK 3.24.38, GLib 2.74.6
- **Debian 13 (Trixie)**: GTK 3.24.43, GLib 2.78.0

**Workflow Jobs:**
1. **Build**: Compile release binaries (automatic)
2. **Test**: Run smoke tests (automatic)
3. **Package**: Create .deb packages (manual trigger)

**Artifacts Available:**
- Stripped release binaries (~100 KB)
- Debug builds with symbols (~500 KB, manual trigger)
- Installable .deb packages (manual trigger)

See [CI-README.md](CI-README.md) for detailed CI documentation, including:
- Why we use Meson instead of CMake
- How GTK version detection works
- Local testing with Docker
- Debugging CI failures

## Project Structure

```
ovpn-tool/
├── meson.build                    # Build system root
├── src/
│   ├── main.c                     # Entry point, GLib main loop, timers, cleanup
│   ├── tray.c/h                   # System tray icon (libayatana-appindicator)
│   ├── dbus/
│   │   ├── dbus_manager.c/h       # D-Bus connection (sd-bus + GLib)
│   │   ├── session_client.c/h     # VPN session operations
│   │   ├── config_client.c/h      # Configuration management
│   │   └── signal_handlers.c/h    # D-Bus signal monitoring
│   ├── ui/
│   │   ├── dashboard.c/h          # Main dashboard window (5 tabs)
│   │   ├── theme.c/h              # Light/dark theme system
│   │   ├── widgets.c/h            # Custom GTK widgets
│   │   ├── icons.c/h              # Icon management
│   │   └── servers_tab.c/h        # Server selection tab
│   ├── monitoring/
│   │   ├── bandwidth_monitor.c/h  # Real-time bandwidth tracking
│   │   └── ping_util.c/h          # Async latency testing
│   └── storage/
│       ├── config_schema.h        # Data structures
│       └── config_storage.c/h     # JSON persistence (unused, uses OpenVPN3 configs)
├── vendor/
│   └── cJSON.c/h                  # JSON parser
└── data/
    ├── ovpn-manager.desktop       # Autostart entry
    └── style-light.css            # Light theme CSS
    └── style-dark.css             # Dark theme CSS
```

## Usage

### System Tray Menu
- Click tray icon to see active sessions and available configurations
- Right-click session name for quick disconnect/pause/resume
- Click configuration name to connect
- "Show Dashboard" opens detailed monitoring window

### Dashboard Window
- **Connections Tab**: Manage active sessions and configurations
- **Statistics Tab**: Real-time bandwidth graphs, packet statistics, connection details
- **Servers Tab**: Browse and test server latency, one-click connect
- **Routing Tab**: (Planned) Configure split tunneling
- **Security Tab**: (Planned) Kill switch and leak protection

### Keyboard Shortcuts
- **Ctrl+Q**: Quit application (from dashboard)

## Performance

- **CPU Usage**: <2% idle, <5% active (graph rendering)
- **Memory**: ~15-20 MB with dashboard open
- **Network Overhead**: Minimal (1s stats polling, 5s session refresh)

## Technical Stack

- **Language**: C99
- **GUI Framework**: GTK3
- **Graphics**: Cairo (for bandwidth graphs)
- **System Tray**: libayatana-appindicator3
- **D-Bus**: sd-bus (systemd)
- **Event Loop**: GLib
- **JSON**: cJSON
- **Build System**: Meson + Ninja

## Architecture

### Event-Driven Design
- **50ms timer**: GTK tray menu event processing (20 FPS)
- **1s timer**: Connection timer labels update (efficient, no rebuild)
- **2s timer**: Dashboard statistics refresh
- **5s timer**: Session state polling (detect new/removed sessions)

### Bandwidth Monitoring
- Primary: OpenVPN3 D-Bus statistics API
- Fallback: sysfs (`/sys/class/net/tun*/statistics/*`)
- 60-second rolling buffer for graph rendering
- Smooth spline interpolation (Catmull-Rom)

### Server Latency Testing
- Async ping using `GIOChannel` and `g_spawn_async_with_pipes()`
- Non-blocking, callback-based result handling
- Proper Source ID lifecycle management

## Troubleshooting

### Application won't start
- Ensure OpenVPN3 is installed: `openvpn3 --version`
- Check D-Bus session: `dbus-daemon --version`
- Verify GTK3 available: `pkg-config --modversion gtk+-3.0`

### Tray icon not visible
- Some desktop environments (GNOME 40+) hide tray icons by default
- Install extension: `sudo apt install gnome-shell-extension-appindicator`
- Or use a tray-compatible DE (XFCE, KDE, MATE)

### Statistics not updating
- Verify VPN is actually connected: `openvpn3 sessions-list`
- Check permissions: statistics require read access to `/sys/class/net/tun*/statistics/`

### GLib-CRITICAL warnings
- These have been fixed in the current version
- If you see Source ID warnings, please report as bug

## Development

### Code Quality
- No compiler warnings (except informational format-truncation)
- No GLib-CRITICAL warnings during runtime
- Proper resource cleanup (no memory leaks detected with valgrind)
- Consistent error handling

### Testing
```bash
# Build and run with timeout to check for errors
timeout 10 ./builddir/src/ovpn-manager > test.log 2>&1
grep -i "critical\|error\|warning" test.log
```

## Future Enhancements

See [PLANS.md](PLANS.md) for detailed implementation plans.

### Phase 4 - Split Tunneling (Week 4-5)
- cgroups-based per-application routing
- iptables rules for selective bypass
- Domain-based routing with DNS interception

### Phase 5 - Security (Week 5-6)
- Kill switch with iptables traffic blocking
- DNS leak prevention (NAT rules)
- IPv6 leak prevention (disable or tunnel)
- PolicyKit integration for privilege escalation

### Post-v1.0 Ideas
- Connection profiles (save/load complete settings)
- Historical bandwidth graphs (daily/weekly)
- Auto-connect to fastest server
- Desktop notifications for events
- Export/import settings backup
- Scripting API (D-Bus control interface)

## License

MIT License - Copyright (c) 2025 Renny Koshy

See [LICENSE](LICENSE) file for details.

## Contributing

Contributions welcome! Please:
1. Follow existing code style (K&R, 4-space indent)
2. Ensure no new compiler warnings
3. Test with both light and dark themes
4. Verify no GLib errors during runtime
5. Update documentation for new features

## Credits

- Uses [cJSON](https://github.com/DaveGamble/cJSON) for JSON parsing
- Icons from system icon theme (freedesktop.org standard)
- Inspired by commercial VPN clients but focused on power user needs
