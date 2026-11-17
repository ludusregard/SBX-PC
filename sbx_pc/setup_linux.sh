#!/usr/bin/env bash
# Install what the Linux build needs (Ubuntu 24.04, native or WSL). Run once:
#   ./setup_linux.sh
#
# clang 20 (the SDK's Linux CI compiler) and lld, CMake, Ninja, and the
# headers SDL and the SDK look for: X11 and Wayland for the window, ALSA,
# PulseAudio and PipeWire for audio. Vulkan itself comes from the SDK's
# third-party copy and is loaded at run time.
set -eu

if [ -r /etc/os-release ]; then
    . /etc/os-release
    if [ "${ID:-}" != "ubuntu" ] || [ "${VERSION_ID:-}" != "24.04" ]; then
        echo "Written for Ubuntu 24.04; this is ${PRETTY_NAME:-unknown}."
        echo "Package names may differ. Continuing anyway."
    fi
fi

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    SUDO="sudo"
fi

$SUDO apt-get update
$SUDO apt-get install -y \
    clang-20 lld-20 cmake ninja-build git pkg-config build-essential python3 \
    libx11-dev libx11-xcb-dev libxext-dev libxrandr-dev libxcursor-dev \
    libxi-dev libxfixes-dev libxss-dev libxtst-dev libxkbcommon-dev \
    libwayland-dev libwayland-bin wayland-protocols libdecor-0-dev \
    libasound2-dev libpulse-dev libpipewire-0.3-dev

echo
echo "Done. Build with ./build_vulkan.sh"
