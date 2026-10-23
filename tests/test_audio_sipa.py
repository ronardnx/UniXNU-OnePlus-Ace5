"""Synthetic SIPA data and fault-injected amplifier I/O; no hardware required."""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('sipa', ROOT/'scripts/ios16_audio_sipa.py')
sipa = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sipa)


def seal(blob):
    struct.pack_into('<I', blob, 0, zlib.crc32(blob[4:]))
    return bytes(blob)


def fixture(count=4):
    """Small independent 1.0.5 image, deliberately NOT stock register values."""
    header = bytearray(12 + 44*count)
    struct.pack_into('<I', header, 4, 0x10005)
    payload = bytearray(384)

    def records(rows):
        offset = len(payload)
        for row in rows:
            payload.extend(struct.pack('<' + 'I'*len(row), *row))
        return [offset, len(rows), 4*len(rows[0])] if rows else [0, 0, 0]

    def proc(address, mask, action, value=0, delay=0):
        return [address, mask, action, 0, delay] + [value]*6

    for channel in range(2):
        struct.pack_into('<I', header, 8+4*channel, 1)
        struct.pack_into('<III', header, 8+4*count+12*channel, channel*192, 1, 192)
        chip = [0]*48
        chip[:4] = [11, 8, 16, 0]
        chip[4:7] = records([[0x5880, 0x5880]])
        chip[16:19] = records([[0x10, 0] + [0x1200]*6])
        chip[19:22] = records([proc(0x10, 0xff, 1, 0x34), proc(0x11, 1, 2, 1)])
        chip[22:25] = records([proc(0x12, 0xffff, 1, 0)])
        chip[25] = 8
        chip[26:29] = records([proc(0x20+i, 0xffff, 0) for i in range(3)])
        chip[29:32] = records([proc(0x23, 0xff, 0)])
        chip[32:35] = records([proc(0x24, 0xffff, 1, 0x42)])
        struct.pack_into('<48I', payload, channel*192, *chip)
    struct.pack_into('<I', header, len(header)-4, len(payload))
    return seal(header + payload)


class AudioSipa(unittest.TestCase):
    def test_4_and_8_channel_layouts_and_scene_selection(self):
        for count in (4, 8):
            parsed = sipa.parse_sipa(fixture(count))
            self.assertEqual(parsed['channel_count'], count)
            program = sipa.playback_program(parsed)
            self.assertEqual([c['i2c_address'] for c in program['channels']], [0x6c, 0x6d])
            self.assertEqual(program['channels'][0]['init'][0]['value'], 0x1200)
            self.assertEqual(program['channels'][0]['chip_id_address'], 6)
            self.assertFalse(program['playback_verified'])
            self.assertTrue(program['speaker_protection_required'])

    def test_crc_and_version(self):
        blob = bytearray(fixture())
        blob[-1] ^= 1
        with self.assertRaisesRegex(ValueError, 'CRC32'):
            sipa.parse_sipa(bytes(blob))
        struct.pack_into('<I', blob, 4, 0x10004)
        with self.assertRaisesRegex(ValueError, 'version'):
            sipa.parse_sipa(seal(blob))

    def test_truncation_and_file_size(self):
        blob = fixture()
        for size in (0, 7, 187, 188, len(blob)-1):
            with self.subTest(size=size), self.assertRaises(ValueError):
                sipa.parse_sipa(blob[:size])
        with self.assertRaises(ValueError):
            sipa.parse_sipa(bytes(sipa.MAX_BYTES+1))

    def test_every_nested_descriptor_is_bounds_checked(self):
        for word in (4, 16, 19, 22, 26, 29, 32, 43):
            for field, value in ((0, 0xffffffff), (1, 4097), (2, 1)):
                blob = bytearray(fixture())
                # Make the empty SRAM descriptor nonempty to exercise stride.
                struct.pack_into('<I', blob, 188+43*4+4, 1)
                struct.pack_into('<I', blob, 188+43*4+8, 32)
                struct.pack_into('<I', blob, 188+4*word+4*field, value)
                with self.subTest(word=word, field=field), self.assertRaises(ValueError):
                    sipa.parse_sipa(seal(blob))

    def test_invalid_mask_action_delay_and_values(self):
        original = fixture()
        startup = struct.unpack_from('<I', original, 188+19*4)[0]+188
        for word, value in ((0, 255), (1, 0x10000), (2, 4), (4, 1000001), (5, 0x10000)):
            blob = bytearray(original)
            struct.pack_into('<I', blob, startup+word*4, value)
            with self.subTest(word=word), self.assertRaises(ValueError):
                sipa.parse_sipa(seal(blob))

    def test_trim_crc_size_and_enabled_channels(self):
        for offset, value in ((188+25*4, 4), (8, 2), (16, 1), (188, 12)):
            blob = bytearray(fixture())
            struct.pack_into('<I', blob, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                sipa.playback_program(sipa.parse_sipa(seal(blob)))

    def test_sha_pin_prevents_accepting_resealed_foreign_data(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'sipa.bin'
            blob = fixture()
            path.write_bytes(blob)
            result = sipa.verify_sipa(path, hashlib.sha256(blob).hexdigest())
            self.assertEqual(result['bytes'], len(blob))
            with self.assertRaisesRegex(ValueError, 'SHA256'):
                sipa.verify_sipa(path)

    def test_c_engine_fault_injection(self):
        compiler = os.environ.get('CC') or next(
            (path for name in ('clang', 'cc', 'gcc') if (path := shutil.which(name))), None)
        if not compiler:
            self.skipTest('Set CC or run under WSL with a C compiler for amplifier I/O tests')
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            binary = folder/('speaker.exe' if os.name == 'nt' else 'speaker')
            firmware = folder/'synthetic-sipa.bin'
            firmware.write_bytes(fixture())
            build = subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                                    str(ROOT/'tests/ios16_audio_speaker.c'), '-o', str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout+build.stderr)
            result = subprocess.run([str(binary), str(firmware)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            self.assertIn('SIPA fault-injection checks passed', result.stdout)
            stock = ROOT/'.cache/audio-speaker-stock/sipa.bin'
            if stock.exists():
                sipa.verify_sipa(stock)
                result = subprocess.run([str(binary), str(stock), '--validate-stock'],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                self.assertIn('no I/O executed', result.stdout)


if __name__ == '__main__':
    unittest.main()
