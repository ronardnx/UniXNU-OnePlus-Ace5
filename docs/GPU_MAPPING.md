# GPU 映射层

本文对应 `GiuliaGPU.cpp`、`giulia_gpu_dev.h`、`giulia_gpu_mapping.h`、
`giulia_gpu_mapping_impl.h`、`giulia_gpu_memstore.h`、
`giulia_gpu_memstore_mapping.h`、`giulia_gpu_secure_mapping.h`、
`giulia_gpu_ipc.h`、`giulia_gpu_ipcc_owner.h`、
`giulia_gpu_fence_provider.h`、`giulia_gpu_fence_mapping.h`、`giulia_gpu_fence_impl.h`、
`giulia_gpu_platform_impl.h` 和
`scripts/gpu_terminal_native.py`。2026-10-23 的补齐范围是已有 A750
开发路径的地址、权限、分配请求与失败生命周期；设备执行状态仍以实机记录为准。

## 地址与所有权

| 资源 | CPU / 物理内存 | GPU / GMU 地址 | 当前约束 |
|---|---|---|---|
| 普通开发池 | 16 段各 4 MiB、CPU Normal NC；段间物理地址可不连续 | GPU `0x61000000 + offset`；普通 GMU 同址 | 4 KiB 叶页、36 位 PA；首 4 MiB 专供 GMU cache 地址区分配 |
| GMU cache 地址区 | 与普通池首段共用已拥有的 backing | GMU `0x00800000 + offset` | 仅 flags bit7 选择；普通 GMU 别名无效，PTE 仍为 Normal NC |
| 页表 | 独立、连续且显式发布的 CPU copyback 内存 | CB0 GPU / CB2 GMU 的各自根表 | 表页不可重叠数据，不允许跨分支或跨层复用表页 |
| GMU 固件 / HFI / log / VRB | 现有固件预留与独立已固定内存 | 原固件地址；HFI `0x60010000`，log `0x60030000`，VRB `0x60040000` | 普通池编辑不触及这些叶页或内存内容 |
| memstore | 独立连续 32 KiB、CPU Normal NC；无开发池 offset | GPU `0x66000000`；请求 GMU_ACC 时 GMU 同址 | 固定单例，GPU privileged RW / NX，GMU 权限独立；不支持 cache 地址区别名 |
| IPC_CORE | 固定 DT 物理窗口 `0x400000 / 0x140000`；不分配 RAM、不映射 CPU、不清零 | 仅 GMU `0x67000000`，GPU 地址为 0 | 320 个 4 KiB 叶页；取得宿主互斥租约后才生效，保留至重启 |
| hardware-fence queues | 内核提供者的真实 CTX0 描述符；默认无提供者 | 仅 GMU `0x68000000`，GPU 地址为 0 | 2 MiB 全零预留拓扑；验证 epoch/READY/布局后只导入真实长度，尾部保持零 |
| secure 池 | 512 段各 64 KiB，交给 CP_PIXEL 后禁止 CPU 数据访问 | CB3 / SID2，`0xf0000000` 起的八个 4 MiB 槽 | 独立 SCM 所有权与页表；仅已分配部分按 4 KiB 启用，槽尾与未用槽无效；无 GMU / 普通池别名；保留至重启 |

普通池的分配记录、两个游标、对齐、范围和相互重叠在分配前统一检查。
开启 managed policy 后，CPU 写入/清零必须完整落在一个已分配对象内。
CPU 读回及物理地址查询仍是全池诊断接口，不能视作按任务或 HOST_ACC 的隔离。
HOST_INIT 只表示初始化请求，不是 CPU 写权限。

## 权限与更新顺序

GPU、GMU 分别使用各自 ACC / PRIV / WRITEABLE 位。普通 GPU 数据为 NX；
未请求某个 master 时对应叶页无效。首次发布根表前，普通池、两个 GMU
地址区、memstore 和 IPC_CORE 的叶页均已撤销有效位；页表只保留拓扑与固定的
物理地址。开启 policy 时重新核对普通池与两个 GMU 别名，再按已有分配
记录启用权限；未分配页面保持无效。GMU 固件 / HFI / log / VRB 不受影响。

页表编辑前检查完整表树和全部受影响页面，随后执行撤销旧叶页、发布页表、
TLB 同步、安装新叶页、再次发布及同步。活动 CB0/CB2 必须匹配固定的
4 KiB / 32 位 IOVA 配置、完整 TTBR/ASID、MAIR 和 stream 路由且无有效故障。
上下文未启用时不读取或认可复位状态下的 TTBR。secure 页表预建完整
32 MiB 拓扑但初始叶页无效；每次分配仅使向上取整到 4 KiB 的请求范围
有效。分配与 OP16 审计同时核对真实内存描述符的 PA / 长度、页表叶页和
槽记录，且不会访问交给 CP_PIXEL 的数据。secure 分配检查 CB3 完整配置、
故障和 TLB 状态，完成页表发布及 TLB 同步后才输出 `mapped=1`；失败保留
可能已生效的资源并隔离后续操作。

SQE/AQE 只能从校验过的固件包复制到已有 generic、GPU 可访问分配。
managed 模式先撤销该分配的所有设备别名并同步，然后清零整个分配、复制
固件并安装 GPU 只读可执行权限，GMU 别名无效。开启 policy 之前装入的
固件也记录 UCODE 属性，之后不会被误映射为 NX 数据。装载后整个分配封存，
普通写入、清零和再次复制均拒绝。

映射失败后保留分配记录和 backing，并隔离后续 CPU 修改、固件装载、
分配和 HFI 提交；不把可能仍被设备引用的地址重新分配。原 `gpu.hfi`
命令也不能绕过该隔离。主机 native HWSCHED 入口要求显式 `--platform`
及 ABI3 policy。重复开启 policy 会重新审计并返回 `managed=1`，不会重建
正在使用的表。

## HFI 内存种类

种类编号来自本地固定 OnePlus `adreno_hfi.h`；处理依据为同版本
`adreno_gen7_hwsched_hfi.c` 的 `get_mem_alloc_entry` / `process_mem_alloc`。

| kind | 原厂用途 | 当前处理 |
|---|---|---|
| 0–4 | generic、ringbuffer、scratch、SMMU info、非安全 privileged context | 普通池，按请求映射 GPU/GMU |
| 5 | secure privileged context | 仅显式 secure 后端，flags `0x1007`；普通池拒绝 |
| 6–10 | nonpriv context、counter、preemption counter、syslog、crash dump | 普通池，按请求权限映射 |
| 11–13 | DPU、TCSR、QDSS STM MMIO | 拒绝用普通 RAM 替代；需专用资源与所有权后端 |
| 14–18 | profile、profile IB、command、GPU busy data/command | 普通池，按请求权限映射 |
| 19 | IPC core MMIO | OP19 导入固定窗口，GMU-only；与宿主 IPCC 使用者互斥 |
| 20 | IPCC AOSS MMIO | 固定源尚未证明板级资源绑定，拒绝替代映射 |
| 21 | LPAC nonsecure privileged context | 普通池，按请求权限映射 |
| 22 | memstore | OP17 导入独立固定单例；请求须匹配现有权限，重复请求不清零 |
| 23 | hardware fence | OP20 支持已注册提供者的 GMU 导入；默认无真实 Gunyah 提供者，拒绝并保持未映射 |
| 24–25 | preemption scratch / AQE buffer | 普通池，按请求权限映射 |

主机按 `(kind, GMU handle)` 缓存分配；完全相同的重复请求复用原映射，
不重复清零、分配或转移 secure 所有权。同一键的大小、flags 或对齐变化
直接拒绝。回复保留固件请求的逻辑大小；已拥有的 backing 按页/请求对齐。
请求格式、内核返回的地址和大小、分配数及连续请求数都有边界检查。
没有成功建立相应资源时不会发送成功分配应答。

## Memstore 专用资源

`OP=0` 保留原 ABI1 状态行，并增加 `resources_abi=1 memstore=1
secure_allocation_mappings=1` 资源能力行；平台协议仍为 ABI3。`OP=17` 参数为
`bytes,wire_alignment,flags,22`，从固定提供者借用已拥有的 32 KiB backing，
不消耗普通池。对齐保留 HFI 原始编码：低字节为 VA 对齐指数、次字节为
大小对齐指数。逻辑大小及按请求对齐后的大小不得超过 backing，固定 IOVA
必须满足 VA 对齐；回复报告完整 backing 大小。GPU 的 ACC / PRIV /
WRITEABLE 三位必须全开，GMU 权限按独立三位设置；GMU_CACHEABLE、secure
和未知 flags 均拒绝。同一单例后续 flags 变化也拒绝；HOST_INIT 不触发
再次清零。该资源不暴露普通开发池 offset。

结构来源固定在 [memstore 源码清单](../hardware/gpu-memstore-source.json)：
原厂 `kgsl_devmemstore` 为十个 u32、40 字节，SOP 位于 `+0`，EOP 位于
`+8`；gen7-9-1 支持 APRIV，原厂设备 memstore 预分配为 privileged、GPU
可写资源。原厂大小公式是 `8 * PAGE_SIZE`；本工程显式选用 `8 * 4096`
字节，这不证明原厂内核的 host PAGE_SIZE，也不使用 XNU PAGE_SIZE。
按该固定容量，允许观察的 context 为 0–812，其中 812 为 global context、
1–811 为用户 context 范围；ringbuffer 槽和尾部不完整记录不作为 context。

`OP=18` 参数为 `context,0,0,0`，在 CPU 读屏障之间读取两组 SOP / EOP。
`stable=1` 仅说明两次样本相同；它不保证 GPU 原子快照、context 已注册或
命令完成。返回始终包含 `completion_verified=0`，不伪造时间戳、不提供
硬件 fence 等待，也不将读取行为当成提交成功证据。

## IPC_CORE 与共享仲裁

IPC_CORE 的 `OP=19` 参数为 `bytes,wire_alignment,flags,19`。内核另以
`ipc_core=1 ipc_host_arbitration=1 ipc_retained_until_reboot=1` 宣告能力；
旧内核缺少能力时主机不发该命令。flags 仅允许 GMU_ACC / PRIV / WRITEABLE
和 BUFFERABLE（掩码 `0x78`），GMU_ACC 必须开启；GFX、HOST_ACC、HOST_INIT、
cacheable、secure 与未知位均拒绝。逻辑大小须在 `1..0x140000`，VA 对齐
指数最多 22，大小对齐指数最多 18，并核对圆整后大小不超过真实资源。
固件回复保留逻辑大小，内核资源记录报告完整 `0x140000` 窗口。

IPC_CORE 使用固定原厂 `get_attrs` 对应的 Normal NC、outer-shareable
GMU 描述符；不是 CPU Device 映射，也未声称原厂为其设置 NX。根表准备时
只建立 GMU 拓扑并撤销全部叶页有效位，不建立普通 GPU 别名或 CPU 指针。
首次导入完整预检并原子取得仲裁权，再发布 PTE、同步 CB2；任何含糊失败
保留映射状态和租约，隔离后续修改。相同 flags 的导入复用单例，不写资源
内容；flags 变化拒绝。OP16 同时核对固定 PA、叶属性与仲裁 token。

仲裁只管理本内核参与者，不代表其他执行世界的所有权或电源许可。Wi-Fi
与音频可同时持有宿主成员资格；GMU 全窗口租约与二者互斥。Wi-Fi 在创建
IPCC 物理映射前 claim，失败回滚，真正释放映射后 release；QMP close
不会提前释放资格，quarantine 保留。Audio platform 成功注册时持有资格
至重启，注册前不得映射/访问 IPCC；重复注册不能释放已注册者的资格。
生成脚本覆盖 Wi-Fi / GPU / Audio 的八种组合，仅有 GPU 时启用对应 gate。
GMU 导入还要求 UEFI QMP 交接未保留不确定通道；导入后到重启前拒绝新的
宿主成员，不进行未验证的在线回收。
GPU 配置的原始 `reg.map` 同时拒绝任何与该物理窗口相交的范围，防止
调试终端创建未登记的 CPU 别名绕过仲裁；其他 MMIO 窗口不受此限制。

## 验证与尚未完成部分

硬件 fence 的内核注册、IPCC 配对、kind23 导入及 OP21 只读状态已接入，
详见 [fence 提供者接口](GPU_FENCE_PROVIDER.md)。它不包含真实 Gunyah
生产者、队列提交或完成后端，默认返回 provider-not-registered。

```bash
bash scripts/test-ios16-gpu.sh
python3 -m unittest discover -s tests -v
git diff --check
```

测试直接执行生产映射、分配、worker 和平台方法，并以独立页表遍历核对
33,792 个普通池/别名叶页；覆盖 256 组权限组合、跨不连续物理段、零叶页
别名、损坏的上下文、UCODE 装载顺序、失败隔离和固件请求重用。
C/C++ 主机测试使用 ASan/UBSan。主机寄存器模型不证明 ARM 总线、缓存
一致性或真实设备翻译。

本轮 WSL Ubuntu 验证：带固定原厂固件输入的完整 GPU runner 通过；
包括 890 个控制场景、33,792 个普通池/别名叶页、8,192 个 secure 叶页、
memstore / IPC_CORE / fence 导入实际命令及发布/TLBI 失败路径。主机 NativeHfi 专项
55 项通过；原厂固件内存准备覆盖 230 个失败场景。IPCC 仲裁覆盖 36,000
次并发竞争，生产 Wi-Fi 映射及 Audio 注册函数分别在 gate 开/关配置中测试。
原始终端窗口拒绝测试覆盖边界交叠与无 GPU 配置。

Wi-Fi runtime runner 通过；GPU / Wi-Fi / Audio 的八种 GUI 组合补丁应用
检查通过。新增能力标记及三个 IPCC 导出符号已加入内核产物检查。
当前全仓 Python 回归快照为 271 项：268 项通过、1 项屏幕 C packet fixture
及 2 项需显式路径的库存集成测试按条件跳过。库存 module+boot 已另行通过
只读校验器验证；合成提供者测试不代表真实控制器。`git diff --check` 通过。
本轮日志保存在 `reports/gpu-fence-20261023/`，前轮日志仍保留；未执行完整
XNU 内核构建。

kind11–13 / 20、hardware fence 的真实平台/完成后端、按任务的地址空间、生产回收、完整提交和
Metal 集成仍未实现。固定源尚未给出其他 MMIO kind 的确定板级绑定。
[硬件 fence 源码清单](../hardware/gpu-fence-source.json) 核对了真实队列表头、
共享表、IPCC signal 和 Gunyah 依赖。原厂注册采用静态布局，没有可伪造的
注册 ACK；实际 fence 操作仍要求 peer READY、精确共享内存所有权和
Gunyah doorbell。库存模块指令与独立 boot 配置现已分别证明 4 KiB，但构建
配对及活跃控制器仍未证明，见 [库存证据](../hardware/gpu-fence-stock.json)。
队列地址仍需真实提供者验证后才能导入，不能用条件推导授权初始化，
也不能用 memstore 冒充完成事件。
本轮没有刷机、执行 SMC、提交 RAW 或新增实机验证。
