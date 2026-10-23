/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_MODEM_PLATFORM_H
#define GIULIA_MODEM_PLATFORM_H
#include "giulia_modem_contract.h"

#define GMP_ABI 1u
#define GMP_NATIVE 1u
/* Kernel-only provider contract. Registration pins the provider, callbacks and
 * referenced storage until reboot. submit copies an action and returns queue
 * acceptance, never a synthesized completion. poll returns 1 for one actual
 * event, 0 for none, or a negative error. Both run on the service worker outside
 * the core lock; callbacks never call core entrypoints directly. IRQ handlers
 * use their own bounded queue and publish events through poll.
 * The provider must use the common resource broker and prove each hardware
 * operation; membership in the software broker is not a hardware grant. */
struct gmp_platform {
    uint32_t abi,bytes,source;
    void *ctx;
    int (*submit)(void *,const struct gmr_action *);
    int (*poll)(void *,struct gmr_event *);
    /* Accept one already validated raw IP packet into the native interface.
     * Called outside the core lock. 0 means accepted by that interface. */
    int (*deliver_packet)(void *,const struct gmr_identity *,const uint8_t *,size_t);
};
#ifdef __cplusplus
extern "C" {
#endif
int giulia_modem_platform_register(const struct gmp_platform *);
int giulia_modem_submit(const struct gmr_command *);
int giulia_modem_get_snapshot(struct gmr_snapshot *);
int giulia_modem_terminal(const char *);
#ifdef __cplusplus
}
#endif
#endif
