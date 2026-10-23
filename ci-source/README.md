# CI source snapshots / CI 源码快照

`NativeHotload/` is a source-only build snapshot of the common implementation
in `UniXNU/kits/NativeHotload`. The canonical implementation stays in that
repository. `native-hotload-source.json` records every copied file's SHA-256;
both Actions jobs verify those digests before using the snapshot. Each run
also checks module and kernel provenance against the same source revision.
The copied files retain their original license notices.

`NativeHotload/` 是公共 `UniXNU/kits/NativeHotload` 实现的纯源码构建快照；
实现归属仍在公共仓库。`native-hotload-source.json` 记录全部文件的 SHA-256，
两个 Actions 任务使用前均核对摘要，并将模块、内核来源绑定到同一源码版本。
副本保留原许可证通知。

From the device repository, stage into new output paths:

在设备仓库中使用新输出路径生成快照：

```sh
python3 scripts/stage-hotload-ci.py --source ../UniXNU/kits/NativeHotload \
  --out ci-source/NativeHotload --manifest ci-source/native-hotload-source.json
```

The script performs local staging only. Committing/pushing the snapshot and
dispatching Actions are separate operations. Hotload Actions compile and
verify the kernel and 15 policy images; they skip UEFI/boot-image integration.
Device execution and remaining whole-subsystem conversion require separate
validation.

脚本只生成本地副本，提交、推送和运行 Actions 是独立操作。热加载 Actions
编译并核验内核及 15 个策略镜像，跳过 UEFI／启动镜像集成。实机运行及其余
完整子系统迁移仍需独立验证。
