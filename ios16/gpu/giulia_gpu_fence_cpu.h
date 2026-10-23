/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * CPU import of an already owned hardware-fence queue. No allocation,
 * carveout initialization, queue access, VM sharing or fence registration.
 */
#ifndef GIULIA_GPU_FENCE_CPU_H
#define GIULIA_GPU_FENCE_CPU_H
#include "giulia_gpu_fence.h"

enum { GGFC_OK=0,GGFC_ARGUMENT=1,GGFC_BUSY=2,GGFC_LOST=3,
    GGFC_MAP=4,GGFC_AUDIT=5,GGFC_QUARANTINED=6,GGFC_ALREADY=7 };
enum { GGFC_EMPTY=0,GGFC_ACTIVE=1,GGFC_MAPPED=2,GGFC_BROKEN=3 };
enum { GGFC_SHARED=1,GGFC_READY=2,GGFC_LAYOUT=4,GGFC_CPU_RANGE=8,
    GGFC_CACHE_OWNER=16,GGFC_SOURCE_REQUIRED=31 };
enum { GGFC_EXACT_PA=1,GGFC_NORMAL_NC=2,GGFC_NO_WB_ALIAS=4,
    GGFC_PINNED=8,GGFC_AUDIT_REQUIRED=15 };

struct ggfc_envelope {
    struct ggf_layout layout;
    uint64_t physical;
    uint32_t bytes,offset,local_page_bytes,prefix_bytes,suffix_bytes;
};
/* CPU mappings are rounded using the local CPU granule, never the provider's
 * verified host ABI. Both neighboring pieces remain shared-controller data:
 * fitting inside the carveout does not authorize mapping or accessing them.
 * Mapping the whole carveout rounded down would expose unowned pages before
 * its 4 KiB-aligned start, so that operation is deliberately not offered. */
static inline int ggfc_envelope(const struct ggf_layout *layout,uint32_t local_page,
    struct ggfc_envelope *out)
{
    if(!out || !layout || (uintptr_t)layout%__alignof__(*layout) ||
       (uintptr_t)out%__alignof__(*out) || !ggf_layout_valid(layout) ||
       (local_page!=4096 && local_page!=16384))return 0;
    uint64_t first=layout->physical&~((uint64_t)local_page-1);
    uint64_t end=(layout->physical+layout->bytes+local_page-1)&~((uint64_t)local_page-1);
    if(first<GGF_CARVEOUT_PHYSICAL || end>GGF_CARVEOUT_PHYSICAL+GGF_CARVEOUT_BYTES ||
       end<=first || end-first>UINT32_MAX)return 0;
    struct ggfc_envelope result;
    result.layout=*layout;result.physical=first;result.bytes=(uint32_t)(end-first);
    result.offset=(uint32_t)(layout->physical-first);result.local_page_bytes=local_page;
    result.prefix_bytes=result.offset;result.suffix_bytes=(uint32_t)(end-layout->physical-layout->bytes);
    *out=result;return 1;
}
static inline int ggfc_envelope_valid(const struct ggfc_envelope *p)
{
    struct ggfc_envelope expected;
    return p && !((uintptr_t)p%__alignof__(*p)) &&
        ggfc_envelope(&p->layout,p->local_page_bytes,&expected) &&
        p->physical==expected.physical && p->bytes==expected.bytes &&
        p->offset==expected.offset && p->prefix_bytes==expected.prefix_bytes &&
        p->suffix_bytes==expected.suffix_bytes;
}
struct ggfc_source {
    const void *identity;
    uint64_t epoch,carveout_physical,envelope_physical;
    uint32_t carveout_bytes,envelope_bytes,parcel_handle,proofs;
    uint16_t self_vmid,peer_vmid;
    struct ggf_layout layout;
};
/* All addresses/lengths below must originate from the actual mapper, not the
 * envelope calculator or a requested cache option. queue is its real CPU
 * subrange; this layer verifies the offset but never dereferences it. */
struct ggfc_mapping {
    const void *handle;
    volatile void *base,*queue;
    uint64_t physical;
    uint32_t bytes,local_page_bytes;
};
struct ggfc_cpu_audit { struct ggfc_mapping mapping;uint32_t proofs; };
struct ggfc_owner {
    const void *identity;
    uint64_t epoch;
    void *cookie;
    int (*observe)(void *,const struct ggfc_envelope *,struct ggfc_source *);
    int (*map)(void *,const struct ggfc_envelope *,struct ggfc_mapping *);
    int (*audit)(void *,const struct ggfc_envelope *,const struct ggfc_mapping *,struct ggfc_cpu_audit *);
};
/* The actual kernel owner serializes hardware/channel/cache ownership for
 * the complete operation. Bounded callbacks may call these APIs, but reentry
 * never maps or observes shared bytes. observe is authoritative and read-only:
 * it proves current sharing/peer identity, exact controller/page ABI, explicit
 * authorization for the FULL CPU envelope (including neighboring 4 KiB pages)
 * and the cache-attribute lease. A boot report or historical share transcript
 * cannot supply these proofs. Each returned generation must retain exactly
 * the same dynamic self/peer/parcel identity across mapping and later audits.
 *
 * map installs only an actual Normal NC mapping and preserves every created
 * descriptor/map until reboot, even if it returns failure or no handle.
 * retained is set before map is called. Never clear/free/reuse the envelope
 * after ambiguity. audit checks actual descriptor/CPU PA translations, Normal
 * NC attributes, absence of any writeback alias over the ENTIRE envelope and
 * pinned lifetime; passing kIOMapWriteCombineCache alone is not that audit.
 * CPU/cache alias ownership is global: an IOKit-only mapping registry alone
 * does not establish the absence of a direct-map or another WB alias.
 *
 * State, callback code, cookie, source identity and real mapper objects stay
 * pinned until reboot. No terminal, DT property or report installs this owner.
 * No API here reads/zeros the carveout or generates GGFP_CPU_NC automatically.
 */
struct ggfc_state {
    unsigned state,loss,retained,last_result;
    struct ggfc_owner owner;
    struct ggfc_envelope envelope;
    struct ggfc_source source;
    struct ggfc_mapping mapping;
};
static inline int ggfc_range(const void *p,size_t bytes)
{return p && bytes && bytes<=UINTPTR_MAX-(uintptr_t)p;}
static inline int ggfc_disjoint(const void *a,size_t na,const void *b,size_t nb)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return ggfc_range(a,na) && ggfc_range(b,nb) && (x+na<=y || y+nb<=x);
}
static inline int ggfc_lost(const struct ggfc_state *s)
{return __atomic_load_n(&s->loss,__ATOMIC_ACQUIRE)!=0;}
static inline void ggfc_zero(void *p,size_t bytes)
{for(size_t i=0;i<bytes;i++)((unsigned char *)p)[i]=0;}
static inline int ggfc_fail(struct ggfc_state *s,unsigned result)
{
    __atomic_store_n(&s->loss,1u,__ATOMIC_RELEASE);s->last_result=result;
    __atomic_store_n(&s->state,GGFC_BROKEN,__ATOMIC_RELEASE);return (int)result;
}
static inline int ggfc_source_valid(const struct ggfc_state *s,const struct ggfc_source *p)
{
    return p && p->identity==s->owner.identity && p->epoch==s->owner.epoch &&
        p->carveout_physical==GGF_CARVEOUT_PHYSICAL && p->carveout_bytes==GGF_CARVEOUT_BYTES &&
        p->envelope_physical==s->envelope.physical && p->envelope_bytes==s->envelope.bytes &&
        p->proofs==GGFC_SOURCE_REQUIRED && p->self_vmid && p->self_vmid!=UINT16_MAX &&
        p->peer_vmid && p->peer_vmid!=UINT16_MAX && p->self_vmid!=p->peer_vmid &&
        ggf_layout_valid(&p->layout) && p->layout.page_bytes==s->envelope.layout.page_bytes;
}
static inline int ggfc_observe(struct ggfc_state *s,int first)
{
    struct ggfc_source proof;ggfc_zero(&proof,sizeof(proof));
    if(ggfc_lost(s) || !ggfc_envelope_valid(&s->envelope) ||
       s->owner.observe(s->owner.cookie,&s->envelope,&proof)!=1 || ggfc_lost(s) ||
       !ggfc_source_valid(s,&proof))return 0;
    if(first){s->source=proof;return 1;}
    return proof.self_vmid==s->source.self_vmid && proof.peer_vmid==s->source.peer_vmid &&
        proof.parcel_handle==s->source.parcel_handle;
}
static inline int ggfc_mapping_valid(const struct ggfc_state *s,const struct ggfc_mapping *m)
{
    if(!s || !m)return 0;
    const struct ggfc_envelope *e=&s->envelope;
    uintptr_t base=(uintptr_t)m->base;
    return m->handle && ggfc_range((const void *)m->base,m->bytes) &&
        !(base&(e->local_page_bytes-1)) && m->physical==e->physical && m->bytes==e->bytes &&
        m->local_page_bytes==e->local_page_bytes && (uintptr_t)m->queue==base+e->offset &&
        ggfc_disjoint(s,sizeof(*s),(const void *)m->base,m->bytes) &&
        ggfc_disjoint(m->handle,1,(const void *)m->base,m->bytes);
}
static inline int ggfc_same_mapping(const struct ggfc_mapping *a,const struct ggfc_mapping *b)
{
    return a->handle==b->handle && a->base==b->base && a->queue==b->queue &&
        a->physical==b->physical && a->bytes==b->bytes && a->local_page_bytes==b->local_page_bytes;
}
static inline int ggfc_audit_mapping(struct ggfc_state *s)
{
    struct ggfc_cpu_audit audit;ggfc_zero(&audit,sizeof(audit));
    return ggfc_mapping_valid(s,&s->mapping) && ggfc_observe(s,0) &&
        s->owner.audit(s->owner.cookie,&s->envelope,&s->mapping,&audit)==1 &&
        !ggfc_lost(s) && ggfc_mapping_valid(s,&s->mapping) &&
        audit.proofs==GGFC_AUDIT_REQUIRED && ggfc_same_mapping(&audit.mapping,&s->mapping) &&
        ggfc_observe(s,0);
}
/* Zero state once in private pinned kernel memory. One map attempt per state;
 * successful imports are revalidated using get, never by remapping. */
static inline int ggfc_import(struct ggfc_state *s,const struct ggfc_owner *owner,
    const struct ggf_layout *layout,uint32_t local_page)
{
    struct ggfc_envelope envelope;
    if(!s || (uintptr_t)s%__alignof__(*s))return GGFC_ARGUMENT;
    unsigned state=__atomic_load_n(&s->state,__ATOMIC_ACQUIRE);
    if(state!=GGFC_EMPTY)return state==GGFC_BROKEN?GGFC_QUARANTINED:
        state==GGFC_MAPPED?GGFC_ALREADY:GGFC_BUSY;
    if(!owner || (uintptr_t)owner%__alignof__(*owner) || !owner->identity || !owner->epoch ||
       !owner->observe || !owner->map || !owner->audit ||
       !ggfc_disjoint(s,sizeof(*s),owner,sizeof(*owner)) ||
       !ggfc_disjoint(s,sizeof(*s),layout,sizeof(*layout)) ||
       !ggfc_envelope(layout,local_page,&envelope))return GGFC_ARGUMENT;
    unsigned expected=GGFC_EMPTY;
    if(!__atomic_compare_exchange_n(&s->state,&expected,GGFC_ACTIVE,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))
        return expected==GGFC_BROKEN?GGFC_QUARANTINED:expected==GGFC_MAPPED?GGFC_ALREADY:GGFC_BUSY;
    s->owner=*owner;s->envelope=envelope;
    if(!ggfc_observe(s,1))return ggfc_fail(s,GGFC_LOST);
    s->retained=1;
    if(s->owner.map(s->owner.cookie,&s->envelope,&s->mapping)!=1)return ggfc_fail(s,GGFC_MAP);
    if(ggfc_lost(s) || !ggfc_observe(s,0))return ggfc_fail(s,GGFC_LOST);
    if(!ggfc_audit_mapping(s))return ggfc_fail(s,GGFC_AUDIT);
    s->last_result=GGFC_OK;
    __atomic_store_n(&s->state,GGFC_MAPPED,__ATOMIC_RELEASE);return GGFC_OK;
}
/* Return only the actual mapper's frozen output, after current authoritative
 * source and CPU mapping audits. Outputs cannot overlap state or any byte of
 * the mapped envelope, including prefix/suffix padding. Failure preserves out. */
static inline int ggfc_get(struct ggfc_state *s,struct ggfc_mapping *out)
{
    if(!s || (uintptr_t)s%__alignof__(*s) || !out || (uintptr_t)out%__alignof__(*out) ||
       !ggfc_disjoint(s,sizeof(*s),out,sizeof(*out)))return GGFC_ARGUMENT;
    unsigned expected=GGFC_MAPPED;
    if(!__atomic_compare_exchange_n(&s->state,&expected,GGFC_ACTIVE,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))
        return expected==GGFC_BROKEN?GGFC_QUARANTINED:expected==GGFC_ACTIVE?GGFC_BUSY:GGFC_ARGUMENT;
    if(!ggfc_disjoint(out,sizeof(*out),(const void *)s->mapping.base,s->mapping.bytes)) {
        __atomic_store_n(&s->state,GGFC_MAPPED,__ATOMIC_RELEASE);return GGFC_ARGUMENT;
    }
    if(!ggfc_audit_mapping(s))return ggfc_fail(s,GGFC_AUDIT);
    *out=s->mapping;s->last_result=GGFC_OK;
    __atomic_store_n(&s->state,GGFC_MAPPED,__ATOMIC_RELEASE);return GGFC_OK;
}
/* Explicit loss can be latched from a callback/another owner context. Mapping
 * handles and retained backing survive quarantine; there is no release API. */
static inline int ggfc_invalidate(struct ggfc_state *s)
{
    if(!s || (uintptr_t)s%__alignof__(*s))return GGFC_ARGUMENT;
    unsigned state=__atomic_load_n(&s->state,__ATOMIC_ACQUIRE);
    if(state==GGFC_EMPTY)return GGFC_ARGUMENT;
    __atomic_store_n(&s->loss,1u,__ATOMIC_RELEASE);
    unsigned expected=GGFC_MAPPED;
    if(__atomic_compare_exchange_n(&s->state,&expected,GGFC_ACTIVE,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))
        return ggfc_fail(s,GGFC_LOST);
    return state==GGFC_BROKEN?GGFC_QUARANTINED:GGFC_LOST;
}
#endif
