# 音频协议热加载接入

[English](AUDIO_HOTLOAD.en.md)。本轮尚无新目标构建或设备验收结论。

`giulia-audio-proto` 是 GH ABI 1 的纯协议 service，接口 `0x47480001`、state ABI 1、
无依赖。它分类实际 SPF/APM 接收包；ADSP、IRQ、DMA、固件、图与功放保持常驻。
当前缺少 native/PCM provider，协议模块不能使实际音频播放变为可用。

| 固定上游 | 实际调用点 | 模块与常驻责任 |
| --- | --- | --- |
| OnePlus audio-kernel `453d8875...` 的 `gpr-lite.h/c`、`spf-core.c` | `gat_poll_one` → `gat_spf_receive` | `gapt_classify` 分类；transport 关联当前 epoch/token 后采用 |
| AudioReach `76d04ffb...` 的 `gsl_main.c`、GPR `gpr_packet.h` | `gar_start` → SPF 查询／响应循环 | BasicRsp 只记诊断；常驻控制真实时间、重试和故障隔离 |
| 同提交 `gsl_common.c`、`gsl_spf_timeout.h` | `gag_receive`、`gag_submit` | 模块判定 ACK；常驻检查 deadline，保留图状态和 DSP 错误 |

SPF 有独立于 GLINK 协商的5秒总窗，单次查询最多1秒，重试间隔50ms。
真实单调 `gar_ops.now_ms` 将 poll、热加载与调度时间计入 deadline。
GET_SPF_STATE 的 BasicRsp（包括 status=0）不能表示 ready；typed state!=1 也会重试。
每次重试使用新 token，过期查询在 poll 前后淘汰，晚到 ready 不会恢复旧事务。
APM OPEN/PREPARE/START-STOP/其他命令分别采用固定 GSL 的4/2/5.5/1秒时限。
clock rollback、fatal/SSR、lease 或 transport 故障不会变成成功。

## 按值请求

`giulia_audio_proto_wire.h` 固定17个 u64：ABI、operation、token、本地端口、远端端口、
原命令、实际包长、pending，以及9个按小端打包的 packet words。支持24–60字节头
和4/8字节 payload，最多68字节；client data 低4位合法，其余保留位须零。
公共 bridge 复制请求，模块只得到绑定本次 invocation 的 ticket。

单个 u64 响应低32位为分类（0 IGNORE、1 SPF_READY、2 SPF_STATE、3 SPF_BASIC、
4 APM_ACK、5 APM_ERROR），高32位为完整 DSP status。常驻只接受同一 baseline
结果或更严格的 IGNORE；非法输出、不可用或更新中的模块立即回退。
采用前再次验证 epoch、pending、token、opcode 和 deadline。

hardware epoch 来自真实 native lease，`gan_prepare` 绑定 transport；图 provider
在 CLOSED 时用 `gag_bind_epoch` 绑定自身 lease，未绑定的图不能发送。
NLH START/STOP 只启停模块私有逻辑，EXPORT/IMPORT 只迁移32字节计数。
完整硬件停止仍要求实际 PA/PCM/IPC/PAS/IRQ/DMA 完成与资源释放证据。

## 自动化验收入口

新内核的 `audio.status` 即使 mode 关闭仍返回缓存 service、固件、worker、provider 和
错误／清理字段；`audio.preflight` 区分关闭、固件无效、worker失败、busy等原因。
`local_quiesced` 不是硬件停止证明。

`audio.proto` 需要 `giulia_debug=1`，可在音频 mode 关闭或 provider 缺失时执行。
它对独立对象调用真实 `gat_spf_receive` 和 `gag_receive`，覆盖20个合成场景，
正常的通过位图为 `0xfffff`，模块正常运行时每轮有16次选择与采用。
精确计数要求 mode0或provider0且 `counter_exact=1`；live并发时计数可包含worker流量。
卸载时应有16次常驻回退；BAD_REPLY 变体应有16次拒绝及回退，位图仍全部通过。
输出始终标记 `synthetic=1 hardware_verified=0`。这项测试不写设备，不建立 lease。
fixture 使用最低逻辑 epoch 1或最近已发布的 native epoch；不能把域 high-water
人为提高到未来硬件 epoch 之上。旧epoch压力场景应安排在最后或重启后继续。

模块普通 CALL 0/1/2/3/4在公共 GET_META 拒绝后可只读私有 calls/imports/ignored/
APM-errors/version；有效 resident ticket（即使数值很小）始终优先走实际请求。
这不会授予 USB CALL 访问请求或硬件的权限。

专用故障构建宏为 `GAPT_FAIL_START`、`GAPT_FAIL_IMPORT`、`GAPT_FAIL_STOP` 和
`GAPT_BAD_REPLY`；数值0关闭、1开启。START故障在修改状态前返回，不代表覆盖
部分硬件启动清理。统一自动化由热加载聊天执行；上传、生命周期、协议合成包和
真实 DSP/PCM/发声必须分别判定。无 provider 时真实硬件项应为 BLOCKED。
统一套件使用 `giulia_audio_hotload_module.c` 与 `GH_MODULE_VERSION/GH_FAIL_*`，
标准失败宏已映射到对应 GAPT 实际分支。
