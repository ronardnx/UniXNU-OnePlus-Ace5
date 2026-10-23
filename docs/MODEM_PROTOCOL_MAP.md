# MPSS 蜂窝协议与统一生命周期映射

本文展开 SIM、注册、双卡、IPv4/IPv6 数据及反向存储的线协议。其余服务的全部已提取字段见
[机器可读逐字段地图](../hardware/modem-qmi-service-map.json)，跨服务调用者、所有权、超时和恢复见
[统一机制清单](../hardware/sm8650-modem-mechanisms.json)。固定源的 545 条 libqmi 方向消息、
4035 个嵌套字段、RMTFS/TFTP 和 IPA descriptor 均可从原始字节重建核验。
QRTR node/port、协商版本和会话句柄是运行时输出，消费方必须按当前 epoch 接收，不能从服务 ID 猜值。

固定参考版本：libqmi [`d9e8af5cae7fdf9749baf87606568d3af4d03082`](https://github.com/linux-mobile-broadband/libqmi/tree/d9e8af5cae7fdf9749baf87606568d3af4d03082)，linux-msm/rmtfs [`b30a3eb38f9af283f18dbd3c7755653efc52c094`](https://github.com/linux-msm/rmtfs/tree/b30a3eb38f9af283f18dbd3c7755653efc52c094)。以下字段来自这些版本，未从另一台手机猜测 giulia 硬件参数。

## 分层及寻址

```text
SIM 诊断 / 蜂窝状态协调 / 网络接口配置
        UIM / NAS / DMS / WDS / WDA 等 QMI 服务
        QMI service header + TLV（控制面）
        QRTR service discovery + node:port
        MPSS GLINK / SMEM 传输与中断

IPv4 数据包 ↔ XNU 网络接口 ↔ IPA/GSI 数据通路 ↔ MPSS
                              ↑
           endpoint / mux / aggregation 必须与控制面约定一致
```

QRTR 的 `service` 标识协议，`node:port` 标识这一次运行中的服务器地址。服务版本、instance 与 node/port 不互为别名；服务重新公布或 MPSS 重启后必须重新发现并撤销旧事务/数据状态。蜂窝 MPSS 和 WLANFW 是不同服务、不同固件域，不能把 Wi-Fi 的 MHI/IPCR 状态当成 MPSS 已启动的证据。

[libqmi 服务枚举][enums] 中的常用 ID：

| 服务 | ID | 用途/方向 |
| --- | --- | --- |
| WDS | `0x01` | AP 查询/建立/终止数据会话 |
| DMS | `0x02` | 设备能力与 operating mode |
| NAS | `0x03` | 注册、驻网、packet attach 状态 |
| VOICE | `0x09` | 网络呼叫、入呼、补充业务；网络状态与 ADSP 媒体状态分开 |
| UIM | `0x0b` | SIM 卡、应用、PIN、逻辑 slot/session |
| RMTFS | `0x0e` | AP 向 MPSS 提供远程存储服务，方向与 WDS 客户端相反 |
| IMS settings / IMSA | `0x12` / `0x21` | IMS 配置/应用状态；不能由数据拨号成功推导 |
| WDA | `0x1a` | 数据端口格式、聚合协议 |
| RFSA | `0x1c` | 另一套远端文件访问协议，不是 RMTFS 的别名 |
| DSD | `0x2a` | 数据系统状态；可补充 NAS 信息 |
| DPM | `0x2f` | 数据端口管理；不是 UIM 的 message `0x002f` |

服务 ID 不是 message ID，也不是固定 QRTR 端口。内置 MPSS 的 QRTR 消息无需 USB QMUX client-allocation 流程；具体客户端端口/生命周期由 QRTR 传输实现管理。

另有平台内核 IPA peer 服务 `0x31`，来源是 OnePlus dataipa，而非上述 libqmi 枚举。其两个 instance 和方向见下节；它不能被 WDS/WDA 客户端替代。

## QMI 公共帧

[service_header 定义][message] 的 wire 顺序如下，公共头与下文明确列出的 `u16/u32/u64` 字段均 little-endian；其他消息字段可能有不同的字节序，例如 WDS IPv6 地址的数组元素明确使用 network byte order。

| 偏移 | 长度 | 字段 |
| --- | --- | --- |
| 0 | 1 | flags：request=`0`，response=`2`，indication=`4` |
| 1 | 2 | transaction ID |
| 3 | 2 | message ID |
| 5 | 2 | 后续全部 TLV 的字节数 |
| 7 | 可变 | TLV 序列 |

每个 TLV 是 `type:u8, length:u16, value[length]`；length 不包含三字节 TLV 头。不是每个字段前都有 validity byte；C 结构中的 optional-valid 标志不应写到 wire。QRTR 数据报 payload 不包含 libqmi 内部用于 bookkeeping 的伪 QRTR/QMUX header。完整校验应要求 QMI 长度正好等于 payload 长度，不能把 QRTR 外层 padding 送进 QMI 解码器。[flags 定义][private]

响应必须包含唯一的 result TLV `0x02,length=4,{result:u16,error:u16}`。只有 `{0,0}` 可进入成功路径；失败不能使用响应中的 handle/address/card status 推进状态。事务必须同时匹配收到的服务器 endpoint、pending transaction 和 message。重复 TLV、截断字段、数组 count 溢出、错误 flags 和尾部剩余字节应被拒绝。未知且 framing 正确的单个 optional TLV 可以跳过。[result 定义][common]

## UIM：卡、应用与 PIN

`Get Card Status`：service=`0x0b`，message=`0x002f`，请求无 TLV。响应除 result 外，`0x10` 是 Card Status。`Card Status` indication 使用 message=`0x0032`；不是一次请求的 response。[UIM schema][uim]

`0x10` value 的完整结构：

| 顺序 | 类型 | 字段 |
| --- | --- | --- |
| 1–4 | 各 `u16` | GW primary、1x primary、GW secondary、1x secondary provisioning index |
| 5 | `u8` | card count |
| 每张 card 的 1–6 | 各 `u8` | card state、UPIN state、UPIN retries、UPUK retries、card error、application count |
| 每个 app 的 1–6 | 各 `u8` | app type、app state、personalization state、personalization feature、personalization retries、personalization unblock retries |
| app 的 7 | `u8` | AID length |
| app 的 8 | `u8[AID length]` | AID bytes，可检查长度后跳过；状态输出不需要复制 |
| app 的 9–15 | 各 `u8` | UPIN replaces PIN1、PIN1 state、PIN1 retries、PUK1 retries、PIN2 state、PIN2 retries、PUK2 retries |

一个 provisioning index 的高八位是 card index，低八位是 app index；`0xffff` 表示未配置，不能当作第零张卡。双卡情况下不能凭“第一张 present”决定 WDS subscription。[qmicli 对 index 的解释][qmicli-uim]

[状态枚举][uim-enums]：

| 字段 | 关键取值 |
| --- | --- |
| card state | absent=`0`，present=`1`，error=`2` |
| app type | SIM=`1`、USIM=`2`、RUIM=`3`、CSIM=`4`、ISIM=`5` |
| app state | unknown=`0`、detected=`1`、PIN required=`2`、PUK required=`3`、personalization check=`4`、PIN blocked=`5`、illegal=`6`、ready=`7` |
| PIN state | uninitialized=`0`、enabled/unverified=`1`、enabled/verified=`2`、disabled=`3`、blocked=`4`、permanently blocked=`5` |
| personalization state | unknown=`0`、in progress=`1`、ready=`2`、code required=`3`、PUK required=`4`、permanently blocked=`5` |

最小 3GPP ready 判定：有效的目标 provisioning index → card present → app 是 SIM/USIM → app ready → effective PIN 为 verified 或 disabled；`UPIN replaces PIN1=1` 时读取 card.UPIN，不能继续只检查 app.PIN1。锁卡/初始化中/未 provisioned 必须保留各自状态，不能统一打印“无 SIM”。此阶段无需读取 ICCID、IMSI、手机号、PIN 内容；也不自动尝试解锁。

## NAS 与 DMS：驻网不等于数据链路

`NAS Get Serving System`：service=`0x03`，message=`0x0024`，请求无 TLV。[NAS schema][nas]

| 响应 TLV | 结构 | 用途 |
| --- | --- | --- |
| `0x01` | registration:u8, CS attach:u8, PS attach:u8, selected network:u8, radio count:u8, radios:i8[count] | 必须完整解析 count，不能只读前四字节 |
| `0x10` | roaming:u8 | `0` 为 roaming，`1` 为 home；缺失表示未知 |
| `0x11` | count:u8, data capabilities:u8[count] | 可选能力信息 |

registration=`1` 是 registered；`0/2/3/4` 是 not registered/searching/denied/unknown。PS attach=`1` 是 attached，`0/2` 是 unknown/detached。selected network=`2` 是 3GPP。radio interface 是单独枚举，LTE=`8`、5GNR=`12`，不能用有 radio 项代替 registered/PS attached。[NAS enums][nas-enums]

`DMS Get Operating Mode`：service=`0x02`，message=`0x002d`，请求无 TLV；响应 `0x01:u8 mode`，可选 `0x10:u16 offline reason` 和 `0x11:u8 hardware restricted`。`Set Operating Mode` message=`0x002e`，请求 `0x01:u8 mode`；online=`0`、low power=`1`、offline=`3`、reset=`4`。模式切换是实际设备状态变更，最小编解码层没有调用它。[DMS schema][dms] [DMS enums][dms-enums]

## IPA peer：AP 与 MPSS 驱动的初始化和规则同步

此节固定于 OnePlus [`android_kernel_modules_and_devicetree_oneplus_sm8650` commit `453d8875f815bf9dfad9035982861226a4823a6c`][ipa-service]。除 UAPI 结构和 message 宏之外，已核对同版本 [`ipa_qmi_service_v01.c` 的 `qmi_elem_info` 编码描述][ipa-ei]；下面 TLV 编号不是从 C 成员顺序推算的。

| 角色 | QRTR service | version | instance | 原厂代码证据 |
| --- | --- | --- | --- | --- |
| AP/A5 提供的 IPA server | `0x31` | `1` | `1` | `qmi_add_server(ipa3_svc_handle, IPA_A5_SERVICE_SVC_ID, IPA_A5_SVC_VERS, IPA_A5_SERVICE_INS_ID)` |
| MPSS/Q6 提供的 IPA server，AP 为 client | `0x31` | `1` | `2` | `qmi_add_lookup(ipa_q6_clnt, IPA_Q6_SERVICE_SVC_ID, IPA_Q6_SVC_VERS, IPA_Q6_SERVICE_INS_ID)` |

这两个 instance 的实际 QRTR node/port 仍由发现事件获得，不能硬编码为 `1/2`。AP 必须既能发请求，又能处理 modem 发往 AP server 的请求并回复。[注册与 handler 表][ipa-service]

| Message | 已核实方向 | 作用/关键门槛 |
| --- | --- | --- |
| `0x0020` Indication Register | MPSS→AP；代码也有 AP→Q6 注册函数 | `0x10` master init complete、`0x11` quota、`0x12` MHI ready、`0x13` endpoint desc、`0x14` bandwidth change，均 optional u8；不同方向只请求有意义的 indication |
| `0x0021` Init Modem Driver | AP→MPSS，response 反向 | 提供平台、IPA 内存布局、控制 endpoint、SSR 标志与统计区；成功不代表全部后续初始化已经完成 |
| `0x0022` Master Driver Init Complete indication | AP→已注册的 MPSS client | `0x02:{result:u16,error:u16}`；AP 初始化成功与 indication 注册两件事都完成后才发送 |
| `0x0035` Init Modem Driver Complete request | MPSS→AP，AP response | 请求 `0x01:u8 status`；response 标准 result。不是 `0x21` 的 response，也不是 indication |
| `0x0023` Install Filter Rule | MPSS→AP handler | 安装 modem 提交的 filter 规则；不能收到规则就报告已安装 |
| `0x0024` Filter Installed Notification request | MPSS→AP handler，同时存在 AP→MPSS sender | 带 source pipe、安装结果、handle/index 映射；仍是需要 response 的 request |
| `0x0037` Install Filter Rule Ex | AP→MPSS sender | 扩展规则结构，与 `0x23` 不是同一个可互换 payload |
| `0x0027` Config | MPSS→AP handler | 平台配置请求；具体有效字段须沿 descriptor 和实际配置继续核对 |

`0x21` 请求的 optional TLV 映射如下。[编码描述][ipa-ei] [字段语义][ipa-uapi]

| TLV | wire 类型 | 含义 |
| --- | --- | --- |
| `0x10` | i32 enum | platform type；原厂 MSM_ANDROID 分支为 `3`，不是 iOS 已兼容的证明 |
| `0x11` | u32 start, u32 end | modem header table 范围 |
| `0x12`, `0x13` | 各 `{u32 start,u32 num_indices}` | IPv4/IPv6 非 hash routing table |
| `0x14`, `0x15` | 各 u32 | IPv4/IPv6 非 hash filter 起始位置 |
| `0x16` | u32 start, u32 size | modem memory block |
| `0x17` | u32 | AP 接收 modem 控制命令的 destination endpoint |
| `0x18` | u8 | SSR boot 标志 |
| `0x19`, `0x1a` | 各 `{u32 start,u32 end}` | header processing context / compression table 范围 |
| `0x1b`, `0x1c` | 各 `{u32 start,u32 num_indices}` | IPv4/IPv6 hash routing table |
| `0x1d`, `0x1e` | 各 u32 | IPv4/IPv6 hash filter 起始位置 |
| `0x1f`, `0x20` | 各 u32 | quota stats base / size |
| `0x21`, `0x22` | 各 u32 | drop stats base / size |
| `0x23` | u32 start, u32 size, u8 first, u8 last | hardware filter stats 区及索引范围 |
| `0x24`, `0x25` | 各 `{u32 start,u32 size}` | SMEM info / peripheral stats SMEM info |

这些数字不是一律“物理地址”。原厂发送函数对多数 table 字段使用 `IPA_MEM_PART(...) + smem_restr_bytes`；peripheral stats 的 start 来自 `per_stats_smem_pa`。必须逐字段保留 offset、物理地址与尺寸的不同语义。当前文档未固定 giulia 启动时的实际分配值；不可用 header 的历史 endpoint 上限注释替代新硬件端点表。[`ipa3_qmi_init_modem_send_sync_msg`][ipa-service]

`0x21` response：`0x02` result；optional `0x10:u32 ctrl destination endpoint`、`0x11:u32 default endpoint`、`0x12:u8 modem_driver_init_pending`。UAPI 明确说明 pending 表示还要等 `0x35`，包括 microcontroller image loading 完成，才能与 IPA HWP 交互。所查发送函数仅返回通用 result 检查，而 `0x35` 回调直接标记已收到完成请求；移植时仍须确认 `status` 的有效值、uC ready 和 pending/SSR 状态，不能把这个回调简化成无条件“硬件可用”。[UAPI 与原厂流程][ipa-uapi] [handler][ipa-service]

Filter 的最低字段映射：`0x23` 请求 `0x10` 是带 u8 count 的 filter-spec 数组，`0x11:u32 source pipe`、`0x12/0x13:u32 v4/v6 filter counts`、`0x14` xlat index 数组、`0x15/0x16` extended filter-spec 数组、`0x17` firewall index 数组。嵌套的 mask/range/equation 和扩展结构必须逐项按 descriptor 实现，本文没有将其缩减成一个通用规则字节块。[filter descriptor][ipa-ei]

`0x24` 请求的必需项是 `0x01:u32 source pipe`、`0x02:u16 install status`、`0x03:u8 count + handle/index struct array`；optional `0x10:u32 embedded pipe`、`0x11:u8 retain header`、`0x12:u32 embedded call mux ID`、`0x13/0x14:u32 v4/v6 counts`、`0x15/0x16:u32 first v4/v6 index`、`0x17/0x18/0x19` rule IDs / destination pipe IDs / extended rule IDs 数组。这里 request 的 `0x02` 是二字节 status，不是 response 的四字节 result；必须同时按 service、方向、message 和 flags 选择 schema。[notification descriptor][ipa-ei]

`rmnet_ipa.c` 的 `ipa3_q6_handshake_complete` 还处理 SSR 时的 proxy clock vote、IPACM 通知、统计恢复及 modem-up 状态；这些不是 WDS Start 成功后可省略的操作。giulia 的具体 IPA 31 个静态端点候选映射见 `hardware/sm8650-modem.json`，运行态端点所有权、filter 内容、用户态 IPACM 等职责仍待实现/确认。[原厂 rmnet 流程][ipa-rmnet]

## WDA / WDS：先确定数据端口契约

`WDA Set Data Format`：service=`0x1a`，message=`0x0020`。[WDA schema][wda]

| 请求 TLV | 类型 | 字段 |
| --- | --- | --- |
| `0x10` | u8 | QoS format |
| `0x11` | u32 | link layer protocol |
| `0x12`, `0x13` | 各 u32 | uplink/downlink aggregation protocol |
| `0x14` | u32 | NDP signature（只在对应协议有意义） |
| `0x15`, `0x16` | 各 u32 | downlink max datagrams / max size |
| `0x17` | u32 + u32 | endpoint type、interface number |
| `0x1b`, `0x1c` | 各 u32 | uplink max datagrams / max size |

响应 `0x10–0x16` 对应同名格式字段；但响应 `0x17/0x18` 是 uplink max datagrams/max size，不是 endpoint；响应 `0x1a` 是 minimum padding:u32，`0x1b` 是 flow control:u8。相同 type 在请求和响应中的含义可以不同。`Get Data Format` message=`0x0021` 的可选请求 endpoint 是 `0x10:{u32,u32}`，不能复用 Set 的 `0x17`。[WDA schema][wda]

link layer RAW_IP=`2`；aggregation disabled=`0`，QMAP=`5`，QMAPv2/v3/v4/v5=`6/7/8/9`。必须按服务实际支持并返回的格式实现帧头、padding、checksum、聚合边界和 flow control；不能只把所有 QMAP 变体当作相同的四字节头。[WDA enums][wda-enums]

`WDS Bind Mux Data Port`：message=`0x00a2`：

| 请求 TLV | 类型 | 字段 |
| --- | --- | --- |
| `0x10` | u32 + u32 | endpoint type、interface number |
| `0x11` | u8 | mux ID |
| `0x13` | u32 | client type（tethered=`1`，undefined=`0xff`） |

endpoint type embedded=`4`、BAM_DMUX=`5`、PCIe=`3` 是协议枚举，不是 giulia 已验证的参数。interface number/mux ID 必须与 IPA 端点和 rmnet 通道匹配，不能从 WLAN PCIe 编号推导。旧式 `WDS Bind Data Port` message=`0x0089` 使用 `0x01:u16 SIO port`，与 Bind Mux 的结构不同。[WDS schema][wds] [endpoint enums][enums]

`WDS Bind Subscription` message=`0x00af` 使用 `0x01:u32 subscription`；default/primary/secondary/tertiary=`0/1/2/3`。`Set IP Family` message=`0x004d` 使用 `0x01:u8 family`，IPv4=`4`、IPv6=`6`。这些操作、Start Network、Get Current Settings 必须在一致的客户端/端口绑定上下文内执行。实际固件支持和顺序要核对，不应仅凭上游有此 schema 就强行发送。[WDS schema][wds] [common enums][enums]

## WDS：建立会话、取配置、断开

`Start Network` message=`0x0020`。[WDS schema][wds]

| 请求 TLV | 类型 | 用途 |
| --- | --- | --- |
| `0x14` | TLV 长度限定的字符串，无 NUL | APN，由调用者明确提供 |
| `0x16` | u8 | authentication preference |
| `0x17`, `0x18` | 字符串 | username/password；需要时才提供，不记录内容 |
| `0x19` | u8 | IP family preference，IPv4=`4` |
| `0x31`, `0x32` | 各 u8 | 3GPP/3GPP2 profile index |
| `0x33` | u8 | enable autoconnect |

成功响应 `0x01:u32 packet data handle` 是后续 Stop 的句柄。失败时可能有 `0x10:u16 call end reason` 及 `0x11:{u16 reason type,i16 reason}`；不可把通用 remote error 和 call end reason 混为一个枚举。底层 codec 默认构造 APN + IPv4；统一核心按 bearer 的 IP family 将 `0x19` 设为 4 或 6。认证、profile 和 autoconnect 的完整策略仍由相应执行器负责。

`Get Current Settings` message=`0x002d`，请求 `0x10:u32 requested settings mask`。DNS/IP/gateway/MTU/family 位分别是 `1<<4 / 1<<8 / 1<<9 / 1<<13 / 1<<15`，合计 `0x0000a310`。[WDS requested-settings enum][wds-enums]

| 响应 TLV | 类型 | 含义 |
| --- | --- | --- |
| `0x15`, `0x16` | 各 u32 | primary/secondary IPv4 DNS |
| `0x1e` | u32 | IPv4 address |
| `0x20` | u32 | IPv4 gateway |
| `0x21` | u32 | IPv4 netmask |
| `0x29` | u32 | MTU |
| `0x2b` | u8 | family |

这些 IPv4 数字按 LE 解码后使用“最高有效字节为第一段”的数值语义：wire `02 00 00 0a` → 数值 `0x0a000002` → `10.0.0.2`。交给网络栈时要按接口要求转为 network byte order，不能把原始四字节直接作为地址数组。缺失 DNS 不等于 `0.0.0.0` DNS；缺失 MTU 也不允许自动宣称已协商某个值。

IPv6 使用 `0x25` 地址、`0x26` 网关（均为 16 字节 network-order 地址加 u8 前缀），
`0x27/0x28` 为 16 字节 DNS，`0x29` 仍是 LE MTU，`0x2b` 若存在必须为 6。
核心逐项检查长度、前缀不超过 128、非零非多播地址、MTU 在当前缓冲能力内，并保留 optional presence。
这组真实编解码已经用于统一 IPv6/QMAP 场景，原生网络配置仍需消费同一份结果。

`Stop Network` message=`0x0021`，请求 `0x01:u32 packet data handle`；可选 `0x10:u8 disable autoconnect`。仅对当前客户端成功取得的句柄执行；本最小 codec 拒绝零和全一的句柄，避免误用特殊句柄。响应仅需成功 result。[WDS schema][wds]

完整的数据可用条件至少包括：

1. MPSS 启动、依赖服务可用、QRTR 发现服务。
2. 目标 SIM/provisioning/subscription 已确认；NAS 注册和 packet attach 状态有效。
3. IPA/GSI 资源、共享内存/DMA/IOMMU、数据端点和中断路径已建立，故障恢复有清理流程。
4. 确认 WDA 实际格式；把 WDS 客户端绑定到正确 endpoint/mux/subscription/family。
5. Start 成功并取得 handle；获得地址、路由、DNS、MTU，校验后配置 XNU 接口。
6. 收发真实 IP 流量、核对 TX/RX/丢包/校验错误，并验证断线/MPSS 重启后撤销旧路由、句柄和事务。

这里只定义步骤之间的依赖；giulia 的第 3–4 步具体资源/端口值尚未由该 codec 实现。CommCenter 兼容层同样是独立工作：原生 iOS 服务不能直接调用这组 C 编解码函数。

## RMTFS、EFS/NV 与 RFSA 的区别

RMTFS 是 MPSS 向 AP 请求持久化/共享缓冲区操作的 QMI 服务。AP 发布服务，不能以“连上 UIM/WDS”替代 RMTFS server。EFS/NV 是 modem 使用的文件系统/数据内容概念，不是一个 WDS 网络会话，也不能用空文件冒充设备校准和运营商配置。[rmtfs 主循环][rmtfs-main]

[rmtfs 的 QMI 定义][rmtfs-qmi]：

| Message | 请求 TLV | 响应/说明 |
| --- | --- | --- |
| OPEN `1` | `1:string path` | result `2`；caller ID `0x10:u32` |
| CLOSE `2` | `1:u32 caller ID` | result `2` |
| RW_IOVEC `3` | `1:u32 caller ID`, `2:u8 direction`, `3:array iovec`, `4:u8 force sync` | iovec 元素是 `{u32 sector address,u32 physical offset,u32 sector count}`；result `2` |
| ALLOC_BUFF `4` | `1:u32 caller ID`, `2:u32 size` | result `2`；`0x10:u64 buffer address` |
| GET_DEV_ERROR `5` | `1:u32 caller ID` | result `2`；`0x10:u8 status` |
| FORCE_SYNC indication `6` | `1:array u32 caller IDs` | 与请求/响应路径不同 |

RMTFS 中的物理偏移/地址只能解释为已核实、由 AP 管理的共享内存范围；不能收到一个地址就任意映射物理内存。sector 计算、共享范围、持久化顺序、重启清理和请求方身份必须各自校验。

上游 [storage.c][rmtfs-storage] 将 `/boot/modem_fs1`、`/boot/modem_fs2`、`/boot/modem_fsc`、`/boot/modem_fsg` 映射到 `modemst1/modemst2/fsc/fsg`，还显式列出 OnePlus/Oppo 的 `oem_stanvbk/oem_dycnvbk` 路径。这是应调查的原厂依赖线索，不是授权覆盖、克隆或重建本机 NV 分区。RFSA 使用另一个 service ID；需另外提取原厂调用和其协议，本文不凭 RMTFS schema 虚构 RFSA message。

## 在统一框架内的执行契约

`giulia_modem_qmi.h` 提供 UIM/NAS/WDS 编解码，`giulia_modem_ip.h` 处理 IPv6 地址与 IP 边界，
`giulia_modem_qmux.h` 为原版 Apple QMUX 分配虚拟 CID、绑定独占 QRTR 端口并重写/还原事务。
`giulia_modem_qmap.h` 和 `giulia_modem_data.h` 将 QMAP 帧关联到同一核心的 bearer、slot、mux 和 epoch。
所有业务状态由 `giulia_modem_runtime.h` 推进；codec 成功不会单独设置 SIM READY 或数据 UP。

| 跨服务机制 | 正常时序与完成条件 | 取消、异常与恢复 |
| --- | --- | --- |
| UIM/PIN/热插拔/STK | UIM 状态来自 provisioning index、应用及 PIN；PIN/PUK/STK 字段保留在完整 schema | 卡移除使旧 slot generation、会话和媒体失效；不能自动尝试 PIN |
| 双卡绑定 | native slot `[0,1]` → subscription → WDS client/endpoint/mux/family；切换必须先排空旧业务 | slot 变化使旧回复失效；Apple D22 单订阅过滤另见 ABI，不伪造双 Apple 订阅 |
| PDC/MBN | Load/Set Selected/Activate 的 request token 与 response/indication 分别关联；active/pending 配置分开 | 超时保留配置结果未知，重新查询 active/pending；APN/profile 不代替 MBN |
| IPA/WDA/WDS | IPA AP/Q6 双向初始化 → WDA 格式 → WDS 绑定 → Start/Settings → DMA → 网络配置 | 任一门槛失败不置 UP；未知句柄和未停止 DMA 保留隔离，SSR 重建双向服务 |
| DPM/DSD/QoS | 端口打开、可用系统变化和 QoS flow 有各自消息及 indication；同 bearer 记录归属 | 服务消失撤销相关路由/flow；不能把 NAS registered 当作 DSD/flow 已建立 |
| RMTFS/TFTP | AP 接收反向请求；身份/范围校验 → 写入 → flush → 原请求应答；重复请求返回缓存结果 | 短写或 flush 失败不确认持久化；停止先排空，再释放共享区；旧 endpoint 请求失效 |
| IMS/VoLTE/VoWiFi | IMS 配置与注册 response、注册 indication、IMS 数据承载、VOICE 呼叫、ADSP 媒体分别完成 | Wi-Fi/蜂窝路径切换涉及 IMS 承载与媒体；网络挂断排空媒体；不由普通数据 UP 推导 IMS |
| WMS/紧急/补充业务 | 消息提交、网络接受、送达分别建状态；入呼、广播、补充和紧急业务字段见服务地图 | 结果未知时不重发有副作用业务；紧急业务能力必须来自服务/运营商状态 |
| 飞行模式/休眠/SSR | 停业务后改变 DMS 模式；休眠前排空；SSR 失效服务/事务/存储请求/DMA/媒体 | 单调时钟超时、旧回复隔离、依赖顺序回收；其他 owner 的资源继续保留 |

表中 wire-codec 与 typed-runtime-contract 的实现层级由机器清单区分。WDA/DPM/PDC/IMS 等完整
schema 已映射，不能将 schema 存在视为物理 provider 已执行。原生远端返回值是协议消费者的输入，
不是停止静态映射的条件。具体端点来自固定 31 项生产表，协商值由当前会话产生并校验。

唯一验收入口是 `bash scripts/test-ios16-modem.sh`。已有字段级断言作为连续系统场景和仓库回归的
内部检查，不再单独作交付结论。报告同时关联原始证据、核心执行、完整组合补丁和产物一致性。

[enums]: https://github.com/linux-mobile-broadband/libqmi/blob/d9e8af5cae7fdf9749baf87606568d3af4d03082/src/libqmi-glib/qmi-enums.h
[message]: https://github.com/linux-mobile-broadband/libqmi/blob/d9e8af5cae7fdf9749baf87606568d3af4d03082/src/libqmi-glib/qmi-message.c
[private]: https://github.com/linux-mobile-broadband/libqmi/blob/d9e8af5cae7fdf9749baf87606568d3af4d03082/src/libqmi-glib/qmi-enums-private.h
[common]: https://github.com/linux-mobile-broadband/libqmi/blob/d9e8af5cae7fdf9749baf87606568d3af4d03082/data/qmi-common.json
[uim]: https://github.com/linux-mobile-broadband/libqmi/blob/d9e8af5cae7fdf9749baf87606568d3af4d03082/data/qmi-service-uim.json
[uim-enums]: https://github.com/linux-mobile-broadband/libqmi/blob/d9e8af5cae7fdf9749baf87606568d3af4d03082/src/libqmi-glib/qmi-enums-uim.h
[qmicli-uim]: https://github.com/linux-mobile-broadband/libqmi/blob/d9e8af5cae7fdf9749baf87606568d3af4d03082/src/qmicli/qmicli-uim.c
[nas]: https://github.com/linux-mobile-broadband/libqmi/blob/d9e8af5cae7fdf9749baf87606568d3af4d03082/data/qmi-service-nas.json
[nas-enums]: https://github.com/linux-mobile-broadband/libqmi/blob/d9e8af5cae7fdf9749baf87606568d3af4d03082/src/libqmi-glib/qmi-enums-nas.h
[dms]: https://github.com/linux-mobile-broadband/libqmi/blob/d9e8af5cae7fdf9749baf87606568d3af4d03082/data/qmi-service-dms.json
[dms-enums]: https://github.com/linux-mobile-broadband/libqmi/blob/d9e8af5cae7fdf9749baf87606568d3af4d03082/src/libqmi-glib/qmi-enums-dms.h
[wda]: https://github.com/linux-mobile-broadband/libqmi/blob/d9e8af5cae7fdf9749baf87606568d3af4d03082/data/qmi-service-wda.json
[wda-enums]: https://github.com/linux-mobile-broadband/libqmi/blob/d9e8af5cae7fdf9749baf87606568d3af4d03082/src/libqmi-glib/qmi-enums-wda.h
[wds]: https://github.com/linux-mobile-broadband/libqmi/blob/d9e8af5cae7fdf9749baf87606568d3af4d03082/data/qmi-service-wds.json
[wds-enums]: https://github.com/linux-mobile-broadband/libqmi/blob/d9e8af5cae7fdf9749baf87606568d3af4d03082/src/libqmi-glib/qmi-enums-wds.h
[rmtfs-main]: https://github.com/linux-msm/rmtfs/blob/b30a3eb38f9af283f18dbd3c7755653efc52c094/rmtfs.c
[rmtfs-qmi]: https://github.com/linux-msm/rmtfs/blob/b30a3eb38f9af283f18dbd3c7755653efc52c094/qmi_rmtfs.qmi
[rmtfs-storage]: https://github.com/linux-msm/rmtfs/blob/b30a3eb38f9af283f18dbd3c7755653efc52c094/storage.c
[ipa-service]: https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/dataipa/drivers/platform/msm/ipa/ipa_v3/ipa_qmi_service.c
[ipa-ei]: https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/dataipa/drivers/platform/msm/ipa/ipa_v3/ipa_qmi_service_v01.c
[ipa-uapi]: https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/dataipa/drivers/platform/msm/include/uapi/linux/ipa_qmi_service_v01.h
[ipa-rmnet]: https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/dataipa/drivers/platform/msm/ipa/ipa_v3/rmnet_ipa.c
