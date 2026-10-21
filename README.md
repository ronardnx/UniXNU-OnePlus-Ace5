# UniXNU giulia — OnePlus Ace 5 UEFI bring-up

当前实机调试使用 Darwin 22.6.0 / iOS 16 内核。已验证 UEFI 交接、XNU
内存管理初始化、平台驱动注册、CPU0 注册及 SGI1 自测、原生 UFS 读取、
USB CDC 调试串口、userdata 的 HFSX 根卷读写挂载，以及 launchd 和系统服务启动。
GUI 内核已显示苹果标志与进度条；串口观察到 7.7 万次画面提交，SpringBoard / backboardd 未见崩溃。
尚未进入可操作界面；正在定位安装服务阻塞，触控芯片身份和触点报文已验证。

## 当前整合修复包（2026-10-21，查询接口已生效，迁移仍等待）

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
