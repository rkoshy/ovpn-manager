#!/bin/bash
# build.sh - Build ovpn-manager binaries and .deb packages for Debian 12 & 13
# Uses shared Docker images from scorussolutions/gtk3-deb-builder-d{12,13}
# with fallback to local Dockerfiles in .github/docker/
#
# Usage:
#   ./build.sh              # Build for both Debian 12 and 13
#   ./build.sh debian12     # Build only for Debian 12
#   ./build.sh debian13     # Build only for Debian 13
#   ./build.sh native       # Build natively (no Docker, current OS only)
#
# Output is placed in dist/

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

APP_NAME="ovpn-manager"
VERSION=$(grep "version:" meson.build | head -1 | sed "s/.*'\([0-9.]*\)'.*/\1/")
ARCH="amd64"
DIST_DIR="$SCRIPT_DIR/dist"

# Shared Docker images (preferred)
SHARED_IMAGE_D12="scorussolutions/gtk3-deb-builder-d12:latest"
SHARED_IMAGE_D13="scorussolutions/gtk3-deb-builder-d13:latest"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No color

log()  { echo -e "${CYAN}[build]${NC} $*"; }
ok()   { echo -e "${GREEN}[  ok ]${NC} $*"; }
warn() { echo -e "${YELLOW}[warn ]${NC} $*"; }
err()  { echo -e "${RED}[error]${NC} $*" >&2; }

# ---------------------------------------------------------------------------
# Resolve Docker image - prefer shared, fallback to local Dockerfile
# ---------------------------------------------------------------------------
resolve_image() {
    local debian_ver="$1"
    local shared_image

    case "$debian_ver" in
        12) shared_image="$SHARED_IMAGE_D12" ;;
        13) shared_image="$SHARED_IMAGE_D13" ;;
        *)  err "Unknown Debian version: $debian_ver"; exit 1 ;;
    esac

    # Check if shared image is available locally
    if docker image inspect "$shared_image" >/dev/null 2>&1; then
        ok "Using shared image: $shared_image" >&2
        echo "$shared_image"
        return
    fi

    # Try pulling from Docker Hub
    log "Pulling shared image: $shared_image" >&2
    if docker pull "$shared_image" >/dev/null 2>&1; then
        ok "Pulled shared image: $shared_image" >&2
        echo "$shared_image"
        return
    fi

    # Fallback: build from local Dockerfile
    local local_dockerfile=".github/docker/debian${debian_ver}-build.Dockerfile"
    if [ ! -f "$local_dockerfile" ]; then
        err "No shared image and no local Dockerfile: $local_dockerfile"
        exit 1
    fi

    local fallback_tag="${APP_NAME}-build:debian${debian_ver}"
    warn "Shared image unavailable, building from $local_dockerfile" >&2
    docker build -q -t "$fallback_tag" -f "$local_dockerfile" . >/dev/null 2>&1
    ok "Fallback image ready: $fallback_tag" >&2
    echo "$fallback_tag"
}

# ---------------------------------------------------------------------------
# Native build (no Docker)
# ---------------------------------------------------------------------------
build_native() {
    log "Native build on $(. /etc/os-release && echo "$PRETTY_NAME")"

    export PATH="/home/renny/.local/bin:/opt/pyenv/bin:$PATH"
    if command -v pyenv >/dev/null 2>&1; then
        eval "$(pyenv init -)"
    fi

    if ! command -v meson >/dev/null 2>&1; then
        err "meson not found in PATH"
        exit 1
    fi

    # Configure and build
    meson setup builddir --buildtype=release --prefix=/usr --wipe 2>&1 | tail -5
    meson compile -C builddir 2>&1 | tail -10
    strip builddir/src/$APP_NAME

    local bin_size
    bin_size=$(stat --printf="%s" builddir/src/$APP_NAME)
    ok "Binary built: builddir/src/$APP_NAME ($(numfmt --to=iec $bin_size))"
}

# ---------------------------------------------------------------------------
# Docker-based build for a specific Debian version
# ---------------------------------------------------------------------------
build_in_docker() {
    local debian_ver="$1"     # "12" or "13"
    local deb_name="${APP_NAME}_${VERSION}_debian${debian_ver}_${ARCH}"

    log "Building for Debian ${debian_ver}"

    # Resolve which image to use
    local image_tag
    image_tag=$(resolve_image "$debian_ver")

    # Run build inside container
    log "Compiling in container..."
    docker run --rm \
        -v "$SCRIPT_DIR:/workspace:ro" \
        -v "$DIST_DIR:/output" \
        -w /build \
        "$image_tag" \
        -c "
            set -e

            # Copy source (read-only mount, so we work in /build)
            cp -a /workspace/. /build/

            # Configure and build (--wipe in case stale builddir was copied)
            rm -rf builddir
            meson setup builddir --buildtype=release --prefix=/usr 2>&1 | tail -3
            ninja -C builddir 2>&1
            strip builddir/src/${APP_NAME}
            echo \"Binary size: \$(ls -lh builddir/src/${APP_NAME} | awk '{print \$5}')\"

            # -- Assemble .deb package --
            PKG_DIR=/tmp/${deb_name}
            mkdir -p \$PKG_DIR/DEBIAN
            mkdir -p \$PKG_DIR/usr/bin
            mkdir -p \$PKG_DIR/usr/share/applications
            mkdir -p \$PKG_DIR/usr/share/icons/hicolor/scalable/apps
            mkdir -p \$PKG_DIR/etc/xdg/autostart

            cp builddir/src/${APP_NAME}                                  \$PKG_DIR/usr/bin/
            install -m 644 data/ovpn-manager.desktop                     \$PKG_DIR/usr/share/applications/
            install -m 644 data/ovpn-manager.desktop                     \$PKG_DIR/etc/xdg/autostart/
            install -m 644 data/icons/hicolor/scalable/apps/ovpn-manager.svg \$PKG_DIR/usr/share/icons/hicolor/scalable/apps/

            # Control file
            cat > \$PKG_DIR/DEBIAN/control <<EOF
Package: ${APP_NAME}
Version: ${VERSION}
Section: net
Priority: optional
Architecture: ${ARCH}
Depends: libglib2.0-0, libgtk-3-0, libsystemd0, libayatana-appindicator3-1, libcairo2, openvpn3
Maintainer: Renny Koshy <renny@koshy.org>
Homepage: https://github.com/rennykoshy/ovpn-manager
Description: Professional GTK3-based VPN client for OpenVPN3
 Lightweight, event-driven VPN manager with real-time monitoring,
 server selection, and advanced features for power users.
EOF

            # postinst
            cat > \$PKG_DIR/DEBIAN/postinst <<'POSTINST'
#!/bin/sh
set -e
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t /usr/share/icons/hicolor 2>/dev/null || true
fi
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database /usr/share/applications 2>/dev/null || true
fi
POSTINST
            chmod 755 \$PKG_DIR/DEBIAN/postinst

            # postrm
            cat > \$PKG_DIR/DEBIAN/postrm <<'POSTRM'
#!/bin/sh
set -e
if [ \"\$1\" = \"remove\" ] || [ \"\$1\" = \"purge\" ]; then
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -f -t /usr/share/icons/hicolor 2>/dev/null || true
    fi
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database /usr/share/applications 2>/dev/null || true
    fi
fi
POSTRM
            chmod 755 \$PKG_DIR/DEBIAN/postrm

            # Build the .deb
            dpkg-deb --build \$PKG_DIR /output/${deb_name}.deb 2>&1

            # Also copy the standalone binary
            cp builddir/src/${APP_NAME} /output/${APP_NAME}-${VERSION}-debian${debian_ver}-linux-${ARCH}

            # Generate checksums
            cd /output
            sha256sum ${deb_name}.deb > ${deb_name}.deb.sha256
            sha256sum ${APP_NAME}-${VERSION}-debian${debian_ver}-linux-${ARCH} > ${APP_NAME}-${VERSION}-debian${debian_ver}-linux-${ARCH}.sha256
        "

    ok "Debian ${debian_ver}: ${deb_name}.deb"
    ok "Debian ${debian_ver}: ${APP_NAME}-${VERSION}-debian${debian_ver}-linux-${ARCH}"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
TARGET="${1:-all}"

# Verify Docker is available for non-native builds
if [ "$TARGET" != "native" ]; then
    if ! command -v docker >/dev/null 2>&1; then
        err "Docker is required for cross-building. Use './build.sh native' for a local build."
        exit 1
    fi
fi

mkdir -p "$DIST_DIR"

log "${APP_NAME} v${VERSION} - build started"
echo ""

case "$TARGET" in
    debian12)
        build_in_docker 12
        ;;
    debian13)
        build_in_docker 13
        ;;
    native)
        build_native
        ;;
    all)
        build_in_docker 12
        echo ""
        build_in_docker 13

        # Combined checksums file
        (cd "$DIST_DIR" && cat *.sha256 > SHA256SUMS.txt)
        ;;
    *)
        err "Unknown target: $TARGET"
        echo "Usage: $0 [debian12|debian13|native|all]"
        exit 1
        ;;
esac

echo ""
log "Build output:"
ls -lh "$DIST_DIR"/*.deb "$DIST_DIR"/*-linux-* 2>/dev/null | while read -r line; do
    echo "  $line"
done
echo ""
ok "Done."
