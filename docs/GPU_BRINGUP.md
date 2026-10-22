# Ace 5 原生 GPU 适配

目标为 Darwin 22 / iOS 16 的原生 GPU 加速，最终由 Metal、IOSurface 和
系统 render server 合成界面。已接入 Darwin 22 的 `GiuliaGPU` 准备服务、
本地固件段和 GMU 主机内存分配，并提供有前置条件的只读电源观察。
**尚未接通 GPU 上电、SMMU 映射、GMU 执行、命令提交和 Metal。**

## 已核实的硬件输入

`hardware/sm8650-gpu.json` 固定捕获设备树的 SHA256、13 份 OnePlus 源码的
Git blob 与 SHA256、四份原厂固件的 SHA256，以及参考 UniXNU 子模块版本。
型号来自设备树，GMEM 和调度器信息来自固定版本的 `adreno-gpulist.h`，
不代表已经读取过上电后的 GPU 寄存器。

| 项目 | Ace 5 配置 |
| --- | --- |
| GPU | Adreno750v2，chip ID `0x43051401`，`qcom,adreno-gpu-gen7-9-1` |
| GPU MMIO | `0x03d00000` / `0x40000` |
| GMU MMIO | `0x03d68000` / `0x37000`；不要把节点名中的 `3d69000` 当作 reg 地址 |
| GPU SMMU | `0x03da0000` / `0x40000`，与 apps SMMU 不同 |
| SMMU stream ID | user=0、LPAC=1、secure=2、GMU=5；这些不是 context bank 编号 |
| GPU / GMU SPI | 300 / 304、305；设备树值未加 GIC 的 SPI 偏移 32 |
| GMEM | 3 MiB，UCHE GMEM 对齐 16 MiB |
| 原厂调度器 | `adreno_gen7_9_0_hwsched_gpudev` |
| 原厂固件 | `gmu_gen70900.bin`、`gen70900_sqe.fw`、`gen70900_aqe.fw`、`gen70900_zap.mbn` |
| CX 硬件状态 | GPUCC `+0x9168`，由 CX GDSC 的 `hw-ctrl-addr` 引用；不是 sheng 的 `+0x953c` |

固件来自本地 PKG110 `16.0.3.500(CN01)` 的 vendor 镜像。
固件正文保存在忽略目录 `out/`，不进入 Git。

## 已实现代码

- `scripts/ios16_gpu.py`：核对板级身份、按名称提取 MMIO 窗口、解析 GPU
  SMMU 路由、校验设备树/源码/固件摘要。不访问硬件。
- `scripts/fetch-ios16-gpu-sources.py`：获取固定版本源码并验证 Git blob 和 SHA256。
- `scripts/extract-ios16-gpu-firmware.py`：从本地 EROFS 提取四份固件，全部通过
  摘要校验后才写出；不同版本的已有输出会被拒绝。
- `ios16/giulia_gpu_firmware.h`：独立 C/C++ 固件解析器，保留预分配、持久
  预分配和版本元数据；校验长度、对齐、GMU 虚拟地址范围、数据段重叠和顺序。
  出错不写入输出条目，返回条目数清零。SQE/AQE 按原厂规则跳过首个文件 dword。

真实 GMU 固件有 15 个块：11 个元数据块和 4 个数据块。两块持久内存申请
位于 GMU 虚拟地址 `0x60000000` 和 `0x60001000`，各 4 KiB。
这些地址不能作为宿主物理地址使用。解析器现已通过 `--gpu` 接入内核构建。
`giulia_gpu_memory.h` 将非 TCM 申请按 4 KiB 对齐并合并重叠/相邻区间，
保留持久属性，要求每个非 TCM 数据段都被申请范围覆盖。原厂固件生成五段
GMU 虚拟地址预留；`GiuliaGPU.cpp` 分配独立的连续物理内存，检查其位于
实际 XNU 启动内存中，清零后复制普通 RAM 代码段。尚未写 TCM 或建立 GPU 映射。

`make-gpu-firmware-pack.py` 生成 221184 字节的确定性容器。内核只包含其
SHA256，打包时才通过 `embed-gpu-firmware.py` 加入只读 `__GPU_FW` 段。
服务显式映射该段对应的物理 RAM，再检查整个容器和四个成员的摘要；
不能假定 XNU 已为自定义段建立静态虚拟映射。GPU 与 Wi-Fi 固件段可共存。

服务仅匹配 Ace 5 GUI 平台，并由 `giulia_gpu` 启动参数启用：

| 参数 | 行为 |
| --- | --- |
| 未指定 | 不启动 GPU 服务 |
| `giulia_gpu=1` | 校验固件，分配并准备主机 RAM，不访问 GPU MMIO |
| `giulia_gpu=2` | 在模式 1 后观察 GCC；仅在 CFG 时钟、CX 状态和相关分支逐级满足条件时继续读取 GPUCC 和 SMMU 全局状态 |

模式 2 不使能任何时钟或电源，不写 SMMU，也不读取 GX GPU 寄存器。
观察结果通过 USB 日志及 IORegistry 的 `PowerObservation` 输出。
服务始终声明 `HardwareReady=false`，不发布 Metal 或 AGX 加速器。
启动失败或服务停止时释放当前阶段的主机内存和 CPU 映射；这仅适用于当前
尚无 GPU DMA 的阶段，后续硬件接管必须加入停机及失效确认后才能释放。

## 参考栈的适配缺口

不能只开启 `CONFIG_XNU_NATIVE_ADRENO`。当前已锁定的参考版本存在以下差异：

1. NativeAdrenoService 使用 `ShengGmu*` 平台启动代码，并调用
   `NativeAdrenoPwrupList::buildA740`。需实现 SM8650 电源/时钟/RPMh、
   有超时的复位和失败恢复，再接 gen7-9-1 的硬件初始化。
2. 需要独立接管 GPU SMMU、分配 4 KiB 页表与 GMU 内存，并确定实际空闲
   context bank。不能把 SID 当作 bank，也不能沿用 sheng 的静态映射。
3. 需要接入 ZAP 安全固件加载/认证及 GMU/HFI 协议，确认 A750 的
   SQE/AQE 与调度路径。原厂使用硬件调度器，参考 A740 的 CP ring 路径
   能否直接移植尚未证明。
4. `turnip-darwin-kmd.patch` 仅为 `0x43050a01` 特判 GPU ID 740；
   其通用字节公式不能正确解释 `0x43051401`。接入时需同时报告实际
   chip ID、750、GMEM 大小/基址，并重新生成绑定目标 GPU 的 shader cache。
5. `build-turnip-darwin.py` 与 `build-adreno-metal-plugin.py` 当前目标为
   `arm64-apple-ios17.0`。iOS 16 的编译、链接、Metal 私有接口及设备发布
   必须单独检查，不能以修改 MinimumOSVersion 代替兼容性验证。
6. 当前 Ace 5 显示路径需要接入真实 fence、IOSurface 生命周期和 scanout
   同步；之后才可向 backboardd 发布加速设备并测试系统合成。

## 复现离线验证

在仓库根目录执行。以下固件提取与 C/C++ 检查使用 Linux/WSL：

```bash
python3 scripts/fetch-ios16-gpu-sources.py --out .cache/gpu-source
python3 scripts/extract-ios16-gpu-firmware.py \
  --vendor-erofs .cache/wifi-stock-16.0.3.500/vendor.raw \
  --out out/ios16/gpu-stock-firmware
python3 scripts/make-gpu-firmware-pack.py \
  --firmware-dir out/ios16/gpu-stock-firmware \
  --out out/ios16/gpu-stock-firmware/firmware.pack
python3 scripts/check-ios16-gpu-inputs.py \
  --dts ../ace5-analysis/running-device.dts \
  --source-dir .cache/gpu-source \
  --firmware-dir out/ios16/gpu-stock-firmware \
  --out out/ios16/gpu-stock-firmware/qualification.json
bash scripts/test-ios16-gpu.sh out/ios16/gpu-stock-firmware/gmu_gen70900.bin
python3 tests/ios16_gpu_embed.py out/ios16/gpu-stock-firmware/firmware.pack
python3 tests/ios16_gpu_patch.py
python3 -m unittest discover -s tests -v
git diff --check
```

2026-10-22：输入校验、真实固件解析、C/C++ 编译检查、ASan/UBSan
边界测试及 WSL 下 18 项 Python 测试通过。Windows Python 全量测试中的
5 项既有子进程测试报 WinError 2；同一套测试在 WSL 全部通过。
这些离线结果不证明 GPU 上电、命令执行、Metal 或系统合成可用。

下一阶段验收点：冷启动电源/时钟交接与 GPU SMMU 所有权可观察，随后
GMU HFI 握手、一次内存写入命令和真实 fence 完成；再推进渲染及系统合成。

## GUI、触控和 Wi-Fi 统一接入（2026-10-23）

主工作流 `ios16-xnu.yml` 增加 `gpu` 输入，可在同一份 GUI 内核中同时选择
`touch=true wifi=true gpu=true`。主构建与独立 GPU 构建共用链接检查，要求
服务、显示适配及电源／SMMU／GMU 控制函数和终端入口均为实际定义的符号。
固件正文仍只在本地嵌入，CI 产物不含固件。

统一打包沿用 `build-ios16-wifi-bringup.sh` 的九个参数，通过环境变量显式启用：

```bash
export GIULIA_GPU_PACK="$PWD/out/ios16/gpu-stock-firmware/firmware.pack"
export GIULIA_GPU_MODE=1   # 2 还会在启动后进行一次门控只读观察
bash scripts/build-ios16-wifi-bringup.sh \
  <prepared-uefi> <uefi-python> <unified-kernel.macho> <verified-gui.adt> \
  <root-report.json> <trust-catalog> <captured-board.dts> <new-output-directory> \
  <local-wifi-firmware.pack>
```

脚本在修改 UEFI 前验证内核摘要、两份固件 pin、两次段插入顺序及 Mach-O
头空间。先加入 `__WIFI_FW`，再加入 `__GPU_FW`，保留两者元数据并写入
`giulia_gpu=1/2`。包含 GPU 的内核缺少显式固件输入时拒绝打包，防止整合镜像
静默关闭服务。产物保留完整 GPU C/C++、头文件、ArmSmmu4K 来源及摘要。

USB 新增 `gpu.status` 和 `gpu.observe`。状态包含准备阶段、固件校验、GMU
RAM 与未激活页表数量、最近观察及有效字段位图。命令通过静态原子邮箱交给
保留服务引用的工作线程；USB 线程不访问 GPU MMIO，服务停机时等待观察结束
后才释放映射。模式 1 只有在显式执行 `gpu.observe` 时才读取门控寄存器。
不包含 GPU 的 base／GUI 配置提供 unavailable 响应，不引入未定义外部符号。

这些接入仍处于准备和观察阶段：电源租约、SMMU 激活、GMU 执行和 Metal
均未启用。Wi-Fi 的 AOP/QMP 传输当前只有单一所有者，GPU ACD 请求需要共享
提供者；UEFI Clock 协议也不能直接当作 XNU 运行时函数调用。后续应先完成
电源／时钟交接与所有权，再推进实际硬件启动。

## 内核与本地负载

```bash
python3 scripts/prepare-ios16.py --reference ../reference-vermeer \
  --work .cache/gpu-build-source --out out/ios16/gpu-build-profile --gui --touch --gpu
python3 scripts/make-gpu-firmware-pack.py \
  --firmware-dir out/ios16/gpu-stock-firmware \
  --out out/ios16/gpu-stock-firmware/firmware.pack
python3 scripts/embed-gpu-firmware.py --kernel <rebased-kernel.macho> \
  --pack out/ios16/gpu-stock-firmware/firmware.pack --out <new-kernel.macho>
```

`prepare-ios16.py` 生成 `0077-giulia-gpu-runtime.patch`，将服务加入
`iokit/conf/files.arm64` 和 `IOResources / IOBSD` 匹配表。
`.github/workflows/ios16-gpu.yml` 提供独立的 macOS 内核编译与链接检查。
本地 `make-ios16-payload.py` 增加 `--gpu-mode 1` 或 `--gpu-mode 2`；
必须同时提供经过验证的 GUI ADT、根卷报告和已嵌入固件的匹配内核。
`verify-payload.py` 重新核对固件段、内核摘要和启动参数。

2026-10-22 的内核构建
[39734691344](https://github.com/jasper2863/UniXNU-giulia/actions/runs/39734691344)
通过完整编译、GPU 服务链接检查和 DarwinBoot 重定位检查。
源码提交为 `1de7d6b98f1907ad69b04e00a96488d3d9f8e91c`；此处编译成功
仍不代表服务已在手机上运行。CI 不包含原厂 GPU 固件正文。

`scripts/build-ios16-gpu-bringup.sh` 使用独立、已准备好的 Darwin 22 UEFI
源码目录及已验证的内核、ADT、根卷报告、信任缓存和 GPU 固件包，完成
本地嵌入、FAT 负载、UEFI 编译与 boot v3 镜像校验。默认模式 1；设置
`GIULIA_GPU_MODE=2` 可加入只读电源观察。该脚本不操作手机，也不改写分区。

本地候选为 `out/ios16/gpu-observe-boot/giulia-ios16-gpu.img`，69,279,744 字节，
SHA256 `31ea6929402049c8e966e0e974e3b17767f24d96fd1a862561050c29ae2d4a99`。
它使用模式 2，保留全屏显示、触控和完整日志；不包含 Wi-Fi 运行时，
也不发起 Wi-Fi PCIe 初始化。已有 Wi-Fi 代码及其镜像未被覆盖。
用户要求先保留镜像、暂不实测，本次没有启动手机或刷写分区。
构建来源、检查结果和源码摘要见 `hardware/ios16-gpu-integration.json`。

## 电源、SMMU、GMU 离线控制核心

本轮新增控制接口及故障注入测试，不启动手机。原有模式 1/2 仍不执行
电源申请、SMMU 激活、TCM 写入或 GMU 解除复位。

- `giulia_gpu_power_control.h`：按原厂 HWSCHED 路径规划 CX、GMU 625 MHz、
  hub 200 MHz、相关时钟及 SMMU 时钟的有所有权资源申请。失败按反序回收；
  结果不确定、无法确认 DMA 停止或释放失败时保留资源并禁止自动重试。
  实际 AOP、时钟、电压父依赖和租约提供者仍需平台后端实现，未猜测寄存器投票值。
- `vendor/ArmSmmu4K`：固定提交的原生 4 KiB 页表/context/stream 实现，保留
  MIT 许可证及源码摘要。`GiuliaGPU` 现在可为五段 GMU RAM 建立 **未激活**
  的页表；原厂固件对应 6 个表页、104 个叶页，空洞保持未映射。
- `giulia_gpu_smmu.h`：只接受独立 GPU SMMU 的显式 bank/ASID/slot 租约和
  GMU SID 5。检查 fault-on-unmatched、其他 stream 冲突、上下文读回和
  有界 TLB 同步；先 fault stream，再停 context。不会选择 Wi-Fi bank、
  重写全局策略或接管 secure SID 2。ACTLR/security、fault handler、真实
  页表 cache 发布和租约提供者是必须实现的后端条件。
- `giulia_gpu_gmu.h`：完整固件包摘要检查、复位保持下的 TCM 写入与逐字读回、
  先验证普通 RAM 内容、拒绝陈旧 ready 状态，再执行 FW/HFI ready 有界等待。
  PDC/RSC、secure CX、IRQ/带宽及 HFI 队列配置必须由真实回调完成；缺少
  回调就拒绝执行。该接口只到 HFI transport-ready，尚不包含全部 HWSCHED
  feature/performance/bandwidth/start 消息及 ACK 流程。

公共接口在 `giulia_gpu_control.h`，内核 C 单元编译和导出它们；当前服务
只调用无 MMIO 的页表构建函数，没有接入任何写硬件的启动开关。
调试字段、错误码、主错误/清理错误及真实主机输出示例见
[GPU_DEBUG_LOGS.md](GPU_DEBUG_LOGS.md)。

离线测试使用 ASan/UBSan、软件寄存器模型、固定 Linux 原始 TCR helper
和独立页表遍历。原始 AArch64 MMIO 源码另做交叉汇编检查；主机内存边界
测试把 DSB 替换为主机内存屏障，**不验证总线、缓存一致性或硬件时序**。
旧候选 `gpu-observe-boot` 保留原样，仍对应上一轮提交，不包含本轮新增代码。

最终源码 `9f495f2e1a5ff3f17a906591bec641a1028dffd7` 的完整内核构建与
控制入口符号检查通过：
[run 39742622397](https://github.com/jasper2863/UniXNU-giulia/actions/runs/39742622397)。
本地 890 个控制场景、日志输出契约和独立页表遍历通过；详细边界、
摘要与未完成项见 `hardware/ios16-gpu-offline-control.json`。
本轮没有访问手机，没有重新打包或替换旧启动镜像。

## 统一候选验证（2026-10-23）

源码 `aa6cc82ec4f6b8080d73c2b8c94554f54ec5e0fa` 的完整 GUI＋触控＋
Wi-Fi＋GPU 内核构建通过：
[run 39843230533](https://github.com/jasper2863/UniXNU-giulia/actions/runs/39843230533)。
链接检查直接解析 Mach-O 外部符号、指令节和执行权限，确认八个 GPU 控制及
终端入口位于真实可执行代码中，支持 XNU 的 `__TEXT_EXEC` 布局。

本地统一候选为
`out/ios16/gpu-wifi-integrated-boot-20261023/giulia-ios16-wifi-pcie.img`，
69,287,936 字节，SHA256
`19f7b7c7f4d90202cb76fd31915b60b6ffbe03e7af9106451709e682aa36d694`。
默认 GPU 模式 1，保留开发版 ADT、现有 userdata、显示、触控、屏幕采集、
Wi-Fi 终端等待和全量日志。配套 `host-tools/` 使用同次 CI 的终端脚本。

固件、内核、FAT、根卷绑定、UEFI 及最终 boot v3 镜像布局校验通过。
固定源码快照中的 370 个文件和重新生成的 79 份补丁／构建脚本已与 CI
核对；构建期间其他聊天中的投屏改动未纳入本候选。完整本地记录为同目录
`delivery.json`，脱敏记录见
[`hardware/ios16-gpu-unified-integration.json`](../hardware/ios16-gpu-unified-integration.json)。

该候选尚未启动手机或刷写分区。GPU 仍未取得实际电源租约、激活 SMMU、
启动 GMU 或执行渲染命令，Metal 和系统 GPU 合成尚未实现。
