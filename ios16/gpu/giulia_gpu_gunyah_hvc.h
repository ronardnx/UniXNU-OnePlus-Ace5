/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_GPU_GUNYAH_HVC_H
#define GIULIA_GPU_GUNYAH_HVC_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define GGH_HVC_MAX_BYTES 240u
#define GGH_HVC_PUSH 1u
#define GGH_HVC_OK 0
#define GGH_HVC_AGAIN 1
#define GGH_HVC_ERROR (-1)

struct ggh_hvc_regs { uint64_t x[8]; };

enum { GGH_PROBE_OK=0,GGH_PROBE_ARGUMENT=1,GGH_PROBE_BUSY=2,
    GGH_PROBE_USED=3,GGH_PROBE_LEASE=4,GGH_PROBE_IDENTITY=5,
    GGH_PROBE_ABI=6,GGH_PROBE_UNAVAILABLE=7 };
struct ggh_hvc_compatibility {
    uint64_t epoch;
    struct ggh_hvc_regs uid,identify;
};
/* Separate boot-only permission to execute the two fixed discovery HVCs.
 * This live callback does NOT require already-known Gunyah compatibility:
 * the trusted boot owner instead proves that HVC discovery is permitted on
 * this boot and holds its probe lease for the entire operation. It returns
 * exactly 1. No DT property or terminal command supplies that permission.
 * Zero this state once, then set only epoch/lease_cookie/live before use.
 * The owner pins and serializes the state and callbacks through return.
 * Never reset/reuse it after success or failure; create a fresh state only
 * for a genuinely new owned boot epoch. The descriptor stays immutable.
 * Compatibility is not a capability lease, IRQ ownership or permission to
 * execute MSGQ, legacy doorbell, SCM assignment or any GPU operation. */
struct ggh_hvc_probe {
    uint64_t epoch;
    void *lease_cookie;
    int (*live)(void *,uint64_t);
    /* All eight full-width outputs survive rejected UID/identify results.
     * attempted counts actual completed HVCs (0..2). identify.x0 is API info,
     * not a status code. observed is diagnostic until complete is set. */
    struct ggh_hvc_compatibility observed;
    struct ggh_hvc_regs last;
    unsigned busy,attempted,complete,quarantined,last_result;
};
/* Canonical HVC #0: UID 0x8600ff01, then IDENTIFY 0xc6008000, at most once
 * each. Match the UID's low 32 bits as the pinned source does, preserving
 * upper bits in the snapshot. Require API1, little-endian, 64-bit ABI and
 * zero reserved API-info bits. Feature/variant values are preserved.
 * The output is aligned kernel memory outside the probe, unchanged on every
 * failure. A started failure quarantines this state; there is no retry or
 * automatic activation. These calls cannot preempt an HVC/callback. */
int ggh_hvc_probe_compatibility(struct ggh_hvc_probe *,struct ggh_hvc_compatibility *);

/* The owning kernel worker must hold the exclusive RM channel lease across
 * the complete operation. live() must prove that this epoch still permits
 * HVC, the verified canonical Gunyah SMCCC ABI and these actual capabilities;
 * a captured DT, terminal request or
 * static capability number is insufficient. Return exactly 1 when valid.
 * These fields must not change during a call. The callback checks the lease,
 * but does not replace the owner's serialization or IRQ ownership protocol.
 * No function in this adapter acquires or installs a lease automatically. */
struct ggh_hvc {
    uint64_t epoch,tx_cap,rx_cap;
    void *lease_cookie;
    int (*live)(void *,uint64_t,uint64_t,uint64_t);
    /* Diagnostics change even when a completed HVC result is rejected.
     * attempted is zero if this call did not reach HVC; last/native_code
     * retain the last actual HVC output, including all 64 status bits. */
    struct ggh_hvc_regs last;
    uint64_t native_code;
    unsigned attempted;
};

/* Fixed canonical SMCCC MSGQ operations only; see gpu-gunyah-source.json.
 * Return 0=accepted, 1=FULL/EMPTY, -1=invalid lease/arguments/other error.
 * ready=false after a successful send still means accepted: do not resend.
 * Receive stages data privately and only publishes bytes/size on success.
 * Each operation attempts HVC at most once; only FULL/EMPTY are safe for the
 * bounded RM owner to retry. In particular RETRY(-2) is not mapped to AGAIN.
 * These calls have no CPU-side timeout once HVC itself is entered. Callers
 * supply kernel-accessible buffers disjoint from the adapter and each other;
 * scalar output pointers must have their type's natural alignment.
 * Capability zero is permitted; the pinned source invalid value is U64_MAX. */
int ggh_hvc_valid(void *cookie,uint64_t epoch);
int ggh_hvc_send(void *cookie,const void *data,size_t size,unsigned flags);
int ggh_hvc_recv(void *cookie,void *data,size_t capacity,size_t *received);

/* The pinned fence path reads and clears doorbells using legacy hvc #0x6013.
 * This is a separate ABI from the canonical MSGQ above. Its independent live
 * callback must prove legacy ABI compatibility, the actual RX capability and
 * exclusive interrupt/doorbell ownership for the complete epoch. Successful
 * canonical RM setup alone does not establish any of those conditions.
 * A read-and-clean changes doorbell/IRQ state; use only in an owned worker.
 * No discovery, registration, interrupt binding or automatic ACK is provided. */
struct ggh_hvc_doorbell {
    uint64_t epoch,rx_cap;
    void *lease_cookie;
    int (*live)(void *,uint64_t,uint64_t);
    struct ggh_hvc_regs last;
    uint64_t native_code;
    unsigned attempted;
};
int ggh_hvc_doorbell_valid(void *cookie,uint64_t epoch);
/* Exactly one HVC at most. Return 0 only after status zero and a still-live
 * lease, publishing prior unmasked flags then. All failures return -1 and
 * leave *previous untouched; an attempted failure may already have cleared
 * bits and must not be retried without the owner's recovery protocol. */
int ggh_hvc_doorbell_read_clean(void *cookie,uint64_t clear_flags,uint64_t *previous);

#ifdef __cplusplus
}
#endif
#endif
