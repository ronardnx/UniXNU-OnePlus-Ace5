# Ace 5 音频驱动映射

后续进展见[ADSP 启动与扬声器播放核心](AUDIO_BRINGUP.md)：加载、通信、功放及
APM 事务核心已加入，原生资源和 PCM provider 尚未完整绑定，实际播放仍不可用。

2026-10-23：已加入基于实机 Android DT、原厂固件及一加开源 BSP 的音频资源映射，
并接入 base／GUI 内核的 USB 元数据查询。当前阶段为 `board-mapping-only`；
尚未实现 ADSP 启动、PCM 播放、录音或 IOAudio2Family 音频设备。

## 板级连接

| 部件 | 本机映射 | 依据和限制 |
| --- | --- | --- |
| ADSP | MMIO `0x03000000 / 0x10000`；主映像 RAM `0x9df00000 / 0x5280000`；DTB RAM `0x9de80000 / 0x80000` | 固件保留区不是 XNU 可分配内存 |
| GLINK／GPR | remote PID 2，IPCC client 3／signal 0，`adsp_apps`；SPF／packet／PRM 服务 3／23／7 | 服务编号、DSP module instance、音频端口属于不同地址空间 |
| LPASS RX／TX／VA | `0x06ac0000`／`0x06ae0000`／`0x06d44000`，各 `0x1000` | 原 DT 长度为零，长度来自对应宏驱动的 `devm_ioremap` |
| RX／VA SoundWire | `0x06ad0000`／`0x06d30000`，各 `0x60a8` | 原厂 SWRM v2 配置传给 `ioremap` 的精确长度；偏移 `0x60a8` 本身不在边界内 |
| LPI GPIO | `0x06e80000 / 0x20000` | 长度来自 `pinctrl-lpi.c`，DT 定义 23 个 GPIO |
| 双 SIA9177 | I2C4 `0x6c / 0x6d`，reset GPIO `173 / 178`；secondary MI2S | 控制器 `0x0099c000 / 0x4000`；SIPA 初始化依赖 `sipa.bin`，尚未实现 |
| WCD939x | RX `0x0e01170224`、TX `0x0e01170223`，reset GPIO 107 | SoundWire 设备地址；codec 使用 TX slave regmap，16 位寄存器地址／8 位数据 |
| WCD USB switch | I2C `0x0e`，控制器 `0x00a8c000 / 0x4000` | 与 SoundWire codec 区分；没有已确认的独立 reset GPIO |
| WSA／WSA2／WSA884x | 板级禁用 | 不作为本机扬声器路径，映射函数拒绝解析禁用资源 |
| Audio ION | IOVA `[0x10000000, 0x20000000)`；SID／mask `0x1001/0x80`、`0x1061/0` | 只记录 Android 配置，未建立 XNU DMA domain |

原厂 ODM `mixer_paths.xml` 的 `speaker` 路径开启左右 SIPA Playback，`handset`
路径使用左 SIPA Receiver；音频参数版本为 `Giulia_20251222_V29`。
WCD939x 的麦克风／耳机路由不能替代这两条功放路径。

## 文件与运行方式

- [`hardware/sm8650-audio.json`](../hardware/sm8650-audio.json)：资源、原始 DT 属性、
  中断与总线路由、49 个源码文件哈希、56 个原厂固件文件哈希和剩余工作。
- [`ios16/audio/giulia_audio_mapping.h`](../ios16/audio/giulia_audio_mapping.h)：从清单生成的 C/C++
  静态映射和边界函数。`gamap_resolve` 只返回有效资源内的物理地址；
  `gamap_page_window` 计算 4K／16K 页覆盖范围，不扩大寄存器访问权限。
- [`scripts/check-audio-mapping.py`](../scripts/check-audio-mapping.py)：检查生成文件及本地输入。
  [`scripts/ios16_audio_firmware.py`](../scripts/ios16_audio_firmware.py) 解析并校验分段 ELF。

用本轮源码重新构建内核后，在 USB 终端执行：

```text
audio.status
audio.map
logs audio
```

`audio.map` 输出已固定的映射表，不读取寄存器。`audio.status` 明确返回
`hardware_ready=0 playback=0 recording=0`。`android_enabled` 表示原 DT 状态，
不能用来判断 XNU 驱动、功放或时钟已经启动。命令不需要新增 `--audio` 开关，
并不会加入 `all`／`collect` 的自动查询。更新电脑工具后仍需使用包含这些命令的新内核。

在仓库根目录进行离线检查：

```bash
python3 scripts/check-audio-mapping.py
bash scripts/test-ios16-audio.sh
python3 -m unittest discover -s tests -v
```

本地有原始输入时可进一步核对；源码缓存布局为 `<repository-name>/<upstream-path>`：

```bash
python3 scripts/check-audio-mapping.py \
  --dts ../ace5-analysis/running-device.dts \
  --source-dir .cache/audio-source \
  --firmware-dir .cache/audio-firmware
```

更新清单后运行 `python3 scripts/check-audio-mapping.py --write-header` 重新生成头文件。
生成步骤本身不重新证明数据来源，仍需核对相应 DT、源码和固件证据。

## 来源和验证范围

原始 DTB SHA256 为 `1f32afa0b9ec436bdad9726c557dc9abdd35b629531f9593002f5ed886447d9b`。
BSP 固定于一加 Ace 5 分支的
[模块／设备树提交](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650/tree/453d8875f815bf9dfad9035982861226a4823a6c)
和 [内核提交](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/tree/d763ed47deec55451366c1187f813f2f309d88a7)。
源码发布为 `16.0.9.400`，本地固件为 `16.0.3.500`；本轮只确认映射及文件完整性，
不据此认定两版本的所有 DSP ABI 相同。

原厂 `NON-HLOS.bin` 中 `adsp.mdt` 为 ELF32 Hexagon，`adsp_dtb.mdt` 为 ELF32
DTB 容器；BSP 的 `uses_elf64=true` 是其资源配置字段，不改变实物 ELF 标识。
两份 MDT 和 54 个非零 PT_LOAD 分段共 41,485,350 字节均已核对。
`adsp.b24`／`adsp.b55` 对应零填充内存，不要求外部分段文件；hash 分段不作为普通
PT_LOAD 加载。校验工具检查整个 `p_memsz` 是否位于对应保留区，包括 BSS 部分。
此检查不执行 PAS 签名认证，也不加载或启动固件。专有固件和校准数据只保存在本地缓存。

本轮验证通过：69 项 DT 属性、49 个固定源码文件、56 个固件文件，23 项音频 Python
测试，C ASan／UBSan 边界检查、C++ 编译以及剥离 libc 整数宏后的内核嵌入编译。
全仓库运行 164 项 Python 测试，无失败，1 项屏幕测试因未提供专用 C 数据包夹具跳过；
base 和 GUI（同时启用 Wi-Fi／GPU）补丁生成及完整 USB 补丁链应用检查通过。
CI 已加入 `test-ios16-audio.sh`。完整 XNU 目标编译与实机音频验证尚未执行。

后续需要完成 PAS／SCM 和保留内存所有权、SMEM／GLINK／GPR、音频时钟和电源、
SPF graph 与 ACDB 校准、`sipa.bin` 校验及功放时序、PCM DMA 和 iOS 音频接口，
之后才能验证扬声器、听筒、耳机与录音。
