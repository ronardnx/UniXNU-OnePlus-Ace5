/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Resident, by-value protocol selection. Hardware resources stay resident.
 */
#ifndef GIULIA_HOTLOAD_H
#define GIULIA_HOTLOAD_H
#include <stdint.h>

#ifndef GIULIA_HOTLOAD_ENABLED
#define GIULIA_HOTLOAD_ENABLED 0
#endif

#define GH_ABI 1u
#define GH_INTERFACE_ABI 0x47480001u
#define GH_STATE_ABI 1u
#define GH_MAX_WORDS 32u
#define GH_DOMAIN_COUNT 4u
#define GH_DOMAIN_GPU 1u
#define GH_DOMAIN_WIFI 2u
#define GH_DOMAIN_AUDIO 3u
#define GH_DOMAIN_MODEM 4u
#define GH_NAME_GPU "giulia-gpu-proto"
#define GH_NAME_WIFI "giulia-wifi-proto"
#define GH_NAME_AUDIO "giulia-audio-proto"
#define GH_NAME_MODEM "giulia-modem-proto"

/* host.platform(context, op, ticket, selector_or_value, &scalar).
 * Only an authenticated ACTIVE, depth-zero resident NLH_CALL can use these.
 * The scalar ticket is never an address. None of these operations touch HW.
 */
#define GH_PLATFORM_GET_META 0x4700u
#define GH_PLATFORM_READ_REQUEST_BASE 0x4720u
#define GH_PLATFORM_WRITE_RESPONSE_BASE 0x4760u
#define GH_META_DOMAIN 0u
#define GH_META_EPOCH 1u
#define GH_META_OPERATION 2u
#define GH_META_REQUEST_WORDS 3u
#define GH_META_RESPONSE_WORDS 4u
#define GH_META_ABI 5u
#define GH_META_COUNT 6u

/* Fixed Darwin ENOTSUP for the no-link fallback, including freestanding
 * protocol sources. This header deliberately does not require errno.h.
 */
#define GH_UNAVAILABLE 45

#ifdef __cplusplus
extern "C" {
#endif

#if GIULIA_HOTLOAD_ENABLED
/* Called once in sleepable resident startup before accepting any upload.
 * Registration or missing build-identity failure is sticky until reboot.
 */
int giulia_hotload_init(void);
/* Atomic approximate counters only; does not initialize or access hardware. */
void giulia_hotload_status(void);

/* Both counts are 1..GH_MAX_WORDS. The caller supplies an initialized baseline
 * response. All request words are copied before dispatch; response is copied
 * back only after successful execution, at least one request read and every
 * response field written. Any error preserves response byte-for-byte.
 *
 * epoch belongs to the resident owner and must never decrease/reset during
 * this boot, including across module replacement. The bridge rejects values
 * below the domain's admitted high-water mark. After return the owner must
 * validate its current epoch/state under its own lock, then validate every
 * response value before adopting it. A successful selection proves no HW
 * operation. Do not hold a lock needed by STOP; admission never waits.
 */
int giulia_hotload_select(uint32_t domain, uint64_t epoch, uint32_t operation,
    const uint64_t *request, uint32_t request_words,
    uint64_t *response, uint32_t response_words);
#else
static inline int giulia_hotload_select(uint32_t domain, uint64_t epoch,
    uint32_t operation, const uint64_t *request, uint32_t request_words,
    uint64_t *response, uint32_t response_words)
{
    (void)domain;
    (void)epoch;
    (void)operation;
    (void)request;
    (void)request_words;
    (void)response;
    (void)response_words;
    return GH_UNAVAILABLE;
}
#endif

#ifdef __cplusplus
}
#endif
#endif
