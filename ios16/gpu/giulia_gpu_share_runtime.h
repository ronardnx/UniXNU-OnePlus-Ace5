/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * One primary share transaction, gated before it can reach the hypervisor.
 *
 * The transaction itself lives in `ggsh_run()`; this unit owns the ordering the
 * service must not get wrong:
 *
 *   1. the channel must still be the one this epoch owns (not busy, not
 *      quarantined, sequence space intact);
 *   2. the owner must still be live for the epoch (pin held, IRQ lease, no
 *      loss);
 *   3. the dynamic self/peer VMIDs must be known and distinct;
 *   4. primary role and exclusive carveout ownership must be **proved** by a
 *      real backend before any init happens. Without that proof the unit
 *      refuses, and no RM/SCM traffic is produced;
 *   5. only then is `ggsh_init()` called; `ggsr_finish()` runs the transaction
 *      and freezes the identity through `ggsn_take()`.
 *
 * The authority callback is the one place where the missing platform backend
 * plugs in. A stub that always returns 1 is not an implementation; it would
 * authorise a second sharing driver and a competing owner.
 */
#ifndef GIULIA_GPU_SHARE_RUNTIME_H
#define GIULIA_GPU_SHARE_RUNTIME_H
#include "giulia_gpu_gunyah_share.h"
#include "giulia_gpu_share_snapshot.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GGSR_OK=0, GGSR_ARGUMENT=1, GGSR_BUSY=2, GGSR_CHANNEL=3, GGSR_NOT_LIVE=4,
    GGSR_IDENTITY=5, GGSR_AUTHORITY=6, GGSR_INIT=7, GGSR_RUN=8, GGSR_SNAPSHOT=9
};

struct ggsr_gate {
    void *cookie;
    /* Owner still live for this epoch: pin, IRQ lease, no loss. */
    int (*live)(void *,uint64_t epoch);
    /* Dynamic identity this epoch resolved: both VMIDs 1..63 and distinct. */
    int (*identity)(void *,uint64_t epoch,uint16_t *self_vmid,uint16_t *peer_vmid);
    /* Primary role + exclusive source ownership of the fixed carveout + no
     * competing sharing driver. Only a real backend may answer 1. */
    int (*authority)(void *,uint64_t epoch,uint16_t self_vmid,uint16_t peer_vmid,
        uint64_t physical,uint32_t bytes);
};

struct ggsr_state {
    struct ggsh_state share;
    struct ggsn_state snapshot;
    struct ggsn_binding binding;
    uint16_t self_vmid,peer_vmid;
    unsigned prepared,finished,runs,refused,last;
    unsigned live_checks,identity_checks,authority_checks;
};

static inline const char *ggsr_code_name(unsigned code)
{
    const char *const names[]={"ok","argument","busy","channel","not-live",
        "identity","no-authority","init","run","snapshot"};
    return code<sizeof(names)/sizeof(names[0])?names[code]:"invalid";
}

static inline int ggsr_owner_ok(const struct ggsh_owner *owner)
{
    return owner && owner->valid && owner->authorize && owner->publish;
}

/* Prepare the transaction. Every refusal before step 5 leaves `share`
 * untouched, so a later attempt with real authority starts from zero. */
static inline int ggsr_prepare(struct ggsr_state *state,const struct ggsh_owner *owner,
    struct ggrm_state *channel,uint64_t epoch,const struct ggsr_gate *gate)
{
    uint16_t self_vmid=0,peer_vmid=0;
    int result;
    if (!state || !owner || !channel || !epoch || !gate ||
        !gate->live || !gate->identity || !gate->authority || !ggsr_owner_ok(owner))
        return GGSR_ARGUMENT;
    if (state->prepared || state->finished || state->runs) return GGSR_BUSY;
    if (channel->epoch!=epoch || channel->busy || channel->quarantined ||
        channel->next_sequence>65529u)
        return GGSR_CHANNEL;
    ++state->live_checks;
    if (gate->live(gate->cookie,epoch)!=1) {
        ++state->refused;
        state->last=GGSR_NOT_LIVE;
        return GGSR_NOT_LIVE;
    }
    ++state->identity_checks;
    if (gate->identity(gate->cookie,epoch,&self_vmid,&peer_vmid)!=1 ||
        self_vmid<1 || self_vmid>63 || peer_vmid<1 || peer_vmid>63 ||
        self_vmid==peer_vmid) {
        ++state->refused;
        state->last=GGSR_IDENTITY;
        return GGSR_IDENTITY;
    }
    ++state->authority_checks;
    if (gate->authority(gate->cookie,epoch,self_vmid,peer_vmid,
            GGF_CARVEOUT_PHYSICAL,GGF_CARVEOUT_BYTES)!=1) {
        ++state->refused;
        state->last=GGSR_AUTHORITY;
        return GGSR_AUTHORITY;
    }
    result=ggsh_init(&state->share,owner,channel,epoch);
    if (result) {
        ++state->refused;
        state->last=GGSR_INIT;
        return GGSR_INIT;
    }
    state->self_vmid=self_vmid;
    state->peer_vmid=peer_vmid;
    state->binding.snapshot=&state->snapshot;
    state->binding.live=&state->share;
    state->prepared=1;
    state->last=GGSR_OK;
    return GGSR_OK;
}

/* Run the prepared transaction and freeze its identity. The share unit latches
 * any ambiguity itself; this unit reports it and never retries. */
static inline int ggsr_finish(struct ggsr_state *state,unsigned timeout_us)
{
    int result;
    if (!state) return GGSR_ARGUMENT;
    if (!state->prepared) return GGSR_ARGUMENT;
    if (state->finished) return GGSR_BUSY;
    result=ggsh_run(&state->share,timeout_us);
    if (result) {
        state->last=GGSR_RUN;
        return GGSR_RUN;
    }
    result=ggsn_take(&state->snapshot,&state->share);
    if (result) {
        state->last=GGSR_SNAPSHOT;
        return GGSR_SNAPSHOT;
    }
    ++state->runs;
    state->finished=1;
    state->last=GGSR_OK;
    return GGSR_OK;
}
#ifdef __cplusplus
}
#endif
#endif
