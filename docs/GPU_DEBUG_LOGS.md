# GPU 调试日志约定

日志记录观察到的事实和执行结果，不自动推断硬件根因。所有失败默认标记
`cause=unconfirmed`。例如超时只能证明“在规定轮询内未看到期望状态”，
不能据此宣布电源、时钟、固件或 SMMU 中某一项损坏。

## 通用事件

`giulia_gpu_log_event()` 是内核和主机故障注入共用的格式化函数。

| 字段 | 含义 |
| --- | --- |
| `seq` | 当前子系统状态对象中的递增事件序号；不是全系统时间戳 |
| `subsystem` | `power`、`smmu` 或 `gmu` |
| `phase` | 可读阶段名和数值，例如 `program-context(2)`、`firmware-ready(4)` |
| `op` | 读、写、校验、申请、释放、轮询、等待或配置 |
| `address_space` | `physical`、`gmu-iova` 或 `none`，避免把 IOVA 当物理地址 |
| `address` | 访问地址；无地址的资源操作显示 `unavailable` |
| `expected` | 期望值；没有读回期望时显示 `unavailable` |
| `observed` | 实际观察值；读取失败时显示 `unavailable`，不能填一个假零 |
| `mask` | 比较掩码；没有同时给出 expected/observed 时不表示一次读回比较 |
| `code` / `code_name` | 当前子系统的软件结果码和名称 |
| `native_code` | 原始回调返回码，和软件结果码分开；不可用时明确标注 |
| `cleanup` | `0` 为主流程，`1` 为清理流程 |
| `cause` | 失败时为 `unconfirmed`；成功事件为 `none` |

`gg_diagnostics.failure` 保留首次主失败，后续清理不覆盖它。
`cleanup_failure` 单独保留首次清理失败。资源申请/回收、SMMU 和 GMU
公开入口还输出 `GIULIA GPU RESULT`：电源保留位图、bank/slot/SID、
复位是否确认、是否必须保留内存及是否进入隔离状态。
电源 `held_mask` 只表示已确认取得的租约；`quarantined=1` 时，失败步骤还
可能存在结果未确认的资源变化，不能因为某个位未置位就认定该资源可释放。
GMU 汇总的 `writes` 是写入尝试次数，包含失败的写回调；不能等同于成功
写入次数。写回调成功也不代表读回验证成功，必须看后续 read/validate 事件。

常见错误码按子系统解释，不能跨子系统套用：

| 子系统 | 结果码 |
| --- | --- |
| power | 1 参数；2 busy；3 provider；4 timeout；5 release-failed |
| smmu | 1 参数；2 bad-range；3 overlap；4 capacity；5 budget；6 unsupported；7 readback-mismatch；8 I/O；9 timeout；10 busy；11 fault |
| gmu | 1 参数；2 busy；3 firmware；4 prerequisite；5 I/O；6 timeout；7 readback-mismatch |

电源 readiness 最多 1000 轮，每次未就绪请求等待 10 µs。
GMU FW/HFI readiness 各最多 10000 轮，每次未就绪请求等待 10 µs。
SMMU TLBSYNC 使用 1000 次读取预算，不能把它说成测得的毫秒数。
这些是代码的预算与等待请求，不是测得的实际运行时长。

## 实际故障注入输出示例

以下来自主机软件寄存器模型，**不是手机日志**：

```text
subsystem=smmu phase=program-context(2) op=read
address_space=physical address=0x3dc2030
expected=0x80802020 observed=0x80802021 mask=0xffffffff
code=7 code_name=readback-mismatch native_code=0 cleanup=0 cause=unconfirmed
```

这表示 TCR 读操作本身成功，但读回数据与已写入的配置不同。它不能证明
实际手机有这个问题。测试会检查地址、期望/实际值及首次失败保留行为。

```text
subsystem=gmu phase=firmware-ready(4) op=poll
address_space=physical address=0x3d7e070
expected=0x100 observed=0 mask=0x1ff
code=6 code_name=timeout native_code=unavailable cleanup=0 cause=unconfirmed

subsystem=gmu phase=abort-reset(7) op=write
address_space=physical address=0x3d7e000
expected=0x1 observed=unavailable mask=0xffffffff
code=5 code_name=io native_code=89 cleanup=1 cause=unconfirmed
```

这两条应分别报告：“FW ready 轮询超时”和“后续复位写回调返回 89”。
89 是测试注入码，不是高通错误码。第二条不能替换第一条，也不能据此
释放可能仍在被 DMA 使用的页表、固件内存或电源资源。

## 当前内核准备服务

`GiuliaGPU.cpp` 另外记录：

- 固件 Mach-O 头、命令边界、重复段、权限、地址范围、映射及容器校验的
  21 个明确失败检查；摘要不匹配时输出完整 expected/observed SHA256。
- 固件块解析与 GMU 内存规划的返回码、条目数和容量。
- 分配大小、连续物理跨度、物理地址、CPU 映射是否存在，以及页表构建结果。
- 只读电源观察保存完整 GCC/GPUCC/SMMU 快照；前置条件失败时输出
  **第一个不满足条件的寄存器地址、掩码、期望值及读回值**，并标记后续窗口跳过。
- 每个寄存器单独输出名称和物理地址。没有读取、被跳过或读取失败的字段
  显示 `observed=unavailable valid=0`；只有成功读取才能显示数值及 `valid=1`。
  IORegistry 的 `PowerObservationVersion=3`，二进制报告另有 `valid_fields` 位图。
- MMIO descriptor 创建和映射失败使用不同 `native_code`（1 参数检查，
  2 descriptor 返回空，3 map 返回空），不把空返回值解释为“电源有问题”。

固件日志中的 `location` 是当前检查对象的位置：命令表检查为内核头相对
偏移；段范围检查为虚拟地址；RAM 映射检查为物理地址。`check` 指明类型，
不要把这些位置直接当作 GPU 寄存器地址。
时钟分支的 `CLK_OFF` 读回也不是时钟物理损坏的证明：硬件门控可能造成
空闲时的关闭状态。当前驱动只据保守前置条件决定是否继续读取。

## 复现与边界

```bash
python3 scripts/make-gpu-smmu-oracle.py --fetch --out .cache/gpu-source-oracle.c
bash scripts/test-ios16-gpu.sh out/ios16/gpu-stock-firmware/gmu_gen70900.bin
```

`--fetch` 只获取一次固定摘要的公开 Linux 测试输入；后续主机测试不需网络。
`tests/ios16_gpu_log.py` 会校验共用格式化函数的真实输出，不只检查源码里
有没有日志字符串。完整记录保存于 `out/ios16/gpu-offline-control/offline-tests.log`。

本轮不启动手机。电源与写寄存器控制接口尚未绑定真实平台后端；
`giulia_gpu=1/2` 不调用这些写入入口，不能把主机模型通过当成设备已启动。
