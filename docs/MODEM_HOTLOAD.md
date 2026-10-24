# 蜂窝常驻控制与热加载边界

[English](MODEM_HOTLOAD.en.md)

2026-10-24：本页描述新的源码接口和待统一验证步骤。原生 MPSS provider 尚未注册，
当前内建蜂窝运行时不能整体热替换。`gmr_runtime` 的 pending/event/storage payload 已超过
NLH 的 64 KiB 状态上限，并且包含回调和在途身份；它、worker、锁、IRQ、DMA、SMMU、固件及资源账本必须常驻。

固定上游、固件、配置缺口和完整执行链见[全链路入口](MODEM_BRINGUP.md)、
[启动映射](MODEM_BOOT_MAP.md)和[硬件映射](MODEM_HARDWARE_MAP.md)。本次不修改 UEFI。

## 已实现的常驻入口

内核契约位于 `ios16/modem/giulia_modem_platform.h`，都访问同一个 GiuliaModem 服务：

| 入口 | 实际语义 |
| --- | --- |
| `giulia_modem_get_snapshot(out)` | 在服务锁内复制一次完整软件快照；不访问硬件 |
| `giulia_modem_submit(command)` | 内核类型化业务队列；复制命令并保存 epoch/lifecycle/slot/bearer 代次 |
| `giulia_modem_control(operation, expected_epoch)` | 新增按值诊断控制；支持 `GMP_CONTROL_START/STOP/SUSPEND/RESUME`，与终端共用检查和队列 |
| `giulia_modem_terminal(command)` | `modem.status/start/stop/suspend/resume` 及纯解析fixture `modem.proto` |

`control` 要求服务在线、worker 可用、`giulia_debug=1`、modem mode 非零以及精确的当前 epoch。
START 还要求原生 provider、OFF、无未决资源与操作，且没有先前排队命令；没有 provider 返回
`GMR_UNSUPPORTED=9`，不增加 epoch。`GMR_OK=0` 只表示入队接受，完成仍来自同一 worker 和真实 provider 事件。
其他结果使用 `gmr_result`，不能直接当成系统 errno。

排队命令在 worker 执行前复核接受时的身份。RF_MODE 保存实际 selected_slot；START 保存 epoch。
STOP 停止当前所有者，并取消尚未执行的 START 和 mode 2 自动启动；SSR 排空期间仍可用 STOP 取消自动重启。
STOP 使用独立屏障，不受普通队列满影响；它取消未执行的业务/SSR 命令，并记录取消数。
worker 在处理计时器、poll 完成或 RX 投递前应用屏障，不能让 SSR 的末级完成先重启。
公开业务队列的 SSR 也在接受和执行时拒绝缺失 provider。
这不是单独的硬件状态机，也不重新初始化 gmr；已发行且效果未决的启动仍会隔离保留资源。

provider `submit` 的有效非零结果表示该动作未被接受且无硬件操作；worker 经 `gmr_reject` 精确退休，
不增加未知效果。已提交后的失败由 `poll` 报告；违反返回值契约的 provider 按未知失败处理。
已拥有的资源继续保留隔离，不能把拒绝当成已完成回滚。

DATA_REMOVE 和全局 DATA_DRAIN 均要求 NETWORK/RX/TX/DMA 四项完成，才能清承载并继续停止。
IPC_STOP 不释放后续 FIRMWARE_STOP 所需的 SMP2P/sysmon/SMEM/stop-ack。非 ONLINE 不接受新 RX，
worker 投递前再次复核身份。短信在换卡、关闭 RF、SIM 变化和 SSR 中保留未决身份，不自动重发。

## 已接入的 QMI 解析切口

源码在真实回包链的 `gmr_qmi_complete` 调用常驻 `qmi_check`，再通过公共
`extern "C"` / `noinline` 入口 `giulia_modem_hotload_decide` 调用
`giulia_hotload_select` 选择 `giulia-modem-proto`。目标产物必须核实生产路径到该外部入口、
以及入口到select的真实ARM64 B/BL；源函数存在或fixture调用本身不能替代链接证明。
hotload关闭时同一调用路径经公共头返回不可用并采用原生baseline，不引入select链接依赖。
仅完整QMI framing/result验证可替换；
QRTR来源、pending身份、epoch/generation、详细UIM/NAS/WDS解析和状态更新仍常驻。
模块拒绝影响实际处理；模块OK也不能跳过常驻详细解析而生成SIM/网络成功。

GH ABI 1：domain4、operation1、interface0x47480001、state1、kind=service、无依赖。
请求前4个u64为kind/txn、length/message、opid、origin；其余为完整payload的LE字节副本。
origin0为真实provider，origin1为fixture。最多224字节完整报文，超过上限整包原生fallback，不截断。
响应一个u64：低8位 `gmq_error`，位8..23 `remote_error`，其余0。来源和代次不由模块重写。

`giulia_modem_proto_module.c` 是独立NLH2编译入口，不编入常驻翻译单元。
private state56B、迁移snapshot64B，仅纯计数和running；START/STOP真正启停私有逻辑，
EXPORT/IMPORT不迁移硬件、指针或请求。`NLH_FAIL_START/IMPORT/STOP` 可构建生命周期失败场景。
USB CALL参数0..5只读calls/native/fixture/rejected/remote/imports。

`modem.proto` 使用22个独立手写golden/长度边界fixture经过同一adapter，不注入gmr事件。
有效模块应passed22、failed0、module_delta20、fallback_delta2；两个225B输入必须整包fallback。
未加载/卸载后应module_delta0、fallback_delta22，仍22/22正确。两种情况都必须native_delta0、
state_unchanged1、hardware_event0。fixture成功不能代替实际MPSS回包。

本模块只读使用公共GH契约，没有修改共享NativeHotload、platform、terminal、装配或构建文件。
统一验收方负责桥前置注册、构建、摘要绑定、终端能力清单及新内核临时启动。
当前旧镜像不会因电脑脚本更新而拥有这些入口，目标构建/设备结果仍待统一验证。

## 公共桥和控制权限

桥必须以实际 image/version/phase 身份授权，不能只按 slot 名称白名单授权硬件写。
普通 NLH_CALL 的整数必须保持按值；不能把 `gmr_runtime*`、snapshot、命令、包或文件缓冲指针编码进去。
GH只允许本次真实resident CALL的ticket读写有界纯值邮箱；USB/依赖CALL、生命周期callback和过期ticket不能访问它。
常驻调用点持服务锁保存相同epoch，返回后复核epoch和每个响应字段；原生baseline不能被模块放宽。

本阶段GH不调用 `giulia_modem_control`，解析模块没有硬件控制权。类型化内核控制入口仍检查操作和expected_epoch。
PROBE/EXPORT 不可触发硬件，IMPORT 仅改私有状态；当前控制接口只回报接受，没有每调用完成 ticket；
多请求关联或 payload 操作需要先实现常驻请求对象/回执，不能从 `last_opid` 猜测完成。

热加载模块只能保存私有纯计算/策略状态。更新时关闭新调用并排空引用，再 EXPORT/IMPORT；
常驻资源、代次和在途操作不迁移。任何需要重载物理执行层的操作，都必须先取得真实远端停止、
持久化、网络撤销、DMA 和 IRQ 排空及内存回收证明；未知状态保持隔离。

## 主控统一终端验收

串口和统一自动化由第五个热加载验收聊天独占。本页命令没有在蜂窝聊天执行。先在当前镜像查询：

```text
features
modem.status
hl.status
```

现有实测为 modem OFF、epoch 0、provider 0、mode 0；`hl.status provider=0` 是 NLH 平台桥状态，
与 `modem.status provider=0` 的 MPSS 执行者是两个不同接口。更新电脑脚本不会更新内核。

1. 整组源码完成后，由统一验收方按用户新增授权执行必要自动化、模块边界检查、完整目标构建和产物摘要核验。
2. 临时启动包含常驻修复的诊断内核，启用 `giulia_debug=1 giulia_modem=1`。不刷分区或清 userdata。
3. 无 provider 时执行 `modem.status`、`modem.start`、`modem.status`：必须拒绝为 9，OFF/epoch/资源不变。
4. 在新内核/模块上执行 `modem.proto`，按上述22个case和module/fallback差值区分原生与热加载。
   再验收旧epoch/ticket拒绝、v1→v2迁移、START/IMPORT失败回滚、卸载回退及STOP失败隔离。
   独立验证常驻STOP取消未执行START/SSR；硬件控制完成须读真实事件/日志。
5. 原生 provider 完整后验证冷启动/ready、QRTR、RMTFS、SIM/注册、WDS/IPA/GSI/SMMU 收发、
   短信对账、休眠/恢复、SSR、停止和资源回收，并与其他硬件并发场景一起采集。
6. 终端完整硬件链实机成功后才进入 UEFI 集成。

目前没有包含本次修改的新内核或蜂窝实机通过记录。源码修复和常驻入口不代表 MPSS、iOS 蜂窝或热加载已可用。
