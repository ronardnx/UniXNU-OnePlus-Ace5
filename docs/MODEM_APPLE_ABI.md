# 20H392 Apple 蜂窝接口与 SM8650 统一控制器的连接

本文件描述原版 D22AP / iPhone10,3 用户态实际使用的协议、内核接口和启动条件。机器来源在 `hardware/ios16-modem-abi.json`；调用形状边界在 `ios16/modem/giulia_modem_apple_abi.h`。二者与 [整体生命周期](MODEM_ARCHITECTURE.md)、[启动映射](MODEM_BOOT_MAP.md)、[协议映射](MODEM_PROTOCOL_MAP.md) 合起来使用。

来源是本地 16.7.16 **20H392** IPSW、原 HFSX userdata、其配套 dyld cache 与原始 kernelcache。不能拿其他系统版本的 selector 或网上的接口声明填入本版本。本映射不是驱动运行成功的声明。

## 1. 先确定实际产品分支

```text
当前 GUI 产品身份 D22AP / iPhone10,3
  → 原 ADT /product/baseband-chipset = mav17
  → TelephonyRadiosDetermineRadio 的 D22 分支
  → device enum 0x40，radio enum 5，vendor enum 1
  → CommCenter: MCommandDriversFactory
  → BasebandManager: QMICommandDriverFactory
  → ABP 用户态 transport，qmux1 / qmux2 与 qmap / qmap_control
  → giulia 协议适配器
  → 统一基带控制器 → QRTR/QMI、IPA/GSI → SM8650 modem
```

这条选择链来自指令与数据流，不是因为镜像里存在某个 dylib：

| 决策 | 20H392 实证 |
| --- | --- |
| D22 的硬件键 | `__TelephonyRadiosDetermineRadio` 在 `0x1937c553c..0x1937c55cc` 构造 `0x44_00000016`，即前缀 D、编号 22、通配后缀；保存到栈 `+0x418` |
| D22 到 vendor | `0x1937c6424..0x1937c6454` 匹配后跳 `0x1937c7240`，经 `0x1937c7254/0x1937c735c` 写入 vendor 1；`TelephonyRadiosGetRadioVendor` 从 `0x1e1fd0d38` 读取 |
| CommCenter 工厂 | `CommandDriversFactory::create` `0x1f40d8270`：vendor 1 选择 `_ZN22MCommandDriversFactory6createEv`；vendor 2/3 才选择 K 工厂 |
| BasebandManager 工厂 | `CommandDriverFactory::create_default_global` `0x1edf81dcc`：vendor 1 选择 QMI 工厂，vendor 2/3 选择 ARI 工厂 |
| 移动数据实例 | M 工厂在 `0x1f1b1e1d8` 直接调用 `QMIDataFactoryCommandDriver::create` `0x1f1abc138` |

因此当前目标不需要因为 ARI 库同时存在就引入 ARI→QMI 翻译；需要的是 **Apple QMI/QMUX 语义到 SM8650 QRTR/QMI 服务的适配**。QMUX 客户端分配、控制服务、事务宽度、服务实例、订阅绑定、版本与厂商 TLV 均须显式处理。把 `qmux1` 的字节直接当成 QRTR 请求不能实现这条链。

### 物理双槽、核心双槽与 D22 的单订阅前端

| 层级 | 槽数与身份 | 必须保持的区别 |
| --- | --- | --- |
| giulia 板级接入目标 | 两个物理 SIM 槽 | 两张卡的 UIM 状态、热插拔、选卡与射频订阅绑定分别映射 |
| 统一运行时 | `GMR_SLOTS=2`，native slot 0/1 | 每槽独立 generation、UIM/NAS/WDS、QMUX 客户端及数据所有权；两个数组不是双卡全链完成的证明 |
| 当前原版 D22 CommCenter | Apple slot ID 1，`expectedSimCount=1` | 原版不会因为底层有两个物理槽就建立第二个 iOS 订阅对象；第二个 native 槽的诊断支持不能等同 iOS 双订阅 UI/DSDS |

单订阅结论来自同一 20H392 的完整调用链：

1. `GsmRadioPersonality::getHardwareConfiguration` 在 `0x1f036be10` 调用 `TelephonyRadiosGetProduct`；D22 的 device enum `0x40` 经表 `0x1f03c54e4` 的索引 31、跳转字节 184，到 `0x1f036c11c` 返回 hardwareConfiguration **7**。名称表 `0x20dacc550[7]` 指向 `D22`。
2. 构造器把该值写入对象 `+0x68`。`0x1f036cf70..0x1f036cfbc` 读取它，计算 capability 并写入 `+0x241`。计算包含 `uint8(config-12)<45` 和固定 capability 位图，D22 config 7 的结果为 false。
3. `subscriber::makeSimSlotRange(registry,3)` `0x1f0320bfc` 读取 `+0x241`。false 分支选择 predicate `0x1f0320bf0`，其实际指令为 `cmp w0,#1; cset w0,eq; ret`；候选数组 `0x1f03c9df4` 为 `[1,2]`，只有 1 通过。
4. `subscriber::expectedSimCount` `0x1f03962d0` 调用上述范围并计数，结果为 1。同版本导出的 `GsmRadioPersonality::applyOverrides(CFDictionary*)` `0x1f0370838` 仅有 `ret`，不能假定一个 plist override 就可启用第二槽。

JSON 的 `subscription_contract` 记录这四段决策与数据来源，核验器从真实跳表和代码字节重新核对计数。当前实现不改产品身份、不改 SIM 能力旗标。生产适配器必须显式绑定所选择的 native 订阅与现有 Apple slot 1，不能让两个 native 槽同时冒用该身份；将来若要原生暴露两个 iOS 订阅，需要另行证实兼容的产品能力路径、CommCenter/服务绑定及 UI 行为。

## 2. 启动、provider 与调用形状

原 launchd 缓存中 cellular/noncellular 两个作业的 Label 都是 `com.apple.CommCenter`。前者 `_LimitLoadToDeviceTree = baseband`，后者 `_LimitLoadFromDeviceTree = baseband`。原版两个 helper 各有独立 Label。启动策略和 provider 需要同一份构建证据：

| 策略 | launchd 变化 | 配套条件 |
| --- | --- | --- |
| `disabled` | 删除两个 CommCenter 作业和两个 helper | 保持现有诊断默认值 |
| `managed` | 保留 cellular + 两个 helper；删除 noncellular 的重复 Label | ABI、已链接 native provider、共享资源仲裁、实际 baseband ADT 与 kernel hash 一致 |

`prepare_cellular()` 仅修改蜂窝启动策略。`prepare_startup()` 的音频插件、MediaToolbox 和 SpringBoard 兼容处理独立保留。`rebuild-ios16-userdata.py --cellular-policy managed` 在修改镜像前校验 runtime contract 和 ADT。

`require_managed_contract(runtime, adt, root_report)` 读取真正 ADT bytes，核对唯一 `baseband` 节点和 personality 匹配值，并比较与指定 kernel hash 绑定的逐方法契约；`verified: true` 或库名列表无法替代这些内容。未实现的语义仍明确拒绝 managed 启用。

原 kernel 的 `__PRELINK_INFO` 拓扑：

| 原服务 | 原 provider / 匹配 | 原用户客户端或功能 |
| --- | --- | --- |
| `AppleBasebandD20` | `IOPlatformDevice`，`baseband,n41` | `AppleBasebandUserClient` |
| `AppleBasebandPCIMAVControl` | `AppleBaseband` | `AppleBasebandPCIControlUserClient` |
| `AppleBasebandPCI` | `IOPCIDevice`，`baseband-pcie` | 原 Apple PCI transport |
| `AppleBasebandPCIInterface` | 用户态按类匹配，并按 `ABPInterfaceProtocol` 细分 | `AppleBasebandPCIUserClient` |
| `AppleBasebandPCIPDPQMAPSkywalk` | `AppleBasebandPCIMHIInterface`，`qmap` | QMAP IP 数据面 |
| `AppleBasebandPCIQMAPControl` | `AppleBasebandPCIMHIInterface`，`qmap_control` | QMAP 控制面 |
| `AppleBasebandPCIMAVPDPManager` | `AppleBaseband` | 原 PDP / ifnet 管理 |

这些是原 Apple 拓扑，不是 SM8650 的硬件资源。giulia 应从自己的 IOPlatformDevice 发布语义适配服务，下层仍使用真实 SMEM/GLINK/IPA；不得复制 Apple GPIO、PCI BAR、DART 或固件下载路径。用户态通过类/属性发现的要求，与原内核驱动的物理父类依赖必须分开核验。

从原 kernel **实际 dispatch table** 恢复三张表，而不是用用户态函数名推算：

| 用户客户端 | 项数 | 原虚拟地址 | 原 kernel 文件偏移 |
| --- | ---: | --- | --- |
| `AppleBasebandUserClient`（D20） | 44 | `0xfffffff006fe0480` | `0x2b94480` |
| `AppleBasebandPCIUserClient` | 13 | 见机器映射 | `0x2a86040` |
| `AppleBasebandPCIControlUserClient` | 4 | 见机器映射 | `0x2a84518` |

D20 的 `externalMethod` 在 `0xfffffff006bb8dd4` 检查 selector ≤ `0x2b`，按 **24 字节**步长选择 `{function, scalarIn, structIn, scalarOut, structOut}`。`0xffffffff` 是原 dispatch 的可变结构长度标记。44 个 selector 的全部计数、结构长度、函数地址、表 SHA256 均保存在 JSON；C header 逐项生成相同形状。

关键控制调用：PowerOn `1`、ForceReset `0`、Powercycle `6`、GetReset `3`、ManagerSet/Get `22/23`、StateGet/Set `26/27`、PropertyGet/Set `28/29`。ThermalData `30` 的内核输入结构固定 **120 字节**；DebugCommand `31` 固定 **48 字节**。下面的 producer 追踪进一步证明，当前 D22 的 State/Property 操作明确不适用，不能自行赋予统一运行时枚举。

ABP 的 open/close/write/read/sendImage/readRegister 为 `0..5`；abort/start 为 `6/7`；timeSync `8`；map/unmap `9/10`；register/deregisterTimeEvent `11/12`。同步写使用 scalar `[0,0]` 和结构缓冲区；异步写使用 scalar `[user_address,length]` 和 3 项 async reference。映射请求为 `[region,user_address,length]`。sendImage 同步、异步接口均提供 **2 项 scalar output**。必须把提交返回值和异步完成分开。

`giulia_modem_apple_abi_validate()` 只检查调用形状。通过后仍需权限检查、用户地址验证、资源所有权、请求提交、真实完成及代次隔离；该函数不会返回任何虚构的 SIM、注册或数据状态。

### D22 的状态与属性：已闭合的适用性

`AppleBasebandD20` 实例构造函数 `0xfffffff006bb76ec..76f8` 写入实际 vtable `0xfffffff006fded38`，metaclass 名字来自 `0xfffffff005558c96`。按这张表继续追踪，而不是套用其他基带机型的实现：

| 操作 | 生产端实际行为 | 用户态消费与适配要求 |
| --- | --- | --- |
| ManagerSet `22` | `0xfffffff006bbe034` 仅接受 0/1；其他值返回 `0xe00002c2`；保存到对象 `+0x2bc` | 0=`Stopped`，1=`Started`，日志字符串和选择指令位于 `0x1ea619b05/0x1ea619b0d`、`0x1ea617118` |
| ManagerGet `23` | `0xfffffff006bbe070` 读回该 32 位值；空输出指针拒绝；初始化为 0 | 这是 ABM 管理进程生命周期，不能解释为 SIM 就绪、网络注册或数据连接 |
| ManagerSet 的副作用 | 调用 SetChargerDesense，参数为 `state != Started` | 需要接入共同的功耗/共存策略；停止 ABM 不能误释放其他设备的资源 |
| State/Property `26..29` | 经过 workloop 封装，再到 vtable `+0x730/738/740/748` 对应的 `0xfffffff006bbe44c/458/464/470`；四个函数均返回 **`0xe00002c7`** | 忠实保留本产品 unsupported 行为，不能为了满足接口形状虚构 getter/setter 成功 |
| DiagReady `16` | `0xfffffff006bbe028` 返回 `0xe00002c7` | 不得将 native modem ready 冒充此不适用的 Apple GPIO 接口 |
| FSEraseDone `17` | `0xfffffff006bbd5a4` 将非空输出置 0，然后返回 `0xe00002c7` | 输出 0 仍伴随失败，不能只读结果字段忽略 IOReturn |

安装 action 的代码 `0xfffffff006bbae9c..aebc` 把上述 vtable 槽复制到对象 `+0x230/238/240/248`，后续由 workloop `runAction` 调用。用户态的 `supportsBasebandStateController` 在 `0x1ae1d8900` 实施 `unsigned(radio - 9) < 5`，D22 的 radio 5 为 false；`BootController::setBasebandState` 在 `0x1b7f60afc` 先检查这一能力。因此这里已经有生产端与消费端的一致证据，不再归类为“未恢复的状态/属性枚举”。

### ABP 提交、完成和释放

原 kernel 的 write/read handler `0xfffffff0062d6308/642c` 根据 `IOExternalMethodArguments +8` 的 asyncWakePort 区分同步与异步。异步路径 retain 用户客户端并复制 **64 字节** async reference；提交立即失败就释放复制体并 release 客户端；提交成功仅将清理责任交给完成回调。

| 路径 | 完成函数 | 通知载荷与顺序 |
| --- | --- | --- |
| read/write | `0xfffffff0062d7028` | `sendAsyncResult64(reference, status, args, 1)` → `releaseAsyncReference64` → 释放 reference 存储 → release 客户端 |
| abort/start | `0xfffffff0062d94fc` | status + **0 项**结果；同样完成后释放。selector 6/7 没有 asyncWakePort 时明确返回 unsupported |
| sendImage async | `0xfffffff0062d86e4` | status + **2 项**结果；同样完成后释放 |
| register/deregisterTimeEvent | `0xfffffff0062d6b54/6b80` | 必须存在 asyncWakePort，把 asyncReference 交给事件注册生命周期；不能按一次 I/O 回调的规则立即回收持久订阅 |

这些调用通过内核导入指针 `0xfffffff006ec6908/6918` 指向原 `IOUserClient::sendAsyncResult64` `0xfffffff0077b19a0` 和 `releaseAsyncReference64` `0xfffffff0077b15f0`，不是按函数形状猜出的通知。

sendImage 同步路径尤其要区分两层结果：`0xfffffff0062d665c..6668` 将底层 IOReturn 符号扩展后写入 `scalar_output[1]`，而外层 IOReturn 返回 0。适配器必须同时检查两层结果。这个规则也不授权向 SM8650 写入 Apple 镜像。

## 3. 通道、通知、内存与 IP 配置

ABP 通道表在 `libPCITransport` 的 `0x20d422880`，由 `pciTransportInterfaceToABP` 复制 18 项 `pair<int,const char*>`：

| 枚举 | 协议名 | 枚举 | 协议名 | 枚举 | 协议名 |
| ---: | --- | ---: | --- | ---: | --- |
| 0 | pbl | 1 | sahara | 2 | diag |
| 3 | refs | 4 | qmux1 | 5 | qmux2 |
| 6 | rom | 7 | boot | 8 | ari |
| 9 | trace | 10 | at | 11 | dpl |
| 12 | mediactl | 13 | vcom | 14 | gnss |
| 15 | qdss | 17 | virtual_timesync | 20 | boot_logger |

表中有 `ari` 不代表 D22 选择 ARI 工厂。映射中要保留“能力存在”和“当前选择”两个事实。原 Apple pbl/sahara/sendImage 操作也不能授权将 Apple 固件写进 SM8650 的 PAS 内存或持久化分区。

| 来源 | 消息 / 属性 | 真实接收布局及动作 |
| --- | --- | --- |
| AppleBaseband | `IOGeneralInterest`，`0xe3ff8007` | 先清 wake queue，再转发已注册回调 |
| AppleBaseband | `0xe3ff800d` | 先清 coredump-ready queue，再转发回调 |
| ABPControl | `0xe3ff8100` | argument `+0:u32 error`、`+8:u64 arg1`、`+16:u64 arg2` |
| ABPInterface | `0xe3ff8101` | argument `+0:u32 error`、`+8:u32 index`、`+16:i32 result`，不是紧密排列的三个 u32 |
| ABPControl | `bootstage: CFNumber` | `uint32(value) < 2` 表示 down；读取失败与 down 分开 |
| ABPControl | `logSnapshotBufferSize: CFNumber` | 日志缓冲区容量属性 |
| ACIPC（非当前分支） | `0xe3ff8101` | 三个 u64，不能复用 ABP 的字段宽度 |
| ACIPCBB（非当前分支） | `0xe3ff8200` | 调用无参数 ring-event block |

ACIPCBB 另外恢复了 openRing 输入 24 字节/输出 32 字节、index arrays 输入 104 字节，且其 vtable `+0xe0` 的 apiAdapter 指向 identity 实现。保留这些旁路证据便于比较，不把它们作为当前 D22 managed 的强制依赖。

统一运行时必须持有所有异步请求、映射、客户端与订阅的 generation。SSR 时先停止新请求并撤销路由，再失效事务/回调、停止 DMA、解除映射和关闭通道；旧异步完成不得命中新服务实例，旧地址不得再次映射给不同订阅。客户端退出与取消也走相同的资源账本。

IP 侧已从 CommCenter 的 **Mach-O indirect symbol table + BL 指令 + LC_FUNCTION_STARTS** 恢复 13 个真实调用点：

- IPv6 创建路径 `0x1001d2be4`、IPv4 创建路径 `0x1001d34c4` 调用 `IPConfigurationServiceCreate`；两处调用前均从 `[x20+0x18]` 取出传入 `x0/x1` 的对象。对应成功日志地址为 `0x1013b027d`（IPv6）、`0x1013b0313`（IPv4）。
- `GetNotificationKey` 在 `0x1001d3098/0x1001d3970`；`CopyInformation` 在 `0x1001c57bc/0x1001c64f8`；`RefreshConfiguration` 在 `0x1001c8af0`。
- 接口命名包含 `pdp_ip%d`、`pdp_ip0..15`，并真实调用 `if_nametoindex`。当前 PDP bearer 成功不等于接口已建立，更不等于地址、DNS、路由已经可用。
- 本版本使用 APNName、MTU、IPv4Entity、IPv6Entity、IPv6LinkLocalAddress、EnableDAD、PerformNUD、EnableCLAT46 选项。接口/订阅绑定、地址生命周期、通知与配置刷新必须随同一数据连接状态机推进。
- 原 Mav17 PDP personality 记录了 `rx_slot_count=16`、`tx_slot_count=16`、`max_packet_size=2048`，`pdp_ip0` 覆盖为 RX 1024/TX 512。它们是 Apple 后端证据，不是 giulia IPA ring 的硬件参数。

### PDP 错误边与 IP 配置完成边

原 kernel 的 `AppleBasebandPCIPDPManagerBase` 在 `0xfffffff0062e6f28` 把错误索引 0..12 映射到表 `0xfffffff006ed6bf0`，随后调用原 `IOService::messageClients`，`argumentSize=0`。dyld `PDPListener::generalInterestFunction` `0x1e9f7a318` 使用表 `0x20d423320` 过滤并转发相同的 13 个 ID：

| ID 范围 | 顺序对应名称 |
| --- | --- |
| `0xe3ff8051..8054` | WriteTimeout、BadMuxHeader、BadControlCommand、BadControlResponse |
| `0xe3ff8055..8058` | WriteFailed、ReadFailed、ControlCommandFailed、ControlResponseFailed |
| `0xe3ff8059..805c` | InboundPacketDropped、OutboundPacketDropped、ControlCommandDropped、ControlResponseDropped |
| `0xe3ff805d` | BadBearerMapping |

这里的 argument 原样传递，没有固定 packet struct；不能凭名字解引用成自造结构。**这些消息全部属于错误或丢弃通知，不是链路建立或 IP 就绪通知。** 集成核验同时比较 kernel 生产端和 dyld 消费端的实际表字节。

IP 配置的真正完成路径已经追到以下闭环：

```text
WDS 承载参数 → native 接口与 GMR IP_CONFIG 动作
  → IPConfigurationServiceCreate
  → SCDynamicStoreCreate(callback, owner context)
  → GetNotificationKey → 单元素 CFArray → SetNotificationKeys
  → SetDispatchQueue(既有串行执行队列)
  → IPv4/IPv6 动态存储回调 → 串行处理器
  → CopyInformation → 验证字典、地址/路由 → 发布配置
  → 对应 IPv4ServiceUp / IPv6ServiceUp
```

| 系统对象 | IPv6 | IPv4 |
| --- | --- | --- |
| service 成员 / store 成员偏移 | `+0x270 / +0x280` | `+0x278 / +0x288` |
| store 创建 / callback 地址 | `0x1001d306c / 0x1001ca6d8` | `0x1001d3944 / 0x1001ca604` |
| SetNotificationKeys / SetDispatchQueue | `0x1001d30c8 / 0x1001d30d8` | `0x1001d39a0 / 0x1001d39b0` |
| 信息处理器 | `0x1001c643c` | `0x1001c5704` |
| ServiceUp 分支日志位置 | `0x1001c7140` | `0x1001c6204` |

旧 store 先取消 dispatch queue；旧 service 先从对象成员清空并清理/释放，然后才装入新 service。IPv4 处理器核对 `kSCEntNetIPv4`、Addresses 和 Router；IPv6 处理器核对 IPv6 字典、Addresses/PrefixLength/Router，区分临时地址、解析错误及 CLAT46。空或类型不符的信息会被忽略，源代码日志明确考虑 `configd` 重启，等待后续信息，而非制造新地址。

对应的 GMR `IP_CONFIG` 完成必须绑定原 operation、epoch、slot、bearer 与 IP family，经过有效信息和发布结果才能推进；一次创建返回、WDS 成功或 PDP IOKit 错误都不能代替它。

数据面的原类也已有构造链证据：QMAPSkywalk → QMAPProvider → AppleBasebandPCISkywalkProvider → IOService；MAVPDP personalities 没有声明独立的 `IOUserClientClass`。这里不能发明一个“PDP UserClient selector 表”。实际待实现的是 native 网络接口、packet queue、DMA 与完成的所有权适配，而非给 codec 加一个成功返回值。

原 QMAPControl 消费器 `0xfffffff006c40720..407dc` 约束 mux 为 1..8，长度为 header `+2` 的 BE16，padding 为 byte0 低 6 位，buffer 长度必须等于 payload 长度 +4，command 为 bit7，去 padding 后 command 至少 8 字节；另有四个零字节的特殊接受分支。这是当前 Apple 前端的限制，不是 SM8650 的端点数量。其 command/response 路径必须与 `gmd_receive/gmd_transmit` 的 IP payload 路径分开。

## 4. 实际 QMI 启动请求与版本协商边界

继续从同一 dyld cache 追踪后，客户端控制器定位到 `libATCommandStudioDynamic.dylib` 的
`QMIControl`。`qmi_semantic_contract` 现在保存 122 段函数范围和 66 段分支跳板的实际字节
SHA256、9 个消息构造调用点，以及所选 M/QMI 工厂的 50 个直接创建入口。
核验器同时验证构造消息编号的指令和调用指令；工厂入口存在不表示其中每个业务都会在开机执行。

| 实际构造位置 | 消息 | 控制机制 |
| --- | --- | --- |
| `0x19ed08b94` / `0x19ed08fc8` | CTL `0x22` / `0x23` | 申请/释放客户端，超时参数 `25000 ms` |
| `0x19ed07af4` | CTL `0x26` | `setDataFormat_sync`，不能由 CID 分配成功替代 |
| `0x19ed09b38` | CTL `0x27` | Sync，实际强制发送超时参数 `2000 ms`，失败处理器有重发分支 |
| `0x19ed0a478` / `0x19ed0a4f0` | CTL `0x29` / `0x32` | `configurePowerMode_sync` 的普通/扩展配置路径 |
| `0x19ed07fdc` | CTL `0x2a` | `setPowerMode_sync`；启动路径传入 profile `0` |
| `0x1ae241698` | DMS `0x23` | `radio::QMICommandDriverBase::getBasebandFWVersion` 的请求构造 |
| `0x1ae241f60` | BSP `0xe018` | `getFactoryCalibrationStatus` 的厂商请求构造，不能当作通用 DMS 响应处理 |

D22 的适用性也有条件分支证据：`supportsDataService` 在 `0x1ae1d898c..8994` 计算
`unsigned(radio-9)<5`，`isMav20Plus` 在 `0x1ae1d8b6c..8b74` 计算
`unsigned(radio-9)<4`。D22 radio `5` 使两者均为 false。因此 `QMIControl::start_sync`
在 `0x19ed07a20..7a3c` 会执行数据格式请求 `0x26`，然后执行初始电源模式请求 `0x2a`；
Baseband 工厂在 `0x1ae237ef4..8028` 选择普通 `radio::QMICommandDriver` 分支。

在所有有符号边界的 `QMIControl` 成员函数中，直接消息构造得到上述七个 CTL 编号，
没有观察到 `GetVersionInfo 0x21`。这是有界的调用点结论，不能扩大为整个系统绝不查询版本，
也不能据此返回一张猜测的 native 服务版本表。原 `qmux` 帧转换层只实现 `0x22/0x23`；
`0x26/0x27/0x29/0x2a/0x32` 的实际控制语义，以及 BSP `0xe018` 等厂商请求与 SM8650
返回内容的兼容性，仍是具体且可复现的接入缺口。因此最后一项 unresolved 保留并细化了
这些地址与路径；没有以函数名字或一个成功 result TLV 宣布语义等价。

## 5. 重现、验收和明确的实现边界

以下是**证据提取操作**，不是拆开的验收入口：

```sh
python scripts/ios16_modem_apple.py extract
python scripts/ios16_modem_apple.py extract-kernel
python scripts/ios16_modem_apple.py extract-qmi
python scripts/ios16_modem_apple.py header
```

`extract` 读取原 HFS 文件和 cache mapping，输出 402 个真实函数范围、指令与 SHA256，及网络调用点；避免生成数千万符号的全局 A2S 表。`extract-kernel` 从指定 IPSW 的 BuildManifest 核对 20H392，解压固定成员，解析 prelink XML 的 ID/IDREF，按所属 kext 代码范围唯一定位三张 dispatch 表。原文件不修改。

唯一集成验收入口仍是 `scripts/test-ios16-modem.sh`。其可调用 `verify_sources()`、`verify_kernel_sources()`、`render_abi_header()` 和 `require_managed_contract()`，将来源、C 调用边界、provider、ADT、root 策略和统一运行场景关联到一份报告。单独提取成功或 selector 形状匹配不能作为蜂窝业务已通过的结论。

当前已恢复实际产品协议分支、全 61 个原内核 dispatch 项、54 个用户态 wrapper 契约、通道表、通知布局、provider 拓扑和 IP 配置调用链，并闭合了 D22 状态/属性适用性、ManagerState、三类 ABP 异步完成契约及物理双槽→核心双槽→D22 单订阅前端的能力边界。JSON 的语义记录为每段生产端和消费端代码保存源字节范围与 hash。

剩余未闭合语义项聚焦所选 Apple QMI 服务版本和厂商扩展与原生服务的兼容证据。QMUX/CID/事务/代次和 QMAP/mux/MTU codec、PDP 的实际接口及完成时序分别已有明确映射；native PDP/Skywalk/socket、真实电源/复位及异步资源回收列为**生产适配器实现要求**，不再混写成“ABI 不明”。managed gate 的 native-provider 条件保留。这里的限制来自代码契约，没有以设备未连接替代分析。
