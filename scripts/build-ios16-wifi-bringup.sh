#!/usr/bin/env bash
# Build against an existing, prepared Darwin22 UEFI checkout. No device access.
set -euo pipefail
if [ "$#" -ne 8 ] && [ "$#" -ne 9 ]; then
  echo 'Usage: build-ios16-wifi-bringup.sh UEFI UEFI_PYTHON KERNEL ADT ROOT_REPORT TRUST_CATALOG CAPTURED_DTS OUTPUT [LOCAL_FIRMWARE_PACK]' >&2
  exit 2
fi
port_root="$(cd "$(dirname "$0")/.." && pwd)"
uefi_root="$(realpath "$1")"
# Preserve a venv interpreter symlink: resolving it selects system Python.
uefi_python="$(realpath -s "$2")"
kernel_input="$(realpath "$3")"
adt_input="$(realpath "$4")"
root_report="$(realpath "$5")"
trust_input="$(realpath "$6")"
dts_input="$(realpath "$7")"
output_dir="$(realpath -m "$8")"
runtime_pack=""
if [ "$#" -eq 9 ]; then runtime_pack="$(realpath "$9")"; fi
driver_dir="$uefi_root/UniXNU/drivers/DarwinBoot/DarwinBootPkg/Driver"
board_driver="$uefi_root/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe"
test -x "$uefi_python"
"$uefi_python" -c 'import coloredlogs'
test ! -e "$output_dir"
python3 - "$driver_dir" <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1])
for name in ('BootArgs.h','DarwinBoot.c'):
    text=(p/name).read_text()
    if 'CommandLine[608]' not in text:
        raise SystemExit('Requires the prepared Darwin22 loader: '+name)
PY
mkdir -p "$(dirname "$output_dir")"
mkdir "$output_dir"
exec > >(tee "$output_dir/host-build.log") 2>&1
cd "$port_root"
runtime_checks=()
if [ -n "$runtime_pack" ]; then runtime_checks=(--runtime); fi
python3 tests/ios16_wifi_bundle.py --dts "$dts_input" --out "$output_dir/offline-checks.json" "${runtime_checks[@]}"
if [ -n "$runtime_pack" ]; then
  python3 scripts/embed-wifi-firmware.py --kernel "$kernel_input" --pack "$runtime_pack" --out "$output_dir/kernel.macho"
else
  cp "$kernel_input" "$output_dir/kernel.macho"
  cp "${kernel_input%.*}.json" "$output_dir/kernel.json"
fi
cp "$adt_input" "$output_dir/giulia-ios16.adt"
cp "$dts_input" "$output_dir/captured-board.dts"
cp -a uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe/. "$board_driver/"
cp -a uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe "$output_dir/driver-source"
python3 scripts/enable-touch-diagnostics.py "$driver_dir"
python3 scripts/enable-wifi-diagnostics.py "$driver_dir"
python3 scripts/make-ios16-payload.py --kernel "$output_dir/kernel.macho" \
  --adt "$output_dir/giulia-ios16.adt" --trust-catalog "$trust_input" \
  --out "$output_dir/payload" --root-report "$root_report" \
  --default-root --trace-all --wifi-pcie --enable-touch --touch-gpio
python3 scripts/verify-payload.py "$output_dir/payload"
python3 tests/ios16_dram_handoff.py --driver "$driver_dir" --adt "$output_dir/giulia-ios16.adt"
python3 - "$output_dir" "$board_driver" <<'PY'
from pathlib import Path
import json,sys
out,driver=map(Path,sys.argv[1:])
r=json.loads((out/'payload/identity.json').read_text())
(driver/'PayloadIdentity.h').write_text(
    f"#define GIULIA_PAYLOAD_BYTES {r['payload_bytes']}U\n"
    f"#define GIULIA_PAYLOAD_CRC32 0x{r['payload_crc32']}U\n"
    '#define GIULIA_TOUCH_CLOCK 1\n#define GIULIA_TOUCH_POWER 1\n#define GIULIA_WIFI_PCIE 1\n')
PY
python3 scripts/verify-wifi-native-abi.py --uefi "$uefi_root" --when-enabled
cd "$uefi_root"
if ! CLANGPDB_AARCH64_PREFIX=aarch64-linux-gnu- "$uefi_python" build_uefi.py \
     -d giulia -m 0 -r DEBUG > "$output_dir/build.log" 2>&1; then
  tail -40 "$output_dir/build.log"
  exit 1
fi
cd "$port_root"
python3 scripts/verify-image.py "$uefi_root" --output "$output_dir/firmware.json"
cp "$uefi_root/Mu-giulia-0.img" "$output_dir/firmware.img"
python3 scripts/pack-payload.py --firmware "$output_dir/firmware.img" \
  --firmware-report "$output_dir/firmware.json" --payload "$output_dir/payload/payload.img" \
  --payload-report "$output_dir/payload/payload.json" --header-version 3 \
  --output "$output_dir/giulia-ios16-wifi-pcie.img"
cp "$uefi_root/Build/giuliaPkg/DEBUG_CLANGPDB/BUILD_TOOLS_REPORT.json" "$output_dir/"
cp "$driver_dir/GiuliaWifiReport.h" "$driver_dir/GiuliaWifiEncoding.h" "$output_dir/driver-source/"
cp "$board_driver/PayloadIdentity.h" "$output_dir/driver-source/"
cp "$0" "$output_dir/build-script.sh"
python3 - "$output_dir" "$driver_dir" <<'PY'
from pathlib import Path
import hashlib,json,subprocess,sys
out,driver=map(Path,sys.argv[1:])
files=[*sorted((out/'driver-source').glob('*')),
       *sorted(Path('ios16').glob('giulia_wifi_*.h')),Path('ios16/GiuliaWiFi.cpp'),
       Path('ios16/giulia_display.h'),Path('scripts/ios16_display.py'),
       Path('scripts/embed-wifi-firmware.py'),Path('scripts/ios16_wifi_runtime.py'),
       out/'kernel.macho',out/'giulia-ios16.adt',out/'captured-board.dts',
       driver/'DarwinBoot.c',Path('scripts/enable-wifi-diagnostics.py'),
       Path('scripts/make-ios16-payload.py'),Path('scripts/verify-image.py'),
       Path('scripts/pack-payload.py'),Path('scripts/ios16_wifi_pcie.py')]
runtime=json.loads((out/'payload/payload.json').read_text()).get('wifi_runtime')
report={'repository_head':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
        'source_files':[{'path':str(p.resolve()),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
                        for p in files if p.is_file()],
        'hardware_verified':False,'scan_implemented':bool(runtime),'scan_hardware_verified':False,
        'runtime':runtime,'device_commands_executed':False}
(out/'source-manifest.json').write_text(json.dumps(report,indent=2)+'\n')
PY
echo 'Integrated Wi-Fi bring-up image built; device verification remains separate.'
