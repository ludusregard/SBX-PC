#!/usr/bin/env bash
# Build the Linux sbx binary with the native Vulkan renderer:
#   ./build_vulkan.sh [preset]   (default linux-amd64-relwithdebinfo)
#
# Runs on Linux or in WSL. On Windows, build_vulkan.bat runs this in WSL
# after the Windows build, against the same checkout (generated/ is shared;
# codegen only rewrites files whose content changed).
#
# Output: out/build/<preset>-vulkan/sbx. Configure runs every time.
# Logged to out/build_vulkan_linux.log (overwritten each run). On a failure
# the last lines and every "error" line are shown.
#
# First time: ./setup_linux.sh installs the compiler and libraries
# (Ubuntu 24.04).
set -u
cd "$(dirname "$0")"

PRESET="${1:-linux-amd64-relwithdebinfo}"
B="out/build/${PRESET}-vulkan"
mkdir -p out
LOG="$PWD/out/build_vulkan_linux.log"
echo "build_vulkan.sh ${PRESET} - $(date)" > "$LOG"

failed() {
    echo
    echo "===== FAILED. Last 40 lines of ${LOG}:"
    tail -n 40 "$LOG"
    echo
    echo "===== Lines mentioning errors:"
    grep -n -i -e "error" -e "FAILED:" "$LOG" | tail -n 60
    echo
    echo "Full log: ${LOG}"
    exit 1
}

say() {
    echo "$1"
    echo "$1" >> "$LOG"
}

if [ ! -f generated/default/sources.cmake ]; then
    say "No generated code yet: run codegen first (codegen.bat on Windows)."
    failed
fi

# A Windows checkout seen from WSL is owned by another user, and git refuses
# to work in it. The build calls git (SDK patches, version), so allow it for
# this run only, without touching any git config.
export GIT_CONFIG_COUNT=1
export GIT_CONFIG_KEY_0=safe.directory
export GIT_CONFIG_VALUE_0='*'

# Compiler: the newest clang 20 or later. Ubuntu installs them as
# clang-<version>; a plain clang is used if it's new enough. The presets ask
# for "clang", so wrappers named clang and clang++ in out/linux-clang/ go
# first on PATH. CMake keeps the wrappers' path, which stays the same when
# the compiler behind them changes (a changed compiler path makes CMake
# drop the preset's settings).
CC_BIN=""
CXX_BIN=""
for v in 24 23 22 21 20; do
    if command -v "clang++-$v" > /dev/null && command -v "clang-$v" > /dev/null; then
        CC_BIN="$(command -v "clang-$v")"
        CXX_BIN="$(command -v "clang++-$v")"
        break
    fi
done
if [ -z "$CXX_BIN" ] && command -v clang++ > /dev/null && command -v clang > /dev/null; then
    major="$(clang++ -dumpversion | cut -d. -f1)"
    if [ "$major" -ge 20 ]; then
        CC_BIN="$(command -v clang)"
        CXX_BIN="$(command -v clang++)"
    fi
fi
if [ -z "$CXX_BIN" ]; then
    say "clang 20 or later not found: run ./setup_linux.sh first."
    failed
fi
WRAP="$PWD/out/linux-clang"
mkdir -p "$WRAP"
write_wrapper() {
    local text
    text="$(printf '#!/bin/sh\nexec "%s" "$@"' "$2")"
    if [ "$(cat "$WRAP/$1" 2> /dev/null)" != "$text" ]; then
        printf '%s\n' "$text" > "$WRAP/$1"
        chmod +x "$WRAP/$1"
    fi
}
write_wrapper clang "$CC_BIN"
write_wrapper clang++ "$CXX_BIN"
export PATH="$WRAP:$PATH"
for tool in cmake ninja; do
    if ! command -v "$tool" > /dev/null; then
        say "$tool not found: run ./setup_linux.sh first."
        failed
    fi
done

say "=== Configuring (${PRESET}, ${B}, $(basename "$CXX_BIN"))"
cmake --preset "$PRESET" -B "$B" >> "$LOG" 2>&1 || failed

say "=== Building (the first build compiles the SDK and generated code: slow)"
cmake --build "$B" >> "$LOG" 2>&1 || failed
if [ ! -x "$B/sbx" ]; then
    say "The build reported success but $B/sbx is missing."
    failed
fi
echo
echo "Built $B/sbx  -  run it with run_vulkan.sh"
exit 0
