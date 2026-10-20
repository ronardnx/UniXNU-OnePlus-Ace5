#!/usr/bin/env bash
set -euo pipefail
port_root="$(cd "$(dirname "$0")/.." && pwd)"
work="${1:?Usage: build-uefi.sh /absolute/build/workspace}"
python3 "$port_root/scripts/prepare.py" --workspace "$work"
python3 -m venv "$work/.venv"
"$work/.venv/bin/pip" install -r "$work/uefi-giulia/pip-requirements.txt"
cd "$work/uefi-giulia"
export CLANGPDB_AARCH64_PREFIX=aarch64-linux-gnu-
# Explicit setup/update also repairs an interrupted first build with an existing Build/.
"$work/.venv/bin/python" Platforms/OnePlus/giuliaPkg/DeviceBuild.py --setup -t DEBUG
"$work/.venv/bin/python" Platforms/OnePlus/giuliaPkg/DeviceBuild.py --update -t DEBUG
"$work/.venv/bin/python" build_uefi.py -d giulia -m 0 -r DEBUG
python3 "$port_root/scripts/verify-image.py" "$work/uefi-giulia" --output "$work/image-verification.json"
