#!/usr/bin/env bash
# Launch the Linux build of the recompiled Shutoko Battle X (build_vulkan.sh),
# on the native Vulkan renderer.
#
#   ./run_vulkan.sh [options]                  e.g. ./run_vulkan.sh --log_level=debug
#   SBX_PRESET=linux-amd64-debug ./run_vulkan.sh   another build
#
# Settings live in sbx.toml next to sbx (the F4 menu's Save button writes
# it). The first run makes one with the Windows launcher's defaults.
# Options given here win over the file for that run only.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
preset="${SBX_PRESET:-linux-amd64-relwithdebinfo}"
exe="$here/out/build/${preset}-vulkan/sbx"
if [ ! -x "$exe" ]; then
    echo "$exe not found: run build_vulkan.sh first"
    exit 1
fi

export REX_GAME_DATA_ROOT="$(cd "$here/../assets" && pwd)"

settings="$(dirname "$exe")/sbx.toml"
if [ ! -f "$settings" ]; then
    cat > "$settings" <<'EOF'
# Shutoko Battle X settings, loaded at every start.
# Written by the Save button in the F4 menu; safe to edit by hand while the
# game is closed. One setting per line: name = value (strings in quotes).
# Launch options (--name=value) and REX_<NAME> variables win over this file.

# Made by run_vulkan.sh from the launcher defaults
fullscreen = true
mnk_mode = true
skip_cutscenes = true
new_music_roll = 1
EOF
fi

exec "$exe" "$@"
