"""Audio Mach-O embedding and handoff metadata are checked independently."""
import copy
import hashlib
from pathlib import Path
import runpy
import struct
import unittest
from unittest import mock

import test_audio_pack as pack_fixtures

ROOT = Path(__file__).resolve().parents[1]
append_pack = runpy.run_path(str(ROOT/'scripts/embed-audio-firmware.py'))['append_pack']
check_runtime = runpy.run_path(str(ROOT/'scripts/ios16_audio_runtime.py'))['check_runtime']


def kernel_image(pack, mixed=False):
    digest = hashlib.sha256(pack).digest()
    data = bytearray(0xc000 if mixed else 0x8000)
    struct.pack_into('<8I', data, 0, 0xfeedfacf, 0x100000c, 0, 2,
                     2 if mixed else 1, 224 if mixed else 152, 0, 0)
    struct.pack_into('<II16sQQQQiiII', data, 32, 0x19, 152, b'__TEXT',
                     0xfffffe0007004000, 0x8000, 0, 0x8000, 5, 5, 1, 0)
    struct.pack_into('<16s16sQQIIIIIIII', data, 104, b'__text', b'__TEXT',
                     0xfffffe0007008000, 0x4000, 0x4000, 14, 0, 0, 0, 0, 0, 0)
    markers = (b'GIULIA AUDIO: compiled runtime disabled;\0'
               b'GIULIA AUDIO: firmware pack mapped read-only and digest verified\0') + digest
    data[0x4000:0x4000+len(markers)] = markers
    if mixed:
        struct.pack_into('<II16sQQQQiiII', data, 184, 0x19, 72, b'__WIFI_FW',
                         0xfffffe000700c000, 0x4000, 0x8000, 0x1000, 1, 1, 0, 0)
        data[0x8000:0x9000] = b'W'*4096
    return bytes(data)


class AudioEmbed(unittest.TestCase):
    def setUp(self):
        case = pack_fixtures.AudioPack('test_deterministic_wire_format_and_bss')
        case.setUp()
        self.addCleanup(case.doCleanups)
        self.pack = case.build()
        self.digest = hashlib.sha256(self.pack).digest()
        self.kernel = kernel_image(self.pack)

    def append(self, kernel=None, pack=None, digest=None):
        return append_pack(self.kernel if kernel is None else kernel,
                           self.pack if pack is None else pack,
                           self.digest if digest is None else digest)

    def check(self, kernel, metadata):
        # Only the compiled synthetic pin is replaced. Production parsing,
        # hashing and report comparison execute unchanged.
        with mock.patch.object(Path, 'read_text', return_value=pack_fixtures.packer.pin_header(self.pack)):
            return check_runtime(kernel, {'audio_runtime_included': True,
                                           'audio_firmware': metadata})

    def test_append_preserves_sections_and_existing_wifi_segment(self):
        for mixed in (False, True):
            kernel = kernel_image(self.pack, mixed)
            output, meta = self.append(kernel)
            self.assertEqual(output[meta['file_offset']:], self.pack)
            self.assertEqual(output[0x4000:len(kernel)], kernel[0x4000:])
            self.assertEqual(struct.unpack_from('<II', output, 16),
                             (3, 296) if mixed else (2, 224))
            self.assertEqual(meta['virtual_bytes'] % 0x4000, 0)
            self.assertEqual(meta['file_offset'] % 0x4000, 0)
            report = self.check(output, meta)
            self.assertFalse(report['hardware_verified'])
            self.assertFalse(report['playback'])
            self.assertFalse(report['native_platform_provider'])

    def test_digest_and_container_length_are_bound(self):
        changed = bytearray(self.pack)
        changed[-1] ^= 1
        with self.assertRaisesRegex(ValueError, 'digest'):
            self.append(pack=bytes(changed))
        struct.pack_into('<I', changed, 8, len(changed)+1)
        with self.assertRaisesRegex(ValueError, 'length'):
            self.append(pack=bytes(changed), digest=hashlib.sha256(changed).digest())

    def test_capability_markers_and_pin_are_required(self):
        for start in (self.kernel.index(self.digest), self.kernel.index(b'compiled runtime'),
                      self.kernel.index(b'firmware pack mapped')):
            changed = bytearray(self.kernel)
            changed[start] ^= 1
            with self.assertRaises(ValueError):
                self.append(bytes(changed))

    def test_header_space_signature_duplicate_and_image_bounds(self):
        output, _ = self.append()
        cases = [output, self.kernel[:20]]
        for offset, value in ((184, 1), (20, 153), (104+48, 200), (32, 0x1d)):
            changed = bytearray(self.kernel)
            struct.pack_into('<I', changed, offset, value)
            cases.append(bytes(changed))
        changed = bytearray(self.kernel)
        struct.pack_into('<Q', changed, 32+32, 0x20000000)
        cases.append(bytes(changed))
        for index, kernel in enumerate(cases):
            with self.subTest(index=index), self.assertRaises(ValueError):
                self.append(kernel)

    def test_runtime_rejects_changed_pack_even_with_recomputed_report_digest(self):
        output, meta = self.append()
        changed = bytearray(output)
        changed[-1] ^= 1
        meta['sha256'] = hashlib.sha256(changed[meta['file_offset']:]).hexdigest()
        with self.assertRaisesRegex(ValueError, 'pin mismatch'):
            self.check(changed, meta)

    def test_runtime_rejects_report_disagreement_and_writeable_mapping(self):
        output, meta = self.append()
        report = copy.deepcopy(meta)
        report['bytes'] -= 1
        with self.assertRaisesRegex(ValueError, 'metadata mismatch'):
            self.check(output, report)
        for offset, value in ((184+56, 3), (184+60, 3), (184+64, 1), (184+68, 1)):
            changed = bytearray(output)
            struct.pack_into('<I', changed, offset, value)
            with self.assertRaises(ValueError):
                self.check(changed, meta)

    def test_runtime_requires_embedding_and_compiled_mapping(self):
        with self.assertRaisesRegex(ValueError, 'requires verified local'):
            check_runtime(self.kernel, {'audio_runtime_included': True})
        self.assertIsNone(check_runtime(b'ordinary kernel', {}))
        with self.assertRaisesRegex(ValueError, 'without compiled'):
            check_runtime(b'ordinary kernel', {'audio_runtime_included': True})

    def test_runtime_rejects_invalid_type_and_virtual_mapping_even_when_report_matches(self):
        output, original = self.append()
        for mutation in ('type', 'overlap', 'overflow', 'unaligned'):
            changed, meta = bytearray(output), copy.deepcopy(original)
            if mutation == 'type':
                struct.pack_into('<I', changed, 12, 1)
            else:
                va = {'overlap': 0xfffffe0007004000, 'overflow': 2**64-0x4000,
                      'unaligned': int(meta['virtual_address'], 16)+1}[mutation]
                struct.pack_into('<Q', changed, 184+24, va)
                meta['virtual_address'] = hex(va)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.check(changed, meta)

    def test_runtime_requires_compiled_pin_length_as_well_as_digest(self):
        output, meta = self.append()
        pin = pack_fixtures.packer.pin_header(self.pack).replace(
            f'GA_FIRMWARE_PACK_BYTES {len(self.pack)}u',
            f'GA_FIRMWARE_PACK_BYTES {len(self.pack)-1}u')
        with mock.patch.object(Path, 'read_text', return_value=pin):
            with self.assertRaisesRegex(ValueError, 'firmware size'):
                check_runtime(output, {'audio_runtime_included': True, 'audio_firmware': meta})


if __name__ == '__main__':
    unittest.main()
