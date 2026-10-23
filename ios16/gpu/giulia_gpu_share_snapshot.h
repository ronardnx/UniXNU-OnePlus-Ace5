/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Frozen identity of a completed primary share transaction.
 *
 * `ggsh_run()` leaves the authoritative result in its own state: the epoch, the
 * dynamic self/peer VMIDs from the query transcript, the parcel handle and the
 * completion phase. The fence import path needs that identity on every call, so
 * this unit freezes it once and re-checks it against the live state before
 * answering. It reads recorded fields only: no HVC, no SMC, no carveout access.
 *
 * Rules:
 *   - a snapshot can be taken once, and only from a state that completed the
 *     whole identify -> assign -> share -> lookup -> recheck sequence with an
 *     unquarantined channel and an intact identity snapshot;
 *   - every `ggsn_share()` re-validates the live state and compares it with the
 *     frozen copy; any divergence latches invalidation, and a latched snapshot
 *     never answers again;
 *   - the frozen copy is never updated, so later mutation of the live state can
 *     only invalidate it, not rewrite it;
 *   - the carveout range is the fixed fence carveout, not a value derived from
 *     the transaction, and holding this snapshot is not ownership: it is one
 *     input to `ggfn_import()`.
 */
#ifndef GIULIA_GPU_SHARE_SNAPSHOT_H
#define GIULIA_GPU_SHARE_SNAPSHOT_H
#include "giulia_gpu_gunyah_share.h"
#include "giulia_gpu_fence_native.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GGSN_OK=0, GGSN_ARGUMENT=1, GGSN_TAKEN=2, GGSN_NOT_COMPLETE=3,
    GGSN_IDENTITY=4, GGSN_PARCEL=5, GGSN_CHANNEL=6, GGSN_STALE=7
};

enum {
    GGSN_PROOF_EPOCH=1, GGSN_PROOF_IDENTITY=2, GGSN_PROOF_PARCEL=4,
    GGSN_PROOF_COMPLETE=8, GGSN_PROOF_CHANNEL=16, GGSN_PROOF_REQUIRED=31
};

struct ggsn_snapshot {
    uint64_t epoch;
    uint32_t parcel,self_vmid,peer_vmid,sequence;
    uint64_t carveout_physical,carveout_bytes;
    uint32_t proofs;
};

struct ggsn_state {
    struct ggsn_snapshot frozen;
    unsigned taken,invalidated,refused,stale;
};

/* One binding for `ggfn_inputs.share`: the frozen snapshot plus the live state
 * it must still agree with. Both are owned by the caller and live to reboot. */
struct ggsn_binding {
    struct ggsn_state *snapshot;
    struct ggsh_state *live;
};

/* Read-only: true only for a completed, unquarantined, identity-intact share. */
static inline int ggsn_live_ok(const struct ggsh_state *live)
{
    if (!live || !live->initialized || !live->attempted) return 0;
    if (live->invalid || live->loss || live->busy) return 0;
    if (live->phase!=GGSH_COMPLETE || !live->shared || !live->assigned) return 0;
    if (!live->epoch || !live->channel) return 0;
    return 1;
}

static inline int ggsn_channel_ok(const struct ggsh_state *live)
{
    if (!live || !live->channel) return 0;
    if (live->channel->epoch!=live->epoch || live->channel->quarantined) return 0;
    return 1;
}

static inline int ggsn_identity_ok(const struct ggsh_state *live)
{
    if (!live) return 0;
    if (live->identity.epoch!=live->epoch || live->identity.invalid) return 0;
    if (live->identity.self_vmid<1 || live->identity.self_vmid>63) return 0;
    if (live->identity.peer_vmid<1 || live->identity.peer_vmid>63) return 0;
    if (live->identity.self_vmid==live->identity.peer_vmid) return 0;
    return 1;
}

/* Freeze the identity of a completed transaction. Refusal leaves the snapshot
 * empty and the state reusable only in the sense that nothing was taken. */
static inline int ggsn_take(struct ggsn_state *state,const struct ggsh_state *live)
{
    struct ggsn_snapshot frozen;
    if (!state || !live) return GGSN_ARGUMENT;
    if (state->taken) return GGSN_TAKEN;
    if (state->invalidated) return GGSN_STALE;
    if (!ggsn_live_ok(live)) return GGSN_NOT_COMPLETE;
    if (!ggsn_channel_ok(live)) return GGSN_CHANNEL;
    if (!ggsn_identity_ok(live)) return GGSN_IDENTITY;
    if (!live->parcel) return GGSN_PARCEL;
    frozen.epoch=live->epoch;
    frozen.parcel=live->parcel;
    frozen.self_vmid=live->identity.self_vmid;
    frozen.peer_vmid=live->identity.peer_vmid;
    frozen.sequence=live->channel->next_sequence;
    frozen.carveout_physical=GGF_CARVEOUT_PHYSICAL;
    frozen.carveout_bytes=GGF_CARVEOUT_BYTES;
    frozen.proofs=GGSN_PROOF_REQUIRED;
    state->frozen=frozen;
    state->taken=1;
    return GGSN_OK;
}

/* Answer the fence import input, re-checking the live state against the frozen
 * identity. A mismatch is permanent: the snapshot latches and never answers
 * again, because the transaction it described no longer exists. */
static inline int ggsn_share(struct ggsn_state *state,const struct ggsh_state *live,
    struct ggfn_share *out)
{
    const struct ggsn_snapshot *frozen;
    if (!state || !live || !out) return GGSN_ARGUMENT;
    if (!state->taken) return GGSN_ARGUMENT;
    if (state->invalidated) return GGSN_STALE;
    frozen=&state->frozen;
    if (!ggsn_live_ok(live) || !ggsn_channel_ok(live) || !ggsn_identity_ok(live) ||
       live->epoch!=frozen->epoch || live->parcel!=frozen->parcel ||
       live->identity.self_vmid!=frozen->self_vmid ||
       live->identity.peer_vmid!=frozen->peer_vmid) {
        state->invalidated=1;
        state->stale=1;
        ++state->refused;
        return GGSN_STALE;
    }
    out->epoch=frozen->epoch;
    out->parcel=frozen->parcel;
    out->self_vmid=frozen->self_vmid;
    out->peer_vmid=frozen->peer_vmid;
    out->complete=1;
    out->invalid=0;
    return GGSN_OK;
}

static inline int ggsn_share_cb(void *cookie,struct ggfn_share *out)
{
    struct ggsn_binding *binding=(struct ggsn_binding *)cookie;
    if (!binding) return GGSN_ARGUMENT;
    return ggsn_share(binding->snapshot,binding->live,out);
}

/* Permanent latch: epoch loss, teardown or any caller-observed divergence. */
static inline int ggsn_invalidate(struct ggsn_state *state)
{
    if (!state) return GGSN_ARGUMENT;
    state->invalidated=1;
    return GGSN_OK;
}

static inline const char *ggsn_code_name(unsigned code)
{
    const char *const names[]={"ok","argument","taken","not-complete","identity",
        "parcel","channel","stale"};
    return code<sizeof(names)/sizeof(names[0])?names[code]:"invalid";
}
#ifdef __cplusplus
}
#endif
#endif
