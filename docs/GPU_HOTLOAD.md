# GPU 热加载接入与本轮修正

[English](GPU_HOTLOAD.en.md)

2026-10-24：NLH2 核心策略实测不等于 GPU 驱动热替换。主控本轮采集到
`gpu.status service=unavailable mode=0`、NLH `provider=0`；当前启动没有活动
GPU 实例或 GPU 硬件桥。源码修正尚未进入这份镜像，没有新增实机通过结论。

## 电源阻塞

最近启用 mode 3 的既有记录为
`out/ios16/gui-touch-keys-gpu-device-40002797063-followup1/07-gpu.status.log`：
HUB 请求 200 MHz 时 `phase=7 code=6 op=4 resource=3`，原生状态
`0x8000000000000013`，未取得有效频率读值，`confirmed=0x1002`。
`GpuPower.c` 的 SET_RATE 回调在调用 `SetClockFreqHz` 前先读旧配置，
同版二进制的错误路径据此指向 `GetClockFreqHz` 在旧频率计划为空时返回
`EFI_NOT_STARTED`，提前终止请求；这一调用边界仍需新终端后端记录确认。固定上游和
同版 ClockDxe 都支持 200 MHz；不能由该错误推断目标频率不受支持。

共享层后续须区分“旧频率计划尚未建立”与真正的设频失败，保持设频结果及
后读精确匹配；不能放宽 `GGPH_PREPARED` 或 power lease。原生 ClockDxe
依赖其频率计划、父时钟和电压 corner 账本，UEFI 协议指针不能在 XNU 运行时
直接调用。先补齐有所有权的终端资源后端，实机验证后再合并 UEFI。

## GPU 目录内已修正的链路

- GMU 加载器继续校验完整 GGFWPK1 容器；服务改传整个固件包，避免把裸 GMU
  成员传给整包解析器。AHB fence 改为原厂公式的 `0x80c808a0`。
- GMU 的 load/start 共用阶段机的设备写入授权、前置阶段和隔离检查，USB
  不能绕过默认关闭的写入策略。拒绝发生在状态机和硬件操作之前。现有寄存器
  配置只是一部分，不能替代完整 HWSCHED 启动后端。
- SMMU 日志适配器保留原生库已有的单向回读规则：只对当前 bank 的 32 位
  TCR2 允许 TBI 由 0 读为 1，对 SCTLR 允许 ASIDPNE 由 1 读为 0。其他位
  仍精确检查，整幅 context 图像继续由 ArmSmmu4K 校验。
- HFI MEM_ALLOC 回答保留逻辑请求大小；legacy 请求也回答完整 11 dword
  描述符，末项 align 为 0。大块分配按独立 4 MiB CPU 映射逐段清零和发布，
  不把相邻 GPU VA 当成连续 CPU VA。
- 自动分配 pump 只在有阶段授权的原生 HFI client 活动时工作；手工终端模式
  保留队列控制权。pump 验证完整队列头和报文边界，保留 ACK/error 给明确
  消费者，拒绝后锁存停止。应答入队后执行真实屏障和 HOST2GMU 门铃；失败
  隔离，不重放。`replied` 表示已发布并发出通知，不表示固件已消费或 ACK。
- share、GMU、SMMU、fence/HFI 仍留存时，阶段退出报告未确认，阶段机停止
  反向释放并隔离；不再把“保留到重启”的日志当作成功 STOP。

固定上游为 OnePlus graphics/device tree
`453d8875f815bf9dfad9035982861226a4823a6c`；SCM kernel 为
`d763ed47deec55451366c1187f813f2f309d88a7`。HWSCHED 目标顺序见
`adreno_gen7_hwsched.c::gen7_hwsched_gmu_first_boot`；不能采用另一条 legacy
路径的 PDC/RSC 时序。已有来源细节见 [安全启动审计](GPU_SECURE_START_AUDIT.md)。

## 热替换边界与下一步

GH ABI 1 的首批实际入口为
`hfiPumpOnce → ggpp_pump → gga_handle → hfiPoolDecode → giulia_gpu_hotload_decide → giulia_hotload_select`。
生产入口为 `extern C/noinline`，内部直接调用公共桥，HFI 边界直接调用入口；
不依赖函数指针去虚拟化来证明链接。可移植回调辅助函数复用同一输入准备和
输出提交实现。
模块源码为 `ios16/gpu/hotload/giulia_gpu_proto_module.c`，名称
`giulia-gpu-proto`，service/interface `0x47480001`/state ABI 1。13 个 u64
请求包含 schema、实际报文长度和11个逐字复制的 u32；响应是错误码、有效权限、
所需映射域、专用资源标志与圆整长度组成的 u64。模块通过 invocation ticket
索引读写，常驻侧完整比对固定上游安全包络后，把响应解码为内部计划。
普通 SYSRAM 分配的圆整长度、有效权限、GPU/GMU 映射分支和 backing 校验
实际读取该计划；专用资源仍由原始 kind 选择各自 owner。回复继续保留原始
flags、逻辑 size 和 align。不可用、更新忙、旧
epoch 或非法响应保留常驻解析路径。`gpu.status` 的 `GIULIA GPU PROTO` 行
报告 calls/selected/fallback/rejected；其中 selected 只表示已核验的协议结果，
可能是协议拒绝。单独的 module_allocated/module_reused 计数只在分配或缓存
复用真正成功后增长，也不表示 DMA、GMU 执行或 GPU 加速验收。模块计划仍须
与固定上游等价，没有为制造 changed 计数修改硬件参数。

纯模块私有状态为32字节，迁移快照40字节；START/STOP管理私有运行和在途
调用，EXPORT/IMPORT只迁移计数。`GG_PROTO_FAIL_START`、`GG_PROTO_FAIL_IMPORT`、
`GG_PROTO_FAIL_STOP` 可用于统一故障验收。协议模块不拥有硬件资源，模块
STOP成功不表示GPU停机。当前原生HFI client尚未实现，mode0没有服务，实际
firmware包覆盖仍待统一实测，不能以编译或人工请求替代。

IOService、锁和 worker、固件、DMA backing、SMMU 页表及租约、安全所有权、
HFI 队列/待处理 ACK、IRQ/fault owner、Gunyah epoch、share 和 fence provider
必须常驻。普通 NLH_CALL 的标量不能转换为内核指针。常驻桥必须收到装载器
验证的 image identity、version、generation 和生命周期 phase，使用类型化
请求和常驻 opaque handle；模块名称不能授权硬件写入。

开放原生启动前仍需完整实现 ACD/电源/时钟依赖、AHB timeout/timer、PDC/RSC、
secure CX、完整 register/version 初始化、IRQ/fault、DDR 带宽、HFI feature/
performance/START、controller 和 fence。停止还需 idle/slumber、RSC sleep、
GBIF halt、IRQ/worker 排空、最后一次 HFI drain、DMA 和 fence 确认，之后才能
撤销 stream/context、同步 TLB 和回收资源。现有 `platformIdle()` 要求 bank
全部已停，不能用来证明运行中的 master 已停和 DMA 已排空。

主控统一执行终端采集和后续构建。本轮没有运行本地主机测试套件、访问设备、
集成 UEFI、提交或推送。完整内核编译、产物核验和新镜像实测仍待统一完成。
GPU 提交、真实 fence、渲染、Metal 和整体驱动重载均未验收。
