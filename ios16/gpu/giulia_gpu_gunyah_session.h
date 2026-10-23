/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * One read-only RM discovery attempt on a pinned, exclusively owned channel.
 * Results describe a query transcript, never a usable fence provider.
 */
#ifndef GIULIA_GPU_GUNYAH_SESSION_H
#define GIULIA_GPU_GUNYAH_SESSION_H
#include "giulia_gpu_gunyah_rm.h"
#include "giulia_gpu_gunyah_queries.h"
#include "giulia_gpu_gunyah_hvc.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { GGRS_OK=0,GGRS_ARGUMENT=1,GGRS_BUSY=2,GGRS_INVALID=3,
    GGRS_LEASE=4,GGRS_RPC=5,GGRS_CODEC=6,GGRS_IDLE=7,GGRS_BUDGET=8,
    GGRS_MAX_POLL_FRAMES=GGRM_POLLS };

struct ggrs_events {
    void *cookie;
    uint64_t (*now_us)(void *);
    int (*delay_us)(void *,unsigned);
    int (*notification)(void *,uint32_t,const void *,size_t);
};

/* Allocate and zero this object outside the kernel stack before init. The
 * owner pins it, all callbacks and the exclusive channel lease for the whole
 * epoch, including uncertain failures. Init copies the I/O descriptor. The
 * owner serializes RX delivery: no other receiver may consume this channel.
 * An actual RM owner must supply capabilities and ABI/IRQ compatibility;
 * neither DT constants nor a terminal can establish that ownership.
 */
struct ggrs_session {
    unsigned initialized,busy,attempted,invalid,last_result,loss_pending;
    unsigned timing,time_failed,timeout_us;
    unsigned poll_frames,poll_notifications;
    uint64_t epoch,start_us,last_us;
    struct ggrm_io owner;
    struct ggrm_state rm;
    struct gg_rq_snapshot snapshot;
    uint8_t request[44];
    struct ggh_hvc native_hvc;
    struct ggrs_events events;
    void *native_lease_cookie;
    int (*native_live)(void *,uint64_t,uint64_t,uint64_t);
};

int ggrs_init(struct ggrs_session *,const struct ggrm_io *,uint64_t epoch);
/* Canonical HVC adapter, copied together with event callbacks before publish.
 * This installs no interrupt or lease. The supplied live callback must prove
 * real channel ownership and running ABI compatibility on every operation.
 */
int ggrs_init_hvc(struct ggrs_session *,const struct ggh_hvc *,
    const struct ggrs_events *,uint64_t epoch);
/* Seven fixed query steps, one whole-attempt deadline, no mutation RPCs.
 * A failed attempt cannot be restarted, even after an explicit RM error.
 * Run on the owning kernel worker, never an interrupt or USB parser stack.
 */
int ggrs_probe(struct ggrs_session *,unsigned timeout_us);
/* Receive only, on the same owner worker after a successful probe. Each
 * call consumes at most frame_budget (1..MAX_POLL_FRAMES) frames and has one
 * monotonic deadline. IDLE means EMPTY was actually observed with no partial
 * notification; BUDGET means the bound was reached without observing EMPTY.
 * Neither result proves a drained remote lifecycle, READY, ownership or
 * completion. No request, sequence allocation or implicit ACK is performed.
 * Notifications retain exact seq/id/continuation count. An incomplete
 * assembly at EMPTY/budget expiry, an unexpected reply, malformed/unknown
 * message, transport loss or timeout quarantines this session. Partial
 * lifecycle events are never kept as apparently valid cached evidence.
 */
int ggrs_poll(struct ggrs_session *,unsigned frame_budget,unsigned timeout_us);
/* Deliver later notifications on the same serialized owner worker. Peer or
 * self lifecycle events invalidate the transcript, including a new READY.
 * Every notification is also delivered to the original owner's handler.
 */
int ggrs_notification(struct ggrs_session *,uint32_t,const void *,size_t);
/* Explicit discontinuity: RX loss, IRQ/lease loss, reconnect, or reset.
 * This may be called during a callback or from another owner context. Loss
 * is atomically latched even while busy. The active/next worker discards the
 * transcript; every subsequent validity check rejects the latched loss.
 * No shared state is reset by the caller that only latches loss. Returns
 * INVALID once loss is recorded. */
int ggrs_invalidate(struct ggrs_session *);
/* Copies a completed historical transcript while the local lease holds.
 * This does not receive pending events. The owner must keep polling/delivering
 * lifecycle events and treat IDLE as only a local observation, not an atomic
 * remote snapshot or a READY/ownership/coherence/completion proof. No result
 * here authorizes giulia_gpu_fence_register or any shared-memory access.
 */
int ggrs_copy_snapshot(struct ggrs_session *,struct gg_rq_snapshot *);

#ifdef __cplusplus
}
#endif
#endif
