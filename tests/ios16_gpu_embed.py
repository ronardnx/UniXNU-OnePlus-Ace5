import hashlib
import runpy
import struct
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
append = runpy.run_path(str(root / 'scripts/embed-gpu-firmware.py'))['append_pack']
pack = bytearray(4096)
pack[:8] = b'GGFWPK1\0'
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

# GPU and Wi-Fi segments coexist in either insertion order.
wifi_append = runpy.run_path(str(root / 'scripts/embed-wifi-firmware.py'))['append_pack']
wp = bytearray(128); wp[:8] = b'GWFWPK1\0'; struct.pack_into('<I', wp, 16, len(wp))
wd = hashlib.sha256(wp).digest()
for first_gpu in (True, False):
    if first_gpu:
        first, fm = append(bytes(kernel), bytes(pack), digest, required=False)
        both, sm = wifi_append(first, bytes(wp), wd, required=False)
        first_pack = pack
    else:
        first, fm = wifi_append(bytes(kernel), bytes(wp), wd, required=False)
        both, sm = append(first, bytes(pack), digest, required=False)
        first_pack = wp
    assert both[fm['file_offset']:fm['file_offset']+len(first_pack)] == first_pack
    assert struct.unpack_from('<I', both, 16)[0] == 3
print('GPU/Wi-Fi segment coexistence in both orders: passed')

if len(sys.argv)==2:
    sys.path.insert(0,str(root/'scripts'))
    from ios16_gpu_runtime import check_runtime
    actual=Path(sys.argv[1]).read_bytes()
    actual_digest=hashlib.sha256(actual).digest()
    markers=(actual_digest+b'GIULIA GPU: host preparation complete;\0'
             b'GIULIA GPU: firmware pack mapped read-only and digest verified\0'
             b'GIULIA FB: native panel 1264x2780 full coverage;\0')
    bound=bytearray(kernel);bound[0x1000:0x1000+len(markers)]=markers
    binary,firmware=append(bytes(bound),actual,actual_digest)
    metadata={'gpu_runtime_included':True,'gpu_firmware':firmware}
    assert check_runtime(binary,metadata)['acceleration'] is False
    for mode in range(3):
        altered=bytearray(binary);report=dict(metadata)
        if mode==0:altered[-1]^=1
        elif mode==1:report['gpu_firmware']=dict(firmware,bytes=1)
        else:report['gpu_runtime_included']=False
        try:check_runtime(bytes(altered),report)
        except ValueError:pass
        else:raise AssertionError('Invalid GPU embedding accepted')
    print('Pinned pack/kernel/metadata binding and corruptions: passed')
