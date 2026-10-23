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
| Power and volume keys | Verified in minimal key build | Physical power key turns the panel off/on; volume UI responds. See [validation](docs/DISPLAY_POWER.md) |
| Alert slider | Unverified | AK09970 driver compiled; sensor reads and three-position calibration pending |
| Wi-Fi | Networking unverified | PCIe, BHI/BHIE, AMSS and QMI resource requests verified; latest device run stops at QMP open before firmware-ready. Station/WPA2/data-path host tests pass |
| GPU | Preparation service | A750 firmware/RAM preparation, inactive SMMU tables and gated observation; no GPU execution or Metal acceleration |
| Audio | Experimental runtime cores | Pinned ADSP/PAS, GLINK/GPR, APM and dual SIA9177 code; native resource/PCM provider incomplete; no device playback or recording validation |
| Cellular and DRM | Unverified | No complete implementation or device validation |

Display rotation, full system suspend/resume and application compatibility require further testing. Physical power-key display off/on was verified separately in the minimal key build. Wi-Fi includes station association, WPA2/WPA3-SAE, PMF, directed/background scanning, basic roaming, REO queue reclamation and Ethernet transport. These changes have not been built into a new kernel or tested on the device. See [networking implementation and limits](docs/WIFI_NETWORK.md) and [firmware-readiness evidence](docs/WIFI_FIRMWARE_READY.md).

See [development status](docs/DEVELOPMENT_STATUS.md), [audio bring-up](docs/AUDIO_BRINGUP.md) and the [firmware transfer test record](hardware/sm8650-wifi-root-master.json) for evidence and remaining work.

2026-10-24 firmware-startup audit: Wi-Fi now installs the board PDC defaults before MHI and advertises WLAN_EN support plus the mapped SMMU IOVA range in HOST_CAP. Host regression tests pass; a new image and device capture are still needed to establish `FW_INIT_DONE`. See [startup corrections](docs/WIFI_FIRMWARE_READY.md).

The follow-up [upstream call-chain audit](docs/WIFI_UPSTREAM_CHAIN.md) maps platform power, PCIe/MHI, QMI, readiness and teardown to pinned OnePlus sources. It adds registration-status handling, firmware pin-result reporting and a cached `wifi.status` boot snapshot. Cold-boot calibration, RDDM/recovery and suspend/resume remain incomplete; no new device validation is claimed.

The [modem integration map](docs/MODEM_BRINGUP.md) covers hardware resources, firmware startup, SIM/data protocols and iOS interface dependencies. The unified lifecycle, shared resource broker, QMI/QMUX/QMAP codecs and recovered 20H392 ABI share the build and packaging contracts. Native MPSS/PDP providers are not yet connected; managed cellular remains gated.

## Requirements

- Linux or Ubuntu under WSL for UEFI and local image packaging.
- Git and Python 3.11 or newer with `venv`.
- Clang/LLVM, LLD, build-essential, NASM, `uuid-dev`, `gcc-aarch64-linux-gnu`, ACPICA tools and the device-tree compiler.
- Mono and certificate packages for the upstream NuGet tooling; Mtools for FAT payload packaging.


Source revisions are fixed in [`sources.json`](sources.json) and the target-specific workflows. Kernel, device tree, firmware and payload metadata must match.

The GUI kernel workflow also accepts `gpu=true` alongside touch and Wi-Fi. Local unified packaging uses `GIULIA_GPU_PACK` and `GIULIA_GPU_MODE=1/2`; USB `gpu.status` / `gpu.observe` expose preparation and gated observation. See [GPU bring-up](docs/GPU_BRINGUP.md) for inputs and remaining hardware work, and the [GPU stack framework](docs/GPU_STACK_FRAMEWORK.md) for the ordered stage machine, gates and milestones.

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
gh workflow run ios16-xnu.yml --repo tycykp/UniXNU-OnePlus-Ace5 \
  --ref main -f profile=gui -f touch=true -f wifi=true
```

The workflow produces the `ace5-ios16-xnu` artifact. GUI packaging also requires the verified local product ADT, root-volume report, trust catalogue and matching WLAN firmware. Firmware is embedded locally after CI and is not uploaded with kernel sources.

[`scripts/build-ios16-wifi-bringup.sh`](scripts/build-ios16-wifi-bringup.sh) assembles and validates the integrated image from those inputs and an existing prepared UEFI checkout. Run it without arguments to display the required paths. Use a new output directory for each build.

To skip the first-boot Setup UI in a new userdata image, pass `--skip-setup` to `scripts/rebuild-ios16-userdata.py`. The switch defaults to off; omit it or use `--no-skip-setup` to retain Setup. See the [Setup switch guide](docs/SETUP_SWITCH.md) for build commands and verification.

## Validation

```bash
git diff --check
```

Build and packaging commands validate their inputs and generated artifacts. Record build results, image validation and physical-device results separately; hardware support requires device evidence.

See [Wi-Fi mappings](docs/WIFI_MAPPING.md) for MMIO resources, CPU/physical/IOVA address translation, resource cleanup and the supported iOS requests.

See [GPU mappings](docs/GPU_MAPPING.md) for GPU/GMU permissions, firmware allocation lifecycle, HFI resource coverage and the remaining dedicated backends.

## Repository layout

Driver sources are organized into 13 modules under `ios16/`. Shared resource arbitration lives in the platform layer; optional driver registration and input startup are assembled centrally. See the [driver module rules](docs/DRIVER_MODULES.md) for ownership, allowed dependencies, and validation commands.

For an interactive USB debug console on Windows CMD, run `scripts\debug-terminal.cmd`. Protocol 2 diagnostics, explicit MMIO access and `reboot.fastboot` have been tested on the device. The PTY shell still requires a separately staged daemon and signed ARM64 shell/tools and has not been validated on hardware. See [USB terminal instructions](docs/USB_TERMINAL.md) for deployment and commands.

| Path | Purpose |
| --- | --- |
| `board.toml`, `configs/`, `targets/` | Board and target configuration |
| `sources.json`, `patches/`, `uefi-overlay/` | Upstream pins and UEFI integration |
| `sources/uefi-giulia`, `sources/UniXNU` | Development source snapshots pinned as Git submodules |
| `ios16/` | Darwin 22 drivers and compatibility patches |
| `scripts/` | Build, packaging and diagnostic tools |
| `hardware/`, `docs/` | Validation records and development documentation |
| `.github/workflows/` | Manual CI workflows |
| `out/`, `.cache/`, `reports/` | Local artifacts; excluded from version control |

## Contributing

Follow [`CONTRIBUTING.md`](CONTRIBUTING.md). Include the affected subsystem, reproducible checks, expected and actual behavior, and the tested image or commit. Keep logs redacted and proprietary firmware, credentials and device-unique data out of commits.

## License

Repository code is licensed under [BSD-2-Clause-Patent](LICENSE). Upstream components and firmware retain their respective licenses.
