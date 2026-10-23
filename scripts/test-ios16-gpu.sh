#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined tests/uefi_gpu_cold_clocks.c -o "$tmp/cold-clocks"
"$tmp/cold-clocks"
python3 tests/ios16_gpu_vendor.py
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    tests/ios16_gpu_firmware.c -o "$tmp/firmware"
"$tmp/firmware" "$@"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    tests/ios16_gpu_runtime.c -o "$tmp/runtime"
if [ "$#" = 1 ] && [ -f "$(dirname "$1")/firmware.pack" ]; then
    "$tmp/runtime" "$(dirname "$1")/firmware.pack"
else
    "$tmp/runtime"
fi
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
    tests/ios16_gpu_terminal.c -o "$tmp/terminal"
"$tmp/terminal"
python3 tests/ios16_gpu_terminal_profile.py
python3 tests/ios16_gpu_service.py
python3 tests/ios16_gpu_platform_runtime.py
python3 tests/ios16_gpu_mapping_runtime.py
python3 tests/ios16_gpu_dev_runtime.py
python3 tests/ios16_gpu_ipcc_clients.py
python3 tests/ios16_gpu_ipc_raw.py
python3 tests/ios16_gpu_watch_boundary.py
if [ "$#" = 1 ] && [ -f "$(dirname "$1")/firmware.pack" ]; then
    python3 tests/ios16_gpu_host_memory.py "$(dirname "$1")/firmware.pack"
else
    python3 tests/ios16_gpu_host_memory.py
fi
python3 tests/ios16_gpu_cache.py
python3 tests/ios16_gpu_probe.py
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    tests/ios16_gpu_scm.c -o "$tmp/scm"
"$tmp/scm"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    tests/ios16_gpu_mapping.c ios16/vendor/ArmSmmu4K/ArmSmmu4K.c -o "$tmp/mapping"
"$tmp/mapping"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    tests/ios16_gpu_memstore.c -o "$tmp/memstore"
"$tmp/memstore"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    tests/ios16_gpu_ipc.c ios16/vendor/ArmSmmu4K/ArmSmmu4K.c -o "$tmp/ipc"
"$tmp/ipc"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -pthread \
    tests/ios16_gpu_ipcc_owner.c -o "$tmp/ipcc-owner"
"$tmp/ipcc-owner"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    tests/ios16_gpu_fence.c -o "$tmp/fence"
"$tmp/fence"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    tests/ios16_gpu_fence_mapping.c -o "$tmp/fence-mapping"
"$tmp/fence-mapping"
cc -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation -fsanitize=address,undefined -fno-sanitize-recover=all -pthread \
    tests/ios16_gpu_fence_provider.c -o "$tmp/fence-provider"
"$tmp/fence-provider"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    tests/ios16_gpu_platform.c -o "$tmp/platform"
if [ "$#" = 1 ]; then
    "$tmp/platform" "$(dirname "$1")/gen70900_zap.mbn"
else
    "$tmp/platform"
fi
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    tests/ios16_gpu_hfi.c -o "$tmp/hfi"
"$tmp/hfi"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    tests/ios16_gpu_hwsched.c -o "$tmp/hwsched"
"$tmp/hwsched"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    tests/ios16_gpu_ring.c -o "$tmp/ring"
"$tmp/ring"
python3 scripts/derive-a750-native.py --check
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    tests/ios16_gpu_a750_native.c -o "$tmp/a750-native"
"$tmp/a750-native"
for name in prepare consumer; do
    cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
        "tests/ios16_gpu_power_$name.c" -o "$tmp/power-$name"
    "$tmp/power-$name"
done
python3 tests/ios16_gpu_power_handoff.py
python3 tests/ios16_gpu_qmp_transport.py
vendor=ios16/vendor/ArmSmmu4K
sources=("$vendor/ArmSmmu4K.c" "$vendor/ArmSmmu4KContext.c" "$vendor/ArmSmmu4KProgram.c" "$vendor/ArmSmmu4KStreams.c")
python3 scripts/make-gpu-smmu-oracle.py --out "$tmp/source-oracle.c"
python3 - "$vendor/ArmSmmu4KMmio.c" "$tmp/mmio-host.c" <<'PY'
from pathlib import Path
import sys
source=Path(sys.argv[1]).read_text()
arch='#if !defined(__aarch64__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__'
barrier='__asm__ volatile("dsb sy":::"memory");'
assert source.count(arch)==1 and source.count(barrier)==1
# Host-only normal-RAM fixture: address checks/accessors stay original, while
# DSB becomes a host memory barrier. Never included in a target kernel.
Path(sys.argv[2]).write_text(source.replace(arch,'#if 0').replace(barrier,'__sync_synchronize();'))
PY
clang --target=aarch64-none-elf -ffreestanding -O2 -S "$vendor/ArmSmmu4KMmio.c" -o "$tmp/mmio-aarch64.s"
python3 - "$tmp/mmio-aarch64.s" <<'PY'
from pathlib import Path
import re,sys
text=Path(sys.argv[1]).read_text()
assert re.search(r'\bdsb\s+sy\b',text)
assert re.search(r'\bldr\s+x\d+,\s*\[',text) and re.search(r'\bstr\s+x\d+,\s*\[',text)
print('Original AArch64 MMIO source emits DSB and 64-bit TTBR accesses; host tests use a labeled RAM-only barrier shim')
PY
for name in tables context program streams; do
    clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -O1 -I "$vendor" \
        "$vendor/test_$name.c" "${sources[@]}" "$tmp/source-oracle.c" "$tmp/mmio-host.c" -o "$tmp/$name"
    "$tmp/$name"
done
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -O1 -DGG_HOST_TEST \
    tests/ios16_gpu_control.c ios16/giulia_gpu_control.c "${sources[@]}" -o "$tmp/control"
if [ "$#" = 1 ] && [ -f "$(dirname "$1")/firmware.pack" ]; then
    "$tmp/control" "$(dirname "$1")/firmware.pack" | tee "$tmp/control.log"
else
    "$tmp/control" | tee "$tmp/control.log"
fi
python3 tests/ios16_gpu_log.py "$tmp/control.log"
c++ -x c++ -std=c++17 -Wall -Wextra -Werror -fsyntax-only \
    -include ios16/giulia_gpu_pack.h -include ios16/giulia_gpu_memory.h \
    -include ios16/giulia_gpu_power.h -include ios16/giulia_gpu_control.h /dev/null
python3 -m unittest discover -s tests -p 'test_gpu.py' -v

cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined tests/ios16_gpu_dev.c -o "$tmp/dev"
"$tmp/dev"
