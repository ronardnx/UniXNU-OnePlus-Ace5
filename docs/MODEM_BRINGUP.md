# giulia 高通基带全链路入口

目标是 OnePlus Ace 5 / SM8650 的 SIM、双卡、注册、移动数据、通话、短信、IMS、持久化和恢复，
并保持原版 20H392 CommCenter。先读 [统一架构](MODEM_ARCHITECTURE.md)，再沿下表进入细节。
同一生产核心维护生命周期和业务状态，终端、数据适配器、后续 Apple provider 使用同一份状态。

## 全局依赖与实现位置

| 路径 | 固定证据与完整机制 | 已落地代码 | 原生执行边界 |
| --- | --- | --- | --- |
| UEFI → 内存保留 → XNU | [启动映射](MODEM_BOOT_MAP.md)、[原厂镜像清单](../hardware/sm8650-modem-stock.json) | 三个遗漏保留区的 MemoryMapLib overlay、prepare 漂移检查、固件覆盖核验 | 软件保留不等于 SCM 已转交 DSM，也不等于已运行 MPSS |
| 平台资源 → 固件/PAS → ready | [硬件映射](MODEM_HARDWARE_MAP.md)、[板级清单](../hardware/sm8650-modem.json) | `gpr` owner/epoch/token 账本、投票、DMA/内存转交隔离；启动/停止动作链 | PAS 4/38、低地址认证缓冲、CX/mss.lvl/XO/ICC、DSM 的物理 provider 尚未实现 |
| SMP2P/SMEM/IPCC → GLINK/IPCRTR → QRTR | 同版 remoteproc/SMEM/GLINK/QRTR 源、硬件图、机制清单 | 服务代次、地址失效、事务/超时、IPC_PREPARE 与固件启动顺序 | MPSS host 1 的原生 FIFO/IRQ/intent/QRTR 驱动尚未接入；ADSP 与 WLANFW 不是替代品 |
| Apple QMUX → QRTR → QMI | [QMUX 桥](MODEM_QMUX_BRIDGE.md)、[协议图](MODEM_PROTOCOL_MAP.md) | CTL/CID 分配释放、独占端口绑定、事务重写还原、重传、取消、SSR | QMUX virtual client 需要真实 QRTR socket 和 ABP 通道消费者 |
| SIM/双卡 → NAS → 数据会话 | [全部逐字段协议](../hardware/modem-qmi-service-map.json) | UIM/NAS/WDS 编解码、双槽代次、PIN 状态、IPv4/IPv6 与承载状态机 | PIN/PUK/STK、DMS/DSD、PDC/MBN、运营商策略有源定义，尚非完整业务执行器 |
| RMTFS/TFTP → EFS/NV → flush/reply | 原厂 `rmt_storage`/`tftp_server` 的 ELF 与 QMI IDL、上游固定协议 | 反向请求身份、重复请求、短写/flush、确认缓存、停止排空 | 原生共享缓冲与文件后端未接入；不能用空 NV 或无条件成功回复代替 |
| IPA 双向握手 → WDA/WDS → GSI/SMMU → QMAP | 31 个生产端点、IPA descriptor 与 message binding、WDS/WDA schema | 多阶段数据门槛、QMAP/basic/v5 校验头、mux/订阅归属、RX 接收凭据、背压 | 物理 DMA、filter/router、原生 ifnet/Skywalk、IP/DNS/route 实现仍缺 |
| IMS/VOICE/WMS → 媒体/交付 | IMSA/IMSP/IMS/VOICE/WMS/PDC 等固定 schema、原厂服务对象 | 注册应答与 indication 分离；网络呼叫与媒体分离；SMS 接受与送达分离 | 完整消息编解码、入呼/补充/紧急业务、ADSP 音频图、VoWiFi 路由尚未接通 |
| IOKit → CommCenter → CoreTelephony → PDP | [20H392 原始二进制 ABI](MODEM_APPLE_ABI.md) | 61 项真实 selector 形状、通知/工厂/通道证据、生成头、独立 cellular root 策略、产物门槛 | 原生 Apple provider/UserClient 语义和网络接口未实现；当前不会开放 managed |
| 飞行模式/休眠/SSR/停机 → 其他子系统继续运行 | [机制清单](../hardware/sm8650-modem-mechanisms.json) | 取消、服务重建、旧事件隔离、排空、顺序回收、不确定状态隔离 | 真实远端停止和 DMA 停止由对应 provider 证明；失败不释放仍占用资源 |

这些边界是实现清单，不是以设备连接状态替代分析。动态值的生产者已经指定：QRTR 产生 node/port，
服务版本协商产生可用消息集合，UIM 产生 provisioning 状态，WDA/WDS 返回格式和会话参数。
它们由当前运行代次取得，不能把其他设备的值或源码枚举值固化为本机成功结果。

## 三个已经消除的跨层歧义

1. 固件不是“未知 ELF64”。本地 PKG110 `16.0.3.500(CN01)` 原厂包已提取 55 个固件文件并核验。
   `modem.mdt` 为 ELF32 LE，实际加载 `[0x8bc00000,0x99e00000)`；DTB 为
   `[0x9b000000,0x9b003d3c)`。缺失的四个 split 编号是 `filesz=0` 的 BSS 段。
   驱动 `uses_elf64` 控制 coredump，不能据此解释输入固件。
2. D22 的实际 Apple 分支是 Mav17/QMI。原始 cellular/noncellular 作业使用同一 Label，
   由 ADT `baseband` 互斥选择；ADT、IOKit provider、UserClient 和 root policy 必须一致。
   原产品的订阅过滤只暴露 Apple slot 1。giulia 双物理槽和核心 slot `[0,1]` 不会自动变成双 Apple 订阅。
3. 原厂 MPSS 文件服务包含 RMTFS service `0x0e` 和 TFTP QRTR service `4096`。
   TFTP 不是 RFSA QMI `0x1c`。已检查的 40 个 IDL 对象没有 RFSA，不能推导为全部固件永久不支持，
   也不能凭相似进程名生成不存在的 RFSA 消息。

## 共用生命周期、资源与错误传播

`giulia_modem_contract.h` 定义命令、动作、事件、快照；`giulia_modem_runtime.h` 是唯一业务核心。
启动依次准备资源、存储、IPC、固件和通信。反向存储请求可在固件等待 ready 时处理。
数据连接经过 IPA、WDA、WDS 绑定、Start、地址读取、DMA、网络配置，全部成立才进入 UP。

`GiuliaModem.cpp` 负责内核串行队列和工作线程。底层接受请求不代表完成；完成事件必须匹配
epoch、operation、slot generation、endpoint generation、service、node、port 和 transaction。
provider 回调在核心锁外执行，超时依据单调时钟。共享内存、DMA、文件写入或媒体操作结果不确定时保留隔离状态。

`GiuliaPlatformResources.cpp` 管理共用账本。Wi-Fi/音频/GPU 的现有入口继续进入共同仲裁；
即使未编入 GPU，Wi-Fi 与音频也不能绕开 broker。AOP IPCC 0/0 可共享路由，完整 QMP 会话
另持唯一邮箱租约。modem 停止不能撤销其他 owner 的投票或租约。

## 统一构建、打包与验收

`prepare-ios16.py --gui --modem` 输出统一模块注册、modem 声明和终端能力。
cellular policy 从原 startup compatibility 独立出来；默认禁用，不连带改变音频策略。
rebase、payload 生成、打包和反向验证使用同一 kernel/ABI/ADT/root-policy 契约。
缺少已链接的原生 provider 时，managed 请求被拒绝；诊断框架不声明可用蜂窝接口。

唯一验收入口：

```sh
bash scripts/test-ios16-modem.sh
```

该入口核验原始设备树、固定源码、原厂固件、20H392 dyld/kernel/root；运行相同生产核心、
内核封装、平台账本、QMUX/QMAP 与连续业务/异常/恢复场景；验证八个 Wi-Fi/GPU/audio 组合的
完整 XNU 补丁链和依赖准备、UEFI 保留区、产物契约及整个仓库回归。只有全部检查结束才输出关联报告。
报告保存在 `reports/modem-system.json`，记录全部源码摘要和验证来源，运行中源码变化会拒绝验收。

默认 reference 为 `d68e05df...`，XNU 为 `xnu-8796.141.3`；缓存目录和依赖 pin 由入口脚本声明。
原始证据环境依赖见 `scripts/modem-test-requirements.txt`。CI `--recorded-evidence` 会明确标记
只检查提交的原始证据身份，不能冒充重新读取私有镜像。主机内核封装编译、ARM64 核心编译、
完整组合补丁应用与 CI 的真实 XNU 编译链接分别记录；补丁应用不是整个内核已编译的证明。

单条 TLV、单个返回码、一个业务状态位或多个局部测试通过都不是最终蜂窝完成结论。
机制清单明确记录 source-ABI、wire-codec、typed-runtime-contract、kernel broker 和原生执行层级，
保持文档、代码和报告对同一实现范围的描述一致。
