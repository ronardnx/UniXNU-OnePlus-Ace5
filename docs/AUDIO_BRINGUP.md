# ADSP 启动与扬声器播放核心

2026-10-23：在[板级映射](AUDIO_MAPPING.md)之上，已加入固定固件加载、PAS、
SMEM／GLINK／GPR、APM 图事务和双 SIA9177 控制核心，以及可选 XNU 协调服务。
这些组件已有历史离线验证记录。**当前仓库尚未提供完整的平台资源所有者和 PCM 后端，
因此不能在手机上完成 ADSP 启动或扬声器播放。** 此轮没有连接设备，也没有刷写。

## 已实现的链路

| 环节 | 实现 | 校验与失败处理 |
| --- | --- | --- |
| 原厂固件 | `ios16_audio_pack.py`、`giulia_audio_boot.h` | 56 个文件固定 SHA256；解析真实 ELF；DTB PAS36 在 ADSP PAS1 前；分段复制、BSS 清零、同步和认证；未知结果保留资源 |
| SCM/PAS | `giulia_audio_scm.h` | 固定 PIL 命令及 PAS ID、SMC64 探测、完整寄存器结果、busy／interrupted 处理；需要平台逐次验证授权与资源 |
| 电源规划 | `giulia_audio_power.h` | 从 CmdDb AUX 将 LCX／LMX 逻辑级别 384 转为原生索引，XO=3；生成规划，不直接写未知 RSC |
| 原生资源 | `giulia_audio_native.h` | 有效 lease 后处理固定保留内存、SMEM 硬锁、SMP2P 和 IPCC；不以地址可映射代替所有权 |
| IPC | `giulia_audio_transport.h` | SMEM 分区与 FIFO、GLINK 协商和 intent、GPR 请求关联；只有本次启动的新 ready 通知才通过 |
| 播放图 | `giulia_audio_graph.h` | 固定原厂图和基线校准哈希；APM OPEN／SET_CFG／PREPARE／START／STOP／CLOSE；逐笔验证 token、端口、域、原 opcode 和 DSP 返回值 |
| 双功放 | `giulia_audio_speaker.h` | 校验 `sipa.bin`，检测两个 SIA9177 ID、reset、init／startup／shutdown、efuse trim 和 CRC；无效 trim 阻止播放 |
| 协调服务 | `GiuliaAudio.cpp`、`giulia_audio_runtime.h` | 串行工作线程，先 ADSP／SPF，再已校准 PCM，再功放；停机先静音功放再停 PCM；异常保留状态供诊断 |

原生 IPCC SEND 地址为 `0x40600c`，GLINK 写 `0x00030000`，SMP2P 写
`0x00030002`。SMEM 硬锁3在 `0x1f43000`，AP owner=1；只持有本机普通锁不能
替代跨处理器硬锁。源代码依据见[传输层证据](../hardware/sm8650-audio-transport.json)。

ADSP 包为 41,603,072 字节，SHA256 为
`cbbbe5ad380a011a848677c1b57ed658ba4874833e84f95785743d5731b99f49`。
包和 SIPA 固件仅保存在本地；仓库只保存实现、布局与哈希。

## 原厂播放图与动态模块

已用固定的官方 AudioReach graphservices
[`76d04ffb`](https://github.com/AudioReach/audioreach-graphservices/tree/76d04ffb30caa1fe609b94f9f6081210cb962fc1)
构建主机端 ACDB 解析库，原厂 `acdb_cal.acdb` 初始化成功。
Python 解析的 781 个 GKV 已与官方 `GET_SUPPORTED_GKVS`／`GET_GRAPH` 对照。

低延迟、实例1、扬声器 MBDRC 图包含4个 subgraph；OPEN 属性为2792字节、
连接为120字节。默认校准为6400字节，按完整参数记录分成2批 SET_CFG。
其余21组校准选择也已导出供核对。默认校准只是基线数据，不代表完成每台设备的
扬声器电气校准、保护反馈或当前场景选择。

图包含 I2S sink `0x0700100a` 和 vendor 模块 `0x1700e900`。
后者由原厂 AMDB 记录关联至 `libsixth_rx_v3.so`、符号 `sixth_sia81xx_rx`。
已从 ODM RFSA 目录提取并核对该库；**DSP 文件服务、AMDB 加载及动态依赖仍需接入**，
不能只发送 APM OPEN 就认定图已可运行。

离线重建解析库与检查原厂 ACDB：

```bash
# CHECKOUT 必须处于上述固定提交；输出目录必须不存在。
bash scripts/build-audio-acdb-host.sh CHECKOUT NEW_OUTPUT_DIRECTORY
python3 scripts/ios16_audio_graph.py .cache/audio-speaker-stock/acdb_cal.acdb \
  --library NEW_OUTPUT_DIRECTORY/libacdb.so
```

`ios16_audio_graph.py --output` 只向显式指定的新本地目录导出专有图与校准数据，
需要同时给定完整 `--key` 选择。导出数据不能提交到源码仓库。

## 构建和命令

```bash
python3 scripts/prepare-ios16.py --reference PINNED_REFERENCE \
  --work NEW_WORK --out NEW_PROFILE --gui --audio
python3 scripts/make-audio-firmware-pack.py \
  --firmware-dir .cache/audio-firmware --out .cache/audio-pack/firmware.pack
```

CI 的 `audio=true` 与本地 `--audio` 对应，只编译协调服务和固定哈希。
完成目标内核构建及现有重定位检查后，使用本地打包工具：

```bash
python3 scripts/embed-audio-firmware.py \
  --kernel REBASED_KERNEL --pack .cache/audio-pack/firmware.pack --out NEW_KERNEL
```

该工具添加只读 `__AUDIO_FW` 段，并校验内核中编译的摘要；`NEW_KERNEL.json`
继承原内核报告。统一 payload 工具的 `--audio-prepare` 会校验该段并加入
`giulia_audio=1`。此开关只启动固件检查和协调线程，不自动执行 PAS／功放操作。

| USB 命令 | 行为 |
| --- | --- |
| `audio.map` | 固定板级映射，与上一轮相同 |
| `audio.status` | 启用协调服务后显示固件、provider、ADSP／SPF 和隔离状态 |
| `audio.preflight` | 查看服务状态，并对已注册 provider 执行只读前置检查 |
| `audio.start` | 要求 `giulia_debug=1`、固定固件和完整平台 provider，然后调用启动状态机 |
| `audio.stop` | 经 provider 停止所有 IPC 客户，再执行双 PAS shutdown；不强制清除不确定状态 |
| `audio.test` | 要求真实 PCM／MI2S／校准／模块就绪；播放250ms、1kHz、约-48dBFS测试音后按序停止 |

当前没有 provider producer，`audio.start`／`audio.test` 会报告
`native-platform-owner-missing` 并保持零硬件写入。没有任意寄存器命令、启动参数或
用户填入的标志可以绕过这项条件。`audio.start/stop/test` 不允许 `watch` 定期执行。

`giulia_audio_platform_register()` 是内核内部集成接口。调用者必须保持 callback、
固件、DMA backing 和 lease 的生命周期；它本身不会授予资源所有权。
原生 helpers 和 APM helpers 已实现，但**其完整 XNU provider 绑定仍未完成**。

## 2026-10-24 停止与热加载边界修复

真实 SPF/APM 分类调用点、纯协议 service、启动重试与自动化包验收入口已接入，
见[音频协议热加载](AUDIO_HOTLOAD.md)（[English](AUDIO_HOTLOAD.en.md)）。
这些新改动仍需第五热加载聊天统一完成目标构建和设备验收。

本轮保留功放 probe/start 内部回滚的首次错误；失败的 shutdown/reset 不会经第二次
stop 被覆盖或重放。两个独立 reset 都会尝试，随后执行固定一加 BSP 要求的 5ms
等待；等待失败同样隔离。未确认功放停止时保留其 PCM/MI2S 图和 DMA backing。
runtime 分别保存 `cause_result/backend` 与 `cleanup_backend`，SPF BASIC_RSP 拒绝
也保留真实 DSP 错误，不能用零值的 SPF state 掩盖拒绝原因。

平台契约提升至 `GA_PLATFORM_ABI=2`：`metadata_release(cookie, image)`、
`unprepare(cookie, const gab_state *)` 和 native 的 `unmap_nc` 必须返回实际释放结果。
只在成功后清除对应地址、mask 或 prepared 状态；失败保留所有权与首次清理错误，
禁止重放失败释放。`cleanup_phase/cleanup_image` 区分 metadata 和资源撤销；
后者的 image 值为 `GAB_IMAGES`，不表示某一固件映像。

`gan_shutdown_complete` 不再清除已有故障，必须取得 resident boot 对象记录的
PAS1→PAS36 成功停止、metadata 释放以及当前 lease 仍有效的证据。
prepared 的 native 对象取得这项确认后才可释放映射；AUTH1 前不能直接释放。
provider 必须核对传入 boot 对象确属自身绑定的 runtime 和 native epoch。
这些均为内核内部契约，不允许通过 NLH_CALL 标量传递对象指针或伪造证明。

`gar_quiesced` 检查本地 metadata、graph、PA、GLINK intent/ring/SPF 残留，
只是热替换的必要条件。真实 provider 仍须关闭新入口并排空 worker/calls，完成
同 epoch 的 SMP2P stop-ack/sysmon、IRQ/RX work、图 STOP/CLOSE ACK 与 DMA drain。
现有 `gat` 没有完整主动 GLINK CLOSE/ACK 停止流程，清零结构不能代替实际停止。
硬件资源、固件、IRQ、DMA backing 和运行时对象必须常驻，只可迁移已获准的模块
私有数值状态；当前没有音频 native/PCM provider，纯协议 bridge 接入不能宣称支持播放或
驱动整体热替换。

本轮完成整组源码与固定上游对照，模块边界及文本检查结果记录于主控交接报告。
未运行本地主机测试套件；新目标内核构建、终端实测及 UEFI 集成均尚未完成。

## 当前阻塞与验证边界

已对原厂 PILDxe、PILProxyDxe、NpaDxe、ICBDxe、RpmhDxe 做版本固定的只读审计，
记录在[平台交接审计](../hardware/sm8650-audio-platform-audit.json)。具体发现：

- PIL 配置查询和 SCM “supported” 响应不能证明 ADSP 已停止。原厂充电驱动也有
  `FULL_ADSP` 路径，需要可靠的启动周期标识或协调停机及双 PAS shutdown 确认。
- 原厂 PILProxy 投票可能通过定时器撤销；它不能代替本驱动独立持有的资源。
- RPMh 在 ExitBootServices 卸载协议并清理 TCS 控制。这不证明所有 ACTIVE 硬件
  投票已清零，但也不能据此承诺 sleep／wake 编程仍然有效。
- 仍需完成 XNU 的 RSC／带宽资源接管、保留内存 lease、DSP 文件服务／AMDB、
  PCM DMA／SMMU 和完成回报、MI2S 媒体配置，以及实际 I2C／GPIO provider。

历史离线验证不作为 DSP 启动或发声证据。完整 XNU 目标编译、实机 ADSP 启动、
扬声器输出和 IOAudio2Family／系统音频应用接入均未验收。

本轮音频 Python 测试66项通过；全仓库237项无失败，1项屏幕测试因缺少专用 C
夹具跳过。C ASan／UBSan、完整服务的 C++14 主机编译和四种 Wi-Fi／GPU 组合补丁
检查通过。结果与未绑定依赖见[运行时记录](../hardware/sm8650-audio-runtime.json)。
