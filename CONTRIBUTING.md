# 贡献与发布规范

## 文件约定

- `configs/`、`board.toml`：设备配置；`sources.json`：上游版本锁定。
- `ios16/`、`patches/`、`uefi-overlay/`：源码、补丁与平台覆盖文件。
- `scripts/`：构建、打包及诊断工具；`tests/`：离线验证。
- `hardware/`：脱敏后的机器可读实机记录；`docs/`：开发说明与历史。
- `out/`、`.cache/`、`reports/`：本地产物，不提交到 Git。

遵循 `.editorconfig` 和 `.gitattributes`；文本使用 UTF-8，通常使用 LF，PowerShell 文件使用 CRLF。保持现有源码格式，不对上游补丁做批量格式化。英文与中文 README 的关键说明同步更新。

## 验证

在仓库根目录运行：

```bash
git diff --check
python3 -m unittest discover -s tests -v
```

修改工作流时检查 YAML 和 GitHub Actions 表达式。硬件功能改动还应运行对应离线测试，并分别记录编译、产物检查和实机结果。

## 两个账号的同步与构建

源码仓库：<https://github.com/tycykp/UniXNU-OnePlus-Ace5>。
构建仓库：<https://github.com/jasper2863/UniXNU-giulia>（访问需相应权限）。

本地 `origin` 指向源码仓库，`build-jasper` 指向构建仓库。两个仓库的 `main` 保持同一提交；同步前先 fetch 并检查历史，不使用强制推送覆盖远端改动。

```bash
git fetch origin
git fetch build-jasper
git push origin main
git push build-jasper main
```

所有工作流保留手动触发，且每个作业都检查 `github.repository_owner == 'jasper2863'`，因此 tycykp 端不会分配构建 runner。使用具有构建仓库权限的 GitHub 登录会话触发当前 GUI 开发配置：

```bash
gh workflow run ios16-xnu.yml --repo jasper2863/UniXNU-giulia --ref main \
  -f profile=gui -f touch=true -f wifi=true
gh run list --repo jasper2863/UniXNU-giulia --workflow ios16-xnu.yml
```

GUI 内核还需在本地使用已验证的产品 ADT 打包。固件保持本地，不随 CI 上传。基础启动镜像可使用 `profile=base` 且关闭 `touch`、`wifi`；它不等同于 GUI 配置。复用产物时，run ID 必须来自对应构建仓库；历史记录中的旧 run ID 不应直接用于另一个仓库。

凭据仅通过 GitHub CLI／系统凭据库管理，不写入脚本、文档或 Git 远程 URL。
