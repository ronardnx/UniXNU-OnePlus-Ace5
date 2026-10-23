"""PAS pack wire format, invalid inputs and cross-language loader checks."""
import copy
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import ios16_audio_pack as packer
from ios16_audio_firmware import ELF32, PHDR32


def metadata(machine, base, segments):
    header = ELF32.pack(b'\x7fELF\x01\x01\x01' + bytes(9), 2, machine, 1,
                        base, 52, 0, 0, 52, 32, len(segments), 0, 0, 0)
    return header + b''.join(PHDR32.pack(*p) for p in segments) + b'h' * 16


class AudioPack(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')
        cls.binary_dir = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.binary_dir.name) / 'audio-boot'
        if cls.compiler:
            subprocess.run([cls.compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(ROOT / 'tests/ios16_audio_boot.c'), '-o', str(cls.binary)],
                           check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.binary_dir.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.manifest = {'files': [], 'images': []}
        self.segments = {}
        for image, name in enumerate(packer.IMAGE_NAMES):
            pas_id, base, capacity = packer.IMAGE_LIMITS[image]
            count = 4 if image else 3
            entries = [(0, 0, 0, 0, 52 + count * 32, 0, 0x07000000, 0),
                       (1, 4096, base, base, 7 + image, 16, 0x08000005, 4096)]
            if image:
                entries.append((1, 8192, base + 4096, base + 4096, 0, 16, 0x08000006, 4096))
            entries.append((0, 0, 0, 0, 16, 16, 0x02000000, 0))
            self.segments[name] = entries
            self.put(name, metadata((1, 164)[image], base, entries))
            self.put(name[:-4] + '.b01', bytes([0x31 + image]) * (7 + image))
            self.manifest['images'].append({'path': name, 'pas_id': pas_id,
                                           'base': base, 'bytes': capacity})

    def put(self, name, data):
        (self.folder / name).write_bytes(data)
        self.manifest['files'] = [x for x in self.manifest['files'] if x['path'] != name]
        self.manifest['files'].append({'path': name, 'bytes': len(data),
                                       'sha256': hashlib.sha256(data).hexdigest()})

    def build(self):
        return packer.build_pack(self.folder, self.manifest)

    def check_c(self, content):
        if not self.compiler:
            self.skipTest('A C compiler is required for the independent loader check')
        path = self.folder / 'firmware.gafw'
        path.write_bytes(content)
        return subprocess.run([str(self.binary), str(path)], capture_output=True, text=True)

    def test_deterministic_wire_format_and_bss(self):
        data = self.build()
        self.assertEqual(data, self.build())
        self.assertEqual(data[:8], b'GAFWPK1\0')
        self.assertEqual(len(data), 5 * 4096)
        self.assertEqual(struct.unpack_from('<6I', data, 8), (len(data), 2, 3, 1, 64, 192))
        self.assertEqual(struct.unpack_from('<8I', data, 256),
                         (1, 2, 4096, 16, 0, 0, 0x08000006, 0))
        self.assertEqual(struct.unpack_from('<I', data, 64)[0], 36)
        self.assertEqual(struct.unpack_from('<I', data, 128)[0], 1)
        self.assertEqual(data[8192:8199], b'1111111')
        self.assertEqual(data[16384:16392], b'22222222')
        self.assertEqual(data[8199:12288], bytes(4089))
        self.manifest['files'].reverse()
        self.manifest['images'].reverse()
        self.assertEqual(data, self.build())

    def test_generated_pin_matches_exact_pack(self):
        data = self.build()
        pin = packer.pin_header(data)
        self.assertIn('#define GA_FIRMWARE_PACK_BYTES 20480u', pin)
        self.assertIn(','.join(f'0x{x:02x}' for x in hashlib.sha256(data).digest()), pin)

    def test_python_pack_accepted_by_c_loader(self):
        result = self.check_c(self.build())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('3 segments, DTB span 4096, ADSP span 8192', result.stdout)

    def test_python_rejects_metadata_unusable_by_c(self):
        original = copy.deepcopy(self.segments['adsp.mdt'])
        for index, field, value in ((1, 7, 3), (1, 6, 5), (2, 3, 0x9df00000),
                                    (3, 5, 17), (3, 6, 0x01000000)):
            with self.subTest(index=index, field=field):
                entries = copy.deepcopy(original)
                entry = list(entries[index])
                entry[field] = value
                entries[index] = tuple(entry)
                self.put('adsp.mdt', metadata(164, 0x9df00000, entries))
                with self.assertRaises(ValueError):
                    self.build()

    def test_pinned_image_cannot_change_destination_or_pas(self):
        for field, value in (('base', 0x9de70000), ('bytes', 0x100000), ('pas_id', 35)):
            old = self.manifest['images'][0][field]
            self.manifest['images'][0][field] = value
            with self.assertRaises(ValueError):
                self.build()
            self.manifest['images'][0][field] = old

    def test_second_read_is_bounded_and_pinned(self):
        verify = packer.verify_firmware
        def changing_verify(*args):
            result = verify(*args)
            (self.folder / 'adsp.b01').write_bytes(b'x' * 10000)
            return result
        with mock.patch.object(packer, 'verify_firmware', changing_verify):
            with self.assertRaisesRegex(ValueError, 'changed while packing'):
                self.build()

    def test_package_limit_includes_padding(self):
        with mock.patch.object(packer, 'MAX_PACK_BYTES', 4 * 4096):
            with self.assertRaisesRegex(ValueError, 'loader capacity'):
                self.build()

    def test_c_rejects_modified_destination(self):
        data = bytearray(self.build())
        struct.pack_into('<I', data, 200, 4096)
        result = self.check_c(data)
        self.assertNotEqual(result.returncode, 0)

    def test_local_stock_pack_matches_compiled_pin_and_c_loader(self):
        folder = ROOT / '.cache/audio-firmware'
        if not folder.is_dir():
            self.skipTest('Local stock firmware is intentionally not included in source')
        manifest = json.loads((ROOT / 'hardware/sm8650-audio.json').read_text(encoding='utf-8'))
        data = packer.build_pack(folder, manifest['firmware'])
        self.assertEqual(len(manifest['firmware']['files']), 56)
        self.assertEqual(packer.pin_header(data),
                         (ROOT / 'ios16/audio/giulia_audio_firmware_pin.h').read_text(encoding='utf-8'))
        self.assertEqual(len(data), 41603072)
        result = self.check_c(data)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('56 segments, DTB span 57344, ADSP span 86507520', result.stdout)


if __name__ == '__main__':
    unittest.main()
