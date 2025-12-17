# GitHub Actions CI/CD Setup Documentation

## Overview

This project uses GitHub Actions to build OpenVPN3 Manager for multiple Debian versions with automatic GTK version detection via Meson's dependency system.

## Why Meson Instead of CMake?

### TL;DR
**Meson is the standard build system for GTK3/GNOME projects**, and it provides cleaner, simpler syntax for pkg-config dependencies like GTK.

### Detailed Rationale

1. **GTK/GNOME Ecosystem Standard**
   - GTK3 itself uses Meson
   - GLib uses Meson
   - GNOME projects migrated to Meson
   - Using the same build system as our dependencies ensures better compatibility

2. **Cleaner pkg-config Integration**
   ```meson
   # Meson - one line, automatic version check
   gtk_dep = dependency('gtk+-3.0', version: '>= 3.22')
   ```

   vs.

   ```cmake
   # CMake - more verbose
   find_package(PkgConfig REQUIRED)
   pkg_check_modules(GTK REQUIRED gtk+-3.0>=3.22)
   target_include_directories(target PRIVATE ${GTK_INCLUDE_DIRS})
   target_link_libraries(target ${GTK_LIBRARIES})
   ```

3. **Faster Builds**
   - Always uses Ninja backend (parallel by default)
   - CMake typically uses Make unless explicitly configured

4. **Simpler Syntax**
   - Python-like, easier to read and maintain
   - Less boilerplate for common tasks

5. **Better Dependency Detection**
   - Automatic version checking
   - Feature detection (e.g., X11 backend availability)
   - Fallback mechanisms (see appindicator example in meson.build)

6. **Modern Design**
   - Built from scratch for speed and simplicity
   - No legacy baggage from Make era

### Trade-offs

**What We're Giving Up:**
- Wider IDE support (CLion, Visual Studio prefer CMake)
- More mature ecosystem (more Stack Overflow answers)
- Better Windows support (though we're Linux-only)

**Why It's Worth It:**
- We're a Linux GTK3 project (Meson's sweet spot)
- Simple build requirements (no exotic features needed)
- Most GTK developers know Meson
- Can switch to CMake later if needed (unlikely)

---

## CI Workflow Architecture

### Jobs

1. **Build** - Compile binaries for each Debian version
2. **Test** - Run smoke tests on compiled binaries
3. **Package** - (Optional) Create .deb packages

### Supported Distributions

| Distro | Image | GTK Version | GLib Version | Status |
|--------|-------|-------------|--------------|--------|
| Debian 12 (Bookworm) | `debian:12` | 3.24.38 | 2.74.6 | ✅ Tested |
| Debian 13 (Trixie) | `debian:trixie` | 3.24.43 | 2.78.0 | ✅ Tested |

Both versions exceed our minimum requirements:
- GTK >= 3.22 ✅
- GLib >= 2.58 ✅

### How GTK Version Detection Works

**Meson Automatically Detects GTK Version:**

```meson
# From meson.build
gtk_dep = dependency('gtk+-3.0', version: '>= 3.22')
```

**Behind the Scenes:**
1. Meson calls `pkg-config --modversion gtk+-3.0`
2. Compares version against requirement (`>= 3.22`)
3. If version insufficient → build fails with clear error
4. If version sufficient → proceeds with build

**During CI Workflow:**
```bash
# Our workflow logs show:
$ pkg-config --modversion gtk+-3.0
3.24.38  # Debian 12

$ meson setup builddir --buildtype=release
Dependency gtk+-3.0 found: YES 3.24.38 (cached)
```

**No Manual Configuration Needed!** Meson handles everything.

### Build Variants

#### 1. Release Builds (Default)
- **Trigger**: Automatic on every commit
- **Optimizations**: `-O2`, function sections, strip symbols
- **Binary Size**: ~100 KB
- **Purpose**: Production-ready binaries

```bash
meson setup builddir --buildtype=release
ninja -C builddir
strip builddir/src/ovpn-manager
```

#### 2. Debug Builds (Manual)
- **Trigger**: Manual pipeline trigger
- **Optimizations**: `-g`, no stripping, debug symbols
- **Binary Size**: ~500 KB
- **Purpose**: Debugging with GDB/valgrind

```bash
meson setup builddir --buildtype=debug
ninja -C builddir
# No stripping
```

### Artifacts

**Release Builds:**
- Binary: `builddir/src/ovpn-manager` (stripped)
- Logs: `builddir/meson-logs/` (build details)
- Retention: 1 week

**Test Results:**
- Output: `test-output.log` (runtime logs)
- Retention: 1 week

**Debug Builds:**
- Binary: `builddir/src/ovpn-manager` (with symbols)
- Retention: 3 days (larger size)

**.deb Packages:**
- Package: `ovpn-manager_0.1.0-<commit>_debian<ver>_amd64.deb`
- Retention: 1 month

---

## CI Jobs Breakdown

### `build-debian12`
**Purpose**: Build release binary for Debian 12

**Steps:**
1. Install build dependencies
2. Verify GTK/GLib versions
3. Configure with Meson (release mode)
4. Build with Ninja
5. Strip binary
6. Save artifacts

**Expected Output:**
```
===== Building for Debian 12 (Bookworm) =====
Verifying GTK version...
3.24.38
Configuring build with Meson...
Build targets in project: 1
Build complete!
-rwxr-xr-x 1 root root 102K ovpn-manager
```

### `test-debian12`
**Purpose**: Validate Debian 12 binary

**Tests:**
1. ✅ Binary exists and is executable
2. ✅ ELF format correct (`file` check)
3. ✅ Dynamic dependencies present (`ldd` check)
4. ✅ Binary size reasonable (`size` check)
5. ✅ Required symbols present (`nm` check)
6. ✅ No crashes on startup (3-second timeout test)
7. ✅ No GLib-CRITICAL warnings in output

**Expected Output:**
```
Checking binary type...
builddir/src/ovpn-manager: ELF 64-bit LSB executable, x86-64

Checking dynamic dependencies...
libgtk-3.so.0 => /lib/x86_64-linux-gnu/libgtk-3.so.0
libglib-2.0.so.0 => /lib/x86_64-linux-gnu/libglib-2.0.so.0
...

Test suite passed!
```

### `build-debian13`
**Purpose**: Same as `build-debian12` but for Debian 13/Trixie

**Differences:**
- Uses newer GTK (3.24.43 vs 3.24.38)
- Uses newer GLib (2.78.0 vs 2.74.6)
- May have different appindicator versions

### `test-debian13`
**Purpose**: Same tests as `test-debian12` but for Debian 13

### `build-debian12-debug` (Manual)
**Purpose**: Create debug build for Debian 12

**Differences from Release:**
- No optimization (`-O0` vs `-O2`)
- Debug symbols included (`-g`)
- No stripping
- ~5x larger binary size

**Use Cases:**
- Debugging with GDB
- Valgrind memory leak analysis
- Profiling with perf/gprof

### `build-debian13-debug` (Manual)
**Purpose**: Same as `build-debian12-debug` but for Debian 13

### `package-debian12` (Manual)
**Purpose**: Create installable .deb package for Debian 12

**Package Contents:**
```
/usr/bin/ovpn-manager              # Binary
/etc/xdg/autostart/ovpn-manager.desktop  # Autostart entry
```

**Package Metadata:**
- Name: `ovpn-manager`
- Version: `0.1.0-<git-commit-sha>`
- Dependencies: `libglib2.0-0, libgtk-3-0, libsystemd0, libayatana-appindicator3-1, libcairo2, openvpn3`

**Install:**
```bash
sudo dpkg -i ovpn-manager_0.1.0-abc1234_debian12_amd64.deb
sudo apt-get install -f  # Fix any missing dependencies
```

### `package-debian13` (Manual)
**Purpose**: Same as `package-debian12` but for Debian 13

---

## Local Testing

### Replicate CI Build Locally

**Debian 12:**
```bash
docker run -it --rm -v $(pwd):/workspace -w /workspace debian:12 bash

# Inside container:
apt-get update
apt-get install -y build-essential pkg-config python3 python3-pip ninja-build \
  libglib2.0-dev libsystemd-dev libgtk-3-dev libayatana-appindicator3-dev libcairo2-dev
pip3 install --break-system-packages meson

meson setup builddir --buildtype=release
ninja -C builddir
strip builddir/src/ovpn-manager
ls -lh builddir/src/ovpn-manager
```

**Debian 13:**
```bash
docker run -it --rm -v $(pwd):/workspace -w /workspace debian:trixie bash
# (same commands as Debian 12)
```

### Run Smoke Test
```bash
# Test for critical errors
timeout 3 builddir/src/ovpn-manager 2>&1 | tee test.log
grep -i "critical\|segfault\|abort" test.log && echo "FAIL" || echo "PASS"
```

---

## Debugging CI Failures

### Build Fails: "Dependency gtk+-3.0 not found"

**Cause**: GTK3 development package not installed

**Fix**: Check `.github/workflows/build.yml` install dependencies step includes:
```yaml
- libgtk-3-dev
```

### Build Fails: "Meson version X.Y.Z too old"

**Cause**: Debian's meson package is outdated

**Fix**: Install via pip3 (already in our CI):
```yaml
- pip3 install --break-system-packages meson
```

### Test Fails: "GLib-CRITICAL" in output

**Cause**: Runtime error (should not happen, we fixed these!)

**Fix**: Check `test-output.log` artifact, find source of warning, fix in code

### Test Fails: "Segmentation fault"

**Cause**: Serious bug in code

**Fix**:
1. Download debug build artifact
2. Run with GDB: `gdb builddir/src/ovpn-manager`
3. Get backtrace: `bt`
4. Fix the bug

### Package Fails: "dpkg-deb: error: control file"

**Cause**: Invalid DEBIAN/control syntax

**Fix**: Check `package-debian12` job, ensure control file is valid

---

## GTK Version Compatibility Matrix

| Feature | Min GTK | Debian 12 | Debian 13 | Status |
|---------|---------|-----------|-----------|--------|
| GtkNotebook (tabs) | 3.0 | ✅ 3.24.38 | ✅ 3.24.43 | OK |
| GtkDrawingArea (graphs) | 3.0 | ✅ 3.24.38 | ✅ 3.24.43 | OK |
| Cairo rendering | 3.0 | ✅ 3.24.38 | ✅ 3.24.43 | OK |
| GtkListBox (configs) | 3.10 | ✅ 3.24.38 | ✅ 3.24.43 | OK |
| CSS theming | 3.20 | ✅ 3.24.38 | ✅ 3.24.43 | OK |

**Conclusion**: All features work on both Debian versions.

---

## Dependency Version Summary

### Debian 12 (Bookworm)
```
GTK3:         3.24.38
GLib:         2.74.6
libsystemd:   252
Cairo:        1.16.0
appindicator: 0.5.91
```

### Debian 13 (Trixie)
```
GTK3:         3.24.43
GLib:         2.78.0
libsystemd:   256
Cairo:        1.18.0
appindicator: 0.5.93
```

**Our Requirements (from meson.build):**
```
GTK3:         >= 3.22  ✅
GLib:         >= 2.58  ✅
libsystemd:   >= 239   ✅
appindicator: any      ✅
```

---

## Performance Benchmarks

**Build Times (approximate):**
- Debian 12: ~45 seconds (clean build)
- Debian 13: ~45 seconds (clean build)
- Debian 12: ~5 seconds (incremental)
- Debian 13: ~5 seconds (incremental)

**Binary Sizes:**
- Release (stripped): ~100 KB
- Debug (symbols): ~500 KB

**CI Workflow Total Time:**
- Build + Test: ~3-4 minutes per distro
- Both distros run in parallel

---

## Future CI Enhancements

### Planned
- [ ] Add Ubuntu 22.04 LTS build
- [ ] Add Ubuntu 24.04 LTS build
- [ ] Add Fedora 39/40 builds (RPM packages)
- [ ] Add valgrind memory leak tests
- [ ] Add cppcheck static analysis
- [ ] Add code coverage reporting
- [ ] Add automated release tagging

### Maybe Later
- [ ] Cross-compile for ARM (Raspberry Pi)
- [ ] Cross-compile for ARM64 (modern RPi, servers)
- [ ] AppImage builds (universal Linux binary)
- [ ] Flatpak builds

---

## FAQ

**Q: Why not use official Meson Docker images?**
A: We need full Debian environment with GTK packages. Official Meson images are minimal.

**Q: Why install meson via pip instead of apt?**
A: Debian's meson package is often outdated. pip3 ensures latest version.

**Q: Why `--break-system-packages` flag?**
A: Debian 12+ restricts pip installations outside venv. This flag allows it (safe in container).

**Q: Can I use the .deb on Ubuntu?**
A: Maybe. Ubuntu is Debian-based, but GTK versions may differ. Build for Ubuntu separately.

**Q: Why no Windows/macOS builds?**
A: Project is Linux-only (uses systemd, Linux-specific APIs). Cross-platform is Phase 6+ (see PLANS.md).

**Q: How do I trigger manual jobs?**
A: GitHub Actions → Workflow runs → Run workflow button (for debug builds and packages)

---

## Meson vs CMake Quick Reference

### Dependency Declaration
```meson
# Meson
gtk_dep = dependency('gtk+-3.0', version: '>= 3.22')
```
```cmake
# CMake
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK REQUIRED gtk+-3.0>=3.22)
```

### Executable Definition
```meson
# Meson
executable('ovpn-manager',
  sources: src_files,
  dependencies: [gtk_dep, glib_dep]
)
```
```cmake
# CMake
add_executable(ovpn-manager ${SRC_FILES})
target_link_libraries(ovpn-manager ${GTK_LIBRARIES} ${GLIB_LIBRARIES})
target_include_directories(ovpn-manager PRIVATE ${GTK_INCLUDE_DIRS})
```

### Compiler Flags
```meson
# Meson
add_project_arguments(
  cc.get_supported_arguments(['-D_GNU_SOURCE']),
  language: 'c'
)
```
```cmake
# CMake
add_definitions(-D_GNU_SOURCE)
```

### Configuration
```bash
# Meson
meson setup builddir --buildtype=release

# CMake
cmake -S . -B builddir -DCMAKE_BUILD_TYPE=Release
```

### Building
```bash
# Meson
ninja -C builddir

# CMake
cmake --build builddir
```

**Winner for GTK projects: Meson** (cleaner syntax, better pkg-config integration)

---

*Last Updated*: 2025-12-17
*CI Version*: 1.0
