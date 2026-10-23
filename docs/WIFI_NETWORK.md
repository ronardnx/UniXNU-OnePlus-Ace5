# Wi-Fi 单站联网代码与验证边界

2026-10-23 从中断的 Wi-Fi 对话继续实现。本轮只修改代码和运行主机测试，没有
构建内核、打包镜像、触发 CI 或重新运行设备。当前不能宣称手机已经能联网。

## 启动与请求

最新设备记录停在 QMP 通道打开阶段，早于固件就绪通知。已修复与捕获状态一致的
远端打开握手，QMI 对支持的非安全内存类型分配真实 IOVA，拒绝以零地址回答失败
分配。来源、实机时间顺序与尚需确认的节点见 [固件就绪记录](WIFI_FIRMWARE_READY.md)。

`GiuliaWiFi.cpp` 在 QMI 配置后准备独立 4 MiB 数据区，连接 HTC HTT 服务，等待
WMI 硬件能力与 CN 监管规则，再启动数据环并发布接口。国家码请求等待异步规则
事件，错误或超时不会把未授权频道当作可发送频道。
创建 vdev 后显式设置 TX Ethernet 封装和 RX Ethernet 解封装，匹配数据环解析器；
不能依赖 WMI resource 配置中的 Native Wi-Fi 默认值。

`giulia_wifi_network.h` 解码 20H392 的 592 字节关联请求、派生后的 32 字节 PMK，
以及 SAE 请求的 1–64 字节密码。密码的 cipher=10、长度 +0x3c、内联数据 +0x48
来自目标框架反汇编；不解析用户指针，不记录密码或密钥。网络层从实际扫描条目选择 SSID、安全类型、
可选 BSSID、速率及当前监管规则均匹配的 AP。请求锁和运行时锁分别保护用户回传
与设备状态。短输出不会先改变连接请求，用户输入快照和临时输出释放前擦除。
Apple80211Bind 首次以零输入 payload 查询 DRIVER_VERSION，仍返回 256 字节；
该绑定入口与通用 GET 的 256 字节输入形式均有完整 UserClient 回归。
Apple80211DisassociateExt 的 SET22 使用 8 字节 payload，断开原因位于 `+4`；
首 word 只是框架栈初始化值，不误当 version。普通零长度 scalar 形式继续支持。
Apple80211CopyCurrentNetwork 的 GET103 返回实际关联 peer 的完整 1164 字节 BSS，
包括 SSID、BSSID、IE 和 age；连接中扫描不阻止查询，也不消费扫描结果游标。

## 关联与安全

`giulia_wifi_station.h` 依次等待 peer map、vdev start、开放认证响应、关联响应和
peer-assoc 确认；管理帧 DMA 缓冲保留到对应 TX 完成。断连等待 vdev stop、peer
delete/unmap，迟到事件不能提前确认另一个操作。无法确认设备停止时保留 DMA 资源。
legacy PHY 按实际支持的速率区分：2.4 GHz 纯 CCK 为 11b，含 OFDM 为 11g，
5 GHz 为 11a；WMI peer-assoc 和 iOS 当前 PHY 共用速率判定，未知可选速率不算 OFDM。
接口发布及每次关联前提交明确的 ACTIVE 策略：UAPSD=0、RX_WAKE=WAKE、
TX_WAKE=ALWAYS、PSPOLL=NO_MAX，再提交 STA_PS_MODE_DISABLED。五条命令之一失败
即不声明策略已建立，并阻止继续关联。GET5 只在成功提交该策略后报告 disabled=0；
这里记录的是驱动策略，不是固件功耗读回，也没有宣称实现睡眠 / TIM / DTIM offload。

`giulia_wifi_rsn.h` 处理 WPA2 四次握手、MIC、RFC 3394 GTK 解包、重放计数与组密钥
更新；SHA1/AES 由项目已有 XNU 加密接口提供。PTK/GTK 只有在实际固件密钥完成事件
后生效，普通 IP 数据只在受控端口打开后发送。重复握手不得重新安装相同密钥或重置
PN；GTK 更新完成后重新打开端口。早于 WMI 关联确认的 EAPOL M1 排队保留到 RSN 初始化。

SAE group 19 实现 H&P 与 H2E、commit/confirm、anti-clogging token、重试与超时，
采用固定版本 BearSSL P-256/SHA256。仅在确认成功后将 PMK/PMKID 交给关联和 RSN；
WPA3 四次握手使用 SHA256 PTK 派生及 AES-CMAC MIC，并验证 H2E RSNXE，拒绝降级。
WPA2 可协商 PMF，WPA3 必须启用 PMF。IGTK 从已验证 MIC 且解包成功的 KDE 安装；
网络层同时上报目标框架的 SAE capability 75（第 9 字节 `0x08`），使显式和自动
选择 WPA3 的路径可以进入真实 SET20；目标框架的默认 MFPC 选择也使用该位。
PMF 的单播 CCMP、组播 BIP-CMAC/IPN 和 SA Query 接入管理帧路径，受控端口等待
固件 PTK/GTK 确认和 IGTK 就绪。管理帧先复制到有界 CPU 队列，退出设备 RX 分发后
再执行认证与密码学运算，防止递归消费同一描述符。

## 扫描、重连与漫游

缺少有效扫描条目时，连接请求先发送指定 SSID/BSSID 的主动扫描；收到匹配的
probe response 后才创建 peer。支持连接中扫描并设置回到当前频道的驻留间隔。
取消扫描等待固件完成事件；PREEMPTED/RESTARTED 不会提前归还固件所有权。
排队或扫描阶段取消连接会把关联结果从 pending 结束为失败并通知框架；
结果结束与固件扫描归还分别处理，不能因 station 尚为 IDLE 而永久等待关联完成。
扫描结果及 link/association 通知按 20H392 的字段布局生成，BSD 通知在锁外发送。
网络层就绪但尚未关联时，能力、状态和关联结果查询不依赖有效 peer；PHY 能力仅声明
已实现的 11a/b/g。扫描网络列表接受目标 ABI 的零长通配 SSID，并在排队前合并重复项。
旧 AP 的 GTK 更新重新进入 RUN 时，不会把仍在扫描或已经失败的另一网络请求
误报为成功，也不会重复发送关联完成事件。

连接成功后保留当前会话凭据以自动重连；重试最多三次，显式断开、关闭 Wi-Fi、
最终失败或停止驱动时擦除。每 30 秒可扫描同一网络，在当前 RSSI 不高于 -70 dBm、
候选 AP 至少强 12 dB 时重新关联。用户明确指定的 BSSID 不被自动切换；临时漫游
目标不会覆盖原有配置。此处为断开后重新认证的基本漫游，未实现 802.11r 快速切换。

## 数据传输与释放

`giulia_wifi_dp.h` 使用 TCL→WBM 发送和 RXDMA→REO 接收。CE4 只承载 HTT 控制。
cookie 是有界整数，固件返回的地址和索引必须对应本驱动拥有的槽位；不把内核指针
放入设备 cookie 表。数据区包含 TX/RX 缓冲、link descriptor、REO 队列和管理帧。

普通流量使用 Ethernet 封装与 CCMP。EAPOL 使用独立 Native Wi-Fi bank，保持
802.11 Protected 位为零，保证 PTK 已安装时 M4 仍可明文发送。RX 校验长度、完整
MSDU、peer、解密状态和每密钥/每 TID 的 PN；不完整或不支持的帧不交给 BSD。
WCN7850 描述符 KEY_ID 保存低位密钥索引；专用 HAL 将它左移 6 位来重建空口 IV。
上一轮据通用注释把描述符再次右移 6 位的修改已纠正，以实际 HAL 实现为准。
GTK 1/2/3 分别检查自身安装状态和 PN，不能误用 GTK 0 的重放窗口。
PTK / GTK 的选择使用硬件报告的空口 addr1 / RA 组播标志，而非解封装后的 Ethernet
目的地址，覆盖 AP 将组播流量转为单播发送的情况。测试从固定 Linux v6.12 `hal.c`
直接提取 WCN7850 的 KEY_ID 转换和 MCBC 判断语句，避免仅按通用注释生成设备模型。
数据区同步还校验完整 4 MiB 动态子区已分配，
相对偏移不能回绕或跨入其他 DMA 分区。

网络栈的输出回调只复制到有界 CPU 队列；设备 worker 操作硬件。RX 在退还设备
ring entry 后排队，释放运行时锁再调用 `ifnet_input`，允许网络栈同步回调而不死锁。
EAPOL 遇到发送队列暂满时有界等待完成事件，真正提交后才推进握手；超时、断连
或设备错误保持失败，不伪造发送成功。HTT TX metadata 使用 WCN7850 的逻辑 pdev 0，
与硬件能力报告中的物理 radio ID 分开保存。
注册 XNU Ethernet demux、framer 和协议处理器，设置 6 字节地址与 14 字节帧头。
BSD 驱动回调显式处理地址启用、72–1500 字节 MTU、设备 MTU 范围和组播增删，
不再将这些 IP 配置请求交给目标 XNU 中固定返回 `EOPNOTSUPP` 的 `ether_ioctl`。
32 / 64 位媒体查询返回同一真实受控端口状态，仅接受自动媒体选择；设置地址不
伪造已关联或端口授权。接口置 DOWN 会立即关闭端口、清待发队列和会话凭据，
再排队断连。组播接收已由 HTT RX 过滤和 REO 路由订阅，BSD 维护实际组成员。
广播与组播使用各自的 mbuf 标志。关闭 Wi-Fi 或请求断连立即关闭数据端口，再由
worker 完成管理命令和资源回收；旧接收包和密钥不会留到下一次连接。

FW 来源的 TX 通知仅在上游明确归还 DMA 的 OK / DROP / TTL / REINJ / INSPECT
状态下回收缓冲。MEC_NOTIFY 和未知状态即使碰巧带有有效 cookie，也不能证明
设备已结束访问；驱动隔离数据区并保留槽位和 pending 计数，直到确认总线停止。

速率样本来自硬件而非 AP 能力：TX 只读取带 VALID 的 TQM ACKED / REMOVE_TX 完成，
RX 只记录本 peer 的完整、安全检查与接收回调均成功的帧。TX 与 RX 独立保存原始
字段、kbps 和时间；选择 peer、unmap 或 stop 后复位。固定原厂码表解码 20 MHz
11a / 11b（含短前导），HT / VHT / HE 或无效编码返回 unknown，不从缺失的 NSS 猜速率。
GET8 已按同版原厂 getter 的整数 `/1000` 及 Family scalar 拷回路径接入真实 TX 样本，
单位为整数 Mbps，5.5 Mbps 截断为 5；无有效样本时返回 NotReady。
GET57 在当前实际协商的 legacy 模式下返回原厂包装的兼容默认 0，这不是有效 HT MCS0。
扫描 BSS 的速率数组也转换为相同的整数 Mbps；原始 IE 与关联速率不变。

重连等待 peer delete/unmap 和管理帧 TX 确认，再通过 REO 命令环使每个队列失效，
等待真实状态、老化间隔及 cache flush 完成，排空 RX/错误/归还环后复用队列区。
不再依赖消耗 144 个一次性队列区；错误、超时或迟到状态不能授权复用 DMA。

## 当前限制

- 单个 infrastructure station；开放网络、WPA2-PSK 或 WPA3-SAE，CCMP-128；20 MHz 传统速率。
- 尚不支持 WPA1/TKIP、企业 EAP、AP 模式、802.11r、SAE group 19 以外的组、
  SAE password identifier/SAE-PK、GCMP 或 BIP-GMAC；未将这些能力标成可用。
- 仅使用固件 CN 规则允许主动发送的 2.4/5 GHz 频道；不启用 DFS、6 GHz 或缺少
  环境依据的室内限定频道。
- 主机测试不能验证射频、实际固件事件顺序、物理 DMA 一致性或 iOS 的完整联网流程。
  仍需设备确认固件就绪、扫描、关联、DHCP、DNS、双向流量与断线重连。

## 离线验证

```bash
bash scripts/test-ios16-wifi-runtime.sh
python3 tests/ios16_wifi_dp_oracle.py
python3 tests/ios16_wifi_station_oracle.py
python3 tests/ios16_wifi_apple_oracle.py
python3 -m unittest discover -s tests -v
git diff --check
```

主脚本包含原传输与映射测试、station、data-path、iOS association、扫描、WPA2、SAE 与 PMF 测试，
并直接提取当前生产运行时函数在有界 IOKit/BSD 桩上执行，启用 ASan/UBSan。
运行时覆盖实际 station→RSN→密钥 ACK→TCL 调用链，以及隐藏网络、连接中扫描、
漫游、三次重试、失败切换保留原网络和取消重连。数据面覆盖 174 个初始化/失败用例、
2,000 次 TX/RX 回收和 300 次 REO 失效/刷新/重连循环。全仓 unittest 本次运行
281 项，无失败，3 项跳过。C++14 头文件和 AArch64 `-O2`/`-O3` DMA 字节访问检查通过。
地址映射覆盖 100 项；另通过两个固定 Linux v6.12 布局校验。数据面 oracle 校验
10 个源码哈希、30 个结构、82 个宏、3 个 HTT RX 订阅位、3 段 DP 语义片段及原厂速率表；密钥归属回归
覆盖空口 RA 与 Ethernet DA 不同的两种情形。实际运行时还覆盖绑定时零 payload 的
驱动版本查询、替换连接失败后的迟到 RUN、真实 GTK 更新不重复发关联通知、取消
连接后的终态，以及实际 BSD ioctl 的 IP 配置、MTU 边界和接口 DOWN 回调。
station oracle 校验 31 个结构和 71 个常量，覆盖五条 ACTIVE 命令及失败阻断，关联序列覆盖 CCK、混合、OFDM 与 5 GHz。
`ios16_wifi_network_traffic.inc` 将真实 RSN 握手、固件密钥 ACK、BSD 地址启用、
TCL/WBM 完成、RXDMA/REO 和 BSD 输入串成组合回归。ARP、DHCP / DNS 格式的
IPv4 UDP 和 IPv6 帧均保持字节不变，重放帧被真实 PN 检查拦截，接口 DOWN 立即停发。
固件 DMA 和 BSD 输入边界仍由主机模型提供，测试不运行完整 IP 栈或声称手机已经取得 IP。
加密测试依赖主机 OpenSSL 开发库；SAE 和运行时测试还使用固定提交的 BearSSL
缓存（默认 `.cache/wifi-sae-bearssl`，可用 `BEARSSL_SOURCE` 指定），不自动下载。
两个可选 oracle 脚本要求已有的固定 Linux v6.12
源码缓存；C 测试直接使用已保存的上游结构定义，不联网下载源文件。
另一个可选 `ios16_wifi_apple_oracle.py` 使用已有 20H392 dyld cache，直接核对
绑定、GET103、两种断开、关联结果、SAE、GET5 / 18 scalar ABI 和 GET371 消费者的原始指令字节；不依赖反汇编库，也不下载镜像。
GET371 与 GET21 共用 Apple 关联结果表，已映射；GET5 已绑定显式 ACTIVE 策略。
同版原内核的额外生产者对照已确认 GET8 整数 Mbps、GET57 legacy 兼容默认值，
并直接证明 GET18 / SET18 在原驱动也返回 Unsupported，而非某个已知模式值。
INT_MIT 保持原名，不再把它推断为 IRQ 中断缓解；没有用 CE polling 伪造响应。
可选命令 `python tests/ios16_wifi_apple_oracle.py --stock-kernel out/iphone16-audit/kernel.raw`
会另外验证固定 SHA-256 原内核的 dispatcher / vtable / getter / Family 包装链。

`scripts/ios16_wifi_runtime.py` 现在识别新编译的 station profile，不再将其固定标成
仅被动扫描。旧扫描内核保留原报告格式；固件段内的同名字符串不能升级能力声明，
所有报告仍标记 `hardware_verified=False`。四项合成 Mach-O 元数据回归已加入主脚本，
本轮没有执行内核构建或镜像打包。

BSD 接口依据 Apple 的 [XNU Ethernet 声明](https://github.com/apple-oss-distributions/xnu/blob/xnu-8792.61.2/bsd/net/if_ether.h)
和 [if_fake 接口实现](https://github.com/apple-oss-distributions/xnu/blob/xnu-8792.61.2/bsd/net/if_fake.c)，
无线字段依据 [Linux v6.12 ath12k](https://github.com/torvalds/linux/tree/v6.12/drivers/net/wireless/ath/ath12k)。
