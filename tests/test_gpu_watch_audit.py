"""Offline checks for boundary isolation and durable per-read evidence."""
import json
from pathlib import Path
import re
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import gpu_terminal_watch_audit as audit


class FakeLive:
    def __init__(self, path, fail_index=None, values=None, collapse_after=None):
        self.path, self.fail_index = path, fail_index
        self.calls, self.gx_count = [], 0
        self.collapse_after = collapse_after
        self.values = {address: wanted for address, (_, wanted) in audit.POWER_EXPECTED.items()}
        self.values.update(values or {})

    def read(self, address, width=32):
        saved = json.loads(self.path.read_text(encoding="utf-8"))
        rows = saved["watch_read_audits"][-1]["reads"]
        assert rows[-1]["status"] == "pending" and "pending_utc" in rows[-1]
        assert int(rows[-1]["address"], 16) == address and rows[-1]["width"] == width == 32
        assert all(row["status"] == "complete" for row in rows[:-1])
        self.calls.append((address, width))
        if len(self.calls) - 1 == self.fail_index:
            raise RuntimeError("USB disappeared")
        if address in {item.address for item in audit.GX_REGISTERS}:
            self.gx_count += 1
        if address == audit.GX_POWER.address and self.collapse_after is not None:
            if self.gx_count >= self.collapse_after:
                return 3
        return self.values.get(address, 0)

    def __getattr__(self, name):
        raise AssertionError("Only read is allowed, attempted " + name)


class WatchAuditTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.path = Path(temporary.name) / "result.json"
        self.report = {"existing_evidence": {"retained": True}}

    def run_audit(self, live, **kwargs):
        return audit.audit_watch_reads(live, self.report, self.path, **kwargs)

    def test_missing_prerequisites_never_access_device(self):
        for cx_ready, gx_held in ((False, False), (True, False), (False, True)):
            live = FakeLive(self.path)
            with self.assertRaisesRegex(ValueError, "acknowledged"):
                self.run_audit(live, cx_ready=cx_ready, gx_held=gx_held)
            self.assertEqual(live.calls, [])
            saved = json.loads(self.path.read_text())
            self.assertEqual(saved["watch_read_audits"][-1]["failure_stage"], "caller_prerequisites")

    def test_complete_uses_only_exact_32bit_reads_and_retains_report(self):
        live = FakeLive(self.path)
        entry = self.run_audit(live, cx_ready=True, gx_held=True)
        self.assertEqual(entry["status"], "complete")
        self.assertFalse(entry["raw_cp_submitted"] or entry["kernel_snapshot_requested"])
        self.assertEqual(len(live.calls), audit.MAX_READS)
        expected = [(item.address, 32) for item in audit.POWER_REGISTERS]
        expected += [(audit.GX_POWER.address, 32), (audit.AHB_FENCE.address, 32)]
        expected += [(item.address, 32) for item in audit.AO_REGISTERS]
        for item in audit.GX_REGISTERS:
            expected += [(audit.GX_POWER.address, 32), (audit.AHB_FENCE.address, 32), (item.address, 32)]
        self.assertEqual(live.calls, expected)
        saved = json.loads(self.path.read_text())
        self.assertEqual(saved["existing_evidence"], {"retained": True})
        self.assertEqual(saved["watch_read_audits"][-1], entry)
        self.assertTrue(all(row["status"] == "complete" and "value" in row for row in entry["reads"]))

    def test_each_transport_failure_stops_and_preserves_attempt(self):
        for index in range(audit.MAX_READS):
            with self.subTest(index=index):
                self.report = {"existing_evidence": True}
                live = FakeLive(self.path, fail_index=index)
                with self.assertRaisesRegex(RuntimeError, "USB disappeared"):
                    self.run_audit(live, cx_ready=True, gx_held=True)
                saved = json.loads(self.path.read_text())
                entry = saved["watch_read_audits"][-1]
                self.assertEqual(len(live.calls), index + 1)
                self.assertEqual(entry["status"], "failed")
                self.assertEqual(entry["failure_address"], hex(live.calls[-1][0]))
                self.assertEqual(entry["failure_stage"], entry["reads"][-1]["stage"])
                self.assertEqual(entry["reads"][-1]["status"], "failed")
                self.assertIn("pending_utc", entry["reads"][-1])
                self.assertIn("USB disappeared", entry["reads"][-1]["error"])
                self.assertTrue(saved["existing_evidence"])

    def test_lost_oob_guard_stops_before_next_gx_read(self):
        live = FakeLive(self.path, collapse_after=1)
        with self.assertRaisesRegex(RuntimeError, "prerequisite changed"):
            self.run_audit(live, cx_ready=True, gx_held=True)
        self.assertEqual(live.gx_count, 1)
        entry = json.loads(self.path.read_text())["watch_read_audits"][-1]
        self.assertEqual(entry["failure_stage"], "snapshot_gx_guard")
        self.assertEqual(entry["reads"][-1]["status"], "complete")
        self.assertEqual(entry["reads"][-1]["value"], "0x3")
        self.assertFalse(entry["reads"][-1]["prerequisite"]["matched"])

    def test_invalid_value_or_power_identity_stops(self):
        for value in (-1, 1 << 32, True, "0", 0xffffffff, 0):
            with self.subTest(value=value):
                live = FakeLive(self.path, values={0x3da0020: value})
                with self.assertRaises((ValueError, RuntimeError)):
                    self.run_audit(live, cx_ready=True, gx_held=True)
                self.assertEqual(live.calls[-1], (0x3da0020, 32))

    def test_prior_host_snapshot_distinguishes_unread_secure_and_far_widths(self):
        self.report["fault_snapshots"] = [{"phase": "configured", "status": "complete", "reads": [
            {"address": "0x3dc0058", "width": 32, "status": "complete", "value": "0x400"},
            {"address": "0x3dc0060", "width": 64, "status": "complete", "value": "0x123456789"},
        ]}]
        entry = self.run_audit(FakeLive(self.path), cx_ready=True, gx_held=True)
        observed = {row["address"]: row["prior_host_observation"] for row in entry["reads"]}
        self.assertEqual(entry["baseline_phase"], "configured")
        self.assertEqual(observed["0x3dc0058"], "exact_32bit_read")
        self.assertEqual(observed["0x3dc0060"], "covered_by_64bit_read_only")
        self.assertEqual(observed["0x3dc0064"], "covered_by_64bit_read_only")
        for address in (0x3dc3058, 0x3dc3060, 0x3dc3064, 0x3dc3068, 0x3da140c):
            self.assertEqual(observed[hex(address)], "not_observed")

    def test_historical_r2_allowlist_covers_new_fault_gated_snapshot(self):
        # The helper intentionally reproduces r2's successful 53-read isolation
        # experiment, including formerly unconditional secure syndrome reads.
        # New OP14 reads statuses first and fault-gates syndromes; it never GX-reads.
        historical = [0x3d7e340, 0x3d8ec40, 0x3d7e648, 0x3d8ec14, 0x3d8ec4c,
                      0x3d8ec54, 0x3d8ec30, 0x3da0048, 0x3dc0058, 0x3dc0060,
                      0x3dc0064, 0x3dc0068, 0x3dc2058, 0x3dc2060, 0x3dc2064,
                      0x3dc2068, 0x3dc3058, 0x3dc3060, 0x3dc3064, 0x3dc3068,
                      0x3da1400, 0x3da1408, 0x3da140c]
        self.assertEqual([item.address for item in audit.AO_REGISTERS], historical)
        self.assertEqual([item.address for item in audit.GX_REGISTERS],
                         [0x3d000e0, 0x3d02084, 0x3d0208c, 0x3d02090, 0x3d3d000])
        source = (ROOT / "ios16/giulia_gpu_platform_impl.h").read_text()
        body = re.search(r"const uint64_t ao\[\]=\{([^}]+)\};", source).group(1)
        self.assertRegex(body, r"^\s*0x[0-9a-f]+(?:\s*,\s*0x[0-9a-f]+)*\s*$")
        statuses = [int(token, 16) for token in re.findall(r"0x[0-9a-f]+", body)]
        self.assertEqual(statuses, historical[:9] + [0x3dc2058, 0x3dc3058])


if __name__ == "__main__":
    unittest.main()
