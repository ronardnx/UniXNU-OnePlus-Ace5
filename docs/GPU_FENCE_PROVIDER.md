# GPU hardware-fence 导入接口

当前已实现内核提供者注册、专用 GMU 映射及只读队列校验；原生 Gunyah
通道 owner 已接入 GPU worker，但共享 backing、CPU mapper 和本地 CTX0
工厂尚未合并成真实提供者，也没有 fence 完成后端。默认没有注册对象，kind23 返回不可用；不以普通
RAM、memstore、虚构句柄、固定 READY 位或合成 ACK 替代。

已补齐独立的 [Gunyah 传输与查询会话](GPU_GUNYAH_TRANSPORT.md)：内核
HVC adapter、RM 分片、动态 VM 查询和失败隔离已连接。它依赖真实通道
所有者提供租约，不自动注册 fence，也不把 parcel lookup 当作共享授权。
该层现包含空闲通知 pump 与 primary 精确 carveout 的 assign/share 事务；
事务结果仍不能直接生成 provider 的全部 proof bits。

## 固定输入

来源见 [原厂源码](../hardware/gpu-fence-source.json) 和
[库存二进制](../hardware/gpu-fence-stock.json)。库存 `msm_hw_fence.ko` 的
客户端大小/偏移取整指令为 4 KiB；独立库存 boot.img 的 IKCONFIG 为
`CONFIG_ARM64_4K_PAGES=y`。两份产物构建版本不同，不能证明完整配对或
当前运行控制器、VM 与内存所有权状态。

`scripts/check-gpu-fence-stock.py --module MODULE [--boot BOOT]` 只接受显式
普通文件，核验摘要、ELF 符号/指令、Android 容器与 IKCONFIG，不连接设备。

固定布局包含 24 字节 table header、两个 56 字节 queue header、每队列
800 个 64 字节 payload。size/offset 是**字节**；索引是 **dword**，
须小于 12800 且按 16 dword 对齐。`start_addr` 是物理地址，不能换成 GMU
IOVA。status、watermark、IRQ 等原厂初始化器未设置的字段不作为就绪证明。
control queue 的 24 字节分配常量不能套用客户端 64 字节 payload 解释。

布局计算器支持 4K / 16K / 64K host 页几何校验，实际提供者仍须证明控制器
ABI。4 KiB 下 GPU CTX0 区域为 `0xd4f6d000 / 0x1a000`，来自完整 carveout
`0xd4e23000 / 0x2dd000`；计算结果不授权初始化或转移该物理内存。

## 内核提供者

`giulia_gpu_fence_register(const struct ggf_provider *)` 仅供内核代码调用。
注册复制并冻结 ABI、identity、epoch、layout、CPU 指针和回调，只允许一个
并发注册者成功。它不保留调用者可变结构体地址，不读取/清零队列，也不代表
资源就绪。没有终端、ADT 属性或启动参数能够注册提供者。

提供者必须将回调、cookie、identity 和精确 Normal NC 队列映射保持至
重启。有界只读 `observe` 回调必须依据真实平台状态验证：resources READY、
VM READY、精确共享 carveout、队列 PA/大小、host 页/控制器 ABI、peer 3、
memory label 5、doorbell label 6、真实 CTX0 client 1、CPU 一致性及 IPCC。
ABI1 的 `peer=3` 是板级 peer-name enum，不能填写动态 VMID，也不能用它
替代当前 epoch 的动态 self/peer 校验。`GGFP_CLIENT` 是本地独占 CTX0
登记和原厂等价队列初始化；固定源码的 controller registration 是 no-op，
没有可等待的远端 ACK。`page_bytes` 是 vendor 队列布局粒度，并非 XNU
CPU 页大小；源一致几何应由已支持的 ABI 和真实 header 验证。
这些证明不能实现为常量返回。epoch 失效必须上报，已映射 backing 不得在
设备复位前撤销、清零或重用。

提供者在访问 protocol4 fence 通道前必须取得
`giulia_gpu_fence_ipcc_claim(identity,epoch)`，并以对应 `..._owns` 核对。
配对要求 kind19 的 GPU IPC 租约已存在，只允许相同 identity/epoch 复用，
仍排斥 QMP/audio；配对后 GPU 租约不能提前释放。导入层不发 IPCC signal，
此软件配对不授予远端 VM 或共享内存的所有权。

## 终端与 HFI

`OP=0` 的 `fence_import_abi=1 fence_completion=0` 只表示接口已编译。

| OP | 参数 | 行为 |
|---|---|---|
| 20 | bytes,wire alignment,flags,23 | 验证提供者并导入 GMU `0x68000000`；返回真实 backing 容量及 epoch |
| 21 | 0,0,0,0 | 报告注册/就绪/映射状态；提供者有效时双读队列索引 |
| 16 | 0,0,0,0 | 同时审计提供者存活状态与精确 PTE |

本路径遵循固定源码的 GMU 导入方式，GPU 地址为 0。flags 仅接受 `0x178`
内的位，GMU_ACC 必须开启，GMU PRIV/WRITEABLE 独立控制；GFX、HOST_INIT、
cacheable、secure 和未知位拒绝。请求大小及对齐后的大小必须适合真实
descriptor；HFI 回复保留逻辑大小。

根表首次发布前只为 2 MiB IOVA 窗口建立**全零 L3**，不填写假定 PA。
导入预检全部 512 叶，只映射真实 descriptor 长度，尾部继续为零，拒绝
其他 IOVA 到该 backing 的别名。发布/TLBI 后再次确认 epoch/READY/布局，
成功才回复。失败保留描述符、backing 和租约。

同 key 的 HFI 重复请求仍重新进入内核验证，不能只依赖主机缓存。
已导入提供者失效时，普通资源/内存修改与 HFI 发送/消费均先检查并隔离。
主机收到任何内核 `code=8` 后停止后续设备 I/O，避免清理继续写 MMIO。
只读状态的 `stable` 仅表示两次索引相同，始终 `completion_verified=0`。
本实现不消费 payload，不修改队列索引，不启用 HFI feature25。

`giulia_gpu_fence_cpu.h` 计算 CPU 16 KiB 的完整队列 envelope，导入前后
核对同一动态 VM/parcel/epoch 和真实 mapper 审计。4 KiB 队列对应
`0xd4f6c000 / 0x1c000` CPU 范围，队列前后各有额外 4 KiB，必须一起获得
共享和缓存属性授权。仅 IOMemoryMap registry、传入 WriteCombine 选项或
unmanaged `pmap_cache_attributes` 都不能证明无 WB alias；该缺口现由
[CPU pmap 门控](GPU_CPU_PMAP.md) 的原生守卫关闭：`pmap_enter_options_addr`
只允许 attr index 1（Normal NC）别名固定 carveout，其他属性在提交前返回
`KERN_PROTECTION_FAILURE`，因此“无 WB 别名”成为执行点而不是观察结论。

`giulia_gpu_fence_pmap.h` 的离线描述符校验覆盖完整 CPU envelope 的每个
叶页，要求精确页数、连续 PA、Normal NC、AF、内核 RO/RW 与 PXN/UXN；
拒绝低地址 VA、48 位以外的 PA、DBM 可变权限和范围溢出。测试覆盖
4/16/64 KiB 粒度，包括中间页及边界填充页损坏。64 KiB 仅是解码器测试，
不扩大 `ggfc_import` 的 4/16 KiB 支持范围。`giulia_gpu_pmap.h` 与
`0086-giulia-gpu-pmap.patch` 现已实现真实的 boot 锁存、MAIR/TCR/T1SZ/TTBR1
核对、逐叶 `pmap_pte` 审计、直接映射/管理 RAM 别名窗口拒绝和 boot 期拒绝
锁存；`giulia_gpu_fence_native.c` 提供最小内核入口 `ggfn_import()`，冻结
provider 并用真实 `IOMemoryDescriptor` Normal NC 映射 + pmap 审计完成
`GGFC_AUDIT_REQUIRED`。仍未完成的是 share 事务运行时接入与控制器相邻页
授权，因此服务不会自动发起 CPU 导入，也不能据此注册 fence 提供者。

2026-10-23 已撤销此前合入 `sources/UniXNU` 的 QEMU EL2 工具修改，
四个文件逐一核对合并前 SHA-256，配套合并工具和测试移出活动目录。
撤销记录及原配套文件备份在 `reports/gpu-virtualization-revert-20261023/`。
这次撤销不改变 GPU 的 Gunyah 通道代码。

## 验证边界

纯 C 测试覆盖布局、索引、冻结注册、并发唯一注册者、全零预留拓扑、
PA 别名与失败原子性。实际生产方法测试使用明确标注的合成提供者，覆盖
缺失/失效证明、epoch 变化、重复导入、发布/TLBI 故障及后续变更隔离；
合成回调没有进入目标驱动。主机测试不证明物理控制器参与。
CPU pmap 门控的 RAM 页表模型另覆盖 boot 锁存、TTBR1/TCR/MAIR 策略、
逐叶审计、别名窗口、boot 期拒绝和只放行 Normal NC 的守卫，见
`reports/gpu-native-pmap-20261023/`；它同样不执行真实翻译。

真实后端仍需 Gunyah RM peer/READY、共享所有权、doorbell 分发、跨 VM 锁、
fence 表句柄和 Tx/Rx/错误/复位生命周期。本轮未执行 HVC、SMC、设备 MMIO、
RAW 或硬件 fence 操作。
