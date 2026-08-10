# UU Remote AMF bridge for Wine

为 Wine 下的 UU 远程提供 AMD AMF 编码和 Wayland 兼容层。项目实现 UU
`streamer.dll` 所需的 Windows `amfrt64.dll` ABI，并通过 Linux helper 将
D3D11 NV12 帧交给 Mesa RADV Vulkan Video。控制其他设备时使用 UU 内置的
H.264 软件解码；Wine 下的实验性 DXVA11 回传路径会积压帧，因此不对外报告。
Wine Wayland 下的桌面采集与输入通过 app-local `d3d11.dll` 修补
`streamer.dll` 的 `GDI32!BitBlt` 和 `USER32!SendInput` 导入：画面从
ScreenCast Portal/PipeWire 取得，键盘和鼠标事件通过仅监听 loopback 的 helper
转为 Linux `/dev/uinput` 事件。

本项目不包含或修改 UU 的认证、信令、传输协议和客户端文件，也不绕过账号或
会员限制。

## 支持范围

- x86_64 Arch Linux 或兼容发行版
- AMD GPU，Mesa RADV 支持 Vulkan Video H.264 encode/decode
- Wine Staging 及 Wine 的 Windows 开发头文件/导入库
- FFmpeg 提供 `h264_vulkan` 编码器和 Vulkan H.264 解码支持
- H.264、8-bit、4:2:0、NV12

已验证组合：Wine Staging 11.14、UU 远程 4.35、AMD Renoir、Mesa 26.1.4，
分辨率最高 3840x2160。H.265、10-bit 和 4:4:4 当前明确返回不支持。

## Arch Linux 安装

先通过任意 AUR helper 安装 UU Wine 客户端。安装 AUR 软件不需要 AUR 账户：

```sh
yay -S uuyc-wine
```

添加本项目的 Pacman 仓库并安装桥接包：

```sh
sudo curl -o /etc/pacman.d/uu-amf-bridge.conf \
  https://wuxiaoweisjz.github.io/driver-uu-remote/uu-amf-bridge.conf
echo 'Include = /etc/pacman.d/uu-amf-bridge.conf' | sudo tee -a /etc/pacman.conf
sudo pacman -Syu uu-amf-bridge-git
```

也可以从 [GitHub Releases](https://github.com/wuxiaoweisjz/driver-uu-remote/releases)
下载 `.pkg.tar.zst` 后使用 `sudo pacman -U <package>` 安装。桥接仓库和桥接包
本身不包含 UU 客户端二进制。

完全退出 UU 后安装 app-local DLL 并启动用户服务：

```sh
uu-amf-bridge-install
```

安装器会自动查找常见的 UU Wine prefix，备份已有 DLL，并安装固定校验版本的
DXVK。它不会替换 Wine `system32` 中的 DLL，也会拒绝在 UU 主程序、服务端或
硬件探测器仍在运行时替换 app-local DLL。Wayland 桌面模式会让 UU 使用 Wine X11
窗口驱动，以兼容 UU 的键盘、鼠标抓取，同时通过 Portal/PipeWire 采集真实 Wayland
桌面，并通过 EIS 注入远端输入。首次运行时必须在已解锁
的本机 Wayland 桌面批准系统的屏幕共享请求；授权会由 Portal 持久保存，capture
helper 或系统重启后自动恢复并轮换 restore token。UU 会等到首帧可用后再启动。
卸载时会恢复原来的图形驱动配置。

需要回退到独立 X11 桌面时运行：

```sh
UU_REMOTE_BACKEND=x11 uu-amf-bridge-install
```

X11 模式会启动或复用 `:99`，并让 UU 主程序、后台服务和会话守护统一运行在该
显示上。切回 Wayland 使用 `UU_REMOTE_BACKEND=wayland uu-amf-bridge-install`。

## 源码构建

安装构建、Wayland Portal 和 X11 回退依赖：

```sh
sudo pacman -S --needed base-devel ffmpeg libarchive mesa pkgconf vulkan-radeon \
  wine-staging qt6-base pipewire gst-plugin-pipewire gst-plugins-base-libs \
  xdg-desktop-portal xdg-desktop-portal-kde \
  xorg-server-xvfb xorg-xwayland kwin plasma-workspace
```

构建并检查主机 Vulkan Video 能力：

```sh
make probe
make
make smoke
```

产物：

- `build/amfrt64.dll`: AMF ABI 和编码桥
- `build/d3d11.dll`: DXVK 代理和 Wayland 捕获注入入口
- `build/uu-amf-helper`: 本机 Vulkan Video helper
- `build/uu-wayland-capture-helper`: Portal/PipeWire 桌面采集 helper
- `build/amf_pe_smoke.exe`: Wine 端编码和解码回退 smoke test

源码安装同样要求先完全退出 UU：

```sh
./scripts/install.sh
```

重复安装或升级时，安装器会先正常停止当前后端的 remote 和 session guard 服务，
部署完成后再自动启动；若另有手工启动的 UU 进程，安装器仍会拒绝替换正在使用的
DLL。

## 路径配置

脚本按以下顺序定位 Wine prefix：

1. `UU_WINEPREFIX`
2. `WINEPREFIX`
3. `${XDG_DATA_HOME:-$HOME/.local/share}/uuyc-wine/wineprefix`
4. `${XDG_DATA_HOME:-$HOME/.local/share}/uu-game-booster/wineprefix`
5. `$HOME/.wine`，仅当其中存在 GameViewer

非标准安装可以显式指定：

```sh
UU_WINEPREFIX=/path/to/wineprefix uu-amf-bridge-install
```

也可以用 `UU_BIN` 直接指定 GameViewer 的 `bin` 目录。源码安装器默认使用
`build/`，打包安装器使用 `/usr/lib/uu-amf-bridge`；可通过
`UU_AMF_ARTIFACT_DIR` 覆盖。

## 验证

检查 helper：

```sh
systemctl --user status uu-amf-helper.service
journalctl --user -u uu-amf-helper.service -n 50
systemctl --user status uu-wayland-capture.service
journalctl --user -u uu-wayland-capture.service -n 50
```

Wayland 模式启用 `uu-wayland-session-guard.service`，X11 模式启用
`uu-session-guard.service`。它们按 Wine 前缀监控 UU 的
`GameViewer.exe`、`GameViewerServer.exe` 和 `GameViewerHealthd.exe`：主客户端
退出而 WebView2 残留时自动回收孤儿会话；主客户端仍在运行但后台服务意外退出时
以当前图形后端拉起后台服务，避免设备在远端显示为离线。守护进程不会触碰
其他 Wine 前缀，也不会在活跃连接期间根据采集日志重启服务端。

Wayland 模式下，`uu-wayland-capture.service`、`uu-wayland-remote.service`
和 `uu-wayland-session-guard.service` 分别负责桌面帧、UU 自启动及会话守护。
ScreenCast Portal 不允许在锁屏状态创建首次授权。首次批准后，helper 会以权限
`0600` 保存 Portal restore token，并在 capture helper 或系统重启时自动恢复；
Portal 权限被撤销、原显示器不再可用或 token 失效时，才需要在已解锁的本机桌面
重新批准屏幕共享请求。helper 运行期间会通过桌面标准 ScreenSaver API 抑制自动
锁屏，避免 Portal 在无人值守连接中切成黑帧；手工锁屏仍然生效，解锁后服务会
自动重试采集。

X11 回退模式下，`uu-x11-display.service`、`uu-x11-desktop.service` 和
`uu-x11-remote.service` 负责专用 X11 显示、Plasma 桌面及 UU 自启动。可用以下
命令检查运行状态：

```sh
systemctl --user status uu-x11-display.service uu-x11-desktop.service \
  uu-x11-remote.service
```

专用 X11 桌面与本机登录的 Wayland 桌面是两个不同会话。Wine X11 只能采集
X11 窗口，无法读取原生 Wayland 窗口，所以远端看到的是 `:99` 上的 Plasma
桌面，而不是当前 `:0` Wayland 会话。需要自定义显示号、分辨率或桌面命令时，
可以通过 systemd user drop-in 覆盖 `UU_X11_DISPLAY`、`UU_X11_SCREEN` 或
`UU_X11_DESKTOP_COMMAND`。

UU 的 `StreamerCodecDetector.exe` 需要两个与硬件相关的参数。它们不适合写入
公共仓库；从 UU 的一次正常探测命令或日志中取得后运行：

```sh
UU_DEVICE_ID=<device-id> UU_ADAPTER_ID=<adapter-id> uu-amf-bridge-verify
```

验证成功时，AMF 编码会报告 H.264 8-bit 4:2:0 hardware support，DXVA11
解码则保持禁用以选择 UU 的低延迟软件解码。验证器还会确认正在验证的 DLL 与当前
构建或已安装软件包完全一致。
升级 UU、Wine、Mesa 或 DXVK 后应重新安装并验证。

## 回滚

完全退出 UU 后运行：

```sh
uu-amf-bridge-uninstall
```

安装前存在的同名 DLL 会恢复，原本不存在的文件会删除。备份保留在
`${XDG_STATE_HOME:-$HOME/.local/state}/uu-amf-bridge/backup`。

## 工作原理与限制

```text
UU streamer.dll
  -> amfrt64.dll / d3d11.dll compatibility bridge
  -> encode/decode helper (127.0.0.1:47890)
  -> FFmpeg + Mesa RADV Vulkan Video
  -> GDI BitBlt hook -> capture helper (127.0.0.1:47892)
  -> ScreenCast Portal + PipeWire
  -> USER32 SendInput hook -> capture helper -> KWin EIS/libei
```

在 Plasma Wayland 下，UU 窗口运行于 XWayland，以兼容作为控制端连接 Windows 或
macOS 时所需的键盘、鼠标抓取；被控端输入优先通过 KWin 的 EIS RemoteDesktop
通道注入，避免合成器忽略 `/dev/uinput` 虚拟设备。非 KWin 桌面仍保留
`/dev/uinput` 回退。安装器同时禁用 Wine 的鼠标回卷，使指针能够离开 UU 远控窗口；
卸载时会恢复原有注册表值。Wayland 启动器将 DXVK 限制在 60 FPS，避免 UU 主界面
空闲时持续占用一个 CPU 核心，同时保留远控所需的 60 FPS 上限。

- helper 仅监听 loopback；协议没有认证，不应暴露到局域网。
- DLL 与 helper 使用带版本号的握手；混用不同协议版本时硬件能力探测会失败，
  不会进入半初始化状态。
- helper 的阻塞发送有 5 秒上限；输入和采集连接允许在整个 UU 会话中保持空闲，
  不会丢失空闲后的第一个按键或点击。helper 卡死或退出时调用会失败返回并在后续
  请求时重连，避免长期阻塞 UU 的图形线程。
- D3D11 与 helper 之间目前经过 CPU 映射，硬编和硬解各有一次内存拷贝。
- “原画”仍受 UU 自身网络自适应控制，高丢包时可能独立降档。
- 这是第三方兼容项目，与网易、UU 或 AMD 无隶属关系。

## 隐私与问题报告

公开 issue 前请删除日志中的账号标识、设备标识、会话信息、Wine 注册表内容、
内网地址和公网地址。安全问题请遵循 [SECURITY.md](SECURITY.md)。

## 许可证

项目代码使用 MIT License。`third_party/AMF` 保留 AMD 上游许可证；DXVK 在
构建时从其官方 release 获取，其 zlib 许可保存在
`third_party/DXVK-LICENSE.txt` 并随二进制包安装。
