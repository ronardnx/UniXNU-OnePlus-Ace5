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
| 电源与音量键 | 按键最小镜像已实测 | 电源键可熄屏并恢复亮屏，音量界面有响应；见[验证记录](docs/DISPLAY_POWER.md) |
| 三段式开关 | 尚未验证 | AK09970 驱动已编译，传感器读数与三个档位标定待完成 |
| Wi-Fi | 联网尚未实测 | PCIe、BHI/BHIE、AMSS 和 QMI 资源请求已实测；最新实机运行在固件就绪之前的 QMP 打开阶段停止；关联、WPA2 与数据通路主机测试通过 |
| GPU | 已接入准备服务 | A750 固件／RAM 准备、未激活 SMMU 页表及门控观察；尚无 GPU 执行与 Metal 加速 |
| 音频 | 已加入实验性运行时核心 | 固定固件 ADSP／PAS、GLINK／GPR、APM 和双 SIA9177 代码；原生资源／PCM provider 未完成，尚无实机播放或录音验证 |
| 蜂窝网络与 DRM | 尚未验证 | 尚无完整实现或实机验证 |

显示旋转、完整系统休眠／恢复和应用兼容性仍需验证；物理电源键的面板熄屏／亮屏已在独立按键最小镜像中实测。Wi-Fi 已加入单站关联、WPA2/WPA3-SAE、PMF、定向与后台扫描、基本漫游、REO 队列回收和 Ethernet 传输代码；本轮没有构建新内核，也没有实机验证这些改动。实现与限制见 [Wi-Fi 联网](docs/WIFI_NETWORK.md)，启动证据见 [固件就绪记录](docs/WIFI_FIRMWARE_READY.md)。

详细证据与待办事项见[开发状态](docs/DEVELOPMENT_STATUS.md)、[音频驱动进展](docs/AUDIO_BRINGUP.md)及[固件传输测试记录](hardware/sm8650-wifi-root-master.json)。

2026-10-24 固件启动链审计：已补齐 MHI 前的板级 PDC 默认配置，以及 HOST_CAP 中的 WLAN_EN 能力与实际 SMMU IOVA 区间。主机回归通过；仍需新镜像和实机采集确认 `FW_INIT_DONE`。见[启动链修复记录](docs/WIFI_FIRMWARE_READY.md)。

后续[上游调用链审计](docs/WIFI_UPSTREAM_CHAIN.md)已把供电、PCIe/MHI、QMI、就绪与释放流程对应到一加固定源码，补上注册状态处理、固件引脚结果和 `wifi.status` 启动快照。冷启动校准、RDDM／恢复和休眠恢复仍未完整实现；尚无新的实机验证。

基带的硬件资源、固件启动、SIM/数据协议与 iOS 接口依赖见[完整接入映射](docs/MODEM_BRINGUP.md)。统一生命周期、共享资源仲裁、QMI/QMUX/QMAP 与 20H392 ABI 映射共用构建和打包契约。原生硬件 provider 和 managed 系统接入仍按契约推进。

## 环境要求

- UEFI 构建与本地镜像打包使用 Linux 或 WSL 中的 Ubuntu。
- Git，以及支持 `venv` 的 Python 3.11 及以上版本。
- Clang/LLVM、LLD、build-essential、NASM、`uuid-dev`、`gcc-aarch64-linux-gnu`、ACPICA 工具和设备树编译器。
- 上游 NuGet 工具所需的 Mono 及证书软件包；FAT 负载打包所需的 Mtools。


上游版本由 [`sources.json`](sources.json) 及各目标工作流固定。内核、设备树、固件和负载元数据必须配套使用。

GUI 内核工作流支持同时选择触控、Wi-Fi 和 `gpu=true`。本地统一打包通过 `GIULIA_GPU_PACK` 与 `GIULIA_GPU_MODE=1/2` 启用 GPU，USB `gpu.status`／`gpu.observe` 提供准备状态和门控观察。输入及硬件待办见 [GPU 适配说明](docs/GPU_BRINGUP.md)，分层顺序、门控与里程碑见 [GPU 接入框架](docs/GPU_STACK_FRAMEWORK.md)。

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
gh workflow run ios16-xnu.yml --repo tycykp/UniXNU-OnePlus-Ace5 \
  --ref main -f profile=gui -f touch=true -f wifi=true
```

工作流生成 `ace5-ios16-xnu` 产物。GUI 打包还需要经过验证的本地产品 ADT、根卷报告、信任缓存及匹配的 WLAN 固件。固件在 CI 完成后于本地嵌入，不随内核源码上传。

[`scripts/build-ios16-wifi-bringup.sh`](scripts/build-ios16-wifi-bringup.sh) 使用上述输入及已准备好的 UEFI 源码树组装、校验整合镜像。无参数运行可查看所需路径。每次构建应使用新的输出目录。

构建新 userdata 时，可向 `scripts/rebuild-ios16-userdata.py` 传入 `--skip-setup` 跳过开机设置／激活引导页面。开关默认关闭；省略参数或传 `--no-skip-setup` 时保留正常向导。构建命令与验证说明见[开机向导开关](docs/SETUP_SWITCH.md)。

## 验证

USB 交互调试：在 Windows CMD 运行 `scripts\debug-terminal.cmd`。协议 2 诊断、MMIO 访问和 `reboot.fastboot` 已通过实机测试；PTY shell 仍需另行安装守护进程与已签名的 ARM64 shell/工具包，尚未实机验证。部署步骤及命令见 [USB 终端说明](docs/USB_TERMINAL.md)。

```bash
git diff --check
```

构建和打包命令会核验输入与生成的产物。编译结果、镜像校验与实机结果应分别记录；硬件支持必须有实机证据。

Wi-Fi 的 MMIO、CPU/物理地址/IOVA、资源释放及 iOS 请求映射范围见 [Wi-Fi 映射层](docs/WIFI_MAPPING.md)。

GPU/GMU 权限、固件分配生命周期、HFI 资源覆盖及尚缺的专用后端见 [GPU 映射层](docs/GPU_MAPPING.md)。

## 仓库结构

驱动源码已按职责拆分到 `ios16/` 下的 13 个模块目录。共享资源仲裁位于平台层，可选驱动与输入启动注册由统一装配入口处理。目录职责、依赖规则及检查命令见[驱动模块规范](docs/DRIVER_MODULES.md)。

| 路径 | 用途 |
| --- | --- |
| `board.toml`、`configs/`、`targets/` | 板级及目标配置 |
| `sources.json`、`patches/`、`uefi-overlay/` | 上游版本锁定与 UEFI 集成 |
| `sources/uefi-giulia`、`sources/UniXNU` | 以 Git 子模块固定版本的开发源码快照 |
| `ios16/` | Darwin 22 驱动及兼容性补丁 |
| `scripts/` | 构建、打包及诊断工具 |
| `hardware/`、`docs/` | 验证记录与开发文档 |
| `.github/workflows/` | 手动触发的 CI 工作流 |
| `out/`、`.cache/`、`reports/` | 本地产物，不纳入版本控制 |

## 参与贡献

请遵循 [`CONTRIBUTING.md`](CONTRIBUTING.md)。提交内容应说明受影响的子系统、可复现的检查、预期与实际行为，并关联测试镜像或源码提交。日志需脱敏，专有固件、凭据及设备唯一信息不得提交。

## 许可证

仓库代码采用 [BSD-2-Clause-Patent](LICENSE) 许可证。上游组件与固件保留各自的许可证。
