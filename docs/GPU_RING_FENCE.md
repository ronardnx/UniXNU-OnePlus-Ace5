# CP Ring 与时间戳 fence（A7XX / A750）

ring 与 fence 是 GPU 真正开始"执行作业"的入口：没有它，CP_INIT 之后的任何东西都只能靠
HFI RAW 单发；有了它，提交、完成、超时和故障才有可验证的语义。

协议不是新发明的：它和 pinned 上游驱动在 A740 实机上跑的那套一致
（`drivers/NativeAdreno/NativeAdrenoRing.cpp`，对应 Linux MSM 的 ring + `CP_EVENT_WRITE7`）。

## 1. 寄存器与 fence 页布局

| 步骤 | 寄存器（dword offset） | 值 |
| --- | --- | --- |
| ring 基址 | `CP_RB_BASE` 0x800 / `_HI` 0x801 | ring IOVA（须按 ring 大小对齐） |
| 控制字 | `CP_RB_CNTL` 0x802 | `ilog2(ring_bytes/8) \| (ilog2(32/8) << 8)`；8 KiB ring → `0x20a` |
| 读指针地址 | `CP_RB_RPTR_ADDR` 0x804 / `_HI` 0x805 | fence 页 IOVA |
| 写指针 | `CP_RB_WPTR` 0x807 | 每次提交写入新的 end |
| 序号 | `CP_SCRATCH(2)` 0x885 | 每次提交写 seqno |

fence 页（与上游 `msm_rbmemptrs` 一致）：

| 偏移 | 内容 |
| --- | --- |
| +0 | CP 写入的读指针影子 |
| +4 | **完成时间戳**（只有 GPU 会写） |
| +8 | 保留 |

## 2. 一次提交的包序列

```
[用户命令字...]
0x48088501  seqno                     ; TYPE4 CP_SCRATCH(2) = seqno
0x70460004  0x88000031  lo  hi  seqno ; TYPE7 CP_EVENT_WRITE7(4)
```

`0x31` 是 A7XX 的 `CACHE_CLEAN`，`0x88000031` 打开 ENABLE 与 IRQ 位，`lo/hi` 是
`fence_iova + 4` 的 64 位地址，最后一个字是序号。共 7 个字，和上游 `GGR_FENCE_WORDS` 对齐。

完成判据：`fence[+4] == seqno`。如果读到比已发出序号更大的值，说明 fence 页不是我们以为的那块，
引擎直接 poison，而不是把它当进度。

## 3. 语义（与上游逐条一致）

- attach 之前先校验全部参数（ring 大小是 2 的幂且在 [8 KiB, 1 MiB]、IOVA 按 ring 大小对齐、
  fence 页 ≥64 字节且 8 字节对齐），**任何一个不合法都不会写寄存器**。
- 提交前先用 `CP_RB_RPTR` 计算剩余空间；空间不足返回 `GGR_NO_SPACE`，不写 ring 内存
  （写一半会让 WPTR 与包流对不上）。
- 提交顺序：写命令字 → 写 fence 包 → **整片 ring 做一次 cache publish** → barrier →
  写 `CP_RB_WPTR`。任何一步失败都 poison，不原地重试。
- `CP_RB_RPTR` 读出越界值直接 poison（CPU 侧无法再推理 ring 状态）。
- `wait` 只读 fence 页，从不写寄存器、从不推进 ring；超时只是超时，不是 poison。
- 故障探针（`health`）在 `wait` 里每轮检查，观察不到健康就直接 poison。
- `detach` 只在所有提交都被观察到之后允许。

## 4. 代码与测试

| 文件 | 内容 |
| --- | --- |
| [`ios16/giulia_gpu_ring.h`](../ios16/giulia_gpu_ring.h) | 纯 C 引擎：TYPE4/TYPE7 编码、attach/submit/wait/observe/detach、poison 语义 |
| [`tests/ios16_gpu_ring.c`](../tests/ios16_gpu_ring.c) | 主机测试：0x20a 控制字、7 字 fence 金标准、整片 publish、wrap-around、满环拒绝、超时、publish/barrier/WPTR 失败 poison、非法 rptr、故障、误用 |
| [`scripts/gpu_terminal_ring.py`](../scripts/gpu_terminal_ring.py) | 设备侧实现：用现有 transport 分配 ring/fence、写寄存器、提交并等待 |
| [`tests/test_gpu_ring.py`](../tests/test_gpu_ring.py) | 主机侧单测：包编码、寄存器计划、提交落点、fence 只认已发序号、超时带故障快照、对齐拒绝、空间拒绝 |

## 5. 实机执行

```bash
python3 scripts/gpu-native-hwsched.py --execute --platform --stage ring --host-raw-dispatch
```

它会在 `cp-stock` 的全部步骤（厂商顺序 HFI 启动、CP_INIT、ZAP、安全模式退出、首个 GPU 写入）
之后，再 attach 一个 8 KiB ring 与 4 KiB fence 页，提交 `CP_MEM_WRITE(0xa750cafe)` + 时间戳，
并要求：

| 字段 | 期望 |
| --- | --- |
| `ring.control` | `0x20a` |
| `ring_submissions[0].seq` | `1` |
| `ring_write_value` | `0xa750cafe` |
| `ring_verified` / `ring_fence_verified` | `1` |

失败时 `ring_fence_error` 会带上 `cp_hw_fault`、`cp_interrupt_status`、`rbbm_status` 四个快照，
便于区分"CP 没跑"和"提交没到 CP"。

## 6. 边界

- 这一层只证明"提交 → GPU 执行 → 完成"这条链路。它不含上下文切换、抢占、IB 间接缓冲，
  也不是 Metal 的 command queue。
- 时间戳 fence 与 `--hw-fence` 那条 Gunyah 队列导入是两种实现；在 ring fence 稳定前不要打开
  `--hw-fence`。
- 实机尚未跑过这一层；引擎与主机两侧的协议测试已通过（C 12 组、Python 9 项）。
  下一步见 [GPU_METAL_UNLOCK.md](GPU_METAL_UNLOCK.md)。
