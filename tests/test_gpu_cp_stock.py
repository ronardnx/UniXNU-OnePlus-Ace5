import sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import gpu_terminal_cp_stock as stock


class FakeTransport:
    """Answers every acknowledged message the way the GMU does."""

    def __init__(self, reject=None, context_queue=1):
        self.calls = []
        self.reject = reject or {}
        self.context_queue = context_queue
        self.published = []

    def send(self, msg, payload, seconds=10):
        self.calls.append((msg, list(payload)))
        key = (msg, payload[0] if payload else None)
        if key in self.reject:
            return dict(id=msg, value=self.reject[key], status=0)
        value = self.context_queue if msg == 12 else 0
        return dict(id=msg, value=value, status=0)

    def packet(self, msg_id, payload, msg_type=0):
        self.published.append((msg_id, list(payload)))
        return 0x90000000 | (len(payload) + 1 << 8) | msg_id


class StockSequence(unittest.TestCase):
    def test_feature_plan_matches_vendor_order(self):
        plan = stock.feature_plan({})
        self.assertEqual([p[1] for p in plan], [12, 11, 21, 9, 1, 16, 27, 26, 17])
        # ACD/BCL/CLX/IFPC/HWSCHED/KPROF/DMS/PERF_NORETAIN/BAIL_OUT
        self.assertEqual([p[2] for p in plan], [1, 1, 1, 1, 1, 1, 1, 0, 1])
        self.assertEqual(plan[1][3], 1)          # bcl_data
        self.assertEqual(plan[3][3], 0)          # IFPC stays off without hysteresis
        self.assertEqual([p[4] for p in plan], [True, True, True, True, False, True, True, True, True])
        self.assertEqual(stock.feature_plan({'ifpc_hyst': 0x1000})[3][3], 0x1000)
        self.assertEqual(stock.feature_plan({'bcl_data': 0})[1][3], 0)

    def test_post_core_plan_encodes_preemption_data(self):
        # HW_FENCE needs a registered provider and LPAC a set soft fuse, so the
        # default plan carries neither; AQE is unconditional in stock.
        plan = stock.post_core_plan({})
        self.assertEqual([p[1] for p in plan], [2, 29])
        self.assertEqual(plan[0][3], 1)          # preempt level 1, no gmem/skip
        full = stock.post_core_plan({'hw_fence_provider_registered': True, 'lpac_enabled': True})
        self.assertEqual([p[1] for p in full], [25, 2, 24, 29])
        self.assertEqual(stock.post_core_plan({'preempt_level': 3,
                                               'preempt_usesgmem': True,
                                               'preempt_skipsaverestore': True})[0][3],
                          0x3 | 0x4 | 0x8)

    def test_start_follows_core_fw_start_then_context_queue_then_start(self):
        transport = FakeTransport()
        report = {}
        self.assertEqual(stock.start(transport, report), 1)
        ids = [call[0] for call in transport.calls]
        self.assertEqual(ids, [11] * 9 + [14, 12] + [11] * 2 + [10])
        self.assertEqual(transport.calls[9], (14, [0]))
        self.assertEqual(transport.calls[10], (12, [121, 0]))
        self.assertEqual([call[1][0] for call in transport.calls[11:13]], [2, 29])
        self.assertTrue(report['core_fw_start_accepted'])
        self.assertTrue(report['hwsched_start_accepted'])
        self.assertEqual(report['context_queue'], 1)
        with self.assertRaisesRegex(RuntimeError, 'already attempted'):
            stock.start(transport, report)

    def test_optional_feature_rejection_is_tolerated_but_mandatory_is_not(self):
        transport = FakeTransport(reject={(11, 21): stock.GMU_NOT_SUPPORTED})
        report = {}
        stock.start(transport, report)
        self.assertEqual(report['feature_rejections'][0]['feature'], 'clx')
        self.assertEqual(report['feature_rejections'][0]['outcome'], 'unsupported')

        transport = FakeTransport(reject={(11, 1): stock.GMU_NOT_SUPPORTED})
        with self.assertRaisesRegex(RuntimeError, 'hwsched rejected'):
            stock.start(transport, {})
        transport = FakeTransport(reject={(11, 2): 3})
        with self.assertRaisesRegex(RuntimeError, 'preemption rejected'):
            stock.start(transport, {})

    def test_start_rejects_error_values_without_recording_success(self):
        for msg, payload, pattern in ((14, [0], 'CORE_FW_START'),
                                      (10, [], 'HWSCHED START')):
            transport = FakeTransport()
            original = transport.send

            def send(m, p, seconds=10, msg=msg, original=original):
                out = original(m, p)
                if m == msg:
                    out = dict(id=m, value=5, status=0)
                return out
            transport.send = send
            report = {}
            with self.assertRaisesRegex(RuntimeError, pattern):
                stock.start(transport, report)
            self.assertNotIn('hwsched_start_accepted', report)

    def test_idle_poll_is_bounded(self):
        class Config:
            def __init__(self, busy):
                self.busy = busy
                self.reads = 0

            def rd(self, name):
                assert name == 'GEN7_GPU_GMU_AO_GPU_CX_BUSY_STATUS'
                self.reads += 1
                return (1 << 23) if self.busy else 0
        report = {}
        self.assertTrue(stock.wait_idle(Config(False), report, 'idle'))
        self.assertFalse(stock.wait_idle(Config(True), report, 'busy', limit_us=0))
        self.assertEqual(report['idle_polls']['busy'], 1)


if __name__ == '__main__':
    unittest.main()
