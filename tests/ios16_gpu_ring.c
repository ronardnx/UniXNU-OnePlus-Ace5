/* Host test for the A7XX ring/fence engine. No device, no MMIO.
 *
 * The peer below behaves like the CP: it consumes ring words when the host
 * writes CP_RB_WPTR, performs CP_SCRATCH(2) and CP_EVENT_WRITE7, stores the
 * timestamp at fence_iova + 4 and advances the read pointer. Golden vectors
 * come from the pinned AdrenoPM4 encoding and upstream NativeAdrenoRing.cpp. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/giulia_gpu_ring.h"

#define RING_WORDS 2048u             /* 8 KiB, the minimum the engine accepts */
#define FENCE_WORDS 16u
struct sim {
    uint32_t regs[16][2];
    unsigned nregs;
    uint32_t ring[RING_WORDS];
    uint32_t fence[FENCE_WORDS];
    uint32_t rptr, next;
    uint64_t now;
    unsigned publishes, acquires, barriers, executed;
    int execute;              /* 1: the CP runs packets when WPTR is written */
    int rptr_bad;             /* 1: CP_RB_RPTR reads beyond the ring */
    int timestamp_bogus;      /* 1: the CP writes a fence it never issued */
    int unhealthy;
    int fail_publish, fail_barrier, fail_wptr, fail_read;
    uint32_t last_wptr;
};
static struct sim peer;
static struct ggr_state ring;
static struct ggr_io io;

static uint32_t *regslot(uint32_t reg)
{
    for (unsigned i = 0; i < peer.nregs; ++i)
        if (peer.regs[i][0] == reg) return &peer.regs[i][1];
    assert(peer.nregs < sizeof(peer.regs) / sizeof(peer.regs[0]));
    peer.regs[peer.nregs][0] = reg;
    peer.regs[peer.nregs][1] = 0;
    return &peer.regs[peer.nregs++][1];
}
/* Parse what the host published between `next` and `end` and act on it. */
static void run_cp(uint32_t end)
{
    uint32_t at = peer.next;
    const uint32_t mask = RING_WORDS - 1;
    while (at != end) {
        const uint32_t header = peer.ring[at & mask];
        const uint32_t payload = header & 0x3fffu;
        const uint32_t type = header >> 28;
        if (type == 4) {
            const uint32_t reg = (header >> 8) & 0x3ffffu;
            const uint32_t count = header & 127u;
            if (reg == GGR_SCRATCH_2 && count == 1) {
                uint32_t seqno = peer.ring[(at + 1) & mask];
                peer.fence[1] = peer.timestamp_bogus ? seqno + 99u : peer.fence[1];
            }
            at += 1 + count;
        } else if (type == 7) {
            const uint32_t opcode = (header >> 16) & 0x7fu;
            if (opcode == GGR_EVENT_WRITE7 && payload == 4) {
                const uint32_t value = peer.ring[(at + 4) & mask];
                peer.fence[1] = peer.timestamp_bogus ? value + 99u : value;
                peer.executed++;
            }
            at += 1 + payload;
        } else {
            fprintf(stderr, "bad header %08x at %u end=%u next=%u\n", header, at & mask, end, peer.next);
            assert(0 && "unexpected packet type");
        }
    }
    peer.rptr = end;
    peer.next = end;
}
static int sim_write(void *cookie, uint32_t reg, uint32_t value)
{
    (void)cookie;
    if (peer.fail_wptr && reg == GGR_RING_WPTR) return -1;
    *regslot(reg) = value;
    if (reg == GGR_RING_WPTR) {
        peer.last_wptr = value;
        /* A rewind (attach programs WPTR = 0) is not work: only a forward
         * pointer within half the ring asks the CP to fetch. */
        const uint32_t delta = (value - peer.rptr) & (RING_WORDS - 1);
        if (peer.execute && delta && delta < RING_WORDS / 2) run_cp(value);
    }
    return 0;
}
static int sim_read(void *cookie, uint32_t reg, uint32_t *value)
{
    (void)cookie;
    if (peer.fail_read) return -1;
    if (reg == GGR_RING_RPTR) {
        *value = peer.rptr_bad ? RING_WORDS : peer.rptr;
        return 0;
    }
    *value = *regslot(reg);
    return 0;
}
static int sim_barrier(void *cookie) { (void)cookie; ++peer.barriers; return peer.fail_barrier ? -1 : 0; }
static int sim_publish(void *cookie, const volatile uint32_t *words, size_t bytes)
{
    (void)cookie; (void)words; (void)bytes;
    ++peer.publishes;
    return peer.fail_publish ? -1 : 0;
}
static int sim_acquire(void *cookie, volatile uint32_t *words, size_t bytes)
{
    (void)cookie; (void)words; (void)bytes;
    ++peer.acquires;
    return 0;
}
static uint64_t sim_now(void *cookie) { (void)cookie; peer.now += 10; return peer.now; }
static int sim_health(void *cookie) { (void)cookie; return peer.unhealthy ? 0 : 1; }

static void setup(int execute)
{
    memset(&peer, 0, sizeof(peer));
    peer.execute = execute;
    memset(&ring, 0, sizeof(ring));
    memset(&io, 0, sizeof(io));
    io.cookie = &peer;
    io.write32 = sim_write; io.read32 = sim_read; io.barrier = sim_barrier;
    io.publish = sim_publish; io.acquire = sim_acquire;
    io.now_us = sim_now; io.health = sim_health;
}

int main(void)
{
    const uint64_t ring_iova = 0x61000000u, fence_iova = 0x61700000u;
    /* 1. Attachment writes exactly the upstream register set. */
    setup(1);
    assert(ggr_attach(&io, &ring, peer.ring, 8192, ring_iova, peer.fence, 64, fence_iova) == GGR_OK);
    assert(*regslot(GGR_RING_BASE) == (uint32_t)ring_iova);
    assert(*regslot(GGR_RING_BASE_HI) == 0);
    assert(*regslot(GGR_RING_CONTROL) == 0x20au);        /* ilog2(1024) | ilog2(4)<<8 */
    assert(*regslot(GGR_RING_RPTR_ADDR) == (uint32_t)fence_iova);
    assert(*regslot(GGR_RING_WPTR) == 0);
    assert(*regslot(GGR_SCRATCH_2) == 0);
    for (unsigned i = 0; i < FENCE_WORDS; ++i) assert(peer.fence[i] == 0);
    assert(ring.attached && !ring.poisoned && ring.seqno == 0);

    /* 2. Argument validation refuses before touching a register. */
    setup(1);
    assert(ggr_attach(&io, &ring, peer.ring, 8190, ring_iova, peer.fence, 64, fence_iova) == GGR_ARGUMENT);
    assert(ggr_attach(&io, &ring, peer.ring, 8192, ring_iova + 4, peer.fence, 64, fence_iova) == GGR_ARGUMENT);
    assert(ggr_attach(&io, &ring, peer.ring, 8192, ring_iova, peer.fence, 32, fence_iova) == GGR_ARGUMENT);
    assert(ggr_attach(&io, &ring, peer.ring, 8192, ring_iova, peer.fence, 64, fence_iova + 4) == GGR_ARGUMENT);
    assert(peer.nregs == 0 && peer.publishes == 0);
    assert(ggr_attach(&io, &ring, peer.ring, 8192, ring_iova, peer.fence, 64, fence_iova) == GGR_OK);
    assert(ggr_attach(&io, &ring, peer.ring, 8192, ring_iova, peer.fence, 64, fence_iova) == GGR_BUSY);

    /* 3. One submission: golden fence words, whole-ring publish, WPTR commit. */
    setup(1);
    assert(ggr_attach(&io, &ring, peer.ring, 8192, ring_iova, peer.fence, 64, fence_iova) == GGR_OK);
    {
        const uint32_t cmds[] = {0x703d8003u, 0x40u, 0x0u, 0xa750cafeu};
        uint32_t fence = 0, observed = 0;
        assert(ggr_submit(&io, &ring, cmds, 4, &fence) == GGR_OK && fence == 1);
        assert(peer.ring[0] == cmds[0] && peer.ring[3] == cmds[3]);
        assert(peer.ring[4] == 0x48088501u && peer.ring[5] == 1u);   /* TYPE4 CP_SCRATCH(2) */
        assert(peer.ring[6] == 0x70460004u);                          /* TYPE7 CP_EVENT_WRITE7, 4 */
        assert(peer.ring[7] == 0x88000031u);                          /* event | ENABLE | IRQ */
        assert(peer.ring[8] == (uint32_t)(fence_iova + 4));
        assert(peer.ring[9] == 0u && peer.ring[10] == 1u);
        assert(peer.last_wptr == 11u && ring.next == 11u);
        assert(peer.publishes == 1 && peer.barriers == 1 && peer.executed == 1);
        assert(peer.fence[1] == 1u);
        assert(ggr_wait(&io, &ring, fence, 1000, &observed) == GGR_FENCE_COMPLETE);
        assert(observed == 1u && ring.completed == 1u && ggr_outstanding(&ring) == 0);
    }

    /* 4. Two submissions keep sequence numbers and completion monotonic. */
    {
        uint32_t fence = 0, observed = 0;
        assert(ggr_submit(&io, &ring, (const uint32_t[]){0x70000000u}, 1, &fence) == GGR_OK && fence == 2);
        assert(peer.ring[11] == 0x70000000u);
        assert(ggr_wait(&io, &ring, 2, 1000, &observed) == GGR_FENCE_COMPLETE && observed == 2);
        assert(ggr_observe(&io, &ring, 2, &observed) == GGR_FENCE_COMPLETE);
        assert(ggr_observe(&io, &ring, 3, &observed) == GGR_FENCE_ARGUMENT);
    }

    /* 5. Wrap-around: packets continue at index 0 and the fence still lands. */
    setup(1);
    assert(ggr_attach(&io, &ring, peer.ring, 8192, ring_iova, peer.fence, 64, fence_iova) == GGR_OK);
    {
        uint32_t big[1000];
        uint32_t fence = 0, observed = 0;
        for (unsigned i = 0; i < 1000; ++i) big[i] = 0x70000000u;
        assert(ggr_submit(&io, &ring, big, 1000, &fence) == GGR_OK && fence == 1);   /* 0..1006 */
        assert(ggr_submit(&io, &ring, big, 1000, &fence) == GGR_OK && fence == 2);   /* 1007..2013 */
        assert(ggr_submit(&io, &ring, big, 100, &fence) == GGR_OK && fence == 3);    /* wraps */
        assert(ring.next < 200u);
        assert(peer.ring[0] == big[0]);
        assert(ggr_wait(&io, &ring, 3, 1000, &observed) == GGR_FENCE_COMPLETE && observed == 3);
    }

    /* 6. A ring the CP never drains fills up and is refused, not overwritten. */
    setup(0);
    assert(ggr_attach(&io, &ring, peer.ring, 8192, ring_iova, peer.fence, 64, fence_iova) == GGR_OK);
    {
        uint32_t big[1000], fence = 0;
        for (unsigned i = 0; i < 1000; ++i) big[i] = 0x70000000u;
        assert(ggr_submit(&io, &ring, big, 1000, &fence) == GGR_OK);
        assert(ggr_submit(&io, &ring, big, 1000, &fence) == GGR_OK);
        assert(ggr_submit(&io, &ring, big, 1000, &fence) == GGR_NO_SPACE);
        assert(ggr_submit(&io, &ring, big, 4000, &fence) == GGR_NO_SPACE);
        assert(peer.last_wptr == 2014u);
    }

    /* 7. No execution means no completion: the wait times out, nothing else. */
    {
        uint32_t observed = 0;
        assert(ggr_wait(&io, &ring, 1, 200, &observed) == GGR_FENCE_TIMEOUT);
        assert(observed == 0 && !ring.poisoned);
        assert(ring.waits > 1u);
    }

    /* 8. A timestamp the ring never issued is not progress: poison. */
    setup(1);
    peer.timestamp_bogus = 1;
    assert(ggr_attach(&io, &ring, peer.ring, 8192, ring_iova, peer.fence, 64, fence_iova) == GGR_OK);
    {
        uint32_t fence = 0, observed = 0;
        assert(ggr_submit(&io, &ring, (const uint32_t[]){0x70000000u}, 1, &fence) == GGR_OK);
        assert(ggr_wait(&io, &ring, fence, 1000, &observed) == GGR_FENCE_POISONED);
        assert(ring.poisoned && peer.fence[1] > ring.seqno);
        assert(ggr_submit(&io, &ring, (const uint32_t[]){0}, 1, &fence) == GGR_POISONED);
        assert(ggr_detach(&ring) == GGR_POISONED);
    }

    /* 9. Publication, barrier and WPTR failures poison instead of retrying. */
    for (unsigned which = 0; which < 3; ++which) {
        setup(1);
        assert(ggr_attach(&io, &ring, peer.ring, 8192, ring_iova, peer.fence, 64, fence_iova) == GGR_OK);
        /* Inject after attach: attach itself writes CP_RB_WPTR = 0. */
        if (which == 0) peer.fail_publish = 1;
        else if (which == 1) peer.fail_barrier = 1;
        else peer.fail_wptr = 1;
        {
            uint32_t fence = 0;
            assert(ggr_submit(&io, &ring, (const uint32_t[]){0x70000000u}, 1, &fence) == GGR_IO);
            assert(ring.poisoned && fence == 0 && ring.seqno == 0);
        }
    }

    /* 10. An unusable read pointer or a fault never becomes progress. */
    setup(1);
    peer.rptr_bad = 1;
    assert(ggr_attach(&io, &ring, peer.ring, 8192, ring_iova, peer.fence, 64, fence_iova) == GGR_STATE);
    assert(ring.poisoned);
    setup(1);
    assert(ggr_attach(&io, &ring, peer.ring, 8192, ring_iova, peer.fence, 64, fence_iova) == GGR_OK);
    {
        uint32_t fence = 0, observed = 0;
        assert(ggr_submit(&io, &ring, (const uint32_t[]){0x70000000u}, 1, &fence) == GGR_OK);
        peer.unhealthy = 1;
        assert(ggr_wait(&io, &ring, fence, 1000, &observed) == GGR_FENCE_POISONED);
        assert(ring.poisoned);
    }

    /* 11. Detach only after every submission was observed. */
    setup(1);
    assert(ggr_attach(&io, &ring, peer.ring, 8192, ring_iova, peer.fence, 64, fence_iova) == GGR_OK);
    {
        uint32_t fence = 0, observed = 0;
        assert(ggr_submit(&io, &ring, (const uint32_t[]){0x70000000u}, 1, &fence) == GGR_OK);
        assert(ggr_detach(&ring) == GGR_BUSY);
        assert(ggr_wait(&io, &ring, fence, 1000, &observed) == GGR_FENCE_COMPLETE);
        assert(ggr_detach(&ring) == GGR_OK);
        assert(ggr_attach(&io, &ring, peer.ring, 8192, ring_iova, peer.fence, 64, fence_iova) == GGR_OK);
    }

    /* 12. Misuse is refused without side effects. */
    setup(1);
    {
        uint32_t fence = 7;
        assert(ggr_submit(&io, &ring, (const uint32_t[]){0}, 1, &fence) == GGR_NOT_READY && fence == 0);
        assert(ggr_submit(&io, &ring, 0, 1, &fence) == GGR_ARGUMENT);
        assert(ggr_wait(&io, &ring, 1, 10, 0) == GGR_FENCE_POISONED); /* upstream: not attached is poisoned */
        assert(peer.nregs == 0 && peer.publishes == 0);
    }
    printf("RING: 0x20a control word, golden fence packets, whole-ring publish, "
           "wrap-around, full-ring refusal, timeouts, poison on failed publish/"
           "barrier/WPTR, unusable rptr, fault and misuse passed\n");
    return 0;
}
