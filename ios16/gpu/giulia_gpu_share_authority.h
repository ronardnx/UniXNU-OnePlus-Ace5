/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Decidable half of the share authority proof.
 *
 * `ggsh` requires a caller that can prove primary role and exclusive ownership
 * of the fence carveout before it may assign anything. Most of that is not
 * knowable from inside this driver, but the parts that are decidable should be
 * checked in code instead of being waved through:
 *
 *   - the epoch and the owner token must be this transaction's;
 *   - the control page must be an owned, page-aligned 16 KiB block that does
 *     not overlap the carveout;
 *   - the carveout must be exactly the fixed fence window;
 *   - the UEFI power report must say prepared and the platform must be healthy;
 *   - the RM-side facts (this VM is primary; no other sharing driver owns the
 *     range) are **inputs**, never inferred here. While they are absent the
 *     verdict is UNPROVEN, which is a refusal, not a pass.
 *
 * Reading this module must not suggest that a green local check authorises a
 * transaction: the last two bits come from the resource manager or they do not
 * exist. Constants in place of them would authorise a second owner.
 */
#ifndef GIULIA_GPU_SHARE_AUTHORITY_H
#define GIULIA_GPU_SHARE_AUTHORITY_H
#include <stdint.h>
#include "giulia_gpu_fence.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GGSA_OK=0, GGSA_ARGUMENT=1, GGSA_EPOCH=2, GGSA_OWNER=3, GGSA_CONTROL=4,
    GGSA_RANGE=5, GGSA_POWER=6, GGSA_PLATFORM=7, GGSA_UNPROVEN=8
};

enum {
    GGSA_PROOF_EPOCH=1, GGSA_PROOF_OWNER=2, GGSA_PROOF_CONTROL=4,
    GGSA_PROOF_RANGE=8, GGSA_PROOF_POWER=16, GGSA_PROOF_PLATFORM=32,
    GGSA_PROOF_PRIMARY=64, GGSA_PROOF_EXCLUSIVE=128,
    GGSA_PROOF_LOCAL=(GGSA_PROOF_EPOCH|GGSA_PROOF_OWNER|GGSA_PROOF_CONTROL|
        GGSA_PROOF_RANGE|GGSA_PROOF_POWER|GGSA_PROOF_PLATFORM),
    GGSA_PROOF_RM=(GGSA_PROOF_PRIMARY|GGSA_PROOF_EXCLUSIVE)
};

struct ggsa_inputs {
    uint64_t epoch, expected_epoch;
    const void *owner, *expected_owner;
    uint64_t control_pa;
    uint32_t control_bytes;
    uint64_t carveout_physical, carveout_bytes;
    unsigned power_prepared;   /* UEFI mode-3 report says the clocks are prepared */
    unsigned platform_healthy; /* not poisoned, no observed fault */
    unsigned rm_primary;       /* RM says this VM is the primary owner */
    unsigned rm_exclusive;     /* RM says no second sharing driver owns the range */
};

struct ggsa_verdict {
    unsigned code;    /* GGSA_* */
    unsigned proofs;  /* bits proven by this call */
    unsigned missing; /* bits still required */
};

static inline const char *ggsa_code_name(unsigned code)
{
    const char *const names[]={"ok","argument","epoch","owner","control","range",
        "power","platform","unproven"};
    return code<sizeof(names)/sizeof(names[0])?names[code]:"invalid";
}

static inline int ggsa_control_ok(uint64_t control_pa,uint32_t bytes,
    uint64_t carveout_physical,uint64_t carveout_bytes)
{
    if (!control_pa || bytes!=16384 || (control_pa&16383)) return 0;
    if (control_pa>(UINT64_C(1)<<36)-16384) return 0;
    /* The control page must sit outside the carveout it describes. */
    if (control_pa<carveout_physical+carveout_bytes &&
        carveout_physical<control_pa+16384) return 0;
    return 1;
}

/* Verify the local facts, then require the RM-side ones. Never returns OK while
 * rm_primary or rm_exclusive is absent, regardless of how clean the rest is. */
static inline int ggsa_verify(const struct ggsa_inputs *in,struct ggsa_verdict *out)
{
    struct ggsa_verdict verdict={GGSA_OK,0,GGSA_PROOF_LOCAL|GGSA_PROOF_RM};
    if (!in || !out) return GGSA_ARGUMENT;
    if (!in->epoch || in->epoch!=in->expected_epoch) {
        verdict.code=GGSA_EPOCH;
        *out=verdict;
        return verdict.code;
    }
    verdict.proofs|=GGSA_PROOF_EPOCH;
    if (!in->owner || in->owner!=in->expected_owner) {
        verdict.code=GGSA_OWNER;
        *out=verdict;
        return verdict.code;
    }
    verdict.proofs|=GGSA_PROOF_OWNER;
    if (in->carveout_physical!=GGF_CARVEOUT_PHYSICAL ||
        in->carveout_bytes!=GGF_CARVEOUT_BYTES) {
        verdict.code=GGSA_RANGE;
        *out=verdict;
        return verdict.code;
    }
    verdict.proofs|=GGSA_PROOF_RANGE;
    if (!ggsa_control_ok(in->control_pa,in->control_bytes,
            in->carveout_physical,in->carveout_bytes)) {
        verdict.code=GGSA_CONTROL;
        *out=verdict;
        return verdict.code;
    }
    verdict.proofs|=GGSA_PROOF_CONTROL;
    if (!in->power_prepared) {
        verdict.code=GGSA_POWER;
        *out=verdict;
        return verdict.code;
    }
    verdict.proofs|=GGSA_PROOF_POWER;
    if (!in->platform_healthy) {
        verdict.code=GGSA_PLATFORM;
        *out=verdict;
        return verdict.code;
    }
    verdict.proofs|=GGSA_PROOF_PLATFORM;
    if (in->rm_primary) verdict.proofs|=GGSA_PROOF_PRIMARY;
    if (in->rm_exclusive) verdict.proofs|=GGSA_PROOF_EXCLUSIVE;
    verdict.missing=(GGSA_PROOF_LOCAL|GGSA_PROOF_RM)&~verdict.proofs;
    verdict.code=verdict.missing?GGSA_UNPROVEN:GGSA_OK;
    *out=verdict;
    return verdict.code;
}
#ifdef __cplusplus
}
#endif
#endif
