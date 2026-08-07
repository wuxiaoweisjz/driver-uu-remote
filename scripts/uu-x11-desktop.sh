#!/usr/bin/env bash
set -euo pipefail

display=${UU_X11_DISPLAY:-:99}
xauthority=${UU_X11_XAUTHORITY:-}

for _ in {1..100}; do
    if DISPLAY="$display" XAUTHORITY="$xauthority" xdpyinfo >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done
DISPLAY="$display" XAUTHORITY="$xauthority" xdpyinfo >/dev/null 2>&1 || {
    printf 'uu-x11-desktop: X server %s did not become ready\n' "$display" >&2
    exit 1
}

export DISPLAY="$display" XAUTHORITY="$xauthority" XDG_SESSION_TYPE=x11
unset WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS

if test -n "${UU_X11_DESKTOP_COMMAND:-}"; then
    exec dbus-run-session -- bash -lc "$UU_X11_DESKTOP_COMMAND"
fi

command -v kwin_x11 >/dev/null || {
    printf 'uu-x11-desktop: kwin_x11 is required or set UU_X11_DESKTOP_COMMAND\n' >&2
    exit 1
}
command -v plasmashell >/dev/null || {
    printf 'uu-x11-desktop: plasmashell is required or set UU_X11_DESKTOP_COMMAND\n' >&2
    exit 1
}

exec dbus-run-session -- bash -c '
    trap '\''kill "$kwin_pid" "$plasma_pid" 2>/dev/null || true; wait || true'\'' EXIT INT TERM
    kwin_x11 & kwin_pid=$!
    plasmashell & plasma_pid=$!
    wait -n "$kwin_pid" "$plasma_pid"
'
