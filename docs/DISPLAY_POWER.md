# 物理电源键与显示电源

2026-10-23：一加 Ace 5（PKG110）的物理电源键熄屏、再次亮屏已实机通过。
用户确认“能熄屏，也能恢复亮屏”，串口记录 6 次成功显示电源请求。

## 修复内容

之前按键的按下／松开已送入 HID，但 framebuffer 对 iOS 的显示电源请求
直接返回成功，未关闭面板。用户观察到按键后锁屏仍可见且颜色异常。

现在 `IOMobileFramebuffer` selector 12 将 0 映射为关闭显示、非零映射为开启显示，
通过继承的 DSI0 控制器发送 DCS `0x28`／`0x29`。关屏后暂停 framebuffer 呈现，
亮屏后标记重新刷新。调用与屏幕复制串行化，检查命令完成和 ACK，并恢复借用的
控制寄存器；超时或状态不确定时报告错误，不虚报成功。

代码入口为 [`giulia_display_power.inc`](../ios16/display/giulia_display_power.inc)，
底层事务为 [`giulia_display_power.h`](../ios16/display/giulia_display_power.h)，
GUI 补丁由 [`ios16_display_power.py`](../scripts/ios16_display_power.py) 生成。

## 实机配置与证据

- 内核提交：`3b6b4e1afa6bb371396a18b87e8a60d927740c41`。
- 成功构建：[39927732244](https://github.com/tycykp/UniXNU-OnePlus-Ace5/actions/runs/39927732244)。
- GUI 按键最小配置：触控、Wi-Fi、GPU、音频可选驱动关闭。
- 使用已有根卷，以 `fastboot boot` 临时启动；本次没有刷写分区。
- 镜像 SHA256：`0de1632447af42a7ab3a9f02490222f32a9a5ffc53414cf388d6af9d802b06aa`。
- 完整事件及内核摘要见 [`sm8650-display-power.json`](../hardware/sm8650-display-power.json)。

实测关屏为 `request=0 known=1 on=0`，亮屏为 `request=1 known=1 on=1`；
6 次请求均为 `error=0 ack=0 frozen=0`。这项结果仅覆盖面板显示关闭／开启，
不证明完整 SoC 休眠、面板电源轨关闭或电源键长按行为。
音量键界面响应此前已确认；三段式仍待传感器读取和档位标定。

## 验证方法

实机使用包含上述修复的 GUI 内核，采集 USB 串口后短按电源键、等待约 3 秒、
再短按一次。确认屏幕完全熄灭并恢复亮屏，同时检查 `GIULIA DISPLAY POWER:`
的请求值、`on`、`error`、`ack` 和 `frozen`。`keys.status` 只证明按键驱动状态，
不能替代面板响应和用户的目视验收。
