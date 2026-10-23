/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * OS-independent SM8650 native audio backend. Original protocol implementation.
 * Register evidence: OnePlusOSS/android_kernel_oneplus_sm8650
 * d763ed47deec55451366c1187f813f2f309d88a7, drivers/mailbox/qcom-ipcc.c
 * and drivers/hwspinlock/qcom_hwspinlock.c; captured giulia device tree.
 * Serial worker only. No default lease producer exists: mappings and firmware
 * addresses alone cannot authorize power, memory ownership or DSP startup.
 */
#ifndef GIULIA_AUDIO_NATIVE_H
#define GIULIA_AUDIO_NATIVE_H
#include "giulia_audio_boot.h"
#include "giulia_audio_transport.h"

#define GAN_SMEM_PHYSICAL UINT64_C(0x81d00000)
#define GAN_SMEM_BYTES UINT32_C(0x200000)
#define GAN_MUTEX_PHYSICAL UINT64_C(0x1f43000)
#define GAN_IPCC_SEND UINT64_C(0x40600c)
#define GAN_SPANS_MAX 16u
#define GAN_LOCK_TIMEOUT_US 1000000u
enum { GAN_OK=0, GAN_ARGUMENT=-101, GAN_LEASE=-102, GAN_RANGE=-103,
    GAN_MAP=-104, GAN_IO=-105, GAN_TIMEOUT=-106, GAN_STATE=-107, GAN_FAULT=-108 };
enum { GAN_FAULT_FATAL=1u, GAN_FAULT_WATCHDOG=2u };
enum { GAN_PROOF_COLD_EPOCH=1u, GAN_PROOF_RESERVATIONS=2u,
    GAN_PROOF_POWER_CLOCKS=4u, GAN_PROOF_INTERCONNECT=8u,
    GAN_PROOF_SMEM_EXCLUSIVE=16u, GAN_PROOF_IPCC=32u,
    GAN_PROOF_REQUIRED=63u };
struct gan_span { uint64_t physical,bytes; };
struct gan_lease {
    uint64_t epoch;
    unsigned proofs,occupied_count;
    /* Authoritative kernel/boot/other live allocations. Include the actual
     * kernel span; zero entries cannot prove carveout non-overlap. */
    struct gan_span occupied[GAN_SPANS_MAX];
};
struct gan_ops {
    void *cookie;
    /* Return exactly 1 only after the platform proves a fresh stopped-ADSP
     * ownership epoch, all three memory reservations, held power/XO/AOP/ICC,
     * exclusive AP SMEM allocator serialization and IPCC access. Keep this
     * lease valid through boot/stop/quarantine. NEVER implement as return 1
     * from a terminal option, DT address, successful map, or ready bit. */
    int (*lease)(void *,struct gan_lease *);
    /* Required authoritative IRQ-latched status for this ownership epoch:
     * 0 = no fatal/watchdog, positive = GAN_FAULT_* bitmask, negative = read
     * failure. The platform must establish a real IRQ baseline before PAS,
     * latch events until a new verified epoch and retain its IRQ owner. This
     * callback must not clear hardware or return a constant success stub. */
    int (*latched_fault)(void *);
    volatile void *(*map_nc)(void *,uint64_t,size_t);
    void (*unmap_nc)(void *,volatile void *,size_t);
    int (*read32)(void *,uint64_t,uint32_t *); /* 0 success */
    int (*write32)(void *,uint64_t,uint32_t);  /* 0 success */
    void (*barrier)(void *);                  /* full device/shared barrier */
    int (*delay_us)(void *,unsigned);         /* 0 success; bounded worker wait */
    void (*packet)(void *,const uint8_t *,size_t); /* optional, non-reentrant */
};
struct gan_native {
    struct gan_ops ops;
    struct gan_lease lease;
    volatile uint8_t *smem,*dtb,*adsp;
    struct gat_smem partition;
    struct gat_smp2p_watch ready_watch;
    struct gat_transport transport;
    struct gat_transport *active_transport;
    uint64_t retired_epoch;
    unsigned mapped,prepared,authenticated,ready,hwlock_owned;
    uint32_t slave_state,slave_flags,fault_bits;
    int error;
};
static inline int gan_fail(struct gan_native *n,int error)
{
    n->error=error;n->ready=0;n->transport.error=error;
    n->transport.spf_ready=0;n->transport.spf_pending=0;
    if(n->active_transport) {
        n->active_transport->error=error;
        n->active_transport->spf_ready=0;n->active_transport->spf_pending=0;
    }
    return error;
}
static inline int gan_span_valid(uint64_t a,uint64_t n)
{ return n && a<=UINT64_MAX-n; }
static inline int gan_span_overlap(uint64_t a,uint64_t n,uint64_t b,uint64_t m)
{ return a<b+m && b<a+n; }
static inline int gan_lease_valid(const struct gan_lease *l)
{
    static const struct gan_span reserved[3]={
        {GAN_SMEM_PHYSICAL,GAN_SMEM_BYTES},{GAB_DTB_PHYSICAL,GAB_DTB_BYTES},
        {GAB_ADSP_PHYSICAL,GAB_ADSP_BYTES}};
    if(!l->epoch||(l->proofs&GAN_PROOF_REQUIRED)!=GAN_PROOF_REQUIRED||
       !l->occupied_count||l->occupied_count>GAN_SPANS_MAX)return GAN_LEASE;
    for(unsigned i=0;i<l->occupied_count;i++) {
        const struct gan_span *s=&l->occupied[i];
        if(!gan_span_valid(s->physical,s->bytes))return GAN_RANGE;
        for(unsigned j=0;j<3;j++)if(gan_span_overlap(s->physical,s->bytes,
            reserved[j].physical,reserved[j].bytes))return GAN_RANGE;
    }
    return GAN_OK;
}
static inline int gan_check_lease(struct gan_native *n)
{
    struct gan_lease l;int r;
    gat_zero(&l,sizeof(l));
    if(!n||!n->ops.lease||n->ops.lease(n->ops.cookie,&l)!=1)return GAN_LEASE;
    r=gan_lease_valid(&l);if(r)return r;
    if((n->lease.epoch&&n->lease.epoch!=l.epoch)||n->retired_epoch==l.epoch)return GAN_LEASE;
    n->lease=l;return GAN_OK;
}
/* Check at each native operation boundary, after lease validation and before
 * a write/ready/poll. This does not replace platform IRQ ownership or allow
 * an external GPR producer to skip checking the live native state. */
static inline int gan_check_live(struct gan_native *n)
{
    int r;
    if(!n||!n->ops.latched_fault)return GAN_ARGUMENT;
    if(n->error)return n->error;
    r=gan_check_lease(n);if(r)return r;
    r=n->ops.latched_fault(n->ops.cookie);
    if(r<0)return gan_fail(n,GAN_IO);
    if(r) {n->fault_bits|=(uint32_t)r;return gan_fail(n,GAN_FAULT);}
    return GAN_OK;
}
static inline int gan_init(struct gan_native *n,const struct gan_ops *o)
{
    if(!n||!o||!o->lease||!o->latched_fault||!o->map_nc||!o->unmap_nc||!o->read32||!o->write32||
       !o->barrier||!o->delay_us)return GAN_ARGUMENT;
    /* Once per state lifetime, never reset a quarantined/active native state. */
    gat_zero(n,sizeof(*n));n->ops=*o;return GAN_OK;
}
static inline void gan_unmap(struct gan_native *n)
{
    if(n->adsp)n->ops.unmap_nc(n->ops.cookie,n->adsp,GAB_ADSP_BYTES);
    if(n->dtb)n->ops.unmap_nc(n->ops.cookie,n->dtb,GAB_DTB_BYTES);
    if(n->smem)n->ops.unmap_nc(n->ops.cookie,n->smem,GAN_SMEM_BYTES);
    n->adsp=n->dtb=n->smem=0;n->mapped=0;
}
static inline int gan_inspect_items(const struct gat_smem *part)
{
    static const unsigned ids[5]={478,479,429,443,480};
    static const size_t sizes[5]={32,GAT_SMEM_TX_BYTES,GAT_SMP2P_BYTES,GAT_SMP2P_BYTES,0};
    size_t needed=0;
    for(unsigned i=0;i<5;i++) {
        volatile void *p;size_t bytes;int r=gat_smem_get(part,ids[i],&p,&bytes);
        if(r<0)return r;
        if(r==GAT_WAIT) {if(i<3)needed+=16+gat_align8(sizes[i]);continue;}
        if(sizes[i]&&bytes!=sizes[i])return GAT_INVALID;
        if((i==0||i==2||i==3)&&((uintptr_t)p&3))return GAT_INVALID;
        if(i==4&&(bytes<64||(bytes&7)||bytes>UINT32_MAX))return GAT_INVALID;
        if(i==3) {
            uint32_t state,flags;r=gat_smp2p_slave(p,bytes,&state,&flags);
            if(r<0)return r;
        }
    }
    size_t u=*(const volatile uint32_t *)(part->base+12),c=*(const volatile uint32_t *)(part->base+16);
    if(u>c||c>part->bytes)return GAT_INVALID;
    return needed<=c-u?GAN_OK:GAT_LIMIT;
}
/* Read-only preflight; no register or shared-memory writes. Repeated calls
 * recheck the authoritative lease and SMEM metadata after blocking allocations. */
static inline int gan_preflight(struct gan_native *n)
{
    struct gat_smem part;int r;
    if(!n||n->prepared||n->error)return GAN_STATE;
    r=gan_check_live(n);if(r)return r;
    if(!n->mapped) {
        n->smem=(volatile uint8_t *)n->ops.map_nc(n->ops.cookie,GAN_SMEM_PHYSICAL,GAN_SMEM_BYTES);
        if(!n->smem)goto failed_map;
        n->dtb=(volatile uint8_t *)n->ops.map_nc(n->ops.cookie,GAB_DTB_PHYSICAL,GAB_DTB_BYTES);
        if(!n->dtb)goto failed_map;
        n->adsp=(volatile uint8_t *)n->ops.map_nc(n->ops.cookie,GAB_ADSP_PHYSICAL,GAB_ADSP_BYTES);
        if(!n->adsp)goto failed_map;
        n->mapped=1;
    }
    n->ops.barrier(n->ops.cookie);
    r=gat_smem_resolve_partition(n->smem,GAN_SMEM_BYTES,2,&part);
    if(r!=GAT_OK) { gan_unmap(n);return r==GAT_WAIT?GAN_MAP:r; }
    r=gan_inspect_items(&part);if(r) {gan_unmap(n);return r;}
    n->partition=part;return GAN_OK;
failed_map:
    gan_unmap(n);return GAN_MAP;
}
static inline void gan_barrier(void *cookie)
{ struct gan_native *n=(struct gan_native *)cookie;n->ops.barrier(n->ops.cookie); }
static inline int gan_notify(struct gan_native *n,unsigned signal)
{
    int r;
    if(!n)return GAN_ARGUMENT;
    if(signal!=0&&signal!=2)return GAN_ARGUMENT;
    r=gan_check_live(n);if(r)return gan_fail(n,r);
    n->ops.barrier(n->ops.cookie);
    if(n->ops.write32(n->ops.cookie,GAN_IPCC_SEND,(3u<<16)|signal))return gan_fail(n,GAN_IO);
    n->ops.barrier(n->ops.cookie);return GAN_OK;
}
static inline void gan_kick(void *cookie)
{ (void)gan_notify((struct gan_native *)cookie,0); }
static inline void gan_packet(void *cookie,const uint8_t *data,size_t bytes)
{
    struct gan_native *n=(struct gan_native *)cookie;
    if(n->ops.packet)n->ops.packet(n->ops.cookie,data,bytes);
}
static inline int gan_lock(struct gan_native *n)
{
    uint32_t owner;int r;
    if(n->hwlock_owned)return GAN_STATE;
    r=gan_check_live(n);if(r)return gan_fail(n,r);
    /* An already AP-owned mutex is not evidence that this worker owns it. */
    if(n->ops.read32(n->ops.cookie,GAN_MUTEX_PHYSICAL,&owner))return GAN_IO;
    if(owner==1)return GAN_LEASE;
    for(unsigned elapsed=0;elapsed<GAN_LOCK_TIMEOUT_US;elapsed+=100) {
        r=gan_check_live(n);if(r)return gan_fail(n,r);
        /* An I/O failure after the write leaves acquisition uncertain. Keep
         * mappings/lease pinned rather than pretending no lock was taken. */
        n->hwlock_owned=2;
        if(n->ops.write32(n->ops.cookie,GAN_MUTEX_PHYSICAL,1)||
           n->ops.read32(n->ops.cookie,GAN_MUTEX_PHYSICAL,&owner))return GAN_IO;
        if(owner==1) { n->hwlock_owned=1;gan_barrier(n);return GAN_OK; }
        n->hwlock_owned=0;
        if(n->ops.delay_us(n->ops.cookie,100))return GAN_IO;
    }
    return GAN_TIMEOUT;
}
static inline int gan_unlock(struct gan_native *n)
{
    uint32_t owner;int r;
    if(!n->hwlock_owned)return GAN_STATE;
    r=gan_check_live(n);if(r)return gan_fail(n,r);
    gan_barrier(n);
    if(n->ops.read32(n->ops.cookie,GAN_MUTEX_PHYSICAL,&owner))return GAN_IO;
    if(owner!=1)return GAN_LEASE;
    if(n->ops.write32(n->ops.cookie,GAN_MUTEX_PHYSICAL,0))return GAN_IO;
    n->hwlock_owned=0;gan_barrier(n);return GAN_OK;
}
static inline int gan_slave_snapshot(struct gan_native *n,uint32_t *state,uint32_t *flags)
{
    volatile void *p;size_t bytes;int r;
    gan_barrier(n);r=gat_smem_get(&n->partition,443,&p,&bytes);
    if(r!=GAT_OK)return r;
    return gat_smp2p_slave(p,bytes,state,flags);
}
/* Before any PAS call: allocate only AP-owned items under the hardware lock,
 * retain DSP-owned items/indices, and capture the old SMP2P readiness baseline. */
static inline int gan_prepare(struct gan_native *n)
{
    static const unsigned ids[3]={478,479,429};
    static const size_t sizes[3]={32,GAT_SMEM_TX_BYTES,GAT_SMP2P_BYTES};
    volatile void *item[3];size_t bytes;uint32_t state=0,flags=0;int r,unlock_result;
    const struct gat_ops ops={gan_barrier,gan_kick,gan_packet};
    if(!n||!n->mapped||n->prepared||n->error)return GAN_STATE;
    r=gan_check_live(n);if(r)return r;
    n->prepared=1;r=gan_lock(n);if(r)return gan_fail(n,r);
    for(unsigned i=0;i<3;i++) {
        r=gan_check_live(n);if(r)goto out;
        r=gat_smem_alloc_locked(&n->partition,ids[i],sizes[i],gan_barrier,n);
        if(r!=GAT_OK)goto out;
        r=gat_smem_get(&n->partition,ids[i],&item[i],&bytes);
        if(r!=GAT_OK||bytes!=sizes[i]) { r=GAT_INVALID;goto out; }
    }
    r=gan_slave_snapshot(n,&state,&flags);
    if(r!=GAT_OK&&r!=GAT_WAIT)goto out;
    r=gat_smp2p_watch_capture(&n->ready_watch,r,state);if(r!=GAT_OK)goto out;
    n->slave_state=state;n->slave_flags=flags;
    r=gan_check_live(n);if(r)goto out;
    r=gat_init(&n->transport,item[0],32,item[1],GAT_SMEM_TX_BYTES,0,0,&ops,n,1);
    if(r!=GAT_OK)goto out;
    r=gan_check_live(n);if(r)goto out;
    r=gat_smp2p_init_outbound(item[2],GAT_SMP2P_BYTES,gan_barrier,n,1);
out:
    unlock_result=gan_unlock(n);
    if(unlock_result)return gan_fail(n,unlock_result);
    if(r!=GAT_OK)return gan_fail(n,r);
    return gan_notify(n,2);
}
/* Invoke only after synchronous PAS AUTH(1) returned transport AND secure
 * success. PAS AUTH(36), timeout, or a ready bit must never call this method. */
static inline int gan_auth_complete(struct gan_native *n)
{
    int r;
    if(!n||!n->prepared||n->authenticated||n->error)return GAN_STATE;
    r=gan_check_live(n);if(r)return gan_fail(n,r);
    r=gat_smp2p_watch_arm(&n->ready_watch);if(r!=GAT_OK)return gan_fail(n,r);
    n->authenticated=1;return GAN_OK;
}
static inline int gan_ready_sample(struct gan_native *n)
{
    uint32_t state=0,flags=0;int r;
    if(!n||!n->authenticated||n->error)return n&&n->error?n->error:GAN_STATE;
    r=gan_check_live(n);if(r)return gan_fail(n,r);
    r=gan_slave_snapshot(n,&state,&flags);
    r=gat_smp2p_watch_observe(&n->ready_watch,r,state);
    n->slave_state=state;n->slave_flags=flags;
    if(r<0)return gan_fail(n,r);
    n->ready=r==GAT_OK;return r;
}
static inline int gan_wait_ready(struct gan_native *n,unsigned timeout_ms)
{
    if(!timeout_ms||timeout_ms>GAB_READY_TIMEOUT_MS)return GAN_ARGUMENT;
    for(unsigned elapsed=0;;elapsed++) {
        int r=gan_ready_sample(n);if(r==GAT_OK)return GAN_OK;if(r<0)return r;
        if(elapsed==timeout_ms)return gan_fail(n,GAN_TIMEOUT);
        if(n->ops.delay_us(n->ops.cookie,1000))return gan_fail(n,GAN_IO);
    }
}
/* Adapter for gar_ops.ipc_open. Bind once to the runtime-owned transport after
 * fresh ready; the runtime may then send GPR directly through that same object.
 * Its lifetime must cover the entire native epoch, including quarantine. */
static inline int gan_ipc_open(struct gan_native *n,struct gat_transport *target)
{
    int r;
    if(!n||!target)return GAN_ARGUMENT;
    r=gan_ready_sample(n);if(r!=GAT_OK)return r;
    if(n->active_transport)return n->active_transport==target?GAT_OK:GAN_STATE;
    if(n->transport.started)return GAN_STATE;
    *target=n->transport;n->active_transport=target;return GAT_OK;
}
/* Returns GAT_OK/GAT_WAIT or a negative error. RX is allocated by ADSP and may
 * appear after PAS/ready. Never allocate 480 or modify its producer index. */
static inline int gan_transport_poll(struct gan_native *n,unsigned budget)
{
    volatile void *rx;size_t bytes;int r;struct gat_transport *t;
    if(!budget)return GAN_ARGUMENT;
    r=gan_ready_sample(n);if(r!=GAT_OK)return r;
    t=n->active_transport?n->active_transport:&n->transport;
    if(!t->rx.fifo) {
        gan_barrier(n);r=gat_smem_get(&n->partition,GAT_SMEM_RX,&rx,&bytes);
        if(r<0)return gan_fail(n,r);
        if(r==GAT_OK) { r=gat_attach_rx(t,rx,bytes);if(r<0)return gan_fail(n,r); }
    }
    r=gan_check_live(n);if(r)return gan_fail(n,r);
    r=gat_start(t);if(n->error)return n->error;
    if(r!=GAT_OK)return r;
    r=gan_check_live(n);if(r)return gan_fail(n,r);
    r=gat_poll(t,budget);
    if(n->error)return n->error;
    if(r<0)return gan_fail(n,r);
    return r;
}
static inline volatile uint8_t *gan_destination(struct gan_native *n,uint64_t pa,size_t bytes)
{
    if(!gan_span_valid(pa,bytes))return 0;
    if(pa>=GAB_DTB_PHYSICAL&&pa-GAB_DTB_PHYSICAL<=GAB_DTB_BYTES&&
       bytes<=GAB_DTB_BYTES-(pa-GAB_DTB_PHYSICAL))return n->dtb+(size_t)(pa-GAB_DTB_PHYSICAL);
    if(pa>=GAB_ADSP_PHYSICAL&&pa-GAB_ADSP_PHYSICAL<=GAB_ADSP_BYTES&&
       bytes<=GAB_ADSP_BYTES-(pa-GAB_ADSP_PHYSICAL))return n->adsp+(size_t)(pa-GAB_ADSP_PHYSICAL);
    return 0;
}
static inline int gan_memory_write(struct gan_native *n,uint64_t pa,const void *source,size_t bytes)
{
    volatile uint8_t *dst;const uint8_t *src=(const uint8_t *)source;int r;
    if(!n||!n->prepared||n->authenticated||n->error)return GAN_STATE;
    r=gan_check_live(n);if(r)return gan_fail(n,r);
    dst=gan_destination(n,pa,bytes);if(!dst)return GAN_RANGE;
    for(size_t i=0;i<bytes;i++)dst[i]=src?src[i]:0;
    return GAN_OK;
}
static inline int gan_memory_sync(struct gan_native *n,uint64_t pa,size_t bytes)
{
    int r;
    if(!n||!n->prepared||n->authenticated||n->error)return GAN_STATE;
    r=gan_check_live(n);if(r)return gan_fail(n,r);
    if(!gan_destination(n,pa,bytes))return GAN_RANGE;
    /* map_nc contract excludes cacheable aliases. Cached mappings would need
     * an actual architecture cache-clean operation, not this barrier. */
    gan_barrier(n);return GAN_OK;
}
/* Invoke only after both PAS SHUTDOWN(1) and SHUTDOWN(36) succeeded and IPC
 * clients are quiesced. A failed/uncertain shutdown must retain all mappings. */
static inline int gan_shutdown_complete(struct gan_native *n)
{
    if(!n||!n->authenticated||n->hwlock_owned)return GAN_STATE;
    n->authenticated=0;n->ready=0;n->transport.spf_ready=0;n->error=0;
    if(n->active_transport)n->active_transport->spf_ready=0;
    return GAN_OK;
}
static inline int gan_release(struct gan_native *n)
{
    if(!n||n->authenticated||n->hwlock_owned||n->error)return GAN_STATE;
    gan_unmap(n);n->retired_epoch=n->lease.epoch;n->lease.epoch=0;
    n->prepared=0;n->ready=0;n->active_transport=0;return GAN_OK;
}
#endif
