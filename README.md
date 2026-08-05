# UU Remote AMF bridge for Wine

为 Wine 下的 UU 远程提供 AMD AMF 编码和 DXVA11 解码兼容层。项目实现 UU
`streamer.dll` 所需的 Windows `amfrt64.dll` ABI，并通过 Linux helper 将
D3D11 NV12 帧交给 Mesa RADV Vulkan Video。DXVA11 兼容层则把 H.264 picture
parameters、slice 和输出纹理桥接到 Vulkan Video 解码。

本项目不包含或修改 UU 的认证、信令、传输协议和客户端文件，也不绕过账号或
会员限制。

## 支持范围

- x86_64 Arch Linux 或兼容发行版
- AMD GPU，Mesa RADV 支持 Vulkan Video H.264 encode/decode
- Wine Staging 及 Wine 的 Windows 开发头文件/导入库
- FFmpeg 提供 `h264_vulkan` 编码器和 Vulkan H.264 解码支持
- H.264、8-bit、4:2:0、NV12

已验证组合：Wine Staging 11.14、UU 远程 4.33、AMD Renoir、Mesa 26.1.4，
分辨率最高 3840x2160。H.265、10-bit 和 4:4:4 当前明确返回不支持。

## AUR 安装

发布后可使用任意 AUR helper：

```sh
yay -S uu-amf-bridge-git
```

该包依赖 AUR 中已有的 `uuyc-wine`，会由 AUR helper 一并安装；桥接仓库和
桥接包本身不包含 UU 客户端二进制。

完全退出 UU 后安装 app-local DLL 并启动用户服务：

```sh
uu-amf-bridge-install
```

安装器会自动查找常见的 UU Wine prefix，备份已有 DLL，并安装固定校验版本的
DXVK。它不会替换 Wine `system32` 中的 DLL。

## 源码构建

安装构建依赖：

```sh
sudo pacman -S --needed base-devel ffmpeg libarchive mesa pkgconf vulkan-radeon wine-staging
```

构建并检查主机 Vulkan Video 能力：

```sh
make probe
make
make smoke
```

产物：

- `build/amfrt64.dll`: AMF ABI 和编码桥
- `build/d3d11.dll`: DXVK 代理及 DXVA11 注入入口
- `build/uu-amf-helper`: 本机 Vulkan Video helper
- `build/amf_pe_smoke.exe`: Wine 端编码/解码 smoke test

源码安装同样要求先完全退出 UU：

```sh
./scripts/install.sh
```

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
```

UU 的 `StreamerCodecDetector.exe` 需要两个与硬件相关的参数。它们不适合写入
公共仓库；从 UU 的一次正常探测命令或日志中取得后运行：

```sh
UU_DEVICE_ID=<device-id> UU_ADAPTER_ID=<adapter-id> uu-amf-bridge-verify
```

验证成功时，AMF 编码和 DXVA11 解码都会报告 H.264 8-bit 4:2:0 hardware
support。升级 UU、Wine、Mesa 或 DXVK 后应重新验证。

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
  -> loopback helper protocol (127.0.0.1:47890)
  -> FFmpeg + Mesa RADV Vulkan Video
```

- helper 仅监听 loopback；协议没有认证，不应暴露到局域网。
- D3D11 与 helper 之间目前经过 CPU 映射，硬编和硬解各有一次内存拷贝。
- “原画”仍受 UU 自身网络自适应控制，高丢包时可能独立降档。
- 这是第三方兼容项目，与网易、UU 或 AMD 无隶属关系。

## 隐私与问题报告

公开 issue 前请删除日志中的账号标识、设备标识、会话信息、Wine 注册表内容、
内网地址和公网地址。安全问题请遵循 [SECURITY.md](SECURITY.md)。

## 许可证

项目代码使用 MIT License。`third_party/AMF` 保留 AMD 上游许可证；DXVK 在
构建时从其官方 release 获取，并遵循其自身许可证。
