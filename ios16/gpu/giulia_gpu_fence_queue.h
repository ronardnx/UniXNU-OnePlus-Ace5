/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * CTX0 queue helpers for a real, separately verified hardware-fence owner.
 * No controller, shared-memory initialization or completion inference here.
 * The stock CPU fallback writes TX, then signals GPU -> APPS signal 0.
 * Normal production belongs to the GMU; CTX0 has update_rxq=false. Therefore
 * this interface never consumes RX or runs as a default GPU submission path.
 */
#ifndef GIULIA_GPU_FENCE_QUEUE_H
#define GIULIA_GPU_FENCE_QUEUE_H
#include "giulia_gpu_fence_provider.h"
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "The pinned CTX0 shared-memory ABI requires a little-endian CPU"
#endif

enum { GGFQ_TYPE=1,GGFQ_VERSION=0x0100,GGFQ_DWORDS=16 };
enum { GGFQ_EMPTY=0,GGFQ_OK=1,GGFQ_INVALID=-1,GGFQ_FULL=-2,
    GGFQ_LOST=-3,GGFQ_QUARANTINED=-4,GGFQ_BUSY=-5,GGFQ_HANDLE=-6 };
enum { GGFQ_UNBOUND=0,GGFQ_ACTIVE=1,GGFQ_READY=2,GGFQ_BROKEN=3 };

struct ggfq_fence { uint64_t context,seqno,hash; };
struct ggfq_payload {
    struct ggfq_fence fence;
    uint64_t flags,client_data,timestamp;
    uint32_t error;
};

/* The wire image is sixteen LE dwords, not a native compiler struct. The
 * pinned writer does not initialize reserve, so decoding ignores that word.
 * Codec calls operate on private buffers only and grant no queue authority.
 */
static inline uint64_t ggfq_join(uint32_t low,uint32_t high)
{ return (uint64_t)low|((uint64_t)high<<32); }
static inline int ggfq_encode(uint32_t wire[GGFQ_DWORDS],const struct ggfq_payload *p)
{
    if(!wire || !p || p->fence.hash>=GGF_FENCE_ENTRIES)return 0;
    struct ggfq_payload copy=*p;p=&copy;
    wire[0]=GGF_GPU_PAYLOAD_BYTES;wire[1]=(GGFQ_VERSION<<16)|GGFQ_TYPE;
    wire[2]=(uint32_t)p->fence.context;wire[3]=(uint32_t)(p->fence.context>>32);
    wire[4]=(uint32_t)p->fence.seqno;wire[5]=(uint32_t)(p->fence.seqno>>32);
    wire[6]=(uint32_t)p->fence.hash;wire[7]=(uint32_t)(p->fence.hash>>32);
    wire[8]=(uint32_t)p->flags;wire[9]=(uint32_t)(p->flags>>32);
    wire[10]=(uint32_t)p->client_data;wire[11]=(uint32_t)(p->client_data>>32);
    wire[12]=p->error;wire[13]=(uint32_t)p->timestamp;
    wire[14]=(uint32_t)(p->timestamp>>32);wire[15]=0;
    return 1;
}
static inline int ggfq_decode(const uint32_t wire[GGFQ_DWORDS],struct ggfq_payload *out)
{
    if(!wire || !out || wire[0]!=GGF_GPU_PAYLOAD_BYTES ||
       wire[1]!=((GGFQ_VERSION<<16)|GGFQ_TYPE))return 0;
    struct ggfq_payload p;
    p.fence.context=ggfq_join(wire[2],wire[3]);
    p.fence.seqno=ggfq_join(wire[4],wire[5]);
    p.fence.hash=ggfq_join(wire[6],wire[7]);
    if(p.fence.hash>=GGF_FENCE_ENTRIES)return 0;
    p.flags=ggfq_join(wire[8],wire[9]);p.client_data=ggfq_join(wire[10],wire[11]);
    p.error=wire[12];p.timestamp=ggfq_join(wire[13],wire[14]);
    *out=p;return 1;
}

struct ggfq_owner {
    const struct ggf_registration *registration;
    const void *identity;
    uint64_t epoch;
    volatile void *memory;
    void *cookie;
    int (*acquire)(void *,const struct ggf_provider *);
    void (*release)(void *,const struct ggf_provider *);
    int (*validate_handle)(void *,const struct ggf_provider *,const void *,struct ggfq_fence *);
    int (*signal)(void *,const struct ggf_provider *);
};
struct ggfq_queue { unsigned state;struct ggfq_owner owner; };

/* Every callback is bounded and non-reentrant. acquire must fail immediately
 * if ownership is unavailable; it may not start/restart hardware or wait for
 * it. Success proves an already established CPU-fallback-exclusive lease:
 * actual GMU producers/consumers are quiesced, pending TX reservations have
 * been reconciled, and all host producers are serialized until release.
 * A host spinlock alone cannot establish this lease. The controller may
 * advance TX read_index. The backing and callbacks stay pinned until reboot.
 *
 * validate_handle resolves a live opaque, provider-owned fence, checks exact
 * context/sequence/hash ownership and eligibility for CPU fallback (including
 * retirement and duplicate prevention). A caller-supplied hash is not a
 * handle. Validation does not allocate or retire a fence. Its results remain
 * reserved until release and stable across the pre-commit validation.
 *
 * signal uses ONLY the matching already-held paired IPCC lease, verifies it
 * is still owned, and sends the stock GPU -> APPS signal 0. It receives no
 * arbitrary register or channel arguments. It returns 1 only on success.
 * Callers must treat all quarantine errors as fatal and never retry a fence
 * whose index was committed: this layer neither rewinds indices nor proves
 * completion. The owner tracks that fence until recovery outside this API.
 *
 * Descriptors and queue state are private, pinned kernel memory. Initialize
 * queue to zero once; binding never clears shared storage or resets a bound
 * or quarantined queue. No terminal entry point may install an owner.
 */
static inline int ggfq_outside(const struct ggf_provider *p,const void *object,size_t bytes)
{
    uintptr_t begin=(uintptr_t)object,shared=(uintptr_t)p->memory;
    return object && bytes<=UINTPTR_MAX-begin &&
        (begin>=shared+p->layout.bytes || shared>=begin+bytes);
}
static inline int ggfq_owner_valid(const struct ggfq_owner *o)
{
    if(!o || !o->registration)return 0;
    const struct ggf_provider *p=ggfp_get(o->registration);
    return ggfp_valid(p) && o->identity==p->identity && o->epoch==p->epoch &&
        o->memory==p->memory && o->acquire && o->release && o->validate_handle && o->signal;
}
static inline int ggfq_bind(struct ggfq_queue *q,const struct ggfq_owner *owner)
{
    if(!q || !ggfq_owner_valid(owner))return GGFQ_INVALID;
    const struct ggf_provider *p=ggfp_get(owner->registration);
    if(!ggfq_outside(p,q,sizeof(*q)) || !ggfq_outside(p,owner,sizeof(*owner)))
        return GGFQ_INVALID;
    struct ggfq_owner copy=*owner;
    unsigned expected=GGFQ_UNBOUND;
    if(!__atomic_compare_exchange_n(&q->state,&expected,GGFQ_ACTIVE,0,
            __ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return GGFQ_BUSY;
    if(!ggfp_live(p)) {
        __atomic_store_n(&q->state,GGFQ_UNBOUND,__ATOMIC_RELEASE);return GGFQ_LOST;
    }
    q->owner=copy;
    __atomic_store_n(&q->state,GGFQ_READY,__ATOMIC_RELEASE);return GGFQ_OK;
}
static inline int ggfq_enter(struct ggfq_queue *q,const struct ggf_provider **out)
{
    if(!q || !out)return GGFQ_INVALID;
    unsigned expected=GGFQ_READY;
    if(!__atomic_compare_exchange_n(&q->state,&expected,GGFQ_ACTIVE,0,
            __ATOMIC_ACQUIRE,__ATOMIC_RELAXED))
        return expected==GGFQ_BROKEN?GGFQ_QUARANTINED:GGFQ_BUSY;
    if(!ggfq_owner_valid(&q->owner)) {
        __atomic_store_n(&q->state,GGFQ_BROKEN,__ATOMIC_RELEASE);return GGFQ_LOST;
    }
    const struct ggf_provider *p=ggfp_get(q->owner.registration);
    if(!ggfp_live(p)) {
        __atomic_store_n(&q->state,GGFQ_BROKEN,__ATOMIC_RELEASE);return GGFQ_LOST;
    }
    if(q->owner.acquire(q->owner.cookie,p)!=1) {
        if(!ggfp_live(p)) {
            __atomic_store_n(&q->state,GGFQ_BROKEN,__ATOMIC_RELEASE);return GGFQ_LOST;
        }
        __atomic_store_n(&q->state,GGFQ_READY,__ATOMIC_RELEASE);return GGFQ_BUSY;
    }
    if(!ggfp_live(p)) {
        q->owner.release(q->owner.cookie,p);
        __atomic_store_n(&q->state,GGFQ_BROKEN,__ATOMIC_RELEASE);return GGFQ_LOST;
    }
    *out=p;return GGFQ_OK;
}
static inline int ggfq_leave(struct ggfq_queue *q,const struct ggf_provider *p,int result,int broken)
{
    q->owner.release(q->owner.cookie,p);
    __atomic_store_n(&q->state,broken?GGFQ_BROKEN:GGFQ_READY,__ATOMIC_RELEASE);
    return result;
}
static inline int ggfq_sample(const struct ggf_provider *p,struct ggf_observation *sample)
{
    /* The controller may legitimately advance TX read_index between the two
     * valid samples. The exclusive lease protects the GMU-owned indices; do
     * not turn ordinary peer consumption into a permanent fault. */
    return ggfp_live(p) && ggf_snapshot(p->memory,p->layout.bytes,p->layout.physical,
        &p->layout,sample,p->barrier,p->cookie) && ggfp_live(p);
}

/* Queue one already eligible real fence notification. flags/error/time are
 * supplied by the trusted owner; context/sequence/hash can only come from its
 * real-handle resolver. No software completion callback is invoked here.
 */
static inline int ggfq_tx_push(struct ggfq_queue *q,const void *handle,
    uint64_t flags,uint32_t error,uint64_t timestamp)
{
    if(!handle)return GGFQ_HANDLE;
    const struct ggf_provider *p;
    int result=ggfq_enter(q,&p);
    if(result!=GGFQ_OK)return result;
    struct ggfq_payload payload={{0,0,0},0,0,0,0};struct ggfq_fence checked={0,0,0};
    payload.flags=flags;payload.error=error;payload.timestamp=timestamp;
    int valid=q->owner.validate_handle(q->owner.cookie,p,handle,&payload.fence);
    if(!ggfp_live(p))return ggfq_leave(q,p,GGFQ_LOST,1);
    if(valid!=1 || payload.fence.hash>=GGF_FENCE_ENTRIES)
        return ggfq_leave(q,p,GGFQ_HANDLE,0);
    struct ggf_observation first,last;
    if(!ggfq_sample(p,&first))return ggfq_leave(q,p,GGFQ_LOST,1);
    uint32_t next=first.tx_write+GGFQ_DWORDS;
    if(next==GGF_QUEUE_ENTRIES*GGFQ_DWORDS)next=0;
    if(next==first.tx_read)return ggfq_leave(q,p,GGFQ_FULL,0);
    uint32_t wire[GGFQ_DWORDS];
    if(!ggfq_encode(wire,&payload))return ggfq_leave(q,p,GGFQ_HANDLE,0);
    valid=q->owner.validate_handle(q->owner.cookie,p,handle,&checked);
    if(!ggfp_live(p))return ggfq_leave(q,p,GGFQ_LOST,1);
    if(valid!=1 || checked.context!=payload.fence.context || checked.seqno!=payload.fence.seqno ||
       checked.hash!=payload.fence.hash)return ggfq_leave(q,p,GGFQ_HANDLE,0);
    volatile uint32_t *words=(volatile uint32_t *)q->owner.memory;
    volatile uint32_t *slot=words+p->layout.tx_payload/4+first.tx_write;
    for(unsigned i=0;i<GGFQ_DWORDS;i++)slot[i]=wire[i];
    p->barrier(p->cookie);
    if(!ggfq_sample(p,&last) || last.tx_write!=first.tx_write)
        return ggfq_leave(q,p,GGFQ_LOST,1);
    /* Publication is irreversible. The callback may fail after the peer has
     * observed the new write index, so retain it on every later failure. */
    words[p->layout.tx_header/4+13]=next;
    p->barrier(p->cookie);
    if(!ggfp_live(p) || q->owner.signal(q->owner.cookie,p)!=1 || !ggfp_live(p))
        return ggfq_leave(q,p,GGFQ_QUARANTINED,1);
    return ggfq_leave(q,p,GGFQ_OK,0);
}

/* RX is an observation only, never host consumption. A syntactically valid
 * packet is not evidence that any fence completed or belongs to this host.
 * The caller must not infer ownership from this raw diagnostic payload.
 */
static inline int ggfq_rx_peek(struct ggfq_queue *q,struct ggfq_payload *out)
{
    if(!q || !out)return GGFQ_INVALID;
    const struct ggf_provider *p;
    int result=ggfq_enter(q,&p);
    if(result!=GGFQ_OK)return result;
    uintptr_t dest=(uintptr_t)out,queue=(uintptr_t)q;
    if(!ggfq_outside(p,out,sizeof(*out)) ||
       (dest<queue+sizeof(*q) && queue<dest+sizeof(*out)))
        return ggfq_leave(q,p,GGFQ_INVALID,0);
    struct ggf_observation first,last;
    if(!ggfq_sample(p,&first))return ggfq_leave(q,p,GGFQ_LOST,1);
    if(first.rx_read==first.rx_write)return ggfq_leave(q,p,GGFQ_EMPTY,0);
    const volatile uint32_t *words=(const volatile uint32_t *)p->memory;
    const volatile uint32_t *slot=words+p->layout.rx_payload/4+first.rx_read;
    uint32_t wire[GGFQ_DWORDS];struct ggfq_payload payload;
    for(unsigned i=0;i<GGFQ_DWORDS;i++)wire[i]=slot[i];
    p->barrier(p->cookie);
    if(!ggfq_decode(wire,&payload) || !ggfq_sample(p,&last) ||
       last.rx_read!=first.rx_read || last.rx_write!=first.rx_write)
        return ggfq_leave(q,p,GGFQ_LOST,1);
    *out=payload;return ggfq_leave(q,p,GGFQ_OK,0);
}
#endif
