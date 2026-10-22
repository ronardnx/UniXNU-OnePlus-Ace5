import runpy
import struct
import unittest
from pathlib import Path


MODULE = runpy.run_path(str(Path(__file__).resolve().parents[1] /
                           'scripts/check-ios16-gpu-kernel.py'))
check_kernel = MODULE['check_kernel']


class LinkedGPU(unittest.TestCase):
    def setUp(self):
        self.kernel = struct.pack('<8I', 0xfeedfacf, 0x100000c, 0, 2, 0, 0, 0, 0)
        self.kernel += b'\0'.join(MODULE['MARKERS'])
        self.symbols = '\n'.join('fffffe0007004000 T ' + name
                                 for name in MODULE['SYMBOLS'])

    def test_linked_preparation_is_not_acceleration(self):
        result = check_kernel(self.kernel, self.symbols)
        self.assertFalse(result['acceleration'])
        self.assertFalse(result['hardware_verified'])

    def test_undefined_controls_do_not_pass_link_verification(self):
        for symbol in MODULE['SYMBOLS']:
            with self.subTest(symbol=symbol), self.assertRaises(ValueError):
                check_kernel(self.kernel, self.symbols.replace(
                    'fffffe0007004000 T ' + symbol, '                 U ' + symbol))

    def test_foreign_or_truncated_kernel_is_rejected(self):
        for kernel in (b'', self.kernel[:16], bytes(32) + self.kernel[32:]):
            with self.assertRaises(ValueError):
                check_kernel(kernel, self.symbols)

    def test_display_and_firmware_mapping_are_required(self):
        for marker in MODULE['MARKERS']:
            with self.subTest(marker=marker), self.assertRaises(ValueError):
                check_kernel(self.kernel.replace(marker, b''), self.symbols)


if __name__ == '__main__':
    unittest.main()
