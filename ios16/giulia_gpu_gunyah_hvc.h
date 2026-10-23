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

/* The owning kernel worker must hold the exclusive RM channel lease across
 * the complete operation. live() must prove that this epoch still permits
 * HVC and owns these actual capabilities; a captured DT, terminal request or
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
 * These calls have no CPU-side timeout once HVC itself is entered. */
int ggh_hvc_send(void *cookie,const void *data,size_t size,unsigned flags);
int ggh_hvc_recv(void *cookie,void *data,size_t capacity,size_t *received);

#ifdef __cplusplus
}
#endif
#endif
