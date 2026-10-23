# 贡献与发布规范

## 文件约定

- `configs/`、`board.toml`：设备配置；`sources.json`：上游版本锁定。
- `ios16/`、`patches/`、`uefi-overlay/`：源码、补丁与平台覆盖文件。
- `scripts/`：构建、打包及诊断工具；`tests/`：离线验证。
- `hardware/`：脱敏后的机器可读实机记录；`docs/`：开发说明与历史。
- `out/`、`.cache/`、`reports/`：本地产物，不提交到 Git。

遵循 `.editorconfig` 和 `.gitattributes`；文本使用 UTF-8，通常使用 LF，PowerShell 文件使用 CRLF。保持现有源码格式，不对上游补丁做批量格式化。英文与中文 README 的关键说明同步更新。

## 验证

驱动按 [模块规范](docs/DRIVER_MODULES.md) 放入 `ios16/<module>/`。业务驱动只能依赖公共契约和平台服务，不得包含兄弟驱动的私有实现；注册、启动顺序及可选组合统一由 `scripts/ios16_modules.py` 装配。提交前运行 `python3 scripts/check-driver-modules.py`，并覆盖受影响的模块组合。

在仓库根目录运行：

```bash
git diff --check
python3 -m unittest discover -s tests -v
```

修改工作流时检查 YAML 和 GitHub Actions 表达式。硬件功能改动还应运行对应离线测试，并分别记录编译、产物检查和实机结果。
