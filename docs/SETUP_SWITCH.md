# 开机设置向导开关

`scripts/rebuild-ios16-userdata.py` 的 `--skip-setup` 控制新 userdata 镜像的开机设置／激活引导页面。默认关闭；不传参数或显式传 `--no-skip-setup` 时保留正常向导。

开启示例（在 Linux / WSL 中运行，使用已验证的原始固件根卷及其报告）：

```bash
sudo python3 scripts/rebuild-ios16-userdata.py \
  --source /path/to/verified-base.img \
  --report /path/to/verified-base-root.json \
  --out out/ios16/userdata-skip-setup \
  --startup-compat --migration-baseline --full-boot-log \
  --skip-setup
```

关闭时，从同一份原始根卷重新构建到新的输出目录，将最后一个参数换成 `--no-skip-setup`，或省略该参数。其他三个选项沿用当前启动兼容、迁移基线和日志配置，也可按原有构建流程选用。

这是 userdata 构建开关。更换 boot 镜像或在已生成的 userdata 上省略参数，不会恢复已经写入的设置状态。输出沿用 `giulia-ios16-userdata.img` 和 `root.json`；脚本不会连接或刷写手机。

开启后合并四份 mobile 偏好，保留已有其他字段：

| 偏好域 | 写入内容 |
| --- | --- |
| `com.apple.purplebuddy` | `SetupDone=true`、`SetupFinishedAllSteps=true`、`SetupVersion=11`、`daemonizeNeedsToRun=false` |
| `com.apple.purplebuddy.notbackedup` | 关闭 diagnostics、HSA2、keychain、Mesa、payment、standard 和 silent ICDP mini-buddy |
| `com.apple.managedconfiguration.notbackedup` | `LockdownActivationIndicatesCloudConfigurationAvailable=false` |
| `com.apple.springboard` | `MCForceShowCloudConfigurationUI=false` |

这些字段参考本地 iOS16 `ios-sweet` 实现。每份 plist 在镜像内写入后读回验证，所有者和权限检查为 `501:501 / 0600`。`root.json` 记录 `skip_setup`，开启时另有 `setup_completion_seeded=true` 及偏好改动摘要。

实机使用该 userdata 后，曾先显示 Apple Logo，随后进入 “Software Update Complete”，点击 Continue 后仍要求连接网络或 Mac/PC。四份偏好没有完成本机激活状态处理，因此当前不能把 `--skip-setup` 描述为已验证可直接进入主屏。

`scripts/make-ios16-adt.py --development-setup` 另提供显式启用的实验开发板路径，必须与 `--gui`、原有 `--product-adt` 及其他所需参数一起使用。省略该参数时，生成器不会写入以下四个 `/chosen` 属性：

| 属性 | LE u32 值 |
| --- | --- |
| `certificate-production-status` | 0 |
| `effective-production-status-ap` | 0 |
| `certificate-security-mode` | 1 |
| `effective-security-mode-sep` | 1 |

这组值来自 `ios-sweet/tools/mkadt.py`。已对照本地原始 20H392 `mobileactivationd`：`DeviceType.init` 在四项 MobileGestalt 值为 `0,0,1,1` 时设置原生 `should_hactivate` 状态。当前 Ace 5 ADT 缺少这四项。下一轮先使用新 ADT 与现有 userdata 验证原始守护进程能否自行处理本机状态；不重建 userdata，不写 `data_ark.plist`，不修改系统二进制。

该开发板路径尚未通过 Ace 5 实机验证。跳过向导或本机开发状态都不表示 Apple 账户、App Store、DRM 或其他在线服务可用；userdata 工具不写入激活记录，报告中的 `activation_state_seeded` 保持 `false`，`setup_skip.hardware_verified` 仍为 `false`。
