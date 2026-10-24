/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_MODEM_PLATFORM_H
#define GIULIA_MODEM_PLATFORM_H
#include "giulia_modem_contract.h"

#define GMP_ABI 1u
#define GMP_NATIVE 1u
/* Kernel-only provider contract. Registration pins the provider, callbacks and
 * referenced storage until reboot. submit copies an action and returns queue
 * acceptance, never a synthesized completion. submit returns GMR_OK or a
 * positive gmr_result rejection. A valid rejection means no work was accepted
 * and no hardware effects were issued; negative/out-of-range results violate
 * this contract. A failure after issuance must return acceptance first, then
 * publish an actual failure event through poll.
 * poll returns 1 for one actual event, 0 for none, or a negative error.
 * Both run on the service worker outside the core lock; callbacks never call
 * core entrypoints directly. IRQ handlers
 * use their own bounded queue and publish events through poll.
 * The provider must use the common resource broker and prove each hardware
 * operation; membership in the software broker is not a hardware grant.
 * DATA_REMOVE and global DATA_DRAIN must confirm all GMR_REMOVE_COMPLETE bits:
 * network withdrawn, RX drained, TX drained and physical DMA stopped. IPC_STOP
 * closes business transport only; retain the SMP2P/sysmon/SMEM IRQ and stop-ack
 * dependencies until FIRMWARE_STOP has completed PAS4/38 shutdown. */
struct gmp_platform {
    uint32_t abi,bytes,source;
    void *ctx;
    int (*submit)(void *,const struct gmr_action *);
    int (*poll)(void *,struct gmr_event *);
    /* Accept one already validated raw IP packet into the native interface.
     * Called outside the core lock. 0 means accepted by that interface. */
    int (*deliver_packet)(void *,const struct gmr_identity *,const uint8_t *,size_t);
};
enum gmp_control_operation { GMP_CONTROL_START=1, GMP_CONTROL_STOP,
    GMP_CONTROL_SUSPEND, GMP_CONTROL_RESUME };
#ifdef __cplusplus
extern "C" {
#endif
int giulia_modem_platform_register(const struct gmp_platform *);
int giulia_modem_submit(const struct gmr_command *);
int giulia_modem_get_snapshot(struct gmr_snapshot *);
/* Resident diagnostic command ingress. Scalars are values, never pointers.
 * A registered platform gateway must authorize the caller/phase before calling
 * this function. The service additionally requires debug + modem mode and an
 * exact current epoch. GMR_OK is queue acceptance; completion is asynchronous.
 * This API does not register an NLH provider or make the runtime reloadable. */
int giulia_modem_control(uint32_t operation,uint64_t expected_epoch);
int giulia_modem_terminal(const char *);
#ifdef __cplusplus
}
#endif
#endif
