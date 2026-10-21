import hashlib
import runpy
import struct
from pathlib import Path

root = Path(__file__).resolve().parents[1]
append = runpy.run_path(str(root / 'scripts/embed-wifi-firmware.py'))['append_pack']
pack = bytearray(128)
pack[:8] = b'GWFWPK1\0'
struct.pack_into('<I', pack, 16, len(pack))
digest = hashlib.sha256(pack).digest()
kernel = bytearray(0x8000)
struct.pack_into('<8I', kernel, 0, 0xfeedfacf, 0x100000c, 0, 2, 1, 152, 0, 0)
struct.pack_into('<II16sQQQQiiII', kernel, 32, 0x19, 152, b'__TEXT', 0xfffffe0007004000, 0x8000, 0, 0x8000, 5, 5, 1, 0)
struct.pack_into('<16s16sQQIIIIIIII', kernel, 104, b'__text', b'__TEXT', 0xfffffe0007008000, 0x4000, 0x4000, 14, 0, 0, 0, 0, 0, 0)
output, meta = append(bytes(kernel), bytes(pack), digest, required=False)
assert output[meta['file_offset']:] == pack
assert output[256:0x8000] == kernel[256:]
assert struct.unpack_from('<II', output, 16) == (2, 224)
for mode in range(5):
    k, p, d = bytearray(kernel), bytearray(pack), digest
    if mode == 0:
        k[184] = 1
    elif mode == 1:
        p[-1] = 1
    elif mode == 2:
        k = bytearray(output)
    elif mode == 3:
        struct.pack_into('<I', k, 104 + 48, 200)
    else:
        struct.pack_into('<I', k, 20, 153)
    try:
        append(bytes(k), bytes(p), d, required=False)
    except ValueError:
        pass
    else:
        raise AssertionError(mode)
try:
    append(bytes(kernel), bytes(pack), digest)
except ValueError:
    pass
else:
    raise AssertionError('Unsigned capability claims accepted')
print('Firmware embedding: byte preservation, segment bounds, duplicate/digest/header/kernel binding rejection passed')
