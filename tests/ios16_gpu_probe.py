"""Assemble the exact CM3 programs tested on Ace 5; no device is accessed.

Resolve the local R_ARM_ABS32 vector relocations so the check needs only clang,
including on macOS CI. These bytes must match the linked images used on-device.
"""
from pathlib import Path
import hashlib
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
cases = (
    ('giulia_gmu_read_probe.S', [], 144,
     '19d86acf6314707bf6518fdd98d748601c23b3cfffda5e5e96888875b51c346b'),
    ('giulia_gmu_read_probe.S', ['-DPROBE_IOVA=0x60002000'], 148,
     '21e1e27d3b6cd4282aa776903ae7b10be526960ca3a34aa18da7de6b702b1aac'),
    ('giulia_gmu_dma_probe.S', [], 180,
     'd66cb06a55e28283a30e335620fafe7d2d9deee63ef8b0eed72b0d744f3747eb'),
)
with tempfile.TemporaryDirectory() as folder:
    obj = Path(folder) / 'probe.o'
    for name, options, length, digest in cases:
        subprocess.run(['clang', '--target=armv7m-none-eabi', '-mcpu=cortex-m3',
                        '-mthumb', *options, '-c', str(ROOT/'tests/firmware'/name),
                        '-o', str(obj)], check=True)
        elf = obj.read_bytes()
        assert elf[:7] == b'\x7fELF\x01\x01\x01'
        assert struct.unpack_from('<HH', elf, 16) == (1, 40)  # relocatable ARM
        shoff = struct.unpack_from('<I', elf, 32)[0]
        entsize, count, strings_index = struct.unpack_from('<HHH', elf, 46)
        assert entsize == 40 and 0 < strings_index < count
        sections = [struct.unpack_from('<10I', elf, shoff+i*40) for i in range(count)]
        strings = sections[strings_index]
        names = elf[strings[4]:strings[4]+strings[5]]
        text_index = next(i for i, s in enumerate(sections)
                          if names[s[0]:].split(b'\0', 1)[0] == b'.text')
        section = sections[text_index]
        code = bytearray(elf[section[4]:section[4]+section[5]])
        relocated = []
        for rel in sections:
            if rel[1] != 9 or rel[7] != text_index:
                continue
            symbols = sections[rel[6]]
            assert symbols[1] == 2 and symbols[9] == 16 and rel[9] == 8
            for at in range(rel[4], rel[4]+rel[5], 8):
                target, info = struct.unpack_from('<II', elf, at)
                assert info & 255 == 2 and 4 <= target < 64 and not target % 4
                symbol = struct.unpack_from('<IIIBBH', elf, symbols[4]+(info >> 8)*16)
                assert symbol[5] == text_index
                addend = struct.unpack_from('<I', code, target)[0]
                struct.pack_into('<I', code, target, addend+symbol[1])
                relocated.append(target)
        assert sorted(relocated) == list(range(4, 64, 4))
        assert len(code) == length and hashlib.sha256(code).hexdigest() == digest
        vectors = struct.unpack_from('<16I', code)
        assert vectors[:2] == (0x10007f80, 0x41)
        assert all(x & 1 and 64 <= x < length for x in vectors[1:])
        assert len(set(vectors[2:])) == 1
print('Three CM3 diagnostics match the exact bytes used in live read, hole and read/write tests')
