# CMD USB 交互调试（协议 2）

协议 2 的诊断查询、MMIO 寄存器访问和 `reboot.fastboot` 已编译并通过实机测试。
更新电脑工具不会让旧内核自动拥有这些能力。PTY shell 尚未实机验证，仍需要
根卷内的守护进程和真正的 iOS ARM64 shell/工具包。

## 启动

在 Windows **CMD** 中执行（不调用 PowerShell）：

```bat
cd /d E:\uniunx\UniXNU-giulia
python -m pip install -r scripts\terminal-requirements.txt
scripts\debug-terminal.cmd --list
scripts\debug-terminal.cmd
```

依赖只需安装一次。本次开发环境已安装 pySerial 3.5。指定端口：

```bat
scripts\debug-terminal.cmd --port COM21
```

自动识别当前在线的 `1209:0001 / ACE5DBG001`，不会使用离线注册表记录。手机启动参数需包含 `vermeer_usb=1`，现有打包脚本已设置。关闭其他串口采集程序后连接。当前上游 USB 驱动在配置后发生断线/总线复位时会停机，通常需要重启手机才能重新连接。方向键上下查阅历史，退格编辑，回车发送；诊断模式中 Ctrl+C 或 `exit` 退出电脑工具。

## 命令覆盖与版本

**已有内核可用**：`help`、`ping`、`status`、`replay`；GUI 内核包含采样补丁时支持 `sample`。它只采样 `sweetsample` 启动参数指定的进程，默认 SpringBoard/backboardd；现有采样线程有生命周期限制，返回 0 表示当前不可用。

**协议 2 内核命令**（旧内核不会因更新电脑工具而自动支持）：

| 命令 | 输出及限制 |
| --- | --- |
| `features` | 命令清单与限制 |
| `info` | 板型、SoC、内核版本、诊断协议版本 |
| `uptime` | 内核单调时间换算的毫秒数 |
| `memory` | 启动 RAM 范围、页大小、空闲/活动/非活动/有线页计数；并发近似值 |
| `ps` | 进程 PID 与名称，最多 128 条，达到上限会标注 |
| `bootargs` | 诊断相关整数启动参数是否存在及数值；不是完整启动命令行 |
| `display` | 启动 framebuffer 宽、高、步长、位深；不是当前显示健康检查 |
| `usb` | CDC 状态、事件、setup、包大小、收发与日志计数 |
| `ufs` | 驱动现有卷状态、可写标记、几何和设备号；锁忙时立即报告 busy，不额外访问磁盘 |
| `keys.status` | 新 GUI 内核中查询物理按键工作线程缓存的寄存器、消抖／投递状态、计数及错误，并显示 HID 服务是否就绪；命令自身不访问硬件 |
| `audio.status` | 音频映射阶段；当前明确报告 hardware_ready／playback／recording 为 0 |
| `audio.map` | 固定的 ADSP／LPASS、中断、SIA9177／WCD939x 与 DMA 映射证据；不读取或写入寄存器 |
| `gpu.status` | GPU 准备阶段、固件校验、RAM／未激活页表数量和最近一次观察；未编译或未启用服务时明确报告 unavailable |
| `gpu.observe` | 向 GPU 工作线程提交一次有前置门控的只读电源／SMMU 观察，随后输出状态；不使能时钟、电源或 DMA |
| `gpu.stack` | 只读推进 15 阶段计划并打印每阶段状态；策略固定 `device_writes=0`，不写任何寄存器 |
| `gpu.share` | 准备一次 primary share 事务；authority 缺少 RM 侧证明时返回 `refused`，share 状态保持为零 |
| `gpu.fence` | 尝试把已注册 provider 与冻结的 share 快照导入 CPU fence 状态机；未注册/未冻结时报告 skipped |
| `gpu.hfi.pin` | 固定诊断 DMA 缓冲到重启（仅 mode 3）；成功后不再允许改变其所有权 |
| `gpu.hfi.status` | 检查 17 个 HWSCHED 队列头，不推动协议 |
| `gpu.hfi.query` | 入队一次 warmboot 查询；门铃仍需手动 |
| `gpu.hfi.poll` | 有界收包并记录回复 |
| `gpu.hfi.log` | 读取 GMU 日志缓冲前 64 个字 |
| `gpu.gmu.status` | 报告 GMU load/start 状态，不访问硬件 |
| `gpu.gmu.load` | 把固件装入 TCM；前置条件是电源已准备、mode 3 且 SID 5 翻译已活跃 |
| `gpu.gmu.start` | 释放复位并等待 HFI ready；失败会 quarantine |

这些命令通过 `prepare-ios16.py` 纳入 base/GUI 补丁；CMD 工具及守护进程源码也随 profile 输出。

`keys.status` 需要重新构建 GUI 内核；base 内核明确报告 `compiled=0`。
`giulia_keys=0` 禁用物理按键线程，默认启用。状态码为 0 未启动、1 启动中、
2 轮询中、3 已禁用、4 故障后释放按键、5 已停止、6 线程创建失败。
`raw`、`stable`、`delivered` 的位 0／1／2 分别表示电源、音量加、音量减；
`events` 仅统计 HID provider 接受的边沿，`retries` 是 provider 尚未就绪或分配失败的重试次数。
字段分别以原子操作读取，属于近似快照；`buttons_service=1` 表示 provider 已发布，
不能单独证明 iOS 已响应按键。可用 `watch keys.status 2` 查询。

2026-10-23 已用临时启动的旧 r2 镜像记录用户确认的音量加两次、音量减两次、
电源两次寄存器变化；实测映射见 [`sm8650-keys.json`](../hardware/sm8650-keys.json)。
上述旧镜像记录仅证明寄存器映射。后续按键最小镜像已验证音量界面响应；
熄屏修复版已通过物理电源键熄屏／亮屏实测，详见[显示电源说明](DISPLAY_POWER.md)。
串口 `GIULIA DISPLAY POWER:` 记录实际请求、面板状态和错误；本轮 6 次请求均
为 `error=0 ack=0 frozen=0`。仅有 `keys.status` 的事件计数仍不足以证明面板已关闭。
三段式开关使用 AK09970 三轴霍尔传感器，
GPIO99 仅为中断脚，仍需完成传感器读取与位置校准。

新 GUI 内核另包含默认关闭的三段式诊断线程，只有 `giulia_slider=1` 且新 UEFI
通过 `gh=3` 传入原生供电及 I2C 时钟准备结果时才启动。线程先检查时钟与空闲
GENI／DMA，再临时取得 GPIO206／207，用开漏 I2C 校验 AK09970 `48 c1`。
发生错误后停止，尝试恢复传感器控制值与 GPIO 配置；不会猜测 PMIC 寄存器。
`slider.status` 只读取缓存，`slider.stop` 向线程提交停止请求。

实测传感器后，在每个物理位置保持拨杆静止，分别发送
`slider.calibrate up RADIUS`、`slider.calibrate middle RADIUS`、
`slider.calibrate down RADIUS`。RADIUS 必须根据实际读数变化选取，范围为
1–32767；三个窗口重叠时拒绝启用分类。没有三个有效标定点时位置为 unknown。
标定仅在本次启动有效，不使用未经实测的磁场正负作为位置依据。
`slider.hid 1` 才开启实验性 Telephony RingEnable 事件桥接，策略为 down 发出
ring=1、up／middle 发出 ring=0；iOS 的事件极性和实际静音效果仍需实机验证。
middle 不代表已实现振动，当前没有触觉驱动。`slider.hid 0` 停止后续事件投递。

音频查询随本轮源码纳入所有 profile，需重新构建内核；不需要额外 `--audio` 开关。
它们只显示静态映射，未进行实机音频验证，也不随 `all`／`collect` 自动执行。
来源、固件校验及后续驱动工作见[音频映射](AUDIO_MAPPING.md)。

后续的可选 `--audio` 内核和本地 `--audio-prepare` 打包会接入协调服务：
`audio.status` 改为报告运行时状态；`audio.preflight` 检查固件及平台 provider。
`audio.start`、`audio.stop`、`audio.test` 要求 debug 模式与完整原生 provider，
当前 provider 尚未实现，命令会明确报告缺失并阻止硬件操作。原厂播放图和驱动核心的
验证范围见[音频驱动进展](AUDIO_BRINGUP.md)。这三个操作命令不支持 `watch`。

GPU 查询需包含 `--gpu` 的新 GUI 内核及本地固件，并通过 `giulia_gpu=1/2`
启动服务。`gpu.status` 可用 `watch gpu.status 2` 定期查询；`gpu.observe`
必须手动请求，不支持定期轮询。命令只向静态邮箱提交请求，工作线程串行执行
观察；服务停止时先关闭邮箱并等待线程结束，再释放映射。所有当前状态仍为
`power_acquired=0 smmu_active=0 gmu_started=0 hardware_ready=0 acceleration=0`。

## 电脑端调试命令

| 命令 | 功能 |
| --- | --- |
| `:help` | 显示完整电脑端帮助 |
| `all` | 依次发送所有基础诊断查询 |
| `collect` | 基础查询 + RAM 日志重放 + GUI sample，15 秒后保存报告；不保证所有异步响应均已返回 |
| `logs wifi` | 查看已收到的 Wi-Fi / PCIe / MHI / QMI / WMI 日志 |
| `logs gpu` | 查看 GPU / GMU / Adreno / SMMU 日志 |
| `logs audio` | 查看已收到的音频映射／ADSP／LPASS／Codec 日志 |
| `logs touch` | 查看触控 / HID 日志 |
| `logs display` | 查看显示 / framebuffer / backboard 日志 |
| `logs ufs` | 查看 UFS / 根卷 / HFS 日志 |
| `logs usb` | 查看 USB 日志 |
| `logs process` | 查看进程 / launchd / 采样日志 |
| `logs panic` | 查看 panic / fault / crash 日志 |
| `logs power` | 查看电源 / 时钟 / PMIC / 电池相关日志 |
| `logs all` | 查看保留的全部会话文本 |
| `watch status 2` | 每 2 秒查询状态，间隔至少 1 秒；不允许周期 replay/sample |
| `unwatch` | 停止周期查询 |
| `save` | 立即保存分类日志和发送命令记录 |
| `clear` | 清屏，保留文件日志 |
| `exit` | 保存并退出 |

`logs` 是**历史日志筛选**，不是新的硬件采样。没有匹配日志不代表设备正常，也不能据此判断模块不存在。Wi-Fi、GPU、触控等驱动未运行或未输出日志时，终端不会伪造结果。

脚本化采集：

```bat
scripts\debug-terminal.cmd --command ping --seconds 5
scripts\debug-terminal.cmd --command collect --seconds 25
```

每次会话创建独立时间戳目录，默认 `out\terminal`。`device-*.log` 保留日志／PTY 原始字节，每 32 MiB 分卷；屏幕二进制包先分离和校验，再单独保存 PNG，避免污染文本日志。分类文本只保留最近 20000 行；`session.json` 记录发送的命令、接收字节数、屏幕帧数及解码错误，发送成功不等于手机执行成功。屏幕过滤控制字符，UTF-8 解码支持跨包字符。API 使用参照 [pySerial 官方文档](https://pyserial.readthedocs.io/en/latest/pyserial_api.html)。

## 实时投屏和截图

需要包含 GSC1 屏幕转发及远程 HID 输入的新 GUI 内核，并启用 `giulia_debug=1`。`features` 会列出 `screen.start`、`screen.stop`、`screen.snapshot`、`screen.configure` 和 `input.*`。更新电脑脚本后仍需重新生成、编译和启动新内核。在同一个终端连接中执行：

```text
ace5> screen.start
ace5> wifi.status
ace5> screen.snapshot
ace5> screen.stop
```

`screen.start` 自动打开电脑默认浏览器中的本地实时预览窗口，地址绑定 `127.0.0.1` 的临时端口，也会打印可复制的 URL。窗口持续显示手机真实 framebuffer，默认最长边 695 像素、目标上限每秒 1 帧；同时可在原终端继续发送诊断命令。传输按块与日志交替，不需要第二个串口连接。`screen.stop` 在块边界停止并保留最后一帧；退出电脑工具或进入 shell 也会停止屏幕传输并释放远程输入。

电脑端操作参考 [scrcpy 的控制方式](https://github.com/Genymobile/scrcpy/blob/master/doc/control.md) 和 [画面设置](https://github.com/Genymobile/scrcpy/blob/master/doc/video.md)。首次打开默认关闭「电脑控制」，收到手机 `input.status` 的真实能力报告后才能开启；触控、键盘和系统按键分别检测，没有服务的操作保持不可用。

| 操作 | 效果 |
| --- | --- |
| 左键点击、长按、拖动 | 单指触控，缩放和旋转后仍对应手机原始坐标 |
| 滚轮 | 转换为短距离单指滑动；支持横向滚动 |
| 右键 / `Alt+B` | 发送 Escape；iOS 是否返回由应用决定 |
| 中键 / `Alt+H` | 主屏幕 HID 按键 |
| 键盘 | 点击画面聚焦后转发物理键位、按下/松开和左右修饰键，由手机键盘布局解释 |
| `Alt+P` / `Alt+↑` / `Alt+↓` | 电源 / 音量加 / 音量减 |
| `Alt+F` / `Alt+←` / `Alt+→` | 全屏 / 向左旋转预览 / 向右旋转预览 |
| 画面设置 | 最长边预设或自定义 160–2780 像素，按原屏幕比例采样；目标帧率 1–30 |
| 适应窗口 / 缩放 | 仅调整电脑预览大小；不改变传输分辨率 |
| 截图 / 保存最新 PNG | 请求手机截图 / 下载最后完整接收的 PNG |

窗口失焦、隐藏、关闭、关闭电脑控制或输入队列溢出时释放远程触点和按键。浏览器心跳失效后电脑端关闭控制，设备端另有 3 秒输入租约；USB 断开、停止投屏和进入 shell 也释放输入。物理触控使用 0–9 号触点，远程触控保留 10 号，避免与手指争用。当前不含多指缩放、输入法组字、Unicode 文本注入、剪贴板同步、音频或 H.264/H.265 编码。

分辨率在设备采样端生效，保持当前帧传输完整后再应用下一帧。原生上限为 1264×2780；目标帧率是采样上限，窗口另显示实际收到的帧率和最近帧时间。当前仍以 RGB565 原始像素经 CDC 发送，提高分辨率会增加传输时间，30 FPS 选项不代表已达到 scrcpy 的编码视频性能。

也可从 CMD 一次启动持续投屏，Ctrl+C 结束：

```bat
scripts\debug-terminal.cmd --command screen.start --seconds 3600
scripts\debug-terminal.cmd --command screen.start --max-size 1280 --max-fps 5 --seconds 3600
scripts\debug-terminal.cmd --command screen.snapshot --seconds 10
```

每次完整且通过 CRC 校验的帧更新会话目录的 `screens\latest.png`，适合直接查看或分享；`screen.snapshot` 另外保留 `screens\screen-帧编号.png`，预览窗口也提供保存最新 PNG 的链接。连续投屏只更新 latest，不会持续堆积图片。`screens\screen.json` 记录帧编号、UTC 接收时间及源尺寸，窗口显示最近接收时间，避免把旧画面当成新帧。

也可以在终端执行 `screen.configure 1280 5` 修改最长边与目标帧率，`input.status` 查询服务，`input.release` 释放远程输入。浏览器只向串口线程提交有界命令，不直接打开第二个串口。HTTP 控制请求限制为本机、同源和本次窗口令牌；允许的操作为投屏设置与输入，不能经该接口发送任意诊断或 shell 命令。

读取与 CPU 合成共用显示锁，复制完成后释放锁，再通过 CDC 发送；没有 framebuffer 时只报告 unavailable，不生成替代画面。早期引导尚未注册显示驱动、用户态直接写默认面板 surface 等情况可能尚无帧或出现撕裂，实际显示、输入效果和帧率需新内核实机核实。

GSC1 每块带长度、帧编号、尺寸和 CRC32，整帧另有 CRC32；最大图像 7027840 字节，块负载不超过 1024 字节。默认 316×695 仍与旧 GSC1 帧兼容，旧电脑解码器需要更新才能接收更高分辨率。损坏／丢字节的数据不会输出到终端文本，也不会保存为有效截图；下一校验包或 `screen.stop` 的校验控制记录恢复解析。PTY 的 BEGIN/END 标记内不解析屏幕魔数，避免损坏 shell 原始输出。

## 真实 shell

数据通路：电脑 CMD → USB CDC → `/dev/giulia-debug` → `giulia-debugd` → PTY → shell 进程。设备节点权限为 root `0600`，只允许一个守护进程打开。`giulia_debug=1` 开启后才注册节点。PTY 使用 Darwin 的 `forkpty`，接口参照 [Apple PTY 文档](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/login_tty.3.html)。

```text
ace5> shell.size 120 40
ace5> shell
```

进入后所有按键发送给手机的 PTY，由真实 shell 解析：`cd`、变量、管道、重定向、脚本以及进程执行使用 shell 自身的语义。`ls`、`ps`、`cat`、`kill` 等外部程序是否存在取决于工具包，不是电脑端的模拟命令。

- Ctrl+C / Ctrl+D / Ctrl+Z 交给手机 PTY。Ctrl+] 结束当前 shell 会话并回到 `ace5>`；不是保留进程的 detach。
- shell 中的 `exit` 结束手机 shell，回到诊断模式。方向键、Home/End/Delete/PageUp/PageDown 转换为终端序列。
- 大小默认为 80×24，可在进入前用 `shell.size` 修改；目前没有自动跟随 CMD 窗口缩放。
- shell 输出保留 ANSI 终端序列及原始字节。该模式暂缓发送内核日志，退出后继续读取日志环。
- RX/TX 各有 32 KiB 队列，输出背压；输入溢出会终止会话，避免静默丢字节后执行错误命令。
- 会话结束、守护进程退出、DTR 撤销或 USB 失败会结束 PTY 会话；守护进程回收 shell。主动脱离控制终端的守护进程不保证被结束。
- 协议保留 `\r\nGIULIA SHELL BEGIN\r\n`、`\r\nGIULIA SHELL END\r\n` 两个模式标记，shell 不应原样输出它们；该终端不是任意二进制文件传输协议。

`daemon unavailable` 表示设备节点尚未由守护进程打开；`disabled` 表示缺少启动参数。若进程启动失败，检查 `/private/var/log/giulia-debugd.log`、代码签名和动态库依赖。

## 重启到 Fastboot

新调试内核支持 `reboot.fastboot` 和 `reboot.status`，需要 `giulia_debug=1`。
2026-10-23 已在 Ace 5 上验证正常卸载根卷并回到 Fastboot，见
[`sm8650-reboot.json`](../hardware/sm8650-reboot.json)。
命令由单独的内核线程执行，先核对 APPS 对 PMK8550 SDAM 的访问权限，写入并
读回 bootloader 原因，再调用 XNU 的正常进程退出、文件系统同步与卸载流程。
最后通过 PSCI SYSTEM_RESET 重启。SPMI 读写失败时不会发起重启。

交互模式输入 `reboot.fastboot`。脚本化测试可使用：

```bat
scripts\debug-terminal.cmd --command reboot.fastboot --seconds 90 --fastboot-serial YOUR_FASTBOOT_SERIAL
```

`YOUR_FASTBOOT_SERIAL` 使用 `fastboot devices` 中的设备编号。指定此参数后，
工具既要求内核确认已经完成关机流程，也等待该编号的设备实际进入 Fastboot，
超时或准备失败返回非零。正常重启引起的串口断开会保存日志并结束会话。
`collect` / `all` 不会触发重启，`watch` 禁止周期执行重启命令。

旧镜像不支持此命令，更新电脑脚本后仍需先临时启动包含该实现的新镜像。

## 真实寄存器读写

Wi-Fi 调试构建另支持 `wifi.status` 和 `wifi.start`。打包时指定
`--debug-terminal --wifi-terminal-wait`（并启用 `--wifi-pcie`），内核会先完成 PCIe
链路和 BAR 枚举，提前预留连续 DMA 内存，在 SID/SMMU 初始化和设备 DMA 启用前
等待 `wifi.start`。提前分配避免等待期间用户空间启动导致 64 MiB 连续内存分配失败。
这样可以先建立串口连接，
保留 SMMU 读写、失败寄存器及回滚日志。`wifi.start` 是一次性的继续执行命令，不会
重启或重试已停止的 Wi-Fi 驱动。新版 `wifi.status` 除了等待门，还显示硬件 worker
发布的启动快照：快照时间、worker 是否仍运行、MHI 阶段和 EE、QMI 子阶段与错误、
最后请求／通知、注册状态、固件返回的引脚结果、PDC 默认／覆盖命令计数，以及
QMP pending／保留状态。快照在轮询时最多每 100 ms 更新，协议收发和退出时立即
更新；worker 退出后仍保留最终快照。查询不读 MMIO，不持有运行时锁，不触发初始化。
`pin_valid` 的三个 bit 分别表示电源、PHY IO、RF 字段实际存在；位图不解释为通过／失败。
这些新增字段需重新构建内核；旧镜像仍只显示等待门。完整对应关系见
[一加启动链审计](WIFI_UPSTREAM_CHAIN.md)。

需要 `giulia_debug=1 giulia_mmio=1`。命令在内核使用 device MMIO 映射、指定宽度的 volatile load/store 和 ARM 屏障执行，结果来自实际访问，不来自历史日志。

| 命令 | 作用 |
| --- | --- |
| `reg.map BASE BYTES` | 映射物理窗口，不读取设备；最多 16 个窗口，每个最多 64 KiB |
| `reg.maps` | 查看映射编号、地址和长度 |
| `reg.unmap SLOT` | 释放指定窗口 |
| `reg.read ADDRESS WIDTH` | 单次 8/16/32/64 位读取 |
| `reg.write ADDRESS WIDTH VALUE` | 单次写入，不自动读回，避免影响 write-only / W1C 寄存器 |
| `reg.rmw ADDRESS WIDTH MASK BITS` | 读取后写入 `(old & ~MASK) | BITS`；BITS 必须在 MASK 范围内 |
| `reg.dump ADDRESS WIDTH COUNT` | 连续读取 1–64 个寄存器，执行前检查全部地址已映射 |

数值支持十进制或 `0x` 十六进制。访问地址必须按宽度自然对齐。映射必须按内核 `PAGE_SIZE` 对齐，先用 `memory` 查看页大小；用 16 KiB 对齐可同时满足本项目 4 KiB/16 KiB 页配置。地址允许范围为 `[0x00100000, 0x80000000)` 的低位外设区域，包含 `0x60000000` 的 PCIe 窗口；额外拒绝与交接 RAM 窗口重叠的映射。不是任意 DRAM 读写器。

下面仅展示映射 PCIe 根窗口的语法，**映射成功不代表 PCIe 已上电或可读**：

```text
reg.map 0x60000000 0x4000
reg.maps
reg.unmap 0
```

按实际寄存器文档选定地址、宽度、掩码后使用 `reg.read` / `reg.write` / `reg.rmw`。可查本仓库 `ios16/wifi/giulia_wifi_link.h`、`ios16/gpu/giulia_gpu_power.h`、`ios16/touch/giulia_touch.c` 及板级 DTS 的地址和电源时序。每条命令上限仍为 63 个 ASCII 字节；64 位地址和数值建议用十六进制缩短。

这里没有通用的时钟/电源自动接管或驱动锁协调：未上电、secure-only 或不支持该访问宽度的寄存器可能导致同步异常/总线挂死，不能保证能返回错误码；`reg.rmw` 不相对现有驱动原子化，也不能用于 W1C、读清零、FIFO 或 write-only 寄存器。修改 USB、自身存储或 SMMU 控制寄存器可能立即中断当前会话/系统。`write-issued` 只表示 store 与屏障已执行，不宣称硬件状态验证成功。

## 后续构建与安装步骤（本次未执行）

1. 在 macOS + iPhoneOS SDK 下显式执行 `sh ios16/userland/build-debugd.sh NEW_OUTPUT_DIR`，得到 ad-hoc 签名的 ARM64 `giulia-debugd`。脚本不会被补丁生成或当前工作流自动执行。
2. 准备真实、已签名的 iOS ARM64 shell，命名为工具目录的 `bin/sh`。其他工具放 `bin/NAME`，配套动态库放 `lib/NAME`。安装工具拒绝 macOS 程序、arm64e、无 CodeDirectory 的文件和符号链接。使用 `otool -L` 确认安装名和库依赖；安装工具不自动修改动态库路径，完整运行依赖仍需实机检查。
3. 对**离线根卷副本的挂载目录**执行以下命令，不要指向主机 `/` 或在线手机。它会安装文件、同时更新 LaunchDaemons plist 和 iOS 的 launchd 缓存；输入预检失败不会写根卷，写入中途发生 I/O 错误则应弃用该副本重新制作。

```sh
sudo python3 scripts/stage-debug-userland.py \
  --root /mnt/giulia-root-copy --daemon /path/to/giulia-debugd \
  --tools /path/to/tools --manifest /path/to/debug-userland.json
```

4. 生成后续内核补丁时传入清单，例如：

```sh
python3 scripts/prepare-ios16.py --reference ../reference-vermeer \
  --work NEW_SOURCE_DIR --out NEW_PROFILE_DIR --gui --touch \
  --debug-userland /path/to/debug-userland.json
```

清单中的精确 CodeDirectory 哈希加入 GUI AMFI 查询路径，仅在 `giulia_debug=1` 时生效；原有官方信任目录和内核代码页哈希校验保留。每次修改/重签 daemon、shell 或工具，都要重新生成清单及内核。没有清单的默认构建不会自动信任这些新增二进制。

5. 将更新后的根卷重新导出、检查并生成新的根卷报告，不能复用旧根卷 SHA256 报告。之后正常构建/打包时，为 `scripts/make-ios16-payload.py` 增加 `--debug-terminal`；需要 MMIO 时再加 `--debug-mmio`。打包器和验证器检查启动参数与内核实现标记对应。这里不提供自动刷机步骤。

## 当前验证范围

电脑协议解析、参数拒绝、跨 USB 包 shell 标记、原始字节保留、安装预检、launchd 缓存和信任清单均有离线 Python 测试。基础版/GUI 版仅生成补丁并检查语法，不代表 C 源码已编译或设备通路已工作。

仍不包含 GDB/LLDB 单步、现有静态驱动的自动暂停/重启、DMA 内存分配、任意 RAM 访问、基带协议栈或 GPU 作业提交器。寄存器接口提供操作能力，具体硬件调试仍需遵守相应硬件时序。

## 开发用原生模块热更新

生成 profile 时可传入 `--hotload-source ../UniXNU/kits/NativeHotload`，由装配层加入公共 NLH2 装载器及 USB 桥；默认不编入。完整构建后，打包增加 `--debug-terminal --debug-hotload`，对应 `unixnu_hotload=1`。首次仍需启动一次包含装载器的新内核。

GitHub Actions 的 `ios16-xnu.yml` 提供 `hotload=true` 输入，从已审查分支的
`ci-source/NativeHotload` 纯源码快照编译。Ubuntu 生成 15 个模块，macOS 编译
固定 XNU，再核对模块／常驻运行时源码摘要、Mach-O 函数定义及真实核心调用。
热加载构建只上传内核、模块、清单和日志，跳过 UEFI／启动镜像任务。

全模块内核在本地嵌入 Wi-Fi、GPU、音频固件后，可能超过原有 64 MiB FAT
容量。`make-ios16-payload.py --payload-mib 128` 可显式选择较大的容量，默认
仍为 64；生成和核验同时检查 FAT 分配、实际 DarwinBoot 的 128 MiB 内核文件、
254 MiB 虚拟跨度及 ADT 声明窗口中的 16 KiB 对齐布局。最终启动镜像可通过
`pack-payload.py --max-image-bytes N` 限制在已独立确认的目标分区容量内。
新 payload 必须更新 UEFI 中的大小／CRC 身份后重新链接；修改 DTB 属性本身
不足以更新固件编译期校验值。固件重链接输入证据与独立二进制身份核验分别记录。

`hl.status`、`hl.list`、`hl.core` 可在本终端查询；后者显示调度／VM／VFS 模块真实调用、回退和拒绝计数，切换期间也可读取。上传、按名称调用、更新、卸载使用公共 kit 的 `hotload.py`，操作前退出 shell／投屏并关闭其他串口进程。主机工具用 nonce 关联就绪查询、逐块确认偏移，完整摘要校验后才提交。断连丢弃未提交上传，已提交操作继续；结果不确定时查询状态，不重复提交。

NLH2 接受位置无关 C 模块，最多 16 个命名模块、每模块 8 项依赖，支持显式 EXPORT／IMPORT 状态迁移。已接入 `thread_quantum_init`、`vm_fault_is_sequential`、`cluster_max_io_size` 和 `cluster_max_prefetch` 的纯数值决策，对应 `xnu-sched`、`xnu-vm`、`xnu-vfs`。对象、锁、CPU ledger、VM CAS 和 I/O 资源保持常驻；更新期间立即回退原算法，模块私有计数迁移。公共 kit 的 `build-core.py` 生成三个域的 baseline／upgrade／失败场景，`hotload.py core` 读取计数，`call xnu-sched 0/1/2` 分别读取调用／改变／导入数（其他域使用对应名称）。

完整调度器、VM、VFS 及 Wi-Fi、GPU、触控等内建驱动仍需继续迁移。示例 counter／client 不访问硬件；尚无目标内核完整构建或实机热替换结果。停止／清理／回滚失败会隔离目标并关闭其依赖方入口，保留相关资源，核心路径继续使用原算法，不能强制卸载。完整接口、构建命令和核心／硬件迁移要求见公共仓库 `docs/zh/drivers/hotload.md`（英文 `docs/drivers/hotload.md`）。
