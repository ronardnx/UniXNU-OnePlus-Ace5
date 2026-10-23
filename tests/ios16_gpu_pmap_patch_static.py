"""Check the checked-in 0086 patch against the generator text, without XNU.

The full check (`ios16_gpu_pmap_patch.py --check-static`) needs the actual
patched XNU tree. This test only proves that the static patch is a diff of the
current generator output and that it touches exactly one source file.
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from ios16_gpu_pmap_patch import PATHS, _GUARD, _STATE

patch = (ROOT / 'ios16/0086-giulia-gpu-pmap.patch').read_text(encoding='utf-8')
files = [line for line in patch.splitlines() if line.startswith('+++ ')]
assert files == ['+++ b/' + name for name in PATHS], files
for text, name in ((_STATE, 'state'), (_GUARD, 'guard')):
    for line in text.strip('\n').splitlines():
        if not line.strip():
            continue
        assert '+' + line in patch, f'{name} line missing from static patch: {line}'
for anchor in ('#include "../../../iokit/Kernel/arm/giulia_gpu_pmap.h"',
               'giulia_gpu_fence_cpu_boot_validate(__builtin_arm_rsr64("TCR_EL1"),',
               'giulia_gpu_fence_cpu_boot_map_check(start, end - start, PAGE_SIZE);',
               'giulia_gpu_fence_cpu_boot_retired();',
               'giulia_gpu_pmap_guard_enter(pa, flags)'):
    assert anchor in patch, anchor
assert patch.count('\n+++ ') == 1
print('GPU pmap static patch matches the generator state/guard text and touches '
      'only osfmk/arm/pmap/pmap.c')
