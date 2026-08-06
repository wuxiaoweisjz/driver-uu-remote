#!/usr/bin/env bash

uu_bridge_die() {
    printf 'uu-amf-bridge: %s\n' "$*" >&2
    exit 1
}

uu_bridge_find_prefix() {
    local data_home=${XDG_DATA_HOME:-$HOME/.local/share}
    local candidate

    if test -n "${UU_WINEPREFIX:-}"; then
        printf '%s\n' "$UU_WINEPREFIX"
        return
    fi
    if test -n "${WINEPREFIX:-}"; then
        printf '%s\n' "$WINEPREFIX"
        return
    fi

    for candidate in \
        "$data_home/uuyc-wine/wineprefix" \
        "$data_home/uu-game-booster/wineprefix" \
        "$HOME/.wine"; do
        if test -d "$candidate/drive_c/Program Files/Netease/GameViewer"; then
            printf '%s\n' "$candidate"
            return
        fi
    done

    uu_bridge_die 'UU Wine prefix not found; set UU_WINEPREFIX or WINEPREFIX.'
}

uu_bridge_find_bin() {
    local prefix=$1
    local candidate

    if test -n "${UU_BIN:-}"; then
        printf '%s\n' "$UU_BIN"
        return
    fi
    for candidate in \
        "$prefix/drive_c/Program Files/Netease/GameViewer/bin" \
        "$prefix/drive_c/Program Files (x86)/Netease/GameViewer/bin"; do
        if test -d "$candidate"; then
            printf '%s\n' "$candidate"
            return
        fi
    done

    uu_bridge_die 'UU GameViewer bin directory not found; set UU_BIN.'
}

uu_bridge_project_dir() {
    cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd
}

uu_bridge_artifact_dir() {
    local project_dir=$1
    local candidate

    if test -n "${UU_AMF_ARTIFACT_DIR:-}"; then
        printf '%s\n' "$UU_AMF_ARTIFACT_DIR"
        return
    fi
    for candidate in "$project_dir/build" /usr/lib/uu-amf-bridge; do
        if test -f "$candidate/amfrt64.dll" && test -f "$candidate/d3d11.dll"; then
            printf '%s\n' "$candidate"
            return
        fi
    done

    uu_bridge_die 'bridge artifacts not found; run make or install the Arch package.'
}

uu_bridge_is_running() {
    pgrep -f '(^|[/\\])(GameViewer|GameViewerServer|GameViewerHealthd|GameViewerService|GameViewerLauncher|StreamerCodecDetector)\.exe([[:space:]]|$)' >/dev/null
}

uu_bridge_assert_stopped() {
    if uu_bridge_is_running; then
        uu_bridge_die 'UU is running. Exit the main window and tray process before changing bridge DLLs.'
    fi
}
