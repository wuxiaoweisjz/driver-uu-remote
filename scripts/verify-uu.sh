#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=common.sh
source "$script_dir/common.sh"
wine_prefix=$(uu_bridge_find_prefix)
uu_bin=$(uu_bridge_find_bin "$wine_prefix")
device_id=${UU_DEVICE_ID:-}
adapter_id=${UU_ADAPTER_ID:-}
output_dir=$(mktemp -d)
state_home=${XDG_STATE_HOME:-$HOME/.local/state}
remote_backend_state=$state_home/uu-amf-bridge/remote-backend
trap 'rm -rf -- "$output_dir"' EXIT

if uu_bridge_is_running; then
    printf 'UU is running; verification would reuse loaded DLLs. Exit UU completely first.\n' >&2
    exit 1
fi

artifact_dir=$(uu_bridge_artifact_dir "$(uu_bridge_project_dir)")
for name in amfrt64.dll d3d11.dll; do
    cmp -s "$artifact_dir/$name" "$uu_bin/$name" || {
        printf '%s does not match the packaged bridge. Rerun uu-amf-bridge-install.\n' "$uu_bin/$name" >&2
        exit 1
    }
done

systemctl --user is-active --quiet uu-amf-helper.service || {
    printf 'uu-amf-helper.service is not running.\n' >&2
    exit 1
}
graphics_driver=$({ WINEPREFIX="$wine_prefix" WINEDEBUG=-all wine reg query \
    'HKCU\Software\Wine\Drivers' /v Graphics 2>/dev/null || true; } |
    tr -d '\r' |
    sed -n 's/^[[:space:]]*Graphics[[:space:]]*REG_SZ[[:space:]]*//p' |
    tail -n1)
mouse_warp=$({ WINEPREFIX="$wine_prefix" WINEDEBUG=-all wine reg query \
    'HKCU\Software\Wine\DirectInput' /v MouseWarpOverride 2>/dev/null || true; } |
    tr -d '\r' |
    sed -n 's/^[[:space:]]*MouseWarpOverride[[:space:]]*REG_SZ[[:space:]]*//p' |
    tail -n1)
if test -s "$remote_backend_state"; then
    remote_backend=$(head -n1 "$remote_backend_state")
elif systemctl --user is-active --quiet uu-wayland-capture.service; then
    remote_backend=wayland
else
    remote_backend=x11
fi
if [[ $graphics_driver != x11 ]]; then
    printf 'Wine Graphics must be x11 for reliable UU keyboard and mouse input, got: %s\n' \
        "${graphics_driver:-unset}" >&2
    exit 1
fi
if [[ $mouse_warp != disable ]]; then
    printf 'Wine MouseWarpOverride must be disable so the pointer can leave UU, got: %s\n' \
        "${mouse_warp:-unset}" >&2
    exit 1
fi
case $remote_backend in
    wayland)
        systemctl --user is-active --quiet uu-wayland-capture.service || {
            printf 'uu-wayland-capture.service is not running.\n' >&2
            exit 1
        }
        "$HOME/.local/libexec/uu-wayland-capture-helper" --wait-ready 47892 5 || {
            printf 'Wayland capture has no frame; unlock the desktop and approve the portal request.\n' >&2
            exit 1
        }
        ;;
    x11)
        systemctl --user is-active --quiet uu-x11-display.service || {
            printf 'uu-x11-display.service is not running.\n' >&2
            exit 1
        }
        systemctl --user is-active --quiet uu-x11-desktop.service || {
            printf 'uu-x11-desktop.service is not running.\n' >&2
            exit 1
        }
        ;;
    *)
        printf 'Remote backend state must be wayland or x11, got: %s\n' \
            "${remote_backend:-unset}" >&2
        exit 1
        ;;
esac
test -n "$device_id" && test -n "$adapter_id" || {
    printf 'Set UU_DEVICE_ID and UU_ADAPTER_ID for StreamerCodecDetector.\n' >&2
    printf 'These values are hardware-specific; inspect a normal UU detector invocation or its log.\n' >&2
    exit 2
}
run_probe() {
    local implementation=$1
    local label=$2
    local output=$output_dir/$implementation.txt
    env WINEPREFIX="$wine_prefix" WINEDEBUG=-all DXVK_LOG_LEVEL=none \
        wine "$uu_bin/StreamerCodecDetector.exe" --batch "$implementation" \
        "$device_id" "$adapter_id" >"$output" 2>&1
    printf '%s:\n' "$label"
    sed -n '/^RESULT/,$p' "$output"
    if ! tr -d '\r' <"$output" | grep -q '^RESULT,1,1,8,[1-9][0-9]*,[1-9][0-9]*,1$'; then
        printf '%s did not report H.264 8-bit 4:2:0 hardware support.\n' "$label" >&2
        exit 1
    fi
}

run_probe 2 'AMD AMF encoder'
printf 'UU H.264 hardware encode probe passed; DXVA11 decode remains disabled.\n'
