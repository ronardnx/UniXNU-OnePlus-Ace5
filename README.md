# UniXNU giulia — OnePlus Ace 5 UEFI bring-up

当前阶段：构建并验证包含 UniXNU 菜单、DarwinBoot 的 UEFI 测试镜像。
不包含 XNU 内核、IPSW 或系统卷，不代表 iOS/macOS 已适配。

## 已验证状态（2026-10-21）

- Ubuntu 24.04 GitHub Actions 构建及镜像结构检查通过。
- 下载到本机后复核通过，镜像大小 2,179,072 字节。
- 原厂 Fastboot 接受临时启动，用户确认屏幕出现 **UniXNU 菜单**。
- 用户确认音量上、下键可以正常移动菜单选中项。
- 存储可用性和 DarwinBoot/XNU 交接尚待分别验证。
- 本次使用 `fastboot boot`，没有执行刷写命令或修改 userdata。

构建源码提交：`8277a88371e0fd6a64cc852652583ac41b5a4609`。
构建记录：https://github.com/tycykp/UniXNU-giulia/actions/runs/39508142284

镜像 SHA256：
`48d659de8ca9c8821a21cea440484ff8c190dfc82aee8c48f95a685e8f0fcb1a`

当前 WSL 的 Ubuntu 26.04 在 Mono/NuGet 准备工具时遇到
`System.Native` 运行库错误，因此本轮使用私有仓库 Actions 构建。

设备为 PKG110 / SM8650，当前 Android 16 固件为
PKG110_16.0.3.500(CN01)。设备使用 KernelSU，BL 属性经过隐藏，
不能以 Android 的 locked/green 属性判断真实解锁状态。

## 源码与改动

`sources.json` 固定 Mu-Silicium、UniXNU 和 DarwinBoot 的版本。
使用上游 `giulia` 的 model 0（Ace 5），不是 model 1（13R）。
`patches/giulia-unixnu.patch` 在 DSC/FDF 中加入 UniXNU 菜单、注册驱动和
DarwinBoot，并修正上游 SMBIOS 的 PKG110 型号拼写。
启动头的 OS/补丁字段匹配当前原厂 boot.img（14.0.0 / 2026-01）；
这描述 GKI 启动头，不是手机设置中的 Android 系统版本。
保持上游板级内存图和驱动，先验证 UEFI 基础能力。

测试镜像使用上游附带的 giulia DTB；实机 DTB 只在本地进行对比。
手机的完整属性、序列号、原始分区及设备转储不应提交到源码仓库。

## Linux / Ubuntu WSL 构建

依赖：git、python3、python3-venv、build-essential、clang、llvm、lld、
uuid-dev、nasm、gcc-aarch64-linux-gnu、acpica-tools、device-tree-compiler。
上游 NuGet 工具下载还需要 mono-devel 和 ca-certificates-mono。

```bash
bash scripts/build-uefi.sh "$HOME/ace5-port"
```

输出：`$HOME/ace5-port/uefi-giulia/Mu-giulia-0.img`。
构建脚本不会调用 adb/fastboot，不会刷写手机。
上游 setup 会递归下载子模块，首次执行需要较长时间。

```bash
python3 scripts/check-device.py /path/to/running-device.dtb \
  "$HOME/ace5-port/uefi-giulia/Resources/DTBs/giulia.dtb" \
  --output reports/device-check.json
```

## 验证边界

`verify-image.py` 检查 Android v4 头、压缩完整性、BootShim ARM64 标识、
固件卷大小和字节一致性、追加 DTB 及镜像 SHA256。
通过这些检查仅表示镜像结构正确，实机显示、侧键和存储仍需验证。
UEFI 含有磁盘访问能力，临时引导本身不等于所有菜单操作都无写入。
首次实机测试只检查菜单、显示和按键；不要执行分区、格式化、刷写操作。

## 后续阶段

1. UEFI 菜单与实机启动日志。
2. 负载卷与 DarwinBoot 交接验证。
3. 准备 macOS/XNU 构建环境，添加 Ace 5 的 ADT、内核配置和平台驱动。
4. 在明确备份与分区方案后评估系统卷；本阶段不处理 userdata。

`board.toml` 和 `giulia_defconfig` 提供第一轮单核 XNU 构建配置。
`make-giulia-adt.py` 使用实机的 GIC、CPU0 与定时器参数；不发布 UFS/GPU 节点。
目标 kernelcache 沿用上游验证过的 iPad7,1 / 17.7.11 / 21H461 ABI，
这些 Apple 内核参数不代表借用了 sheng 的硬件驱动。

`xnu.yml` 在 macOS Actions 构建内核和 FAT 负载，尚未经过内核实机验证。
此负载没有 OS/SystemOS/Data 卷；不能启动到用户界面。
系统卷和 SM8650 存储/GPU/触控适配继续独立推进，不使用 sheng 的设备参数。
