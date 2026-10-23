# Adreno 750 适配：NativeAdreno / NativeMetal 的 A750 落地清单（2026-10-23）

目标：把上游 `drivers/NativeAdreno`（A740 / sheng 实机可渲染）适配到 Ace 5 的
SM8650 / Adreno 750 / gen7_9_1 / Darwin 22，使 GPU 命令、fence 与 accelerator 走同一条
真实硬件路径，而不是继续停在 UEFI 准备 + 主机脚本诊断。

本文只写**已从 pinned 源码核对出来**的差异与已落地的代码。没有实机或 macOS 构建验证的部分
单独列在第 4 节，不混进"已完成"。

## 1. A750 与 A740 的真实差异（逐项核对）

pin 版本见 `hardware/sm8650-gpu.json` 的 `upstream`（NativeAdreno `5cf1135`、NativeMetal
`ff0c70d`、AdrenoFirmware `40fdb8e`、AdrenoPM4 `445f8fe`、ArmSmmu4K `4d0d659`）。
下表左侧是 `adreno-gpulist.h` 里 `adreno_gpu_core_gen7_9_0`（A740）与
`adreno_gpu_core_gen7_9_1`（A750）的字段对比。

| 项目 | A740 / gen7_9_0 | A750 / gen7_9_1 | 适配动作 |
| --- | --- | --- | --- |
| identity | chipid 由 DT 给出，model 不同 | `qcom,chipid = <0x43051401>`、`qcom,gpu-model = "Adreno750v2"`、compatible `qcom,adreno-gpu-gen7-9-1` | 用 DT 值，不硬编码 |
| GMEM | `.gmem_size = 3 * SZ_1M`、`.uche_gmem_alignment = SZ_16M` | 同 | 同，但 GMEM base 必须按 `adreno.c:1199` 计算 `ALIGN(gmem, align) = 0x1000000` |
| GBIF 表 | `.gbif = gen7_2_0_gbif_regs` | `.gbif = gen7_0_0_gbif_regs`（4× `GBIF_QSB_SIDE0..3 = 0x71620` + `RBBM_GBIF_CLIENT_QOS_CNTL = 0x2120212`） | **必须换表**，不能沿用 A740 的列表 |
| HWCGC | 驱动用 mesa 的 54 条 `a740_hwcg` | gen7_9_1 只声明 `gen7_2_0_ao_hwcg_regs`（3 条），**没有** GX hwcg 列表 | 只能写 3 条 AO CGC，然后走 `RBBM_CLOCK_CNTL_GLOBAL=1 → CGC_GLOBAL_LOAD_CMD=1 → P2S_TRIG_CMD=1 → 等 P2S_STATUS bit0 → GLOBAL=0` |
| CP protect | 驱动表 46 条（mesa） | `gen7_9_0_protected_regs`：48 条 span，含 deny 位，编码 `start | (end-start)<<18 | deny<<31` | 换 48 条表；本仓库已与实机写入值对过账（见第 2 节） |
| power-up 列表 | 驱动 16 条（mesa，`ifpc=0`） | `gen7_pwrup_reglist`：17 条，且 gen7_9_x 另有 IFPC-only 列表 | 用 17 条；IFPC 不使能时 `ifpc_list_len=0`、`preemption_list_len=17`、`dynamic_list_len=0` |
| CP_INIT mask | `NativeAdrenoCpInit::meInitMask() = BIT0\|1\|3\|6\|8 = 0x14b` | 同（`ADRENO_DMS` 已声明，不能再加 bit11） | 上游已是 0x14b；Ace5 主机脚本此前发 0x94b 是错的，已修 |
| HFI 启动 | `kits/AdrenoFirmware` 只发 perf 表、BW 表、ACD、IFPC、CORE_FW_START、START | vendor `gen7_hwsched_hfi_start()` 还包含 HWSCHED/KPROF/DMS/PERF_NORETAIN/BAIL_OUT/CORE_FW_START/GET_VALUE(121)/HW_FENCE/PREEMPTION/LPAC/AQE | 精简序列在 A740 实机能跑，说明这些**不一定**是硬前提；但 A750 的 GMU 固件版本不同，先按 vendor 全量跑通，再逐项回退定位。`LPAC(24)`/`HW_FENCE(25)` 只在 soft fuse 置位 / 有 fence provider 时开 |
| SMMU | 由 sheng 设备仓库提供 | SID：user 0、LPAC 1、secure 2、GMU 5；`qcom,actlr` 原始单元 `[0, 0x3ff, 0x32b]` | 用 SID0（用户）/SID2（secure）/SID5（GMU）；LPAC 的 SID1 暂不接 |
| 固件 | `a740_*` | `gmu_gen70900.bin`（78588 B）、`gen70900_sqe.fw`（85364 B）、`gen70900_aqe.fw`（33176 B）、`gen70900_zap.mbn`（12088 B） | 名字与 sha256 已固化进生成头 |
| 时钟/电源 | sheng 自己拉 | UEFI 已自动配置 PLL1、GMU 260 MHz、HUB 200 MHz 与 SMMU 门控 | 驱动侧**不得**再碰 PLL；只做 GX/CX OOB、GMU 频率投票（DT `qcom,gmu-freq-table = <260000000 64>, <625000000 128>`） |

结论：A750 不是"改个型号"。真正会立刻出错的是 **GBIF 表、HWCGC 路径、protect 表、power-up 列表长度**。
HFI feature 集合属于"先按 vendor 全量、再逐项回退"的类别——A740 用精简序列能跑通这一点本身就说明
少发几条 feature 不一定致命，不能拿它当掉线的唯一解释。

## 2. 已落地的代码（离线可验证）

| 文件 | 作用 |
| --- | --- |
| [`scripts/derive-a750-native.py`](../scripts/derive-a750-native.py) | 从 pinned `adreno-gpulist.h` / `adreno_gen7.c` / `gen7_reg.h` / `hardware/sm8650-gpu.json` 生成 A750 平台数据；缺字段、缺符号、未知表达式一律报错，不做默认值兜底 |
| [`ios16/native/giulia_a750_native.h`](../ios16/native/giulia_a750_native.h) | 生成物：identity、GMEM base/size/对齐、GMU 频率表、SMMU SID/actlr、固件名与 sha256、驱动用寄存器 offset、3 条 AO CGC、5 条 GBIF、48 条 protect span、17 条 power-up 寄存器 |
| [`hardware/giulia-a750-native.json`](../hardware/giulia-a750-native.json) | 生成清单：输入 sha256、表长度、GMEM 值、渲染结果 sha256，`hardware_translation_verified` 仍为 `false` |
| [`tests/ios16_gpu_a750_native.c`](../tests/ios16_gpu_a750_native.c) | 主机测试：identity/GMEM/SID/offset/表长度与取值；把 protect span 编码成寄存器字，其中 +0 = `0x13fc0000`、+1 = `0x0160050b` **与 r5 实机 capture 的 `configured_registers` 逐字一致** |
| [`tests/test_gpu_a750_native.py`](../tests/test_gpu_a750_native.py) | 回归：重新生成必须与提交的头部字节一致；manifest 绑定渲染哈希；篡改 vendor 源必须改变输出（证明没有兜底值）；未知寄存器符号必须报错；protect 字与 r5 capture 交叉核对 |

已跑通：`python3 scripts/derive-a750-native.py --check`、C 主机测试、
`python3 -m unittest tests.test_gpu_a750_native`（5 项全过）。`scripts/test-ios16-gpu.sh`
已把生成检查和 C 测试纳入 CI 序列。

## 3. 与 ShengGmu 的接口映射（把 A750 数据接到上游驱动）

上游 `drivers/NativeAdreno` 的 GMU 冷启动来自设备仓库的 `package/ShengGmu.*`（`ShengGmu.h`
提供 `ShengGmuConfig`、`ShengGmuMemory` 等）。giulia 这边不需要照抄，按职责映射即可：

| ShengGmu 职责 | giulia 现有实现 | 适配要点 |
| --- | --- | --- |
| GMU 固件装载（ITCM/DTCM 写、复位释放、FW ready、HFI 队列初始化） | `ios16/giulia_gpu_gmu.h`（`gg_gmu_load` / `gg_gmu_start`，已与 vendor `gen7_gmu_start` 对齐） | 把 `ShengGmuConfig` 的 5 段 payload（icache/dummy/debug/log/HFI）尺寸换成 giulia 的 `GGH_*` 布局 |
| HFI 启动序列 | `scripts/gpu_terminal_cp_stock.py` 与 `ios16/giulia_gpu_hwsched.h`（vendor 顺序） | A750 用同一张表；上游 kit 那条精简序列需要补 AQE/PREEMPTION |
| 电源/时钟（CX/GX、GMU 频率投票） | UEFI `GpuColdClocks` + 主机 `platform` 动作；内核侧 `observePower` | 驱动侧只做 OOB 与投票，不改 PLL |
| SMMU（native 4K 上下文、streams） | `giulia_gpu_platform_impl.h`（bank 0/SID0、bank 3/SID2、bank 2/SID5） | 把 `ArmSmmu4K` 的 context 配置指向同一组 root/TTBR |
| fence 提供者 | `giulia_gpu_fence_provider.h`（接口已有，provider 未注册） | A750 的时间戳写入已由 `CACHE_FLUSH_TS` 引擎证明；接成 provider 后 `HW_FENCE(25)` 才能开 |

## 4. 还没做、且必须 macOS + 实机才能验证

1. 把 `drivers/NativeAdreno` 的 30+ 源文件与 `kits/AdrenoPM4`、`kits/ArmSmmu4K`、
   `drivers/NativeMetal` 接入 `scripts/ios16_gpu_patch.py` 的 `iokit/conf/files.arm64`（现在只接了
   `ios16/giulia_gpu_*.{c,h}` 与 `GiuliaGPU.cpp`）。这一步只是构建接线，但只能在 macOS 上验证。
2. 用 `giulia_a750_native.h` 替换驱动里的 `A740` 常量（GBIF/HWCGC/protect/pwrup 四处），
   并把 `NativeAdrenoPwrup::buildA740` 换成读 A750 列表的入口。
3. accelerator 注册与 Metal 用户态：上游 `NativeAdrenoPublish.cpp` 依赖审计过的
   `IOAcceleratorES` ABI（`scripts/audit-target-accelerator-base.py` 产出的
   `ioaccelerator-base-abi.json`），而 Ace 5 的内核是 Darwin 22 / iOS 16，必须重新审计一份
   目标 ABI，不能复用 sheng 的 iOS 17 记录。
4. 显示/合成：Ace 5 的 `GiuliaGPU` 还没有 IOSurface/present 通路，GPU 渲染结果要接到
   `NativeMobileDisplay` 或 giulia 自己的 display 驱动上。

## 5. 顺序

1. 先用 `--stage cp-stock` 在现有镜像上取得 `gpu_memory_write_verified`（见
   [GPU_HWSCHED_START.md](GPU_HWSCHED_START.md)）——这一步不需要内核改动。
2. 再按第 3 节把 NativeAdreno 的 GMU/SMMU 接到 giulia 资源上，跑通 ring 提交与 fence。
3. 最后接 accelerator + Metal 用户态与显示合成；在此之前不宣称任何 Metal 能力。
