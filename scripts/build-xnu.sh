#!/usr/bin/env bash
set -euo pipefail
port_root="$(cd "$(dirname "$0")/.." && pwd)"
work="${1:?Usage: build-xnu.sh /absolute/workspace}"
mkdir -p "$work"
python3 - "$port_root" "$work" <<'PY'
import json, pathlib, subprocess, sys
root, work = map(pathlib.Path, sys.argv[1:])
pin = json.loads((root/'sources.json').read_text())['unixnu']
source = work/'UniXNU'
if not source.exists():
    subprocess.run(['git','init',str(source)],check=True)
    subprocess.run(['git','-C',str(source),'remote','add','origin',pin['url']],check=True)
    subprocess.run(['git','-C',str(source),'fetch','--depth','1','origin',pin['commit']],check=True)
    subprocess.run(['git','-C',str(source),'checkout','--detach','FETCH_HEAD'],check=True)
assert subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip()==pin['commit']
PY
cd "$work/UniXNU"
python3 "$port_root/scripts/fix-upstream.py" "$work/UniXNU"
python3 unixnu defconfig giulia_defconfig -D "$port_root"
python3 unixnu fetch xnu libplatform libdispatch AvailabilityVersions libpthread corecrypto-2023 -D "$port_root"
python3 - "$port_root" <<'PY'
import sys
sys.path.insert(0, 'scripts')
from unixnu.cli import parser, prepare
from unixnu.config import Context
ctx = Context(parser().parse_args(['tree', '-D', sys.argv[1]]))
prepare(ctx, ['kernel'])
PY
python3 "$port_root/scripts/cache-public-headers.py" "$work/UniXNU"
tree="$work/UniXNU/out/giulia/tree"
# Archive builders need compile records. The upstream normal bootstrap asks for
# archives before generating those records, so prime using its unlinked baseline.
if [[ ! -f "$tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json" ]]; then
  set +e
  (cd "$tree" && XNU_SOURCE_CORECRYPTO14=0 XNU_ARMCPU=0 XNU_ARM_DT_PLATFORM=0 \
    XNU_BOOT_PROGRESS=0 bash scripts/build-xnu-4k-baseline.sh) > "$work/bootstrap.log" 2>&1
  bootstrap_result=$?
  set -e
  printf 'Baseline exit: %s (compile records are checked next)\n' "$bootstrap_result"
  test -s "$tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json"
  test -s "$tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/link.filelist"
fi
python3 unixnu build kernel -D "$port_root" -j 3
python3 unixnu build payload -D "$port_root"
python3 "$port_root/scripts/verify-payload.py" "$work/UniXNU/out/giulia/payload"
