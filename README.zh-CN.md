# UniXNU for OnePlus Ace 5

[English](README.md) | **简体中文**

面向一加 Ace 5（PKG110，代号 `giulia`，Qualcomm SM8650）的实验性 UEFI 与 Darwin/XNU 移植项目。当前开发目标为 iOS 16 / Darwin 22。

仓库提供板级配置、上游源码版本锁定、内核与 UEFI 补丁、构建工具及验证记录，不包含 Apple 系统镜像或设备固件。

> [!WARNING]
> 本项目仍处于开发阶段，不适合日常使用。刷写分区或替换 userdata 可能造成数据丢失。编译成功不代表硬件功能已可用。

## 硬件支持

状态更新于 **2026-10-23**。验证结果仅适用于 [`hardware/`](hardware/) 中记录的测试配置。

| 子系统 | 状态 | 验证范围与限制 |
| --- | --- | --- |
| 启动与内核 | 已验证 | UEFI 交接、XNU 启动与 CPU0；当前使用单核配置 |
| 存储 | 已验证 | 原生 UFS 访问及已准备的 HFSX 根卷 |
| 调试控制台 | 已验证 | USB CDC 串口传输与启动日志采集 |
| 用户界面 | 已验证 | 可启动 Hello / 设置界面；完整系统功能仍在开发中 |
| 显示与触控 | 已验证 | 设置界面铺满 1264×2780 面板，触控对齐；UIKit 使用缩放后的 1125×2436 画布 |
| Wi-Fi | 尚不可用 | PCIe、BHI/BHIE、AMSS 和 QMI 资源请求已实测；当前等待固件就绪通知超时，扫描尚未运行 |
| GPU 加速、音频、蜂窝网络与 DRM | 尚未验证 | 尚无完整实现或实机验证 |

显示旋转、电源状态切换和应用兼容性仍需验证。Wi-Fi 关联、认证及网络数据传输尚未实现。

详细证据与待办事项见[开发状态](docs/DEVELOPMENT_STATUS.md)及[固件传输测试记录](hardware/sm8650-wifi-root-master.json)。

## 环境要求

- UEFI 构建与本地镜像打包使用 Linux 或 WSL 中的 Ubuntu。
- Git，以及支持 `venv` 的 Python 3.11 及以上版本。
- Clang/LLVM、LLD、build-essential、NASM、`uuid-dev`、`gcc-aarch64-linux-gnu`、ACPICA 工具和设备树编译器。
- 上游 NuGet 工具所需的 Mono 及证书软件包；FAT 负载打包所需的 Mtools。
- XNU 使用 macOS 构建机，当前配置的 GitHub Actions 构建账号为 `jasper2863`。

上游版本由 [`sources.json`](sources.json) 及各目标工作流固定。内核、设备树、固件和负载元数据必须配套使用。

已提交的 UEFI 与 UniXNU 开发源码快照同时以 Git 子模块记录在 `sources/` 下。拉取所记录的版本：

```bash
git submodule update --init sources/uefi-giulia sources/UniXNU
```

## 构建

### UEFI

在仓库根目录执行：

```bash
bash scripts/build-uefi.sh "$HOME/ace5-port"
```

输出路径为 `$HOME/ace5-port/uefi-giulia/Mu-giulia-0.img`。首次构建会下载上游依赖。该命令不刷写手机。

### iOS 16 内核

```bash
gh workflow run ios16-xnu.yml --repo jasper2863/UniXNU-giulia \
  --ref main -f profile=gui -f touch=true -f wifi=true
```

工作流生成 `ace5-ios16-xnu` 产物。GUI 打包还需要经过验证的本地产品 ADT、根卷报告、信任缓存及匹配的 WLAN 固件。固件在 CI 完成后于本地嵌入，不随内核源码上传。

[`scripts/build-ios16-wifi-bringup.sh`](scripts/build-ios16-wifi-bringup.sh) 使用上述输入及已准备好的 UEFI 源码树组装、校验整合镜像。无参数运行可查看所需路径。每次构建应使用新的输出目录。

## 验证

USB 交互调试：在 Windows CMD 运行 `scripts\debug-terminal.cmd`。协议 2 诊断、MMIO 访问和 `reboot.fastboot` 已通过实机测试；PTY shell 仍需另行安装守护进程与已签名的 ARM64 shell/工具包，尚未实机验证。部署步骤及命令见 [USB 终端说明](docs/USB_TERMINAL.md)。

```bash
git diff --check
python3 -m unittest discover -s tests -v
bash scripts/test-ios16-wifi-runtime.sh
```

运行时检查覆盖 PCI 配置、DMA 隔离、固件传输、扫描协议及显示边界。这些属于主机端测试，不能证明无线链路已工作。编译结果、镜像校验与实机结果应分别记录。

## 仓库结构

| 路径 | 用途 |
| --- | --- |
| `board.toml`、`configs/`、`targets/` | 板级及目标配置 |
| `sources.json`、`patches/`、`uefi-overlay/` | 上游版本锁定与 UEFI 集成 |
| `sources/uefi-giulia`、`sources/UniXNU` | 以 Git 子模块固定版本的开发源码快照 |
| `ios16/` | Darwin 22 驱动及兼容性补丁 |
| `scripts/`、`tests/` | 构建、打包、诊断及测试工具 |
| `hardware/`、`docs/` | 验证记录与开发文档 |
| `.github/workflows/` | 手动触发的 CI 工作流 |
| `out/`、`.cache/`、`reports/` | 本地产物，不纳入版本控制 |

## 参与贡献

请遵循 [`CONTRIBUTING.md`](CONTRIBUTING.md)。提交内容应说明受影响的子系统、可复现的检查、预期与实际行为，并关联测试镜像或源码提交。日志需脱敏，专有固件、凭据及设备唯一信息不得提交。

## 许可证

仓库代码采用 [BSD-2-Clause-Patent](LICENSE) 许可证。上游组件与固件保留各自的许可证。
