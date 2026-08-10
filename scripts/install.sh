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
graphics_driver_state=$state_dir/wine-graphics-driver.state
mouse_warp_state=$state_dir/wine-mouse-warp.state
webview_policy_state=$state_dir/webview-additional-arguments.state
mouse_warp_key='HKCU\Software\Wine\DirectInput'
webview_policy_key='HKLM\Software\Policies\Microsoft\Edge\WebView2\AdditionalBrowserArguments'
webview_policy_args='--single-process --in-process-gpu'
remote_backend_state=$state_dir/remote-backend
remote_backend=${UU_REMOTE_BACKEND:-wayland}
wine_graphics_driver=x11
dxvk_version=3.0.2
dxvk_hash=9c538924110a7cdef871ca36dee218c0774124374ffdeb38af4b76be55bdf7c2
dxvk_url=https://github.com/doitsujin/dxvk/releases/download/v${dxvk_version}/dxvk-${dxvk_version}.tar.gz

case $remote_backend in
    wayland|x11) ;;
    *) uu_bridge_die 'UU_REMOTE_BACKEND must be wayland or x11.' ;;
esac
for artifact in "$artifact_dir/amfrt64.dll" "$artifact_dir/d3d11.dll" \
    "$artifact_dir/uu-amf-helper" "$artifact_dir/uu-wayland-capture-helper" \
    "$artifact_dir/uu-server-launcher.exe" "$artifact_dir/uu-server-compat.dll"; do
    test -f "$artifact" || { printf 'Missing %s; run make first.\n' "$artifact" >&2; exit 1; }
done
capture_helper_changed=true
if cmp -s "$artifact_dir/uu-wayland-capture-helper" \
    "$libexec_dir/uu-wayland-capture-helper" 2>/dev/null; then
    capture_helper_changed=false
fi
test -d "$uu_bin" || { printf 'UU bin directory not found: %s\n' "$uu_bin" >&2; exit 1; }
systemctl --user stop uu-x11-remote.service uu-x11-desktop.service \
    uu-x11-display.service uu-session-guard.service \
    uu-wayland-remote.service uu-wayland-session-guard.service 2>/dev/null || true
uu_bridge_assert_stopped

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

server_launcher=$uu_bin/GameViewerServer.exe
server_real=$uu_bin/GameViewerServer.real.exe
if ! test -f "$server_real"; then
    cp -a -- "$server_launcher" "$backup_dir/GameViewerServer.exe"
    mv -- "$server_launcher" "$server_real"
elif ! cmp -s "$server_launcher" "$artifact_dir/uu-server-launcher.exe"; then
    cp -a -- "$server_launcher" "$backup_dir/GameViewerServer.exe.upgrade"
    mv -f -- "$server_launcher" "$server_real"
fi
install -m 0755 "$artifact_dir/uu-server-launcher.exe" "$server_launcher"
install -m 0644 "$artifact_dir/uu-server-compat.dll" "$uu_bin/uu-server-compat.dll"

if ! test -f "$graphics_driver_state"; then
    graphics_driver=$({ WINEPREFIX="$wine_prefix" WINEDEBUG=-all wine reg query \
        'HKCU\Software\Wine\Drivers' /v Graphics 2>/dev/null || true; } |
        tr -d '\r' |
        sed -n 's/^[[:space:]]*Graphics[[:space:]]*REG_SZ[[:space:]]*//p' |
        tail -n1)
    if test -n "$graphics_driver"; then
        printf 'present\n%s\n' "$graphics_driver" >"$graphics_driver_state"
    else
        printf 'absent\n' >"$graphics_driver_state"
    fi
fi
WINEPREFIX="$wine_prefix" WINEDEBUG=-all wine reg add \
    'HKCU\Software\Wine\Drivers' /v Graphics /t REG_SZ \
    /d "$wine_graphics_driver" /f >/dev/null
if ! test -f "$mouse_warp_state"; then
    mouse_warp=$({ WINEPREFIX="$wine_prefix" WINEDEBUG=-all wine reg query \
        "$mouse_warp_key" /v MouseWarpOverride 2>/dev/null || true; } |
        tr -d '\r' |
        sed -n 's/^[[:space:]]*MouseWarpOverride[[:space:]]*REG_SZ[[:space:]]*//p' |
        tail -n1)
    if test -n "$mouse_warp"; then
        printf 'present\n%s\n' "$mouse_warp" >"$mouse_warp_state"
    else
        printf 'absent\n' >"$mouse_warp_state"
    fi
fi
WINEPREFIX="$wine_prefix" WINEDEBUG=-all wine reg add "$mouse_warp_key" \
    /v MouseWarpOverride /t REG_SZ /d disable /f >/dev/null
if ! test -f "$webview_policy_state"; then
    if policy_output=$(WINEPREFIX="$wine_prefix" WINEDEBUG=-all wine reg query \
        "$webview_policy_key" /v '*' 2>/dev/null); then
        policy_value=$(printf '%s\n' "$policy_output" | tr -d '\r' | sed -n \
            's/^[[:space:]]*\*[[:space:]]*REG_SZ[[:space:]]*//p' | tail -n1)
        printf 'present\n%s\n' "$policy_value" >"$webview_policy_state"
    else
        printf 'absent\n' >"$webview_policy_state"
    fi
fi
WINEPREFIX="$wine_prefix" WINEDEBUG=-all wine reg add "$webview_policy_key" \
    /v '*' /t REG_SZ /d "$webview_policy_args" /f >/dev/null
WINEPREFIX="$wine_prefix" wineserver -k >/dev/null 2>&1 || true

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
install -m 0755 "$artifact_dir/uu-wayland-capture-helper" \
    "$libexec_dir/uu-wayland-capture-helper"
install -m 0755 "$project_dir/scripts/uu-session-guard.sh" "$libexec_dir/uu-session-guard"
install -m 0755 "$project_dir/scripts/uu-wayland-run.sh" "$libexec_dir/uu-wayland-run"
install -m 0755 "$project_dir/scripts/uu-x11-display.sh" "$libexec_dir/uu-x11-display"
install -m 0755 "$project_dir/scripts/uu-x11-desktop.sh" "$libexec_dir/uu-x11-desktop"
install -m 0644 "$project_dir/packaging/uu-amf-helper.service" "$unit_dir/uu-amf-helper.service"
install -m 0644 "$project_dir/packaging/uu-session-guard.service" "$unit_dir/uu-session-guard.service"
install -m 0644 "$project_dir/packaging/uu-x11-display.service" "$unit_dir/uu-x11-display.service"
install -m 0644 "$project_dir/packaging/uu-x11-desktop.service" "$unit_dir/uu-x11-desktop.service"
install -m 0644 "$project_dir/packaging/uu-x11-remote.service" "$unit_dir/uu-x11-remote.service"
install -m 0644 "$project_dir/packaging/uu-wayland-capture.service" \
    "$unit_dir/uu-wayland-capture.service"
install -m 0644 "$project_dir/packaging/uu-wayland-remote.service" \
    "$unit_dir/uu-wayland-remote.service"
install -m 0644 "$project_dir/packaging/uu-wayland-session-guard.service" \
    "$unit_dir/uu-wayland-session-guard.service"
sha256sum "$uu_bin/amfrt64.dll" "$uu_bin/d3d11.dll" "$uu_bin/d3d11_dxvk.dll" "$uu_bin/dxgi.dll" >"$state_dir/installed.sha256"
printf '%s\n' "$remote_backend" >"$remote_backend_state"
systemctl --user daemon-reload
systemctl --user disable --now uu-x11-remote.service uu-x11-desktop.service \
    uu-x11-display.service uu-session-guard.service \
    uu-wayland-remote.service uu-wayland-session-guard.service 2>/dev/null || true
if [[ $remote_backend == wayland ]]; then
    systemctl --user enable uu-amf-helper.service uu-wayland-capture.service \
        uu-wayland-remote.service uu-wayland-session-guard.service
    systemctl --user restart uu-amf-helper.service
    if [[ $capture_helper_changed == true ]]; then
        systemctl --user restart uu-wayland-capture.service
    else
        systemctl --user start uu-wayland-capture.service
    fi
    systemctl --user restart uu-wayland-remote.service uu-wayland-session-guard.service
else
    systemctl --user disable --now uu-wayland-capture.service 2>/dev/null || true
    systemctl --user enable uu-x11-display.service uu-x11-desktop.service \
        uu-amf-helper.service uu-x11-remote.service uu-session-guard.service
    systemctl --user restart uu-x11-display.service uu-x11-desktop.service \
        uu-amf-helper.service uu-x11-remote.service uu-session-guard.service
fi
printf 'Installed. UU is running with the %s backend. Set the detector IDs and run uu-amf-bridge-verify.\n' \
    "$remote_backend"
