# Ace 5 基带固件、认证和启动映射

记录日期：2026-10-23。目标是 giulia / SM8650 的 SIM 识别和移动数据。
本文件是源码与原厂配置映射；未执行基带上电、内存授权或固件启动。
总入口见 [MODEM_BRINGUP.md](MODEM_BRINGUP.md)，硬件资源见
[sm8650-modem.json](../hardware/sm8650-modem.json)。

## 证据与版本

| 输入 | 固定依据 | 用途 |
| --- | --- | --- |
| OnePlus 内核 | `d763ed47deec55451366c1187f813f2f309d88a7` | `qcom_q6v5_pas.c`、`qcom_q6v5.c/.h`、`mdt_loader.c` |
| 原机 DT | `../ace5-analysis/running-device.dts`，哈希见硬件 JSON | 当前板级内存、供电、互联、中断与 SMEM 路由 |
| 原厂 XBL 配置 DTB | `.cache/audio-native-audit/xbl-144d4.dtb`，SHA256 `7dfde297c2587dfb2b26e30b0ea93d22fc1fdc111007b0bf9a9a5b6223010bb4` | `PIL_MODEM` 的 UEFI 代理投票 |
| 原厂 `uefipil.cfg` | `.cache/stock-boot-16.0.3.500/modules/uefi-uefipil.cfg.raw`，SHA256 `a1303d4ff289ee8cbbc57a4f71e954ea670bb67826feea23c9c32fb9a0bafd52` | 固件名、PAS、UEFI 加载范围与 retail 名单 |
| 原厂完整 ROM | `PKG110domestic_11_16.0.3.500CN01_2026010902170644/IMAGES/NON-HLOS.bin`，SHA256 `1964e1f89b887b3c1d5e2bfef5583d50f61f903cb14106a648341963d5b072f0` | 实际 MDT、所有 split 段和 IPA/GSI ELF；离线完整性核验 |
| 原厂 vendor/odm | 已有 EROFS raw 输入，摘要见 stock JSON | 实际 `rmt_storage`、`tftp_server`、QMI IDL 服务库与反汇编 |
| 固定 UEFI 基线 | Mu-Silicium `6c6755012728c9f7de748228758bad94d1e9a660`；MemoryMapLib blob `f9023c8561c70d75dc128fa8af98211cc353dbae` | 原始提交已包含 giulia 板文件；38 条描述符及 MemoryInitPei/RamManagerDxe 源链固定 |
| 当前保留区 overlay | `uefi-overlay/Platforms/OnePlus/giuliaPkg/Library/MemoryMapLib/MemoryMapLib.c` | 全部原条目不变，只新增 QLINK、DSM0、DSM1；生产 prepare 路径验证复制与绑定 |

源码链接：[remoteproc/PAS](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/d763ed47deec55451366c1187f813f2f309d88a7/drivers/remoteproc/qcom_q6v5_pas.c)、
[Q6 信号处理](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/d763ed47deec55451366c1187f813f2f309d88a7/drivers/remoteproc/qcom_q6v5.c)、
[Q6 定义](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/d763ed47deec55451366c1187f813f2f309d88a7/drivers/remoteproc/qcom_q6v5.h)、
[MDT 加载器](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/d763ed47deec55451366c1187f813f2f309d88a7/drivers/soc/qcom/mdt_loader.c)。
原厂固件和可执行文件只保存在 `.cache/modem-stock`，没有提交其内容或 NV 数据。
可提交的 [stock 证据清单](../hardware/sm8650-modem-stock.json) 仅包含哈希、ELF/段表、
服务对象和机器指令事实。`python scripts/ios16_modem_stock.py --check` 可从缓存重建比对；
`--verify-images --package-root <ROM目录>` 还会复核完整镜像哈希。

## 固件身份映射

| 对象 | 原厂定义 | 当前能确认的边界 |
| --- | --- | --- |
| 平台匹配 | `qcom,pineapple-modem-pas` → `pineapple_mpss_resource` | 不是 ADSP，也不是 WCN7850 |
| 主固件 | 默认 `modem.mdt`，分段文件 `modem.bNN` | 已核验同包 ELF32 LE、47 个 PH 和全部非空加载段；entry/min=`0x8bc00000`，加载末端=`0x99e00000` |
| 固件变体 | 驱动允许 `firmware-name` 覆盖；特定项目另有 RF ID 选择 `modem2.mdt` 的分支 | 分支存在不证明 giulia 使用 `modem2`；不能按字符串猜选包 |
| DTB 固件 | 同包 `modem_dtb.mdt` + 相应分段 | 已核验 ELF32 LE、3 个 PH；加载范围 `[0x9b000000,0x9b003d3c)` |
| 主 PAS ID | `4 / 0x04` | 主固件的安全认证身份 |
| DTB PAS ID | `38 / 0x26` | 与 ADSP DTB 的 `36 / 0x24` 不同 |
| SSR 名 | `mpss` | 子系统重启通知域 |
| sysmon / AOP 名 | `modem` | 生命周期控制与 AOP load-state 的名字 |
| SSCTL ID | `0x12` | 系统监控控制身份，不是 PAS ID 或 QRTR 服务 ID |
| crash reason | SMEM item `421` | 崩溃诊断数据，不是 ready 位 |
| minidump ID | `3` | dump 子系统号 |
| 自动启动 | `auto_boot=false` | 驱动探测成功不意味着基带运行 |
| 认证元数据 | `dma_phys_below_32b=true` | 使用 `memory-region[2]` 的低 4 GiB CMA；不能照搬高地址普通 XNU 内存 |
| DSM 授权 | `ssr_hyp_assign_mem=true`，实际调用还检查 DTB 名为 `modem_dtb.mdt` | 三个 `mpss_dsm_mem_reg` 区域参与 HLOS ↔ MSS_MSA RW 授权 |

**ELF 格式需要区分：** `uses_elf64=true` 在 `adsp_probe()` 中用于
`rproc_coredump_set_elf_info(..., ELFCLASS64, EM_NONE)`，描述的是崩溃转储。
同一提交的 `mdt_loader.c` 实际读取 `elf32_hdr/elf32_phdr`。
实际原厂 `modem.mdt`、`modem_dtb.mdt`、`ipa_fws.mdt` 和 `fw_ipa_gsi_5.5_h.elf`
均已确认为 ELF32 little-endian。主 modem 的缺失 split 编号 `22/29/42/45` 对应 `filesz=0`
的 BSS，不是缺文件；其余所有加载段的文件长度均与 PH 匹配。三个 MDT 都等于各自 `b00`
头部加 hash/签名元数据段。IPA split 各段与完整 GSI ELF 对应段的 SHA256 逐项相同，
其可重定位加载范围为 `[0,0x8568)`。这些是结构与输入一致性验证，没有执行安全世界认证。

## 内存对象和所有权

以下使用半开区间 `[base, end)`，物理地址与大小均来自原机 DT。

| 对象 | base | size | end | 消费者 / 权限变化 |
| --- | --- | --- | --- | --- |
| SMEM | `0x81d00000` | `0x00200000` | `0x81f00000` | AP 与多个远端共同使用；基带只取 AP/MPSS 对应分区 |
| qlink logging | `0x84800000` | `0x00200000` | `0x84a00000` | `mpss_dsm_mem_reg[2]`；需确认转交前所有权 |
| DSM 0 | `0x86b00000` | `0x04900000` | `0x8b400000` | `mpss_dsm_mem_reg[0]`；HLOS → MSS_MSA，RW |
| DSM 1 | `0x8b400000` | `0x00800000` | `0x8bc00000` | `mpss_dsm_mem_reg[1]`；HLOS → MSS_MSA，RW |
| MPSS 固件 | `0x8bc00000` | `0x0f400000` | `0x9b000000` | `memory-region[0]`；PAS 4 |
| MPSS DTB | `0x9b000000` | `0x00080000` | `0x9b080000` | `memory-region[1]`；PAS 38 |
| IPA firmware | `0x9b080000` | `0x00010000` | `0x9b090000` | IPA 独立固件资源，不能并入 MPSS 镜像 |
| IPA GSI | `0x9b090000` | `0x0000a000` | `0x9b09a000` | IPA/GSI 独立保留区 |
| PAS metadata CMA | 动态分配 | 见硬件 JSON | 必须低于 4 GiB | `memory-region[2]`；不是固定可写物理地址 |

`mpss_dsm_hyp_assign_control()` 在启动方向把每个区域从 HLOS 授权给
`QCOM_SCM_VMID_MSS_MSA`，关闭方向归还 HLOS。VMID 的枚举、SMC 参数和
当前所有权必须由对应实现解析，不能把 AP 的 SMEM host ID 当成安全世界 VMID。
内存交给基带以后，AP 不得继续作为普通 RAM 分配使用。
原厂实现逐区授权；中途失败时不会回滚此前成功的区域。因此“启动失败”不等于
“全部 DSM 仍由 AP 持有”，恢复时必须逐区确认，不能直接释放整组内存。

UEFI 基线的 `PIL_Reserved` 是 `[0x8bc00000, 0xa3180000)`，覆盖上表的
MPSS/DTB/IPA 静态区，但不覆盖 qlink logging 和两个 DSM 区。
继续追到实际构建路径后，不能把“无条目”解释为“默认已排除”：

- `MemoryMapHelper` 仅按名称或精确起址查询；不执行保留操作。
- `MemoryInitPei` 只为显式描述符建立 HOB。
- `giulia.fdf` 明确加载 `RamManagerDxe`。后者读取当时 EFI map，再从 EFI_RAMPARTITION
  取得 `Base/AvailableLength`，将每段内没有 EFI 描述符覆盖的空隙作为 `EfiConventionalMemory` 加入。
- 因此，若 firmware RAM partition 覆盖 DSM/QLINK 且更早固件没有另建排除描述符，
  这些区会沿该路径被加入普通 RAM。当前未获得该 provider 实际返回集合及最终 EFI map，
  不能宣称已发生覆写，也不能宣称安全排除。下一步应继续分析 `EnvDxeEnhanced` provider、
  最终 EFI/ADT RAM 集合和 XNU 分配器排除范围，并在交接前建立明确保留证据。

这项基线缺口现在已有实际源码修复：板级 overlay 新增 `QLINK_Logging`、`MPSS_DSM0`、
`MPSS_DSM1`，分别覆盖 `0x84800000/0x200000`、`0x86b00000/0x4900000`、
`0x8b400000/0x800000`。三项均采用 `AddMem / MEM_RES / UNCACHEABLE / Reserv /
UNCACHED_UNBUFFERED_XN`，在 PEI 阶段建立显式保留描述符，先于动态 RAM 管理。
其余 38 项和外围源文本保持原样；新表共 41 项，无物理重叠，覆盖 stock modem/DTB/IPA
加载段及完整 DSM/QLINK 范围。

`prepare.apply_uefi_overlay()` 是实际构建使用的复制入口：复制前仅接受固定的、已应用板级
补丁后的基线内容，或完全一致的既有 overlay；其它漂移直接失败。复制后验证本板 DSC 的
MemoryMapLib 绑定、INF 的 C 源文件和 RamManagerDxe 的 FDF 接入。固定上游提交本来就有
该板文件，不能把浅克隆中不可解析的 ref 误认为上游缺文件。这里完成的是构建源码保留修复，
不是 SCM 授权、已运行固件所有权或已观测最终 EFI map 的证明。

## 原厂 UEFI 与运行 DT 的明确冲突

| 来源 | MODEM base | size | end |
| --- | --- | --- | --- |
| 原机运行 DT | `0x8bc00000` | `0x0f400000` | `0x9b000000` |
| 原厂 `uefipil.cfg` 的 `[FULL_MODEM]` | `0x8a200000` | `0x10e00000` | `0x9b000000` |

两者末端相同，起址相差 `0x01a00000`（26 MiB）。旧起址落在运行 DT 的 DSM 区。
`FULL_MODEM` 还指定 `OverrideElfAddr=Yes`，因此不能直接使用该条目去加载当前固件。
`FULL_MODEM_DTB` 的地址 `0x9b000000/0x80000` 则与运行 DT 一致。

该配置的 `[RETAIL]` 只有 `ImageFv`、`FULL_ADSP` 和 `FULL_ADSP_DTB`，没有
`FULL_MODEM`。存在 MODEM 段、存在 `PIL_MODEM` 代理，均不是 UEFI 已加载基带的证据。
本轮实际 ROM 已解决当前固件的地址选择：其 entry 和加载最小物理地址都是 `0x8bc00000`，
完整加载范围落在运行 DT 的 MPSS 保留区内；主镜像具有可重定位标志。
重现当前 Android DT 布局应选择 `0x8bc00000`，不能把旧 `OverrideElfAddr=Yes` 的
`0x8a200000` 直接套用，否则会把加载起址移入当前 DSM 区。PIL 接口、secure 认证和运行态
所有权仍需要独立实现/确认，不能由离线 ELF 核验替代。

## 电源和带宽映射

| 层 | 对象 | 原厂值 | 转换要求 |
| --- | --- | --- | --- |
| Android DT | XO | `clocks=<0x2d 0>`，名字 `xo` | phandle 只在这份 DT 内有意义；解析实际时钟 provider |
| Android DT | CX | `cx-uV-uA=<0x180 0x186a0>`，provider `cx.lvl` | 384 / 100000 是该 regulator API 的输入；逻辑性能角不能当成直接 MMIO 值 |
| Android DT | 名为 MX 的 supply | `mx-uV-uA=<0x140 0x186a0>`，实际 provider `mss.lvl` | 320 / 100000；名称是 `mx-supply` 不表示它指向 `mx.lvl`；与 ADSP 的 LMX/LCX 资源不同 |
| Android DT | `rproc_ddr` | `<0x41 4> → <0x41 0x200>` | 经对应 ICC provider 解析与聚合 |
| Android DT | `crypto_ddr` | `<0x40 0x29> → <0x41 0x200>` | PAS 操作期间的独立带宽请求 |
| 原厂驱动 | ICC average / peak | 启用时 `UINT_MAX / UINT_MAX` | 是 ICC API 单位，不是原生 BCM 寄存器编码 |
| Android DT | AOP | `qcom,signal-aop`，`qcom,qmp=<0x3f>` | 向 `modem` load-state 请求资源；需要成功应答 |
| 原厂 XBL DT | `PIL_MODEM` / subsys 4 | `/vcs/vdd_mx`、`/vcs/vdd_cx`、`/vcs/vdd_mxc` 各 `0x100` | UEFI NPA 资源命名/投票；不能代替 Android 的 320/384 与持久化运行所有权 |

本次检查的 `PIL_MODEM` 节点没有 ADSP 节点中的 ICB 路由/带宽字段。
不能把 ADSP 的 `/icb/arbiter` 路由 ID 或 `100000000` 请求复制给 modem。
UEFI proxy 存在也不能证明 ExitBootServices 后资源继续有效。

## 启动与停止时序

下面是固定版本原厂调用关系的映射，尚不是本仓库已连接的执行器。

1. `adsp_probe()` 依据 compatible 选 MPSS 描述符，解析资源、低地址 DMA 池、
   中断、SMEM stop 信号及 GLINK/sysmon/SSR 子设备。`auto_boot=false`。
2. `adsp_load()` 申请 `modem_dtb.mdt`，在 crypto 带宽票有效时调用
   `qcom_mdt_load_no_free()`。元数据与镜像内存必须保持有效。
3. MDT 加载器验证加载段范围，准备认证元数据，`PAS_INIT_IMAGE`；
   只有可重定位镜像才调用 `PAS_MEM_SETUP`。加载有效 `PT_LOAD`，跳过 hash 段，
   按 `.bNN` 读取拆分文件并校验长度，清零 `p_memsz-p_filesz`。
4. `adsp_start()` 重置 ready/stop 完成状态、准备 handover 中断，按适用条件
   转交 DSM，持有 rproc 带宽和可用 power domains，设置 AOP load-state，
   开 XO/可选 aggre2、启用该节点的 regulators。
5. 在 PAS 带宽票有效时先 `AUTH_AND_RESET(38)`，随后读取并加载主固件，
   最后 `AUTH_AND_RESET(4)`。主/DTB PAS 不能交换。
6. 如有独立 RMB 资源，则处理 boot-wait/boot-continue；随后等待新的 ready 信号，
   标准超时为 5000 ms。此次启动的 ready 不得由前次残留状态冒充。
7. GLINK/IPCRTR 建立后才能观察 QRTR 服务，完成 SIM/QMI 及数据通道初始化。
   PAS 返回成功或 ready 位出现都不等于 SIM 已可用。
8. 停止先停业务与数据通道，使用 sysmon 或 SMEM stop 位等待 stop-ack，
   然后 `PAS_SHUTDOWN(4)`、`PAS_SHUTDOWN(38)`，撤销 AOP 状态、释放资源，
   最后按原厂规则回收 DSM。错误/超时不证明 DMA 和基带已停止。

PAS 函数号 `INIT=1 / MEM_SETUP=2 / AUTH_AND_RESET=5 / SHUTDOWN=6`
与 PAS 对象 ID `4 / 38` 是不同字段。现有
`giulia_audio_scm.h` 固定 ADSP 对象 `1 / 36`，不能直接作为 modem adapter。
通用 SCM 重试/返回值处理可以参考 `giulia_scm.h`，但它并不提供基带内存所有权。

## RMB 不能从名字或邻接地址推断

`qcom_q6v5.h` 定义的可选 RMB 偏移是 WAIT `+0x08`、CONT `+0x0c`、STATUS
`+0x10`，轮询上限 250 次。`qcom_q6v5_init()` 却从 **MEM resource index 1**
取 `rmb_base`。当前运行 DT 的 `remoteproc-mss@04080000` 只有一个 reg window。
按这份 DT 和这份驱动，RMB 是缺席的；不能擅自把这些偏移加到 `0x04080000` 后访问。

同一节点的 `reg-names="cx","mx"` 被 `adsp_init_regulator()` 用来枚举电源，
不能因为有两个名字就推导出两个 MMIO window。

## 已确认输入与剩余运行证据

| 输入或事件 | 当前证据 | 获取后需判定 |
| --- | --- | --- |
| MPSS/DTB 与 IPA 固件 | 同包 55 个文件已缓存并逐文件哈希；ELF32、完整加载段、BSS、MDT 元数据拼接和 IPA split/ELF 对应关系已验证 | 后续是 native loader、secure 认证与新 ready；不再将“缺 blob/ELF 待确认”列为输入阻塞 |
| modem NV/EFS/运营商配置 | 存储分区角色可识别；未读取用户校准/订阅内容 | 原机 RMTFS/存储服务实际协议与分区布局；不能新建空校准数据代替 |
| MPSS 当前运行状态 | 本轮无 ADB 设备，无新 ready/SSR 日志 | 冷启动/附着路线、资源拥有者、是否已有远端运行 |
| UEFI → XNU 交接 | 有内存描述符与其他子系统框架 | 保留区域完整性、所有权、缓存属性、中断与电源持续性 |
| 原厂服务对象 | `rmt_storage` 的 qmi_csi_register 实参指向 service14、5请求/5响应/1indication；QMI 库有40个可解析服务对象；`tftp_server` 发布 QRTR4096 的原始 selector1..10 | 二进制声明/注册代码不是本次运行枚举；实际 node/port 与订阅映射仍须动态发现 |
| IPA/GSI endpoint 与 mux | DT/协议字段可映射 | 当前固件协商结果、端点表、SMMU stream、真实收发与停止确认 |

这些缺项在获得输入前保持“未验证”。本文件不以源码路径、状态常量或离线测试
替代 MPSS 启动、SIM READY、网络注册和真实 IP 收发证据。
