#!/usr/bin/env bash
set -euo pipefail

runtime_dir=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
wayland_display=${WAYLAND_DISPLAY:-}

if [[ -z $wayland_display ]]; then
    for candidate in "$runtime_dir"/wayland-*; do
        [[ -S $candidate ]] || continue
        wayland_display=${candidate##*/}
        break
    done
fi

[[ -n $wayland_display ]] || {
    printf 'uu-wayland-run: no Wayland display socket found in %s\n' "$runtime_dir" >&2
    exit 1
}

export XDG_RUNTIME_DIR=$runtime_dir
export WAYLAND_DISPLAY=$wayland_display
export XDG_SESSION_TYPE=wayland
export DBUS_SESSION_BUS_ADDRESS=${DBUS_SESSION_BUS_ADDRESS:-unix:path=$runtime_dir/bus}

exec "$@"
