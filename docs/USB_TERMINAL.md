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

这些命令通过 `prepare-ios16.py` 纳入 base/GUI 补丁；CMD 工具及守护进程源码也随 profile 输出。

## 电脑端调试命令

| 命令 | 功能 |
| --- | --- |
| `:help` | 显示完整电脑端帮助 |
| `all` | 依次发送所有基础诊断查询 |
| `collect` | 基础查询 + RAM 日志重放 + GUI sample，15 秒后保存报告；不保证所有异步响应均已返回 |
| `logs wifi` | 查看已收到的 Wi-Fi / PCIe / MHI / QMI / WMI 日志 |
| `logs gpu` | 查看 GPU / GMU / Adreno / SMMU 日志 |
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

需要包含 GSC1 屏幕转发的新 GUI 内核，并启用 `giulia_debug=1`。`features` 会列出 `screen.start`、`screen.stop`、`screen.snapshot`。在同一个终端连接中执行：

```text
ace5> screen.start
ace5> wifi.status
ace5> screen.snapshot
ace5> screen.stop
```

`screen.start` 自动打开电脑默认浏览器中的本地实时预览窗口，地址绑定 `127.0.0.1` 的临时端口，也会打印可复制的 URL。窗口持续显示手机真实 framebuffer，目标上限为每秒 1 帧，实际速度取决于 USB 和日志流量；同时可在原终端继续发送诊断命令。传输按块与日志交替，不需要第二个串口连接。`screen.stop` 在块边界停止并保留最后一帧；退出电脑工具或进入 shell 也会停止屏幕传输。

也可从 CMD 一次启动持续投屏，Ctrl+C 结束：

```bat
scripts\debug-terminal.cmd --command screen.start --seconds 3600
scripts\debug-terminal.cmd --command screen.snapshot --seconds 10
```

每次完整且通过 CRC 校验的帧更新会话目录的 `screens\latest.png`，适合直接查看或分享；`screen.snapshot` 另外保留 `screens\screen-帧编号.png`，预览窗口也提供保存最新 PNG 的链接。连续投屏只更新 latest，不会持续堆积图片。`screens\screen.json` 记录帧编号、UTC 接收时间及源尺寸，窗口显示最近接收时间，避免把旧画面当成新帧。

画面为原生 1264×2780 面板每 4 像素采样后的 316×695、RGB565 图像，不包含声音或远程触控。读取与 CPU 合成共用显示锁，复制完成后释放锁，再通过 CDC 发送；没有 framebuffer 时只报告 unavailable，不生成替代画面。早期引导尚未注册显示驱动、用户态直接写默认面板 surface 等情况可能尚无帧或出现撕裂，实际显示和帧率需新内核实机核实。

GSC1 每块带长度、帧编号、尺寸和 CRC32，整帧另有 CRC32；最大图像 439240 字节，块负载不超过 1024 字节。损坏／丢字节的数据不会输出到终端文本，也不会保存为有效截图；下一校验包或 `screen.stop` 的校验控制记录恢复解析。PTY 的 BEGIN/END 标记内不解析屏幕魔数，避免损坏 shell 原始输出。

离线验证：`bash scripts/test-ios16-screen.sh`，包含真实 C 发送格式到 Python/PNG 的联测、连续预览更新、CRC/丢字节、停止恢复、USB 任意分片及 framebuffer 取样边界。离线模型通过不等于手机投屏已验证。

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
重启或重试已停止的 Wi-Fi 驱动；`wifi.status` 显示这个等待门的状态。

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

按实际寄存器文档选定地址、宽度、掩码后使用 `reg.read` / `reg.write` / `reg.rmw`。可查本仓库 `ios16/giulia_wifi_link.h`、`ios16/giulia_gpu_power.h`、`ios16/giulia_touch.c` 及板级 DTS 的地址和电源时序。每条命令上限仍为 63 个 ASCII 字节；64 位地址和数值建议用十六进制缩短。

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

仍不包含 GDB/LLDB 单步、自动驱动暂停/重启、DMA 内存分配、任意 RAM 访问、基带协议栈或 GPU 作业提交器。寄存器接口提供操作能力，具体硬件调试仍需遵守相应硬件时序。
