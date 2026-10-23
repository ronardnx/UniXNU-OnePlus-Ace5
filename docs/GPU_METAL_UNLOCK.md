# Metal 解锁路线（Ace 5 / Adreno 750 / Darwin 22）

ring 与 fence 已落地（见 [GPU_RING_FENCE.md](GPU_RING_FENCE.md)）。Metal 不是"再打开一个开关"，
在非 Apple GPU 上它是一条四层链路，每层都有独立的验收判据。本文把每层需要的接口、现有可复用的部分、
以及还缺什么写清楚，避免再出现"发个设备名就宣称支持"。

## 1. 链路与判据

| 层 | 内容 | 判据 | 现状 |
| --- | --- | --- | --- |
| A 提交与完成 | command stream → ring → fence | 一次提交的 GPU 写入被主机读到，fence 按序完成 | **已实现**：`ios16/giulia_gpu_ring.h` + `scripts/gpu_terminal_ring.py`，主机测试 9 项、引擎测试 12 组 |
| B 内核加速器 | accelerator 注册 + IOUserClient + VM/BO/fence 句柄 | 用户态能拿到设备、分配/映射资源、提交并等待完成 | 未接：`GiuliaGPU` 目前是 bring-up 服务，不发布 accelerator |
| C Metal 用户态 | Metal 插件（Metal.framework → 后端 → 我们的 IOUserClient） | 应用创建 `MTLDevice`、编码一次 draw/compute、结果可核对 | 未接：需要按 Darwin 22 重新审计 ABI，并打包插件 |
| D 合成与显示 | IOSurface / present / 合成器 | SpringBoard 实际由 GPU 合成并连续刷新 | 未接：`GiuliaGPU` 没有 IOSurface 通路 |

A 层现在是唯一可以立刻在现有 r5 镜像上验证的一层：`--stage ring` 会把 ring 程序化、提交
`CP_MEM_WRITE(0xa750cafe)` 与时间戳 fence，并要求两者都被观察到。

## 2. B 层：内核加速器

上游 `drivers/NativeAdreno` 已经有完整的实现骨架（`NativeAdrenoAccelerator.cpp`、
`NativeAdrenoUserClient.cpp`、`NativeAdrenoVm.cpp`、`NativeAdrenoSubmit.cpp`）。移植到 Ace 5 时
必须处理的三件事：

1. **ABI 版本**：上游的 accelerator 绑定的是审计过的 `IOAcceleratorES` 基类（
   `scripts/audit-target-accelerator-base.py` 产出的 `ioaccelerator-base-abi.json`，136 字节、
   168 个虚表项）。那份记录来自 sheng 的 iOS 17 内核；Ace 5 是 Darwin 22 / iOS 16，
   类布局和选择器表必须重新审计一份，不能复用。
2. **资源句柄**：`NativeAdrenoUserClient` 的选择器直接落在 VM/BO/fence 上。我们的 ring/fence
   已经把"提交—完成"这半边补齐；还缺按进程的资源表（VM 上下文、BO 生命周期、句柄回收）。
   这部分可以直接采用上游 `NativeAdrenoVm*` 的结构，把 `ArmSmmu4K` 的上下文换成 giulia
   已跑通的 bank 0/SID 0。
3. **发布条件**：只有 `ring` 的首次 fence 完成、且 `MMU` 上下文有真实翻译证据之后才允许
   `registerService()`。现在的 `GiuliaGPU` 明确不发布 accelerator，这条要保留到 B 层通过。

## 3. C 层：Metal 用户态

上游把 Metal 拆成 `NativeMetal` + `kits/AirNir` + Turnip 后端，工具链是
`scripts/build-adreno-metal-plugin.py`、`scripts/build-turnip-darwin.py`、
`scripts/add-gpu-plugin-data.py`。移植要点：

1. **插件 ABI**：Darwin 22 的 Metal 驱动接口（`MTLDevice` 背后的 AGX 兼容层）与 17 不同，
   插件必须重新对 22 编译并重新审计导出符号；`NativeMetal` 的 `OriginalAcceleratorABI.h`
   是 17 的，要重新生成。
2. **着色器**：Turnip/AirNir 产出的 A7XX ISA 必须和 A750 的 SQE 固件版本匹配（`gen70900_sqe.fw`，
   sha256 见 `ios16/native/giulia_a750_native.h`）。上游在 A740 上验证的是 a740_sqe；A750 的
   shader 缓存与 pipeline 需要重新验证，不能直接复用。
3. **缓存**：`scripts/check-priming-cache-host.py`、`kits/NativePrelink` 的 cache 路径也要按
   A750 重跑；缓存格式里带有 GPU id，混用会直接崩。

## 4. D 层：合成与显示

Ace 5 的显示走 giulia 自己的 panel 驱动（`ios16/giulia_screen*`、`giulia_display*`），
没有 IOSurface 生产/消费链路。要做的是：

1. 给 accelerator 发布一个 `IOSurface` 兼容的 backing（GPU 可写、CPU 可读的 scanout buffer）。
2. 把 present 事件接到 giulia 的 display power/panel 提交路径，按 vsync 节流。
3. 验证 SpringBoard/backboardd 的实际合成，而不是只看我们自己的测试图案。

## 5. 顺序与门禁

1. `--stage ring`：拿到 `ring_verified=1` 与 `ring_fence_verified=1`（A 层）。
2. 连续提交（≥100 次）与超时/故障注入：ring 不许丢 fence、不许覆盖未消费数据。
3. B 层：accelerator 注册 + 用户态分配/提交/等待，用一个小测试客户端跑通。
4. C 层：单个离屏 draw + 结果读回校验；再做连续帧。
5. D 层：合成器接管，再看系统 UI。

每一层没有实机证据就不进入下一层，也不再对外宣称"Metal 可用"。

## 6. 明确未验证

- A 层的 ring 提交尚未在 A750 实机上跑过（引擎与主机两侧的协议测试已通过）。
- B/C/D 层没有代码落地，本文只固定接口边界与判据。
- `HW_FENCE`（Gunyah 队列）与本文的时间戳 fence 是两种不同实现；在 ring 的 fence 稳定前，
  不应把 `--hw-fence` 打开。
