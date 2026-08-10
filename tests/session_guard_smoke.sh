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
