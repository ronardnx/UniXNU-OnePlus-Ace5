"""Verify the linked A750 preparation service before local firmware embedding."""
import argparse
import json
import struct
import subprocess
from pathlib import Path


MARKERS = (
    b'GIULIA GPU: host preparation complete;',
    b'GIULIA GPU: firmware pack mapped read-only and digest verified',
    b'GIULIA FB: native panel 1264x2780 full coverage;',
    b'GiuliaGPU',
)
SYMBOLS = tuple('_giulia_gpu_' + name for name in (
    'power_acquire', 'power_release', 'smmu_start', 'smmu_stop',
    'gmu_load', 'gmu_start', 'log_event', 'terminal',
))


def check_kernel(kernel, symbols):
    if len(kernel) < 32:
        raise ValueError('Truncated GPU kernel')
    if struct.unpack_from('<4I', kernel) != (0xfeedfacf, 0x100000c, 0, 2):
        raise ValueError('Expected ARM64 Mach-O executable')
    missing = [m.decode() for m in MARKERS if m not in kernel]
    # Require actual defined symbols, rather than arbitrary occurrences in a
    # string or an undefined external reference in nm output.
    defined = set()
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[1] in ('T', 't'):
            defined.add(fields[2])
    missing += [name for name in SYMBOLS if name not in defined]
    if missing:
        raise ValueError('GPU kernel lacks linked implementation: ' + ', '.join(missing))
    return {'chip_id': '0x43051401', 'linked_symbols': list(SYMBOLS),
            'firmware_embedded': False, 'hardware_verified': False,
            'acceleration': False}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('kernel', type=Path)
    p.add_argument('--nm', nargs='+', default=['nm'], help='Symbol tool command, e.g. xcrun nm')
    a = p.parse_args()
    try:
        symbols = subprocess.check_output([*a.nm, str(a.kernel)], text=True)
        report = check_kernel(a.kernel.read_bytes(), symbols)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        p.exit(1, str(error) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
