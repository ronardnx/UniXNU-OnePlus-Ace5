# 一加 Kiwi 固件启动链审计

审计日期：2026-10-24。目标是 Ace 5 / PKG110 / WCN7850，范围从平台探测到
`FW_INIT_DONE` 后交给 WLAN 驱动，并检查失败释放和重新启动条件。
这是源代码对应记录，不代表所有环节已经通过实机验证。

## 固定来源与适用条件

- CNSS2：一加 `android_kernel_modules_and_devicetree_oneplus_sm8650`，固定提交
  `453d8875f815bf9dfad9035982861226a4823a6c`，
  [main.c](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/wlan/platform/cnss2/main.c)、
  [power.c](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/wlan/platform/cnss2/power.c)、
  [pci.c](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/wlan/platform/cnss2/pci.c)、
  [qmi.c](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/blob/453d8875f815bf9dfad9035982861226a4823a6c/vendor/qcom/opensource/wlan/platform/cnss2/qmi.c)、`qmi.h`、`Kconfig`，以及 `cnss_utils/wlan_firmware_service_v01.{c,h}`。
- 内核总线：一加 Ace 5 分支已固定的 `66305c56af1fcd7299c906c3257e585be93d93ed`，
  [MHI boot.c](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/66305c56af1fcd7299c906c3257e585be93d93ed/drivers/bus/mhi/host/boot.c)、
  [MHI pm.c](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/66305c56af1fcd7299c906c3257e585be93d93ed/drivers/bus/mhi/host/pm.c)、
  [MHI main.c](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/66305c56af1fcd7299c906c3257e585be93d93ed/drivers/bus/mhi/host/main.c)、`internal.h`、`init.c`、`include/linux/mhi.h`。
  PCIe RC 的同版来源及与 ROM 模块的有限对照见 `hardware/sm8650-wifi-stock-init.json`。
- 实机设备树：`/soc/qcom,cnss-kiwi@b0000000`，DTS SHA-256
  `a23d858906208fcd7f9567aa6d6d7847e9e03fd5da448dd8e3b9a298a81de0a1`，
  身份记录见 `hardware/sm8650-wifi.json`。只引用板级字段，不复制设备唯一信息。
- 该节点有 `wlan-en-gpio=16`、`qcom,qmp`、PDC 默认表、`use-pm-domain`、
  `qcom,wlan-cbc-enabled`；没有 `qcom,sleep-clk-support`、`qcom,xo-clk-gpio`、
  `qcom,set-wlaon-pwr-ctrl`、`qcom,on-chip-pmic-support`。
- Kconfig 条件不能仅由节点推定：`CNSS2_DEBUG` 会影响 host build type 和超时，
  `CONFIG_PULLDOWN_WLANEN` 会影响复位脉冲。没有在本轮恢复整个原厂最终 `.config`，
  不将这些编译条件擅自当作已启用或已移植。

## 上游步骤与本地入口

表中的“接入”表示存在实际调用；硬件结果以最后一列和原始日志为准。

| 阶段／上游函数 | 本地实现与接入 | 边界、差异和剩余工作 |
| --- | --- | --- |
| 平台匹配：`cnss_probe`、`cnss_get_dev_cfg_node` | 固定 Ace 5 板级配置；`gw_handoff`、`gwl_handoff` 检查 UEFI v5 交接，PCI ID 再核对 `17cb:1107` | 不支持自动发现其他板型；不把 PCI ready 当固件 ready |
| 资源与 AOP：`cnss_power_misc_params_init`、`cnss_aop_interface_init`、`cnss_aop_pdc_reconfig` | `gq_power_defaults` → `qpp_defaults` → `gq_apply_power`，10 条默认值，逐条等消费 ACK 和 close | 本轮保留上一轮修复；当前在 XNU MHI 前执行，晚于 UEFI 的物理上电，尚未复原完整原厂上电时序 |
| 稳压器／时钟：`cnss_get_resources`、`cnss_power_on_device` | UEFI `WifiPcie.c` 的 `PrepareResources`／`Resource`，板级 RPMh 电压、使能、时钟／电源域；XNU 接管已准备资源 | XNU 不拥有完整供电生命周期；不能用 `wifi.start` 重做冷上电 |
| GPIO/pinctrl：`cnss_get_pinctrl`、`cnss_select_pinctrl_enable` | UEFI PCIe 策略配置 WLAN_EN、PERST、SW_CTRL 路由；`gwl_train` 再核对状态 | 原厂条件性的 BT_EN 协调、失败后多次断电重试未完整接入 |
| RC 互连／PHY／复位：PCI-MSM enable link | `WifiPciePolicy.h`、CESTA／bandwidth／stock 配置、`gwl_train` 的 PERST/LTSSM/CFG0 | 既有实机已进入 PCI 枚举；运行时带宽缩放／DRV handoff 尚未实现 |
| PCI 探测：`cnss_pci_probe`、`cnss_pci_enable_bus` | `gwp_enumerate`、BAR 窗口、受检读写及原值恢复 | 枚举不是 DMA 授权；仅隔离映射完成后开启 BME |
| IOMMU：`cnss_pci_init_smmu` | `gwp_sid_claim` → `gws_take`，私有页表与 64 MiB arena；HOST_CAP 使用实际 IOVA | 不沿用 Android 的大 IOVA/IPA 区间，错误不以零地址应答 |
| MSI：`cnss_pci_enable_msi`、`cnss_pci_get_mhi_msi` | `gwp_msi_sink` 把 MSI 写限制在驱动 arena；MHI/CE 由 worker 轮询 | 无完整 GIC 中断和 WAKE MSI 路径；常醒 bring-up 适配，不是原厂运行时 PM |
| MHI 注册：`cnss_pci_register_mhi` | `gwm_configure` 建立控制／数据事件环与 IPCR 20/21、命令环、地址范围 | 512 KiB SBL 和 BHIE 分段与 Kiwi 上游一致；不创建 DIAG、SATELLITE 等未实现客户端 |
| 固件选择：`cnss_pci_update_fw_name`、`cnss_pci_load_m3` | `gwf_main`／`gwf_phy` 由实际 major 选择 amss/phy_ucode 或 amss20/phy_ucode20；固定 pack SHA | 不下载或替换厂商签名数据；缺少变体即失败 |
| BHI → READY/M0 → BHIE → AMSS | `gwm_boot`：镜像校验、DMA 同步、BME、BHI 状态、READY/M0、BHIE 完成、EE=2 | 历史实机通过；MHI 错误执行 bus-master 关闭／排空，无法确认则隔离内存 |
| 启动 IPCR：MHI channel prepare/start | `gwm_start_channels` 等待命令完成，发布有界 RX 缓冲；`gm_receive` → `gwq_receive` | QMI 回调不递归轮询 MHI；描述符和 DMA 的归还必须先有设备证据 |
| QRTR/WLFW discovery：`cnss_wlfw_connect_to_server` | `gwq_start`／`gwq_receive`，service 0x45 instance 1，校验节点／端口／目的客户端，确认流控 | 发现／RPC 各 10 秒，mission 总预算 40 秒；保留路由过滤诊断 |
| 注册：`cnss_wlfw_ind_register_send_sync` | `IND_REGISTER` 注册 REQUEST_MEM、MEM_READY、INIT_DONE、PIN_CONNECT_RESULT | 新增检查响应 `fw_status`：`ALREADY_REGISTERED` 时在 HOST_CAP 前退出；不接管旧客户端 DMA |
| HOST_CAP：`cnss_wlfw_host_cap_send_sync` | BDF/M3 支持、`cal_done=0`、`mem_cfg_mode=0`、features=0xc、实际 SMMU IOVA 范围 | 未声明 WAKE MSI、DRV、MLO、FW INI 配置；这些完整能力不能靠增加 bit 实现。host build type 尚保留未指定，不冒充 Android 编译配置 |
| 请求／响应内存：`cnss_wlfw_request_mem_ind_cb`、`cnss_pci_alloc_fw_mem`、`cnss_wlfw_respond_mem_send_sync` | `gwq_memory` 验证所有段，实际分配／清零／同步，再发 RESPOND_MEM | 支持已定义非安全段；安全 carveout 拒绝；部分失败保留已分配 backing 直到总线停止 |
| FW_MEM_READY → CAP：`cnss_fw_mem_ready_hdlr`、`cnss_wlfw_tgt_cap_send_sync` | `gwq_provision`：读取 chip/board/SOC/CMEM/PMU；缺省 board=0xff，与原厂一致 | CAP 身份不符、元数据损坏时不继续资源下发 |
| CAP 电压：`cnss_aop_ol_cpr_cfg_setup` | `gq_power` → `qpp_make`，共享电源轨取最大值、应用原厂舍入／margin，再逐条消费 | 真实 CAP 9 个 pin 合并成 7 条命令；不覆盖没有 CAP 值的三个 retention 默认值 |
| REGDB/BDF：`cnss_wlfw_bdf_dnld_send_sync` | `gwq_download`，REGDB=4、ELF BDF=1；remaining size、seg_id、end；CN 项目选择 | 两者都必须真实 RPC 成功；不是只找到文件就认为已加载 |
| M3：`cnss_wlfw_m3_dnld_send_sync` | 实际分配／复制 phy_ucode，发送 `0x3c` IOVA/size | M3 的 RPC 成功只表示 provisioning，不能设置 `fw_ready` |
| QDSS 配置 | HOST_CAP 声明 `CNSS_QDSS_CFG_MISS`；不提供不存在的 trace 配置或 buffer | 这是现有无 QDSS 的明确适配；不声称实现原厂 QDSS 下载／dump |
| `FW_INIT_DONE (0x38)` → driver probe | `gwq_initialize` 同时要求 `provisioned` 和真实 indication；随后 CE、WLAN_CFG/MODE、WMI | 原厂也是 ready 后再 probe；不前移 WLAN_MODE，也不接受 legacy 0x21 代替 ready |
| 冷启动校准：`CNSS_IN_COLD_BOOT_CAL`、CAL_REPORT、CALIBRATION mode | 当前只实现 mission，`cal_done` 不伪造为 1 | **缺少完整 CBC、校准文件装载、完成通知和重启流程**；该分支在 FW_INIT_DONE 后，不解释此前未收到 ready |
| 异常：timeout、server exit、SYS_ERR/RDDM | 有界超时／错误、诊断快照、停止 DMA、隔离不确定资源 | **缺少原厂 RDDM vector/dump 与 SSR/断电重试闭环**；不自动重试已停止实例 |
| 退出与释放：MHI poweroff → bus disable → SMMU teardown | `gw_shutdown` 关闭数据端口、停 CE；`gwm_stop` 等 RESET 清零、重写 BHI_INTVEC、读 EE，非 PBL 类等待 READY，再关闭并排空 BME；确认后停 DP 并恢复 MSI/SID/SMMU/PCI；保留不确定 QMP | reset/READY/PCI 停止任一步失败都隔离；UEFI 供电票据继续保留，XNU 无完整原厂 power-off。释放 host backing 不等于关闭所有硬件电源 |
| suspend/resume：CNSS PM domain、MHI M3、PCI runtime PM | 当前持续轮询／保持活动状态 | **缺少完整休眠恢复、wake IRQ、恢复次序**；不把软件接口 DOWN 当硬件 suspend |

## 条件分支核对

`cnss_is_aux_support_enabled` 仅在 Peach 且固件声明支持时走 AUX；TME 的加载也受
对应芯片条件限制。Kiwi 不应照搬 Peach 的额外镜像。
HDS 受 `hds_enabled` 控制，审计到的默认值未开启；不存在“默认一定要下发 HDS”的依据。
QCA6490 内部功放电源、CPR 分支和设备树控制的 WLAON QFPROM 修改也不能无条件套到
本板。`qcom,set-wlaon-pwr-ctrl` 在捕获节点不存在，故本轮不增加该寄存器写入。
ADSP power-collapse 协调属于上游跨子系统依赖；本轮未确认完整构建条件及原生提供方，
将其保留为平台协同待核项，不以空函数宣称接入。

`HOST_CAP` 中 MLO 拓扑、host build type、link-speed 能力和 WAKE MSI 的差异已记录。
不能为了逐字段一致而宣称未实现的 MLO／唤醒能力；也不能仅凭这些差异认定 firmware
超时根因。需要先取得新镜像的完整事务与状态记录，再决定适用分支。

## 本轮修复与观测

本轮新增：

1. 检查 IND_REGISTER 响应的可选 8 字节 `fw_status`。畸形长度报 wire error；
   bit 0 表示已有注册时返回 `GWQ_ALREADY_REGISTERED`，不发送 HOST_CAP、不分配新固件内存。
   其他 ready 位只保留为诊断，不能代替本实例收到的真实通知。
2. 与原厂 HST/HSP 注册一致，使用 `FW_INIT_DONE`，不再主动订阅 legacy FW_READY；
   补上 PIN_CONNECT_RESULT 订阅和三个可选 u32 结果。先验证整个通知再更新状态，
   引脚位图不转换成虚假的“硬件通过”。
3. QMI 启动子阶段分别标记 discovery、register、host-cap、memory、cap、pmu、
   regdb、bdf、m3、wait-fw-init-done、ready 和 WLAN configure/mode，失败停留在实际步骤。
4. `wifi.status` 返回不含运行时指针的原子一致快照。查询不执行 MMIO、不抢设备锁；
   worker 退出后仍保留最终状态，可区别“未启动”“正在等固件”“失败已停止”。

前一轮三项修复保留：PDC 默认表、WLAN_EN feature bit、SMMU HOST_CAP 地址范围。
源码、平台资源、USB 命令桥、模块装配及本地固件打包入口已共同核对。
`ios16_wifi_patch.py` 按模块通配打包新增状态头；状态输出不包含固件、MAC 或密钥。

## 下一次终端采集

新内核需包含 Wi-Fi，固件 pack/ADT/平台交接须配套；调试包开启 `giulia_debug=1`
与 `giulia_wifi_wait=1`，先接 USB 再执行：

```text
wifi.status
wifi.start
watch wifi.status 2
```

期望依次观察默认命令消费 10、MHI EE=2、HOST_CAP features=0xc、内存 ready、
CAP 覆盖消费 7（仅针对已捕获的那份 CAP）、REGDB/BDF/M3，然后真实 `fw_ready=1`。
若仍超时，应同时保存 `qmi_step`、registration status、pin_valid/results、最后
RPC/indication、QMP pending、SMMU/AER 与原始启动日志。单独的 `M3-info` 或
`commands consumed` 不能作为固件完成证据。

上一轮主机枚举为 **no ports found**，没有执行启动采集。2026-10-24 13:06，
主控已在唯一串口会话取得 `waiting=1 active=1 stage=3 error=0 quarantine=0` 的
启动前快照；其时 MHI/QMI 尚未运行。这只确认等待门，未确认固件完成启动。
依工作区规定，待终端验证参数、时序与失败处理成功后，再推进本轮流程的 UEFI 集成；
本轮没有改写 UEFI 上电顺序、刷写分区或伪造实机完成记录。

## 本轮构建与产物核验

- 使用工具要求的参考提交 `d68e05df683f863f054b0f9c063c54d2b1342b2a` 生成
  GUI+touch+Wi-Fi，以及 GUI+touch+Wi-Fi+GPU+audio+modem 两种 profile。
  工作区根部 `reference-vermeer` 已不在该提交，因此使用已有固定版本缓存，未重置参考仓库。
- 两种产物的 Wi-Fi 补丁相同；新增状态头、QMI 头和运行时源码均与当前源码经正式
  `kernel_source` 转换后的内容一致。平台与 Wi-Fi 翻译单元、IO80211 personality
  均只注册一次；相关本地 include 均能在生成目录解析，USB 主机命令也已复制进 profile。
- 用生成头文件，将 QMI 初始化／接收／配置、PDC 默认表和提取的真实
  `gw_publish_status`／`giulia_wifi_terminal` 编译为 AArch64 C++14 freestanding
  对象，使用 `-O2 -mgeneral-regs-only -Wall -Wextra -Werror`。编译通过；
  对象没有额外 `libatomic` 依赖。终端／快照编译只提供内核声明与最小类型适配，
  **不等同于完整 `GiuliaWiFi.cpp` 的 XNU 编译或链接**。
- `check-driver-modules.py` 通过（161 个源码、13 个模块），修改范围的
  `git diff --check` 通过。本轮依更新要求未运行主机测试套件。
- 本地产物位于 `.cache/wifi-chain-review-20261024/`，包括两个 profile、
  生成源码、`boot-compile.o` 和 `artifact-report.json`。未构建可启动内核／镜像、
  未触发远程 CI，`FW_INIT_DONE` 和引脚报告仍需新内核实机验证。

## 2026-10-24 热加载边界续审

本轮 owned 源码修复尚待主控统一构建与实采，不更改上述历史验证结果：

- `FW_INIT_DONE` 在置 ready 前校验固定 WLANFW schema 的可选字段：
  `0x10=u32`、`0x11=u16`、`0x12=u64`。合法空通知仍可接受；畸形已知字段不能
  产生 `fw_ready` 或成功时间。内存请求的 count/bytes/failure index/type 在错误
  返回前输出，便于保留分配失败证据。这些是边界修复，没有证明其为旧超时根因。
- `mhi_pm_disable_transition` → `gwm_stop`：RESET 清零、BHI_INTVEC 清零读回、
  EE 有效性和按需 READY 等待。`mhi_poll_reg_field` 的 `25000` 是 25 ms 轮询间隔，
  每一等待沿用 CNSS 的 10000 ms 预算；PBL/PTHRU/EDL（EE 0/5/6）不等待 READY。
  当前没有 fatal-link PM 状态或 `rddm_image`，不能冒用上游 skip-reset 条件。
  失败仍尝试 BME 关闭／排空，但保留首个失败阶段与 quarantine；再次调用不将
  已失败的停止变为成功。该改动也不提供完整断电／复位后重新启动能力。
- `wifi.start` 仅以 CAS 释放已发布的 pre-DMA WAITING 门一次。提前、重复、
  停止后的请求会明确拒绝，不保存一个可在稍后实例触发的全局 go 标志。
  `wifi.status` 新增 MHI bus-master/running、CE/DP/SMMU 所有权及停止证据，
  所有字段仍来自 worker 发布的原子快照，终端不会读取运行时指针或设备寄存器。

新增停止阶段为 `0=none, 1=reset, 2=wait-reset, 3=intvec, 4=execenv,
5=wait-ready, 6=master, 7=done`；`mhi_error` 是首个失败阶段。
`phase=7` 只说明该停止序列已走到尾端，需同时核对 error=0、quarantine=0、
reset_complete、按 EE 要求的 ready_complete 以及 master_stopped。
从未启用 DMA 时这些完成字段为零；不能因此判断停止失败，也不能反过来将
`fw_ready=0` 解释为没有 DMA。waiting gate 为 `0=closed, 1=waiting, 2=released,
3=cancelled`，没有 reload/reset/unload 语义。

当前实机旧镜像的 NativeHotload 平台 provider 未接入 Wi-Fi。硬件热替换仍需要受信模块身份、
owner epoch、typed resource handles、常驻回调与完整停止／回滚契约；不通过
普通 `NLH_CALL` 标量传内核指针。完成终端实机链路后才推进 UEFI 集成。

## 真实协议算法热替换接入（源码，待统一验证）

`giulia_wifi_protocol.h` 提供完整常驻回退和无指针 TLV shape parser，
`giulia_wifi_protocol_module.c` 是独立 NLH2 service 源码，名称
`giulia-wifi-proto`、interface ABI `0x47480001`、state ABI 1、GH domain 2。
公共调用契约为 `common/giulia_hotload.h`；构建未启用 `GIULIA_HOTLOAD_ENABLED`
时使用同一完整常驻解析，不产生公共桥未解析符号。

真实入口为 `gm_receive → gwq_receive` 的响应／通知协议验证，以及
`gc_receive → gww_receive → gww_wmi_receive` 的已知 core WMI 事件。
两者通过 `gw_protocol_select → giulia_wifi_hotload_decide → giulia_hotload_select`
读取有界请求并采用经验证的回复。QMI 的来源、transaction、完整 wire framing、
mission 截止时间、ready/pin 状态与实际收发保持常驻；WMI 内部字段、扫描／peer／
密钥和监管状态也保持常驻。未知 WMI 事件不新增通用 TLV 限制，重复数组仍合法。

32 个 u64 邮箱包含 3 项 metadata 和最多 29 项 `{tag,length,offset,header_bytes}`
结构，完全不复制字段值、MAC/SSID/IE、序列号、密钥或设备地址。超容量走完整
常驻回退。模块返回 status/count/failure-offset/presence 标量；常驻只在同 epoch、
未停止且回复与完整 baseline 一致时采用。因此当前支持在既有协议验收规则内替换
解析实现，不能宣称任意改变协议判定或整体驱动重载。epoch 用饱和 CAS 发号，
耗尽后永久拒绝，模块升级不改变硬件 epoch。

模块私有状态 128 字节，START 执行六组真实解析自检后打开 admission，STOP
关闭私有入口并检查在途调用为零，EXPORT/IMPORT 仅迁移固定 80 字节计数格式。
没有设备资源或异步事件迁出常驻层；模块 STOP=0 只证明该纯解析模块已停止。
故障 variant 为 `GWPR_FAIL_START/IMPORT/STOP=1`，由统一自动化构建，不改变硬件参数。

`wifi.status` 的 `hotload` 行分别记录 epoch、真实接收调用 calls、已采用的 native、
fallback、rejected、mismatch、oversize 和 QMI/WMI 调用数。模块标量 `hl.call`
参数 0..6 依次只读 calls/accepts/rejects/imports/starts/stops/bridge_errors，
不读取当前请求或触发硬件；普通 scalar 无权冒充 resident ticket。
模块 calls 或公共 dispatches 增长不能单独替代驱动 native adoption 证据。

需要新常驻内核统一编入桥、模块入口与上述修复，再由唯一设备会话装载、升级、
回滚、卸载及验证真实调用。运行期覆盖依赖真实扫描的 `0x3001/0x7001`，
普通 HTT 数据包不证明 WMI 切口被调用。若 QMP/MHI 在 QMI 前失败，协议 fixture
通过也不能算真实协议调用通过；FW_INIT_DONE 与联网验收仍独立记账。
