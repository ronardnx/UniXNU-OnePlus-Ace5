# Wi-Fi 映射层

本说明对应 `ios16/wifi/giulia_wifi_mapping.h`、`ios16/wifi/giulia_wifi_apple.h` 和
`ios16/wifi/GiuliaWiFi.cpp`。映射规则以纯 C helper 表达，运行时负责 IOKit 对象、
锁、设备访问与退出顺序。当前功能范围包括 PCIe / SMMU / MHI / QMI / CE / WMI
所需的地址转换，以及 iOS 16.7.16 / 20H392 的扫描与单站接口。

这些代码和主机测试不代表射频或联网验证完成。后续已接入关联、认证、密钥安装和
数据收发代码，见 [Wi-Fi 联网](WIFI_NETWORK.md)；完整初始化成功后才发布 `en0`
和 IO80211 服务。下文扫描 ABI 的默认行为由网络层在实际就绪时扩展。

## 硬件资源

`gwmap_resource()` 集中定义资源范围。前八个资源编号与
`giulia_wifi_link.h` 的 `GWL_*` 一致，链路训练与后续驱动使用同一组映射。
下表的大小是逻辑资源边界；IOKit 的页粒度映射不扩大寄存器访问权限。

| 资源 | 物理基址 | 大小 | 用途 |
| --- | --- | --- | --- |
| ROOT | `0x60000000` | `0x2000` | PCIe 根端口及 ATU |
| PARF | `0x01c00000` | `0x3000` | PCIe 平台控制 |
| PHY | `0x01c06000` | `0x2000` | PCIe PHY |
| GCC | `0x0016b000` | `0x2000` | 时钟及复位 |
| GPIO | `0x0f110000` | `0x51000` | WLAN 控制引脚 |
| CFG | `0x60100000` | `0x1000` | 枚举后端点配置空间 |
| REFCLK | `0x01ff1000` | `0x1000` | 参考时钟 |
| CRM | `0x01d05000` | `0x2000` | 资源管理 |
| SMMU | `0x15000000` | `0x100000` | 独立 DMA 地址转换 |
| QMP | `0x0c300000` | `0x400` | APSS 与 AOP 邮箱 |
| IPCC | `0x00406000` | `0x1000` | QMP 门铃 |
| BAR | PCI 枚举结果 | PCI 枚举结果 | WCN7850 寄存器窗口 |

BAR 必须位于 `[0x60300000, 0x64000000)`，大小至少为 1 MiB、为 2 的幂，
基址按大小对齐。驱动在枚举后才接受该资源，不把固定地址当作已枚举 BAR。

`gw_map_region()` 保存 `IOMemoryDescriptor` 和 `IOMemoryMap`，使用
`kIOMapInhibitCache`。同一资源重复请求复用已持有的映射；失败时清理本次
创建的对象。`gw_register()` 在计算 CPU 指针之前检查资源、4 字节对齐、
偏移和溢出。非法读返回 `0xffffffff`，非法写不访问设备，并记录
`mapping_error`。

MHI 寄存器位于 BAR 的低 512 KiB 固定窗口。CE 使用第二个 512 KiB 窗口，
由 `0x310c` 选择设备地址。`gwmap_window()` 限制设备地址在 32 MiB 范围内，
检查对齐、窗口边界和实际 BAR 容量，保留无关窗口配置位。运行时串行执行
窗口切换并检查读回，避免把超范围地址截成六位窗口号后访问到其他寄存器。

## DMA 地址与分区

64 MiB 连续 arena 使用 CPU 虚拟地址、真实物理地址和设备 IOVA 三种地址。
`gwmap_dma_at()` 以经过校验的 arena 偏移同时计算三者：

```text
CPU 地址 = dma.cpu      + offset
物理地址 = dma.physical + offset
设备地址 = 0x20000000   + offset
```

`gwmap_dma_from_cpu()`、`gwmap_dma_from_physical()` 和
`gwmap_dma_from_iova()` 分别检查对应地址空间的范围后得到偏移。设备收到的
QMI 地址是 IOVA；即使分配回调的历史参数名为 `pa`，也不能传入真实物理地址。
没有使用 CPU 指针或恒等映射推断设备地址。

下表区间均为左闭右开。

| 分区 | arena 偏移 | 设备 IOVA | 容量 |
| --- | --- | --- | --- |
| MHI / 固件暂存 | `[0, 0x1200000)` | `[0x20000000, 0x21200000)` | 18 MiB |
| CE | `[0x1200000, 0x1400000)` | `[0x21200000, 0x21400000)` | 2 MiB |
| QMI / 数据区动态资源 | `[0x1400000, 0x4000000)` | `[0x21400000, 0x24000000)` | 44 MiB |

arena 的 CPU 地址至少按 16 KiB 对齐，物理地址与 IOVA 按 2 MiB 对齐。
物理范围必须低于 36 位地址上限；IOVA 位于 32 位空间，且不跨 1 GiB 边界。
独立的 16 KiB 页表分配按 16 KiB 对齐，与 arena 的 CPU 和物理范围均不重叠。
运行时同时检查连续物理段长度以及启动时声明的 RAM 边界。

QMI 从第三个分区按 16 KiB 对齐分配；空间不足、越界或对齐不满足时不推进
分配游标。MHI 同步偏移相对于 arena 起点，CE 同步偏移相对于 CE 分区，
`gc_sync()` 先加上 `0x1200000` 再检查完整范围。`gq_clean()` 只接受已经分配
的 QMI 区域，不接受其他分区或尚未分配的尾部空间。

固件 QMI 资源分配完成后，`gn_prepare()` 从同一动态尾区分配独立 4 MiB 数据区，
不预设它的固定 IOVA。`dp_offset` 由实际 IOVA 减 arena IOVA 得到；
`gd_sync()` 使用 `gwmap_dma_allocated_offset()`，先确认整个数据区位于已分配的
动态范围，再检查相对访问。越界、未分配尾部和加法回绕不能落入 MHI / CE 分区。

arena 和页表采用无缓存 CPU 映射。同步回调保留设备可见性屏障，并检查地址
范围；MHI/QMI wire helper 继续使用字节访问。主机测试检查优化后的 AArch64
访问指令，不能据此确认实机缓存一致性或设备完成 DMA。

## 资源退出与保留

正常退出先停止 CE/MHI，关闭 PCI bus mastering，再恢复 MSI、SMMU、SID 和
PCI 配置，最后释放 arena、页表及 MMIO 映射。MMIO 按资源编号逆序释放，
先释放 map，再释放 descriptor。部分初始化失败也通过同一退出路径清理。
服务停止请求由工作线程完成退出，随后撤销 IO80211 服务和 BSD 接口。

DMA 停止或恢复结果不确定时保留对应内存和映射，避免仍在访问旧地址的设备
使用已经释放的内存。`quarantine`、MHI quarantine 和 SMMU quarantine 都参与
释放判断。

QMP 通道存在 pending 请求、状态不确定或关闭失败时，设置 `qmp_retained`。
此时保留原 QMP/IPCC 映射、运行时上下文和所有者令牌，禁止另一客户端复用该
邮箱。已确认停止的 PCIe DMA 资源可以独立回收。QMP 命令被消费不等于已经
读回或验证电压，不能把软件应答解释为电源测量结果。

## iOS 请求映射

`gwi_decode()` 接受两种不同容量的代理报文，接口名必须为 `en0`。输入先复制
到内核缓冲区，不跟随尾部结构内的用户指针。

| 操作 | command | 完整代理报文 | payload 上限 |
| --- | --- | --- | --- |
| GET | `0xc02869c9` | `0x3c30` | `0x3c00` |
| SET | `0x802869c8` | `0x3c030` | `0x3c000` |

`gwi_query()` 使用运行时锁保护的状态快照。GET 输出容量可以大于实际结果，
成功时仅返回实际长度；失败时输出长度为零，纯映射层不改变状态、输出内容或
结果游标。SET 允许无输出；请求回显时必须提供完整容量，先校验再改变状态。
直接缓冲区及 `IOMemoryDescriptor` 输入/输出均由 UserClient 包装层处理。

| GET 类型 | 请求 payload / 返回字节数 | 当前映射 |
| --- | --- | --- |
| 11 SCAN_RESULT | `1164 / 1164` | 真实接收的 BSS；扫描中返回 Busy，结束返回 `0xe0820446` |
| 103 CURRENT_NETWORK | `1164 / 1164` | 实际关联 peer 的完整 BSS 快照；支持扫描期间查询，不消费扫描游标 |
| 12 CAPABILITIES | `13 / 13` | 数据区就绪后提供 CCMP / WPA2 / SAE（位 75）；未确认的其他能力位为零 |
| 13 STATE | `0 / 4` | 连接调度与 station 映射为 INIT / SCAN / AUTH / ASSOC / RUN；连接中扫描保留 RUN |
| 14 PHY_MODE | `12 / 12` | version=1，`+4` 支持 legacy 模式为 `0x0e`，`+8` 来自已关联频道；未关联为零 |
| 15 OP_MODE | `0 / 4` | infrastructure station |
| 19 POWER | `24 / 24` | 一个逻辑 radio 的扫描使能状态 |
| 5 POWERSAVE | `0 / 4` | 显式提交 WMI ACTIVE 策略后返回 disabled=0；策略未建立时返回 NotReady |
| 8 RATE | `0 / 4` | 当前 peer 最后有效 TX 样本的整数 Mbps，kbps / 1000 截断；无样本或未关联返回 NotReady |
| 57 MCS | `0 / 4` | 当前协商仅 legacy，按原厂成功包装路径返回兼容默认 0；不表示有效 HT MCS0 |
| 18 INT_MIT | `0 / 4` | 同版原厂 GET / SET 也直接 Unsupported；不以 IRQ polling 推断其语义或伪造模式 |
| 23 DEVICE_NAME | `1..1024 / 同请求` | 保留原有有界设备名扩展；不作为 Apple80211Get 已支持操作的证据 |
| 27 CHANNELS | `4824 / 4824` | 与 WMI 一致的 22 个 2.4/5 GHz 被动扫描信道，ACTIVE 位不置位 |
| 43 DRIVER_VERSION | `0 或 256 / 256` | Apple80211Bind 首次空 payload 查询及通用 GET 均返回完整版本字符串 |
| 44 HARDWARE_VERSION | `256 / 256` | WCN7850 硬件标识 |
| 51 COUNTRY | `3 / 3` | 当前固定配置 `CN` |
| 54 RADIO_INFO | `8 / 8` | 从 WMI service-ready 获得的 RF 链数 |
| 1 / 4 / 9 SSID / CHANNEL / BSSID | `32 / 16 / 6` | 来自实际关联 peer；没有关联时返回 NotReady |
| 16 / 17 RSSI / NOISE | 各 `52` | 来自实际关联 peer；没有关联时返回 NotReady |
| 2 AUTH_TYPE | `16 / 16` | 实际 peer 的开放 / WPA2 / SAE 类型；没有关联时返回 NotReady |
| 21 / 50 ASSOCIATE_RESULT / ASSOCIATION_STATUS | 各 `0 / 4` | 网络层就绪后可查询 pending、真实完成或失败结果，不依赖有效 peer |
| 371 ASSOCIATE_RESULT_EXTENDED | `0 / 4` | 使用目标框架与 GET21 共用的 Apple 关联结果域；不混入 IEEE / WMI 原始 status |
| 其他 | — | Unsupported |

STATE 和 OP_MODE 由框架通过 `req_val` 传递，代理请求长度为零、输出为四字节。
该差异与 GET 结构长度来自本地 20H392 IO80211 的 dispatch 反汇编，不能套用
桌面 macOS 的 `apple80211_*` 结构大小。关键位置为 `0x1d3ec4038`、
`0x1d3ecb034..0x1d3ecb06c`，GET jump table 位于 `0x1d3ef565e`。
SAE 能力对应第 9 字节的 `0x08`：Bind 把 13 字节复制到 handle `+0x50`，
手动与自动认证路径检查 `+0x59` 的 bit 3，未置位会在发送 SET20 前返回 Unsupported。
该位只由已经接入 SAE / PMF 后端的网络层提供，纯扫描层不声明支持。
Apple80211Bind 的首次 DRIVER_VERSION 请求在 `0x1d3ed4ef8` 清零完整信封，
仅填入 type 43 后在 `0x1d3ed4f5c..0x1d3ed4f7c` 请求 256 字节输出；
因此只为该已观察形式接受零输入长度，其余非 scalar GET 的长度约束保留。

| SET 类型 | payload 字节数 | 当前行为 |
| --- | --- | --- |
| 10 SCAN_REQUEST | `5456` | 校验并快照 SSID / BSSID / 频道后排队；支持合法通配 SSID，合并重复 SSID；主动发送受监管规则约束 |
| 19 POWER | `24` | version=1、逻辑 radio 数为1、状态为0或1；扫描期间关闭返回 Busy |
| 20 ASSOCIATE | `592` | 解码开放 / WPA2 PMK / SAE 密码请求；扫描选择、认证、密钥确认后才完成 |
| 22 DISASSOCIATE | `0 或 8` | 普通形式读取 scalar value；DisassociateExt 读取 payload `+4` 原因，首 word 不作 version；立即闭口，再由 worker 断连 |
| 其他 | — | Unsupported |

POWER 控制软件使能并在关闭时断连，不承担物理下电或独立 RF 链切换。网络层
将扫描筛选映射到 WMI 请求，主动扫描只使用当前监管规则允许的频道。每个 UserClient
独立保存 `{index, generation}`；检测到 WMI `scan_id` 改变时从新结果的第一项
读取，防止其他客户端启动扫描后沿用旧结果下标。

UserClient 的请求锁覆盖输入快照、查询和输出回传；只有完整回传成功才提交
扫描游标。descriptor 短写后重试仍返回同一条结果，同一客户端的并发请求
不会同时消费同一个游标。请求锁分配失败时拒绝打开客户端，销毁时成对释放。

扫描缓存不代表当前连接。网络层仅在真实关联、WPA2 密钥完成确认后放行 BSD 输出，
此前返回 `ENETDOWN`；Ethernet 协议接入和解复用使用 XNU 的实现。
当前 BSSID、RSSI 来自已确认的 peer，关联结果来自连接调度和 station 完成事件，
不能由扫描缓存冒充。
未关联时的空 peer 不参与 CAPABILITIES / STATE / 未连接 PHY / 关联结果校验。
旧 AP 的 GTK 更新可重复进入 RUN；新连接仍在选择、扫描或断开旧 AP 时，该事件
不能完成新连接请求或改写其 pending 结果。替换网络已经失败后，迟到的旧 AP
密钥更新也不得覆盖失败结果或重复发出关联完成通知。

## BSD 联网控制映射

`gn_ioctl()` 直接处理 Ethernet 接口建立 IP 配置所需的请求。目标 XNU 的
`ether_ioctl()` 固定返回 `EOPNOTSUPP`，不能代替这些驱动回调。

| 请求 | 映射行为 |
| --- | --- |
| `SIOCSIFADDR` | 置逻辑 `IFF_UP`；仍由真实关联及密钥完成决定数据端口 |
| `SIOCSIFMTU` / `SIOCGIFDEVMTU` | 验证并设置 `IF_MINMTU=72` 至 1500；返回当前值与上下限 |
| `SIOCADDMULTI` / `SIOCDELMULTI` | BSD 维护组成员；已有 HTT RX 订阅接收组播 / 广播，不伪造固件地址表 |
| `SIOCGIFMEDIA32` / `SIOCGIFMEDIA64` | 返回实际就绪、使能及受控端口状态；不跟随用户列表指针 |
| `SIOCSIFMEDIA` | 仅接受当前 802.11 自动选择 |
| `SIOCSIFFLAGS` | DOWN 时立即闭口、清待发队列 / 会话凭据，随后由 worker 断连 |

WMI 与 iOS 的 legacy 当前 PHY 通过同一 OFDM 速率判定区分 11a / 11b / 11g，
不再把全部 2.4 GHz 连接都报告成 11g。取消尚未完成的连接也会结束关联结果，
并继续等待固件扫描归还。FW TX 通知中的非完成状态保留 DMA 所有权，避免过早复用。

## 主机验证

在具有 clang、clang++ 和 Python 3 的 Linux/WSL 环境中，从仓库根目录运行：

```sh
bash scripts/test-ios16-wifi-runtime.sh
```

该脚本运行现有传输协议测试、`ios16_wifi_mapping.c` 地址边界与分区测试、
`ios16_wifi_apple.c` 请求映射测试、DMA 字节访问检查、显示测试和 C++14 头文件
编译。C/C++ 执行测试启用 AddressSanitizer 与 UndefinedBehaviorSanitizer。

`tests/ios16_wifi_runtime.py` 从当前 `GiuliaWiFi.cpp` 提取真实方法体，使用有界
IOKit stub 检查 MMIO 所有权、部分映射失败、释放/保留、连续分配、GET/SET 长度、
descriptor 读写失败和客户端游标。它不替换生产映射/query 实现，不访问设备。

需要单独运行生产方法的主机测试时：

```sh
python3 tests/ios16_wifi_runtime.py
```

这些测试覆盖软件边界和资源生命周期。最终内核构建、固件运行、真实扫描、
射频及联网结果需要各自独立验证，本说明不宣称已完成实机验证。

2026-10-23 最初映射层检查：完整 Wi-Fi 运行时脚本通过，其中新增地址映射测试
82 项、实际驱动函数测试 9 组，包含双线程客户端及 descriptor 短写回归；
C++14 头文件和 AArch64 `-O2` / `-O3` DMA 字节访问检查通过。
全仓 unittest 运行 145 项，3 项因工作区另行开发的
`ios16/audio/giulia_audio_mapping.h` 尚缺失而报错，1 项跳过；未将该结果记为全仓通过。

2026-10-23 本次续接检查：完整 Wi-Fi 运行时脚本通过，包含 100 项地址映射检查、
174 组数据环初始化 / 失败场景及实际驱动方法回归。新增默认未关联 GET、动态数据区
越界和 GTK 1/2/3 重放窗口回归；C++14 头文件及 AArch64 DMA 字节访问检查通过。
全仓 unittest 共运行 280 项，无失败，3 项跳过；两个固定版本上游布局校验通过。
关联、RSN 与数据环覆盖见 [Wi-Fi 联网](WIFI_NETWORK.md)。

同日再次续接补齐了 Apple80211Bind 的零 payload 版本查询，以及替换连接失败后
迟到密钥更新的结果保留。接收密钥归属改用 WCN7850 空口 RA 的 MCBC 标志，并纠正
上一轮把描述符 KEY_ID 当作空口 IV 字节的判断；测试直接使用固定上游 HAL 语句，
当前校验 10 个源码哈希、原厂速率表和三段语义片段。相关完整 UserClient、BSD ioctl、RSN、取消
连接及数据面回归已纳入主脚本，station 序列额外覆盖 CCK / OFDM / 5 GHz 的实际 PHY。
完整框架接口核对又补齐了 8 字节 DisassociateExt 和 GET103 当前网络查询。
主机组合回归从实际 WPA2 / 固件密钥确认开始，经 BSD 地址启用、TCL/WBM、RXDMA/REO
到 BSD 输入，保持 ARP / IPv4 UDP / IPv6 帧字节不变，并检查 PN 重放和接口 DOWN。
该模型不运行完整 IP 栈，不将 DHCP / DNS 格式帧的驱动回传等同于真实地址分配或解析成功。
`tests/ios16_wifi_apple_oracle.py` 直接读取本地 20H392 dyld cache 验证这些核心
请求的跳表和指令字节；GET371 已按原始消费者映射到关联结果域。
目标机器码校验也包含 SAE 能力位的 Bind 复制、两条认证入口及 PMF 默认选择依据，
避免后端已实现却被框架能力检查提前拒绝。

本轮又补齐固件 ACTIVE 策略与 GET5：发布接口及每次关联前，先提交四条 STA PS
参数及 MODE_DISABLED。`active_policy` 只表示 WMI 命令成功提交，不表示功耗读回。
数据面从有效 TQM 已发送完成和本 peer 的有效 RX 元数据记录独立的 TX / RX
速率、raw 和时间戳，按固定上游表解码 20 MHz legacy kbps；无效、未发送、其他 PHY
或旧 peer 不产生虚假的有效速率。该内部单位不会在缺少目标 ABI 证据时直接填入 GET8。

后续已使用固定 20H392 原内核闭合 RATE / MCS / INT_MIT 生产链：GET8 原厂缓存值
除以 1000 后由 Family 原样拷为 scalar；GET57 原厂在 legacy 分支成功而不填 index，
Family 的零初始化字段因此原样可见为 0；GET18 / SET18 都直达 Unsupported。
使用 `python tests/ios16_wifi_apple_oracle.py --stock-kernel out/iphone16-audit/kernel.raw`
可对原始内核 SHA-256、正确请求号、跳表、vtable 和生产 / 包装指令逐项复验。
扫描结果和当前网络 BSS 的 `+0x24` 速率数组也按目标消费者修正为整数 Mbps，
先清空 basic-rate 位再把空口 500 kbps 编码除以 2；5.5 Mbps 以整数 5 表示。
原始 IE、扫描缓存和关联用速率保持空口编码，不改变实际协商内容。

## 完成验收缺口

当前主机测试和原厂 ABI 对照能够证明源码中的边界、字段映射与模型调用链；
不能证明以下设备端要求已经满足，因此没有把“完整映射 Wi-Fi”目标标为完成：

| 设备端要求 | 当前证据 | 仍需取得 |
| --- | --- | --- |
| 固件完成启动 | 最近设备记录停在旧镜像 QMP 打开，后续修复有主机测试 | 包含当前改动的新内核运行至 `FW_INIT_DONE` 的完整日志 |
| 映射与 DMA 在设备上有效 | MMIO / IOVA / 生命周期主机模型通过 | 真机 CE / HTT / TCL / REO 收发及无 DMA fault 的记录 |
| 扫描、认证、密钥与状态接口联动 | 原 20H392 生产者 / 消费者对照和实际函数主机回归通过 | 当前驱动的真实扫描、开放 / WPA2 / SAE 关联及密钥完成 |
| IP 联网与重连 | 合成协议帧的驱动回传通过，未运行实际 IP 栈 | DHCP、DNS、双向流量、关闭开启与断线重连的实测 |

用户此前明确要求“不要构建”；在该限制未改变且没有外部新构建实测资料时，
不自行构建内核、打包测试镜像、触发 CI 或把旧镜像的结果当作当前代码的验证。
