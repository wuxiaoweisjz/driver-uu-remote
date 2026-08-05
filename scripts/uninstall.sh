#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=common.sh
source "$script_dir/common.sh"
wine_prefix=$(uu_bridge_find_prefix)
uu_bin=$(uu_bridge_find_bin "$wine_prefix")
config_home=${XDG_CONFIG_HOME:-$HOME/.config}
state_home=${XDG_STATE_HOME:-$HOME/.local/state}
state_dir=$state_home/uu-amf-bridge

if pgrep -f '[G]ameViewer(Server|Healthd|Service|Launcher)\.exe|[S]treamerCodecDetector\.exe' >/dev/null; then
    printf 'UU is running. Stop it before uninstalling.\n' >&2
    exit 1
fi
systemctl --user disable --now uu-amf-helper.service 2>/dev/null || true

for name in amfrt64.dll d3d11.dll d3d11_dxvk.dll dxgi.dll; do
    marker=$state_dir/$name.state
    test -f "$marker" || continue
    case $(<"$marker") in
        present) install -m 0644 "$state_dir/backup/$name" "$uu_bin/$name" ;;
        absent) rm -f -- "$uu_bin/$name" ;;
    esac
done
rm -f -- "$config_home/systemd/user/uu-amf-helper.service"
systemctl --user daemon-reload
printf 'UU AMF bridge removed. State and backups remain in %s.\n' "$state_dir"
