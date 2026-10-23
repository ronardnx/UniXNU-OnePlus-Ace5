/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_GPU_GUNYAH_SHARE_H
#define GIULIA_GPU_GUNYAH_SHARE_H
#include "giulia_gpu_gunyah_memory.h"
#include "giulia_gpu_gunyah_queries.h"
#include "giulia_gpu_gunyah_rm.h"
#include "giulia_gpu_platform.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { GGSH_OK=0,GGSH_ARGUMENT=1,GGSH_BUSY=2,GGSH_LOST=3,
    GGSH_RPC=4,GGSH_SCM=5,GGSH_AUTHORITY=6,GGSH_CODEC=7 };
enum { GGSH_IDENTIFY=1,GGSH_PROBE=2,GGSH_ASSIGN=3,GGSH_SHARE=4,
    GGSH_LOOKUP=5,GGSH_RECHECK=6,GGSH_COMPLETE=7 };
struct ggsh_owner {
    void *cookie;
    /* Actual boot/carveout/control-memory and global SCM serialization lease,
     * including one exclusive RM transaction token across every RPC and SCM
     * callback. The actual owner holds it across the complete operation;
     * callbacks must not service another channel consumer. */
    int (*valid)(void *,uint64_t);
    /* Prove primary role, exclusive source ownership of the exact carveout,
     * no competing sharing driver, and authority for these dynamic VMIDs.
     * Called immediately before each assignment attempt; constants cannot
     * implement this proof. No memory is read or mapped by this callback. */
    int (*authorize)(void *,uint64_t,uint16_t,uint16_t,uint64_t,uint32_t);
    int (*publish)(void *,const void *,size_t); /* NC control visibility */
    struct ggrm_io rm;
    struct ggs_ops scm; /* Verified SCM64 transport; native call is supported. */
    void *control;
    uint64_t control_pa;
    uint32_t control_bytes;
};
/* Zero and pin this state, channel, descriptors, callbacks and NC control
 * before init. The channel is the existing sole owner's RM state: sequence
 * space is never reset or duplicated. All channel access stays serialized
 * by that owner, including between this transaction and a query session.
 * control is an independent 16 KiB allocation, outside the fixed carveout.
 * No queue/carveout access, allocation, default boot lease or registration
 * occurs here. No terminal can initiate or supply authority to this API. */
struct ggsh_state {
    unsigned initialized,busy,attempted,loss,invalid,retained;
    unsigned phase,assigned,shared,last_result,timeout_us,time_failed,timing,rpc_active;
    uint64_t epoch,start_us,last_us;
    uint32_t parcel,next_sequence;
    struct ggsh_owner owner;
    struct ggrm_state *channel;
    struct ggs_state scm_state;
    struct gg_rq_snapshot identity;
    uint8_t request[44];
};
int ggsh_init(struct ggsh_state *,const struct ggsh_owner *,struct ggrm_state *,uint64_t);
/* Identify -> SCM availability -> assign -> share -> exact lookup -> READY.
 * A single monotonic deadline covers the entire attempt. Any failed attempt
 * is quarantined; retain before an assignment may take effect. Never retry
 * the transaction, reclaim, unshare or free backing after ambiguity.
 * Success is only an ownership RPC transcript, not controller/CTX0 readiness,
 * peer acceptance, coherent CPU mapping or authorization to register fences. */
int ggsh_run(struct ggsh_state *,unsigned timeout_us);
int ggsh_invalidate(struct ggsh_state *);
#ifdef __cplusplus
}
#endif
#endif
