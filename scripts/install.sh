#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=common.sh
source "$script_dir/common.sh"
project_dir=$(uu_bridge_project_dir)
wine_prefix=$(uu_bridge_find_prefix)
uu_bin=$(uu_bridge_find_bin "$wine_prefix")
artifact_dir=$(uu_bridge_artifact_dir "$project_dir")
config_home=${XDG_CONFIG_HOME:-$HOME/.config}
state_home=${XDG_STATE_HOME:-$HOME/.local/state}
state_dir=$state_home/uu-amf-bridge
backup_dir=$state_dir/backup
libexec_dir=$HOME/.local/libexec
unit_dir=$config_home/systemd/user
dxvk_version=3.0.2
dxvk_hash=9c538924110a7cdef871ca36dee218c0774124374ffdeb38af4b76be55bdf7c2
dxvk_url=https://github.com/doitsujin/dxvk/releases/download/v${dxvk_version}/dxvk-${dxvk_version}.tar.gz

for artifact in "$artifact_dir/amfrt64.dll" "$artifact_dir/d3d11.dll" "$artifact_dir/uu-amf-helper"; do
    test -f "$artifact" || { printf 'Missing %s; run make first.\n' "$artifact" >&2; exit 1; }
done
test -d "$uu_bin" || { printf 'UU bin directory not found: %s\n' "$uu_bin" >&2; exit 1; }
if pgrep -f '[G]ameViewer(Server|Healthd|Service|Launcher)\.exe|[S]treamerCodecDetector\.exe' >/dev/null; then
    printf 'UU is running. Stop it before installing.\n' >&2
    exit 1
fi

mkdir -p "$backup_dir" "$libexec_dir" "$unit_dir"
temp_dir=$(mktemp -d)
trap 'rm -rf -- "$temp_dir"' EXIT
if ! test -f "$artifact_dir/d3d11_dxvk.dll" || ! test -f "$artifact_dir/dxgi.dll"; then
    archive=$temp_dir/dxvk.tar.gz
    curl -fL --retry 3 -o "$archive" "${DXVK_URL:-$dxvk_url}"
    printf '%s  %s\n' "$dxvk_hash" "$archive" | sha256sum -c -
    bsdtar -xf "$archive" -C "$temp_dir"
fi

backup_target() {
    local name=$1
    local marker=$state_dir/$name.state
    if test -e "$marker"; then return; fi
    if test -e "$uu_bin/$name"; then
        cp -a -- "$uu_bin/$name" "$backup_dir/$name"
        printf 'present\n' >"$marker"
    else
        printf 'absent\n' >"$marker"
    fi
}

for name in amfrt64.dll d3d11.dll d3d11_dxvk.dll dxgi.dll; do backup_target "$name"; done
install -m 0644 "$artifact_dir/amfrt64.dll" "$uu_bin/amfrt64.dll"
install -m 0644 "$artifact_dir/d3d11.dll" "$uu_bin/d3d11.dll"
if test -f "$artifact_dir/d3d11_dxvk.dll" && test -f "$artifact_dir/dxgi.dll"; then
    install -m 0644 "$artifact_dir/d3d11_dxvk.dll" "$uu_bin/d3d11_dxvk.dll"
    install -m 0644 "$artifact_dir/dxgi.dll" "$uu_bin/dxgi.dll"
else
    install -m 0644 "$temp_dir/dxvk-${dxvk_version}/x64/d3d11.dll" "$uu_bin/d3d11_dxvk.dll"
    install -m 0644 "$temp_dir/dxvk-${dxvk_version}/x64/dxgi.dll" "$uu_bin/dxgi.dll"
fi
install -m 0755 "$artifact_dir/uu-amf-helper" "$libexec_dir/uu-amf-helper"
install -m 0644 "$project_dir/packaging/uu-amf-helper.service" "$unit_dir/uu-amf-helper.service"
sha256sum "$uu_bin/amfrt64.dll" "$uu_bin/d3d11.dll" "$uu_bin/d3d11_dxvk.dll" "$uu_bin/dxgi.dll" >"$state_dir/installed.sha256"
systemctl --user daemon-reload
systemctl --user enable --now uu-amf-helper.service
printf 'Installed. Set the detector IDs and run uu-amf-bridge-verify before starting UU.\n'
