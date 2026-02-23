FROM debian:trixie

# Install build dependencies
RUN apt-get update -qq && \
    apt-get install -y -qq \
      build-essential \
      pkg-config \
      python3 \
      python3-pip \
      ninja-build \
      libglib2.0-dev \
      libsystemd-dev \
      libgtk-3-dev \
      libayatana-appindicator3-dev \
      libcairo2-dev \
      git && \
    pip3 install --quiet --break-system-packages meson && \
    rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /workspace

# Verify installations
RUN pkg-config --modversion gtk+-3.0 && \
    pkg-config --modversion glib-2.0 && \
    meson --version && \
    ninja --version

# Add labels
LABEL org.opencontainers.image.description="Build environment for ovpn-manager (Debian 13)"
LABEL org.opencontainers.image.source="https://github.com/YOUR_USERNAME/ovpn-tool"
