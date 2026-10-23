"""Synthetic split ELF fixtures; no proprietary firmware or device is required."""
import copy
import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('ios16_audio_firmware', ROOT / 'scripts/ios16_audio_firmware.py')
audio = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audio)


def elf(machine, entry, segments):
    ident = b'\x7fELF\x01\x01\x01' + bytes(9)
    header = audio.ELF32.pack(ident, 2, machine, 1, entry, audio.ELF32.size,
                             0, 0, audio.ELF32.size, audio.PHDR32.size,
                             len(segments), 0, 0, 0)
    return header + b''.join(audio.PHDR32.pack(*segment) for segment in segments)


class AudioFirmware(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.segments = [
            (0, 0, 0, 0, 52, 0, 0x07000000, 0),  # ELF metadata, not PT_LOAD.
            (1, 0x1000, 0x1000, 0x1000, 4, 0x20, 0x08000005, 0x1000),
            (1, 0x2000, 0x1020, 0x1020, 0, 0x20, 0x08000006, 0x1000),
            (1, 0x3000, 0x1040, 0x1040, 3, 0x10, 0x08000006, 0x1000),
            (0, 0x4000, 0, 0, 16, 16, 0x02000000, 0),
        ]
        self.dtb_segments = [
            (0, 0, 0, 0, 52, 0, 0x07000000, 0),
            (1, 0x1000, 0x2000, 0x2000, 5, 0x20, 0x09000007, 0x1000),
            (0, 0x2000, 0, 0, 16, 16, 0x02000000, 0),
        ]
        blobs = {'adsp.mdt': elf(164, 0x1000, self.segments),
                 'adsp.b01': b'abcd', 'adsp.b03': b'xyz',
                 'adsp_dtb.mdt': elf(1, 0x2000, self.dtb_segments),
                 'adsp_dtb.b01': b'12345'}
        self.manifest = {'files': [], 'images': [
            {'path': 'adsp.mdt', 'base': 0x1000, 'bytes': 0x100, 'pas_id': 1},
            {'path': 'adsp_dtb.mdt', 'base': 0x2000, 'bytes': 0x100, 'pas_id': 0x24},
        ]}
        for name, data in blobs.items():
            self.put(name, data)

    def put(self, name, data):
        (self.root / name).write_bytes(data)
        record = {'path': name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
        self.manifest['files'] = [r for r in self.manifest['files'] if r['path'] != name] + [record]

    def verify(self):
        return audio.verify_firmware(self.root, self.manifest)

    def replace_segments(self, segments):
        self.put('adsp.mdt', elf(164, 0x1000, segments))

    def test_valid_split_images_need_neither_bss_nor_metadata_hash_files(self):
        result = self.verify()
        self.assertEqual(result['file_count'], 5)
        self.assertEqual(result['images'][0]['payload_files'], 2)
        self.assertEqual(result['images'][0]['bss_segments'], 1)
        self.assertEqual(result['images'][0]['load_end'], 0x1050)
        self.assertEqual(result['images'][1]['elf_machine'], 1)
        self.assertEqual(result['total_bytes'], sum(r['bytes'] for r in self.manifest['files']))

    def test_missing_and_same_size_corrupt_payload_fail(self):
        path = self.root / 'adsp.b01'
        path.unlink()
        with self.assertRaisesRegex(ValueError, 'Missing firmware'):
            self.verify()
        path.write_bytes(b'WXYZ')
        with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
            self.verify()

    def test_file_size_mismatch_fails(self):
        (self.root / 'adsp.b01').write_bytes(b'abcd0')
        with self.assertRaisesRegex(ValueError, 'size mismatch'):
            self.verify()

    def test_pinned_payload_length_must_also_match_program_header(self):
        self.put('adsp.b01', b'longer')
        with self.assertRaisesRegex(ValueError, 'incorrectly sized PT_LOAD'):
            self.verify()

    def test_payload_index_must_match_its_program_header(self):
        self.put('adsp.b02', b'abcd')
        self.manifest['files'] = [r for r in self.manifest['files'] if r['path'] != 'adsp.b01']
        with self.assertRaisesRegex(ValueError, 'PT_LOAD payload: adsp.b01'):
            self.verify()

    def test_extra_hash_or_bss_assets_are_not_accepted_as_payloads(self):
        for name in ('adsp.b00', 'adsp.b02', 'adsp.b04'):
            with self.subTest(name=name):
                self.put(name, b'not a payload')
                with self.assertRaisesRegex(ValueError, 'Unexpected firmware assets'):
                    self.verify()
                self.manifest['files'] = [r for r in self.manifest['files'] if r['path'] != name]

    def test_hash_flag_wins_over_pt_load_type(self):
        segments = copy.deepcopy(self.segments)
        segments[4] = (1, 0, 0, 0xffffffff, 16, 16, 0x02000000, 0)
        self.replace_segments(segments)
        self.assertEqual(self.verify()['images'][0]['hash_segments'], 1)
        for index in (1, 2, 3):
            values = list(segments[index])
            values[6] = 0x02000000
            segments[index] = tuple(values)
        self.replace_segments(segments)
        with self.assertRaisesRegex(ValueError, 'no nonzero PT_LOAD'):
            self.verify()

    def test_load_and_bss_must_fit_the_entire_carveout(self):
        for index, paddr, memsz in ((1, 0xfff, 0x20), (1, 0x10ff, 0x20),
                                    (2, 0x10ff, 0x20), (2, 0xfffffff0, 0x20)):
            with self.subTest(index=index, paddr=paddr):
                segments = copy.deepcopy(self.segments)
                values = list(segments[index])
                values[3], values[5] = paddr, memsz
                segments[index] = tuple(values)
                self.replace_segments(segments)
                with self.assertRaisesRegex(ValueError, 'PT_LOAD escapes'):
                    self.verify()

    def test_filesz_must_not_exceed_memsz(self):
        segments = copy.deepcopy(self.segments)
        values = list(segments[1])
        values[5] = 3
        segments[1] = tuple(values)
        self.replace_segments(segments)
        with self.assertRaisesRegex(ValueError, 'memory/file size'):
            self.verify()

    def test_elf_encoding_machine_header_and_program_table(self):
        original = elf(164, 0x1000, self.segments)
        cases = []
        for offset, value in ((4, 2), (5, 2), (6, 0)):
            data = bytearray(original)
            data[offset] = value
            cases.append(bytes(data))
        for offset, fmt, value in ((16, '<H', 3), (18, '<H', 1), (20, '<I', 0),
                                   (28, '<I', 0xffffffff), (40, '<H', 0),
                                   (42, '<H', 16), (44, '<H', 101)):
            data = bytearray(original)
            struct.pack_into(fmt, data, offset, value)
            cases.append(bytes(data))
        cases += [original[:20], original[:-1]]
        for index, data in enumerate(cases):
            with self.subTest(index=index):
                self.put('adsp.mdt', data)
                with self.assertRaises(ValueError):
                    self.verify()

    def test_dtb_machine_exception_is_specific_to_dtb(self):
        self.put('adsp_dtb.mdt', elf(164, 0x2000, self.dtb_segments))
        with self.assertRaisesRegex(ValueError, 'encoding/type/machine'):
            self.verify()

    def test_manifest_paths_duplicates_and_ranges(self):
        original = copy.deepcopy(self.manifest)
        for name in ('../adsp.b01', '/adsp.b01', 'dir/adsp.b01', 'adsp.b0a'):
            with self.subTest(name=name):
                self.manifest = copy.deepcopy(original)
                self.manifest['files'][0]['path'] = name
                with self.assertRaisesRegex(ValueError, 'Invalid firmware path'):
                    self.verify()
        self.manifest = copy.deepcopy(original)
        self.manifest['files'].append(copy.deepcopy(self.manifest['files'][0]))
        with self.assertRaisesRegex(ValueError, 'Duplicate firmware asset'):
            self.verify()
        for field, value in (('base', True), ('bytes', 0), ('base', -1), ('bytes', 1 << 32),
                              ('pas_id', False), ('base', 0xfffffff0)):
            with self.subTest(field=field, value=value):
                self.manifest = copy.deepcopy(original)
                self.manifest['images'][0][field] = value
                with self.assertRaises(ValueError):
                    self.verify()

    def test_missing_or_duplicate_image_is_rejected(self):
        self.manifest['images'][1] = copy.deepcopy(self.manifest['images'][0])
        with self.assertRaisesRegex(ValueError, 'Duplicate or missing firmware image'):
            self.verify()

    def test_symlink_cannot_escape_bundle(self):
        with tempfile.TemporaryDirectory() as outside:
            target = Path(outside) / 'payload'
            target.write_bytes(b'abcd')
            link = self.root / 'adsp.b01'
            link.unlink()
            try:
                link.symlink_to(target)
            except OSError:
                self.skipTest('Creating symlinks requires platform permission')
            with self.assertRaisesRegex(ValueError, 'escapes bundle'):
                self.verify()


if __name__ == '__main__':
    unittest.main()
