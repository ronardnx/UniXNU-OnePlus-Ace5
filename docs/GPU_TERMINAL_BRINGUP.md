# Ace 5 GPU 终端实测（2026-10-23）

## 2026-10-23 09:00 起：安全平台 ABI 2 集成

安全池、独立 secure 页表、固定 SCM/PAS 操作、secure SID 2 / bank 3、
RAW 提交的内核有界轮询快照，以及分段 HFI log 读取已统一加入。
这仍是诊断后端，没有发布 Metal accelerator；中断保持屏蔽，故障采集采用
轮询。接口及生命周期见 [GPU_PLATFORM_ABI.md](GPU_PLATFORM_ABI.md)。

首个集成提交 `a12e163ab9a1888b0987e13515a5d8d7f9a5f592` 的 CI
`39879914562` 成功，镜像 `1874909046085a09d2f1173856be718fb21a86b2c75536af21ac367932a2ee91`
经完整来源、产物 SHA256 和嵌入内容验证后临时启动。启动验收通过，熄屏完成。
09:04 的第一次 platform probe 在获取 4 MiB 连续内存时失败，日志明确为
`vm_page_find_contiguous(num=256): found 0 pages`。没有执行 SCM、所有权转移或
CP；没有出现 900E。09:11 独立清理确认 secure_owned=0、active=0，恢复保存的
寄存器并移除全部 13 个终端映射。

修复提交 `2624d8ef990061ed5f48f753da35da94af49fe76` 将安全池改为 512 个
64 KiB 物理分段，通过独立页表组成八个 4 MiB 连续 GPU VA 槽。每次最多
32 个 region 组成 2 MiB 批次，数据 16 次加页表 1 次所有权调用。实际方法
的宿主测试核对了不连续物理页的独立遍历、批次参数、CPU 访问拒绝和失败隔离。
修复版 CI `39882866383` 已通过；后续设备结论以新证据记录为准。

修复版已下载并通过 SHA256、源码快照、CI/local profile、固件嵌入及镜像布局
校验，镜像摘要为 `57bf4871f7bd268ece074fc7be981b6b613b9d4de829d847a44873ac4da350e0`。
09:34 自动进入 Fastboot 后，`fastboot boot` 返回成功；之后 100 秒内没有
CDC 枚举。随后的主机检查也未发现目标 USB、Fastboot 或 900E，所以只能记录
**启动后设备状态未知、需要现场确认**，不能将其写成已证明的 900E 或内核 panic。
已经停止进一步实机操作，并向用户询问当前物理状态。修复版尚未做平台
初始化、SCM 所有权转移、CP 提交或 Metal 验证。

修复版启动日志：`out/ios16/gpu-platform-device-20261023-r2/boot.json`；
打包验收：`hardware/ios16-gpu-platform-r2-integration.json`。

本轮用户新增约束：**一旦出现 USB 900E，立即停止设备操作并暂停对话，
不自动恢复或重放。** 主机在断连、未确认的命令超时或 SCM 待完成状态时
也停止本次会话的后续 I/O，保留原始捕获与未确认的清理状态。

证据：`hardware/ios16-gpu-platform-progress.json`；首次启动与失败在
`out/ios16/gpu-platform-device-20261023-r1/` 和
`out/ios16/gpu-native-hwsched-terminal/20261023-090455-480047/`；独立清理在
`out/ios16/gpu-platform-preassign-cleanup/20261023-091159-929023/`。

## 2026-10-23 08:06–08:20 续调：AQE 握手与安全分配阻塞

继续使用 `gpu-dev-boot-20261023-r1`；本轮没有编译内核、刷写或发送 RAW/CP
命令，没有再次进入 900E。当前 CDC 正常，最后的 display-off 命令完成，
DSI ACK error=0。**GPU 提交、fence、Metal 仍未通过。**

### 新实测结果

- 原厂 gen7-9-1 在 `CORE_FW_START` 后、`START` 前启用 AQE（feature 29）。
  主机脚本此前遗漏了此消息；补齐后得到 `value=0,status=0` ACK。
- AQE 启用后，START 在原有五块分配之外申请 kind 3 的 24 字节 SMMU
  record、kind 4 的 `0x37d000` 字节普通上下文，然后请求 kind 5 的安全上下文：
  `flags=0x1007, bytes=0x37d000 (3657728), gmu_handle=5`。
  当前接口拒绝该安全请求，未发送成功应答，START 因此没有完成。
- 单独在 AQE 前显式发送 PREEMPTION `[2,0,0]` 也得到成功 ACK，但仍收到
  同一安全请求。因此不能将关闭抢占视为已验证的绕过方法。
- 保留 `--without-aqe` 作为**不完整的寄存器基线诊断**。该路径的 START、
  GX 上电、配置、休眠通过；配置快照显示 `SECVID_TRUST_CNTL=1`、
  `CP_HW_FAULT=0`、GPU/GMU FSR=`0x400`，AO unmasked mirror=`1` 是 GPUIDLE。
  这不证明完整 AQE 初始化，更不证明 CP 已执行或安全模式已退出。
- 修正 PRR 前置条件：原先 ACTLR=`0x32b` 已包含 bit 5，却没有设置零页
  marker。现从固定池单独分配并清零一页，核对 36 位物理地址，将原始 PA
  两半写入 SMMU 全局 `0x3da6008/0x3da600c` 后才启用 context。实机回读
  匹配；最终额外回读确认 marker 恢复为 0、CM3 保持复位、两个 context
  已禁用、两个 stream 撤销、全局 fault=0。零页继续固定到重启。

### 诊断和协议修正

`gpu_terminal_diagnostics.py` 在每次读寄存器前后原子保存主机 JSON，记录
GPU bank 0 / GMU bank 2 的 FSR/FAR/FSYNR、AO 中断和 fence、固件版本，
在已获 OOB 保持且 GX 可访问时读取 CP/trust 状态。它不读取有特殊掉电
语义的 `RBBM_INT_0_STATUS`，不清除或启用中断。快照最多 114 次读取，
传输失败即停止；**这不是内核中断处理器，也不能保证捕捉突然 900E 的原因。**

MEM_ALLOC 应答现在保留请求的逻辑大小（例如 24 字节），分别按 GFX_ACC /
GMU_ACC 返回地址；底层分配仍按页扩展。安全分配在主机层明确拒绝并记录
原始请求。没有把 kind 3 强行改写为另一条驱动路径使用的 SMMU 结构体。

加入 PRR 窗口时曾超过已部署内核的 16 个 MMIO mapping 槽，导致一次配置
中止，清理成功；现将相邻 SMMU 窗口合并，完整基线重测通过。终端错误
识别也加入 mapping 满/重叠/不可用回复，不再将这些回复误记为等待超时。

### 接下来的平台依赖

安全 kind 5 不能使用当前拥有 CPU、普通 GPU 和 GMU 别名的共享池。
原厂需要独立页面、HLOS 到 CP_PIXEL 的所有权转移，以及安全 GPU context
映射；转移后不允许继续由 CPU/GMU 访问，失败恢复也必须保留真实所有权状态。
同时还需 secure-CX、SMMU aperture、主机 GPU/GMU 故障处理以及后续 ZAP/PAS。
这些必须一起核对后再统一编译；本轮没有因只有部分接口就再编译内核。

`ios16/giulia_gpu_scm.h` 已作为未接入设备的候选实现加入：固定 SCM64 探测、
固定 `0xB` secure-CX 请求、原始返回值保存、有界返回重试及 WAITQ 隔离。
它仅通过宿主和目标代码生成检查，**没有在手机执行 SMC，也未完成运行时
前置条件或安全分配实现**。详见 [安全启动核对](GPU_SECURE_START_AUDIT.md)。

证据汇总：`hardware/ios16-gpu-aqe-diagnostics.json`。关键原始记录：

- AQE + 关闭抢占 + PRR + 明确安全请求拒绝：
  `out/ios16/gpu-native-hwsched-terminal/20261023-081822-328180/`。
- PRR 修正后的非 AQE 基线和四阶段快照：
  `out/ios16/gpu-native-hwsched-terminal/20261023-081633-171938/`。
- 清理后的独立回读：`out/ios16/gpu-post-aqe-state/20261023-081949-566312/`。
- 最后熄屏：`out/ios16/panel-display-terminal/20261023-081949-774856/`。

## 2026-10-23 07:45–08:02 CP 前置核对与 bank 修正

本轮沿用 `gpu-dev-boot-20261023-r1`，没有编译新内核。

配置阶段已实测：补入 A750 的 48 个 CP 保护区、UCHE/GMEM/UBWC 参数、
AO CGC 参数及 P2S 装载完成握手；读取实际寄存器值，打包 58 个 IFPC
register pairs、17 个 preemption pairs、0 个 dynamic triplets。
`cpu_gpu_lock` 前三个字为零，计数字为 `0x113a`；共 154 个字。
每个字上传后回读一致。性能计数掩码请求全 1 但只读回实现位，GMEM 上界
低 12 位读为零；报告保留请求/读值，没有要求保留位可写。

补齐这些寄存器后，首个 HFI raw NOP 仍触发 USB 900E，CP_INIT 和 MEM_WRITE
尚未执行。ACK 等待现只读取宿主 HFI RAM，不在等待循环读写 GMU MMIO；
这排除了“只有 CP_INIT 内容或等待时那一次 MMIO 读取才会触发失败”的解释，
但不足以确定硬件根因。

继续核对固定 Qualcomm SMMU 实现发现明确的布局约束：context bank 0、1
保留给 GPU 的硬件页表切换，其余设备（含 GMU）从 bank 2 分配。此前
诊断借用 GMU bank 0，不适合作为真实 GPU 命令路径。控制器现使用
GPU SID 0 -> bank 0，GMU SID 5 -> bank 2；bank 1 保留未使用。
原厂 DT 的 user SID=0/mask=0、GMU SID=5/mask=0、ACTLR=0x32b 已核对。
修正后 HWSCHED、寄存器配置、GX 上电/休眠及清理都通过，两个 context
FSR 仍为无有效故障的 0x400。**该修正后的 NOP 仍进入 900E，所以不能把
bank 错配报告成已证明的唯一根因。**

另查明固定 GMU core 是 0x40020014（4.2.20）。vendor 只有在 core >=5.1.6
时才移除主机 RBBM IRQ；当前诊断路径没有原厂主机 GPU fault IRQ 处理。
中断归属、可用的故障快照和完整安全/取指前置条件必须一起核对，不能
通过放开没有处理器的 IRQ 或直接重放 NOP 来碰运气。未执行 SMC/ZAP
认证，不把寄存器配置阶段称为完整 GPU 启动。

失败 `cp`、`cpv2`、`nop` 阶段现在都在 CLI 入口拒绝，相关模块也单独拒绝
直接运行。可继续使用 `start`、`gx`、`configure` 收集对应阶段证据。
实测摘要：`hardware/ios16-gpu-cp-audit.json`。两个 NOP 失败的原始日志保留；
两次均经用户恢复 Fastboot 后临时启动同一镜像，未刷写分区。最后恢复记录
在 `out/ios16/gpu-dev-device-20261023-recovery3/`：准备检查全部通过，熄屏命令完成。


## 2026-10-23 07:34–07:40 实机结果

整合内核 `03fb2a3fe78f5f6dede7f8f08152aa961ad8b24f` 已完成一次
[CI 编译](https://github.com/tycykp/UniXNU-OnePlus-Ace5/actions/runs/39870687828)，
并经来源/摘要/嵌入载荷校验后自动 `fastboot boot`。没有刷写分区。
镜像 SHA256 为 `406e4220a0657ad265fa719b99b9e1e46b59caf3b4caaf236fe3b2eae761f0b5`。

实机建立 64 MiB 池、独立 GPU 根表，以及 GMU 的 40 表页/17547 叶页；
17 个 HFI 队列初始为空，未捕获内核 panic。首次端口枚举时一次打开失败，
稍后重试成功并发送 DCS display-off；不能把那次主机错误当作内核失败。

原生 HFI 接口不修改 NMI 向量。固件申请的 kind 2/14/15/16/1 分别分配
4096/4096/4096/32768/32768 字节，清零回读后应答。`START` 返回 result=0
的匹配 ACK。GPU 与 GMU context 的 FSR 均为 `0x400`，没有有效故障。

随后最低性能档/带宽请求 ACK 成功，OOB 唤醒 ACK 成功，GX 状态 `3 -> 0`，
RBBM_STATUS=1、STATUS3=0；slumber ACK 成功，GX 状态恢复为 3。
这是 GX 电源/时钟状态切换的证据，不是频率测量或 GPU 渲染通过。

恢复启动记录位于 `out/ios16/gpu-dev-device-20261023-recovery1/`；同一镜像的
准备检查再次全部通过，显示关闭命令完成。

CP 实验从已验证包复制 SQE/AQE，去掉原厂文件前四字节，GPU 指令地址
保持页对齐，并回读前四个指令字。发送原始 CP_INIT 后 CDC 消失、出现
900E；没有取得 CP_INIT ACK，更没有执行后续 MEM_WRITE 验证。
该实验只做了部分 CP 相关寄存器初始化，不能视作已实现完整 gen7_start、
保护寄存器、power-up register list 和 ZAP/secure 初始化。异常硬件原因
尚未确定；CP 实验入口已禁用，保留源代码与报文供离线分析。

汇总见 `hardware/ios16-gpu-native-terminal.json`。稳定入口为
`scripts/gpu-native-hwsched.py --execute --stage start` 或 `--stage gx`；
`--stage cp` 当前明确拒绝运行。后续应先补全并核对原厂 CP 启动和错误
捕获流程，再进行提交/fence；Metal 尚未发布或验证，不能报告可用。


## Native development ABI (memory/HWSCHED/GX verified; CP/Metal pending)

Candidate `03fb2a3fe78f5f6dede7f8f08152aa961ad8b24f` adds an owned
64 MiB pool in sixteen 4 MiB chunks, an independent GPU page-table root,
and GMU mappings. Pool IOVA starts at `0x61000000`. The first 4 MiB also
has a GMU cache-address-region alias at `0x800000`. All data mappings are
Normal NC and privileged RWX: this is a development facility, not a
production per-task Metal allocator or per-allocation permission boundary.
Secure allocations and host requests for specific MMIO resources are refused.
After pinning, memory remains allocated until reboot; no unsafe free/reset.

`gpu.dev TOKEN OP A B C D` carries six u32 values in at most 63 characters.
The worker executes one request and logs the matching token and result code.
Operations: 0 current roots/ranges; 1 allocate(bytes,alignment,flags,kind);
2 read(offset,count<=32); 3 write(offset,value); 4 zero(offset,bytes);
5 stage HFI(index,value); 6 send(count, every word must be staged);
7 copy verified firmware asset(id,pool offset); 8 list allocations;
9 query owned physical address(offset); 10 clear HFI staging validity.
Pinning is required for operations other than info. Hardware register access,
reset, faults and stream/context activation use the existing recorded reg.*
interface. Raw HFI reception/logging use gpu.hfi.poll/log. No automatic SMC
or secure firmware authentication is performed by this interface.

The native controller answers MEM_ALLOC with owned addresses after zero/readback;
it never treats a memory request as a successful START ACK. Offline checks cover
real preparation methods, independent page walks, allocation failure cleanup,
worker/stop sequencing, parser bounds, and the recorded scratch reply layout.
Build and device results are recorded separately; these tests do not prove GPU
submission or Metal readiness.


## 最新状态：原厂 HFI 初始化已前进至动态内存请求

以下新增结果来自 2026-10-23 06:21–07:00（北京时间）的终端实测。
本文后面的早期阶段记录保留原时间语义，不能据其“尚未启动”判断最新状态。
**GPU 命令执行和 Metal 仍未完成。当前镜像的下一处阻塞是 GPU/GMU
共享内存的分配和映射接口。** 本轮没有重编译或刷写内核。

- 前一轮原厂共享 RAM 队列完成 260 个不同序号的 warmboot 查询 ACK，
  并跨越环形队列边界。TCM 队列实验失败，`GMU2HOST_INTR_INFO=0x8000`
  对应 CM3 fault，不能使用该路径。
- 本轮保留原厂共享 RAM 队列，通过临时 NMI 邮箱读写这块**已固定的 HFI
  分配**。GMU 原厂消息处理代码未修改；NMI 向量临时从 `0x235` 改为
  `0x3a41`，因此这些结果必须标为“带诊断传输的原厂固件”，不能声称
  完整未修改固件或生产驱动已通过。结束后恢复向量、代码和邮箱。
- warmboot 查询返回 `value=1,status=0`；HWSCHED、KPROF 启用均返回
  `value=0,status=0`。性能索引和 GPU 时钟查询返回 `value=0xffffffff,
  status=8`，即 `GMU_ERROR_NO_ENTRY`，不计为功能通过。
- 原厂 AOP 镜像中两套 ARC 表的 CX/MX/GFX 数据一致。按固定 OnePlus
  `adreno_gen7_rpmh.c` 与设备树构造最低档 231 MHz 的诊断性能表及带宽表，
  两者得到三字 ACK、result=0；这类命令按 vendor generic ACK 解析，
  **不能误把没有第四字状态当成 v5 查询成功**。表被接受不证明实际时钟、
  电压或带宽已经生效；没有发送 DCVS 档位切换请求。
- `CORE_FW_START` 得到 result=0 的 ACK；随后查询 context queue 得到
  `value=1,status=0`。`START` 尚未收到完成 ACK，而是收到以下请求：

```text
header=0x00700b14 version=1
gpu_addr=0 flags=0x33f kind=2(SCRATCH)
host_handle=0 gmu_handle=2 gmu_addr=0 bytes=4096 alignment=0
```

`flags=0x33f` 要求 GPU 与 GMU 可访问、privileged、可写、host 可访问并
负责初始化。当前服务没有 GPU 上下文映射或动态分配命令，不能用未拥有的
RAM、现有队列或虚构地址应答。控制器在收到该请求后结束实验并复位，没有
伪造成功回复。最终 `SMMU FSR=0x400`（不含有效 fault）、全局 fault=0，
CM3 保持复位，SID 5 已撤销，bank 0 已关闭，TLB 同步完成。

### 屏幕、异常与恢复

按用户要求，通过 DSI0 FIFO 发送 DCS `0x28`（display off）。每次都先清除
旧 completion，再观察新 completion；`INT_CTRL` 从 `0x500` 到 `0x501`，
`ACK_ERR_STATUS=0`。这证明命令发送完成，不是摄像头观察；未发送 sleep-in，
USB 终端继续工作。恢复启动后再次执行熄屏。

一次对 DT 保留区 `0x81c60000..0x81c7ffff` 的只读 command DB 探测失败。
SMMU bypass 请求首先被硬件拒绝，回读仍为 FAULT，未解除复位执行该方案。
随后使用专用只读翻译表的实验，在提交首个读取请求时 CDC 消失，设备进入
`05c6:900e`。没有取得 command DB 数据；异常的硬件原因尚未确认，不能
仅凭这个现象断言 XPU 或某一总线出错。该实验脚本已禁用，源代码仅保留作
离线分析。其 `finally` 没有完成，因此报告明确记录 cleanup 未确认。

用户将手机恢复 Fastboot 后，使用原有 HFI 镜像重新 `fastboot boot`，
没有刷写分区。新启动的 17 个队列准备检查全部通过，重新采集了表根与
分配地址；后续测试不依赖旧启动地址。新加入的 boot lease 同时核对本地
单调时钟与设备 uptime，重启后拒绝继续使用旧 lease。

### 证据与复现工具

- 汇总：`hardware/ios16-gpu-hwsched-terminal.json`。
- 恢复启动：`out/ios16/gpu-hfi-device-20261023-recovery1/`。
- 最新 HWSCHED：`out/ios16/gpu-hwsched-start-terminal/20261023-065930-942285/`。
- 失败探测：`out/ios16/gpu-cmddb-terminal/20261023-065339-144950/`。
- 恢复后熄屏：`out/ios16/panel-display-terminal/20261023-065711-734746/`。
- `scripts/gpu-terminal-hwsched.py --execute --stage tables|start`：仅供本地
  开发诊断；必须持有当前启动的 `.cache/gpu-live-lease.json`、匹配的启动日志
  和固定固件。默认不执行硬件写入；`start` 在首个未处理分配请求处以失败
  退出，保存原始报文并恢复硬件。不是后台 GPU 驱动。
- `scripts/panel-display-terminal.py off|on`：显式熄屏／亮屏命令，恢复 DSI
  FIFO 的配置，不重建镜像。
- `tests/firmware/giulia_gmu_hfi_mailbox.S`：140 字节有界 NMI 邮箱程序；
  唯一允许的外部数据访问是现有 HFI 分配 `0x60010000..0x60021fff`。
  `tests/ios16_gpu_mailbox.py` 在 Unicorn 中执行实机同一组字节，15 项检查
  覆盖越界、溢出、非法操作、精确写入与寄存器／栈恢复。

邮箱二进制只用于终端诊断，与内核镜像编译无关：

```bash
clang --target=armv7m-none-eabi -fuse-ld=lld -mcpu=cortex-m3 -mthumb \
  -nostdlib -Wl,--image-base=0,-Ttext=0x3a40,-e,mailbox_handler \
  tests/firmware/giulia_gmu_hfi_mailbox.S -o .cache/gmu-hfi-mailbox.elf
llvm-objcopy -O binary .cache/gmu-hfi-mailbox.elf .cache/gmu-hfi-mailbox.bin
python tests/ios16_gpu_mailbox.py
```

下一次统一内核改动需提供：有所有权与生命周期的 GPU/GMU 分配池、真实
GPU context/SMMU 映射、通用 HFI 请求／分配应答接口和可回读的当前表根。
之后继续验证 START 完成、CP/SQE/AQE、提交和 fence，最后才能验收 Metal。

本轮使用已存在的 USB 终端 `reg.map/read/write/rmw/dump` 在手机上完成
测试。截图中的 GMU RAM 清零 panic 已修复并实机越过；进一步验证了
CM3 诊断程序执行、GPU SMMU 页表遍历、主机 RAM 读写和未映射页故障。
**这不是原厂 GMU/HFI 启动、GPU 渲染或 Metal 已完成的证明。**

启动镜像：`out/ios16/gpu-memory-fix-boot-20261023-r2/giulia-ios16-wifi-pcie.img`。
SHA256：`d83cf8301fd0f057e9a0a223a81a397debd1a09c04c5fd9bb20c7962110d04db`。
内核提交 `206aa56484d5da3e47c8881f443e5de9b9639e1b`，
[CI 39850691157](https://github.com/tycykp/UniXNU-OnePlus-Ace5/actions/runs/39850691157)。
打包脚本提交 `12476be59c884ef60f18cd9287ed17695bb17884` 另修复了 GPU
头文件插入后 Wi-Fi 安装脚本重复生成函数的问题。使用 `fastboot boot`
临时启动，未刷写分区。

## 启动与时钟事实

实机完成固件摘要校验、五段 GMU RAM 初始化、6 个表页／104 个叶页构建，
并输出 `host RAM cache publication complete`。原先的 `DC ZVA` panic
没有再次发生，USB 终端和用户态继续运行。

UEFI 报告 `phase=8 code=4 attempted=confirmed=0x1002`：CX 与 AOP
前置步骤之后，因 GMU 分支已经 enabled 而拒绝继续。
后续终端读到 `CM3_SYSRESET=1`，说明 enabled 不等于 CM3 已经执行固件。
尚未证明目标频率 625/200 MHz，也没有改写 PLL 或频率源配置。

| 寄存器 | 初始读值 | 终端实验 |
| --- | --- | --- |
| GCC CFG `0x171004` | `0x80000003` | 清 bit 1 后读到 `1`；结束恢复原值 |
| GCC MEMNOC `0x171010` | `0x80000003` | 清 HWCG bit 1 后读到 `1`；结束恢复原值 |
| GCC DVM `0x171018` | `1` | 未改写 |
| CX GDSCR `0x3d99108` | `0xf822f800` | 未改写 |
| CX HW CTRL `0x3d99168` | `0x80000202` | 未改写 |
| GMU CBCR `0x3d9913c` | `0x2001` | 未改写 |
| SMMU vote `0x3d97000` | `0x80000000` | 置 bit 0 后读到 `1`；结束清除该位并读回初始值 |
| AHB `0x3d9911c` | `0` | enable 写入后仍为 `0`，但 SMMU/TCM/CM3 访存实测可用 |
| HUB `0x3d993e8` | `0` | enable 写入后仍为 `0`，原因未确认 |
| CXO AON `0x3d99004` | `0` | enable 写入后仍为 `0`，原因未确认 |

固定 OnePlus `gpucc-pineapple.c` 将 AHB 标为 `BRANCH_HALT_DELAY`。
据此及实机证据，后续源码的只读观察不再要求 AHB bit 0 为 1；仍检查
halt、CX 和独立的 GCC/SMMU 门控。这个观察修正及短状态日志是实测后
的源码增量，**不在上述已启动镜像内**。

## SMMU 与 TCM

SMMU `ID0/ID1/ID2 = 0x4c017e18 / 0x40000016 / 0x5511`，
`sCR0=0x2f0406`，全局 fault 为 0。24 个 stream 初始均为
`SMR=0, S2CR=0x20000`，22 个 bank 的 `SCTLR.M=0`。
未初始化 TTBR 中有不确定值，不能把它们当成有效映射。

本次仅使用 bank 0、slot 0、SID 5、ASID `0x750`。其他 stream/bank
未改写。当前内核分配的表根是 `0x8c6b78000`：该地址只适用于这次启动，
下一次必须从新分配重新取得，不能直接重放本次物理地址。

配置回读确认 `CBAR=0x1f300`、`CBA2R=1`、`TCR=0x80802020`、
`MAIR0=0xff44`。`TCR2` 写 `0x38011` 回读 `0x38071`，
`SCTLR` 写 `0x1067` 回读 `0x67`；两项差异属于已有库允许的单向
TBI/ASIDPNE 位行为。本地诊断使用关闭 context fault IRQ 的 `SCTLR=0x27`，
通过终端读取故障，未注册运行时故障处理器。

`ID0.ATOSNS=1` 表明不支持地址翻译查询指令。一次 ATOS 尝试得到的
`PAR=0` **已作废，未用作成功证据**。正负验证随后改用实际 CM3 访问。
该位的处理可对照 [Linux 的能力判断](https://kernel.googlesource.com/pub/scm/linux/kernel/git/torvalds/linux/+/77a1c68a96f4e018860033f3827602014a4f1663/drivers/iommu/arm-smmu.c)。

原厂 GMU 三段 TCM 数据为 476、40、24 字节，合计 540 字节／135 个字。
保持复位时，全部通过终端逐字写入并读回一致。非 TCM 固件已由内核准备
在宿主 RAM；这里没有把 GMU IOVA 误作物理地址。

## 实际执行验证

诊断源码保存在 `tests/firmware/giulia_gmu_read_probe.S` 和
`tests/firmware/giulia_gmu_dma_probe.S`。程序只读取规定的 GMU 地址、
写 DTCM 结果邮箱，以及在读写测试中临时修改并恢复独占缓冲区的一个字。
每次实验先核对复位、无 stream 占用、当前表根及无有效 fault，再逐字
验证 ITCM，接通 SID 5，短暂解除复位并有界等待结果。

- **读测试**：从 IOVA `0x4000/0x4004` 读回 `0xbd1fb51f/0xbd10b510`，
  与固定原厂固件的相应两个字一致；完成标记为 `0xa7500002`。
- **负向测试**：读取未映射的 `0x60002000`，SMMU `FSR=0x402`、
  `FAR=0x60002000`。CM3 返回零值，没有进入其 HardFault 处理器；因此
  以 SMMU fault 记录判断拒绝，不能仅根据 CM3 返回值判断是否映射。
- **读写测试**：在已映射、已分配的 `0x60000000` 保存原值 0，写入
  `0xa750cafe`，执行 DSB 后读回一致，再恢复 0 并读回确认。
  两个固件字仍匹配，CM3 CFSR 为 0，SMMU 无有效 fault。

SMMU 的 `FSR=0x400` 不命中有效故障掩码 `0xc00001fe`。此时 FAR 和
FSYNR 可以保留之前负向实验的数据，不能报告为新的故障。
另一次探索性读取 `0x50000` 虽产生 fault，但 FAR 为 `0x6ff000`，
未满足预期，未记为负向验收通过；ICACHE 区域行为仍需进一步分析。

测试结束后，确认 CM3 reset 为 1、slot 0 回到 invalid/FAULT、bank 0
关闭且 TLB 同步完成，清零 TTBR0/1；恢复原厂 ITCM 和诊断覆盖的 DTCM，
恢复本轮修改的 GCC/SMMU 时钟位并释放全部终端映射。bank 0 的禁用配置
字段保留明确的诊断值，没有恢复初始不确定寄存器内容。没有启动原厂
GMU/HFI，也没有遗留运行中的诊断程序或有效 DMA stream。

## 结果与下一阶段

脱敏结果及本地日志摘要在 `hardware/ios16-gpu-terminal-bringup.json`；
原始日志位于 `out/ios16/gpu-live-terminal/`、`gpu-live-tcm/`、
`gpu-live-cm3-read/` 和 `gpu-live-cm3-dma/`。原厂固件正文不进入 Git。

这次终端实测证明了 CM3 与 GPU SMMU 的基本访存通路。下一阶段需要把
已验证的流程接入有生命周期管理的运行时适配器，分配并映射真实的
HFI 队列、日志和 VRB，完成原厂启动所需的 secure CX、PDC/RSC、IRQ、
带宽及 HWSCHED 消息。诊断程序成功不能替代这些原厂固件前置条件。

离线核对诊断程序的编译字节（不访问手机）：

```bash
python3 tests/ios16_gpu_probe.py
bash scripts/test-ios16-gpu.sh out/ios16/gpu-stock-firmware/gmu_gen70900.bin
```

## HFI 队列与终端准备（实机准备阶段已验收）

后续候选内核增加 17 个原厂 HWSCHED 队列，以及独占日志区和 VRB。
HFI、日志、VRB 的 IOVA 分别是 `0x60010000`、`0x60030000`、
`0x60040000`。共享区 CPU 映射使用 Normal NC，固件和页表仍使用
copyback 加显式 cache publication。固定固件现在对应 8 段预留、
6 个表页和 139 个叶页；这些数字只说明宿主内存准备完成。

2026-10-23 06:12（北京时间）已通过 `fastboot boot` 临时启动候选
`gpu-hfi-boot-20261023-r1` 并完成只读验收。17 个队列均为 `code=0`、
`read=0`、`write=0`，共享缓冲区初始化成功，采集日志中没有
`panic(cpu` 或 `Unaligned kernel data abort`。联合内核来自
[CI 39860963355](https://github.com/tycykp/UniXNU-OnePlus-Ace5/actions/runs/39860963355)，
编译提交 `0faf86018401cfea760b9fd94fb6fb933ad20786`。
证据位于 `out/ios16/gpu-hfi-device-20261023-r1/`。

本次同时确认了下一阶段的具体阻塞：UEFI 电源报告
`prepared=0 phase=8 code=4 resource=4`，在观察到已开启的 GMU
时钟 `0x3d9913c=0x2001` 后按策略退出，尚未请求后续时钟和频率。
运行时只读观察在 `0x3d97000=0x80000000` 停止，GPU SMMU
时钟未开启。没有绕过这一状态解除 CM3 复位，也没有激活 DMA stream。
后续必须处理继承时钟的频率/复位验证及电源所有权，再启动原厂固件。

终端增加 `gpu.hfi.status`、`gpu.hfi.pin`、`gpu.hfi.query`、
`gpu.hfi.poll`、`gpu.hfi.log`。`pin` 将诊断内存保留至重启；没有
`unpin`，因为原始 MMIO 调试不能证明所有 DMA 已停止。`query`
只向队列提交 warmboot 属性查询，还需实验控制端写入 doorbell。
匹配 ACK 时分别记录返回值和错误状态；缺少状态的旧协议响应仅证明
收到消息，不标为命令成功。每个响应至多匹配一次。

可在临时启动候选镜像后执行只读阶段验收：

```powershell
python scripts/check-ios16-gpu-device.py --output out/ios16/gpu-hfi-device-check-1
```

脚本仅连接标识为 `ACE5DBG001` 的 CDC 设备，清除接收缓冲历史后请求
新的 service/queue 状态，并保存原始日志、SHA256 和 JSON 结果。
端口被占用、旧内核、缺失/重复队列、非空队列或捕获到 panic 均不能
通过此阶段。输出目录必须是新目录。此脚本不启动 GMU、不重启手机，
通过结果不代表原厂 HFI 握手、GPU 命令提交或 Metal 可用。

原厂启动仍需实际完成 secure CX、PDC/RSC、时钟/带宽和 IRQ 配置，
再验证 FW ready、HFI ready、消息 ACK 与 HWSCHED 初始化。
随后才可推进 GPU context/提交/fence 和 Metal 用户态接口。
