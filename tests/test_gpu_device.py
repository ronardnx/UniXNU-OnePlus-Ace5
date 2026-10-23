import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
spec = importlib.util.spec_from_file_location('gpu_device', ROOT/'scripts/check-ios16-gpu-device.py')
device = importlib.util.module_from_spec(spec)
spec.loader.exec_module(device)


class GpuDeviceEvidence(unittest.TestCase):
    def setUp(self):
        self.status = ('GIULIA GPU: command=gpu.status stage=host-memory-prepared mode=3 '
                       'firmware_verified=1 host_prepared=1 reservations=8 table_pages=6 leaf_pages=139\n')
        self.table = ('GIULIA GPU HFI: table_iova=0x60010000 log_iova=0x60030000 '
                      'vrb_iova=0x60040000 cpu_cache=normal-nc pinned=0\n')
        self.queues = ''.join(f'GIULIA GPU HFI: queue={q} code=0 read=0 write=0\n' for q in range(17))
        self.valid = self.status + self.table + self.queues

    def test_preparation_never_proves_execution(self):
        report = device.evaluate(self.valid)
        self.assertTrue(report['passed'])
        for key in ('stock_gmu_execution_verified', 'hfi_handshake_verified',
                    'gpu_submission_verified', 'metal_verified'):
            self.assertFalse(report[key])

    def test_incomplete_stale_corrupt_busy_or_panicked_capture_fails(self):
        for text in ('', self.status, self.valid+self.status, self.valid+self.queues,
                     self.valid.replace('reservations=8', 'reservations=5'),
                     self.valid.replace('queue=16', 'queue=15'),
                     self.valid.replace('code=0', 'code=4', 1),
                     self.valid.replace('write=0', 'write=4', 1),
                     self.valid.replace('pinned=0', 'pinned=1'),
                     self.valid+'panic(cpu 0 caller 0xffff): test\n'):
            with self.subTest(text=text):
                self.assertFalse(device.evaluate(text)['passed'])


if __name__ == '__main__':
    unittest.main()
