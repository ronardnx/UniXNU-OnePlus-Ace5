# Wi-Fi 映射层

本说明对应 `ios16/giulia_wifi_mapping.h`、`ios16/giulia_wifi_apple.h` 和
`ios16/GiuliaWiFi.cpp`。映射规则以纯 C helper 表达，运行时负责 IOKit 对象、
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
| QMI 动态资源 | `[0x1400000, 0x4000000)` | `[0x21400000, 0x24000000)` | 44 MiB |

arena 的 CPU 地址至少按 16 KiB 对齐，物理地址与 IOVA 按 2 MiB 对齐。
物理范围必须低于 36 位地址上限；IOVA 位于 32 位空间，且不跨 1 GiB 边界。
独立的 16 KiB 页表分配按 16 KiB 对齐，与 arena 的 CPU 和物理范围均不重叠。
运行时同时检查连续物理段长度以及启动时声明的 RAM 边界。

QMI 从第三个分区按 16 KiB 对齐分配；空间不足、越界或对齐不满足时不推进
分配游标。MHI 同步偏移相对于 arena 起点，CE 同步偏移相对于 CE 分区，
`gc_sync()` 先加上 `0x1200000` 再检查完整范围。`gq_clean()` 只接受已经分配
的 QMI 区域，不接受其他分区或尚未分配的尾部空间。

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
| 12 CAPABILITIES | `13 / 13` | 未实现的关联、加密、AP 和数据面能力位为零 |
| 13 STATE | `0 / 4` | 待处理或正在扫描为 SCAN，其余为 INIT |
| 14 PHY_MODE | `12 / 12` | version=1，`+4` 支持模式为 `0x1e`，`+8` 当前模式为零 |
| 15 OP_MODE | `0 / 4` | infrastructure station |
| 19 POWER | `24 / 24` | 一个逻辑 radio 的扫描使能状态 |
| 23 DEVICE_NAME | `1..1024 / 同请求` | 保留原有有界设备名扩展；不作为 Apple80211Get 已支持操作的证据 |
| 27 CHANNELS | `4824 / 4824` | 与 WMI 一致的 22 个 2.4/5 GHz 被动扫描信道，ACTIVE 位不置位 |
| 43 DRIVER_VERSION | `256 / 256` | 驱动版本字符串 |
| 44 HARDWARE_VERSION | `256 / 256` | WCN7850 硬件标识 |
| 51 COUNTRY | `3 / 3` | 当前固定配置 `CN` |
| 54 RADIO_INFO | `8 / 8` | 从 WMI service-ready 获得的 RF 链数 |
| 1 / 4 / 9 SSID / CHANNEL / BSSID | `32 / 16 / 6` | 没有关联时返回 NotReady |
| 16 / 17 RSSI / NOISE | 各 `52` | 没有关联时返回 NotReady |
| 21 / 50 ASSOCIATE_RESULT / ASSOCIATION_STATUS | 各 `0 / 4` | 没有关联时返回 NotReady |
| 其他 | — | Unsupported |

STATE 和 OP_MODE 由框架通过 `req_val` 传递，代理请求长度为零、输出为四字节。
该差异与 GET 结构长度来自本地 20H392 IO80211 的 dispatch 反汇编，不能套用
桌面 macOS 的 `apple80211_*` 结构大小。关键位置为 `0x1d3ec4038`、
`0x1d3ecb034..0x1d3ecb06c`，GET jump table 位于 `0x1d3ef565e`。

| SET 类型 | payload 字节数 | 当前行为 |
| --- | --- | --- |
| 10 SCAN_REQUEST | `5456` | version=1 后排队被动扫描；关闭状态返回 NotReady，重复请求返回 Busy |
| 19 POWER | `24` | version=1、逻辑 radio 数为1、状态为0或1；扫描期间关闭返回 Busy |
| 其他 | — | Unsupported |

POWER 当前控制扫描请求的使能，不承担物理下电或独立 RF 链切换。SSID/信道
筛选沿用框架对返回结果的过滤，硬件扫描使用固定被动列表。每个 UserClient
独立保存 `{index, generation}`；检测到 WMI `scan_id` 改变时从新结果的第一项
读取，防止其他客户端启动扫描后沿用旧结果下标。

UserClient 的请求锁覆盖输入快照、查询和输出回传；只有完整回传成功才提交
扫描游标。descriptor 短写后重试仍返回同一条结果，同一客户端的并发请求
不会同时消费同一个游标。请求锁分配失败时拒绝打开客户端，销毁时成对释放。

扫描缓存不代表当前连接。网络层仅在真实关联、WPA2 密钥完成确认后放行 BSD 输出，
此前返回 `ENETDOWN`；Ethernet 协议接入和解复用使用 XNU 的实现。
当前 BSSID、RSSI 和关联结果来自已确认的 peer，不能由扫描缓存冒充。

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
`ios16/giulia_audio_mapping.h` 尚缺失而报错，1 项跳过；未将该结果记为全仓通过。

同日联网续接检查已更新为 13 组实际运行时场景；当前全仓 215 项通过、1 项跳过。
新增关联、RSN 与数据环覆盖见 [Wi-Fi 联网](WIFI_NETWORK.md)。
