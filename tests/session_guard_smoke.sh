#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
temp_dir=$(mktemp -d)
trap 'rm -rf -- "$temp_dir"' EXIT

export UU_WINEPREFIX="$temp_dir/prefix"
export UU_SESSION_GUARD_STATE_DIR="$temp_dir/state"
mkdir -p "$UU_WINEPREFIX/drive_c/Program Files/Netease/GameViewer/log/client/Log"

# shellcheck source=../scripts/uu-session-guard.sh
source "$project_dir/scripts/uu-session-guard.sh"
prefix_image_running() { return 0; }

cleanup_called=
stale_prefix() { return 1; }
stop_prefix() { cleanup_called=stop; }
restart_client() { cleanup_called=restart; }
find_unhandled_disconnect() { return 0; }

cleanup_once
if [[ -n $cleanup_called ]]; then
    printf 'normal disconnect restarted the Wine prefix\n' >&2
    exit 1
fi

stale_prefix() { return 0; }
cleanup_once
if [[ $cleanup_called != stop ]]; then
    printf 'stale WebView session was not cleaned up\n' >&2
    exit 1
fi

stopped_upgrader=
scan_prefix_processes() {
    snapshot_ready=true
    upgrade_pids=(4242)
}
process_elapsed_seconds() { printf '121\n'; }
stop_stale_upgrader() { stopped_upgrader=$1; }
cleanup_stale_upgrades
if [[ $stopped_upgrader != 4242 ]]; then
    printf 'stale UU upgrader was not stopped\n' >&2
    exit 1
fi

starts=()
prefix_image_running() { [[ $1 == *GameViewer.exe ]]; }
start_background_process() { starts+=("$1"); }
maintain_background_processes
if [[ ${starts[*]} != GameViewerHealthd.exe ]]; then
    printf 'controller mode started a local GameViewerServer: %s\n' "${starts[*]}" >&2
    exit 1
fi

starts=()
UU_REMOTE_ROLE=host maintain_background_processes
if [[ ${starts[*]} != 'GameViewerHealthd.exe GameViewerServer.exe' ]]; then
    printf 'host mode did not start the UU server: %s\n' "${starts[*]}" >&2
    exit 1
fi

printf 'Session guard cleanup policy smoke passed\n'

upgrade_root="$UU_WINEPREFIX/drive_c/users/xiao/AppData/Local/GameViewer/upgrade"
mkdir -p "$upgrade_root" \
    "$UU_WINEPREFIX/drive_c/Program Files/Netease/GameViewer/setup" \
    "$UU_WINEPREFIX/drive_c/Program Files/Netease/GameViewer/bak"
printf '%s\r\n' \
    '[4.35.0.9113]' \
    'status=3' \
    'installfilepath=C:/Program Files/Netease/GameViewer/setup/UURemote_Setup_4.35.exe' \
    'changesource=manual_start_ready_version' >"$upgrade_root/upgrade.ini"
touch "$UU_WINEPREFIX/drive_c/Program Files/Netease/GameViewer/setup/UURemote_Setup_4.35.exe"
touch "$UU_WINEPREFIX/drive_c/Program Files/Netease/GameViewer/bak/Upgrade.exe"
mkdir -p "$UU_WINEPREFIX/drive_c/Program Files/Netease/GameViewer/bin"
touch "$UU_WINEPREFIX/drive_c/Program Files/Netease/GameViewer/bin/Upgrade.exe"
mkdir -p "$UU_WINEPREFIX/drive_c/Program Files/Netease/GameViewer/bin/drivers"
touch "$UU_WINEPREFIX/drive_c/Program Files/Netease/GameViewer/bin/drivers/devcon.exe"
disable_failed_upgrade
grep -Fqx 'status=0' "$upgrade_root/upgrade.ini"
test -f "$upgrade_root/disabled/UURemote_Setup_4.35.exe.disabled"
test -f "$UU_WINEPREFIX/drive_c/Program Files/Netease/GameViewer/bak/Upgrade.exe.disabled"
test -f "$UU_WINEPREFIX/drive_c/Program Files/Netease/GameViewer/bak/Upgrade.exe.uu-backup"
test -f "$UU_WINEPREFIX/drive_c/Program Files/Netease/GameViewer/bin/Upgrade.exe"
test ! -e "$UU_WINEPREFIX/drive_c/Program Files/Netease/GameViewer/bin/Upgrade.exe.disabled"
disable_wine_driver_installers
test ! -e "$UU_WINEPREFIX/drive_c/Program Files/Netease/GameViewer/bin/drivers/devcon.exe"
test -e "$UU_WINEPREFIX/drive_c/Program Files/Netease/GameViewer/bin/drivers/devcon.exe.uu-disabled"
printf 'Failed upgrade circuit breaker smoke passed\n'
