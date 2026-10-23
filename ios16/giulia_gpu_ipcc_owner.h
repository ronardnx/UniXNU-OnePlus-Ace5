/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Software arbitration for the overlapping fixed IPC aperture. Host clients
 * may coexist; a GMU aperture lease excludes every host client until reboot.
 * This is not a power/clock grant or authority over another execution world.
 */
#ifndef GIULIA_GPU_IPCC_OWNER_H
#define GIULIA_GPU_IPCC_OWNER_H
#include <stddef.h>
#include <stdint.h>
enum { GGI_HOST_QMP=1, GGI_HOST_AUDIO=2, GGI_GPU=3 };
struct ggi_owner { unsigned gate; const void *owners[3];
    const void *fence_owner;uint64_t fence_epoch; };
static inline int ggi_valid(unsigned client,const void *owner)
{ return client>=GGI_HOST_QMP && client<=GGI_GPU && owner; }
static inline int ggi_owns(const struct ggi_owner *state,unsigned client,const void *owner)
{
    return state && ggi_valid(client,owner) &&
        __atomic_load_n(&state->owners[client-1],__ATOMIC_ACQUIRE)==owner;
}
static inline int ggi_claim(struct ggi_owner *state,unsigned client,const void *owner)
{
    unsigned expected=0;
    if(!state || !ggi_valid(client,owner) ||
       !__atomic_compare_exchange_n(&state->gate,&expected,1u,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return 0;
    /* Observe teardown published by release(), which deliberately does not
     * take gate. Acquiring gate alone cannot synchronize those removals. */
    int allowed=!__atomic_load_n(&state->owners[client-1],__ATOMIC_ACQUIRE);
    if(client==GGI_GPU)allowed=allowed && !__atomic_load_n(&state->owners[0],__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&state->owners[1],__ATOMIC_ACQUIRE);
    else allowed=allowed && !__atomic_load_n(&state->owners[GGI_GPU-1],__ATOMIC_ACQUIRE);
    if(allowed)__atomic_store_n(&state->owners[client-1],owner,__ATOMIC_RELEASE);
    __atomic_store_n(&state->gate,0u,__ATOMIC_RELEASE);
    return allowed;
}
static inline int ggi_release(struct ggi_owner *state,unsigned client,const void *owner)
{
    if(!state || !ggi_valid(client,owner))return 0;
    if(client==GGI_GPU) {
        unsigned idle=0;
        if(!__atomic_compare_exchange_n(&state->gate,&idle,1u,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return 0;
        int allowed=ggi_owns(state,client,owner) && !__atomic_load_n(&state->fence_owner,__ATOMIC_ACQUIRE);
        if(allowed)__atomic_store_n(&state->owners[client-1],(const void *)NULL,__ATOMIC_RELEASE);
        __atomic_store_n(&state->gate,0u,__ATOMIC_RELEASE);return allowed;
    }
    /* Release only removes an owner, so it cannot break claim exclusion.
     * A matching CAS guarantees rollback never fails just because another
     * client is claiming; callers may destroy their token after unmapping. */
    const void *expected=owner;
    return __atomic_compare_exchange_n(&state->owners[client-1],&expected,(const void *)NULL,
        0,__ATOMIC_RELEASE,__ATOMIC_RELAXED);
}
/* Pair once with a pinned GPU lease. No public terminal/client ID can create
 * a fence owner. The producer must still prove its Gunyah/queue lifecycle. */
static inline int ggi_fence_owns(const struct ggi_owner *s,const void *gpu,const void *provider,uint64_t epoch)
{
    return s && gpu && provider && epoch && ggi_owns(s,GGI_GPU,gpu) &&
        __atomic_load_n(&s->fence_owner,__ATOMIC_ACQUIRE)==provider &&
        __atomic_load_n(&s->fence_epoch,__ATOMIC_ACQUIRE)==epoch;
}
static inline int ggi_pair_fence(struct ggi_owner *s,const void *gpu,const void *provider,uint64_t epoch)
{
    unsigned idle=0;
    if(!s || !gpu || !provider || !epoch ||
       !__atomic_compare_exchange_n(&s->gate,&idle,1u,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return 0;
    int allowed=ggi_owns(s,GGI_GPU,gpu) && !__atomic_load_n(&s->owners[0],__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&s->owners[1],__ATOMIC_ACQUIRE);
    const void *held=__atomic_load_n(&s->fence_owner,__ATOMIC_ACQUIRE);
    if(held)allowed=allowed && held==provider && __atomic_load_n(&s->fence_epoch,__ATOMIC_ACQUIRE)==epoch;
    else if(allowed) {
        __atomic_store_n(&s->fence_epoch,epoch,__ATOMIC_RELEASE);
        __atomic_store_n(&s->fence_owner,provider,__ATOMIC_RELEASE);
    }
    __atomic_store_n(&s->gate,0u,__ATOMIC_RELEASE);return allowed;
}
#endif
