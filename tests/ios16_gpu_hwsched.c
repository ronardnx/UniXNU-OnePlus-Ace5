/* Host test for the gen7_9_1 hwsched bring-up engine. No device, no MMIO.
 *
 * The peer below plays the GMU: it ACKs every acknowledged HFI message the way
 * adreno_gen7_hfi.c does, never ACKs H2F_MSG_ISSUE_CMD_RAW, and only clears the
 * GPU busy bit after it has "executed" the CP packets it received. The engine
 * must therefore reach readiness without any reply on message 131, and every
 * negative case must fail instead of reporting progress. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/giulia_gpu_hwsched.h"

#define SIM_MAX_MSG 32
#define SIM_MAX_WORDS 24
struct sim {
    struct { uint32_t reg, value; } regs[32];
    unsigned nregs;
    uint32_t ids[SIM_MAX_MSG];
    unsigned lens[SIM_MAX_MSG];
    uint32_t words[SIM_MAX_MSG][SIM_MAX_WORDS];
    uint32_t ack_header[SIM_MAX_MSG];
    uint32_t ack_value[SIM_MAX_MSG];
    unsigned ack_present[SIM_MAX_MSG];
    unsigned acks, sent;
    uint32_t mem[64];
    int busy_pending;       /* polls the busy bit stays set */
    int acks_enabled;       /* 0: the GMU never answers (raw-only behaviour) */
    int perform_mem_write;  /* 0: the CP silently does nothing */
    int never_idle;         /* 1: the busy bit never clears */
    uint32_t ack_error;     /* value returned in every ACK (GMU_SUCCESS is 0) */
    uint32_t fault[3];      /* cp_hw_fault, cp_interrupt, cp_protect */
    unsigned healthy_calls, waits;
};

static uint32_t *sim_reg(struct sim *p, uint32_t reg)
{
    for (unsigned i = 0; i < p->nregs; ++i)
        if (p->regs[i].reg == reg) return &p->regs[i].value;
    assert(p->nregs < sizeof(p->regs)/sizeof(p->regs[0]));
    p->regs[p->nregs].reg = reg;
    p->regs[p->nregs].value = 0;
    return &p->regs[p->nregs++].value;
}
static int sim_read(void *cookie, uint32_t reg, uint32_t *value)
{
    struct sim *p = cookie;
    if (reg == GGHS_GMU_AO_GPU_CX_BUSY_STATUS) {
        /* The peer finishes one unit of work per observation. */
        if (p->never_idle) { *value = 1u << 23; return 0; }
        if (p->busy_pending > 0) { *value = 1u << 23; p->busy_pending--; }
        else *value = 0;
        return 0;
    }
    if (reg == GGHS_CP_HW_FAULT) { *value = p->fault[0]; return 0; }
    if (reg == GGHS_CP_INTERRUPT_STATUS) { *value = p->fault[1]; return 0; }
    if (reg == GGHS_CP_PROTECT_STATUS) { *value = p->fault[2]; return 0; }
    *value = *sim_reg(p, reg);
    return 0;
}
static int sim_write(void *cookie, uint32_t reg, uint32_t value)
{
    struct sim *p = cookie;
    *sim_reg(p, reg) = value;
    return 0;
}
static int sim_barrier(void *cookie) { (void)cookie; return 0; }
static int sim_wait_us(void *cookie, unsigned us) { struct sim *p = cookie; p->waits++; (void)us; return 0; }
static int sim_healthy(void *cookie)
{
    struct sim *p = cookie;
    p->healthy_calls++;
    return (!p->fault[0] && !p->fault[1] && !p->fault[2]) ? 1 : 0;
}
static int sim_read_mem(void *cookie, uint32_t offset, uint32_t *value)
{
    struct sim *p = cookie;
    if ((offset & 3u) || offset / 4 >= sizeof(p->mem)/sizeof(p->mem[0])) return -1;
    *value = p->mem[offset / 4];
    return 0;
}
static void sim_ack(struct sim *p, uint32_t header, uint32_t value)
{
    assert(p->acks < SIM_MAX_MSG);
    p->ack_header[p->acks] = header;
    p->ack_value[p->acks] = value;
    p->ack_present[p->acks] = 1;
    p->acks++;
}
static int sim_wait_ack(void *cookie, uint32_t header, uint32_t *value, uint32_t *status,
    unsigned timeout_us)
{
    struct sim *p = cookie;
    (void)timeout_us;
    for (unsigned i = 0; i < p->acks; ++i)
        if (p->ack_present[i] && p->ack_header[i] == header) {
            *value = p->ack_value[i];
            *status = 0;
            p->ack_present[i] = 0;
            return 1;
        }
    (void)p;
    return 0; /* never invents a late ACK */
}
/* Execute the CP packets. The GMU itself only converts the raw message into CP
 * work; it never answers it. */
static void sim_cp(struct sim *p, const uint32_t *w, unsigned n)
{
    for (unsigned i = 1; i + 1 < n; ) {
        uint32_t pkt = w[i];
        if ((pkt & 0xf0000000u) != 0x70000000u) return;
        unsigned count = pkt & 0x3fffu;
        unsigned op = (pkt >> 16) & 0x7fu;
        if (op == GGHS_OP_CP_ME_INIT) { p->busy_pending = 2; return; }
        if (op == GGHS_OP_CP_MEM_WRITE && count == 3) {
            p->busy_pending = 2;
            /* payload: addr_lo, addr_hi, value */
            uint64_t addr = ((uint64_t)w[i+2] << 32) | w[i+1];
            if (p->perform_mem_write && addr / 4 < sizeof(p->mem)/sizeof(p->mem[0]))
                p->mem[addr / 4] = w[i + 3];
            return;
        }
        if (op == GGHS_OP_CP_EVENT_WRITE && count == 3) {
            p->busy_pending = 2;
            /* payload: event, addr_lo, addr_hi */
            uint64_t addr = ((uint64_t)w[i+3] << 32) | w[i+2];
            if (p->perform_mem_write && addr / 4 < sizeof(p->mem)/sizeof(p->mem[0]))
                p->mem[addr / 4] = 0x00c0ffeeu;
            return;
        }
        if (op == GGHS_OP_CP_SET_SECURE_MODE) { p->busy_pending = 2; *sim_reg(p, GGHS_RBBM_SECVID_TRUST_CNTL) = 0; return; }
        i += 1 + count;
    }
}
static int sim_send(void *cookie, const uint32_t *w, unsigned n)
{
    struct sim *p = cookie;
    uint32_t id = w[0] & 0xffu;
    if (((w[0] >> 8) & 0xffu) != n) return -1;
    if (p->sent < SIM_MAX_MSG && n <= SIM_MAX_WORDS) {
        p->ids[p->sent] = id;
        p->lens[p->sent] = n;
        memcpy(p->words[p->sent], w, n * sizeof(uint32_t));
        p->sent++;
    }
    if (id == GGHS_MSG_ISSUE_CMD_RAW) { sim_cp(p, w, n); return 0; }
    if (!p->acks_enabled) return 0;
    if (id == GGHS_MSG_GET_VALUE)
        sim_ack(p, w[0], p->ack_error ? p->ack_error : (w[1] == 121 ? 1u : 0u));
    else sim_ack(p, w[0], p->ack_error);
    return 0;
}
static void sim_init(struct sim *p)
{
    memset(p, 0, sizeof(*p));
    p->acks_enabled = 1;
    p->perform_mem_write = 1;
}

static struct gghs_state state;
static struct gghs_io io;
static struct sim peer;
static struct gghs_caps caps;
static struct gghs_run run;
static unsigned zap_calls;

static int fake_zap(void *cookie) { (void)cookie; zap_calls++; return 0; }

static void setup(int with_zap)
{
    sim_init(&peer);
    memset(&caps, 0, sizeof(caps));
    caps.hw_fence = caps.lpac = caps.aqe = caps.dms = 1;
    caps.acd = caps.bcl = 1;
    caps.preemption = 1; caps.preempt_level = 1; caps.bcl_data = 1;
    memset(&run, 0, sizeof(run));
    run.sqe_iova = 0x61796000u;
    run.aqe_iova = 0x617ac000u;
    run.restore_list = 0x61794000u;
    run.test_gpu_addr_lo = 0x40u;
    run.test_gpu_addr_hi = 0;
    run.test_offset = 0x40;
    run.test_value = 0xa750cafeu;
    run.ack_timeout_us = 1000; run.idle_timeout_us = 200;
    run.zap = with_zap ? fake_zap : 0;
    zap_calls = 0;
    memset(&state, 0, sizeof(state));
    memset(&io, 0, sizeof(io));
    io.cookie = &peer;
    io.read = sim_read; io.write = sim_write; io.barrier = sim_barrier;
    io.wait_us = sim_wait_us; io.send = sim_send; io.wait_ack = sim_wait_ack;
    io.read_mem = sim_read_mem; io.healthy = sim_healthy;
}
static unsigned feature_set_secure(const struct sim *p)
{
    unsigned n = 0;
    for (unsigned i = 0; i < p->sent; ++i)
        if (p->ids[i] == GGHS_MSG_ISSUE_CMD_RAW && p->words[i][1] == 0x70e60001u) n++;
    return n;
}

int main(void)
{
    /* 1. PM4 headers against the vendor parity rule. */
    {
        uint32_t p = 0;
        assert(!gghs_type7(GGHS_OP_CP_THREAD_CONTROL, 1, &p) && p == 0x70970001u);
        assert(!gghs_type7(GGHS_OP_CP_ME_INIT, 7, &p) && p == 0x70c80007u);
        assert(!gghs_type7(GGHS_OP_CP_MEM_WRITE, 3, &p) && p == 0x703d8003u);
        assert(!gghs_type7(GGHS_OP_CP_EVENT_WRITE, 3, &p) && p == 0x70468003u);
        assert(!gghs_type7(GGHS_OP_CP_SET_SECURE_MODE, 1, &p) && p == 0x70e60001u);
        assert(gghs_type7(0x80, 1, &p) == GGHS_ARGUMENT);
        assert(gghs_type7(1, 0x4000, &p) == GGHS_ARGUMENT);
    }
    /* 2. CP_ME_INIT ordinal mask: bit 11 means DMS unsupported. */
    assert(gghs_me_init_mask(1) == 0x14bu);
    assert(gghs_me_init_mask(0) == 0x94bu);
    /* 3. The exact eleven-word message, with the header the r5 capture showed
     *    (0x91600b83 for sequence 0x916). */
    {
        uint32_t w[11];
        unsigned n = 0;
        struct gghs_state s;
        memset(&s, 0, sizeof(s));
        assert(!gghs_cp_init_message(0x916, 0x61794000u, 1, w, 11, &n, &s));
        assert(n == 11 && w[0] == 0x91600b83u);
        assert(w[1] == 0x70970001u && w[2] == (1u << 27));
        assert(w[3] == 0x70c80007u && w[4] == 0x14bu && w[5] == 3);
        assert(w[6] == 0x20000000u && w[7] == 2);
        assert(w[8] == 0x61794000u && w[9] == 0 && w[10] == 0x80000000u);
        assert(s.cp_init_header == w[0] && s.cp_init_mask == 0x14bu);
        assert(gghs_cp_init_message(1, 1, 1, w, 10, &n, 0) == GGHS_ARGUMENT);
        assert(!gghs_cp_init_message(1, 0, 1, w, 11, &n, 0));
        assert((w[4] & GGHS_MEINIT_REGISTER_INIT_LIST) == 0); /* list bit cleared */
    }
    /* 4. Full cold path: exact vendor message order, no ACK for message 131. */
    setup(1);
    peer.mem[run.test_offset / 4] = 0;
    assert(gghs_boot(&io, &state, &caps, &run) == GGHS_OK);
    assert(state.phase == GGHS_PHASE_READY);
    assert(zap_calls == 1);
    assert(peer.mem[run.test_offset / 4] == 0xa750cafeu);
    assert(state.context_queue == 1);
    assert(state.feature_acks == 11);
    {
        static const uint32_t expect[] = {
            GGHS_MSG_FEATURE_CTRL, GGHS_MSG_FEATURE_CTRL, GGHS_MSG_FEATURE_CTRL,
            GGHS_MSG_FEATURE_CTRL, GGHS_MSG_FEATURE_CTRL, GGHS_MSG_FEATURE_CTRL,
            GGHS_MSG_FEATURE_CTRL, GGHS_MSG_CORE_FW_START, GGHS_MSG_GET_VALUE,
            GGHS_MSG_FEATURE_CTRL, GGHS_MSG_FEATURE_CTRL, GGHS_MSG_FEATURE_CTRL,
            GGHS_MSG_FEATURE_CTRL, GGHS_MSG_START, GGHS_MSG_ISSUE_CMD_RAW,
            GGHS_MSG_ISSUE_CMD_RAW, GGHS_MSG_ISSUE_CMD_RAW
        };
        assert(peer.sent == sizeof(expect)/sizeof(expect[0]));
        for (unsigned i = 0; i < peer.sent; ++i) assert(peer.ids[i] == expect[i]);
        /* Vendor feature order inside the first block. */
        static const uint32_t features[] = {
            GGHS_FEAT_ACD, GGHS_FEAT_BCL, GGHS_FEAT_HWSCHED, GGHS_FEAT_KPROF,
            GGHS_FEAT_DMS, GGHS_FEAT_PERF_NORETAIN, GGHS_FEAT_BAIL_OUT_TIMER
        };
        for (unsigned i = 0; i < 7; ++i) {
            assert(peer.words[i][1] == features[i]);
            assert(peer.words[i][2] == (features[i] == GGHS_FEAT_PERF_NORETAIN ? 0u : 1u));
        }
        assert(peer.words[1][3] == 1);  /* BCL data: response type bit */
        assert(peer.words[9][1] == GGHS_FEAT_HW_FENCE);
        assert(peer.words[10][1] == GGHS_FEAT_PREEMPTION && peer.words[10][3] == 1);
        assert(peer.words[11][1] == GGHS_FEAT_LPAC);
        assert(peer.words[12][1] == GGHS_FEAT_AQE);
        assert(peer.words[14][4] == 0x14bu);      /* DMS supported */
        assert(peer.words[14][8] == 0x61794000u); /* power-up list address */
        assert(peer.words[15][1] == 0x70e60001u); /* CP_SET_SECURE_MODE(0) */
        assert(peer.words[15][2] == 0);
        assert(peer.words[16][1] == 0x703d8003u); /* CP_MEM_WRITE */
        assert(peer.words[16][4] == 0xa750cafeu);
    }
    assert(*sim_reg(&peer, GGHS_CP_SQE_INSTR_BASE_LO) == 0x61796000u);
    assert(*sim_reg(&peer, GGHS_CP_AQE_INSTR_BASE_LO_0) == 0x617ac000u);
    assert(*sim_reg(&peer, GGHS_RBBM_SECVID_TRUST_CNTL) == 0);

    /* 5. Register-only path: no ZAP, so no CP_SET_SECURE_MODE is invented. */
    setup(0);
    assert(gghs_boot(&io, &state, &caps, &run) == GGHS_OK);
    assert(zap_calls == 0);
    assert(feature_set_secure(&peer) == 0);
    /* 6. A GMU error ACK must stop the boot and quarantine. */
    setup(1);
    peer.ack_error = 1;
    assert(gghs_features(&io, &state, &caps, 1000) == GGHS_ACK);
    assert(state.error == GGHS_ACK);
    setup(1);
    peer.acks_enabled = 0;
    assert(gghs_boot(&io, &state, &caps, &run) == GGHS_TIMEOUT);
    assert(state.phase == GGHS_PHASE_FAILED && state.quarantined);
    /* 7. A CP fault is never turned into progress. */
    setup(1);
    peer.fault[0] = 0x10;
    assert(gghs_boot(&io, &state, &caps, &run) == GGHS_FAULT);
    assert(state.quarantined && state.phase == GGHS_PHASE_FAILED);
    /* 8. A busy GPU that never idles fails on the bound. */
    setup(1);
    assert(gghs_cp_init(&io, &state, 0x61794000u, 1, 0) == GGHS_TIMEOUT);
    assert(state.idle_polls == 1);
    setup(1);
    peer.never_idle = 1;
    assert(gghs_cp_init(&io, &state, 0x61794000u, 1, 200) == GGHS_TIMEOUT);
    assert(state.idle_polls == 21);
    /* 9. A write the GPU never performed is a failure, not a success. */
    setup(1);
    peer.perform_mem_write = 0;
    assert(gghs_boot(&io, &state, &caps, &run) == GGHS_STATE);
    /* 10. The list-less form is refused outright. */
    setup(1);
    assert(gghs_cp_init(&io, &state, 0, 1, 200) == GGHS_ARGUMENT);
    /* 11. Timestamp fence: a cleared slot that only the GPU can fill. */
    setup(1);
    peer.mem[4] = 0;
    assert(gghs_timestamp(&io, &state, 0x10, 16, 200) == GGHS_OK);
    assert(state.cp_fence_value == 0x00c0ffeeu);
    setup(1);
    peer.perform_mem_write = 0;
    assert(gghs_timestamp(&io, &state, 0x10, 16, 50) == GGHS_TIMEOUT);
    /* 12. Argument and misuse rejection leaves state untouched. */
    setup(1);
    {
        struct gghs_state before = state;
        assert(gghs_boot(&io, 0, &caps, &run) == GGHS_ARGUMENT);
        run.restore_list = 0;
        assert(gghs_boot(&io, &state, &caps, &run) == GGHS_ARGUMENT);
        assert(!memcmp(&before, &state, sizeof(state)));
        assert(peer.sent == 0 && zap_calls == 0);
    }
    printf("HWSCHED: packet parity, 11-word CP_ME_INIT golden vector, vendor "
           "feature order (11 ACKs), async RAW without any ACK, DMS mask, ZAP "
           "gate, secure-mode exit, GPU write and fence, plus ACK/fault/busy/"
           "readback negatives passed\n");
    return 0;
}
