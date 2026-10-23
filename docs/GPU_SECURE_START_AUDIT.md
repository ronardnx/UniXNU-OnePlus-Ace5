# A750v2 安全启动源码审计（2026-10-23）

下文的“未接入候选”描述 08:20 时的审计状态。后续实现已接入显式
[平台 ABI 2](GPU_PLATFORM_ABI.md)，实机结果和构建进度见
[终端记录](GPU_TERMINAL_BRINGUP.md)；源码依据与安全所有权要求不变。

本文记录固定原厂源码的启动顺序、内存所有权和窄 SCM 候选接口。
`ios16/platform/giulia_scm.h` 当前只供离线验证，未接入 `GiuliaGPU` worker 或
`gpu.dev` 命令；该候选接口没有构建进新内核，也没有在设备执行 SMC。
安全分配器、secure context 和完整故障 IRQ 处理尚未完成，因此不能据此
解除 CP/RAW 提交禁用。实机时间线和结果由 [终端记录](GPU_TERMINAL_BRINGUP.md)
及相应 `hardware/` 汇总维护，本文不复制这些结果。

**固定来源与目标。** 模块源码来自 OnePlus
`android_kernel_modules_and_devicetree_oneplus_sm8650` 的
`453d8875f815bf9dfad9035982861226a4823a6c`；内核 SCM 源码来自
`android_kernel_oneplus_sm8650` 的 `d763ed47deec55451366c1187f813f2f309d88a7`。
本文行号按下载的原始文件核对，不能用搜索工具重新排版后的行号替代。
设备匹配的是 `gen7_9_1`，复用 `gen7_9_0_hwsched_gpudev`；其能力包含
AQE、BCL、LPAC、内容保护，没有 CLX。[目标定义][profile]

**原厂顺序把三个不同的安全工作分开。**

1. GMU 首次启动先开启 CX 和时钟、配置超时检测与计时器、加载固件及
   PDC/RSC，再执行 `gen7_scm_gpu_init_cx_regs()`，随后配置 GMU 寄存器、
   版本、IRQ 和最低 DDR 带宽，才调用 `gen7_gmu_device_start()` 和 HFI
   启动。这是发生在 GMU 启动之前的 secure-CX 寄存器初始化。[GMU 首次启动][gmu-order]
2. HFI 初始化先配置性能、带宽和功能；`CORE_FW_START` 之后查询 context
   queue，再配置 HW fence、preemption、LPAC、AQE，最后发送 `START`。
   AQE 功能号为 29，启用报文数据是 `{29, 1, 0}`。固件要求的内存由
   带权限的分配应答处理，不能用 `START` ACK 推导之后不会再申请内存。
   [HFI 启动顺序][hfi-order]、[AQE 功能号][aqe-id]
3. GPU 启动调用 `kgsl_mmu_start()`、OOB 唤醒、`gen7_start()`，清除旧
   FSR 并建立 GPU IRQ 处理，设置 SQE/AQE 指令地址后进入 GPU 冷启动。
   第一组 RAW 命令是 `CP_THREAD_CONTROL` 加 `CP_ME_INIT`，然后等待
   ACK 和 idle。固定路径没有在 CP_INIT 前发送试探性 RAW NOP。
   [GPU 启动][gpu-order]、[CP_INIT 命令][cp-commands]
4. `gen7_hwsched_cp_init()` 在 CP_INIT 成功后才加载并认证 ZAP，再通过
   `CP_SET_SECURE_MODE(0)` 退出安全模式。A750v2 的 `zap_name` 非空；
   加载失败直接返回，不会进入直接写 TRUST_CNTL 的分支。
   [CP_INIT、ZAP 与模式切换][zap-order]、[ZAP 加载结果][zap-result]

因此，secure-CX 缺失是明确的前置流程缺口；ZAP 缺失会阻止完整安全模式
切换。仅凭 ZAP 尚未加载，不能断言它导致了 CP_INIT 前的 RAW 失败。
`RBBM_SECVID_TSB_*` 的寄存器值也不能证明物理内存已获得安全所有权，或
ZAP 已认证。失败原因仍需独立的 IRQ、fault、取指和内存证据。

**HFI secure 分配不能借用当前 64 MiB 开发池。**

`HFI_MEMFLAG_GFX_SECURE` 是 bit 12，与 `GFX_PRIV` 的 bit 1 不同。
`GFX_ACC`、`GMU_ACC`、`HOST_ACC` 和 `HOST_INIT` 也是分别声明的权限。
原厂将 secure 位转换为 `KGSL_MEMFLAGS_SECURE`，分配 GPU global 后仅在
`GMU_ACC` 存在时导入 GMU 映射。[HFI 权限定义][memflags]、[分配处理][allocation]

原厂 secure global 分配进入独立路径：先获得系统页和 scatterlist，再由
`hyp_assign_table` 将所有权从 `VMID_HLOS` 转为 `VMID_CP_PIXEL`，权限为
读写；secure memdesc 的操作表没有普通内核映射回调。若所有权转换失败且
返回 `-EADDRNOTAVAIL`，原厂认为页的安全状态未知，不能放回分配池。
这不是设置一个 SMMU PTE secure/privileged 位能完成的操作。
[所有权转换与失败语义][ownership]、[secure 分配][secure-pages]、[secure global][secure-global]

原厂另建立 `gfx3d_secure` IOMMU domain，设置 `VMID_CP_PIXEL`、附加设备、
注册故障处理器并建立 secure pagetable；secure 映射使用这个 domain。
普通 GPU 用户页表和 GMU 页表不能代替该路径。
[secure context][secure-context]、[secure 映射][secure-map]

当前 `giulia_gpu_dev_impl.h::prepareDevMemory()` 将整个开发池预先映射到
普通 GPU 和 GMU 页表；首个 4 MiB 还有 GMU cache 地址别名。CPU 保存可直接
访问的 `IOBufferMemoryDescriptor`，`devPointer()`、读写、清零和固件复制
操作都能触及这些页。因此不能从池中取一段内存，补上 `0x1000` 标志后就
回答 secure 请求，也不能把非安全句柄作为 secure GPU 地址返回。

后续移植需要独立管理安全页的物理所有权和生命周期：

- 安全候选页从一开始就不进入普通 GPU/GMU 全池映射，也不能经开发接口
  的 CPU 指针、读写或清零操作访问。需要初始化时，应在安全所有权转移前
  完成，并明确撤销临时访问；转移后不得以回读作为验收手段。
- 对没有 `GMU_ACC` 的 secure 请求不创建 GMU 别名；没有 `HOST_ACC` 或
  `HOST_INIT` 不能自动执行当前开发池的 CPU 清零/回读流程。相反权限组合
  需要逐项证明可实现，不能凭记忆猜测固件意图。
- secure domain 必须拥有明确的 SID、context bank、VMID、映射及故障处理，
  并单独验证。仅因为已知 secure SID=2，不能直接激活该 stream。
- 所有权未知、secure unmap/shutdown 未确认或 DMA 尚未排空时，保留并隔离
  相关资源；不能复用为普通开发池，也不能向固件伪造成功应答。

这里“不保留可用 CPU/GMU 开发别名”是结合本移植全池映射方式提出的实现
约束。原厂证据直接证明的是所有权转换、secure memdesc 无普通 map-kernel
回调、以及按 HFI 权限决定 GMU 映射；本文不声称 Linux 会删除所有页的
线性映射 PTE，也不以虚拟地址仍存在推导 HLOS 仍获准访问物理内存。

**ZAP 使用另一套 PAS 认证流程。** 原厂从 `zap-shader` 子节点解析
`memory-region`，核对固件所需大小，映射指定 carveout，再调用
`qcom_mdt_load(..., GPU_PASID=13, ...)` 和 `qcom_scm_pas_auth_and_reset(13)`。
它既不是上述 HFI secure 缓冲区分配，也不是 secure-CX 的标量请求。
当前固件包包含 ZAP 文件不等于 carveout 归属、MDT 装载或 PAS 认证已实现。
[ZAP 装载流程][zap-loader]

**窄 secure-CX 候选 ABI。** `gen7_scm_gpu_init_cx_regs()` 根据目标能力
组合请求。A750v2 的固定值是 `ALWAYS(1) | BCL(2) | TSENSE(8) = 0xB`；
CLX bit 2 不设置。底层服务为 SiP service `0x28`、command `1`，只有一个
值参数，不涉及 DMA buffer、扩展参数数组或 PAS。
[请求组合][cx-request]、[请求位定义][cx-bits]、[SCM 调用][cx-scm]

| 阶段 | x0 | x1 | x2 | 判定 |
| --- | --- | --- | --- | --- |
| SCM64 convention 探测 | `0xC2000601`，FAST | `1` | `0x02000601` | 原始 a0=0 且 a1=1 |
| GPU 初始化可用性查询 | `0x42000601`，STANDARD | `1` | `0x02002801` | 原始 a0=0 且 a1 非零 |
| 固定 secure-CX 初始化 | `0x42002801`，STANDARD | `1` | `0xB` | 原始 a0=0 |

上述函数号由固定源码的 owner、convention、服务和命令宏推导，不是实机
返回值。初始 x3–x7 清零。初始化接口不以 a1 为状态；原厂传入空结果指针。
候选仅支持已探测成功的 SCM64，不回退 legacy、SMC32 或其他机型特例。
[convention 探测][convention]、[可用性查询][availability]、[SMC 参数组装][smc-args]

`qcom_scm-smc.c` 对 INTERRUPTED=1 重新进入原调用：x0 改为 1，x1 和
x2–x5 保持原参数，仅 x6 使用上次返回的 Qualcomm cookie。返回的 a1–a3
不是恢复参数。BUSY 重试重新使用原始函数号和零 cookie。候选保留完整
输入、输出 x0–x7，并限制返回后的重试次数；WAITQ_SLEEP=2、WAITQ_WAKE=3
立即隔离，因为本移植没有对应的 SCM waitqueue IRQ/resume owner。
[INTERRUPTED 恢复][smc-resume]、[WAITQ 与 BUSY][smc-wait]

原厂最终错误映射接受 `int`，但此前 success、INTERRUPTED、WAITQ 和 BUSY
比较使用完整的 a0。候选现在同样要求完整 a0=0 才成功，完整 1/2/3 才是
相应控制状态；只为已知 BUSY=-12 额外接受 `0x00000000fffffff4`，同时接受
标准符号扩展的 `0xfffffffffffffff4`。前者是明确记录的兼容扩展，不声称
原厂 BUSY 循环有相同处理，也不声称已在设备上观察到这种编码。
`0x100000000` 等高位异常值不能截断成成功，低位为 1/2/3 的同类值也不能
触发恢复或 WAITQ。原始 64 位结果始终保留。
[原厂最终错误映射][smc-result]、[错误码定义][scm-errors]

**离线候选的边界。** 头文件和 `tests/ios16_gpu_scm.c` 已进行 host
ASan/UBSan 测试，检查固定函数号、权限前置条件、禁止重复初始化、结果保留、
INTERRUPTED cookie、两种规范 BUSY 编码、重试上限与隔离。AArch64 和
Darwin arm64 C++ 编译检查确认完整 x0–x7 装入/保存和 `smc #0` 发射。
这些检查没有执行目标端 SMC，也没有验证 TZ 是否支持请求。

后续若接入，只能由具备当前启动所有权的 worker 使用固定操作；USB 解析器
不能执行 SMC，也不能接收任意 function ID、请求位或物理地址。已有
`giulia_reboot.inc::gr_smc()` 是 PSCI SMC32 接口，参数位置和返回约定不同，
不能复用为 Qualcomm SCM。UEFI 的 `ScmSipSysCall` 协议定义也不是 XNU
运行期可直接调用的服务。

单次 SMC 不返回时，主机等待超时不能中断它；“有界”仅描述返回后的重试。
成功状态只描述当前启动周期的一次调用，原始 MMIO 复位后不能继续复用。
完整安全分配器、IRQ/fault 处理和恢复机制完成之前，候选保持未接入状态，
不构建只增加此接口的新内核，不执行 CP/RAW。

**原始来源摘要。** 以下 SHA256 对本次重新下载的固定 commit 原始字节计算。
URL 中的 commit 是内容固定点；哈希用于发现缓存漂移，不表示代码已完成
移植或硬件验收。

现有缓存中的 `kgsl_util.c`、`kgsl_util.h`、`qcom_scm.c`、`qcom_scm.h`
各比原始文件多一个文件末尾 LF，已逐字节确认除此之外完全一致，引用行号
不受影响。本审计保留旧缓存以免改动已有实测来源摘要，表中列原始来源
哈希；这四份原始文件也重新匹配了已有 Git blob pin。重新下载比较的完整
元数据保存在本地 `.cache/gpu-source/secure-audit-hashes.json`。

| 文件 | SHA256 |
| --- | --- |
| [adreno-gpulist.h][profile] | `08293f60e0be8b3a16ec0b800588d700f54b34d2e9e32e64aac115f05548c8f3` |
| [adreno_gen7.c][cx-request] | `1d63fca08e1d45428b80342d3d35a8c4166ad93c74ade6d125d2b3f9f4267d5d` |
| [adreno_gen7_hwsched.c][gmu-order] | `5452e244e35f19e8d5f896f3fd1805edf420211f04cfc8f39decd003eb01bc21` |
| [adreno_gen7_hwsched_hfi.c][hfi-order] | `306930a131d0fa3bc3cc3873dbfaf205b6bd5a4243f55a74cced411c08de6782` |
| [adreno_hfi.h][memflags] | `03fe8212d1a75148999f67df2044133a047501e91a234d0c1176d264428f713c` |
| [adreno.c][zap-result] | `7610fc36f8a482f4a09a3d332c3ebb7505a8540281bf770f8e7b2f0d21346799` |
| [kgsl_util.c][zap-loader] | `0e37e004d4265f62c6eb03929640558ac5d59b2476bc25aa6ead31e361329acc` |
| [kgsl_util.h][cx-bits] | `e7b8c1a8fc598987a0f1b81279e7f06e6ee2b6d6f1be417f9d7dd8417124c18d` |
| [kgsl_sharedmem.c][ownership] | `69c46df984557e6c227b829b7c7191263dd8c6df939ee2572effa46cce5ce6e0` |
| [kgsl_iommu.c][secure-context] | `158fa146b934d10ddbdfc34e678863b00b2a0afc9f8b56e00c0193c5a07fc033` |
| [qcom_scm.c][cx-scm] | `6854c1aacb81a5d58fe15b4613f92deec848811b5e52803be3db0d9aa6ad29f8` |
| [qcom_scm.h][scm-errors] | `777ebd4d0389453868372f9e41610cfb461de66a11a202dc2123ef6b3aea836a` |
| [qcom_scm-smc.c][smc-result] | `3478b65bd2df3c3c0659b91ab0a80137392f4f2889b1dc963a68b7fbbc6a8e70` |

[profile]: https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/graphics-kernel/adreno-gpulist.h#L2418-L2454
[gmu-order]: https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/graphics-kernel/adreno_gen7_hwsched.c#L486-L562
[hfi-order]: https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/graphics-kernel/adreno_gen7_hwsched_hfi.c#L2382-L2478
[aqe-id]: https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/graphics-kernel/adreno_hfi.h#L54-L80
[gpu-order]: https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/graphics-kernel/adreno_gen7_hwsched.c#L788-L850
[cp-commands]: https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/graphics-kernel/adreno_gen7.c#L224-L270
[zap-order]: https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/graphics-kernel/adreno_gen7_hwsched_hfi.c#L2547-L2605
[zap-result]: https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/graphics-kernel/adreno.c#L106-L119
[memflags]: https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/graphics-kernel/adreno_hfi.h#L280-L311
[allocation]: https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/graphics-kernel/adreno_gen7_hwsched_hfi.c#L1611-L1681
[ownership]: https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/graphics-kernel/kgsl_sharedmem.c#L596-L640
[secure-pages]: https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/graphics-kernel/kgsl_sharedmem.c#L1579-L1713
[secure-global]: https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/graphics-kernel/kgsl_sharedmem.c#L1891-L1936
[secure-context]: https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/graphics-kernel/kgsl_iommu.c#L2474-L2548
[secure-map]: https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/graphics-kernel/kgsl_iommu.c#L648-L654
[zap-loader]: https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/graphics-kernel/kgsl_util.c#L102-L175
[cx-request]: https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/graphics-kernel/adreno_gen7.c#L924-L957
[cx-bits]: https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/graphics-kernel/kgsl_util.h#L39-L58
[cx-scm]: https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/d763ed47deec55451366c1187f813f2f309d88a7/drivers/firmware/qcom_scm.c#L1690-L1704
[convention]: https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/d763ed47deec55451366c1187f813f2f309d88a7/drivers/firmware/qcom_scm.c#L187-L251
[availability]: https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/d763ed47deec55451366c1187f813f2f309d88a7/drivers/firmware/qcom_scm.c#L333-L361
[smc-args]: https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/d763ed47deec55451366c1187f813f2f309d88a7/drivers/firmware/qcom_scm-smc.c#L199-L223
[smc-resume]: https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/d763ed47deec55451366c1187f813f2f309d88a7/drivers/firmware/qcom_scm-smc.c#L32-L53
[smc-wait]: https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/d763ed47deec55451366c1187f813f2f309d88a7/drivers/firmware/qcom_scm-smc.c#L117-L195
[smc-result]: https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/d763ed47deec55451366c1187f813f2f309d88a7/drivers/firmware/qcom_scm-smc.c#L283-L291
[scm-errors]: https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/d763ed47deec55451366c1187f813f2f309d88a7/drivers/firmware/qcom_scm.h#L262-L288
