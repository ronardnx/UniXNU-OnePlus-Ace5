"""Offline safety and evidence-preservation checks; no device imports or access."""
import importlib.util
import json
from pathlib import Path
import re
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("gpu_diagnostics", ROOT / "scripts/gpu_terminal_diagnostics.py")
diagnostics = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostics)


class FakeLive:
    def __init__(self, path, values=None, fail_at=None, collapse_after=None):
        self.path = path
        self.values = values or {}
        self.fail_at = fail_at
        self.collapse_after = collapse_after
        self.calls = []
        self.gx_reads = 0
        self.gx_addresses = {r.address for r in diagnostics.GX_REGISTERS}

    def read(self, address, width=32):
        # The attempted read must already be durable before touching the bus.
        saved = json.loads(self.path.read_text(encoding="utf-8"))
        attempt = saved["fault_snapshots"][-1]["reads"][-1]
        assert attempt["status"] == "pending"
        assert int(attempt["address"], 16) == address and attempt["width"] == width
        self.calls.append((address, width))
        if address == self.fail_at:
            raise RuntimeError("USB disappeared")
        if address in self.gx_addresses:
            self.gx_reads += 1
        if address == diagnostics.GX_POWER.address and self.collapse_after is not None:
            if self.gx_reads >= self.collapse_after:
                return 3
        return self.values.get(address, 0)

    def __getattr__(self, name):
        raise AssertionError("Snapshot must only use read, attempted " + name)


class DiagnosticsTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "result.json"
        self.report = {"existing_evidence": True}

    def capture(self, live, **options):
        return diagnostics.snapshot(live, self.report, self.path, "offline-fixture", **options)

    def test_default_requires_live_prerequisites_without_mmio(self):
        live = FakeLive(self.path)
        entry = self.capture(live, include_gx=True, gx_held=True)
        self.assertEqual(entry["status"], "skipped")
        self.assertEqual(live.calls, [])

    def test_gated_or_fenced_gpu_never_reads_gx(self):
        for power, fence in ((3, 0), (1, 0), (2, 0), (0, 1), (0xffffffff, 0xffffffff)):
            with self.subTest(power=power, fence=fence):
                live = FakeLive(self.path, {diagnostics.GX_POWER.address: power,
                                           diagnostics.AHB_FENCE.address: fence})
                entry = self.capture(live, cx_ready=True, include_gx=True, gx_held=True)
                self.assertEqual(entry["gx"]["status"], "skipped")
                self.assertEqual(live.gx_reads, 0)
                self.assertEqual(live.calls[:2], [(diagnostics.GX_POWER.address, 32),
                                                  (diagnostics.AHB_FENCE.address, 32)])

    def test_power_observation_does_not_replace_oob_hold_or_mapping(self):
        for include_gx, gx_held in ((True, False), (False, True)):
            live = FakeLive(self.path)
            entry = self.capture(live, cx_ready=True, include_gx=include_gx, gx_held=gx_held)
            self.assertEqual(entry["gx"]["status"], "skipped")
            self.assertEqual(live.gx_reads, 0)

    def test_held_gpu_reads_allowlist_and_guards_each_read(self):
        live = FakeLive(self.path)
        entry = self.capture(live, cx_ready=True, include_gx=True, gx_held=True)
        self.assertEqual(entry["gx"]["status"], "complete")
        self.assertEqual(live.gx_reads, len(diagnostics.GX_REGISTERS))
        self.assertLessEqual(len(live.calls), diagnostics.MAX_READS)
        allowlist = {(r.address, r.width) for r in diagnostics.ALL_REGISTERS}
        self.assertTrue(set(live.calls) <= allowlist)
        for index, (address, _) in enumerate(live.calls):
            if address in live.gx_addresses:
                self.assertEqual(live.calls[index-2:index], [(diagnostics.GX_POWER.address, 32),
                                                             (diagnostics.AHB_FENCE.address, 32)])
        # A read of INT_0_STATUS can release GX; clear/indexed debug registers
        # are also excluded even though the transport API itself is read-only.
        forbidden_words = (0x201, 0x37, 0x1f991, 0x23b04, 0x23b14, 0x908, 0x909)
        for word in forbidden_words:
            self.assertNotIn((0x3d00000 + word * 4, 32), live.calls)

    def test_power_collapse_between_gx_reads_stops_remainder(self):
        live = FakeLive(self.path, collapse_after=1)
        entry = self.capture(live, cx_ready=True, include_gx=True, gx_held=True)
        self.assertEqual(live.gx_reads, 1)
        self.assertEqual(entry["gx"]["status"], "stopped")

    def test_transport_loss_stops_and_preserves_previous_evidence(self):
        failed_register = diagnostics.CX_REGISTERS[3]
        live = FakeLive(self.path, fail_at=failed_register.address)
        entry = self.capture(live, cx_ready=True, include_gx=True, gx_held=True)
        self.assertEqual(entry["status"], "failed")
        self.assertEqual(live.calls[-1], (failed_register.address, 32))
        saved = json.loads(self.path.read_text(encoding="utf-8"))
        self.assertTrue(saved["existing_evidence"])
        self.assertEqual(saved["fault_snapshots"][-1], entry)
        self.assertEqual(entry["reads"][-1]["status"], "failed")
        self.assertTrue(all(row["status"] == "complete" for row in entry["reads"][:-1]))
        self.assertEqual(live.gx_reads, 0)

    def test_first_guard_failure_never_reads_another_register(self):
        live = FakeLive(self.path, fail_at=diagnostics.GX_POWER.address)
        self.assertEqual(self.capture(live, cx_ready=True)["status"], "failed")
        self.assertEqual(len(live.calls), 1)

    def test_correct_context_banks_native_far_width_and_fault_format_bit(self):
        values = {0x3dc0058: 0x400, 0x3dc2058: 0x402,
                  0x3dc0060: 0x12345678000, 0x3dc2060: 0xabcdef01000,
                  0x3d74fe0: 0x40020014, 0x3d74ff0: 0x20060001}
        live = FakeLive(self.path, values)
        entry = self.capture(live, cx_ready=True)
        self.assertFalse(entry["smmu_faults"]["gpu"]["valid_fault"])
        self.assertTrue(entry["smmu_faults"]["gmu"]["valid_fault"])
        self.assertIn((0x3dc0060, 64), live.calls)
        self.assertIn((0x3dc2060, 64), live.calls)
        self.assertNotIn((0x3dc1060, 64), live.calls)
        self.assertIn((0x3da1400, 32), live.calls)
        self.assertIn((0x3da1408, 32), live.calls)
        self.assertTrue(entry["firmware"]["host_rbbm_irq_required_by_vendor"])
        self.assertEqual(entry["firmware"]["core"], "0x40020014")

    def test_snapshots_append_without_replacing_prior_phase(self):
        self.capture(FakeLive(self.path))
        diagnostics.snapshot(FakeLive(self.path), self.report, self.path, "second")
        saved = json.loads(self.path.read_text(encoding="utf-8"))
        self.assertEqual([s["phase"] for s in saved["fault_snapshots"]], ["offline-fixture", "second"])

    def test_allowlist_matches_pinned_register_definitions(self):
        path = ROOT / ".cache/gpu-source/gen7_reg.h"
        if not path.exists():
            self.skipTest("Pinned vendor cache not present")
        definitions = dict(re.findall(r"^#define\s+(GEN7_\w+)\s+(0x[0-9a-fA-F]+)\b",
                                      path.read_text(), re.M))
        for register in diagnostics.ALL_REGISTERS:
            if register.name.startswith("GEN7_"):
                self.assertIn(register.name, definitions)
                self.assertEqual(register.address, 0x3d00000 + 4 * int(definitions[register.name], 16))


if __name__ == "__main__":
    unittest.main()
