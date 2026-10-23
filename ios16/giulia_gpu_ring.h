/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_GPU_RING_H
#define GIULIA_GPU_RING_H
#include <stdint.h>
#include <stddef.h>
/* A7XX command-processor ring and timestamp fence for the Ace 5 bring-up.
 *
 * The protocol is not invented here: it is the one the pinned upstream driver
 * runs on A740 hardware (drivers/NativeAdreno/NativeAdrenoRing.cpp), which in
 * turn follows the Linux MSM ring:
 *
 *   attach   CP_RB_BASE/HI = ring IOVA, CP_RB_CNTL =
 *            ilog2(ring_bytes/8) | (ilog2(32/8) << 8), CP_RB_RPTR_ADDR/HI =
 *            fence IOVA, CP_RB_WPTR = 0, CP_SCRATCH(2) = 0.
 *   fence    CP_SCRATCH(2) = seqno, then CP_EVENT_WRITE7 (opcode 0x46) with
 *            four payload words {event 0x31 | ENABLE | IRQ, fence_lo,
 *            fence_hi, seqno}. The GPU stores seqno at fence_iova + 4.
 *   submit   copy words, emit the fence, publish the whole ring page set,
 *            barrier, then write CP_RB_WPTR = end. Completion is the observed
 *            value at fence_iova + 4, never a register that merely accepted
 *            the write.
 *
 * Fence page layout (as upstream, i.e. msm_rbmemptrs): +0 CP read-pointer
 * shadow, +4 completed timestamp. All layout and argument checks happen before
 * the first register write. A failed publish, barrier or WPTR write poisons the
 * ring: the submission state is unknown and must not be retried in place.
 *
 * This file is pure C, allocation free and MMIO free. The caller supplies the
 * register window, cache maintenance, a monotonic clock and a health probe.
 */
enum {
    GGR_RING_BASE = 0x0800,
    GGR_RING_BASE_HI = 0x0801,
    GGR_RING_CONTROL = 0x0802,
    GGR_RING_RPTR_ADDR = 0x0804,
    GGR_RING_RPTR_ADDR_HI = 0x0805,
    GGR_RING_RPTR = 0x0806,
    GGR_RING_WPTR = 0x0807,
    GGR_SCRATCH_2 = 0x0885,
    GGR_EVENT_WRITE7 = 0x46,
    GGR_EVENT_CACHE_CLEAN = 0x31,
    GGR_EVENT_ENABLE = 1u << 27,
    GGR_EVENT_IRQ = 1u << 31,
    GGR_RING_BLOCK_BYTES = 32,
    GGR_FENCE_WORDS = 7,      /* TYPE4 scratch + TYPE7 event, no padding */
    GGR_FENCE_TIMESTAMP = 4   /* dword offset of the CP-written timestamp */
};

enum { GGR_OK = 0, GGR_ARGUMENT, GGR_NOT_READY, GGR_BUSY, GGR_NO_SPACE,
       GGR_IO, GGR_TIMEOUT, GGR_POISONED, GGR_STATE };

enum { GGR_FENCE_COMPLETE = 0, GGR_FENCE_TIMEOUT, GGR_FENCE_POISONED,
       GGR_FENCE_ARGUMENT };

struct ggr_io {
    void *cookie;
    int (*write32)(void *, uint32_t reg, uint32_t value);
    int (*read32)(void *, uint32_t reg, uint32_t *value);
    int (*barrier)(void *);
    /* Clean the owned ring bytes to the point of coherency before the CP reads
     * them, and invalidate the fence page before the CPU reads CP writes. */
    int (*publish)(void *, const volatile uint32_t *words, size_t bytes);
    int (*acquire)(void *, volatile uint32_t *words, size_t bytes);
    uint64_t (*now_us)(void *);
    int (*health)(void *); /* 1 only while no fault was observed */
};

struct ggr_state {
    volatile uint32_t *ring;
    size_t ring_bytes;
    uint64_t ring_iova;
    volatile uint32_t *fence;
    size_t fence_bytes;
    uint64_t fence_iova;
    uint32_t next, rptr, seqno, completed, submissions;
    unsigned attached, poisoned, error, waits;
};

static inline unsigned ggr_ilog2(size_t value)
{
    unsigned result = 0;
    while (value > 1) { value >>= 1; ++result; }
    return result;
}

/* Odd-parity bit exactly as AdrenoPM4.c / cp_type4/7_packet(). */
static inline uint32_t ggr_odd_bit(uint32_t field)
{
    uint32_t parity = 1;
    while (field) { parity ^= 1; field &= field - 1; }
    return parity;
}

static inline int ggr_type4(uint32_t reg, uint32_t count, uint32_t *header)
{
    if (!header || reg > 0x3ffffu || !count || count > 127u || count - 1u > 0x3ffffu - reg)
        return GGR_ARGUMENT;
    *header = 0x40000000u | count | (ggr_odd_bit(count) << 7) |
        (reg << 8) | (ggr_odd_bit(reg) << 27);
    return GGR_OK;
}

static inline int ggr_type7(uint32_t opcode, uint32_t count, uint32_t *header)
{
    if (!header || opcode > 127u || count > 0x3fffu) return GGR_ARGUMENT;
    *header = 0x70000000u | count | (ggr_odd_bit(count) << 15) |
        (opcode << 16) | (ggr_odd_bit(opcode) << 23);
    return GGR_OK;
}

static inline int ggr_io_valid(const struct ggr_io *io)
{
    return io && io->write32 && io->read32 && io->barrier && io->publish &&
        io->acquire && io->now_us && io->health;
}

static inline void ggr_poison(struct ggr_state *s, unsigned error)
{
    s->poisoned = 1;
    s->error = error;
}

/* Validate everything before the first register write. */
static inline int ggr_attach(const struct ggr_io *io, struct ggr_state *s,
    void *ring, size_t ring_bytes, uint64_t ring_iova,
    void *fence, size_t fence_bytes, uint64_t fence_iova)
{
    uint32_t control, rptr = 0;
    size_t i;
    if (!ggr_io_valid(io) || !s || !ring || !fence) return GGR_ARGUMENT;
    if (s->attached || s->poisoned) return GGR_BUSY;
    if (ring_bytes < 8192u || ring_bytes > (1u << 20) || (ring_bytes & (ring_bytes - 1)))
        return GGR_ARGUMENT;
    if ((ring_iova | ring_bytes) & (ring_bytes - 1) || (ring_iova & 4095u))
        return GGR_ARGUMENT;
    if (fence_bytes < 64u || fence_bytes >= (1u << 24) || (fence_iova & 7u))
        return GGR_ARGUMENT;
    s->ring = (volatile uint32_t *)ring;
    s->ring_bytes = ring_bytes;
    s->ring_iova = ring_iova;
    s->fence = (volatile uint32_t *)fence;
    s->fence_bytes = fence_bytes;
    s->fence_iova = fence_iova;
    for (i = 0; i < fence_bytes / 4; ++i) s->fence[i] = 0;
    control = (uint32_t)(ggr_ilog2(ring_bytes / 8) & 0x3fu) |
        ((uint32_t)ggr_ilog2(GGR_RING_BLOCK_BYTES / 8) << 8);
    {
        const uint32_t writes[] = {
            GGR_RING_BASE, (uint32_t)ring_iova,
            GGR_RING_BASE_HI, (uint32_t)(ring_iova >> 32),
            GGR_RING_CONTROL, control,
            GGR_RING_RPTR_ADDR, (uint32_t)fence_iova,
            GGR_RING_RPTR_ADDR_HI, (uint32_t)(fence_iova >> 32),
            GGR_RING_WPTR, 0,
            GGR_SCRATCH_2, 0
        };
        for (i = 0; i < sizeof(writes) / sizeof(writes[0]); i += 2)
            if (io->write32(io->cookie, writes[i], writes[i + 1])) {
                ggr_poison(s, GGR_IO);
                return GGR_IO;
            }
    }
    if (io->read32(io->cookie, GGR_RING_RPTR, &rptr)) {
        ggr_poison(s, GGR_IO);
        return GGR_IO;
    }
    if (rptr >= ring_bytes / 4) { /* an unusable read pointer cannot be reasoned about */
        ggr_poison(s, GGR_STATE);
        return GGR_STATE;
    }
    s->next = rptr;
    s->rptr = rptr;
    s->seqno = s->completed = s->submissions = 0;
    s->attached = 1;
    return GGR_OK;
}

static inline int ggr_read_rptr(const struct ggr_io *io, struct ggr_state *s, uint32_t *value)
{
    uint32_t rptr = 0;
    if (io->read32(io->cookie, GGR_RING_RPTR, &rptr)) {
        ggr_poison(s, GGR_IO);
        return GGR_IO;
    }
    if (rptr >= s->ring_bytes / 4) {
        ggr_poison(s, GGR_STATE);
        return GGR_STATE;
    }
    s->rptr = rptr;
    if (value) *value = rptr;
    return GGR_OK;
}

/* CP_SCRATCH(2) = seqno, then the timestamp event that makes it observable. */
static inline int ggr_emit_fence(struct ggr_state *s, uint32_t at, uint32_t seqno, uint32_t *end)
{
    const uint32_t mask = (uint32_t)(s->ring_bytes / 4 - 1);
    const uint64_t target = s->fence_iova + GGR_FENCE_TIMESTAMP;
    uint32_t words[GGR_FENCE_WORDS] = {0};
    unsigned i;
    if (ggr_type4(GGR_SCRATCH_2, 1, &words[0]) ||
        ggr_type7(GGR_EVENT_WRITE7, 4, &words[2]))
        return GGR_STATE;
    words[1] = seqno;
    words[3] = GGR_EVENT_CACHE_CLEAN | GGR_EVENT_ENABLE | GGR_EVENT_IRQ;
    words[4] = (uint32_t)target;
    words[5] = (uint32_t)(target >> 32);
    words[6] = seqno;
    for (i = 0; i < GGR_FENCE_WORDS; ++i) s->ring[(at + i) & mask] = words[i];
    if (end) *end = (at + GGR_FENCE_WORDS) & mask;
    return GGR_OK;
}

/* Copy a command stream, fence it and commit with one WPTR write. */
static inline int ggr_submit(const struct ggr_io *io, struct ggr_state *s,
    const uint32_t *words, size_t word_count, uint32_t *fence)
{
    uint32_t rptr = 0, at, end = 0, mask, size_words, used;
    size_t i;
    int e;
    if (fence) *fence = 0;
    if (!s || !words || !word_count || !fence) return GGR_ARGUMENT;
    if (!s->attached || s->poisoned) return s->poisoned ? GGR_POISONED : GGR_NOT_READY;
    size_words = (uint32_t)(s->ring_bytes / 4);
    mask = size_words - 1;
    /* Reject before touching ring memory: a partial copy would desynchronise
     * the write pointer from the packets the CP will parse. */
    if (word_count > size_words - GGR_FENCE_WORDS - 8) return GGR_NO_SPACE;
    if ((e = ggr_read_rptr(io, s, &rptr))) return e;
    used = (s->next - rptr) & mask;
    if ((uint32_t)word_count + GGR_FENCE_WORDS + 8 > size_words - used) return GGR_NO_SPACE;
    at = s->next;
    for (i = 0; i < word_count; ++i) s->ring[(at + i) & mask] = words[i];
    at = (at + (uint32_t)word_count) & mask;
    if ((e = ggr_emit_fence(s, at, s->seqno + 1, &end))) return e;
    /* One clean of the whole owned ring: a submission may wrap. */
    if (io->publish(io->cookie, s->ring, s->ring_bytes)) {
        ggr_poison(s, GGR_IO);
        return GGR_IO;
    }
    if (io->barrier(io->cookie)) {
        ggr_poison(s, GGR_IO);
        return GGR_IO;
    }
    if (io->write32(io->cookie, GGR_RING_WPTR, end)) {
        ggr_poison(s, GGR_IO);
        return GGR_IO;
    }
    s->next = end;
    *fence = ++s->seqno;
    ++s->submissions;
    return GGR_OK;
}

/* Poll the CP-written timestamp. Never writes a register, never advances the
 * ring. A timestamp beyond the newest issued fence means the fence page is not
 * what we think it is, so the ring is poisoned instead of reporting progress. */
static inline int ggr_wait(const struct ggr_io *io, struct ggr_state *s,
    uint32_t fence, uint32_t timeout_us, uint32_t *observed)
{
    uint64_t start;
    if (observed) *observed = 0;
    if (!s || !fence) return GGR_FENCE_ARGUMENT;
    if (!s->attached || s->poisoned) return GGR_FENCE_POISONED;
    if (fence > s->seqno) return GGR_FENCE_ARGUMENT;
    start = io->now_us(io->cookie);
    for (;;) {
        uint32_t value;
        if (io->health(io->cookie) != 1) {
            ggr_poison(s, GGR_STATE);
            return GGR_FENCE_POISONED;
        }
        if (io->acquire(io->cookie, s->fence, s->fence_bytes)) {
            ggr_poison(s, GGR_IO);
            return GGR_FENCE_POISONED;
        }
        value = s->fence[GGR_FENCE_TIMESTAMP / 4];
        ++s->waits;
        if (observed) *observed = value;
        if (value > s->seqno) {
            ggr_poison(s, GGR_STATE);
            return GGR_FENCE_POISONED;
        }
        if (value >= fence) {
            s->completed = value;
            return GGR_FENCE_COMPLETE;
        }
        if (io->now_us(io->cookie) - start >= timeout_us) return GGR_FENCE_TIMEOUT;
    }
}

static inline int ggr_observe(const struct ggr_io *io, struct ggr_state *s,
    uint32_t fence, uint32_t *observed)
{
    uint32_t value;
    if (observed) *observed = 0;
    if (!s || !fence) return GGR_FENCE_ARGUMENT;
    if (!s->attached || s->poisoned) return GGR_FENCE_POISONED;
    if (fence > s->seqno) return GGR_FENCE_ARGUMENT;
    if (io->acquire(io->cookie, s->fence, s->fence_bytes)) {
        ggr_poison(s, GGR_IO);
        return GGR_FENCE_POISONED;
    }
    value = s->fence[GGR_FENCE_TIMESTAMP / 4];
    if (observed) *observed = value;
    if (value > s->seqno) {
        ggr_poison(s, GGR_STATE);
        return GGR_FENCE_POISONED;
    }
    return value >= fence ? GGR_FENCE_COMPLETE : GGR_FENCE_TIMEOUT;
}

static inline uint32_t ggr_outstanding(const struct ggr_state *s)
{
    return s->seqno - s->completed;
}

/* Only a fully observed ring may detach; a poisoned ring is never reused. */
static inline int ggr_detach(struct ggr_state *s)
{
    if (!s) return GGR_ARGUMENT;
    if (s->poisoned) return GGR_POISONED;
    if (!s->attached) return GGR_NOT_READY;
    if (ggr_outstanding(s)) return GGR_BUSY;
    s->attached = 0;
    s->ring = 0; s->ring_bytes = 0; s->ring_iova = 0;
    s->fence = 0; s->fence_bytes = 0; s->fence_iova = 0;
    s->next = s->rptr = s->seqno = s->completed = s->submissions = 0;
    return GGR_OK;
}
#endif
