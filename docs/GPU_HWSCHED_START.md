# HWSCHED 冷启动与 CP 执行的厂商对照（2026-10-23）

本文记录把 `out/uefi-auto-r5` 那条 CP_INIT 掉线路径，逐行对照 pinned OnePlus SM8650
graphics-kernel（`hardware/sm8650-gpu.json` 中的 `vendor_sources`）后得到的结论，以及据此
落地的代码和离线测试。**结论不是"再加一个开关"，而是旧流程从未执行过厂商的 HWSCHED 启动
序列，并且把两条协议用错了。**

判定标准沿用仓库既定口径：没有实测日志的东西不算通过。本文新增的是：

- 已实现、离线测试通过（`tests/ios16_gpu_hwsched.c`、`tests/test_gpu_cp_stock.py`）；
- 可在**当前 r5 镜像上直接跑**的主机侧修正路径（`--stage cp-stock`），不需要重编内核；
- 内核侧可复用的 CP 引擎（`ios16/giulia_gpu_hwsched.h`），等 GPU 寄存器窗口在内核可写后接入。

## 1. 四个已确认的偏差

### 1.1 `CP_ME_INIT` ordinal mask：0x94b 是错的，A750 应为 0x14b

厂商 `gen7_cp_init_cmds()`（`adreno_gen7.c:224`）：

```c
mask |= BIT(0);   /* multiple HW contexts */
mask |= BIT(1);   /* error detection */
mask |= BIT(3);   /* default reset state */
mask |= BIT(6);   /* no perfcounter save/restore across preemption */
mask |= BIT(8);   /* register init list with spinlock */
if (!adreno_dev->dms_enabled)
        mask |= BIT(11);  /* "DMS not supported, disable it" */
```

`gen7_9_1` 的 feature 列表含 `ADRENO_DMS`（`adreno-gpulist.h:2386`），
`gen7_hwsched_probe()` 因此置 `dms_enabled = true`（`adreno_gen7_hwsched.c:1970`）。
所以 A750 的正确 mask 是 `0x14b`。旧代码固定发 `0x94b`，即**明确告诉 CP"本机不支持 DMS"**，
与硬件声明相反。

已修正位置：

- `scripts/gpu_terminal_cp_secure.py::init_commands(..., dms=True)` → `0x14b`，`dms=False` 才回落 `0x94b`；
- 可选列表形式只清 bit 8（`0x4b` / `0x84b`），不再顺带把 DMS 位写错；
- `tests/test_gpu_native.py` 的期望值同步更新，并补了 `dms=False` 的用例。

### 1.2 `H2F_MSG_ISSUE_CMD_RAW` 没有 ACK，旧流程等的是一个不存在的回复

厂商 `submit_raw_cmds()`（`adreno_gen7_hwsched_hfi.c:2513`）：

```c
ret = gen7_hfi_send_cmd_async(adreno_dev, cmds, size_bytes);   /* 只发布，不等回复 */
if (ret) return ret;
ret = gmu_core_timed_poll_check(KGSL_DEVICE(adreno_dev),
        GEN7_GPU_GMU_AO_GPU_CX_BUSY_STATUS, 0, 200, BIT(23));  /* 完成判据 */
```

`cp_init()`（同文件 2547）就是把 `gen7_cp_init_cmds()` 塞进一条
`H2F_MSG_ISSUE_CMD_RAW`（消息号 131）异步发出，`GEN7_CP_INIT_DWORDS + 1 = 11` 个字。

旧流程用 `transport.send(131, ...)` 等 2 秒 HFI 回复：协议上不存在这条回复，因此**必然超时**；
把超时当成"队列/门铃已确认之后的下一步进度"是错的。r5 里 `queue_publication_confirmed`
与 `doorbell_confirmed` 只证明消息被发布，不证明 CP 执行。

已修正：

- `scripts/gpu_terminal_cp_secure.py::run_mapped()` 的 `submit()` 改用 `transport.packet(131, …)`
  （异步发布 + 门铃），再用 busy bit 轮询，不再读取 131 的"回复"；
- 新引擎里 `gghs_cp_init()` / `gghs_mem_write()` / `gghs_timestamp()` 全部走异步路径，测试用的
  模拟 GMU **故意不给 131 任何 ACK**，引擎仍必须成功，才能通过测试。

### 1.3 启动序列漏掉了大半个 feature 协商，还反着关了 preemption

厂商 `gen7_hwsched_hfi_start()`（`adreno_gen7_hwsched_hfi.c:2350`）的顺序是：

1. `HFI_WARMBOOT_SET_SCRATCH`
2. perf 表（`H2F_MSG_TABLE`）、BW 表（`H2F_MSG_BW_VOTE_TBL`）
3. `ACD(12)` → `BCL(11)` → `CLX(21)` → `IFPC(9)`
4. `HWSCHED(1)` → `KPROF(16)` → `LSR(23)` → `PERF_NORETAIN(26, enable=0)`
5. `DMS(27)` → `BAIL_OUT_TIMER(17)`
6. `CORE_FW_START(14)`
7. `GET_VALUE(121)` 查 context queue
8. `HW_FENCE(25)`（仅当已有注册的 fence provider）→ `PREEMPTION(2, enable=1, data=preempt_level)`
   → `LPAC(24)`（仅当 LPAC soft fuse 置位）→ `AQE(29)`
9. `H2F_MSG_START(10)`

旧流程只发了 `HWSCHED`、`KPROF`、AQE，并且**主动 disable preemption（feature 2 = 0）**，
再打开 AQE——与厂商正好相反（`gen7_9_1` 声明 `ADRENO_PREEMPTION`、`ADRENO_LPAC`、
`ADRENO_HW_FENCE`、`ADRENO_AQE`）。缺 `HW_FENCE`/`LPAC`/`DMS` 协商时，GMU 侧的
context/hw-fence 资源没有建立；这正是"GMU 收到 131 后整机掉线"最需要先排除的差异。

已修正：

- `scripts/gpu_terminal_cp_stock.py::start()` 按上面 1–9 的顺序执行，逐条检查 ACK 的
  `results[2] == GMU_SUCCESS`；可选 feature 收到 `GMU_ERROR_NOT_SUPPORTED(7)` /
  `NO_ENTRY(8)` 时按厂商 `-ENOENT` 语义降级并记录，必需 feature（HWSCHED/AQE）不接受；
- `scripts/gpu-native-hwsched.py --stage cp-stock` 走这条序列（不再重复发 HWSCHED/KPROF）；
- 内核引擎 `gghs_features()` / `gghs_post_core_features()` 是同一张表，测试里断言顺序与取值。
- `HW_FENCE` 与 `LPAC` 默认**不发**：厂商只在 `adreno_hwsched_register_hw_fence()` 注册队列之后、
  以及 LPAC soft fuse 置位时才使能。当前内核没有 fence provider（`fence_completion=0`），
  盲目使能等于让 GMU 去找一个没人提供的队列。需要时用 `--hw-fence` / `--lpac` 显式打开。

一个必须说清的边界：上游 `drivers/NativeAdreno` 在 A740 实机上用的 HFI 启动只有
perf/BW/ACD/IFPC/CORE_FW_START/START（见 `kits/AdrenoFirmware/AdrenoHfiStartup.c`），没有
HWSCHED/DMS/PREEMPTION 等，照样能渲染。所以"少发 feature"本身不足以解释掉线；按 vendor 全量跑是
为了**消除变量**，不是为了断言缺 feature 就是根因。

### 1.4 已排除：HFI 传输层本身是对的

逐字段核对后，主机侧的队列表与消息布局与厂商一致，不是这次掉线的嫌疑：

| 项目 | 厂商 | 本仓库 |
| --- | --- | --- |
| 命令队列 index | `HFI_CMD_ID 0` | `ggh_send` 用 queue 0 |
| 响应队列 | `HFI_MSG_ID 1`、`HFI_DBG_ID 2` | `ggh_receive` 轮询 1、2 |
| 队列表头 | 6 dwords：version/size/qhdr0_offset/qhdr_size/num_q/num_active_q | `{0,210,6,12,17,17}` |
| 队列头 | 12 dwords，`type=(id|pri<<8)`，`queue_size` 以 dword 计 | `h[2]=q|pri<<8`，`h[3]=1024` |
| 消息头 | `seq<<20 | type<<16 | size<<8 | id`，size 含头 | 同 |
| 131 消息 | 头 + 10 dwords（THREAD_CONTROL 2 + ME_INIT 8），共 11 | 同，r5 实测头 `0x91600b83` |

`fw->data[4:]` 上传 SQE、以 `memdesc->gpuaddr` 作 `CP_SQE_INSTR_BASE`（`adreno.c:90`）也与
本仓库一致，所以固件装载不是这次的第一嫌疑。

## 2. 新增代码

| 文件 | 作用 |
| --- | --- |
| [`ios16/giulia_gpu_hwsched.h`](../ios16/giulia_gpu_hwsched.h) | 纯 C、无 MMIO 依赖的 CP 引擎：type7/奇偶校验、11 字 `CP_INIT` 组装、厂商 feature 表、异步 RAW、200 µs 级 busy 轮询、CP/保护中断捕获、`CP_SET_SECURE_MODE(0)`、`CP_MEM_WRITE` 校验、`CACHE_FLUSH_TS` 时间戳 fence |
| [`tests/ios16_gpu_hwsched.c`](../tests/ios16_gpu_hwsched.c) | 模拟 GMU/CP 的主机测试：金标准 11 字（`0x91600b83`）、feature 顺序与 11 个 ACK、131 无 ACK、DMS mask、ZAP 门控、安全模式退出、GPU 写入/时间戳，以及 ACK 失败/CP fault/busy 不落/读回不一致等负例 |
| [`scripts/gpu_terminal_cp_stock.py`](../scripts/gpu_terminal_cp_stock.py) | 同一序列的主机侧实现，可在现有 r5 镜像上直接执行 |
| [`tests/test_gpu_cp_stock.py`](../tests/test_gpu_cp_stock.py) | 主机侧序列/降级/边界单测 |
| `scripts/gpu_terminal_cp_secure.py` | `init_commands` 的 DMS 语义、`run_mapped` 的异步 RAW 修正 |

`giulia_gpu_hwsched.h` 由 `scripts/ios16_gpu_patch.py` 的 `giulia_gpu_*.h` 通配自动纳入内核补丁，
不需要改构建脚本。

## 3. 在现有 r5 镜像上直接验证（不需要重编内核）

```bash
python3 scripts/gpu-native-hwsched.py --execute --platform --stage cp-stock --host-raw-dispatch
# 需要时再加 --lpac / --hw-fence（前提见 1.3）
```

顺序：`tables → 厂商 feature 序列（含 PREEMPTION 使能、AQE；HW_FENCE/LPAC 按开关）→
CORE_FW_START → context queue → START → GX vote/OOB → Gen7Config 寄存器 → SQE/AQE 拷贝 →
CP_INIT（异步）→ ZAP → CP_SET_SECURE_MODE(0) → CP_MEM_WRITE → CACHE_FLUSH_TS`。

通过判据（`result.json` 字段）：

| 字段 | 期望 |
| --- | --- |
| `hwsched_start_accepted` | `true`，且 `feature_rejections` 里没有 hwsched/aqe |
| `cp_init_mask` | `0x14b` |
| `idle_polls.cp_init` | 有值（说明是轮询判据，不是等 ACK） |
| `cp_init_faults` | `cp_hw_fault`、`cp_interrupt_status`、`cp_protect_status` 全 0 |
| `secure_mode_exit` | `true`，`secvid_trust_control == 0` |
| `gpu_memory_write_verified` | `true`，`cp_mem_write_value == 0xa750cafe` |
| `gpu_fence_verified` | `true`，`cp_fence_value != 0` |

`gpu_memory_write_verified` 是**第一次可证伪的 GPU 执行证据**：目标缓冲区先由主机清零，
只有 CP 执行 `CP_MEM_WRITE` 才会出现 `0xa750cafe`。时间戳 fence 再证明一次完成事件，
可供后续 fence 后端使用。

## 4. 若仍然掉线，按这个顺序看

1. **131 之后 GMU 是否还活着**：`transport.dev(14)` 快照里 `hfi`/`gmu` 段是否还能读；读不到说明
   GMU 本身挂了，优先怀疑 feature 协商缺项或 GMU 内部 context 表未建立。
2. **SMMU CB0 翻译**：`0x3dc0058` FSR、`0x3dc0060` FAR、`0x3dc0000` SCTLR。CP 第一次真实取指
   就是 powerup 列表与 SQE，若 FAR 落在 `0x617…`/`0x614…` 段，说明 per-allocation 页表没有覆盖到
   实际被取的那一页（`hardware_translation_verified=0` 一直是这个风险）。
3. **powerup 列表**：`CP_ME_INIT` 里 bit 8 + bit 31 表示"带 spinlock 的动态 triplet 列表"。
   列表头是 `struct cpu_gpu_lock`（3 个 u32 锁字 + `{u8 ifpc_len, u8 preemption_len, u16 dynamic_len}`），
   必须与实际写入的对子数一致；`ifpc_list_len` 只统计 `gen7_ifpc_pwrup_reglist` +
   `gen7_9_x_ifpc_pwrup_reglist`。
4. **CP protect**：`CP_PROTECT_CNTL=0xb` 下若 CP 初始化要写受保护寄存器，会先在
   `CP_PROTECT_STATUS` 留下记录；引擎已把它纳入判据。
5. **GX 时钟**：`GEN7_GMU_GFX_PWR_CLK_STATUS`、`GEN7_GMU_AHB_FENCE_CTRL` 必须仍为 0；
   OOB 保持不能中途丢失。

## 5. 边界

本文与新增代码只覆盖到"CP 执行第一条真实命令并被主机证明"。它**不**等于：

- 稳定的提交/完成队列（还缺 context 注册、IB 提交、完成中断）；
- fence 后端（时间戳写入只是可用的完成原语，尚未接入 Darwin fence/IOFence）；
- 图形后端、accelerator 与 Metal 用户态。

Metal 侧要落地的是上游 `drivers/NativeAdreno` + `drivers/NativeMetal` 的 A750 适配
（`hardware/sm8650-gpu.json` 的 `upstream` 已 pin 版本），与本文件的 CP 引擎是两层东西：
先把 CP 跑通，再把该驱动接到同一套 GMU/HFI/SMMU 资源上，才能谈渲染。
