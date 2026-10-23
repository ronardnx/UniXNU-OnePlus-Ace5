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

## 关联与安全

`giulia_wifi_station.h` 依次等待 peer map、vdev start、开放认证响应、关联响应和
peer-assoc 确认；管理帧 DMA 缓冲保留到对应 TX 完成。断连等待 vdev stop、peer
delete/unmap，迟到事件不能提前确认另一个操作。无法确认设备停止时保留 DMA 资源。

`giulia_wifi_rsn.h` 处理 WPA2 四次握手、MIC、RFC 3394 GTK 解包、重放计数与组密钥
更新；SHA1/AES 由项目已有 XNU 加密接口提供。PTK/GTK 只有在实际固件密钥完成事件
后生效，普通 IP 数据只在受控端口打开后发送。重复握手不得重新安装相同密钥或重置
PN；GTK 更新完成后重新打开端口。早于 WMI 关联确认的 EAPOL M1 排队保留到 RSN 初始化。

SAE group 19 实现 H&P 与 H2E、commit/confirm、anti-clogging token、重试与超时，
采用固定版本 BearSSL P-256/SHA256。仅在确认成功后将 PMK/PMKID 交给关联和 RSN；
WPA3 四次握手使用 SHA256 PTK 派生及 AES-CMAC MIC，并验证 H2E RSNXE，拒绝降级。
WPA2 可协商 PMF，WPA3 必须启用 PMF。IGTK 从已验证 MIC 且解包成功的 KDE 安装；
PMF 的单播 CCMP、组播 BIP-CMAC/IPN 和 SA Query 接入管理帧路径，受控端口等待
固件 PTK/GTK 确认和 IGTK 就绪。管理帧先复制到有界 CPU 队列，退出设备 RX 分发后
再执行认证与密码学运算，防止递归消费同一描述符。

## 扫描、重连与漫游

缺少有效扫描条目时，连接请求先发送指定 SSID/BSSID 的主动扫描；收到匹配的
probe response 后才创建 peer。支持连接中扫描并设置回到当前频道的驻留间隔。
取消扫描等待固件完成事件；PREEMPTED/RESTARTED 不会提前归还固件所有权。
扫描结果及 link/association 通知按 20H392 的字段布局生成，BSD 通知在锁外发送。

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

网络栈的输出回调只复制到有界 CPU 队列；设备 worker 操作硬件。RX 在退还设备
ring entry 后排队，释放运行时锁再调用 `ifnet_input`，允许网络栈同步回调而不死锁。
EAPOL 遇到发送队列暂满时有界等待完成事件，真正提交后才推进握手；超时、断连
或设备错误保持失败，不伪造发送成功。HTT TX metadata 使用 WCN7850 的逻辑 pdev 0，
与硬件能力报告中的物理 radio ID 分开保存。
注册 XNU Ethernet demux、framer 和协议处理器，设置 6 字节地址与 14 字节帧头。
广播与组播使用各自的 mbuf 标志。关闭 Wi-Fi 或请求断连立即关闭数据端口，再由
worker 完成管理命令和资源回收；旧接收包和密钥不会留到下一次连接。

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
python3 -m unittest discover -s tests -v
git diff --check
```

主脚本包含原传输与映射测试、station、data-path、iOS association、扫描、WPA2、SAE 与 PMF 测试，
并直接提取当前生产运行时函数在有界 IOKit/BSD 桩上执行，启用 ASan/UBSan。
运行时覆盖实际 station→RSN→密钥 ACK→TCL 调用链，以及隐藏网络、连接中扫描、
漫游、三次重试、失败切换保留原网络和取消重连。数据面覆盖 152 个初始化/失败用例、
2,000 次 TX/RX 回收和 300 次 REO 失效/刷新/重连循环。全仓 unittest 本次运行
271 项，无失败，3 项跳过。C++14 头文件和 AArch64 `-O2`/`-O3` DMA 字节访问检查通过。
加密测试依赖主机 OpenSSL 开发库；SAE 和运行时测试还使用固定提交的 BearSSL
缓存（默认 `.cache/wifi-sae-bearssl`，可用 `BEARSSL_SOURCE` 指定），不自动下载。
两个可选 oracle 脚本要求已有的固定 Linux v6.12
源码缓存；C 测试直接使用已保存的上游结构定义，不联网下载源文件。

BSD 接口依据 Apple 的 [XNU Ethernet 声明](https://github.com/apple-oss-distributions/xnu/blob/xnu-8792.61.2/bsd/net/if_ether.h)
和 [if_fake 接口实现](https://github.com/apple-oss-distributions/xnu/blob/xnu-8792.61.2/bsd/net/if_fake.c)，
无线字段依据 [Linux v6.12 ath12k](https://github.com/torvalds/linux/tree/v6.12/drivers/net/wireless/ath/ath12k)。
