/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_AUDIO_PLATFORM_H
#define GIULIA_AUDIO_PLATFORM_H
#include "giulia_audio_runtime.h"

#define GA_PLATFORM_ABI 2u
/* A native platform owner may publish this only after it implements the
 * complete ops contract. Registration is not an ownership grant. Every
 * prerequisites()/pcm_available() call must validate the current epoch.
 * The provider, callback code, SIPA bytes and referenced memory must remain
 * pinned until reboot. No terminal or boot argument can install a provider.
 * Before successful registration the provider must not map or access IPCC.
 * A GPU-enabled profile reserves an IPCC host membership during registration,
 * with this immutable ga_platform pointer as its token, until reboot. Every
 * IPCC map/access and GAN_PROOF_IPCC assertion must additionally validate
 * gpr_global_legacy_ipcc_owns(2u, platform)==1. Registration does not prove the other
 * power, clock, memory-reservation, SMEM or fresh-epoch requirements.
 * ABI 2 requires checked metadata_release/unprepare completion; a provider
 * built for ABI 1 cannot prove teardown and must not register unchanged.
 * unprepare(cookie, boot) receives resident boot evidence. Its provider must
 * verify boot belongs to its own bound runtime and current native epoch;
 * copying a stopped boot struct cannot establish ownership. The native
 * unmap_nc callback likewise reports actual release completion.
 * No producer is currently included: do not claim hardware is connected.
 */
struct ga_platform {
    unsigned abi,bytes;
    struct gar_ops ops;
    const void *sipa;
    size_t sipa_bytes;
};
#ifdef __cplusplus
extern "C" {
#endif
int giulia_audio_platform_register(const struct ga_platform *);
int giulia_audio_runtime_command(const char *);
#ifdef __cplusplus
}
#endif
#endif
