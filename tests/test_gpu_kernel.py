import runpy
import struct
import unittest
from pathlib import Path


MODULE = runpy.run_path(str(Path(__file__).resolve().parents[1] /
                           'scripts/check-ios16-gpu-kernel.py'))
check_kernel = MODULE['check_kernel']


class LinkedGPU(unittest.TestCase):
    def setUp(self):
        self.kernel = bytearray(0x1000)
        struct.pack_into('<8I', self.kernel, 0, 0xfeedfacf, 0x100000c, 0, 2, 2, 176, 0, 0)
        self.base = 0xfffffe0007004000
        struct.pack_into('<II16sQQQQiiII', self.kernel, 32, 0x19, 152, b'__TEXT_EXEC',
                         self.base, 0x1000, 0, 0x1000, 5, 5, 1, 0)
        struct.pack_into('<16s16sQQIIIIIIII', self.kernel, 104, b'__text', b'__TEXT_EXEC',
                         self.base + 0x200, 0x100, 0x200, 2, 0, 0, 0x80000400, 0, 0, 0)
        strings = b'\0' + b'\0'.join(s.encode() for s in MODULE['SYMBOLS']) + b'\0'
        struct.pack_into('<6I', self.kernel, 184, 2, 24, 0x400, len(MODULE['SYMBOLS']), 0x500, len(strings))
        self.kernel[0x500:0x500 + len(strings)] = strings
        for i, symbol in enumerate(MODULE['SYMBOLS']):
            struct.pack_into('<IBBHQ', self.kernel, 0x400 + 16 * i,
                             strings.index(symbol.encode()), 0x0f, 1, 0, self.base + 0x200 + 4 * i)
        markers = b'\0'.join(MODULE['MARKERS'])
        self.kernel[0x800:0x800 + len(markers)] = markers

    def test_linked_preparation_is_not_acceleration(self):
        result = check_kernel(self.kernel)
        self.assertFalse(result['acceleration'])
        self.assertFalse(result['hardware_verified'])

    def test_undefined_or_local_controls_do_not_pass_link_verification(self):
        for i, symbol in enumerate(MODULE['SYMBOLS']):
            for kind in (0x01, 0x0e, 0x03, 0xef):
                kernel = bytearray(self.kernel)
                kernel[0x400 + 16 * i + 4] = kind
                with self.subTest(symbol=symbol, kind=kind), self.assertRaises(ValueError):
                    check_kernel(kernel)

    def test_foreign_or_truncated_kernel_is_rejected(self):
        for kernel in (b'', self.kernel[:16], bytes(32) + self.kernel[32:]):
            with self.assertRaises(ValueError):
                check_kernel(kernel)

    def test_display_and_firmware_mapping_are_required(self):
        for marker in MODULE['MARKERS']:
            with self.subTest(marker=marker), self.assertRaises(ValueError):
                check_kernel(self.kernel.replace(marker, bytes(len(marker))))

    def test_data_or_outside_section_is_rejected(self):
        for mode in range(5):
            kernel = bytearray(self.kernel)
            if mode == 0:
                struct.pack_into('<I', kernel, 32 + 60, 1)  # Read-only data segment.
            elif mode == 1:
                struct.pack_into('<I', kernel, 104 + 64, 0)  # No instruction attributes.
            elif mode == 2:
                struct.pack_into('<Q', kernel, 0x400 + 8, self.base + 0x300)
            elif mode == 3:
                kernel[0x400 + 5] = 2  # No such section.
            else:
                struct.pack_into('<I', kernel, 104 + 48, 0x300)  # Inconsistent file mapping.
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                check_kernel(kernel)

    def test_malformed_macho_bounds_are_rejected(self):
        for mode in range(5):
            kernel = bytearray(self.kernel)
            if mode == 0:
                struct.pack_into('<I', kernel, 20, len(kernel))
            elif mode == 1:
                struct.pack_into('<I', kernel, 184 + 8, len(kernel))
            elif mode == 2:
                struct.pack_into('<I', kernel, 184 + 20, len(kernel))
            elif mode == 3:
                struct.pack_into('<I', kernel, 0x400, 0xffffffff)
            else:
                struct.pack_into('<I', kernel, 32 + 4, 71)
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                check_kernel(kernel)


if __name__ == '__main__':
    unittest.main()
