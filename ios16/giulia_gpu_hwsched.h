/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_GPU_HWSCHED_H
#define GIULIA_GPU_HWSCHED_H
#include <stdint.h>
#include <stddef.h>
/* Gen7 hwsched bring-up engine for Adreno 750 (SM8650, gen7_9_1).
 *
 * Every constant and every ordering step below is taken from the pinned
 * OnePlus graphics-kernel revision recorded in hardware/sm8650-gpu.json:
 *   adreno_gen7.c                 gen7_cp_init_cmds()
 *   adreno_gen7_hfi.c             gen7_hfi_send_feature_ctrl(),
 *                                 gen7_hfi_send_bcl/ifpc/dms/acd/clx...,
 *                                 gen7_hfi_send_core_fw_start()
 *   adreno_gen7_hwsched_hfi.c     gen7_hwsched_hfi_start(),
 *                                 gen7_hwsched_coldboot_gpu(), cp_init(),
 *                                 send_switch_to_unsecure(), submit_raw_cmds()
 *   adreno_hfi.h                  message ids, feature ids, ack semantics
 *
 * Two facts in that source decide the whole design and are easy to get wrong:
 *
 *  1. CP initialization is an *asynchronous* H2F_MSG_ISSUE_CMD_RAW message.
 *     The GMU never answers it; stock completion is the 200 us poll of
 *     GEN7_GPU_GMU_AO_GPU_CX_BUSY_STATUS bit 23 plus the later fence. Any
 *     implementation that blocks for an HFI reply to message 131 can only
 *     time out, and must not treat that timeout as device progress.
 *
 *  2. CP_ME_INIT ordinal mask bit 11 means "DMS is NOT supported, disable it".
 *     gen7_9_1 declares ADRENO_DMS, so stock sends 0x14b. Sending 0x94b tells
 *     the CP the opposite of what this GPU supports.
 *
 * This file is pure C, allocation free and MMIO free: the caller supplies the
 * register window, the HFI publisher, the ACK waiter, the owned-RAM reader and
 * any secure-world action. Nothing here reports success it did not observe.
 */

/* GPU register word offsets in the vendor 0x3d00000 namespace. Byte address is
 * 0x3d00000 + 4*offset, exactly as kgsl_regwrite()/kgsl_regread() use them. */
enum {
    GGHS_CP_SQE_INSTR_BASE_LO = 0x830,
    GGHS_CP_SQE_INSTR_BASE_HI = 0x831,
    GGHS_CP_AQE_INSTR_BASE_LO_0 = 0xb70,
    GGHS_CP_AQE_INSTR_BASE_HI_0 = 0xb71,
    GGHS_CP_HW_FAULT = 0x821,
    GGHS_CP_INTERRUPT_STATUS = 0x823,
    GGHS_CP_PROTECT_STATUS = 0x824,
    GGHS_RBBM_STATUS = 0x210,
    GGHS_RBBM_STATUS3 = 0x213,
    GGHS_RBBM_INT_0_STATUS = 0x201,
    GGHS_RBBM_SECVID_TRUST_CNTL = 0xf400,
    GGHS_GMU_AO_GPU_CX_BUSY_STATUS = 0x23b0c,
    GGHS_GMU_AO_GPU_CX_BUSY_MASK = 0x23b0e
};

/* PM4 */
enum {
    GGHS_OP_CP_THREAD_CONTROL = 0x17,
    GGHS_OP_CP_ME_INIT = 0x48,
    GGHS_OP_CP_MEM_WRITE = 0x3d,
    GGHS_OP_CP_EVENT_WRITE = 0x46,
    GGHS_OP_CP_SET_SECURE_MODE = 0x66
};
enum { GGHS_CACHE_FLUSH_TS = 4 };

/* CP_ME_INIT ordinal mask bits used by gen7_cp_init_cmds(). */
enum {
    GGHS_MEINIT_MULTI_HW_CTX = 1u << 0,
    GGHS_MEINIT_ERROR_DETECT = 1u << 1,
    GGHS_MEINIT_DEFAULT_RESET = 1u << 3,
    GGHS_MEINIT_NO_PERFCTR_SAVE = 1u << 6,
    GGHS_MEINIT_REGISTER_INIT_LIST = 1u << 8,
    GGHS_MEINIT_DISABLE_DMS = 1u << 11
};
/* POWER_UP_REGISTER_LIST_LENGTH bit 31: the list carries, and CP must expect,
 * the dynamic triplet form. gen7_cp_init_cmds() always sets it. */
#define GGHS_MEINIT_DYNAMIC_LIST (1u << 31)
enum { GGHS_CP_HW_CONTEXTS = 3 };

/* HFI message ids (adreno_hfi.h enum hfi_msg_type). */
enum {
    GGHS_MSG_INIT = 0,
    GGHS_MSG_FW_VER = 1,
    GGHS_MSG_LM_CFG = 2,
    GGHS_MSG_BW_VOTE_TBL = 3,
    GGHS_MSG_PERF_TBL = 4,
    GGHS_MSG_START = 10,
    GGHS_MSG_FEATURE_CTRL = 11,
    GGHS_MSG_GET_VALUE = 12,
    GGHS_MSG_SET_VALUE = 13,
    GGHS_MSG_CORE_FW_START = 14,
    GGHS_MSG_TABLE = 15,
    GGHS_MSG_GX_BW_PERF_VOTE = 30,
    GGHS_MSG_ACK_F2H = 126,
    GGHS_MSG_REGISTER_CONTEXT = 128,
    GGHS_MSG_ISSUE_CMD = 130,
    GGHS_MSG_ISSUE_CMD_RAW = 131,
    GGHS_MSG_TS_NOTIFY = 132,
    GGHS_MSG_ISSUE_LPAC_CMD_RAW = 135
};

/* HFI feature ids (adreno_hfi.h). */
enum {
    GGHS_FEAT_DCVS = 0,
    GGHS_FEAT_HWSCHED = 1,
    GGHS_FEAT_PREEMPTION = 2,
    GGHS_FEAT_CLOCKS_ON = 3,
    GGHS_FEAT_IFPC = 9,
    GGHS_FEAT_BCL = 11,
    GGHS_FEAT_ACD = 12,
    GGHS_FEAT_KPROF = 16,
    GGHS_FEAT_BAIL_OUT_TIMER = 17,
    GGHS_FEAT_GMU_STATS = 18,
    GGHS_FEAT_CLX = 21,
    GGHS_FEAT_LSR = 23,
    GGHS_FEAT_LPAC = 24,
    GGHS_FEAT_HW_FENCE = 25,
    GGHS_FEAT_PERF_NORETAIN = 26,
    GGHS_FEAT_DMS = 27,
    GGHS_FEAT_AQE = 29
};

/* GMU return codes (adreno_hfi.h enum gmu_ret_type): an ACK is successful only
 * when the returned value is GMU_SUCCESS. */
enum { GGHS_GMU_SUCCESS = 0 };

enum { GGHS_OK = 0, GGHS_ARGUMENT, GGHS_IO, GGHS_TIMEOUT, GGHS_ACK, GGHS_FAULT,
       GGHS_ORDER, GGHS_BUSY, GGHS_QUARANTINED, GGHS_ABSENT, GGHS_STATE };

typedef void (*gghs_trace_fn)(void *cookie, const char *line);

struct gghs_io {
    void *cookie;
    /* Raw GPU register access, vendor word offsets, 32-bit only. */
    int (*read)(void *, uint32_t reg, uint32_t *value);
    int (*write)(void *, uint32_t reg, uint32_t value);
    int (*barrier)(void *);
    int (*wait_us)(void *, unsigned us);
    /* Publish one complete HFI message (header included) on the H2F command
     * queue and ring the host->GMU doorbell. Must publish the whole message or
     * fail; partial publication cannot be recovered from here. */
    int (*send)(void *, const uint32_t *words, unsigned nwords);
    /* Wait for the F2H ACK whose request header matches `header`. Returns 1 and
     * the ACK value/status when one arrived, 0 on timeout, negative on IO. */
    int (*wait_ack)(void *, uint32_t header, uint32_t *value, uint32_t *status,
                    unsigned timeout_us);
    /* Read one dword of owned, GPU-writable RAM by CPU offset. */
    int (*read_mem)(void *, uint32_t offset, uint32_t *value);
    int (*healthy)(void *); /* 1 only while no hardware fault is observed */
    gghs_trace_fn trace;
};

/* What this silicon supports. gen7_9_1 declares
 * ADRENO_APRIV|IOCOHERENT|AQE|CONTENT_PROTECTION|LPAC|IFPC|L3_VOTE|BCL|DMS|
 * HW_FENCE|PREEMPTION|ACD|GMU_WARMBOOT|CPU_PERF. */
struct gghs_caps {
    unsigned preemption, hw_fence, lpac, aqe, dms, acd, bcl, clx, ifpc;
    unsigned preempt_level;   /* ADRENO_PREEMPT_LEVEL, 1 for gen7_9_1 */
    unsigned usesgmem;        /* ADRENO_PREEMPT_GMEM, 0 unless requested */
    unsigned skipsaverestore;
    unsigned bcl_data;        /* gen7_9_1 bcl_data = 1 */
    unsigned ifpc_hyst;       /* from DT qcom,idle-timeout; 0 disables IFPC */
};

struct gghs_tables {
    const uint32_t *perf; unsigned perf_words;   /* H2F_MSG_TABLE */
    const uint32_t *bw;   unsigned bw_words;     /* H2F_MSG_BW_VOTE_TBL */
};

struct gghs_state {
    unsigned phase;          /* see GGHS_PHASE_* below */
    unsigned error;
    unsigned quarantined;
    unsigned sequence;       /* 12-bit HFI sequence */
    unsigned feature_acks;   /* acknowledged feature controls */
    unsigned messages;       /* successfully published HFI messages */
    unsigned context_queue;  /* HFI_VALUE_CONTEXT_QUEUE result */
    unsigned idle_polls;
    uint32_t cp_init_header;
    uint32_t cp_init_mask;
    uint32_t cp_fence_value; /* GPU-written timestamp of the first fence */
    uint32_t last_ack_value;
    uint32_t last_ack_status;
    uint32_t fault[4];       /* cp_hw_fault, cp_int, cp_protect, rbbm_int0 */
};

enum {
    GGHS_PHASE_IDLE = 0,
    GGHS_PHASE_FEATURES = 1,
    GGHS_PHASE_CORE_FW = 2,
    GGHS_PHASE_START = 3,
    GGHS_PHASE_UCODE = 4,
    GGHS_PHASE_CP_INIT = 5,
    GGHS_PHASE_ZAP = 6,
    GGHS_PHASE_UNSECURE = 7,
    GGHS_PHASE_EXEC = 8,
    GGHS_PHASE_READY = 9,
    GGHS_PHASE_FAILED = 10
};

static inline int gghs_ops_valid(const struct gghs_io *io)
{
    return io && io->read && io->write && io->barrier && io->wait_us &&
        io->send && io->wait_ack && io->read_mem && io->healthy;
}

static inline void gghs_trace(const struct gghs_io *io, const char *line)
{
    if (io->trace) io->trace(io->cookie, line);
}

/* Odd-parity bit as pm4_calc_odd_parity_bit(): the emitted bit is 1 when the
 * value already has an even number of set bits, so that value|bit is odd. */
static inline unsigned gghs_parity(uint32_t value)
{
    unsigned p = 0;
    for (unsigned i = 0; i < 32; i += 4) p ^= (value >> i) & 0xf;
    /* 4-bit parity fold */
    p ^= p >> 2; p ^= p >> 1;
    return ((p & 1u) ^ 1u);
}

/* cp_type7_packet(): CP_TYPE7_PKT | count | parity(count)<<15 |
 * opcode<<16 | parity(opcode)<<23. */
static inline int gghs_type7(uint32_t opcode, unsigned count, uint32_t *out)
{
    if (!out || opcode > 0x7f || count > 0x3fffu) return GGHS_ARGUMENT;
    *out = 0x70000000u | (uint32_t)(count & 0x3fffu) |
        ((uint32_t)gghs_parity(count & 0x3fffu) << 15) |
        ((opcode & 0x7fu) << 16) |
        ((uint32_t)gghs_parity(opcode & 0x7fu) << 23);
    return GGHS_OK;
}

/* CREATE_MSG_HDR(): sequence[31:20] | type[19:16] | size[15:8] | id[7:0].
 * `nwords` is the complete message length, header included (adreno_hfi.h
 * MSG_HDR_GET_SIZE returns exactly that and ggh_send enforces it). */
static inline int gghs_header(uint32_t id, unsigned nwords, unsigned sequence,
    unsigned type, uint32_t *out)
{
    if (!out || id > 0xff || !nwords || nwords > 0xff || sequence > 0xfff ||
        type > 0xf) return GGHS_ARGUMENT;
    *out = ((uint32_t)sequence << 20) | ((uint32_t)type << 16) |
        ((uint32_t)nwords << 8) | id;
    return GGHS_OK;
}

/* CP_ME_INIT ordinal mask, gen7_cp_init_cmds(). Bit 11 is inverted: it exists
 * only to tell CP that DMS is unsupported. */
static inline uint32_t gghs_me_init_mask(int dms_supported)
{
    uint32_t mask = GGHS_MEINIT_MULTI_HW_CTX | GGHS_MEINIT_ERROR_DETECT |
        GGHS_MEINIT_DEFAULT_RESET | GGHS_MEINIT_NO_PERFCTR_SAVE |
        GGHS_MEINIT_REGISTER_INIT_LIST;
    if (!dms_supported) mask |= GGHS_MEINIT_DISABLE_DMS;
    return mask;
}

/* Build the exact 11-word H2F_MSG_ISSUE_CMD_RAW message that stock sends for
 * CP initialization: header + CP_THREAD_CONTROL(CONCURRENT_BIN_DISABLE) +
 * CP_ME_INIT(7). `restore_list` may be 0, in which case the REGISTER_INIT_LIST
 * ordinal is cleared and no list address is embedded, matching the vendor's
 * optional-list form only when the caller also disabled the feature in CP. */
static inline int gghs_cp_init_message(unsigned sequence, uint32_t restore_list,
    int dms_supported, uint32_t *out, unsigned capacity, unsigned *nwords,
    struct gghs_state *state)
{
    uint32_t *w = out;
    uint32_t mask = gghs_me_init_mask(dms_supported);
    int e;
    if (!out || !nwords || capacity < 11) return GGHS_ARGUMENT;
    if (!restore_list) mask &= ~(uint32_t)GGHS_MEINIT_REGISTER_INIT_LIST;
    e = gghs_header(GGHS_MSG_ISSUE_CMD_RAW, 11, sequence, 0, &w[0]);
    if (e) return e;
    e = gghs_type7(GGHS_OP_CP_THREAD_CONTROL, 1, &w[1]); if (e) return e;
    w[2] = 1u << 27;                    /* disable concurrent binning */
    e = gghs_type7(GGHS_OP_CP_ME_INIT, 7, &w[3]); if (e) return e;
    w[4] = mask;                        /* enabled ordinal mask */
    w[5] = GGHS_CP_HW_CONTEXTS;         /* number of hardware contexts */
    w[6] = 0x20000000u;                 /* enable error detection */
    w[7] = 0x00000002u;                 /* operation mode mask */
    w[8] = restore_list;
    w[9] = 0;
    w[10] = GGHS_MEINIT_DYNAMIC_LIST;
    *nwords = 11;
    if (state) {
        state->cp_init_header = w[0];
        state->cp_init_mask = mask;
    }
    return GGHS_OK;
}

/* Publish an already-built message and advance the state sequence the same way
 * the host transport does (one sequence number per message). */
static inline int gghs_publish(const struct gghs_io *io, struct gghs_state *s,
    const uint32_t *words, unsigned nwords)
{
    int e;
    if (!gghs_ops_valid(io) || !s || !words || !nwords) return GGHS_ARGUMENT;
    if (s->quarantined) return GGHS_QUARANTINED;
    if (((words[0] >> 8) & 0xffu) != nwords) return GGHS_ARGUMENT;
    e = io->send(io->cookie, words, nwords);
    if (e) { s->error = GGHS_IO; return GGHS_IO; }
    s->messages++;
    return GGHS_OK;
}

/* Fetch the ACK for `header`. The HFI v5 status word, when the firmware sends
 * one, must be zero; the payload word is returned unfiltered because for
 * H2F_MSG_GET_VALUE it carries data, not an error code. */
static inline int gghs_ack_fetch(const struct gghs_io *io, struct gghs_state *s,
    uint32_t header, unsigned timeout_us, uint32_t *value, uint32_t *status)
{
    uint32_t v = 0, st = 0xffffffffu;
    int got;
    if (!gghs_ops_valid(io) || !s) return GGHS_ARGUMENT;
    got = io->wait_ack(io->cookie, header, &v, &st, timeout_us);
    if (got < 0) { s->error = GGHS_IO; return GGHS_IO; }
    if (!got) { s->error = GGHS_TIMEOUT; return GGHS_TIMEOUT; }
    s->last_ack_value = v;
    s->last_ack_status = st;
    if (st != 0xffffffffu && st != 0) { s->error = GGHS_ACK; return GGHS_ACK; }
    if (value) *value = v;
    if (status) *status = st;
    return GGHS_OK;
}

/* ACK of a generic request: gen7_hfi_send_generic_req() treats a non-zero
 * results[2] as an HFI ACK failure. */
static inline int gghs_ack(const struct gghs_io *io, struct gghs_state *s,
    uint32_t header, unsigned timeout_us)
{
    uint32_t value = 0;
    int e = gghs_ack_fetch(io, s, header, timeout_us, &value, 0);
    if (e) return e;
    if (value != GGHS_GMU_SUCCESS) { s->error = GGHS_ACK; return GGHS_ACK; }
    return GGHS_OK;
}

/* H2F_MSG_FEATURE_CTRL {hdr, feature, enable, data} with its ACK. */
static inline int gghs_feature(const struct gghs_io *io, struct gghs_state *s,
    uint32_t feature, uint32_t enable, uint32_t data, unsigned timeout_us)
{
    uint32_t w[4];
    int e;
    if (feature > 0xff || enable > 1) return GGHS_ARGUMENT;
    e = gghs_header(GGHS_MSG_FEATURE_CTRL, 4, ++s->sequence & 0xfff,
        0, &w[0]);
    if (e) return e;
    w[1] = feature; w[2] = enable; w[3] = data;
    e = gghs_publish(io, s, w, 4);
    if (e) return e;
    e = gghs_ack(io, s, w[0], timeout_us);
    if (e) return e;
    s->feature_acks++;
    return GGHS_OK;
}

/* H2F_MSG_GET_VALUE {hdr, type, subtype} -> the ACK payload word is the value. */
static inline int gghs_get_value(const struct gghs_io *io, struct gghs_state *s,
    uint32_t type, uint32_t subtype, unsigned timeout_us, uint32_t *result)
{
    uint32_t w[3], value = 0;
    int e;
    e = gghs_header(GGHS_MSG_GET_VALUE, 3, ++s->sequence & 0xfff, 0, &w[0]);
    if (e) return e;
    w[1] = type; w[2] = subtype;
    e = gghs_publish(io, s, w, 3);
    if (e) return e;
    e = gghs_ack_fetch(io, s, w[0], timeout_us, &value, 0);
    if (e) return e;
    if (result) *result = value;
    return GGHS_OK;
}

/* H2F_MSG_TABLE / BW_VOTE_TBL: a caller-built payload with the header the
 * caller already put in word 0 (stock builds it that way). */
static inline int gghs_table(const struct gghs_io *io, struct gghs_state *s,
    uint32_t id, const uint32_t *words, unsigned nwords, unsigned timeout_us)
{
    uint32_t buf[200];
    int e;
    if (!words || !nwords || nwords > sizeof(buf)/sizeof(buf[0])) return GGHS_ARGUMENT;
    if ((words[0] & 0xffu) != id) return GGHS_ARGUMENT;
    for (unsigned i = 0; i < nwords; ++i) buf[i] = words[i];
    if (((buf[0] >> 8) & 0xffu) != nwords) {
        e = gghs_header(id, nwords, ++s->sequence & 0xfff, 0, &buf[0]);
        if (e) return e;
    }
    e = gghs_publish(io, s, buf, nwords);
    if (e) return e;
    return gghs_ack(io, s, buf[0], timeout_us);
}

/* Vendor feature order of gen7_hwsched_hfi_start(), with the optional entries
 * gated by capability. Tables are sent by the caller before this call. */
static inline int gghs_features(const struct gghs_io *io, struct gghs_state *s,
    const struct gghs_caps *caps, unsigned timeout_us)
{
    int e;
    if (!caps) return GGHS_ARGUMENT;
    s->phase = GGHS_PHASE_FEATURES;
    if (caps->acd) {
        if ((e = gghs_feature(io, s, GGHS_FEAT_ACD, 1, 0, timeout_us))) return e;
    }
    if (caps->bcl) {
        /* BIT[0] response type, BIT[7:1]/[14:8]/[21:15] throttle levels. */
        if ((e = gghs_feature(io, s, GGHS_FEAT_BCL, 1, caps->bcl_data, timeout_us))) return e;
    }
    if (caps->clx) {
        if ((e = gghs_feature(io, s, GGHS_FEAT_CLX, 1, 0, timeout_us))) return e;
    }
    if (caps->ifpc && caps->ifpc_hyst) {
        if ((e = gghs_feature(io, s, GGHS_FEAT_IFPC, 1, caps->ifpc_hyst, timeout_us))) return e;
    }
    if ((e = gghs_feature(io, s, GGHS_FEAT_HWSCHED, 1, 0, timeout_us))) return e;
    if ((e = gghs_feature(io, s, GGHS_FEAT_KPROF, 1, 0, timeout_us))) return e;
    if (caps->dms) {
        if ((e = gghs_feature(io, s, GGHS_FEAT_DMS, 1, 0, timeout_us))) return e;
    }
    /* Perfcounter retention is off in GMU firmware; say so explicitly. */
    if ((e = gghs_feature(io, s, GGHS_FEAT_PERF_NORETAIN, 0, 0, timeout_us))) return e;
    if ((e = gghs_feature(io, s, GGHS_FEAT_BAIL_OUT_TIMER, 1, 0, timeout_us))) return e;
    return GGHS_OK;
}

/* H2F_MSG_CORE_FW_START {hdr, handle=0} then the context-queue query that is
 * only valid afterwards. */
static inline int gghs_core_fw_start(const struct gghs_io *io, struct gghs_state *s,
    unsigned timeout_us)
{
    uint32_t w[2];
    int e;
    s->phase = GGHS_PHASE_CORE_FW;
    e = gghs_header(GGHS_MSG_CORE_FW_START, 2, ++s->sequence & 0xfff, 0, &w[0]);
    if (e) return e;
    w[1] = 0;
    e = gghs_publish(io, s, w, 2);
    if (e) return e;
    if ((e = gghs_ack(io, s, w[0], timeout_us))) return e;
    /* HFI_VALUE_CONTEXT_QUEUE = 121; the payload word is the capability flag. */
    {
        uint32_t queue = 0;
        if ((e = gghs_get_value(io, s, 121, 0, timeout_us, &queue))) return e;
        s->context_queue = queue ? 1u : 0u;
    }
    return GGHS_OK;
}

/* The feature block that stock sends after the context-queue query and before
 * START: hw fence, preemption (enable), LPAC, AQE. */
static inline int gghs_post_core_features(const struct gghs_io *io,
    struct gghs_state *s, const struct gghs_caps *caps, unsigned timeout_us)
{
    int e;
    if (caps->hw_fence) {
        if ((e = gghs_feature(io, s, GGHS_FEAT_HW_FENCE, 1, 0, timeout_us))) return e;
    }
    if (caps->preemption) {
        uint32_t data = (caps->preempt_level & 3u) |
            ((caps->usesgmem ? 1u : 0u) << 2) |
            ((caps->skipsaverestore ? 1u : 0u) << 3);
        if ((e = gghs_feature(io, s, GGHS_FEAT_PREEMPTION, 1, data, timeout_us))) return e;
    }
    if (caps->lpac) {
        if ((e = gghs_feature(io, s, GGHS_FEAT_LPAC, 1, 0, timeout_us))) return e;
    }
    if (caps->aqe) {
        if ((e = gghs_feature(io, s, GGHS_FEAT_AQE, 1, 0, timeout_us))) return e;
    }
    return GGHS_OK;
}

/* H2F_MSG_START {hdr}. The ACK proves the GMU accepted START, not that the CP
 * or the GPU executed anything. */
static inline int gghs_start(const struct gghs_io *io, struct gghs_state *s,
    unsigned timeout_us)
{
    uint32_t w[1];
    int e;
    s->phase = GGHS_PHASE_START;
    if ((e = gghs_header(GGHS_MSG_START, 1, ++s->sequence & 0xfff, 0, &w[0])))
        return e;
    if ((e = gghs_publish(io, s, w, 1))) return e;
    return gghs_ack(io, s, w[0], timeout_us);
}

/* Program the SQE/AQE microcode bases and verify the window accepts them. The
 * caller has already copied the digest-checked payloads into GPU-readable RAM. */
static inline int gghs_ucode(const struct gghs_io *io, struct gghs_state *s,
    uint64_t sqe_iova, uint64_t aqe_iova)
{
    uint32_t value = 0;
    s->phase = GGHS_PHASE_UCODE;
    if (io->write(io->cookie, GGHS_CP_SQE_INSTR_BASE_LO, (uint32_t)sqe_iova)) return GGHS_IO;
    if (io->write(io->cookie, GGHS_CP_SQE_INSTR_BASE_HI, (uint32_t)(sqe_iova >> 32))) return GGHS_IO;
    if (aqe_iova) {
        if (io->write(io->cookie, GGHS_CP_AQE_INSTR_BASE_LO_0, (uint32_t)aqe_iova)) return GGHS_IO;
        if (io->write(io->cookie, GGHS_CP_AQE_INSTR_BASE_HI_0, (uint32_t)(aqe_iova >> 32))) return GGHS_IO;
    }
    if (io->barrier(io->cookie)) return GGHS_IO;
    if (io->read(io->cookie, GGHS_CP_SQE_INSTR_BASE_LO, &value)) return GGHS_IO;
    if (value != (uint32_t)sqe_iova) { s->error = GGHS_STATE; return GGHS_STATE; }
    if (aqe_iova) {
        if (io->read(io->cookie, GGHS_CP_AQE_INSTR_BASE_LO_0, &value)) return GGHS_IO;
        if (value != (uint32_t)aqe_iova) { s->error = GGHS_STATE; return GGHS_STATE; }
    }
    return GGHS_OK;
}

/* GEN7_GPU_GMU_AO_GPU_CX_BUSY_STATUS bit 23: set while the GPU is executing. */
static inline int gghs_gpu_busy(const struct gghs_io *io, int *busy)
{
    uint32_t value = 0;
    if (!busy) return GGHS_ARGUMENT;
    if (io->read(io->cookie, GGHS_GMU_AO_GPU_CX_BUSY_STATUS, &value)) return GGHS_IO;
    *busy = (value >> 23) & 1;
    return GGHS_OK;
}

/* submit_raw_cmds(): publish, then poll the busy bit until it clears. The stock
 * bound is 200 us; a longer bound is only a diagnostic. */
static inline int gghs_wait_idle(const struct gghs_io *io, struct gghs_state *s,
    unsigned timeout_us)
{
    unsigned waited = 0;
    int e, busy = 0;
    for (;;) {
        if (io->healthy(io->cookie) != 1) { s->error = GGHS_FAULT; return GGHS_FAULT; }
        if ((e = gghs_gpu_busy(io, &busy))) { s->error = (unsigned)e; return e; }
        s->idle_polls++;
        if (!busy) return GGHS_OK;
        if (waited >= timeout_us) { s->error = GGHS_TIMEOUT; return GGHS_TIMEOUT; }
        if (io->wait_us(io->cookie, 10)) { s->error = GGHS_IO; return GGHS_IO; }
        waited += 10;
    }
}

/* Capture the four CP fault words. Reading RBBM_INT_0_STATUS needs the GX clock
 * that the caller holds; the CP_* words are always reachable. */
static inline int gghs_capture_fault(const struct gghs_io *io, struct gghs_state *s)
{
    uint32_t *f = s->fault;
    if (io->read(io->cookie, GGHS_CP_HW_FAULT, &f[0])) return GGHS_IO;
    if (io->read(io->cookie, GGHS_CP_INTERRUPT_STATUS, &f[1])) return GGHS_IO;
    if (io->read(io->cookie, GGHS_CP_PROTECT_STATUS, &f[2])) return GGHS_IO;
    if (io->read(io->cookie, GGHS_RBBM_INT_0_STATUS, &f[3])) return GGHS_IO;
    return GGHS_OK;
}

/* No CP fault bit was observed. CP_HW_FAULT carries an error code in [7:0] and
 * a valid bit; CP_PROTECT_STATUS carries an access-protect violation. */
static inline int gghs_fault_free(const struct gghs_state *s)
{
    return !s->fault[0] && !s->fault[1] && !s->fault[2];
}

/* CP_THREAD_CONTROL + CP_ME_INIT through the raw channel, then the vendor
 * completion test and a fault check. Never blocks on an ACK. */
static inline int gghs_cp_init(const struct gghs_io *io, struct gghs_state *s,
    uint32_t restore_list, int dms_supported, unsigned idle_timeout_us)
{
    uint32_t w[11];
    unsigned n = 0;
    int e;
    s->phase = GGHS_PHASE_CP_INIT;
    if (!restore_list) return GGHS_ARGUMENT; /* list-less form is not stock */
    if ((e = gghs_cp_init_message(++s->sequence & 0xfff, restore_list,
            dms_supported, w, 11, &n, s))) return e;
    if ((e = gghs_publish(io, s, w, n))) return e;
    if ((e = gghs_wait_idle(io, s, idle_timeout_us))) return e;
    if ((e = gghs_capture_fault(io, s))) return e;
    if (!gghs_fault_free(s)) { s->error = GGHS_FAULT; return GGHS_FAULT; }
    return GGHS_OK;
}

/* CP_SET_SECURE_MODE(0) through the raw channel, then verify that the GPU left
 * secure mode. Stock only sends this when ZAP was loaded; otherwise it writes
 * RBBM_SECVID_TRUST_CNTL directly. */
static inline int gghs_switch_unsecure(const struct gghs_io *io, struct gghs_state *s,
    int zap_loaded, unsigned idle_timeout_us)
{
    uint32_t w[3], value = 0;
    int e;
    s->phase = GGHS_PHASE_UNSECURE;
    if (zap_loaded) {
        if ((e = gghs_header(GGHS_MSG_ISSUE_CMD_RAW, 3, ++s->sequence & 0xfff, 0, &w[0])))
            return e;
        if ((e = gghs_type7(GGHS_OP_CP_SET_SECURE_MODE, 1, &w[1]))) return e;
        w[2] = 0;
        if ((e = gghs_publish(io, s, w, 3))) return e;
        if ((e = gghs_wait_idle(io, s, idle_timeout_us))) return e;
    } else if (io->write(io->cookie, GGHS_RBBM_SECVID_TRUST_CNTL, 0)) {
        return GGHS_IO;
    }
    if (io->barrier(io->cookie)) return GGHS_IO;
    if (io->read(io->cookie, GGHS_RBBM_SECVID_TRUST_CNTL, &value)) return GGHS_IO;
    if (value != 0) { s->error = GGHS_STATE; return GGHS_STATE; }
    return GGHS_OK;
}

/* CP_MEM_WRITE {addr_lo, addr_hi, value}: the first command whose effect the
 * host can prove. The caller must pre-clear the target so a matching value can
 * only come from the GPU. */
static inline int gghs_mem_write(const struct gghs_io *io, struct gghs_state *s,
    uint64_t gpu_address, uint32_t value, uint32_t offset, uint32_t *observed,
    unsigned idle_timeout_us)
{
    /* CP_MEM_WRITE carries three payload dwords {addr_lo, addr_hi, value}, so
     * the message is header + packet header + 3 dwords = 5 words. */
    uint32_t w[5], got = 0;
    int e;
    s->phase = GGHS_PHASE_EXEC;
    if ((e = gghs_header(GGHS_MSG_ISSUE_CMD_RAW, 5, ++s->sequence & 0xfff, 0, &w[0])))
        return e;
    if ((e = gghs_type7(GGHS_OP_CP_MEM_WRITE, 3, &w[1]))) return e;
    w[2] = (uint32_t)gpu_address;
    w[3] = (uint32_t)(gpu_address >> 32);
    w[4] = value;
    if ((e = gghs_publish(io, s, w, 5))) return e;
    if ((e = gghs_wait_idle(io, s, idle_timeout_us))) return e;
    if ((e = gghs_capture_fault(io, s))) return e;
    if (!gghs_fault_free(s)) { s->error = GGHS_FAULT; return GGHS_FAULT; }
    if (io->read_mem(io->cookie, offset, &got)) return GGHS_IO;
    if (observed) *observed = got;
    if (got != value) { s->error = GGHS_STATE; return GGHS_STATE; }
    return GGHS_OK;
}

/* CP_EVENT_WRITE(CACHE_FLUSH_TS) into an owned timestamp slot. The caller
 * clears the slot first, so a non-zero read is a GPU write. This is the
 * completion primitive a real fence backend can rely on. */
static inline int gghs_timestamp(const struct gghs_io *io, struct gghs_state *s,
    uint64_t gpu_address, uint32_t offset, unsigned timeout_us)
{
    uint32_t w[5], got = 0;
    unsigned waited = 0;
    int e;
    s->phase = GGHS_PHASE_EXEC;
    if ((e = gghs_header(GGHS_MSG_ISSUE_CMD_RAW, 5, ++s->sequence & 0xfff, 0, &w[0])))
        return e;
    if ((e = gghs_type7(GGHS_OP_CP_EVENT_WRITE, 3, &w[1]))) return e;
    w[2] = GGHS_CACHE_FLUSH_TS;
    w[3] = (uint32_t)gpu_address;
    w[4] = (uint32_t)(gpu_address >> 32);
    if ((e = gghs_publish(io, s, w, 5))) return e;
    for (;;) {
        if (io->read_mem(io->cookie, offset, &got)) return GGHS_IO;
        if (got) { s->cp_fence_value = got; return GGHS_OK; }
        if (waited >= timeout_us) { s->error = GGHS_TIMEOUT; return GGHS_TIMEOUT; }
        if (io->wait_us(io->cookie, 10)) return GGHS_IO;
        waited += 10;
    }
}

/* The whole cold path in stock order. `zap` may be NULL for a register-only
 * diagnostic; then secure mode is left the way firmware left it and the caller
 * must not claim a rendering path. */
struct gghs_run {
    uint64_t sqe_iova;
    uint64_t aqe_iova;      /* 0 when AQE is not loaded */
    uint32_t restore_list;  /* GPU address of the power-up register list */
    uint32_t test_gpu_addr_lo;
    uint32_t test_gpu_addr_hi;
    uint32_t test_offset;
    uint32_t test_value;
    unsigned ack_timeout_us;
    unsigned idle_timeout_us;
    int (*zap)(void *);
};

static inline int gghs_boot(const struct gghs_io *io, struct gghs_state *s,
    const struct gghs_caps *caps, const struct gghs_run *run)
{
    int e;
    if (!gghs_ops_valid(io) || !s || !caps || !run) return GGHS_ARGUMENT;
    if (s->quarantined) return GGHS_QUARANTINED;
    if (!run->restore_list) return GGHS_ARGUMENT;
    if (!run->test_value) return GGHS_ARGUMENT;
    if ((e = gghs_features(io, s, caps, run->ack_timeout_us))) goto fail;
    if ((e = gghs_core_fw_start(io, s, run->ack_timeout_us))) goto fail;
    if ((e = gghs_post_core_features(io, s, caps, run->ack_timeout_us))) goto fail;
    if ((e = gghs_start(io, s, run->ack_timeout_us))) goto fail;
    if ((e = gghs_ucode(io, s, run->sqe_iova, run->aqe_iova))) goto fail;
    if ((e = gghs_cp_init(io, s, run->restore_list, (int)caps->dms,
            run->idle_timeout_us))) goto fail;
    if (run->zap) {
        s->phase = GGHS_PHASE_ZAP;
        e = run->zap(io->cookie);
        if (e) { s->error = (unsigned)e; goto fail; }
        if ((e = gghs_switch_unsecure(io, s, 1, run->idle_timeout_us))) goto fail;
    }
    if ((e = gghs_mem_write(io, s,
            ((uint64_t)run->test_gpu_addr_hi << 32) | run->test_gpu_addr_lo,
            run->test_value, run->test_offset, NULL, run->idle_timeout_us))) goto fail;
    s->phase = GGHS_PHASE_READY;
    return GGHS_OK;
fail:
    s->phase = GGHS_PHASE_FAILED;
    s->quarantined = 1;
    return e;
}
#endif
