# UniXNU giulia — OnePlus Ace 5 UEFI bring-up

当前实机调试使用 Darwin 22.6.0 / iOS 16 内核。已验证 UEFI 交接、XNU
内存管理初始化、平台驱动注册、CPU0 注册及 SGI1 自测、原生 UFS 读取、
USB CDC 调试串口、userdata 的 HFSX 根卷读写挂载，以及 launchd 和系统服务启动。
GUI 内核已显示苹果标志与进度条；串口观察到 7.7 万次画面提交，SpringBoard / backboardd 未见崩溃。
进度条停在约 80%，尚未进入可操作界面；正在采样迁移服务，并适配物理触摸。

## 图形版构建产物（2026-10-21）

- 私有构建仓库：`jasper2863/UniXNU-giulia`，本地 remote 为 `build-jasper`。
- GUI kernel run `39583173365` 已成功，源码提交 `8859fc4`；构建、图形服务检查、内核重定位检查全部通过。
- Artifact `12339141121` 已下载并核验 GitHub SHA256 / ZIP CRC；内核 SHA256 为 `9271cb37d8a5602f425a14787dbc3bcdc1919ce9e0683177f4a00ad2b19d04a8`。
- 配套临时启动镜像：`out/ios16/gui-boot-39583173365/giulia-ios16-gui.img`，SHA256 `1fcdeffa431fd8986f7518abb02f31506411be1f9aab5525dc27af70271e5971`。
- 已核验 GUI ADT / 平台与 CPU 驱动匹配、DRAM 交接、根卷标识、Android 启动头和负载校验；实机已确认首次呈现，测试记录见 `hardware/ios16-gui-test.json`。
- 系统入口使用 `sweetfb_hz=30 sweetfb_noconsole=1`，首次呈现后才静默普通屏幕日志；USB 和 panic 日志继续保留。
- 后续下载新仓库产物使用 `scripts/fetch-action-artifact.py --repo jasper2863/UniXNU-giulia ...`。账号凭据仅保存在本机登录会话／凭据库，不在源码中。

## 触摸与服务采样

- `prepare-ios16.py --gui --touch` 加入 Ace 5 的 GENI SPI / TouchCom v1 驱动；`make-ios16-payload.py --enable-touch --trace-services` 启动驱动并扩展迁移服务采样。
- 驱动先验证时钟、SPI 固件、FIFO、DMA 空闲和引脚状态，再识别触控应用、切到 LBP 并读回触摸报文配置；收到有效报文才发 HID 事件。
- ClockDxe 的触摸时钟交接已实机验证。控制器回报 GPI-only（FIFO 禁用）；新增 `--touch-gpio` 调试后端使用已确认的 GPIO 模拟 SPI，不配置 DMA/SMMU。GPIO 后端尚待实机验证。
- 当前未实现 PMIC 电源恢复、GPI DMA 传输及 TouchCom v2/CRC 扩展；尚不能认定物理触摸已可用。
- 迁移采样已捕获 `com.apple.datami` / `com.apple.migrat`：前者等待插件包装进程，后者等待插件的信号量。已加入插件加载地址诊断，供下一轮定位。
- 寄存器来源、引脚和限制见 `hardware/sm8650-touch.json`。触摸解析测试不代替实机测试。

## 根卷和用户态进展（2026-10-21）

- 用户自行刷入已校验的 userdata 原始镜像后，实机确认 GPT、卷标识匹配以及 HFSX `iOSRoot` 读写挂载。
- `OS_REASON_EXEC / 0xe` 是 `WRONG_PLATFORM`。已通过开发内核自带的 `unentitled_ios_sim_launch=1` 解决，没有修改 launchd、dyld 或关闭代码页哈希校验。
- 原始任务缓存加载器在没有 AMFI IOService 时退出；`launchd_unsecure_cache=1` 已通过实机验证，随后开始启动系统服务。
- 此前基础内核镜像：`out/ios16/root-job-cache/giulia-ios16-root-job-cache.img`；SHA256 `e62850f45f2853010a407a4b5c0359528a831c03fee202bc19cfe25715a456f9`。
- 完整本地日志：`out/ios16/root-job-cache/usb-replay.log`。进度与签名页审计分别见 `hardware/ios16-root-mount-test.json` 和 `hardware/ios16-launchd-platform-audit.json`。
- 当前基础内核没有编入 IOSurface / IOMobileFramebuffer / HID 服务。已准备可选 `--gui` 源码配置及 `make-ios16-adt.py --gui --product-adt LOCAL_D22AP.adt`，实际物理屏幕限制为 `0xd5100000 / 1264×2780 / stride 5056`，UIKit 兼容画布为 `1125×2436`。
- 本地通过了 65 个补丁格式检查、8 个实际屏幕几何限制表达式测试、基础配置回归，以及 GUI ADT 与旧内核误配拒绝检查；GUI 内核整体编译和实机首次呈现现已完成。
- `ios16-xnu.yml` 选择 `profile=gui` 会构建图形内核，随后需在本地结合已校验的产品 ADT 打包。GUI 构建不自动使用基础 ADT 生成启动镜像。
- 旧仓库 GUI run `39581534474` 曾在编译启动前因账号付款／消费限额被 GitHub 拦截；已在上述新私仓完成构建。
- 物理触控、GPU 加速和稳定图形会话均未验证。当前已验证结果不等于完整 iOS 移植完成。

## 当前实机结果（2026-10-21）

- 修复低 DDR 保留区扫描、DRAM 设备树交接参数和平台驱动匹配。
- 启动镜像：`out/ios16/platform-match/giulia-ios16-platform-fixed.img`。
- SHA256：`e1692b6d31d9aa1d067c26535c8a20757acc8969d0e8fcff33eafbfde1beab1f`。
- 源码提交 `6625ca0`；本地 WSL 构建 UEFI，复用 run `39565899829` 的内核。
- USB 串口 `ACE5DBG001` / COM21 已完成 `replay`、`status` 双向通信；状态查询报告 `dropped=0`。
- UFS 读取 8192 字节成功，CRC32 `c319db94` 与 Android 基线一致，探测返回 0。
- 当前启动项 `giulia_ufs_probe=1` 会在探测后保持调度器运行，等待 USB 调试命令。
- 本轮仅使用 `fastboot boot`，未刷写 userdata；未尝试挂载未准备好的系统根卷。
- 详细证据、边界及日志 SHA256 见 `hardware/ios16-bringup-verified.json`；完整串口日志保存在本机 `out/ios16/platform-match/`。

当前内核仍沿用 `SweetPlatformExpert` 的历史匹配名，设备树在保留
`oneplus,giulia`、`qcom,sm8650` 和 Ace 5 型号的同时提供兼容项。
后续内核源码配置已改为原生 Ace 5 匹配名；打包时会核对实际内核中的平台和 CPU 匹配规则。

以下为早期 iPadOS 17.7.11 / Darwin 23 路线的构建与测试记录，不能代替上述当前状态。

## 早期 UEFI 验证状态（2026-10-21）

- Ubuntu 24.04 GitHub Actions 构建及镜像结构检查通过。
- 下载到本机后复核通过，镜像大小 2,179,072 字节。
- 原厂 Fastboot 接受临时启动，用户确认屏幕出现 **UniXNU 菜单**。
- 用户确认音量上、下键可以正常移动菜单选中项。
- 实机 `initrd S14 ... Success`：定位到校验匹配的 FAT 负载，并复制到 UEFI 分配的内存。
- 实际加载 `\\EFI\\UniXNU\\unixnu.toml`，显示 Ace 5 XNU 启动项。
- 截图中的绿色条由 `DarwinEnter` 在 `ExitBootServices` 成功后绘制，证明进入加载器交接代码。
- 绿色条在跳转 XNU 前绘制，不能据此认定 XNU 内核已执行。
- 原生 UFS 系统盘访问、内核执行和用户态仍待验证。
- 本次使用 `fastboot boot`，没有执行刷写命令或修改 userdata。

构建源码提交：`8277a88371e0fd6a64cc852652583ac41b5a4609`。
构建记录：https://github.com/tycykp/UniXNU-giulia/actions/runs/39508142284

镜像 SHA256：
`48d659de8ca9c8821a21cea440484ff8c190dfc82aee8c48f95a685e8f0fcb1a`

当前 WSL 的 Ubuntu 26.04 在 Mono/NuGet 准备工具时遇到
`System.Native` 运行库错误，因此本轮使用私有仓库 Actions 构建。

## XNU 与系统卷进度

macOS Actions run `39511969648` 已成功构建 ArmDTPlatform、ArmCpu、
corecrypto、pthread、预链接 XNU 及 192 MiB FAT 负载。
内核 SHA256：`5de566bab4ffbcf7b67905249fdeff50037996bb6f22bfe20b446fab48d2e5f6`。
这是离线构建成功，不代表内核已在手机上执行。

启动器传来的 DT 保留旧 Android initrd 信息，因此仅按它的地址定位失败。
当前版本在 UEFI 声明的普通 RAM 中查找指定大小和 CRC 的 FAT 负载，
复制后再次校验并发布为只读块设备。扫描排除 MMIO、运行时及未知保留区。
CRC 用来排除意外的旧数据，不替代内核或系统签名校验。
`GiuliaInitrdStatus` 是易失诊断变量，不写永久 UEFI 变量。

已验证的构建：UEFI run `39516550749`，XNU run `39511969648`。
完整临时启动镜像 SHA256：
`c34cb52e6d62920e0a96d4f39d08a4abbac8fd9e3774b5c41dbbaac781bd7bf2`。
本机基线位于 `out/verified-uefi/`，仅用于 `fastboot boot`，不刷 boot 分区。
详细观察结果见 `hardware/uefi-verified.json`。

OS/SystemOS 文件已按 `board.toml` 哈希校验并组装为 9,382,658,048 字节
系统映像。原始 OS 的 GPT CRC、SystemOS 的 APFS 超级块校验通过。
映像仍未通过实机存储验证，也未刷写 userdata。
SM8650 存储寄存器证据在 `hardware/sm8650-ufs.json`。

Data 卷 run `39521758751` 已成功。32 MiB APFS 种子已下载到本机，
SHA256、容器大小和超级块校验均通过，包含原始初始 var 树的 39 个文件。
卷 UUID 为 `9E4C0509-9215-4EC5-872F-0254D84A223C`，验证记录见
`hardware/data-seed.json`。
现已把原始 Data 种子作为 dense `RAMDisk` 加入 FAT 负载，设置 `initrd` 路径、
ADT `RAMDisk` 占位项和易失 Data 属性；现有内核包含标准 memdev 支持。
本轮未开启依赖未完成根卷链路的自动 Data 挂载，也未引入尚未资格验证的存储类绑定。
内核/ADT/Data 所需交接内存为 103,743,488 字节，位于 256 MiB 窗口内。

UEFI run `39526260528` 与新负载匹配。实机截图确认 Data 启动项、S14 Success、
DarwinBoot inputs validated 和绿色交接标记；XNU 执行、Data 设备注册及挂载仍未确认。
验证记录在 `hardware/data-handoff-verified.json`，本机临时启动镜像在
`out/verified-data-handoff/`。这是内存 Data，重启不保存运行时改动。
后续 XNU Actions 会下载固定的 Data 种子 artifact，生成并核验 `giulia-xnu-data`
负载；对应 `data-uefi-identity.json` 必须随负载一起用于下一次 UEFI 身份绑定。
同一轮已保存约 5.14 GB 的 OS 固件缓存，后续匹配缓存键的 Data 任务可复用。

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

Actions 固件缓存：`data-seed.yml` 缓存经过校验的 OS 组件，`xnu.yml`
缓存原始/解压 kernelcache。缓存键只依赖 IPSW 身份、下载地址和固件校验值，
修改驱动、设备树或 userdata 配置不会导致重新下载。
OS 下载完成就保存缓存，即使之后制作 Data 卷失败也能复用。
命中缓存后仍校验实际文件 SHA256；有效 OS/解压内核缓存可完全跳过对应的 CDN 请求。

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
