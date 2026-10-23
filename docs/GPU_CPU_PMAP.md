# GPU 队列 CPU pmap 门控（0086）

本文对应 `scripts/ios16_gpu_pmap_patch.py`、生成的
`ios16/0086-giulia-gpu-pmap.patch`、`ios16/gpu/giulia_gpu_pmap.h`、
`ios16/gpu/giulia_gpu_fence_native.h`、`ios16/gpu/giulia_gpu_fence_native.c`、
`ios16/gpu/giulia_gpu_fence_iokit.cpp`。协议核心按 C 编译，IOKit 映射后端按 C++ 编译，
通过同一组 C ABI 接口连接；`IOMemoryMap` 由 XNU 的 `IOMemoryDescriptor.h` 声明。它补齐的是
`docs/GPU_FENCE_PROVIDER.md` 里标注为“原生 pmap ABI 声明仍未实现”的那一层：
真正的页表策略、审计、只读证据和 Normal NC 守卫。

本层不注册 fence 提供者、不读队列内容、不清零 carveout、不取得 Gunyah 租约，
也不把任何测试结论当作实机映射已成立。

## 分层

| 层 | 文件 | 职责 |
|---|---|---|
| 纯解码 | `giulia_gpu_fence_pmap.h` | 单个 48 位叶描述符、范围、MAIR/TG1、填充页检查 |
| 纯策略 | `giulia_gpu_pmap.h` | boot 锁存、TCR/T1SZ/TTBR1、直接映射与管理 RAM 别名窗口、整段 seal |
| 原生页表 | `0086-giulia-gpu-pmap.patch` | 在真实 `osfmk/arm/pmap/pmap.c` 中读取 MAIR/TCR/TTBR1、逐叶 `pmap_pte`、Normal NC 守卫 |
| 原生 owner | `giulia_gpu_fence_native.c` | 冻结 provider、核对 share 身份、调用真实 mapper/auditor、保留失败状态 |

`ggfpm_audit` 由原生代码填写，`ggpm_audit_seal` 在纯策略里再次核对：页粒度、
首末描述符、属性一致性、VA/PA 对齐、范围、别名窗口和 `ggfpm_audit_range`
全部通过才写出结果；失败不修改输出。

## 0086 注入点

生成器只改一个文件，锚点缺失或重复一律失败：

| 位置 | 内容 |
|---|---|
| `#include <arm/pmap/pmap_internal.h>` 之后 | 两个 GPU 头文件 |
| `vm_last_phys` 定义之后 | 状态、MAIR/TCR/TTBR1 读取、策略、boot 钩子、`giulia_gpu_fence_cpu_audit` |
| `pmap_bootstrap()` 的 `pmap_lock_init(kernel_pmap);` | `giulia_gpu_fence_cpu_boot_validate(TCR, MAIR, 粒度)` |
| `pmap_map_bd_with_options()` 的模板 PTE 之后 | 非默认缓存选项映射 carveout 时锁存拒绝 |
| `wimg_to_pte()` 之后 | `giulia_gpu_pmap_guard_enter()`：只允许 attr index 1 |
| `pmap_enter_options_addr()` 入口 | 非 Normal NC 别名返回 `KERN_PROTECTION_FAILURE` |
| `pmap_init()` 的 `pmap_initialized = TRUE;` | `giulia_gpu_fence_cpu_boot_retired()` |

策略要求：

- boot 阶段真实读到 `page ∈ {4,16,64 KiB}`、`TG1` 与之一致、MAIR index 1 = `0x44`；
- `T1SZ` 在 16..32，TTBR1 为 48 位页对齐 BADDR，且与 `kernel_pmap->tte` 的 BADDR 相同；
- 被审计范围必须完全位于固定 carveout `0xd4e23000 / 0x2dd000` 内，且不与
  直接映射窗口（`gPhysBase/gPhysSize`）或内核管理 RAM（`vm_first_phys/vm_last_phys`）相交；
- boot 阶段若把 carveout 以写合并等非默认选项映射，永久锁存拒绝，重新
  `boot_validate` 或 `boot_retired` 也不能解除。

`pmap_enter_options_addr` 的守卫是“无 WB 别名”这一证明的执行点：carveout 只能
以 attr index 1（Normal NC，MAIR `0x44`）建立叶页，写回/写合并映射在提交前
返回失败。守卫只看 PA 与最终 PTE 的 AttrIndx，不看调用者名称或 cache 选项名。
四个调用点和全部实现同样包在 `#if defined(ARM64_BOARD_CONFIG_SWEET)` 内，
其他板级配置不会看到这些符号。

## CPU 导入 owner

`ggfn_import()` 只接受：

- 已注册且 `ggfp_live()` 通过的 provider（每次调用重新询问完整 proof）；
- owner 提供的 share 快照：`epoch` 与 provider 相同、`parcel` 非零、
  `self/peer VMID` 动态有效且不同、事务已完成且未失效；
- 真实 mapper 与 auditor（内核里分别为 `giulia_gpu_fence_native_map/audit`）。

provider 在导入时被复制冻结，之后修改调用者的结构体不改变已存 proof。映射失败
保留 backing 与句柄并隔离；成功后的 `ggfn_get()` 会重新执行 provider proof 与
实际映射审计，任一失败都锁存为隔离。没有释放/回滚/重复导入接口。

内核 mapper 使用 `IOMemoryDescriptor::withPhysicalAddress()` 加
`createMappingInTask(kernel_task, kIOMapAnywhere | kIOMapInhibitCache)`：carveout
在物理口径之外，`wimg_to_pte()` 因此选择 attr index 1（Normal NC），与审计
预期一致；descriptor 与 `IOMemoryMap` 都 retain 到重启。kernel auditor 调用
`giulia_gpu_fence_cpu_audit()` 并核对 ABI、页粒度、VA/PA、长度和 writable，
只有全部匹配才填 `GGFC_AUDIT_REQUIRED`。

## 仍未完成

- share 事务的运行时接入：`ggfn_import()` 需要真实 `ggsh_state` 结果，当前
  GPU worker 只持有 `ggown_state`（传输/查询）与 fence provider 注册表，没有
  可用的 share 快照，因此服务侧不会自动发起 CPU 导入；
- 控制器页 ABI 与相邻页授权：16 KiB CPU 粒度覆盖队列前后各一页，仍需控制器
  所有者证明这两页可被映射；
- 完整 XNU 构建、内核链接和实机 TTBR/TCR/MAIR 观测；本轮的证据是锚点、
  `git apply --check`、RAM 页表模型与 Darwin/AArch64 交叉编译。

## 验证

```bash
# 需要真实 Sweet 补丁后的 XNU 树：
python3 scripts/ios16_gpu_pmap_patch.py --source XNU --check-static \
   ios16/0086-giulia-gpu-pmap.patch
```

历史主机页表模型不证明 ARM 总线、缓存一致性或真实翻译。
