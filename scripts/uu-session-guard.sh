#!/usr/bin/env bash
set -euo pipefail

prefix=${UU_WINEPREFIX:-${WINEPREFIX:-${XDG_DATA_HOME:-$HOME/.local/share}/uuyc-wine/wineprefix}}
prefix=$(readlink -f -- "$prefix")
interval=${UU_SESSION_GUARD_INTERVAL:-15}
wine=${WINE_BIN:-/usr/bin/wine}
bin_dir="$prefix/drive_c/Program Files/Netease/GameViewer/bin"

process_has_prefix() {
    local pid=$1 value
    [[ -r "/proc/$pid/environ" ]] || return 1
    value=$({ tr '\0' '\n' <"/proc/$pid/environ"; } 2>/dev/null | sed -n 's/^WINEPREFIX=//p' | head -n1)
    [[ -n $value ]] && [[ $(readlink -f -- "$value" 2>/dev/null || true) == "$prefix" ]]
}

image_loaded() {
    local pid=$1 image=$2
    [[ -r "/proc/$pid/maps" ]] || return 1
    grep -Fq -- "$image" "/proc/$pid/maps" 2>/dev/null
}

prefix_image_running() {
    local image=$1 pid
    for proc in /proc/[0-9]*; do
        pid=${proc##*/}
        process_has_prefix "$pid" || continue
        image_loaded "$pid" "$image" && return 0
    done
    return 1
}

start_background_process() {
    local executable=$1
    prefix_image_running "$bin_dir/$executable" && return 0
    (
        cd "$bin_dir"
        if [[ ${UU_REMOTE_BACKEND:-x11} == wayland ]]; then
            env WINEPREFIX="$prefix" WINEDEBUG=-all \
                nohup "$wine" "$executable" >/dev/null 2>&1 &
        else
            env -u WAYLAND_DISPLAY WINEPREFIX="$prefix" WINEDEBUG=-all \
                nohup "$wine" "$executable" >/dev/null 2>&1 &
        fi
    )
}

stale_prefix() {
    local pid cmdline
    local has_client=false has_webview=false
    for proc in /proc/[0-9]*; do
        pid=${proc##*/}
        process_has_prefix "$pid" || continue
        cmdline=$(tr '\0' ' ' <"$proc/cmdline" 2>/dev/null || true)
        if image_loaded "$pid" "$bin_dir/GameViewer.exe" ||
           [[ $cmdline == *'GameViewer.exe'* && $cmdline != *'msedgewebview2.exe'* ]]; then
            has_client=true
        fi
        [[ $cmdline == *'msedgewebview2.exe'* ]] && has_webview=true
    done
    [[ $has_client == false && $has_webview == true ]]
}

cleanup_once() {
    local pid cmdline
    stale_prefix || return 0
    printf 'uu-session-guard: stale UU WebView session detected; stopping prefix %s\n' "$prefix" >&2
    WINEPREFIX="$prefix" wineserver -k >/dev/null 2>&1 || true
    for proc in /proc/[0-9]*; do
        pid=${proc##*/}
        process_has_prefix "$pid" || continue
        cmdline=$(tr '\0' ' ' <"$proc/cmdline" 2>/dev/null || true)
        if [[ $cmdline == *'msedgewebview2.exe'* || $cmdline == *'crashpad_handler.exe'* ]]; then
            kill "$pid" 2>/dev/null || true
        fi
    done
}

maintain_background_processes() {
    prefix_image_running "$bin_dir/GameViewer.exe" || return 0
    start_background_process GameViewerHealthd.exe
    start_background_process GameViewerServer.exe
}

trap 'exit 0' INT TERM
while :; do
    cleanup_once
    maintain_background_processes
    sleep "$interval"
done
