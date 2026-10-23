import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from gpu_terminal_clocks import prepare, verify_uefi


class Peer:
    def __init__(self, root):
        self.capture = SimpleNamespace(root=Path(root))
        self.disconnected = False
        self.regs = {0x171004: 0x80000003, 0x171010: 0x80000003,
                     0x3d99168: 0x80000202, 0x3d91000: 0x100,
                     0x3d97000: 0x80000000, 0x3d7e000: 1,
                     0x3da0020: 0x4c017e18, 0x3d9913c: 0x2001}
        self.writes = []
        self.reads = []
        self.maps = []
        self.stuck_smmu = False

    def map(self, base, size):
        self.maps.append(base)
        return base

    def unmap(self, slot):
        self.maps.remove(slot)

    def read(self, address):
        self.reads.append(address)
        return self.regs.get(address, 0)

    def write(self, address, value, verify=False):
        self.writes.append((address, value))
        if address in (0x3d9911c, 0x3d99004):
            value = 0  # Always-on/HALT_DELAY branch, not an enable vote.
        if address == 0x3d97000:
            value = 0x80000001 if self.stuck_smmu else 1
        if address in (0x171004, 0x171010):
            value &= ~0x80000000
        if address == 0x3d91004 and value == 1:
            self.regs[0x3d91000] |= 0x80000000
        if address == 0x3d91000 and value & 1:
            value |= 0x20000000  # Asynchronous PLL active status.
        if address in (0x3d99318, 0x3d993ec):
            value &= ~1  # RCG update completed.
        self.regs[address] = value


class ClockTests(unittest.TestCase):
    def test_uefi_verification_is_read_only_and_rejects_bad_gate(self):
        with tempfile.TemporaryDirectory() as folder:
            peer = Peer(folder)
            report = {}
            prepare(peer, report)
            writes = list(peer.writes)
            verify_uefi(peer, report)
            self.assertTrue(report['uefi_clocks_verified'])
            self.assertEqual(peer.writes, writes)
            peer.regs[0x3d97000] = 0x80000000
            peer.reads.clear()
            with self.assertRaisesRegex(RuntimeError, 'UEFI clock mismatch'):
                verify_uefi(peer, {})
            self.assertNotIn(0x3d7e000, peer.reads)
            self.assertEqual(peer.writes, writes)

    def test_real_initial_state_and_dynamic_pll_status(self):
        with tempfile.TemporaryDirectory() as folder:
            peer = Peer(folder)
            report = {}
            prepare(peer, report)
            self.assertTrue(report['clock_preparation']['complete'])
            self.assertEqual(peer.regs[0x3d9931c], 0x303)
            self.assertEqual(peer.regs[0x3d993f0], 0x505)
            self.assertFalse(peer.maps)
            self.assertFalse(any(a == 0x3d7e000 for a, _ in peer.writes))
            self.assertFalse(any(0x3da0000 <= a < 0x3de0000 for a, _ in peer.writes))

    def test_running_source_refuses_before_writes(self):
        with tempfile.TemporaryDirectory() as folder:
            peer = Peer(folder)
            peer.regs[0x3d91000] = 0xe0000005
            with self.assertRaisesRegex(RuntimeError, 'Not cold'):
                prepare(peer, {})
            self.assertFalse(peer.writes)
            self.assertFalse(peer.maps)

    def test_stuck_gate_never_reads_gmu_or_smmu(self):
        with tempfile.TemporaryDirectory() as folder, patch('gpu_terminal_clocks.time.sleep'):
            peer = Peer(folder)
            peer.stuck_smmu = True
            with self.assertRaisesRegex(RuntimeError, 'Clock poll'):
                prepare(peer, {})
            self.assertNotIn(0x3d7e000, peer.reads)
            self.assertNotIn(0x3da0020, peer.reads)
            self.assertFalse(peer.maps)

    def test_released_reset_blocks_pll_changes(self):
        with tempfile.TemporaryDirectory() as folder:
            peer = Peer(folder)
            peer.regs[0x3d7e000] = 0
            with self.assertRaisesRegex(RuntimeError, 'held in reset'):
                prepare(peer, {})
            self.assertFalse(any(0x3d91000 <= a < 0x3d92000 for a, _ in peer.writes))


if __name__ == '__main__':
    unittest.main()
