"""Compiled Wi-Fi capability reports preserve firmware and validation bounds."""
import hashlib
from pathlib import Path
import runpy
import struct
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
append_pack = runpy.run_path(str(ROOT/'scripts/embed-wifi-firmware.py'))['append_pack']
check_runtime = runpy.run_path(str(ROOT/'scripts/ios16_wifi_runtime.py'))['check_runtime']
STATION = b'GIULIA WIFI PROFILE: 20H392; station=1; legacy20=1; security=open,wpa2,sae-pmf; data=htt-tcl-reo'


def image(station=False, firmware_marker=False):
    pack = bytearray(128)
    pack[:8] = b'GWFWPK1\0'
    if firmware_marker:
        pack.extend(STATION)
    struct.pack_into('<I', pack, 16, len(pack))
    digest = hashlib.sha256(pack).digest()
    kernel = bytearray(0x8000)
    struct.pack_into('<8I', kernel, 0, 0xfeedfacf, 0x100000c, 0, 2, 1, 152, 0, 0)
    struct.pack_into('<II16sQQQQiiII', kernel, 32, 0x19, 152, b'__TEXT',
                     0xfffffe0007004000, 0x8000, 0, 0x8000, 5, 5, 1, 0)
    struct.pack_into('<16s16sQQIIIIIIII', kernel, 104, b'__text', b'__TEXT',
                     0xfffffe0007008000, 0x4000, 0x4000, 14, 0, 0, 0, 0, 0, 0)
    markers = (b'GIULIA WIFI: PCIe/MHI/QMI/WMI ready;\0'
               b'GIULIA FB: native panel 1264x2780 full coverage;\0'
               b'GIULIA WIFI: firmware pack mapped read-only and digest verified\0') + digest
    if station:
        markers += STATION
    kernel[0x4000:0x4000+len(markers)] = markers
    output, metadata = append_pack(bytes(kernel), bytes(pack), digest, required=False)
    pin = ','.join(f'0x{x:02x}' for x in digest)
    return output, metadata, pin


class WifiRuntimeMetadata(unittest.TestCase):
    def check(self, kernel, firmware, pin):
        # Only the compiled test pin is replaced, as in the audio fixture.
        with mock.patch.object(Path, 'read_text', return_value=pin):
            return check_runtime(kernel, {'wifi_runtime_included': True, 'wifi_firmware': firmware})

    def test_scan_only_report_stays_compatible(self):
        kernel, firmware, pin = image()
        result = self.check(kernel, firmware, pin)
        self.assertEqual(result['scan_mode'], 'passive 2.4/5 GHz')
        self.assertFalse(result['association_data_path'])
        self.assertNotIn('compiled_station', result)
        self.assertFalse(result['hardware_verified'])

    def test_station_report_describes_compiled_transport_only(self):
        kernel, firmware, pin = image(station=True)
        result = self.check(kernel, firmware, pin)
        self.assertTrue(result['association_data_path'])
        self.assertEqual(result['compiled_station']['data_transport'], 'HTT/TCL/WBM/RXDMA/REO')
        self.assertIn('WPA3-SAE/CCMP/PMF', result['compiled_station']['security'])
        self.assertFalse(result['hardware_verified'])

    def test_firmware_string_cannot_upgrade_capabilities(self):
        kernel, firmware, pin = image(firmware_marker=True)
        self.assertFalse(self.check(kernel, firmware, pin)['association_data_path'])

    def test_station_marker_does_not_bypass_firmware_binding(self):
        kernel, firmware, pin = image(station=True)
        damaged = bytearray(kernel);damaged[-1] ^= 1
        with self.assertRaisesRegex(ValueError, 'metadata mismatch'):
            self.check(bytes(damaged), firmware, pin)
        with self.assertRaisesRegex(ValueError, 'pin mismatch'):
            self.check(kernel, firmware, ','.join(['0x00']*32))


if __name__ == '__main__':
    unittest.main()
