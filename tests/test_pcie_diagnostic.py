"""Diagnostics must identify measured failures without inventing a root cause."""
import base64
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
from ios16_pcie_diagnostic import decode_trace


def sample():
    words = [0x32535047, 13 | (255 << 16), 7]
    for i in range(8):
        cfg = 0x605 if i == 0 else 1 if i < 3 else 0x100001 if i == 3 else 0x100605
        words += [cfg, 0, 0]
    words += [0x4070000, 0x4070000, 0, 0, 0x201, 3, 0x3c3, 3,
              0x100, 1, 0x10110008, 0x86, 4, 1, 0]
    return words


def decode(words):
    return decode_trace(struct.pack('<42I', *words))


class PcieDiagnostic(unittest.TestCase):
    def test_passed_conditions_do_not_claim_a_root_cause(self):
        r = decode(sample())
        self.assertEqual(r['confirmed_failures'], [])
        self.assertEqual(r['unknown_conditions'], [])
        self.assertFalse(r['final_ltssm']['link_up'])
        self.assertEqual(r['final_ltssm']['name'], 'Detect.Quiet')
        self.assertIn('没有给出唯一', r['conclusion'])

    def test_cesta_boundary_localizes_rate_change(self):
        r = decode(sample())
        change = [c for c in r['clock_changes'] if c['after_value'] == '0x100605'][0]
        self.assertEqual(change['after'], 'CESTA D0 请求完成后')
        self.assertTrue(change['rate_fields_changed'])
        self.assertFalse(change['hw_control_changed'])

    def test_low_perst_is_explicit_failure(self):
        words = sample(); words[32] = 0
        r = decode(words)
        self.assertIn('PERST 已配置输出且回读为高', r['confirmed_failures'])

    def test_invalid_register_is_unknown_not_success(self):
        words = sample(); words[27] = 0xffffffff
        r = decode(words)
        self.assertIn('核心电源域完成上电', r['unknown_conditions'])
        self.assertNotIn('均通过', r['conclusion'])

    def test_uncaptured_phase_does_not_create_change(self):
        words = sample(); words[1] &= ~(1 << (16+4))
        r = decode(words)
        self.assertIsNone(r['phases'][4]['rchng_cfg'])
        self.assertFalse(any(c['after'] == 'CESTA D0 请求完成后' for c in r['clock_changes']))
        self.assertTrue(r['unknown_conditions'])

    def test_hardware_control_loss_is_explicit(self):
        words = sample(); words[3+7*3] = 0x605
        r = decode(words)
        self.assertTrue(any('PHY 就绪' in c for c in r['confirmed_failures']))

    def test_bad_size_and_identity_rejected(self):
        with self.assertRaises(ValueError):
            decode_trace(b'\0'*167)
        words = sample(); words[0] = 5
        with self.assertRaises(ValueError):
            decode(words)

    def test_cli_replay_and_readable_report(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory)/'serial.log'; out = Path(directory)/'trace.json'
            token = b'giulia_pcid2=' + base64.b64encode(struct.pack('<42I', *sample()))
            log.write_bytes(token+b'\n'+token+b'\n')
            subprocess.run([sys.executable, str(ROOT/'scripts/decode-ios16-pcie-snapshot.py'),
                            str(log), '--out', str(out)], check=True, capture_output=True)
            r = json.loads(out.read_text())
            self.assertEqual(len(r['snapshots']), 1)
            self.assertIn('CESTA', out.with_suffix('.txt').read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()
