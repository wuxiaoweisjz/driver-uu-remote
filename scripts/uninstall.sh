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
libexec_dir=$HOME/.local/libexec
graphics_driver_state=$state_dir/wine-graphics-driver.state
webview_policy_state=$state_dir/webview-additional-arguments.state
webview_policy_key='HKLM\Software\Policies\Microsoft\Edge\WebView2\AdditionalBrowserArguments'

systemctl --user disable --now uu-x11-remote.service uu-session-guard.service \
    uu-amf-helper.service uu-x11-desktop.service uu-x11-display.service \
    uu-wayland-remote.service uu-wayland-session-guard.service \
    uu-wayland-capture.service 2>/dev/null || true
uu_bridge_assert_stopped

for name in amfrt64.dll d3d11.dll d3d11_dxvk.dll dxgi.dll; do
    marker=$state_dir/$name.state
    test -f "$marker" || continue
    case $(<"$marker") in
        present) install -m 0644 "$state_dir/backup/$name" "$uu_bin/$name" ;;
        absent) rm -f -- "$uu_bin/$name" ;;
    esac
done
if test -f "$uu_bin/GameViewerServer.real.exe"; then
    mv -f -- "$uu_bin/GameViewerServer.real.exe" "$uu_bin/GameViewerServer.exe"
fi
rm -f -- "$uu_bin/uu-server-compat.dll"
if test -f "$uu_bin/drivers/devcon.exe.uu-disabled"; then
    mv -f -- "$uu_bin/drivers/devcon.exe.uu-disabled" \
        "$uu_bin/drivers/devcon.exe"
fi
if test -f "$graphics_driver_state"; then
    case $(sed -n '1p' "$graphics_driver_state") in
        present)
            graphics_driver=$(sed -n '2p' "$graphics_driver_state")
            WINEPREFIX="$wine_prefix" WINEDEBUG=-all wine reg add \
                'HKCU\Software\Wine\Drivers' /v Graphics /t REG_SZ \
                /d "$graphics_driver" /f >/dev/null
            ;;
        absent)
            WINEPREFIX="$wine_prefix" WINEDEBUG=-all wine reg delete \
                'HKCU\Software\Wine\Drivers' /v Graphics /f >/dev/null 2>&1 || true
            ;;
    esac
    WINEPREFIX="$wine_prefix" wineserver -k >/dev/null 2>&1 || true
fi
if test -f "$webview_policy_state"; then
    case $(sed -n '1p' "$webview_policy_state") in
        present)
            webview_policy_value=$(sed -n '2p' "$webview_policy_state")
            WINEPREFIX="$wine_prefix" WINEDEBUG=-all wine reg add \
                "$webview_policy_key" /v '*' /t REG_SZ \
                /d "$webview_policy_value" /f >/dev/null
            ;;
        absent)
            WINEPREFIX="$wine_prefix" WINEDEBUG=-all wine reg delete \
                "$webview_policy_key" /v '*' /f >/dev/null 2>&1 || true
            ;;
    esac
    WINEPREFIX="$wine_prefix" wineserver -k >/dev/null 2>&1 || true
fi
rm -f -- "$config_home/systemd/user/uu-amf-helper.service"
rm -f -- "$config_home/systemd/user/uu-session-guard.service"
rm -f -- "$config_home/systemd/user/uu-x11-display.service"
rm -f -- "$config_home/systemd/user/uu-x11-desktop.service"
rm -f -- "$config_home/systemd/user/uu-x11-remote.service"
rm -f -- "$config_home/systemd/user/uu-wayland-capture.service"
rm -f -- "$config_home/systemd/user/uu-wayland-remote.service"
rm -f -- "$config_home/systemd/user/uu-wayland-session-guard.service"
rm -f -- "$libexec_dir/uu-session-guard" "$libexec_dir/uu-x11-display" \
    "$libexec_dir/uu-x11-desktop" "$libexec_dir/uu-wayland-run" \
    "$libexec_dir/uu-wayland-capture-helper"
systemctl --user daemon-reload
printf 'UU AMF bridge removed. State and backups remain in %s.\n' "$state_dir"
