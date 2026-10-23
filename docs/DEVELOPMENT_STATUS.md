# UniXNU giulia — OnePlus Ace 5 UEFI bring-up

当前实机调试使用 Darwin 22.6.0 / iOS 16 内核。已验证 UEFI 交接、XNU
内存管理初始化、平台驱动注册、CPU0 注册及 SGI1 自测、原生 UFS 读取、
USB CDC 调试串口、userdata 的 HFSX 根卷读写挂载，以及 launchd 和系统服务启动。
2026-10-22：采用上游迁移基线和全量日志镜像后，用户确认已经出现 Hello／设置界面。
原来的约 50% 迁移卡点已越过；触控芯片身份和触点报文已验证，完整设备功能尚未验收。

## 电源键熄屏／亮屏（2026-10-23，实机通过）

修复前电源事件已经进入 HID，但 framebuffer 对 iOS 显示电源请求空返回，
按键后屏幕仍亮且出现颜色异常。现已将 selector 12 接到 DSI0 的 DCS
`0x28`／`0x29`，关屏时停止呈现，亮屏后重新刷新。

按键最小内核构建 [39927732244](https://github.com/tycykp/UniXNU-OnePlus-Ace5/actions/runs/39927732244)
通过并完成临时启动。用户确认“能熄屏，也能恢复亮屏”；6 次显示电源请求的
`error=0`、`ack=0`、`frozen=0`。本次没有刷写分区。

音量键的界面响应此前已确认；三段式仍未取得传感器读数、未完成档位标定。
本次不代表完整 SoC 休眠或电源键长按行为已验证。配置和复测步骤见
[显示电源说明](DISPLAY_POWER.md)，原始状态见
[实机验证数据](../hardware/sm8650-display-power.json)。

## Wi-Fi 地址与 iOS 接口映射（2026-10-23，主机验证）

Wi-Fi 运行时统一管理 12 类 MMIO 资源，补入访问边界、部分初始化回滚和停止时
的释放流程。DMA 明确区分 CPU 地址、真实物理地址与 IOVA，连续分配绕过系统
mapper；MHI、CE、QMI 各自验证分区，MHI 寄存器限制在 BAR 固定窗口，CE
动态窗口拒绝地址截断。未确认 DMA 或 QMP 停止时继续保留对应资源。

iOS 代理请求集中到可测试的映射层，按 20H392 实际 ABI 修正零长标量请求、
PHY 字段顺序、扫描排队状态和跨客户端扫描游标，并检查完整输出容量。
详细资源表、支持的请求和测试命令见 [Wi-Fi 映射层](WIFI_MAPPING.md)。

该映射层阶段没有进行新的内核镜像构建或实机测试。

2026-10-23 续接：最新实机记录在 QMP 远端请求打开通道时退出，尚未再次进入
旧记录的 `FW_INIT_DONE` 等待；精确握手状态修复、QMI 内存分配修复见
[固件就绪记录](WIFI_FIRMWARE_READY.md)。单站关联、WPA2-PSK/CCMP、密钥完成确认、
TCL/WBM TX 与 RXDMA/REO RX、BSD Ethernet 和 iOS 请求已接入代码，主机测试覆盖
认证门控、组播密钥更新、断连清理与真实运行时函数。尚未构建新内核、打包镜像或
触发 CI，也没有新的射频、DHCP、DNS 或联网实测。实现及限制见 [Wi-Fi 联网](WIFI_NETWORK.md)。

同日继续补入隐藏 SSID 定向扫描、连接中扫描、最多三次自动重连、按 RSSI 重新关联
的基本漫游，以及等待真实 REO invalidate/flush 状态的队列复用。SAE group 19
（H&P/H2E）、PMKID、WPA3 四次握手与 PMF/IGTK/SA Query 已接入生产路径。
主机测试验证协议、内存所有权和实际运行时函数；完整支持范围和实机验收缺口仍以上述文档为准。

## 音频板级映射（2026-10-23）

后续已加入固定 ADSP 包／PAS、SMEM／GLINK／GPR、APM 图事务和双 SIA9177
控制核心，可选 `--audio` 协调服务及本地固件嵌入路径。原厂扬声器图、22 组
基线校准和动态模块均已取得。平台资源 lease、DSP 文件服务和物理 PCM／I2C
provider 仍未完整绑定，实际启动与发声尚不可用。详见[音频驱动进展](AUDIO_BRINGUP.md)。

已根据 Android 实机 DT、原厂 `16.0.3.500` 固件和固定版本的一加开源 BSP
加入 ADSP／LPASS、WCD939x、双 SIA9177、IRQ／GPR／Audio ION 映射。
WSA／WSA2／WSA884x 在本机被禁用；扬声器使用 secondary MI2S 和 I2C4 双功放。
base／GUI profile 均接入 `audio.map`、`audio.status` 静态查询，当前
`hardware_ready=0 playback=0 recording=0`。映射和固件完整性已通过离线校验，
完整 XNU 目标编译、DSP 启动及播放／录音实机验证仍待完成。
详见[音频映射](AUDIO_MAPPING.md)及[来源清单](../hardware/sm8650-audio.json)。

## Wi-Fi 固件传输：根端口 Bus Master 修复（2026-10-22）

最新实机已通过 PCIe 枚举、SMMU、MSI、BHI/BHIE 下载和 AMSS 启动，MHI
通道也已启动。下面的 Detect.Quiet 等章节保留早期测试历史，不代表最新阻塞点。

`wifi-terminal-byte-test-1` 在 BHI 下载时读到 `0xffffffff`，根端口 AER
同时记录 Unsupported Request，其请求头指向 `0x20006000` 的 MSI 写入。
驱动只给端点开启了 Bus Master，根端口 Command 仍为 `0x0002`。
使用同一镜像，在端点 DMA 关闭时将根端口 Command 改成 `0x0006`，随后
执行 `wifi.start`：BHI=`0x8b91e5b0`、BHIE=`0x8b91e5b1`、`ee=2`，
`mhi_boot_result=0`；清理后根端口 AER 两类错误均为零，USB dropped=0。

源码修复在 DMA 隔离准备完成后先启用并核对根端口，再启用端点；保留
Command 上半字的 W1C 状态，停机时先排空端点事务再恢复根端口。
另将连续 DMA 内存分配移到调试等待门之前，避免等待期间内存碎片化。
69 项 PCI 模型、完整 Wi-Fi 运行时 ASan/UBSan 检查、ARM64 字节访问检查和
38 项 Python 单元测试通过。整合内核 `8441413570d88e3b7f1859659b3cdf0fec1d2f8b`
已完成不改寄存器的实机复测：驱动自动启用根端口，BHI=`0x8c464810`、
BHIE=`0x8c464811`、`ee=2`，连续 DMA 内存也已在调试等待之前分配。

2026-10-23：新增 `reboot.fastboot` 已实测。PMK8550 SDAM 的 bootloader
原因写入及读回通过（SPMI v7、APID538、值 `0x04`），根卷正常卸载后
PSCI 重启成功，电脑工具核对同一设备的 Fastboot 编号，退出码为 0。
详细记录见 [`hardware/sm8650-reboot.json`](../hardware/sm8650-reboot.json)。

此阶段的后续阻塞为 `QMI / GWQ_IDENTITY (8)`，下一节记录后续修复。
实机对照、镜像摘要和构建身份见
[`hardware/sm8650-wifi-root-master.json`](../hardware/sm8650-wifi-root-master.json)。

## QMI 可选板号处理（2026-10-23）

实机 CAP 响应共 681 字节：chip ID=`0x2`、family=`0x4`，未包含 board_info。
按原厂 CNSS 的规定，仅在字段缺失时使用默认板号 `0xff`；显式板号和固件
资产匹配仍严格检查。修复后 REGDB、板级数据和 PHY/M3 请求均返回成功，
`memory_ready=1 provisioned=1`，身份错误已解决。

当前等待 `FW_INIT_DONE (0x38)` 超时，`fw_ready=0`，尚未进入 CE/WMI 扫描。
初次身份修复记录见
[`hardware/sm8650-wifi-qmi-identity.json`](../hardware/sm8650-wifi-qmi-identity.json)。

随后将单次 QMI RPC 的 10 秒期限与原厂 mission 启动的 40 秒期限分开。
完整 40 秒对照仍只有 `REQUEST_MEM (0x35)` 和 `FW_MEM_READY (0x37)`，
MHI 保持 AMSS/M0，SMMU 和 PCIe AER 没有新增故障。日志与构建身份见
[`hardware/sm8650-wifi-ready.json`](../hardware/sm8650-wifi-ready.json)。

本轮候选版本在 CAP 成功后、REGDB/BDF/M3 之前调用 AOP/QMP，按原厂算法
将实测九项 PMU 参数合并为七条 PDC 命令。仅使用板级已知映射，先验证整张表，
再执行请求；邮箱访问验证共享 lease、两侧缓冲区边界及不重叠、32 位写入读回、
通道握手和命令消费。超时或所有权不确定时保留邮箱及其运行时上下文。
消费确认不代表实际电压读回。首次实机 run `39833570031`：通道打开成功，
七条命令各在 1–2 ms 内被消费，平台配置在 118 ms 完成，M3 在 186 ms 完成。
完整 40 秒后仍未收到 `FW_INIT_DONE`，路由过滤计数为零，MHI/SMMU/AER
与之前对照一致。CAP 电源表的传输已验证，就绪超时尚未解决。

关闭时 AOP 将远端 channel 放下，但本机未完成对称 ACK，旧代码误报 IO；
下一候选修复仅接管属于 APSS 的 channel ACK，保留共享 link。
另对照原厂发现：其 M3 后下发 QDSS 配置，Linux WCN7850 不提供该配置时
显式声明 `CNSS_QDSS_CFG_MISS`。当前运行时既未提供配置也未声明缺失，
下一轮通过 HOST_CAP `FEATURE_LIST` TLV 的 bit 3 做对照，并记录每段固件
内存回应及 M3 地址。该差异是否导致超时仍需实测。

配套 userdata 从原始根卷重建，启用 `--startup-compat --migration-baseline
--full-boot-log --skip-setup`。仅写入本地设置向导偏好，不写激活记录；
首次实机用户确认先显示“软件更新完成”，点击继续后仍进入选择 Wi-Fi／
连接 Mac 或 PC 的设置页，因此现有偏好未完全跳过向导。后续将对照上游
开发板 ADT 的原始 daemon 路径验证；不把进程已启动视为已进入主屏。具体开关见
[`SETUP_SWITCH.md`](SETUP_SWITCH.md)。

## PCIe CESTA/CRM 资源准备（早期记录：链路未建立）

九路电源请求版仍停在 Detect.Quiet。对照 OnePlus SM8650 固定版本的
`pci-msm.c`、`crm.c` 与实机设备树，UEFI 路径补入复位前的 CESTA/CRM D0
资源请求。此遗漏是否导致链路失败尚待实机验证。

实现仅使用 PCIe0 的硬件投票器，保留原带宽请求和其他资源配置；在未使用的
通道中提高 Gen1 所需性能下限，载入已捕获的状态机程序并等待资源就绪。
共享状态机存在其他使用者、通道切换失败或状态归属不明时停止初始化；
无法安全回滚时保留资源。诊断仍使用固定 168 字节的版本 5 报告。

115 项 CESTA 故障注入、55 项 PCIe 流程、609 项启动参数容量测试及完整
运行时离线检查通过，UEFI 编译与镜像校验通过。`jasper2863/UniXNU-giulia`
run `39724512611` 成功。候选镜像沿用已验证的同账号内核及显示、触控代码。
源代码依据、构建身份和候选镜像摘要见 `hardware/sm8650-wifi-cesta.json`。

统一临时启动实测：已通过资源准备并进入链路训练，但仍停在 Detect.Quiet，
未枚举端点。清理阶段返回 EFI_DEVICE_ERROR，因而保留全部资源请求；该版
报告不能进一步定位清理失败位置。1,092,912 字节日志完整保存，USB dropped=0。
详细记录见 `hardware/ios16-wifi-cesta-test.json`。

后续修正补入原厂 PCIe 到内存路径的 CRM 带宽请求，使用 Command DB 的单位
换算初始平均／峰值带宽，逐项保留已有更高请求；IP0 和其他投票器不受影响。
同时按原厂顺序逐个复位核心与 PHY 并检查复位边沿；回滚时先完成 CRM 切换，
再恢复状态机程序。新的失败报告保留清理错误及寄存器位置。这些后续改动
尚未经过实机验证，不能视为 Wi-Fi 已修复。

## Wi-Fi 运行时与全屏显示整合（统一实机测试已完成）

`prepare-ios16.py --gui --touch --wifi` 已接入 PCIe BAR / SID / MSI、独立 SMMU
DMA 区域、MHI BHI/BHIE、QRTR/QMI 固件资源交换、CE/HTC/WMI 被动扫描和
iOS16 IO80211 / BSD `en0` 接口。只有完整初始化成功才发布扫描服务。
扫描结果来自收到的管理帧；关联、认证和数据传输尚未实现。
主机协议测试、实际内核编译／链接和最终镜像校验均通过。统一临时启动成功，
Setup 已启动；九路电源请求版仍停在 PCIe Detect.Quiet，尚未枚举端点，
因此 MHI 固件传输与 IO80211 扫描路径本次没有运行，Wi-Fi 仍不可用。

显示输出改为铺满 1264×2780 原生面板；内部保留 iOS 支持的 1125×2436
D22 画布并进行缩放，触控按整个面板归一化，不再减去居中偏移。
所有原生输出像素、裁剪、内存边界和边缘触控的离线检查通过。
用户本次实机确认“画面铺满，触控正常”。这验证了当前设置界面的显示与触控，
不代表所有应用、旋转和显示电源状态均已验证。
记录见 `hardware/sm8650-wifi-runtime.json` 与 `hardware/ios16-native-display.json`。

构建使用 `jasper2863/UniXNU-giulia`，run `39669410091` 第 2 次运行成功，
源码提交 `5ea2a8009305eaac3619cceefcddadc41451b719`。
统一镜像为 `out/ios16/wifi-native-display-boot/giulia-ios16-wifi-pcie.img`，
69,283,840 字节，SHA256
`9533ca3673991f174ebfc746dd07b098cb313eb438a521ae70aaf15658ff72d9`。
同目录的 `verification.json`、`offline-checks.json`、`runtime-source/` 保存
构建身份、校验结果和实际运行时源码。

原厂固件仅在本地嵌入 `__WIFI_FW` 段，内核 CI 只包含摘要和驱动源码。
统一构建脚本的第九个可选参数为经过校验的 `firmware.pack`，此模式增加
运行时测试、固件嵌入和内核／固件／显示实现绑定检查。用户要求完成整合后
统一测试；本次执行一次 `fastboot boot`，没有刷写。完整记录见
`hardware/ios16-wifi-native-display-test.json`。采集 1,210,480 字节，
1,736 条用户日志解码完整，USB dropped=0。

## Wi-Fi PCIe 调试续接历史（2026-10-22）

信号诊断版实机确认：7 路电源请求和 12 路时钟使能完成，PHY 就绪条件通过；
PERST 已释放，SW_CTRL / WLAN_SW_CTRL 输入为高，参考时钟门控打开，
PIPE 已选择 PHY，LTSSM 开关已置位，但链路仍超时，尚未枚举 WCN7850。
这些寄存器读回不等于已测量实际时钟波形。原始日志已完整保存，USB 丢失计数为 0。

续接版补齐链路配置：显式开启 DLL、关闭 fast-link 模式，统一端口模式、
GEN2_CTRL 和 LNKCAP，并检查写入是否生效；LNKCAP 写保护只在写入期间开放。
报告包含训练状态位图、状态变化次数以及 PIPE / PHY / CLKREQ 读回。
版本 4 加入有超时的 GCC PIPE 频率测量，保存并恢复五个调试寄存器；
遇到计数器已运行则跳过测量，清零与计数完成各最多等待 10 ms。
168 字节报告采用固定长度 Base64，预留 240 字节后才调用硬件初始化，
适配 Darwin 22 的 608 字节启动参数。版本 5 补入主控 PHY 的 L1I / L3I
电源请求；9 路请求及上限均在实机设备树记录的电压范围内。旧版日志仍可解析。

- 单通道版实机配置读回通过，但 100 次采样均为 Detect.Quiet，仍未建立链路。
  系统继续启动 wifid 和 Setup。默认端口读回 `0x30120`，对应双通道。
- 双通道版实机三处配置也已读回正确，但仍停在 Detect.Quiet。
  因此链路宽度不是唯一阻塞，真实 Wi-Fi 扫描仍未修复。
- 时钟版实测 PIPE 为 62,358,622 Hz，测量完成且五个调试寄存器均恢复；
  仍未读到端点 ID。该结果不能单独证明 PHY / 主控时序已正确。
- 早期整合构建为 `out/ios16/wifi-integrated-boot/giulia-ios16-wifi-pcie.img`，
  SHA256 `33dd156535c8187084d553a79a497d3f3c592d7e1e0e666d1f71c6baeb3ce9cd`。
  后续包含相同主控电源请求的运行时整合版已实测，仍停在 Detect.Quiet；
  记录见 `hardware/ios16-wifi-native-display-test.json`。
- 53 项 PCIe / 时钟测量故障注入、9 项电源缓存、609 项启动参数容量、
  9 路板级电压约束、ABI 编解码、重复补丁及 22 项
  DRAM 交接检查通过；旧版固件与新版负载混用会在打包阶段被拒绝。
- `out/ios16/wifi-pcie-corecfg-boot/` 是启动参数容量不足的废弃构建，
  **不要启动**；该构建未用于实机。
- 该旧构建不含扫描、固件加载、DMA / MSI / MHI 与 IO80211 运行时。
- 按用户最新要求，先集中完成代码和离线检查，不再每个改动都要求用户进入
  Fastboot。以上属于底层初始化整合包，不能称为已完成的可联网 Wi-Fi 驱动。

统一构建脚本 `scripts/build-ios16-wifi-bringup.sh` 使用已准备好的 Darwin22
UEFI 源码树、虚拟环境 Python、已验证内核/ADT、根卷报告、信任缓存及已捕获
Android DTS；输出新目录，保留源文件哈希、测试报告及完整构建日志，不操作设备。

### 原厂 WLAN 固件资源

已从用户提供的 PKG110 `16.0.3.500(CN01)` 镜像中提取 73 份固件和配置，
保存在 `out/ios16/wifi-stock-firmware/`。NON-HLOS、vendor、odm 等输入镜像
与包内校验清单匹配；资源包记录各文件 SHA256、原始 ELF 边界及覆盖来源。
ODM 的 `kiwi/bdwlan.b0c`、`bdwlan.elf`、`regdb.bin` 优先于 NON-HLOS 通用版本，
这与捕获到的 Android 固件搜索路径和 OnePlus CNSS 代码一致。

`scripts/ios16_wifi_firmware.py` 根据实际 SOC_HW_VERSION 与 QMI 芯片/板级 ID
选择 amss / amss20、PHY 微码和精确 BDF；缺少身份、区域不符、缺少变体、
ELF 越界或文件损坏均拒绝。22 项选择及完整性测试通过。尚未读取实际芯片版本
和 QMI board ID，没有导入手机独有的 persist 数据，也没有上传固件。
运行时已接入这些资源，尚未取得实机扫描结果。资源记录见
`hardware/sm8650-wifi-firmware.json`。

## Wi-Fi 第一阶段：恢复 wifid 的设备树前置条件（2026-10-22）

用户已进入设置向导的“选取 Wi-Fi 网络”页面，但没有网络列表。对当前
20H392 根卷和启动包的只读检查确认：`wifid` 文件及启动作业存在、未被禁用，
其 `_LimitLoadToDeviceTree` 要求 `wlan` 或 `marconi-wifi` 节点，而原 ADT
两者都没有，启动日志也没有 `wifid`。此外，当前内核尚无 WCN7850 无线传输
驱动及 IO80211 接口；补入节点只能修复服务的前置条件，不能产生真实扫描结果。

`make-ios16-adt.py --wifi-bringup` 显式加入 `/arm-io/wlan`，声明原厂设备树
对应的 Qualcomm WCN7850 / Kiwi（PCI ID `17cb:1107`），并保持已有 MAC 身份。
此选项需要 `--gui --wifi-mac`；打包要求 `--trace-all`，以观察服务启动和等待点。
节点不含未枚举的 BAR、DMA 或 Broadcom 硬件属性；`0xb0000000` 是原厂
`smmu_iova_ipa` 窗口，不能作为无线芯片的 MMIO 地址。

- 调试启动包：`out/ios16/wifi-service-boot/giulia-ios16-wifi-service.img`，
  SHA256 `d10fb54d93e7aa8b09a1d62db600fac5615ec3cee33d61b5bd590840b25a2f98`。
  复用已进入 Hello 的内核、根卷标识、信任缓存和触摸配置，保留现有 userdata。
- 12 项设备树／作业条件／错误资源测试、22 项内存交接检查和负载／启动包
  校验通过。主机检查不代表实机 Wi-Fi 已修复。
- 用户进入 Fastboot 后已执行一次 `fastboot boot`，传输和启动命令成功；
  实机出现 `EXEC pid=43 ppid=1 name=wifid`，Setup 也已启动。第二轮栈采样中
  `wifid`、`configd`、Setup 主线程均在事件循环；早期网络配置服务等待已结束。
  用户确认已返回 Wi-Fi 页面，列表仍为空，**真实 Wi-Fi 扫描尚未修复**。
- 240 秒采集保留 1,075,475 字节，USB `dropped=0`，911 条用户输出记录解码
  完整；未见内核 panic 或 wifid/Setup 崩溃，仍存在其他驱动／小组件服务失败。
  本轮没有执行 flash。完整记录见 `hardware/ios16-wifi-service-test.json`，
  原始实机日志在 `out/ios16/wifi-service-test-1/`。
- `scripts/audit-ios16-wifi.py` 可独立检查根卷、ADT、内核和串口日志；
  基线证据在 `out/ios16/wifi-baseline-audit/`。硬件依赖见 `hardware/sm8650-wifi.json`。

## 当前交付：上游迁移基线与全量启动日志（实机进入 Hello）

本次实机中迁移服务正常退出，账户服务进入主循环，Setup 已启动。首次启动曾在
ChronoServices 小组件描述符查询上等待，随后继续进入设置界面。记录到一次
SBRestartManager 主动重启 Setup，随后新实例启动；未捕获 SpringBoard、backboardd
或 Setup 的未处理崩溃，USB dropped=0。原迁移进度条随所选基线略过，这是当前策略
的结果。完整证据见 `hardware/ios16-baseline-hello-test.json`，本次没有再次刷写或重启手机。

采用 K70 `ios-vermeer-lab` 已实机进入设置界面的干净根卷迁移基线，
在镜像构建阶段写入 `LastSystemVersion=20H392` 和对应 `DMUserDataDisposition`。
这是明确选择的启动初始化方案，不表示原来的安装回调已经完成。
设置向导与激活状态保持原样；不再使用内核运行时修改迁移偏好的旧实验。

- 新 userdata：`out/ios16/userdata-baseline-full-log/giulia-ios16-userdata.img`，
  单份 16 GiB 原始镜像，SHA256
  `8136ac0905a90af79430c4ebc56231f2ad3ec947fe3c9b9c120fd9b58857ba3e`。
  文件系统复检、整盘稳定哈希、501:501/0600 基线权限和设置偏好保留检查通过。
- 配套启动包：`out/ios16/baseline-full-log-boot/giulia-ios16-touch-trace.img`，
  SHA256 `7c3775509a8108e1c7dc6ffefb75a23f42c086f4e8a9a426943b9be2d73cbf97`。
  负载、内存交接与启动头检查通过。新内核 CI
  `39629676117`（源码 `1e81a4f`）编译通过；含官方信任查询和 `pid_hibernate`
  兼容修复，保留之前的触摸、显示与内存交接。
- 全量日志：所有进程的标准输出／错误按短十六进制记录保留原字节；记录正常
  启动、正常退出、异常退出及退出原因；全进程周期栈采样不再在 40 轮后停止。
  每线程最多 64 帧，逐帧输出并注明结束或深度限制；RAM 日志环为 32 MiB。
- 528 个启动作业开启详细日志环境。`scripts/capture-ios16-full.ps1` 连续接收、
  64 MiB 自动分卷、断线重连并保存状态计数；`scripts/decode-ios16-full-log.py`
  还原各进程原始输出，处理跨卷记录，汇总丢失计数和记录不完整的证据。
- 所有主机检查与上游依据记录在 `hardware/ios16-baseline-full-log.json`。
  userdata 由用户手动刷入后，再临时启动配套镜像并连续记录整次启动。

## 历史整合包：查询接口已生效，迁移仍等待

本次串口实测：官方缓存查询服务已注册，`lsd` 和 `installd` 的 selector 6
均返回匹配成功。当前卡在 SystemAppMigrator **第一组**安装回调等待
（返回地址 `0x8178`），尚未到第二组验证／降级等待。`appstored` 和
`securityd` 等账户服务；`accountsd` 等数据迁移；数据迁移等待当前插件结束。
存在循环依赖迹象，但尚未识别具体未完成的应用协调器／回调。画面提交计数
仍增加到 68,000，USB dropped=0，无捕获到的 kernel panic。
证据：`hardware/ios16-startup-repair-test.json`。本次只读取日志，未刷写或重启。

- userdata：`out/ios16/userdata-startup-repair/giulia-ios16-userdata.img`，
  单份 16 GiB 原始镜像；SHA256
  `e201e552695443d134ac3926d690ac6856cf7ca4130779b30d14826a933c6f21`。
- 配套临时启动：`out/ios16/startup-repair-boot/giulia-ios16-touch-trace.img`；
  SHA256 `590e7787673e402d1b079468545602dbcb5d393475590a4ec3981966b0a3cc2e`。
- 内核构建 `39620286640` 成功，提交 `6f5b6db`。整盘文件系统检查、稳定哈希、
  独立内容读回、内存交接及启动包配对检查通过；此轮没有刷写或重启手机。
  userdata 仍由用户手动刷入；当前运行中的信任查询已验证，完整迁移仍未通过。

- 先整合内核和 userdata，再做一次配套实机验证。
- `SystemAppMigrator` 的第二个等待组（返回地址 `0x8370`）负责验证后的
  应用降级。其 `MISValidateSignature` 使用 `TrustCacheOnly`，原内核没有
  `AppleMobileFileIntegrity` 查询服务。本次添加只读 selector 6：仅对已校验
  官方固件信任缓存中的精确 20 字节代码哈希返回成功，未匹配返回 NotFound。
  这不是完整 Image4/AMFI 实现，也没有关闭既有代码页哈希检查。
- 固定 SHA256 的 20H392 IPSW 提取出 3 份官方缓存，共 2,851 个唯一哈希；
  userdata 内 32 个完整待安装应用的全部 CodeDirectory 哈希均已离线匹配。
- 新 userdata 保留正常迁移、设置向导和账户服务。移除 launchd 缓存中的
  4 个 CommCenter 作业；VirtualAudio 移至 HAL.disabled；D22/D221/Default
  的录制服务启动关闭。加入上游单 CPU 软件绘图所用的 SpringBoard watchdog
  配置。基带、音频、GPU、DRM 功能仍未实现。
- 构建工具：`make-ios16-trust-catalog.py`、`prepare-ios16.py --gui --touch`、
  `rebuild-ios16-userdata.py --startup-compat`、ADT/Payload 的 `--trust-catalog`。
- 完整记录位于 `hardware/ios16-startup-repair.json`；实机是否越过 50%
  必须以后续日志和屏幕观察为准。

## 上一次实机结果（2026-10-21）

- 当前 userdata：`out/ios16/userdata-service-repair/giulia-ios16-userdata.img`，
  单份 16 GiB 原始镜像。SHA256：
  `ed662ef08b289ae56591d3166dc0e34f4a7241111df256c107383a0a3c1185c0`。
- 从已校验的官方系统基底重建，补齐容器目录，不写入 `LastSystemVersion`
  或 `DMUserDataDisposition` 完成标记，保留真实首次迁移流程。
  文件系统、卷标识、属主权限和整盘哈希复检通过；旧原始镜像未变。
- 配套临时启动镜像位于 `out/ios16/service-repair-boot/`，复用先前实机跑通的
  `65ef183f...` 内核，增加 `lsd` / 容器服务采样及设备树 Wi-Fi 身份输入。
- 已定位 `lsd` 在初始化应用数据库、生成内置应用安装标识时，被
  MobileGestalt 对 `IONetworkController` 的等待阻塞。原设备树缺少其先行
  查询的 `/chosen/mac-address-wifi0`。修正版补入六字节本地管理地址；
  这是移植环境的稳定身份，不是工厂 MAC，也不代表 Wi-Fi 已实现。
- **停用** `gui-seed-candidate` 的跳过迁移方案，以及
  `migration-restore-repair` 的内核改偏好实验。后者在根文件系统仍只读时
  重命名返回 EROFS（30）并 panic，备份未提交；相关内核代码已在 `79ad14e`
  移除。不要再启动其 `09236d6b...` 镜像。
- 按用户约定：userdata 由用户手动刷入，报告完成后再临时启动配套镜像。
  此版已实机验证：进度条恢复并曾前进，随后停在约 50%；后两次采样中
  `lsd` 与 `installcoordinationd` 已回到正常主循环。安装／账户链仍待修复。

证据及状态：`hardware/ios16-service-repair.json`。

## 历史：加载卡点复核与已停用的候选镜像（2026-10-21）

- 同次启动的两次只读采样确认 `SystemAppMigrator` 等待系统应用安装；
  `appstored` 此时等待 `DMPerformMigration`，另有线程阻塞在
  `AMSFairPlayGetDeviceType`。此前的账户查询栈不能代表当前等待点。
  `accountsd` / `installd` 在采样时处于主循环或空闲工作线程。
- 原始、哈希匹配的基础镜像缺少 `com.apple.migration.plist` 和上游 GUI
  初始化要求的容器目录。手机上已经变化的文件系统尚未读回。
- `scripts/prepare-ios16-gui-root.py` 在隔离的写时复制副本中补齐新根卷的
  `LastSystemVersion` / `DMUserDataDisposition` 与容器目录；核对固件版本、
  文件系统、卷标识及 sparse 还原哈希。它不连接或刷写手机。
- 这是针对全新系统镜像的启动候选方案，不是安装、账户或 FairPlay 服务的
  完整修复，也不能用于覆盖已有用户数据的迁移记录。实机效果仍待验证。
- 后续 `ios16-root.yml` 已改为 `gui_layout`，只创建容器目录；不再提供
  跳过迁移的构建选项。下面保留早期候选的验证历史，不用于继续刷写。
- 内存日志确认 XNU 分配窗口为 2 GiB，启动初期空闲约 2006 MiB、固定占用约
  42 MiB。当前串口仅支持传输状态与栈采样，不能查询实时可用内存或 RSS；
  已采集日志未见 OOM 终止，但有 Jetsam 高水位设置返回 EINVAL 的记录。

复核证据见 `hardware/ios16-service-stall-audit.json`；本机日志位于
`out/ios16/service-stall-investigation/`。镜像制作命令（Linux/WSL）：

```sh
sudo python3 scripts/prepare-ios16-gui-root.py \
  --source out/ios16/userdata/giulia-ios16-userdata.img \
  --report out/ios16/root.json --out out/ios16/gui-seed-candidate
```

输出目录必须尚不存在。刷入候选镜像会覆盖 userdata，应由用户明确决定并
手动执行；保留原始镜像和分区备份。通过主机校验不等于手机已越过加载画面。

本机候选 `out/ios16/gui-seed-candidate/giulia-ios16-gui-seed.sparse.img`
已完成打包（11,826,057,188 字节），SHA256 为
`9cfe02137fad51c27d85a4766cf4f35a8e1c2095f0d2bb756ee703fec25f7c56`。
其 16 GiB 还原哈希为
`cb70191c4e23a330b6e613c7319bd242f151e36d5160473518f48f9d1c8e093b`。
容器属主、偏好权限、卷标识、整盘 fsck 与复检前后哈希一致性均通过；原始
镜像未变，5 项初始化保护测试通过。手机未重启、未刷写，尚无实机通过结论。

## 图形版构建产物（2026-10-21）

- 私有构建仓库：`jasper2863/UniXNU-giulia`，本地 remote 为 `build-jasper`。
- GUI kernel run `39583173365` 已成功，源码提交 `8859fc4`；构建、图形服务检查、内核重定位检查全部通过。
- Artifact `12339141121` 已下载并核验 GitHub SHA256 / ZIP CRC；内核 SHA256 为 `9271cb37d8a5602f425a14787dbc3bcdc1919ce9e0683177f4a00ad2b19d04a8`。
- 配套临时启动镜像：`out/ios16/gui-boot-39583173365/giulia-ios16-gui.img`，SHA256 `1fcdeffa431fd8986f7518abb02f31506411be1f9aab5525dc27af70271e5971`。
- 已核验 GUI ADT / 平台与 CPU 驱动匹配、DRAM 交接、根卷标识、Android 启动头和负载校验；实机已确认首次呈现，测试记录见 `hardware/ios16-gui-test.json`。
- 系统入口使用 `sweetfb_hz=30 sweetfb_noconsole=1`，首次呈现后才静默普通屏幕日志；USB 和 panic 日志继续保留。
- 后续下载新仓库产物使用 `scripts/fetch-action-artifact.py --repo jasper2863/UniXNU-giulia ...`。账号凭据仅保存在本机登录会话／凭据库，不在源码中。

## 触摸与服务采样

- `prepare-ios16.py --gui --touch` 加入 Ace 5 的 GENI SPI / TouchCom v1 驱动；`make-ios16-payload.py --enable-touch --trace-services` 启动驱动并扩展迁移服务采样。
- 驱动先验证时钟、SPI 固件、FIFO、DMA 空闲和引脚状态，再识别触控应用、切到 LBP 并读回触摸报文配置；收到有效报文才发 HID 事件。
- ClockDxe 的触摸时钟交接已实机验证。控制器回报 GPI-only（FIFO 禁用）；新增 `--touch-gpio` 调试后端使用已确认的 GPIO 模拟 SPI，不配置 DMA/SMMU。GPIO 后端尚待实机验证。
- 当前未实现 PMIC 电源恢复、GPI DMA 传输及 TouchCom v2/CRC 扩展；尚不能认定物理触摸已可用。
- 迁移采样与插件地址已定位到 `SystemAppMigrator`。早期 App Store 取消请求的 45 秒等待已结束；持续等待点是 `waitForSystemAppInstallDispatchGroup`，其超时为每项待安装应用 40 秒。`appstored` 正在等待 `ACAccountStore` 同步 XPC 回复，继续追踪账户/钥匙串服务。
- 新 GUI 构建增加只读串口命令 `sample`，唤醒同一个采样线程；配套 `capture-debug.ps1 -Command sample` 可在同次启动中请求采样。最多 40 轮，返回 0 表示采样线程已停用或结束。
- 寄存器来源、引脚和限制见 `hardware/sm8650-touch.json`。触摸解析测试不代替实机测试。

## 根卷和用户态进展（2026-10-21）

- 用户自行刷入已校验的 userdata 原始镜像后，实机确认 GPT、卷标识匹配以及 HFSX `iOSRoot` 读写挂载。
- `OS_REASON_EXEC / 0xe` 是 `WRONG_PLATFORM`。已通过开发内核自带的 `unentitled_ios_sim_launch=1` 解决，没有修改 launchd、dyld 或关闭代码页哈希校验。
- 原始任务缓存加载器在没有 AMFI IOService 时退出；`launchd_unsecure_cache=1` 已通过实机验证，随后开始启动系统服务。
- 此前基础内核镜像：`out/ios16/root-job-cache/giulia-ios16-root-job-cache.img`；SHA256 `e62850f45f2853010a407a4b5c0359528a831c03fee202bc19cfe25715a456f9`。
- 完整本地日志：`out/ios16/root-job-cache/usb-replay.log`。进度与签名页审计分别见 `hardware/ios16-root-mount-test.json` 和 `hardware/ios16-launchd-platform-audit.json`。
- 当前基础内核没有编入 IOSurface / IOMobileFramebuffer / HID 服务。已准备可选 `--gui` 源码配置及 `make-ios16-adt.py --gui --product-adt LOCAL_D22AP.adt`，实际物理屏幕限制为 `0xd5100000 / 1264×2780 / stride 5056`，UIKit 兼容画布为 `1125×2436`。
- 本地通过了 65 个补丁格式检查、8 个实际屏幕几何限制表达式测试、基础配置回归，以及 GUI ADT 与旧内核误配拒绝检查；GUI 内核整体编译和实机首次呈现现已完成。
- `ios16-xnu.yml` 选择 `profile=gui` 会构建图形内核，随后需在本地结合已校验的产品 ADT 打包。GUI 构建不自动使用基础 ADT 生成启动镜像。
- 旧仓库 GUI run `39581534474` 曾在编译启动前因账号付款／消费限额被 GitHub 拦截；已在上述新私仓完成构建。
- 物理触控、GPU 加速和稳定图形会话均未验证。当前已验证结果不等于完整 iOS 移植完成。

## 当前实机结果（2026-10-21）

- 修复低 DDR 保留区扫描、DRAM 设备树交接参数和平台驱动匹配。
- 启动镜像：`out/ios16/platform-match/giulia-ios16-platform-fixed.img`。
- SHA256：`e1692b6d31d9aa1d067c26535c8a20757acc8969d0e8fcff33eafbfde1beab1f`。
- 源码提交 `6625ca0`；本地 WSL 构建 UEFI，复用 run `39565899829` 的内核。
- USB 串口 `ACE5DBG001` / COM21 已完成 `replay`、`status` 双向通信；状态查询报告 `dropped=0`。
- UFS 读取 8192 字节成功，CRC32 `c319db94` 与 Android 基线一致，探测返回 0。
- 当前启动项 `giulia_ufs_probe=1` 会在探测后保持调度器运行，等待 USB 调试命令。
- 本轮仅使用 `fastboot boot`，未刷写 userdata；未尝试挂载未准备好的系统根卷。
- 详细证据、边界及日志 SHA256 见 `hardware/ios16-bringup-verified.json`；完整串口日志保存在本机 `out/ios16/platform-match/`。

当前内核仍沿用 `SweetPlatformExpert` 的历史匹配名，设备树在保留
`oneplus,giulia`、`qcom,sm8650` 和 Ace 5 型号的同时提供兼容项。
后续内核源码配置已改为原生 Ace 5 匹配名；打包时会核对实际内核中的平台和 CPU 匹配规则。

以下为早期 iPadOS 17.7.11 / Darwin 23 路线的构建与测试记录，不能代替上述当前状态。

## 早期 UEFI 验证状态（2026-10-21）

- Ubuntu 24.04 GitHub Actions 构建及镜像结构检查通过。
- 下载到本机后复核通过，镜像大小 2,179,072 字节。
- 原厂 Fastboot 接受临时启动，用户确认屏幕出现 **UniXNU 菜单**。
- 用户确认音量上、下键可以正常移动菜单选中项。
- 实机 `initrd S14 ... Success`：定位到校验匹配的 FAT 负载，并复制到 UEFI 分配的内存。
- 实际加载 `\\EFI\\UniXNU\\unixnu.toml`，显示 Ace 5 XNU 启动项。
- 截图中的绿色条由 `DarwinEnter` 在 `ExitBootServices` 成功后绘制，证明进入加载器交接代码。
- 绿色条在跳转 XNU 前绘制，不能据此认定 XNU 内核已执行。
- 原生 UFS 系统盘访问、内核执行和用户态仍待验证。
- 本次使用 `fastboot boot`，没有执行刷写命令或修改 userdata。

构建源码提交：`8277a88371e0fd6a64cc852652583ac41b5a4609`。
构建记录：https://github.com/tycykp/UniXNU-giulia/actions/runs/39508142284

镜像 SHA256：
`48d659de8ca9c8821a21cea440484ff8c190dfc82aee8c48f95a685e8f0fcb1a`

当前 WSL 的 Ubuntu 26.04 在 Mono/NuGet 准备工具时遇到
`System.Native` 运行库错误，因此本轮使用私有仓库 Actions 构建。

## XNU 与系统卷进度

macOS Actions run `39511969648` 已成功构建 ArmDTPlatform、ArmCpu、
corecrypto、pthread、预链接 XNU 及 192 MiB FAT 负载。
内核 SHA256：`5de566bab4ffbcf7b67905249fdeff50037996bb6f22bfe20b446fab48d2e5f6`。
这是离线构建成功，不代表内核已在手机上执行。

启动器传来的 DT 保留旧 Android initrd 信息，因此仅按它的地址定位失败。
当前版本在 UEFI 声明的普通 RAM 中查找指定大小和 CRC 的 FAT 负载，
复制后再次校验并发布为只读块设备。扫描排除 MMIO、运行时及未知保留区。
CRC 用来排除意外的旧数据，不替代内核或系统签名校验。
`GiuliaInitrdStatus` 是易失诊断变量，不写永久 UEFI 变量。

已验证的构建：UEFI run `39516550749`，XNU run `39511969648`。
完整临时启动镜像 SHA256：
`c34cb52e6d62920e0a96d4f39d08a4abbac8fd9e3774b5c41dbbaac781bd7bf2`。
本机基线位于 `out/verified-uefi/`，仅用于 `fastboot boot`，不刷 boot 分区。
详细观察结果见 `hardware/uefi-verified.json`。

OS/SystemOS 文件已按 `board.toml` 哈希校验并组装为 9,382,658,048 字节
系统映像。原始 OS 的 GPT CRC、SystemOS 的 APFS 超级块校验通过。
映像仍未通过实机存储验证，也未刷写 userdata。
SM8650 存储寄存器证据在 `hardware/sm8650-ufs.json`。

Data 卷 run `39521758751` 已成功。32 MiB APFS 种子已下载到本机，
SHA256、容器大小和超级块校验均通过，包含原始初始 var 树的 39 个文件。
卷 UUID 为 `9E4C0509-9215-4EC5-872F-0254D84A223C`，验证记录见
`hardware/data-seed.json`。
现已把原始 Data 种子作为 dense `RAMDisk` 加入 FAT 负载，设置 `initrd` 路径、
ADT `RAMDisk` 占位项和易失 Data 属性；现有内核包含标准 memdev 支持。
本轮未开启依赖未完成根卷链路的自动 Data 挂载，也未引入尚未资格验证的存储类绑定。
内核/ADT/Data 所需交接内存为 103,743,488 字节，位于 256 MiB 窗口内。

UEFI run `39526260528` 与新负载匹配。实机截图确认 Data 启动项、S14 Success、
DarwinBoot inputs validated 和绿色交接标记；XNU 执行、Data 设备注册及挂载仍未确认。
验证记录在 `hardware/data-handoff-verified.json`，本机临时启动镜像在
`out/verified-data-handoff/`。这是内存 Data，重启不保存运行时改动。
后续 XNU Actions 会下载固定的 Data 种子 artifact，生成并核验 `giulia-xnu-data`
负载；对应 `data-uefi-identity.json` 必须随负载一起用于下一次 UEFI 身份绑定。
同一轮已保存约 5.14 GB 的 OS 固件缓存，后续匹配缓存键的 Data 任务可复用。

设备为 PKG110 / SM8650，当前 Android 16 固件为
PKG110_16.0.3.500(CN01)。设备使用 KernelSU，BL 属性经过隐藏，
不能以 Android 的 locked/green 属性判断真实解锁状态。

## 源码与改动

`sources.json` 固定 Mu-Silicium、UniXNU 和 DarwinBoot 的版本。
使用上游 `giulia` 的 model 0（Ace 5），不是 model 1（13R）。
`patches/giulia-unixnu.patch` 在 DSC/FDF 中加入 UniXNU 菜单、注册驱动和
DarwinBoot，并修正上游 SMBIOS 的 PKG110 型号拼写。
启动头的 OS/补丁字段匹配当前原厂 boot.img（14.0.0 / 2026-01）；
这描述 GKI 启动头，不是手机设置中的 Android 系统版本。
保持上游板级内存图和驱动，先验证 UEFI 基础能力。

测试镜像使用上游附带的 giulia DTB；实机 DTB 只在本地进行对比。
手机的完整属性、序列号、原始分区及设备转储不应提交到源码仓库。

## Linux / Ubuntu WSL 构建

Actions 固件缓存：`data-seed.yml` 缓存经过校验的 OS 组件，`xnu.yml`
缓存原始/解压 kernelcache。缓存键只依赖 IPSW 身份、下载地址和固件校验值，
修改驱动、设备树或 userdata 配置不会导致重新下载。
OS 下载完成就保存缓存，即使之后制作 Data 卷失败也能复用。
命中缓存后仍校验实际文件 SHA256；有效 OS/解压内核缓存可完全跳过对应的 CDN 请求。

依赖：git、python3、python3-venv、build-essential、clang、llvm、lld、
uuid-dev、nasm、gcc-aarch64-linux-gnu、acpica-tools、device-tree-compiler。
上游 NuGet 工具下载还需要 mono-devel 和 ca-certificates-mono。

```bash
bash scripts/build-uefi.sh "$HOME/ace5-port"
```

输出：`$HOME/ace5-port/uefi-giulia/Mu-giulia-0.img`。
构建脚本不会调用 adb/fastboot，不会刷写手机。
上游 setup 会递归下载子模块，首次执行需要较长时间。

```bash
python3 scripts/check-device.py /path/to/running-device.dtb \
  "$HOME/ace5-port/uefi-giulia/Resources/DTBs/giulia.dtb" \
  --output reports/device-check.json
```

## 验证边界

`verify-image.py` 检查 Android v4 头、压缩完整性、BootShim ARM64 标识、
固件卷大小和字节一致性、追加 DTB 及镜像 SHA256。
通过这些检查仅表示镜像结构正确，实机显示、侧键和存储仍需验证。
UEFI 含有磁盘访问能力，临时引导本身不等于所有菜单操作都无写入。
首次实机测试只检查菜单、显示和按键；不要执行分区、格式化、刷写操作。

## 后续阶段

1. UEFI 菜单与实机启动日志。
2. 负载卷与 DarwinBoot 交接验证。
3. 准备 macOS/XNU 构建环境，添加 Ace 5 的 ADT、内核配置和平台驱动。
4. 在明确备份与分区方案后评估系统卷；本阶段不处理 userdata。

`board.toml` 和 `giulia_defconfig` 提供第一轮单核 XNU 构建配置。
`make-giulia-adt.py` 使用实机的 GIC、CPU0 与定时器参数；不发布 UFS/GPU 节点。
目标 kernelcache 沿用上游验证过的 iPad7,1 / 17.7.11 / 21H461 ABI，
这些 Apple 内核参数不代表借用了 sheng 的硬件驱动。

`xnu.yml` 在 macOS Actions 构建内核和 FAT 负载，尚未经过内核实机验证。
此负载没有 OS/SystemOS/Data 卷；不能启动到用户界面。
系统卷和 SM8650 存储/GPU/触控适配继续独立推进，不使用 sheng 的设备参数。
