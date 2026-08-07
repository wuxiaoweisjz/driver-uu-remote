#!/usr/bin/env bash
set -euo pipefail

display=${UU_X11_DISPLAY:-:99}
screen=${UU_X11_SCREEN:-1920x1080x24}
xauthority=${UU_X11_XAUTHORITY:-}

display_ready() {
    DISPLAY="$display" XAUTHORITY="$xauthority" xdpyinfo >/dev/null 2>&1
}

if display_ready; then
    printf 'uu-x11-display: reusing existing X server on %s\n' "$display"
    while display_ready; do
        sleep 5
    done
    exit 1
fi

printf 'uu-x11-display: starting Xvfb on %s (%s)\n' "$display" "$screen"
exec Xvfb "$display" -screen 0 "$screen" -nolisten tcp -noreset +extension RANDR
