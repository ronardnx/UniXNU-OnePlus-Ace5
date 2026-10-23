/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Kernel-only import contract for a real GPU CTX0 hardware-fence provider.
 * No implementation of the Gunyah controller is installed by this header.
 */
#ifndef GIULIA_GPU_FENCE_PROVIDER_H
#define GIULIA_GPU_FENCE_PROVIDER_H
#include "giulia_gpu_fence.h"
#define GGFP_ABI 1u
enum { GGFP_RESOURCES=1,GGFP_VM_READY=2,GGFP_SHARED=4,GGFP_LAYOUT=8,
    GGFP_CLIENT=16,GGFP_IPCC=32,GGFP_CPU_NC=64,GGFP_REQUIRED=127 };
struct ggf_proof {
    uint64_t epoch,carveout_physical,queue_physical;
    uint32_t carveout_bytes,queue_bytes,page_bytes,peer,mem_label,doorbell_label,client,proofs;
};
struct ggf_provider {
    uint32_t abi,bytes;
    const void *identity;
    void *cookie;
    uint64_t epoch;
    struct ggf_layout layout;
    const volatile void *memory;
    int (*observe)(void *,struct ggf_proof *); /* 1 only for a live authoritative proof */
    void (*barrier)(void *);
};
/* All callback code, cookie, identity and normal-NC queue backing must remain
 * pinned until reboot. Registration copies this descriptor but does not read,
 * initialize or claim the carveout. No terminal can register a provider.
 *
 * observe is bounded and read-only. It must verify actual peer READY, exact
 * shared memory, controller/page ABI, real CTX0 registration and coherent CPU
 * mapping; source constants or a successful map are never sufficient proof.
 * Never hard-code proof bits. Epoch loss must be reported before storage can
 * change; mapped backing is never revoked or reused until reset.
 *
 * The producer must acquire the paired IPCC lease below before touching the
 * protocol4 APPS/GPU channels. It shares only with the matching pinned GPU
 * owner, never QMP/audio. No signals are sent by this import layer.
 */
struct ggf_registration { unsigned state;struct ggf_provider provider; };
static inline int ggfp_valid(const struct ggf_provider *p)
{
    return p && p->abi==GGFP_ABI && p->bytes==sizeof(*p) && p->identity && p->epoch &&
        p->memory && !((uintptr_t)p->memory&3u) && p->observe && p->barrier &&
        ggf_layout_valid(&p->layout) && p->layout.bytes<=UINTPTR_MAX-(uintptr_t)p->memory;
}
static inline int ggfp_register(struct ggf_registration *r,const struct ggf_provider *p)
{
    if(!r || !p)return 0;
    struct ggf_provider copy=*p;
    if(!ggfp_valid(&copy))return 0;
    unsigned expected=0;
    if(!__atomic_compare_exchange_n(&r->state,&expected,1u,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return 0;
    r->provider=copy;
    __atomic_store_n(&r->state,2u,__ATOMIC_RELEASE);return 1;
}
static inline const struct ggf_provider *ggfp_get(const struct ggf_registration *r)
{ return r && __atomic_load_n(&r->state,__ATOMIC_ACQUIRE)==2?&r->provider:NULL; }
static inline int ggfp_live(const struct ggf_provider *p)
{
    if(!ggfp_valid(p))return 0;
    struct ggf_proof proof={0,0,0,0,0,0,0,0,0,0,0};
    if(p->observe(p->cookie,&proof)!=1)return 0;
    return proof.epoch==p->epoch && proof.carveout_physical==GGF_CARVEOUT_PHYSICAL &&
        proof.carveout_bytes==GGF_CARVEOUT_BYTES && proof.queue_physical==p->layout.physical &&
        proof.queue_bytes==p->layout.bytes && proof.page_bytes==p->layout.page_bytes &&
        proof.peer==3 && proof.mem_label==5 && proof.doorbell_label==6 && proof.client==1 &&
        proof.proofs==GGFP_REQUIRED;
}
static inline int ggfp_request(uint32_t bytes,uint32_t alignment,uint32_t flags,uint32_t kind,
    const struct ggf_layout *layout)
{
    if(!ggf_layout_valid(layout) || kind!=23 || !bytes || bytes>layout->bytes ||
       !(flags&8u) || (flags&~0x178u) || alignment>>16 || (alignment&255)>22 ||
       ((alignment>>8)&255)>22)return 0;
    uint32_t size_align=1u<<((alignment>>8)&255);
    return ((bytes+size_align-1)&~(size_align-1))<=layout->bytes;
}
#ifdef __cplusplus
extern "C" {
#endif
int giulia_gpu_fence_register(const struct ggf_provider *);
int giulia_gpu_fence_ipcc_claim(const void *,uint64_t);
int giulia_gpu_fence_ipcc_owns(const void *,uint64_t);
#ifdef __cplusplus
}
#endif
#endif
