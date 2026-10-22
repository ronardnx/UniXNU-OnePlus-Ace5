# UniXNU for OnePlus Ace 5

**Experimental UEFI/XNU bring-up for the OnePlus Ace 5 (`giulia`, Qualcomm SM8650).**

This repository contains board-specific configuration, upstream source pinning, patches, build scripts, GitHub Actions workflows, and validation records for investigating whether UniXNU and Darwin/XNU can be brought up on the OnePlus Ace 5.

> [!WARNING]
> This is an experimental hardware bring-up project, not a daily-driver operating system or a complete iOS port. Do not assume that a successful build means a feature works on the phone. Flashing images or modifying userdata can cause data loss or leave the device unbootable. Use temporary boot workflows only when you understand their limitations, keep verified backups, and do not run destructive commands from unreviewed instructions.

## Project status

The project has reached the iOS 16 / Darwin 22 development path. According to the latest recorded device test (2026-10-22), the device has reached the Hello/Setup interface with a development image.

| Area | Current evidence | Status |
|---|---|---|
| UEFI hand-off and boot flow | Device-side bring-up records and boot tests | Verified for the recorded test configuration |
| XNU startup | Memory-management initialization, platform-driver registration, CPU0 registration and SGI self-test recorded | Partially verified |
| UFS and debug console | Native UFS read test and USB CDC serial communication recorded | Verified for the tested paths |
| Root volume and user space | HFSX root volume mounted read/write; `launchd` and system services started | Verified for the recorded setup |
| Graphical interface | Hello/Setup interface observed on device | Reached; not a complete or stable desktop validation |
| Touch | Controller identity and touch reports observed; GPIO fallback remains unverified | In progress |
| Display | Native-panel geometry and offline bounds checks recorded | Offline checks passed; full display reliability not established |
| Wi-Fi | WCN7850 PCIe link has remained at `Detect.Quiet`; no confirmed endpoint enumeration or real scan results | Not working / in progress |
| GPU, audio, cellular and DRM | No complete device-side validation recorded | Not implemented or unverified |

See [Development status and validation notes](docs/DEVELOPMENT_STATUS.md) for details. Machine-readable evidence is kept under [`hardware/`](hardware/).

## Repository layout

- `board.toml`, `configs/` — board and kernel configuration.
- `sources.json` — pinned upstream source information.
- `patches/` — project-specific patches.
- `scripts/` — build, packaging, device-audit and validation helpers.
- `ios16/`, `targets/`, `uefi-overlay/` — platform-specific build inputs and overlays.
- `hardware/` — machine-readable hardware notes and test evidence.
- `tests/` — automated/offline tests.
- `.github/workflows/` — CI and build workflows.
- `LICENSE` — repository license.

## Requirements

The documented UEFI build path targets Linux, including Ubuntu under WSL. The scripts and workflows for the iOS 16/XNU path may have additional environment and artifact requirements; inspect the relevant workflow and script before running them.

The recorded UEFI build dependencies include:

- Git and Python 3 with `venv`
- build-essential, Clang/LLVM and LLD
- `uuid-dev`, NASM, `gcc-aarch64-linux-gnu`
- ACPICA tools and the device-tree compiler
- Mono and Mono certificate packages where required by upstream NuGet tooling

Install the dependencies appropriate to your distribution before building.

## Build

The repository documents the following UEFI build command:

```bash
bash scripts/build-uefi.sh "$HOME/ace5-port"
```

The expected output recorded by the project is:

```text
$HOME/ace5-port/uefi-giulia/Mu-giulia-0.img
```

The build script is documented as a host-side build and does not itself run `adb`/`fastboot` or flash the device. The initial upstream setup downloads submodules and may take some time.

A device-tree comparison can be run with a local DTB and the upstream `giulia` DTB:

```bash
python3 scripts/check-device.py /path/to/running-device.dtb \
  "$HOME/ace5-port/uefi-giulia/Resources/DTBs/giulia.dtb" \
  --output reports/device-check.json
```

Replace the example input path with a DTB you are authorized to inspect. Do not publish device-unique dumps.

For other targets, use the corresponding workflow under [`.github/workflows/`](.github/workflows/) and review its inputs and outputs. Build commands, image names and validation criteria can differ by profile.

## Validation principles

Treat these as separate milestones:

1. **Build success** — source compiled and an artifact was produced.
2. **Artifact validation** — image structure, hashes, payload and hand-off metadata passed host-side checks.
3. **Device boot validation** — a specific image was tested on physical hardware.
4. **Feature validation** — the relevant hardware path was demonstrated to work on-device.

A passing earlier milestone does not imply that a later milestone passed. In particular, offline Wi-Fi protocol tests and a running `wifid` process do not establish that Wi-Fi scanning, association or data transfer works.

## Safety and privacy

- Prefer `fastboot boot` for controlled temporary-boot testing when supported; it is not a guarantee that every action is non-destructive.
- Do not flash partitions, format storage, or replace userdata unless you have independently reviewed the exact command, confirmed the target device and partition, and made a recoverable backup.
- Keep original images and record SHA-256 hashes before and after transformations.
- Never commit serial numbers, factory MAC addresses, account tokens, credentials, private keys, unredacted device dumps, or other device-unique/private data.
- Do not upload proprietary firmware, user data, or extracted artifacts unless you have the rights and authorization to redistribute them. Keep locally embedded firmware out of CI artifacts unless redistribution is explicitly permitted.
- Test reports should state the exact image/configuration tested and clearly distinguish host-side checks from physical-device results.

## Contributing

Contributions are welcome, especially reproducible bug reports, documentation fixes, host-side tests, and clearly scoped hardware investigations.

Before opening a pull request:

1. Explain the goal and affected hardware/software path.
2. Include the exact commands and environment needed to reproduce host-side checks.
3. Identify whether each result is offline-only or verified on physical hardware.
4. Attach relevant redacted logs or machine-readable reports, without device-unique secrets.
5. Avoid claiming support based only on compilation or static checks.

For hardware reports, include the tested image identifier or commit, device model/firmware context where safe to disclose, expected result, actual result, and the relevant evidence file.

## License

This repository includes a `LICENSE` file declaring **BSD-2-Clause-Patent**. Review that file for the complete terms. Upstream projects, third-party components, firmware and generated artifacts may be subject to separate licenses; the repository license does not automatically relicense those materials.
