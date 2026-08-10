#!/usr/bin/env bash
set -euo pipefail

prefix=${UU_WINEPREFIX:-${WINEPREFIX:-${XDG_DATA_HOME:-$HOME/.local/share}/uuyc-wine/wineprefix}}
prefix=$(readlink -f -- "$prefix")
interval=${UU_SESSION_GUARD_INTERVAL:-15}
upgrade_timeout=${UU_UPGRADE_MAX_SECONDS:-120}
wine=${WINE_BIN:-/usr/bin/wine}
bin_dir="$prefix/drive_c/Program Files/Netease/GameViewer/bin"
snapshot_ready=false
client_running=false
webview_running=false
health_running=false
server_running=false
upgrade_pids=()

process_has_prefix() {
    local pid=$1 entry process_prefix result=1
    {
        while IFS= read -r -d '' entry; do
            case $entry in
                WINEPREFIX=*)
                    process_prefix=${entry#WINEPREFIX=}
                    if [[ $process_prefix == "$prefix" ]] ||
                       [[ $(readlink -f -- "$process_prefix" 2>/dev/null || true) == "$prefix" ]]; then
                        result=0
                    fi
                    break
                    ;;
            esac
        done <"/proc/$pid/environ"
    } 2>/dev/null
    return "$result"
}

image_loaded() {
    local pid=$1 image=$2
    [[ -r "/proc/$pid/maps" ]] || return 1
    grep -Fq -- "$image" "/proc/$pid/maps" 2>/dev/null
}

scan_prefix_processes() {
    local pid cmdline
    client_running=false
    webview_running=false
    health_running=false
    server_running=false
    upgrade_pids=()
    for proc in /proc/[0-9]*; do
        pid=${proc##*/}
        process_has_prefix "$pid" || continue
        cmdline=$(tr '\0' ' ' <"$proc/cmdline" 2>/dev/null || true)
        if image_loaded "$pid" "$bin_dir/GameViewer.exe" ||
           [[ $cmdline == *'GameViewer.exe'* && $cmdline != *'msedgewebview2.exe'* ]]; then
            client_running=true
        fi
        [[ $cmdline == *'msedgewebview2.exe'* ]] && webview_running=true
        [[ $cmdline == *'GameViewerHealthd.exe'* ]] && health_running=true
        [[ $cmdline == *'GameViewerServer.exe'* ]] && server_running=true
        [[ $cmdline == source=upgrade\ * ]] && upgrade_pids+=("$pid")
    done
    snapshot_ready=true
}

process_elapsed_seconds() {
    ps -o etimes= -p "$1" 2>/dev/null | tr -d ' '
}

stop_stale_upgrader() {
    local pid=$1
    kill "$pid" 2>/dev/null || true
}

cleanup_stale_upgrades() {
    local pid elapsed
    [[ $snapshot_ready == true ]] || scan_prefix_processes
    for pid in "${upgrade_pids[@]}"; do
        elapsed=$(process_elapsed_seconds "$pid")
        [[ $elapsed =~ ^[0-9]+$ ]] || continue
        (( elapsed >= upgrade_timeout )) || continue
        printf 'uu-session-guard: stopping stale UU upgrader pid %s after %ss\n' \
            "$pid" "$elapsed" >&2
        stop_stale_upgrader "$pid"
    done
}

prefix_image_running() {
    local image=$1
    [[ $snapshot_ready == true ]] || scan_prefix_processes
    case $image in
        "$bin_dir/GameViewer.exe") [[ $client_running == true ]] ;;
        "$bin_dir/GameViewerHealthd.exe") [[ $health_running == true ]] ;;
        "$bin_dir/GameViewerServer.exe") [[ $server_running == true ]] ;;
        *) return 1 ;;
    esac
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
    [[ $snapshot_ready == true ]] || scan_prefix_processes
    [[ $client_running == false && $webview_running == true ]]
}

stop_prefix() {
    local pid cmdline
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

cleanup_once() {
    stale_prefix || return 0
    printf 'uu-session-guard: stale UU WebView session detected; stopping prefix %s\n' "$prefix" >&2
    stop_prefix
}

maintain_background_processes() {
    prefix_image_running "$bin_dir/GameViewer.exe" || return 0
    start_background_process GameViewerHealthd.exe
    # Linux acting as a controller must not run the server: its SendInput
    # bridge injects into the local desktop instead of forwarding to macOS.
    if [[ ${UU_REMOTE_ROLE:-controller} == host ]]; then
        start_background_process GameViewerServer.exe
    fi
}

main() {
    trap 'exit 0' INT TERM
    while :; do
        snapshot_ready=false
        cleanup_stale_upgrades
        cleanup_once
        maintain_background_processes
        [[ ${UU_SESSION_GUARD_ONCE:-0} == 1 ]] && break
        sleep "$interval"
    done
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    main "$@"
fi
