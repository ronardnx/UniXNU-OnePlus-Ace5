"""A7XX CP ring and timestamp fence on the running device.

Same protocol as ios16/giulia_gpu_ring.h (and upstream NativeAdrenoRing.cpp):

  CP_RB_BASE/HI   ring IOVA
  CP_RB_CNTL      ilog2(ring_bytes/8) | (ilog2(32/8) << 8)
  CP_RB_RPTR_ADDR fence IOVA (the CP writes its read pointer at +0)
  CP_SCRATCH(2)   sequence number
  CP_EVENT_WRITE7 opcode 0x46, four words {0x31 | ENABLE | IRQ, fence+4, hi, seq}
  CP_RB_WPTR      committed write pointer

Completion is the value the GPU stores at fence_iova + 4, never the fact that a
register accepted a write. Every step is bounded and recorded.
"""
import time

RB_BASE = 0x800
RB_BASE_HI = 0x801
RB_CNTL = 0x802
RB_RPTR_ADDR = 0x804
RB_RPTR_ADDR_HI = 0x805
RB_RPTR = 0x806
RB_WPTR = 0x807
SCRATCH_2 = 0x885
EVENT_WRITE7 = 0x46
EVENT_CACHE_CLEAN = 0x31
EVENT_ENABLE = 1 << 27
EVENT_IRQ = 1 << 31


def packet(opcode, count):
    """cp_type7_packet with odd parity, identical to the C engine."""
    def odd(value):
        return 1 ^ (bin(value).count('1') & 1)
    return (0x70000000 | count | (odd(count) << 15) | ((opcode & 0x7f) << 16)
            | (odd(opcode & 0x7f) << 23))


def type4(reg, count):
    """cp_type4_packet for a register sequence with one or more values."""
    def odd(value):
        return 1 ^ (bin(value).count('1') & 1)
    return (0x40000000 | count | (odd(count) << 7) | ((reg & 0x3ffff) << 8)
            | (odd(reg & 0x3ffff) << 27))


def ilog2(value):
    if value <= 0 or value & (value - 1):
        raise ValueError('ilog2 needs a power of two')
    return value.bit_length() - 1


def fence_words(seq, fence_gpu):
    """CP_SCRATCH(2) = seq, then the timestamp event into fence_iova + 4."""
    target = fence_gpu + 4
    return [type4(SCRATCH_2, 1), seq,
            packet(EVENT_WRITE7, 4),
            EVENT_CACHE_CLEAN | EVENT_ENABLE | EVENT_IRQ,
            target & 0xffffffff, (target >> 32) & 0xffffffff, seq]


def attach(live, transport, report, ring_bytes=8192, fence_bytes=4096):
    if ring_bytes < 8192 or ring_bytes > (1 << 20) or ring_bytes & (ring_bytes - 1):
        raise ValueError('ring must be a power of two in [8 KiB, 1 MiB]')
    ring = transport.allocate(ring_bytes, ring_bytes, 0x33f, 2)
    fence = transport.allocate(fence_bytes, 4096, 0x33f, 2)
    transport.dev(4, ring['offset'], ring_bytes)
    transport.dev(4, fence['offset'], fence_bytes)
    for allocation in (ring, fence):
        if allocation['gpu'] & (ring_bytes - 1 if allocation is ring else 7):
            raise RuntimeError('allocation is not aligned for its role')
    control = ilog2(ring_bytes // 8) | (ilog2(4) << 8)
    writes = [(RB_BASE, ring['gpu'] & 0xffffffff), (RB_BASE_HI, ring['gpu'] >> 32),
              (RB_CNTL, control),
              (RB_RPTR_ADDR, fence['gpu'] & 0xffffffff), (RB_RPTR_ADDR_HI, fence['gpu'] >> 32),
              (RB_WPTR, 0), (SCRATCH_2, 0)]
    for reg, value in writes:
        live.write(0x3d00000 + 4 * reg, value)
    rptr = live.read(0x3d00000 + 4 * RB_RPTR)
    if rptr >= ring_bytes // 4:
        raise RuntimeError('CP_RB_RPTR is outside the ring: ' + hex(rptr))
    state = dict(ring=ring, fence=fence, ring_bytes=ring_bytes, control=control,
                 next=rptr, seqno=0, completed=0, submissions=0)
    report['ring'] = dict(ring_gpu=hex(ring['gpu']), fence_gpu=hex(fence['gpu']),
                          ring_bytes=ring_bytes, control=hex(control), initial_rptr=rptr)
    return state


def submit(live, transport, report, state, words):
    ring, mask = state['ring'], state['ring_bytes'] // 4 - 1
    size_words = state['ring_bytes'] // 4
    rptr = live.read(0x3d00000 + 4 * RB_RPTR)
    used = (state['next'] - rptr) & mask
    seq = state['seqno'] + 1
    stream = list(words) + fence_words(seq, state['fence']['gpu'])
    if len(stream) + 8 > size_words - used:
        raise RuntimeError('ring has no space: used=' + str(used))
    for i, value in enumerate(stream):
        transport.write(ring['offset'] + 4 * ((state['next'] + i) & mask), [value])
    end = (state['next'] + len(stream)) & mask
    live.write(0x3d00000 + 4 * RB_WPTR, end)
    state['next'], state['seqno'], state['submissions'] = end, seq, state['submissions'] + 1
    report.setdefault('ring_submissions', []).append(
        dict(seq=seq, wptr=end, words=len(stream), packets=len(words)))
    return seq


def wait(live, transport, report, state, seq, timeout_us=200000):
    offset = state['fence']['offset'] + 4
    deadline = time.monotonic() + timeout_us / 1e6
    value = 0
    while time.monotonic() < deadline:
        value = transport.read(offset)[0]
        if value == seq:
            state['completed'] = seq
            report.setdefault('ring_fences', []).append(dict(seq=seq, observed=value, complete=True))
            return True
        if value > seq:
            report['ring_fence_error'] = dict(seq=seq, observed=value, reason='unissued-timestamp')
            raise RuntimeError('fence page holds a timestamp this ring never issued')
        time.sleep(0.00005)
    faults = {
        'cp_hw_fault': live.read(0x3d00000 + 4 * 0x821),
        'cp_interrupt_status': live.read(0x3d00000 + 4 * 0x823),
        'rbbm_status': live.read(0x3d00000 + 4 * 0x210),
    }
    report['ring_fence_error'] = dict(seq=seq, observed=value, faults=faults, reason='timeout')
    return False


def run(live, transport, report, ring_bytes=8192):
    """Attach, submit one visible write plus a fence, and verify both."""
    state = attach(live, transport, report, ring_bytes)
    target = transport.allocate(4096, 4096, 0x33f, 2)
    transport.write(target['offset'], [0])
    commands = [packet(0x3d, 3), target['gpu'] & 0xffffffff, target['gpu'] >> 32, 0xa750cafe]
    seq = submit(live, transport, report, state, commands)
    if not wait(live, transport, report, state, seq):
        raise RuntimeError('ring fence did not complete')
    value = transport.read(target['offset'])[0]
    report['ring_write_value'] = value
    if value != 0xa750cafe:
        raise RuntimeError('ring submission did not reach the GPU: ' + hex(value))
    report['ring_verified'] = True
    report['ring_fence_verified'] = True
    return state
