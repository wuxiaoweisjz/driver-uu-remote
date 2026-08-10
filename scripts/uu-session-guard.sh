#!/usr/bin/env bash
set -euo pipefail

prefix=${UU_WINEPREFIX:-${WINEPREFIX:-${XDG_DATA_HOME:-$HOME/.local/share}/uuyc-wine/wineprefix}}
prefix=$(readlink -f -- "$prefix")
interval=${UU_SESSION_GUARD_INTERVAL:-15}
upgrade_timeout=${UU_UPGRADE_MAX_SECONDS:-120}
wine=${WINE_BIN:-/usr/bin/wine}
bin_dir="$prefix/drive_c/Program Files/Netease/GameViewer/bin"
upgrade_dir="$prefix/drive_c/users/${USER}/AppData/Local/GameViewer/upgrade"
upgrade_ini="$upgrade_dir/upgrade.ini"
snapshot_ready=false
client_running=false
webview_running=false
health_running=false
server_running=false
upgrade_pids=()

disable_wine_driver_installers() {
    local devcon="$bin_dir/drivers/devcon.exe"
    local disabled="${devcon}.uu-disabled"
    [[ -f $devcon ]] || return 0
    [[ -f $disabled ]] || cp -p -- "$devcon" "$disabled"
    rm -f -- "$devcon"
    printf 'uu-session-guard: disabled Windows-only UU driver installer %s\n' \
        "$devcon" >&2
}

find_upgrade_ini() {
    [[ -f $upgrade_ini ]] && return 0
    upgrade_ini=$(find "$prefix/drive_c/users" -type f \
        -path '*/AppData/Local/GameViewer/upgrade/upgrade.ini' \
        -print -quit 2>/dev/null || true)
    [[ -n $upgrade_ini && -f $upgrade_ini ]]
}

disable_failed_upgrade() {
    local disabled_dir setup_file upgrade_exe
    [[ ${UU_ALLOW_WINE_UPGRADE:-0} != 1 ]] || return 0
    find_upgrade_ini || return 0
    # Vendor INI files are normally CRLF even when updated from Linux.
    tr -d '\r' <"$upgrade_ini" | grep -Fqx 'status=3' || return 0

    # Status 3 is the ready-to-install state. On Wine this UU installer
    # repeatedly stalls, so quarantine the executable and make the state
    # explicitly non-installable before the client is started again.
    disabled_dir="${upgrade_ini%/*}/disabled"
    mkdir -p -- "$disabled_dir"
    for upgrade_exe in \
        "$prefix/drive_c/Program Files/Netease/GameViewer/bak/Upgrade.exe" \
        "$prefix/drive_c/Program Files/Netease/GameViewer/Upgrade.exe"; do
        [[ -f $upgrade_exe ]] || continue
        [[ -f ${upgrade_exe}.uu-backup ]] || cp -p -- "$upgrade_exe" "${upgrade_exe}.uu-backup"
        mv -f -- "$upgrade_exe" "${upgrade_exe}.disabled"
    done
    while IFS= read -r setup_file; do
        setup_file=${setup_file%$'\r'}
        setup_file="$prefix/drive_c/Program Files/Netease/GameViewer/setup/$setup_file"
        [[ -f $setup_file ]] || continue
        mv -- "$setup_file" "$disabled_dir/$(basename "$setup_file").disabled" || true
    done < <(sed -n 's/^installfilepath=C:\/Program Files\/Netease\/GameViewer\/setup\///p' \
        "$upgrade_ini")
    local tmp
    tmp=$(mktemp "${upgrade_ini}.XXXXXX")
    awk '
        { sub(/\r$/, "") }
        /^status=3$/ { print "status=0"; next }
        /^installfilepath=/ { print "installfilepath="; next }
        /^changesource=/ { print "changesource=disabled_by_uu_bridge"; next }
        { print }
    ' "$upgrade_ini" >"$tmp"
    chmod --reference="$upgrade_ini" "$tmp" 2>/dev/null || true
    mv -f -- "$tmp" "$upgrade_ini"
    printf 'uu-session-guard: disabled failed UU upgrade described by %s\n' \
        "$upgrade_ini" >&2
}

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
        disable_wine_driver_installers
        disable_failed_upgrade
        cleanup_stale_upgrades
        cleanup_once
        maintain_background_processes
        [[ ${UU_SESSION_GUARD_ONCE:-0} == 1 ]] && break
        sleep "$interval"
    done
}

if [[ ${1:-} == --disable-failed-upgrade ]]; then
    disable_wine_driver_installers
    disable_failed_upgrade
    exit 0
fi

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    main "$@"
fi
