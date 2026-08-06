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
run_probe 32 'DXVA11 decoder'
printf 'UU H.264 hardware encode and decode probes passed.\n'
