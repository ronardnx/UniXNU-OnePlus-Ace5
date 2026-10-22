# Ace 5 GPU 终端实测（2026-10-23）

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

## HFI 队列与终端准备（尚未实机验收）

后续候选内核增加 17 个原厂 HWSCHED 队列，以及独占日志区和 VRB。
HFI、日志、VRB 的 IOVA 分别是 `0x60010000`、`0x60030000`、
`0x60040000`。共享区 CPU 映射使用 Normal NC，固件和页表仍使用
copyback 加显式 cache publication。固定固件现在对应 8 段预留、
6 个表页和 139 个叶页；这些数字只说明宿主内存准备完成。

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
