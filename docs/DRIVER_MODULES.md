# 驱动模块规范

Darwin 22 自研驱动按职责放在 `ios16/` 的模块目录中。每个源码只有一个归属；不保留旧平铺目录的副本或转发文件。源文件路径通过 `scripts/ios16_sources.py` 维护，生成到 XNU 的目标路径与仓库目录分开处理。

| 模块 | 职责与公开边界 |
| --- | --- |
| `common` | SHA-256、调试 ABI、日志工具、输入接收接口契约 |
| `platform` | 单例资源账本、IPCC/QMP 仲裁、固件交接策略、SCM、重启 |
| `gpu` | A750、GMU、Gunyah、内存映射、队列与 fence |
| `wifi` | WCN7850、PCIe/MHI、无线协议、安全与网络接口 |
| `audio` | ADSP、GLINK、音频图、功放与运行时协调 |
| `modem` | 基带生命周期、QMI/QMUX/QMAP 与系统接口 |
| `display` | 帧缓冲、画面捕获、GUI HID 接收适配器 |
| `touch` | TouchCom 物理触控与 GPIO/SPI |
| `keys` | 物理按键采样与诊断 |
| `slider` | Hall 三段式开关与诊断 |
| `storage` | UFS 的 SMMU 策略及存储诊断适配 |
| `terminal` | USB 调试协议、shell、画面和远程输入协议 |
| `trust` | AMFI 只读信任目录适配 |

`vendor/` 保留固定版本的第三方源码，`userland/` 保留调试守护进程。参考仓库提供的 UFS/USB、IOKit 兼容服务和 UEFI provider 仍通过已有上游补丁集集成。

## 依赖方向

业务驱动只能引用自身、`common`、`platform` 的契约以及已固定的第三方库。禁止包含其他业务驱动的私有头文件、调用其私有函数，或直接访问其状态。

Wi-Fi 和音频通过平台资源 API 获取 IPCC/QMP 资源，不要求 GPU 或基带服务存在。GPU 的本地 fence 生命周期约束仍然保留，资源占用同时进入同一平台账本。平台账本不回调业务驱动，不把软件租约当作硬件就绪证明。QMP 对缺失、损坏或不确定的交接状态按契约处理；存在但为空的交接属性会被拒绝。

输入驱动通过 `common/giulia_input_provider.h` 投递事件。GUI HID 适配器实现这个接口；物理驱动不引用显示类或彼此的源码。USB 命令桥、输入启动钩子和 IOKit 注册属于装配层，可以连接各模块的公开入口。

## 构建装配

`scripts/ios16_modules.py` 是驱动装配入口。各驱动生成器只读取自己的源码与声明的公共依赖，不检查其他驱动是否生成，也不修改其他驱动的补丁。

- GPU、Wi-Fi、音频、基带可以独立选择，共有 16 种组合。
- `0088-giulia-platform-runtime.patch` 提供一次公共实现；`0090-giulia-optional-modules.patch` 统一注册翻译单元与 IOKit personality。
- 触控、按键、Hall 的源码补丁各自独立；`0091-giulia-input-modules.patch` 集中注册 BSD 翻译单元和启动入口。
- `module-registry.json`、`input-module-registry.json` 记录最终模块与翻译单元。
- 源码使用明确的相对跨模块 include；生成器在 XNU 的目标目录中解析这些路径，不通过复制另一业务驱动的代码来消除依赖。

现有 CLI 开关及硬件启用策略保留。独立模块表示源码和构建边界，不代表可动态卸载的 kext；硬件租约、在途 DMA 与不确定状态的保留规则仍然有效。

## 新增与修改模块

1. 在对应目录放置源码，在 `ios16_sources.py` 声明新命名或新模块的归属。
2. 共享机制先定义 `common`／`platform` 接口；禁止向业务驱动添加跨设备协调职责。
3. 由装配层连接注册和生命周期，不在驱动生成器中探测另一个驱动的补丁文件。
4. 更新生成、源码快照及产物核验；新增依赖必须在边界检查中显式允许。

仓库检查：

```bash
python3 scripts/check-driver-modules.py
git diff --check
```

边界检查保留在内核 CI 中，随后按选定模块完成内核构建与产物核验。各组合的硬件行为需以对应镜像的实机记录为准。
