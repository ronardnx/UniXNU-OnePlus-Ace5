# giulia 基带硬件与共享资源映射

本表固定 OnePlus Ace 5 / SM8650 的捕获 Android 设备树，以及同版本族的 OnePlus 原厂源码。
它描述接线、资源、协议对象和实现边界；不把静态地址、软件租约、QMI 成功或回调注册视为硬件已经工作。
整体链路和共同生命周期见 [统一框架](MODEM_ARCHITECTURE.md)，逐机制契约见
[机器清单](../hardware/sm8650-modem-mechanisms.json)，固件内存交接详见 [启动映射](MODEM_BOOT_MAP.md)。

## 固定输入与可复现核验

完整数值、节点路径、phandle 引用、源码路径与 SHA256 都保存在
[sm8650-modem.json](../hardware/sm8650-modem.json)。核验器检查父子作用域和实际 provider 引用，
不会只因为数值与某个 phandle 相等便认定是同一控制器。

```sh
python scripts/ios16_modem_board.py --dts ../ace5-analysis/running-device.dts --source-root .cache/audio-source --local-evidence
```

板级部分包含 44 个节点、218 项属性、29 个关联；IPA 部分从固定源码重建 31 条生产端点和
10 条测试别名。板级核验结果需与构建、产物及实机记录一并评估。
原始 DT 中的设备标识和启动参数不进入公开清单。

## 固件、MMIO 与内存

| 对象 | 地址/长度或标识 | 约束 |
| --- | --- | --- |
| MPSS remoteproc | `0x04080000 / 0x10000` | `qcom,pineapple-modem-pas`，只有一组 MMIO |
| 主镜像 / DTB | `modem.mdt` PAS `4` / `modem_dtb.mdt` PAS `38` | 必须验证配套固件身份、段布局和实际输入 ELF 格式 |
| 主保留区 | `0x8bc00000 / 0x0f400000` | 固定运行 DT 描述；不是可自由分配 RAM |
| DTB 保留区 | `0x9b000000 / 0x80000` | 独立镜像和认证生命周期 |
| DSM1 / DSM2 | `0x86b00000 / 0x4900000`；`0x8b400000 / 0x800000` | 启动/停止涉及 HLOS 与 MSS_MSA 内存授权 |
| QLINK 日志 | `0x84800000 / 0x200000` | 是 `mpss_dsm_mem_reg` 第三个引用，也须处理授权 |
| SMEM | `0x81d00000 / 0x200000` | 共享框架和独立 host 分区；不能整体清零 |
| IPA FW / GSI | `0x9b080000 / 0x10000`；`0x9b090000 / 0xa000` | IPA 节点引用后者，固件名 `ipa_fws`、PAS `15` |
| `memory-region` 第三项 | `linux,cma`，大小 `0x2000000`，对齐 `0x400000`，32 位范围内 | 动态分配池，不是已知固定 modem 固件地址 |
| IPCC MPROC | `0x00406000 / 0x1000` | modem、ADSP、AOP 共享控制器 |
| AOP QMP | `0x0c300000 / 0x400` | APSS 共用单通道及事务串行化 |
| IPA / GSI MMIO | `0x03e00000 / 0x84000`；`0x03e04000 / 0xfc000` | 描述范围有重叠，不能当成两个任意独占映射 |

`reg-names = cx,mx` 被原厂 PAS 驱动用作供电名称，不能由名字数量推导第二组寄存器。
`qcom_q6v5_init()` 的可选 RMB 使用资源索引 1；此 DT 没有该资源，不能把 RMB 偏移直接加到
`0x04080000`。`uses_elf64` 决定 coredump 格式，而对应 MDT loader 使用 ELF32 结构。
本轮已从匹配的完整原厂 ROM 核验实际 modem/DTB/IPA 均为 ELF32 LE：主固件实际加载范围
`[0x8bc00000,0x99e00000)`，DTB 为 `[0x9b000000,0x9b003d3c)`；全部非空加载段长度匹配，
缺失编号属于零文件大小 BSS。结构与源摘要见 [stock 证据](../hardware/sm8650-modem-stock.json)，
不等于安全世界已执行认证或基带已启动。

现有 UEFI `PIL_Reserved` 从 `0x8bc00000` 开始，本条目本身不涵盖两个 DSM 区和 QLINK 日志区。
stock `FULL_MODEM` 配置从 `0x8a200000` 开始，与运行 DT 起点不同，但都结束于 `0x9b000000`。
stock RETAIL 镜像列表没有 `FULL_MODEM`。当前实际 ELF 的 entry/min 已确认是 `0x8bc00000`，
重现运行 DT 时不能沿用旧配置强制移到 DSM 内。静态表缺少 DSM/QLINK 也不是排除证明：
MemoryMapHelper 只查询，giulia 实际加载的 RamManagerDxe 会把 firmware RAM partition 中未被
已有 EFI 描述符覆盖的空隙加入普通 RAM。最终是否覆盖这些区取决于真实 provider 返回和更早的
排除描述符，详见启动映射的完整源码链。

当前 overlay 已修复这项基线遗漏：在相同 MemoryMapLib 中仅新增上述 QLINK 与两个 DSM
保留条目，属性均为 `AddMem/MEM_RES/UNCACHEABLE/Reserv/UNCACHED_UNBUFFERED_XN`。
新表 41 条、原表 38 条，全表无重叠，其余源文本不变。生产 `prepare.apply_uefi_overlay()`
在复制前校验固定基线或精确幂等状态，复制后核验 DSC/INF/FDF 与段表覆盖；没有把源码保留
推断为安全世界权限或实时硬件所有权。原始 UEFI pin `6c675501...` 已确认包含板文件，
对应 MemoryMapLib blob 为 `f9023c8561c70d75dc128fa8af98211cc353dbae`。

## IRQ、SMP2P、SMEM 与 GLINK

SPI 是设备树 GIC 编号，INTID 是 `SPI + 32`。SMP2P 位不是独立 GIC IRQ。

| 事件 | 实际 provider/编号 | 作用 |
| --- | --- | --- |
| MPSS watchdog | GIC SPI `264`，INTID `296`，flag `1` | fatal 状态按 epoch 锁存，优先于 ready |
| fatal | modem SMP2P `slave-kernel` bit `0` | 失效当前控制、持久化与数据操作 |
| handover | 同上 bit `2` | 按原厂生命周期处理资源交接 |
| ready | 同上 bit `1` | 必须是本次启动的新事件 |
| stop-ack | 同上 bit `3` | 不由本地停止请求自行推断 |
| shutdown-ack | 同上 bit `7` | 与 PAS 关闭/DSM 回收一并确认 |
| stop request | modem SMP2P `master-kernel` bit `0` | AP 发出的停止状态 |
| IPCC parent | GIC SPI `229`，INTID `261`，flag `4` | 同一控制器的不同 client/signal 分发 |
| IPA interrupt | GIC SPI `654`，INTID `686`，flag `4` | IPA 事件 |
| GSI interrupt | GIC SPI `432`，INTID `464`，flag `4` | GSI 队列和错误事件 |

| 通信对象 | 固定映射 | 生命周期要求 |
| --- | --- | --- |
| modem GLINK | remote PID `1`，IPCC client `2` / signal `0` | 与 ADSP PID `2` 的 edge 分离 |
| modem SMP2P | AP PID `0` ↔ modem PID `1`；IPCC `2/2` | 入站 item `435`，出站 item `428` |
| SMEM allocator | `qcom,tcsr-mutex`，hardware lock `3` | 本地 CPU mutex 不能替代跨处理器硬件锁 |
| GLINK descriptors | item `478`，32 字节 | 在 host1 分区查找，保留已验证布局 |
| TX / RX FIFO | item `479` / `480`，TX 分配 `16 KiB` | 指针、容量、缓存屏障及通知顺序必须协同 |
| QRTR channel | `IPCRTR`；intent `(0x800,5),(0x2000,3),(0x4400,2)` | 服务地址和事务绑定 edge/epoch |
| DS channel | `DS`；intent `(0x4000,2)` | 不因名字存在就假定用户数据走此通道 |
| IPA SMP2P | `ipa` 入/出 entry | 与 modem kernel stop/ready entry 分离 |
| AOP QMP | IPCC `0/0` | 所有 APSS QMP 客户端共享一个事务 owner |

SMEM 分配、GLINK intent、QRTR 服务、QMI pending、远端存储 caller 及数据 bearer 必须在同一
remote epoch 变化时共同失效。单独重置某层序号会把旧完成事件误认为新一轮结果。

## 电源、时钟与互连

| 消费者 | provider / 原厂参数 | 说明 |
| --- | --- | --- |
| modem XO | `/soc/rsc@17a00000/drv@2/clock-controller`，`RPMH_CXO_CLK = 0` | 名称 `xo` |
| modem CX | `rpmh-regulator-cxlvl` / `cx.lvl` / `pm_v8_s8_level` | 请求 `[0x180, 100000]`；level 语义不能误当真实微伏值 |
| modem MX | `rpmh-regulator-msslvl` / `mss.lvl` / `pm_v6g_s5_level` | 请求 `[0x140, 100000]`，不是通用 `mx.lvl` |
| modem AOP | `power-controller@c300000`，QMP name `modem` | load-state 与电源/时钟/带宽同周期持有 |
| modem `rproc_ddr` | `mc_virt MASTER_LLCC(4)` → `mc_virt SLAVE_EBI1(512)` | 原厂 interconnect route |
| modem `crypto_ddr` | `aggre2_noc MASTER_CRYPTO(41)` → `mc_virt SLAVE_EBI1(512)` | 认证/搬运依赖 |
| IPA core clock | 同 RPMh provider，`RPMH_IPA_CLK = 12` | 原厂 `core_clk` |
| IPA→LLCC | `aggre2_noc MASTER_IPA(42)` → `gem_noc SLAVE_LLCC(561)` | 带宽由统一 provider 管理 |
| LLCC→DDR | `mc_virt MASTER_LLCC(4)` → `mc_virt SLAVE_EBI1(512)` | 与其他客户端聚合 |
| AP→IPA config | `gem_noc MASTER_APPSS_PROC(3)` → `cnoc_main SLAVE_IPA_CFG(532)` | 配置访问依赖 |

modem 节点没有 `power-domains` 属性。其明确依赖是上述 regulator、XO、AOP 与 interconnect。
不同客户端释放请求不能清除另一客户端的票；软件 aggregate 也必须通过同一 provider 事务与
实际硬件完成保持一致，不能在另一投票已提交后应用过期快照。

## IPA、GSI 与 SMMU

`ipa-hw-ver = 0x18` 在固定 `msm_ipa.h` 中对应 `IPA_HW_v5_5 = 24`；
`platform-type = 1` 对应 MSM，因此选择 `IPA_5_5`，不选 XR 表。GSI 版本是 `GSI_VER_5_5`。
表项来自 `ipa_utils.c`，字段布局由 `include/linux/ipa.h` 的 `ipa_gsi_ep_config` 定义：
`endpoint, channel, TLV, AOS, EE, prefetch_mode, prefetch_threshold`。

| SMMU consumer | SID / mask | 补充约束 |
| --- | --- | --- |
| IPA AP | `0x4a0 / 0` | IOVA 池 `0x20000000 / 0x20000000`；Q6 SMEM `0xb000` |
| IPA WLAN | `0x4a1 / 0` | 与 Wi-Fi 自身 PCIe SID `0x1401` 不是同一对象 |
| IPA uC | `0x4a2 / 0` | IOVA 池 `0x20000000 / 0x20000000`，独立上下文 |
| IPA 11ad | `0x4a4 / 0` | 原厂 `iommu-group/shared-cb` 属性需要保留语义 |

provider 是 `apps-smmu@15000000`；AP 额外映射为 `0x14683000 → 0x14683000 / 0x2000`。
IOVA 数值相同不意味着可以跨上下文共享页表。所有 DMA 缓冲区、stream mask、CB/ASID、TLBI 和
缓存属性仍需由实际原生 provider 验证。GSI event ring 的编号、内存和完成生命周期不能从 channel 编号猜测。

下表省略统一前缀 `IPA_CLIENT_`。EE `0=AP`，`1=Q6`；GSI channel 只在对应 EE 内命名。

| Client | Client ID | EP | GSI channel | EE | Group | TLV / AOS |
| --- | --- | --- | --- | --- | --- | --- |
| USB_PROD | 18 | 1 | 0 | 0 | UL | 8 / 16 |
| APPS_WAN_PROD | 34 | 2 | 11 | 0 | UL | 25 / 32 |
| APPS_WAN_LOW_LAT_PROD | 104 | 4 | 9 | 0 | URLLC | 8 / 16 |
| WLAN2_PROD | 12 | 6 | 16 | 0 | UL | 8 / 16 |
| WIGIG_PROD | 86 | 7 | 17 | 0 | UL | 8 / 16 |
| APPS_LAN_PROD | 32 | 9 | 19 | 0 | UL | 26 / 32 |
| APPS_WAN_LOW_LAT_DATA_PROD | 120 | 10 | 5 | 0 | URLLC | 10 / 16 |
| APPS_CMD_PROD | 36 | 14 | 12 | 0 | UL | 20 / 24 |
| Q6_WAN_PROD | 52 | 12 | 0 | 1 | DL | 16 / 28 |
| Q6_CMD_PROD | 54 | 13 | 1 | 1 | UL | 20 / 24 |
| Q6_DL_NLO_DATA_PROD | 74 | 15 | 2 | 1 | DL | 28 / 32 |
| Q6_DL_NLO_LL_DATA_PROD | 122 | 5 | 8 | 1 | URLLC | 28 / 32 |
| APPS_LAN_COAL_CONS | 127 | 16 | 13 | 0 | UL | 8 / 23 |
| APPS_LAN_CONS | 33 | 17 | 14 | 0 | UL | 9 / 9 |
| APPS_WAN_COAL_CONS | 95 | 23 | 4 | 0 | DL | 8 / 23 |
| APPS_WAN_CONS | 35 | 24 | 1 | 0 | DL | 9 / 9 |
| USB_DPL_CONS | 27 | 25 | 20 | 0 | DL | 5 / 5 |
| ODL_DPL_CONS | 83 | 26 | 2 | 0 | DL | 5 / 5 |
| WIGIG1_CONS | 87 | 27 | 21 | 0 | DL | 8 / 14 |
| WLAN2_CONS | 13 | 28 | 3 | 0 | DL | 8 / 14 |
| WLAN2_CONS1 | 119 | 29 | 22 | 0 | DL | 8 / 14 |
| USB_CONS | 19 | 30 | 23 | 0 | DL | 9 / 9 |
| APPS_WAN_LOW_LAT_CONS | 105 | 32 | 8 | 0 | DL | 9 / 9 |
| APPS_WAN_LOW_LAT_DATA_CONS | 121 | 33 | 10 | 0 | DL | 9 / 9 |
| WIGIG2_CONS | 89 | 34 | 6 | 0 | DL | 8 / 14 |
| WIGIG3_CONS | 91 | 35 | 25 | 0 | DL | 8 / 14 |
| Q6_LAN_CONS | 51 | 18 | 3 | 1 | DL | 9 / 9 |
| Q6_QBAP_STATUS_CONS | 79 | 19 | 4 | 1 | UL | 9 / 9 |
| Q6_UL_NLO_DATA_CONS | 75 | 20 | 5 | 1 | UL | 5 / 5 |
| Q6_UL_NLO_ACK_CONS | 77 | 21 | 6 | 1 | UL | 5 / 5 |
| Q6_WAN_CONS | 53 | 22 | 7 | 1 | UL | 9 / 9 |

另有 10 个 `IPA_CLIENT_TEST*` 别名复用上述硬件端点，机器清单单独保存为 `test_aliases`，
它们不是额外可部署的生产端点。分组、过滤、sequencer、QMB、TX instance 和 prefetch 全字段见 JSON。

## 控制、数据、持久化的共同交接

启动顺序为资源 → 存储后端 → IPC_PREPARE → 固件 → IPC/服务发现。这样 reverse-file 请求可在
modem 开始执行前进入受控接收路径。建立数据承载时，IPA peer 握手、WDA 格式、WDS subscription/mux/endpoint
绑定、Start/Settings、DMA/GSI/SMMU、ifnet/IP/DNS/route 都属于同一 bearer 操作，逐阶段实际完成后才能 UP。

持久化远端请求保持原始 peer identity，WRITE → FLUSH → STORAGE_REPLY。重复请求必须保留逻辑请求
cookie，不能因为应答丢失而再次执行已持久化写入。RMTFS 与 RFSA 的 wire codec 仍是不同的待实现边界。

SSR/停止共同失效旧事务、slot/service generation 和 bearer。先持久化/媒体/DMA排空，再停止 IPC/固件、
回收远端内存归属并释放资源；任何部分授权或未知完成保留隔离状态。与音频、Wi-Fi、GPU 的并发资源占用
要放在同一次端到端场景中验证，不能以若干互不相干的“成功”代替整条链路。

当前已实现的全局 broker、运行核心和内核队列属于软件基础设施。MPSS 原生加载/通信、反向文件服务、
IPA/GSI 收发、蜂窝 ifnet 配置及完整 Apple telephony ABI 尚不能由这些接口的存在推导为已实现。
