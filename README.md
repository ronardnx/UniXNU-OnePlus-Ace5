# UniXNU for OnePlus Ace 5

**English** | [简体中文](README.zh-CN.md)

Experimental UEFI and Darwin/XNU port for the OnePlus Ace 5 (PKG110, `giulia`, Qualcomm SM8650). The current development target is iOS 16 / Darwin 22.

The repository provides board configuration, pinned upstream sources, kernel and UEFI patches, build tools, and validation records. Apple system images and device firmware are not included.

> [!WARNING]
> This project is under development and is not suitable for daily use. Flashing partitions or replacing userdata can cause data loss. Build success does not establish hardware support.

## Hardware support

Status as of **2026-10-23**. Results apply to the configurations recorded in [`hardware/`](hardware/).

| Component | Status | Scope and limitations |
| --- | --- | --- |
| Boot and kernel | Verified | UEFI handoff, XNU startup and CPU0; single-core configuration |
| Storage | Verified | Native UFS access and the prepared HFSX root volume |
| Debug console | Verified | USB CDC serial transport and startup log capture |
| User interface | Verified | Hello / Setup starts; complete system functionality remains under development |
| Display and touch | Verified | Full 1264×2780 panel coverage and aligned touch in Setup; UIKit uses a scaled 1125×2436 canvas |
| Wi-Fi | Not operational | PCIe, BHI/BHIE, AMSS and QMI resource requests verified; currently times out waiting for firmware-ready notification before scanning |
| GPU | Preparation service | A750 firmware/RAM preparation, inactive SMMU tables and gated observation; no GPU execution or Metal acceleration |
| Audio, cellular and DRM | Unverified | No complete implementation or device validation |

Display rotation, power transitions and application compatibility require further testing. Wi-Fi association, authentication and packet transport are not implemented.

See [development status](docs/DEVELOPMENT_STATUS.md) and the [firmware transfer test record](hardware/sm8650-wifi-root-master.json) for evidence and remaining work.

## Requirements

- Linux or Ubuntu under WSL for UEFI and local image packaging.
- Git and Python 3.11 or newer with `venv`.
- Clang/LLVM, LLD, build-essential, NASM, `uuid-dev`, `gcc-aarch64-linux-gnu`, ACPICA tools and the device-tree compiler.
- Mono and certificate packages for the upstream NuGet tooling; Mtools for FAT payload packaging.
- A macOS runner for XNU. The configured GitHub Actions build account is `jasper2863`.

Source revisions are fixed in [`sources.json`](sources.json) and the target-specific workflows. Kernel, device tree, firmware and payload metadata must match.

The GUI kernel workflow also accepts `gpu=true` alongside touch and Wi-Fi. Local unified packaging uses `GIULIA_GPU_PACK` and `GIULIA_GPU_MODE=1/2`; USB `gpu.status` / `gpu.observe` expose preparation and gated observation. See [GPU bring-up](docs/GPU_BRINGUP.md) for inputs and remaining hardware work.

Committed UEFI and UniXNU development snapshots are also tracked as Git submodules under `sources/`. Download the recorded revisions with:

```bash
git submodule update --init sources/uefi-giulia sources/UniXNU
```

## Build

### UEFI

From the repository root:

```bash
bash scripts/build-uefi.sh "$HOME/ace5-port"
```

Output: `$HOME/ace5-port/uefi-giulia/Mu-giulia-0.img`. The first build downloads upstream dependencies. This command does not flash the phone.

### iOS 16 kernel

```bash
gh workflow run ios16-xnu.yml --repo jasper2863/UniXNU-giulia \
  --ref main -f profile=gui -f touch=true -f wifi=true
```

The workflow produces the `ace5-ios16-xnu` artifact. GUI packaging also requires the verified local product ADT, root-volume report, trust catalogue and matching WLAN firmware. Firmware is embedded locally after CI and is not uploaded with kernel sources.

[`scripts/build-ios16-wifi-bringup.sh`](scripts/build-ios16-wifi-bringup.sh) assembles and validates the integrated image from those inputs and an existing prepared UEFI checkout. Run it without arguments to display the required paths. Use a new output directory for each build.

To skip the first-boot Setup UI in a new userdata image, pass `--skip-setup` to `scripts/rebuild-ios16-userdata.py`. The switch defaults to off; omit it or use `--no-skip-setup` to retain Setup. See the [Setup switch guide](docs/SETUP_SWITCH.md) for build commands and verification.

## Validation

```bash
git diff --check
python3 -m unittest discover -s tests -v
bash scripts/test-ios16-wifi-runtime.sh
```

The runtime checks cover PCI configuration, DMA isolation, firmware transport, scan protocol handling and display bounds. These are host tests; they do not demonstrate a working radio link. Record build results, image validation and physical-device results separately.

## Repository layout

For an interactive USB debug console on Windows CMD, run `scripts\debug-terminal.cmd`. Protocol 2 diagnostics, explicit MMIO access and `reboot.fastboot` have been tested on the device. The PTY shell still requires a separately staged daemon and signed ARM64 shell/tools and has not been validated on hardware. See [USB terminal instructions](docs/USB_TERMINAL.md) for deployment and commands.

| Path | Purpose |
| --- | --- |
| `board.toml`, `configs/`, `targets/` | Board and target configuration |
| `sources.json`, `patches/`, `uefi-overlay/` | Upstream pins and UEFI integration |
| `sources/uefi-giulia`, `sources/UniXNU` | Development source snapshots pinned as Git submodules |
| `ios16/` | Darwin 22 drivers and compatibility patches |
| `scripts/`, `tests/` | Build, packaging, diagnostic and test tools |
| `hardware/`, `docs/` | Validation records and development documentation |
| `.github/workflows/` | Manual CI workflows |
| `out/`, `.cache/`, `reports/` | Local artifacts; excluded from version control |

## Contributing

Follow [`CONTRIBUTING.md`](CONTRIBUTING.md). Include the affected subsystem, reproducible checks, expected and actual behavior, and the tested image or commit. Keep logs redacted and proprietary firmware, credentials and device-unique data out of commits.

## License

Repository code is licensed under [BSD-2-Clause-Patent](LICENSE). Upstream components and firmware retain their respective licenses.
