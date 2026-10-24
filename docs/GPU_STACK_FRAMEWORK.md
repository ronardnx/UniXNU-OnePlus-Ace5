# A750 GPU 接入框架（阶段机 v1）

本文是 giulia（OnePlus Ace 5 / SM8650 / Adreno 750v2）GPU 接入的**整体框架**，
不是又一份实验记录。它做三件事：

1. 把 `docs/GPU_BRINGUP.md`、`GPU_MAPPING.md`、`GPU_GUNYAH_TRANSPORT.md`、
   `GPU_CPU_PMAP.md`、`GPU_FENCE_PROVIDER.md`、`GPU_TERMINAL_BRINGUP.md` 里
   已经存在的模块，按**启动顺序**排成一条可审计的链；
2. 给每一层定义**进入前必须证明什么**（probe）、**进入时做什么**（enter）、
   **失败/退出怎么处理**（leave）；
3. 用 `ios16/gpu/giulia_gpu_stack.h` 把顺序、门控、证据和回滚固化成代码，
   让"细节"只需填进各自的槽位，不再散落在服务和实验脚本里。

框架本身**不访问硬件、不读 MMIO、不发 HVC**。它只决定：能不能进、凭什么进、
失败了能不能退。

## 1. 现状：GPU 到底哪里还没通

本节把"代码/离线状态"和"实机证据"分开写。**实机一列自上次设备会话以来没有变化**；
代码一列是这十几轮补上的接线，它只说明链路接好了、拒绝路径可复现，不说明硬件能跑。

| 环节 | 代码 / 离线 | 实机 |
| --- | --- | --- |
| UEFI CX/时钟准备（`GIULIA_GPU_MODE=3`） | — | 通：CRC 报告、频率投票、CM3 复位保持有回读 |
| 固件解析、打包、摘要固定 | — | 通：15 块、221184 字节、SHA256 逐字节核对 |
| GMU 宿主 RAM + 4 KiB 页表 + 缓存发布 | — | 通：5 段预留、6 表页、104 叶页；`dc zva` panic 已修 |
| GMU TCM 写入/回读 | — | 通：135 个字写入并读回，实验后恢复原厂 TCM |
| Gunyah HVC/RM 查询会话 | 七步只读查询/分片/epoch 离线通过（归档） | 部分：只有 boot FDT 资源提取 |
| carveout `0xd4e23000/0x2dd000` 共享事务 | worker 已接 `gpu.share`：owner 通道 + 16 KiB NC 控制页 + 门控 + 快照 + authority 判定；缺 RM 两位证明即 `refused` | 未跑过 |
| GPU SMMU SID 5 上下文 | `gg_smmu_start` 适配层已接（lease / quiescent / publish / 挂流），默认 device 授权关闭 | 部分：上次实验是"配置→使用→撤销"，未常驻 |
| GMU START / HFI 初始化 | MEM_ALLOC 应答器已接 pool/pump，worker 每轮推进；START 仍无客户端 | 部分：warmboot 查询与 `CORE_FW_START` ACK，**START 停在首个 SCRATCH 请求** |
| AQE / NOP / CP 提交 | 无 | 未通：NOP 无 ACK；首个 `CP_INIT` 后进入 `05c6:900e`，原因未定，CP 入口已禁用 |
| hardware fence / CTX0 / 相邻页授权 | 布局、导入桥（`ggfn_import`）、provider 注册接口都在；CTX0 后端未实现，controller enter 明确拒绝 | 未通 |
| 命令提交、scanout、Metal 发布 | 三个阶段已注册，enter 分别以 `ib-ring-unwired` / `display-target-unwired` / `acceleration-unwired` 拒绝 | 未通 |

**实机上唯一的硬阻塞**仍是 `START` 之后固件发来的第一个请求：

```text
header=0x00700b14 version=1
gpu_addr=0 flags=0x33f kind=2(SCRATCH)
host_handle=0 gmu_handle=2 gmu_addr=0 bytes=4096 alignment=0
```

它要求一块 **GPU 与 GMU 都可访问、privileged、可写、host 负责初始化的 4 KiB**，
并回填真实的 `gpu_handle`/`gmu_handle`。代码侧现在已经有应答器（解析/flags 折叠/
块校验/回包/失败隔离）并接到服务的 pool 与映射后端，但**从未在设备上跑过**；
它是否真的能让固件继续往下走，只能由实机会话回答。

CP 侧另有独立问题：900E 之前的寄存器初始化只做了部分 `gen7_start`，
保护寄存器、power-up register list、ZAP/secure 初始化都不完整，因此
"能发 IB"之前必须先给出 CP 侧的最小可观测闭环。

## 2. 阶段机

15 个阶段，索引即启动顺序。类别决定授权方式：`只读` 不改任何状态，
`宿主` 只改 CPU 页表和宿主 RAM，`租约` 不写硬件但接管别人已准备的状态，
`设备` 会写 MMIO / 发 HVC / 动固件可见内存。

| 枚举 | 名称 | 类别 | 前置 | 状态 | 现有实现 | probe 必须证明 | 下一步 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `GGSK_BOOT` | boot-handoff | 只读 | — | 部分 | `giulia_gpu_gunyah_boot.h`、`giulia_platform_power.h`、UEFI `GpuGunyah.c` | 本次启动的 ADT 报告 CRC/长度/epoch 有效，`giulia_gpu=N` 与报告标记一致 | 把报告升级为可核对的租约描述（当前只作记录） |
| `GGSK_FIRMWARE` | firmware | 只读 | `GGSK_BOOT` | 通 | `giulia_gpu_pack.h`、`giulia_gpu_firmware.h` | 容器 SHA256、四成员摘要、长度与版本匹配 | 保持摘要固定，新增成员时同步 `hardware/` |
| `GGSK_HOST_MEMORY` | host-memory | 宿主 | `GGSK_FIRMWARE` | 通 | `GiuliaGPU::prepareMemory`、`giulia_gpu_memory.h`、`giulia_gpu_cache.*` | 5 段预留、表页/叶页数量与页表遍历一致，缓存发布到一致性点 | 为动态分配补一条可回收的池路径 |
| `GGSK_POWER` | power-lease | 租约 | `GGSK_BOOT`+`GGSK_HOST_MEMORY` | 部分 | `giulia_gpu_power_control.h`、`giulia_gpu_power.h`、`stackProbePower/adopt` | mode 3 报告 prepared、门控观察走到 `GGP_OBSERVED`、平台未污染；adopt 只登记所有权不写寄存器 | XNU 侧 AOP/ClockDxe 后端（今天采用 UEFI 已准备的状态） |
| `GGSK_GUNYAH` | gunyah-channel | 租约 | `GGSK_BOOT` | 部分 | `giulia_gpu_gunyah_{hvc,rm,queries,session,owner,irq}.h`、`stackProbeGunyah/adopt` | owner 未丢失、epoch 有效、通道可用；adopt 只登记所有权，不发 HVC | worker 已能采用通道；后续接 authority 后端 |
| `GGSK_SHARE` | share | 设备 | `GGSK_GUNYAH`+`GGSK_HOST_MEMORY` | 部分 | `giulia_gpu_gunyah_share.h` | `MEM_SHARE` 后 `MEM_LOOKUP` 返回同一 parcel，self/peer RW、label5、peer READY | 把 share 快照接进 worker，作为 `ggfn_import` 的输入 |
| `GGSK_SMMU` | smmu-active | 设备 | `GGSK_POWER`+`GGSK_HOST_MEMORY` | 部分 | `giulia_gpu_smmu.h`、`giulia_gpu_smmu_impl.h`、`platformIdle()` | 采用租约 + 平台已准备/拥有 + 全局 fault 为 0 + 所有 bank/stream 空闲；enter 按上游顺序跑 `gg_smmu_start`（lease → fault 检查 → 发布表 → 再次静止 → 编程 → 挂流） | 需要显式 device 授权才会真正激活；实机顺序待验证 |
| `GGSK_GMU_LOAD` | gmu-load | 设备 | `GGSK_SMMU`+`GGSK_FIRMWARE`+`GGSK_HOST_MEMORY` | 部分 | `giulia_gpu_gmu.h`、`stackProbeGmuLoad`、`gmuCommand(1)` | 采用租约 + SID 5 活跃 + pack 已校验 + 无 fault/quarantine；enter 才写 TCM | 等 SMMU 常驻后由 device 授权运行 |
| `GGSK_GMU_START` | gmu-start | 设备 | `GGSK_GMU_LOAD` | 未通 | `stackProbeGmuStart`、`gmuCommand(2)` | load 完成且未 quarantine；enter 才释复位/等 HFI ready | 先解决 SCRATCH 应答，再谈 START 完成 ACK |
| `GGSK_HFI` | hfi-client | 设备 | `GGSK_GMU_START` | 部分 | `giulia_gpu_hfi_alloc*.h`、`stackProbeHfi/EnterHfi` | 分配器与 pump 状态可观测；enter 明确拒绝（队列 bring-up 未实现） | 应答器已实现并归档 21 组宿主用例；待接服务的 pool/映射后端与队列初始化 |
| `GGSK_CONTROLLER` | controller | 设备 | `GGSK_SHARE`+`GGSK_SMMU`+`GGSK_GUNYAH` | 未通 | `giulia_gpu_fence*.h`、`stackProbeController/EnterController` | 需要已注册 provider + carveout 已映射 + share 冻结；enter 明确拒绝（无 CTX0 后端） | 写 CTX0 client 1 注册与相邻页授权 |
| `GGSK_FENCE` | fence-provider | 设备 | `GGSK_CONTROLLER`+`GGSK_GMU_START` | 未通 | `giulia_gpu_fence_native.c`、`giulia_gpu_fence_import_impl.h`、`stackProbeFence/EnterFence` | provider/映射/share 三输入齐备才导入；`ggfn_import` 结果决定进入或拒绝 | 写 fence 完成/错误/复位生命周期 |
| `GGSK_SUBMIT` | submit | 设备 | `GGSK_FENCE`+`GGSK_HFI` | 未通 | `stackProbeSubmit/EnterSubmit` | GMU 活跃 + fence 已导入 + 平台就绪；enter 明确拒绝（IB ring 未实现） | 900E 定因后重建最小 CP 路径，写第一条 IB |
| `GGSK_SCANOUT` | scanout | 设备 | `GGSK_SUBMIT` | 未通 | `stackProbeScanout/EnterScanout` | fence 已导入 + carveout 已映射；enter 明确拒绝（display target 未实现） | 接 NativeMobileDisplay 等价的 offer/retire |
| `GGSK_METAL` | metal-publish | 设备 | `GGSK_SUBMIT`+`GGSK_SCANOUT` | 未通 | `stackProbeMetal/EnterMetal` | GMU 活跃 + fence 已导入；enter 明确拒绝（acceleration 未发布） | 复用 sheng 的 publish 路径但按 iOS 16 重新验证 |

## 3. 框架规则

代码在 `ios16/gpu/giulia_gpu_stack.h`，规则写成不变量，不是注释：

1. **只有前置全部 enter 才能 enter**。缺 provider 或 probe 失败会阻塞依赖它的阶段，
   既不跳过，也不允许用常量满足。
2. **probe 只读，且紧挨着 enter 执行**。`ggsk_advance()` 内部先调 `probe()`，
   失败即不进；上一次启动的"ready"不会被复用。
3. **宿主变更与设备写入分开授权**。`ggsk_policy` 两项默认 0，
   也就是默认只能准备与观察——和现在服务里 `HardwareReady=false` 一致。
   `租约` 类不写硬件、只接管 UEFI/固件已准备的状态，但仍需要宿主授权，
   并单独计数（`leases`），不与设备写入混算。
4. **设备阶段 enter 失败 → 全栈 quarantine，并停止前进**；
   `leave` 返回不确定（非 0）→ 同样 quarantine，并**停止反序释放**，
   因为资源可能还持有 DMA。enter 返回 `GGSK_ENTER_REFUSED` 表示"在任何变更之前
   就拒绝"：阶段保持 blocked、栈不 quarantine、后续阶段不再前进——share 的
   authority 拒绝就走这条路径，不能把一次干净的拒绝当成部分写入。
5. **拓扑在首次 advance 时冻结**，一个阶段只能有一个 owner，注册后不能替换。
6. **计数器是累计的**：写过硬件就一直算写过；retreat 只表示"已确认停止"，
   不表示硬件已空闲。

依赖图是 DAG 而不是前缀：`GGSK_GUNYAH` 只依赖 `GGSK_BOOT`，
所以电源租约没通时仍然可以先把 Gunyah 通道打通，日志会同时显示
`blocked=power-lease` 与 `entered` 里的 gunyah-channel。

## 4. 第一个打通点：`F2H_MSG_MEM_ALLOC` 应答

`GGSK_HFI` 的硬门槛是 START 之后固件发来的内存请求。本轮把它做成独立单元
`ios16/gpu/giulia_gpu_hfi_alloc.h`：只做协议判断，不碰硬件，宿主侧 21 组用例可复现。

- 解析固件包（`hdr/version/desc`），校验 id=20、type=`HFI_MSG_CMD`、size 字段与
  帧长一致、`mem_kind<26`、flags 只接受已知位、拒绝 secure、size 非零、
  对齐指数 ≤30；
- 按原厂语义计算有效 flags：普通 SYSRAM 没有 GFX 访问时折入 `0x38`，并且必须
  返回 GMU 地址；有 GFX 访问时只有 `GMU_ACC` 才返回 GMU 地址；
- 只接受 provider 真正拥有的块：地址必须页粒度、GMU 地址满足 VA 对齐、未申请的
  别名必须为 0——GPU 看不到的块 `gpu_addr=0`，GMU 看不到的块 `gmu_addr=0`；
- 应答保留请求的 flags/kind/align，只把 size 换成真实大小，`host_mem_handle`
  回填请求的 `gmu_mem_handle`；
- 同 key `(kind, gmu_mem_handle)` 复用同一块，但 size/flags/align 不一致时拒绝，
  不做隐式共享；
- 失败路径：reserve 之后校验失败或 publish 失败，只在 release 确认后回收；
  release 不确定就整栈 quarantine；表满在分配前拒绝，不泄漏。

kind 5/19/22/23 需要独立后端（secure、IPC core、memstore、hw-fence），
没有 `reserved` 回调时返回 `GGA_RESERVED`，不会拿普通 RAM 顶替。

配套两层也在本轮落地：

- `giulia_gpu_hfi_alloc_pool.h`：应答器的真实后端。`reserve` 走现有 `ggd_pool`
  的 bump 分配，按有效 flags 选择缓存的 GMU 别名窗口，逐空间 `map()` 后再取
  地址；映射失败会把已经建立的映射退掉；`publish` 走清零/回读/缓存发布；
  `release` 只做确认过的 unmap（bump 池不回收空间，这一点写在文件头）。
- `giulia_gpu_hfi_pump.h`：worker 的每轮策略。只把 `F2H_MSG_MEM_ALLOC` 交给
  应答器，ACK/ERR/DEBUG 只计数不解释；请求被拒立即停并上报（否则固件会一直等
  一个不会来的回包）；回包入队失败则 quarantine——序号已经消耗，丢掉回包就是
  静默挂死。

这三层用同一套 `ggh_io` 串起来测：pump 测试直接跑真实队列表内存
（`ggh_init`），因此队列语义不是模型。

接进服务的 glue 已落成 `ios16/gpu/giulia_gpu_hfi_alloc_impl.h`：`gga_pool_io` 的
`address/map/unmap/publish` 直接读本服务的 `devPool`、调用 `mapDevAllocation()` 和
缓存发布；`hfiAllocInit()` 在 `prepareMemory()` 之后调用一次；worker 每轮在
`hfiPumpOnce(2)` 里推进 pump，前置条件是 `hostPrepared && manualMemoryPinned &&
devMappingsManaged && !platformPoisoned && !gmuFaulted`。`unmap` 明确返回"未确认"——
bump 池在重启前不回收，release 只能让整栈 quarantine，不能假装内存已经自由。

服务侧 glue 的内核链接与实机行为仍以 CI 与设备日志为准。

`GGSK_SHARE` 的第一步也落成可测单元 `giulia_gpu_share_snapshot.h`：
`ggsh_run()` 成功后把 epoch、动态 self/peer VMID、parcel、通道序号一次性冻结；
`ggsn_share()` 每次回答前都用 live `ggsh_state` 复核并比对冻结值，任何偏差都
永久失效（`GGSN_STALE`），冻结副本永不跟随 live 变化。它只读记录字段，
不发 HVC/SCM、不碰 carveout；`ggsn_share_cb` 就是 `ggfn_inputs.share` 需要的回调形状。真正的运行时接入
（worker 里跑 `ggsh_run` 再取快照）由 `giulia_gpu_share_runtime.h` 门控：
通道 → owner live → 动态 identity → **authority**，全部通过才允许 `ggsh_init`。

authority 本身拆成两部分。`giulia_gpu_share_authority.h` 负责可判定的六项
（epoch、owner token、控制页对齐/大小/不与 carveout 相交、carveout 精确匹配、
UEFI 电源报告、平台健康），并把 RM 侧的 **primary 角色** 与 **carveout 独占**
作为输入位。服务今天把这两位传 0，于是判定结果是 `GGSA_UNPROVEN`，
`missing=0xc0`，`ggsh_init` 不会发生、也不会有任何 RM/SCM 报文；
只有 RM 后端能置位时才会变成 `GGSA_OK`。判定逻辑本身有 8 组宿主用例（归档），
包括"本地六项全绿仍然 UNPROVEN"和"只补一侧 RM 位仍然不够"。

控制页的对齐不是形式问题：`ggsh_init()` 要求 CPU 指针与物理地址**同时** 16 KiB
对齐。`shareControlAllocate()` 因此按 32 KiB 申请、在保留 16 KiB 余量的前提下
显式挑窗口，并在申请时就区分三种失败原因（`not-contiguous`、
`alignment-congruence`、`range`），而不是把问题留到 `ggsh_init` 里变成一个
没有上下文的 `ARGUMENT`。

## 5. 代码与接入点

| 文件 | 作用 |
| --- | --- |
| `ios16/gpu/giulia_gpu_stack.h` | 阶段表、策略、槽位、`ggsk_init/register/probe/probe_all/advance/retreat` |
| `ios16/gpu/giulia_gpu_stack.c` | 内核入口 `giulia_gpu_stack_*` 与每阶段一行有界日志 |
| `ios16/gpu/giulia_gpu_hfi_alloc.h` | `F2H_MSG_MEM_ALLOC` 应答：解析、flags 语义、块校验、回包、失败隔离 |
| `ios16/gpu/giulia_gpu_hfi_alloc_pool.h` | 应答器后端：`ggd_pool` bump 分配 + GPU/GMU 映射 + 清零回读发布，失败只回收映射 |
| `ios16/gpu/giulia_gpu_hfi_pump.h` | worker 每轮策略：只派发 MEM_ALLOC、其余计数丢弃、拒绝即停、回包失败即 quarantine |
| `ios16/gpu/giulia_gpu_hfi_alloc_impl.h` | 服务侧 glue：`gga_pool_io` 指向本服务的 `devPool`、`mapDevAllocation`、缓存发布；worker 每轮 `ggpp_pump` |
| `ios16/gpu/giulia_gpu_share_snapshot.h` | `GGSK_SHARE` 的冻结身份：只读地固化 epoch/parcel/动态 VMID/通道序号，并供 `ggfn_inputs.share` 回调 |
| `ios16/gpu/giulia_gpu_share_authority.h` | authority 的可判定部分：epoch/owner/控制页/carveout/电源/平台六项本地证明；RM 侧 primary + 独占是**输入**，缺失即 `GGSA_UNPROVEN` |
| `ios16/gpu/giulia_gpu_share_runtime.h` | 一次 share 事务的门控：通道/owner/identity 逐级检查，**authority 必须在 `ggsh_init` 之前**，之后才是 run+快照 |
| `ios16/gpu/giulia_gpu_share_impl.h` | 服务侧 share glue：`gpu.share` → owner 通道 + 16 KiB NC 控制页 + `ggsr_prepare/finish`；authority 走 `ggsa_verify`，缺 RM 两位即拒绝 |
| `ios16/gpu/giulia_gpu_smmu_impl.h` | SID 5 适配层：io/lease/quiescent/publish/release 五组回调 + `smmuInit()` + `stackEnterSmmu`（按上游顺序调用 `gg_smmu_start`） |
| `ios16/gpu/giulia_gpu_fence_import_impl.h` | share → fence 导入桥：`ggfn_import` 的 provider/`ggsn_share_cb`/native map/audit 四路输入与 `gpu.fence` 命令；share 未完成时明确拒绝 |
| `ios16/gpu/giulia_gpu_stack_impl.h` | 服务侧阶段机：十五个已注册 provider 的 probe/enter/leave、`gpu.stack` 计划、`gpu.status` 摘要、stop 时的 retreat |

阶段机已注册进服务，15 个阶段全部有 provider：boot-handoff / firmware /
host-memory / power-lease / gunyah-channel（只读 + 宿主 + 两个采用类）、
share（设备类，authority 缺失时以 `refused` 收场）、smmu-active（设备类，
`platformIdle()` 审计 + `gg_smmu_start` 适配层）、gmu-load / gmu-start
（设备类，复用 `gmuCommand`）、controller / hfi-client（设备类，enter 明确
拒绝并写明缺哪个后端）、fence-provider（设备类，enter 跑 `ggfn_import`）、
submit / scanout / metal（设备类，enter 明确拒绝：IB ring / display target /
acceleration 都未实现）。"有 provider"只表示阶段会被如实评估，不表示已经能进入；
`gpu.stack` 会停在链上第一个未满足的环节。注册契约如下：

1. `GiuliaGPU::start`：`giulia_gpu_stack_init()` 一次，再按层 `ggsk_register()`
   15 次。策略来自 `giulia_gpu` 模式：模式 1/2 全 0，模式 3 允许宿主变更；
   设备写入固定为 0——**没有任何终端命令可以临时放开它**，要验证设备写入只能在
   代码里改策略并重新构建。
2. 新终端命令只有 `gpu.stack`：它调用 `giulia_gpu_stack_plan()`（内部
   `ggsk_advance(GGSK_METAL)`），只读地推进并打印每个阶段的状态，不写任何寄存器。
3. `gpu.status`：追加 `giulia_gpu_stack_log()` 的两行摘要。
4. `GiuliaGPU::stop`：`giulia_gpu_stack_retreat()`；quarantine 时保留资源并只记录，
   不自动 free。

每个阶段的 `probe/enter/leave` 实现放在各自的现有单元旁边，例如
`giulia_gpu_stack_power.c`、`giulia_gpu_stack_hfi.c`，不塞进框架文件。

## 6. 里程碑与验收证据

| 里程碑 | 内容 | 验收证据 |
| --- | --- | --- |
| M1 | 框架接入服务：`gpu.stack` 只读计划 | 已完成：mode 3 下日志为 `entered=5/15 ... blocked=5(share) device_writes=0`，mode 1 为 `entered=2/15 blocked=2(host-memory)`；设备授权打开后 share 以 `code=refused` 收场且不 quarantine |
| M2 | `GGSK_SHARE` 通：share 快照接进 worker | 快照 + 门控 + authority 判定 + `gpu.share` + `gpu.fence` 导入桥都已接；M2b 只差 RM 侧 primary/独占证明（判定里已留为输入位）与一次真正完成的 `ggsh_run`/`ggfn_import` |
| M3 | `GGSK_POWER`+`GGSK_SMMU` 常驻 | power-lease 已能采用 UEFI 准备态；SMMU 适配层已按上游顺序接好 `gg_smmu_start`，但默认 `device_writes=0` 不会执行，`smmu_active=1` 与常驻持有仍需实机验证 |
| M4a | SCRATCH 应答器：kind2/14/15/16/1 由自有映射应答 | 已完成宿主侧；`HFI ALLOC FIXTURE: 21 cases passed`、`HFI POOL FIXTURE: 13 cases passed`、`HFI PUMP FIXTURE: 8 cases passed` |
| M4b | `GGSK_HFI` 通：三层接进 worker + START 完成 ACK | glue 已在宿主 harness 编译并运行（空池/未 pin/坏队三个场景），结构检查通过；内核 CI 链接通过后再看 START 完成 ACK（不是分配请求）、17 队列就绪、无 900E |
| M5 | `GGSK_CONTROLLER`+`GGSK_FENCE` 通 | CTX0 client 1 注册成功，队列前后页映射通过审计，fence 可观察完成 |
| M6 | `GGSK_SUBMIT`→`GGSK_SCANOUT`→`GGSK_METAL` | 一条 IB 的 MEM_WRITE 回读一致；GPU 帧像素与 CPU 参考一致；render server 使用原生设备 |

M1 之后每一步都必须同时更新 `hardware/ios16-gpu-*.json` 与对应 `reports/` 目录，
沿用本仓"离线证据与实机证据分开记录"的规则。

## 7. 与现有文档的分工

| 文档 | 负责 |
| --- | --- |
| 本文 | 顺序、门控、证据、回滚和里程碑 |
| `GPU_BRINGUP.md` | 硬件输入、构建与打包流程、历史阶段 |
| `GPU_MAPPING.md` | 地址、权限、固件封存、HFI kind 处理 |
| `GPU_GUNYAH_TRANSPORT.md` | HVC/RM/会话/共享事务的协议细节 |
| `GPU_CPU_PMAP.md` | 队列 CPU 映射与 0086 原生守卫 |
| `GPU_FENCE_PROVIDER.md` | provider 注册、导入与与 fence 生命周期 |
| `GPU_TERMINAL_BRINGUP.md` | 实机实验与故障记录 |

## 8. 验证

框架需要完整 XNU 构建与第 11 节实机会话记录。历史宿主 fixture 只证明软件拒绝路径，
不证明 A750、SMMU、GMU、Gunyah 或 Metal 的硬件行为；本地测试树已按
`local-test-cleanup` 移出工作区，文中提到的 `tests/*`、`test-ios16-gpu.sh` 与
"xx 组用例"都是**归档记录**，不再可复跑。当前验证入口只有两个：

1. CI：`.github/workflows/ios16-gpu.yml` 先跑两项一致性检查
   （`check-gpu-command-surfaces.py`：内核命令名 / 主机白名单 / 文档；
   `check-gpu-stack-contract.py`：阶段枚举 / desc 表 / 前置依赖 DAG / 文档表 /
   类别 token，以及 provider 覆盖（每阶段恰有一次 `stackOps` 赋值与一次注册）和
   服务方法定义唯一性），再跑 `Build kernel` 与 `Verify GPU service linked`；
2. 实机：第 11 节的七条命令与日志采集。

## 9. 本轮边界

本节按轮次追加，记录每轮做了什么、没做什么。**早期条目里的阶段数、注册数和
"未实现"结论只在其轮次成立**；当前状态一律以第 12 节总表为准，不要用本节旧条目
判断现状。

- 新增 `giulia_gpu_hfi_alloc.h`、`giulia_gpu_hfi_alloc_pool.h`、`giulia_gpu_hfi_pump.h`、
  `giulia_gpu_hfi_alloc_impl.h` 及三套宿主用例（21 + 13 + 8）与两份结构/编译检查；
  协议与语义取自固定的原厂 `adreno_hfi.h`/`adreno_gen7_hwsched_hfi.c`
  （摘要已在 `hardware/` 记录）；pump 用例直接驱动真实 `ggh_init` 队列表内存；
- `GiuliaGPU` 只增加成员、初始化、worker 调用、report 与 stop 日志，
  没有改动任何既有硬件路径，也没有新增终端命令；
- 历史 glue 主机检查曾编译并运行空池/未 pin/坏队三个场景；
  内核链接与实机行为仍待 CI 与设备日志，本文不声称已通过；
- 阶段机接入服务：`gpu.stack` 走 `ggsk_advance()` 并输出 15 行阶段表；策略固定
  `allow_device_writes=0`，mode 3 才允许宿主变更；登记阶段随轮次扩展到 9 个
  （见第 4 节），未注册阶段保持 no-provider；
- share authority 拆成可判定部分 `giulia_gpu_share_authority.h`（8 组用例）：
  六项本地证明 + 两个 RM 输入位，缺失即 `UNPROVEN`；服务把 RM 位固定传 0，
  所以 `gpu.share` 仍停在 `GGSR_AUTHORITY`，日志给出
  `authority=unproven proofs=0x3f missing=0xc0`；
- 阶段机新增 `GGSK_ENTER_REFUSED` 语义（干净拒绝不 quarantine）与 SMMU/GUNYAH/
  SHARE/GMU-LOAD/GMU-START provider：GUNYAH 采用 owner 通道（`leases=2`），
  SHARE 在 device 授权下运行 `gpu.share`、authority 缺失时以 `code=refused` 收场，
  GMU 两个阶段复用 `gmuCommand(1/2)`，SMMU 用 `platformIdle()` 做只读审计；
- `shareControlAllocate()` 按 `ggsh_init()` 的真实要求分配控制页：32 KiB 申请、
  显式挑 16 KiB 窗口、CPU 与 PA 同时对齐、余量校验，并区分
  `not-contiguous` / `alignment-congruence` / `range` / `overlaps-carveout`
  四种拒绝原因；
- 新增 `hardware/gpu-stack-session.example.json`：一次实机会话的结构化记录模板，
  逐条对应六条命令的字段、`verdict`（as-expected/regressed/unknown），并用
  `not_claimed` 明确列出本次不声称的结论；空字段表示未观测。命令面检查会读取
  这个模板，确保它记录的每条命令都是内核真正映射的（已用 `gpu.bogus` 反向验证）；
- `gpu.status` 的 `power_acquired/smmu_active/gmu_started/device_writes` 原本是
  **硬编码 0**，会与真实阶段状态矛盾；改为实时读取 `powerLeaseHeld/smmuActive/
  gmuActive/stackState.writes`，并新增 `platform_ready`；第 11 节判读同步；
  `GPU_BRINGUP.md` 里"始终保留 `smmu_active=0 …`"的旧描述也一并改掉；
- 契约检查新增"`registered=N` 必须等于枚举里的阶段数"一条（已用改成 14 反向验证），
  防止新增阶段却忘了更新汇总日志；
- 新增 `scripts/check-gpu-stack-contract.py` 并接入 CI：核对阶段枚举、desc 表、
  前置依赖必须是更早阶段、文档表的名称/顺序/类别 token，以及 provider 覆盖
  （15 个阶段各一次 `stackOps` 赋值、各一次注册）与 139 个 `GiuliaGPU::` 方法定义的
  唯一性/类内声明；两项都用故意改错验证会准确报错（类别 token 与
  `stackOps[GGSK_FENCE]=ops;` 的空格漂移）；
- 新增 `scripts/check-gpu-command-surfaces.py` 并接入 CI：核对内核
  `gg_terminal_equal` 命令名、主机 `DEVICE_COMMANDS` 白名单与 `USB_TERMINAL.md`
  三方一致；它第一次运行就查出 9 个断点（5 个 HFI 命令未写进文档、
  3 个 GMU 命令主机工具根本发不出去），已全部补齐；
- 主机终端工具 `scripts/debug_terminal.py` 补齐 `gpu.stack` / `gpu.share` / `gpu.fence`
  白名单（此前会被 PC 侧以 "Unknown diagnostic command" 直接拒绝），`watch` 也禁止
  轮询这三个会触发工作的命令；`docs/USB_TERMINAL.md` 命令表与第 11 节 runbook 同步；
- `GGSK_SUBMIT`/`GGSK_SCANOUT`/`GGSK_METAL` 也已注册：probe 只用可观测状态，
  enter 分别以 `ib-ring-unwired` / `display-target-unwired` /
  `acceleration-unwired` 明确拒绝，确保 `gpu.stack` 能把整条链的状态说完整；
- `GGSK_HFI` 也已注册（当时 12 个，现为 15）：probe 读分配器/pump/GMU `hfi_ready` 状态，
  enter 明确拒绝（队列 bring-up 未实现）；
- `GGSK_CONTROLLER`/`GGSK_FENCE` 已注册为 provider：controller 的 enter 明确拒绝
  （没有 CTX0 注册后端，`client=0 table=0 writes=0`），fence 的 enter 直接调用
  `fenceImportAttempt()`，`import skipped` 视为干净拒绝；阶段机注册数 15；
- 新增 `giulia_gpu_fence_import_impl.h` 与 `gpu.fence`：把 share 冻结快照
  （`ggsn_share_cb`）接到 `ggfn_import` 的四路输入（provider / share / native map /
  native audit），share 未完成或 provider 未注册时明确拒绝，不构造合成身份；
  `gpu.share` 成功后会自动尝试一次导入；
- 新增 `giulia_gpu_smmu_impl.h`：SID 5 适配层，按上游顺序实现
  lease（bank 2 / slot 0 / ASID `0x750`，独占且 GMU 未加载）→ fault 检查 →
  发布 `tableMemory` → 再次静止 → `gg_smmu_start` 编程 → 挂流；任何“已部分编程”的
  失败都返回非 0 让框架 quarantine，干净拒绝才返回 `GGSK_ENTER_REFUSED`；
  表在 `stop()` 时若仍被活动翻译引用则保留，不释放；
- 阶段机新增 `租约` 类与 POWER provider：probe 要求 mode 3 报告 prepared 且门控观察
  走到 `GGP_OBSERVED`，enter 只登记所有权（`powerLeaseHeld=1`），leave 只撤销登记并
  明确硬件保留到重启；plan 因此在 mode 3 走到 4→5/15，`device_writes=0`；
- 服务侧 share glue：`gpu.share` 取 owner 通道、分配并发布 16 KiB NC 控制页，
  调 `ggsr_prepare`；`shareAuthorize` 给出结构化判定并拒绝（见第 10 节的
  `proofs/missing`），因此这条命令今天停在 `GGSR_AUTHORITY`，`ggsh_state`
  保持为零且不发 RM/SCM；历史宿主 harness 曾核对这两条日志；
- 新增 `giulia_gpu_share_runtime.h` 与 7 组宿主用例：把"authority 在 `ggsh_init`
  之前"写成顺序约束；拒绝时 `ggsh_state` 保持为零、不产生 RM/SCM 流量，且允许
  带真实授权重试；`ggsr_finish` 的事务路径仍由 share 单元自己的脚本化用例覆盖；
- 新增 `giulia_gpu_share_snapshot.h` 与 10 组宿主用例：冻结已完成 share 的身份，
  每次回答前用 live 状态复核，偏差永久失效；只读记录字段，不发 HVC/SCM；
- 未启动手机、未访问 MMIO/HVC/SMC、未改写任何硬件状态；
- 所有阶段状态来自既有文档与源码，未新增"已验证"结论；
- `GGSK_SUBMIT`/`GGSK_SCANOUT`/`GGSK_METAL` 当时只有槽位与验收条件、没有实现；
  后续轮次已把它们注册为"明确拒绝"的 provider（现状见第 12 节）。

## 10. 上游对照（硬件流程必须可追溯）

按 `docs/DRIVER_MODULES.md` 与一加开源流程，硬件顺序不能凭经验简化。下表把
本地实现逐项对到固定上游提交；"差异"一栏写的是**尚未实现**，不是可以省略。

| 上游（固定提交） | 关键函数 / 结构 | 本地对应 | 差异 |
| --- | --- | --- | --- |
| `adreno_gen7_hwsched_hfi.c` @ `453d8875` | HFI 队列与 `F2H_MSG_MEM_ALLOC` 处理 | `giulia_gpu_hfi_alloc.h`、`_pool.h`、`_pump.h`、`_alloc_impl.h` | 只在原生 HFI client 活动时派发 MEM_ALLOC；其他消息保留给明确消费者；块分配是 append-only，不回收 |
| `adreno_hwsched.c` @ `453d8875` | `adreno_hwsched_register_hw_fence_client()`、CTX0 与 context queue | 无 | `GGSK_CONTROLLER`/`GGSK_FENCE` 没有 provider |
| `msm_hw_fence.c`、`hw_fence_drv_{ipc,utils,priv}.c` @ `453d8875` | carveout 布局、队列、IPC 方向 | `giulia_gpu_fence*.h`（只读模型/导入）、`giulia_gpu_gunyah_share*.h`（RM/SCM 事务） | 没有控制器映射，也没有完成中断 |
| `pineapple-hw-fence.dtsi` @ `453d8875` | carveout 物理地址/大小、SID | `GGF_CARVEOUT_PHYSICAL/BYTES`、SID 5 | 常量一致；运行时未接触 |
| ArmSmmu4K @ `4d0d6598`（vendor 已入库） | 上下文编码、流表、编程 | `giulia_gpu_smmu_impl.h`（io/lease/quiescent/publish/release）、`giulia_gpu_smmu.h`、`stackEnterSmmu` | 已按上游顺序接 `gg_smmu_start`；未在实机跑过，且 bank 2 / slot 0 / ASID `0x750` 与审计profile 绑定 |
| UEFI 模式 3 电源准备（本仓库 UEFI 侧） | CX/GMU/hub/SMMU 时钟、CM3 复位保持 | `giulia_gpu_power.h`、`stackProbePower/adopt` | XNU 只采用已准备状态，不重复 enable |

## 11. 实机会话 runbook

只有实机能回答"阶段机在设备上的可观测部分是否成立"。本框架不写硬件，
所以会话本身没有回滚步骤：失败重启即可，没有任何持久化状态。

| 顺序 | 命令 | 期望 | 判读 |
| --- | --- | --- | --- |
| 0 | `scripts\debug-terminal.cmd --port COMx` | 连上 `1209:0001`，进入诊断模式 | 主机工具的命令白名单已包含 `gpu.stack`/`gpu.share`/`gpu.fence`；未知命令会被 PC 侧直接拒绝 |
| 1 | `gpu.status` | `stage/mode/firmware_verified/host_prepared` 与 `platform_ready/power_acquired/smmu_active/gmu_started/device_writes` 两行都有值 | 这些是实时标志（本轮刚修掉硬编码 0）；`hardware_ready=0`/`acceleration=0` 是端到端结论，不会因阶段推进而变 |
| 2 | `gpu.stack` | mode 3 下 `entered=5/15 blocked=5(share) leases=2 device_writes=0` | `blocked` 更靠前 → 按第 12 节表往上查；`device_writes>0` → 立即停并记录 |
| 3 | `gpu.share` | `authority=unproven proofs=0x3f missing=0xc0`，`share` 状态为零 | 出现 `authority=ok` 说明 RM 位被写死，属回归 |
| 4 | `gpu.fence` | `import skipped reason=provider-not-registered` 或 `reason=share-not-frozen` | provider 未注册前不导入；share 未完成时不会产生 CPU fence 状态 |
| 5 | `gpu.hfi.status` / `gpu.hfi.query` | START 仍停在 SCRATCH 分配，记录 `flags=0x33f size=4096` | 与已知阻塞点一致才算"没退化"，不代表已通 |
| 6 | （仅显式 device 授权时）SMMU | `GIULIA GPU SMMU: lease ... exclusive=1` → `tables published` → `active=1` 或 `start result=... writes=...` | 没有授权时这一行不应出现；出现 `active=1` 才算 SID 5 翻译已装上，仍需独立验证 GMU 能否走通 |

采集要求：这六条命令的完整日志、构建标识与镜像配置一起存档到 `reports/`（脱敏），
再把逐条结果填进 `hardware/gpu-stack-session.example.json` 的副本
（`hardware/ios16-gpu-stack-session-<date>.json`）：**没看到的字段留空**，空值
表示"未观测"，不能当成"通过"；`not_claimed` 段列出本次会话明确不声称的结论。
不要把一次会话写进本文当作"已验证"。若 `gpu.stack` 在设备上返回 unsupported 或
没有响应，先确认镜像里的 USB 终端服务确实把 `gpu.*` 转给 `giulia_gpu_terminal()`，
再怀疑 GPU 服务本身——这两层是独立构建的。

## 12. 15 阶段状态总表

"离线证据"是归档的宿主 fixture 结论（测试树已在 `local-test-cleanup` 中移除，不可复跑）；
"实机"一列本轮全部为未验证，因为还没有设备会话。当前可执行的验证只有 CI 内核构建与
第 11 节会话。

| # | 阶段 | provider | 离线证据（归档） | 实机 |
| --- | --- | --- | --- | --- |
| 0 | boot-handoff | 已注册（只读） | 阶段机 17 例 | 未验证 |
| 1 | firmware | 已注册（只读） | 同上 | 未验证 |
| 2 | host-memory | 已注册（宿主） | 同上 | 未验证 |
| 3 | power-lease | 已注册（租约，采用） | 同上 + harness | 未验证 |
| 4 | gunyah-channel | 已注册（租约，采用） | 同上 + harness | 未验证 |
| 5 | share | 已注册（设备，authority 判定） | 归档：快照 10 + authority 8 + 门控 7 | 未验证 |
| 6 | smmu-active | 已注册（设备，`gg_smmu_start` 适配层） | 归档：probe/enter | 未验证；默认 device 授权关闭，enter 不会执行 |
| 7 | gmu-load | 已注册（设备，`gmuCommand(1)`） | harness probe | 未验证 |
| 8 | gmu-start | 已注册（设备，`gmuCommand(2)`） | harness probe | 未验证 |
| 9 | hfi-client | 已注册（设备，enter 拒绝：队列 bring-up 未实现） | 归档：HFI 三套用例 42 例 | 未验证 |
| 10 | controller | 已注册（设备，enter 明确拒绝：无 CTX0 后端） | 归档：布局/队列模型 | 未开始 |
| 11 | fence-provider | 已注册（设备，enter 跑 `ggfn_import`） | 归档：导入/布局模型 | 未开始 |
| 12 | submit | 已注册（设备，enter 拒绝：IB ring 未实现） | 无 | 未开始 |
| 13 | scanout | 已注册（设备，enter 拒绝：display target 未实现） | 无 | 未开始 |
| 14 | metal-publish | 已注册（设备，enter 拒绝：acceleration 未实现） | 无 | 未开始 |
