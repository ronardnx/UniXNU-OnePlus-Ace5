import sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import gpu_terminal_ring as ring


class FakeTransport:
    """Answers allocation, owned-RAM reads/writes and nothing else."""

    def __init__(self, fence_gpu=0x61700000):
        self.fence_gpu = fence_gpu
        self.ring_gpu = 0x61000000
        self.allocations = []
        self.memory = {}
        self.writes = []

    def allocate(self, size, align, flags, kind):
        index = len(self.allocations)
        gpu = self.ring_gpu if index == 0 else self.fence_gpu
        allocation = dict(handle=index, offset=0x400000 + index * size, bytes=size,
                          flags=flags, kind=kind, gpu=gpu)
        self.allocations.append(allocation)
        return allocation

    def dev(self, op, *args):
        return ''

    def write(self, offset, words):
        for i, value in enumerate(words):
            self.writes.append((offset + 4 * i, value))
            self.memory[offset + 4 * i] = value

    def read(self, offset, count=1):
        return [self.memory.get(offset + 4 * i, 0) for i in range(count)]


class FakeLive:
    def __init__(self, rptr=0):
        self.regs = {}
        self.rptr = rptr
        self.writes = []

    def write(self, address, value):
        self.writes.append((address, value))
        self.regs[address] = value

    def read(self, address):
        return self.rptr if address == 0x3d00000 + 4 * ring.RB_RPTR else self.regs.get(address, 0)


class RingProtocol(unittest.TestCase):
    def test_packet_encoding_matches_the_pinned_pm4_kit(self):
        self.assertEqual(ring.packet(0x17, 1), 0x70970001)
        self.assertEqual(ring.packet(0x48, 7), 0x70c80007)
        self.assertEqual(ring.packet(0x3d, 3), 0x703d8003)
        self.assertEqual(ring.packet(0x46, 4), 0x70460004)
        self.assertEqual(ring.type4(0x885, 1), 0x48088501)

    def test_fence_words_are_the_upstream_seven(self):
        words = ring.fence_words(3, 0x61700000)
        self.assertEqual(words, [0x48088501, 3, 0x70460004, 0x88000031,
                                 0x61700004, 0, 3])

    def test_attach_programs_the_exact_register_plan(self):
        live, transport, report = FakeLive(), FakeTransport(), {}
        state = ring.attach(live, transport, report)
        self.assertEqual([address for address, _ in live.writes],
                         [0x3d00000 + 4 * r for r in (ring.RB_BASE, ring.RB_BASE_HI, ring.RB_CNTL,
                                                      ring.RB_RPTR_ADDR, ring.RB_RPTR_ADDR_HI,
                                                      ring.RB_WPTR, ring.SCRATCH_2)])
        values = dict(live.writes)
        self.assertEqual(values[0x3d00000 + 4 * ring.RB_BASE], 0x61000000)
        self.assertEqual(values[0x3d00000 + 4 * ring.RB_CNTL], 0x20a)
        self.assertEqual(values[0x3d00000 + 4 * ring.RB_RPTR_ADDR], 0x61700000)
        self.assertEqual(values[0x3d00000 + 4 * ring.RB_WPTR], 0)
        self.assertEqual(transport.writes, [])          # only dev(4) zeroing
        self.assertEqual(report['ring']['control'], '0x20a')
        self.assertEqual(state['next'], 0)

    def test_submit_publishes_the_stream_and_advances_the_write_pointer(self):
        live, transport, report = FakeLive(), FakeTransport(), {}
        state = ring.attach(live, transport, report)
        commands = [0x703d8003, 0x40, 0, 0xa750cafe]
        seq = ring.submit(live, transport, report, state, commands)
        self.assertEqual(seq, 1)
        stream = commands + ring.fence_words(1, transport.fence_gpu)
        self.assertEqual([value for _, value in transport.writes], stream)
        self.assertEqual(live.writes[-1], (0x3d00000 + 4 * ring.RB_WPTR, len(stream)))
        self.assertEqual(state['next'], len(stream))
        self.assertEqual(report['ring_submissions'][0]['words'], len(stream))

    def test_wait_accepts_only_the_issued_timestamp(self):
        live, transport, report = FakeLive(), FakeTransport(), {}
        state = ring.attach(live, transport, report)
        seq = ring.submit(live, transport, report, state, [0x70000000])
        transport.memory[state['fence']['offset'] + 4] = seq
        self.assertTrue(ring.wait(live, transport, report, state, seq, timeout_us=1000))
        self.assertEqual(state['completed'], seq)

    def test_a_timestamp_the_ring_never_issued_is_refused(self):
        live, transport, report = FakeLive(), FakeTransport(), {}
        state = ring.attach(live, transport, report)
        seq = ring.submit(live, transport, report, state, [0x70000000])
        transport.memory[state['fence']['offset'] + 4] = seq + 99
        with self.assertRaisesRegex(RuntimeError, 'never issued'):
            ring.wait(live, transport, report, state, seq, timeout_us=1000)
        self.assertEqual(report['ring_fence_error']['reason'], 'unissued-timestamp')

    def test_a_missing_fence_reports_timeout_with_faults(self):
        live, transport, report = FakeLive(), FakeTransport(), {}
        state = ring.attach(live, transport, report)
        seq = ring.submit(live, transport, report, state, [0x70000000])
        self.assertFalse(ring.wait(live, transport, report, state, seq, timeout_us=1))
        self.assertEqual(report['ring_fence_error']['reason'], 'timeout')
        self.assertIn('cp_hw_fault', report['ring_fence_error']['faults'])

    def test_ring_and_fence_must_be_aligned(self):
        transport = FakeTransport(fence_gpu=0x61700004)   # not 8-byte aligned
        with self.assertRaises(RuntimeError):
            ring.attach(FakeLive(), transport, {})
        transport = FakeTransport()
        transport.ring_gpu = 0x61001000                     # not ring_bytes aligned
        with self.assertRaises(RuntimeError):
            ring.attach(FakeLive(), transport, {})

    def test_no_space_is_refused_before_writing(self):
        live, transport, report = FakeLive(), FakeTransport(), {}
        state = ring.attach(live, transport, report)
        state['next'] = 1000
        with self.assertRaisesRegex(RuntimeError, 'no space'):
            ring.submit(live, transport, report, state, [0x70000000] * 3000)
        self.assertEqual(transport.writes, [])


if __name__ == '__main__':
    unittest.main()
