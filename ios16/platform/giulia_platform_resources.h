/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Bounded platform resource ledger, ABI 1. No MMIO, SCM or allocator calls.
 * The caller serializes every operation; the kernel wrappers below use one
 * global lock. A lease records software exclusion, not proof of hardware
 * readiness. Never initialize a second ledger for another device driver.
 */
#ifndef GIULIA_PLATFORM_RESOURCES_H
#define GIULIA_PLATFORM_RESOURCES_H
#include <stddef.h>
#include <stdint.h>
#ifndef UINT64_MAX
#define UINT64_MAX (~(uint64_t)0)
#endif

#define GPR_ABI 1u
#define GPR_MAX_LEASES 96u
#define GPR_MAX_OWNERS 32u
#define GPR_LOCAL_DOMAIN 0u
/* Software namespace for the captured 0x00406000 MPROC controller. */
#define GPR_IPCC_MPROC_DOMAIN 0u
enum gpr_kind {
    GPR_MMIO=1, GPR_MEMORY, GPR_IPCC, GPR_QMP, GPR_SCM,
    GPR_DMA_STREAM, GPR_DMA_BUFFER, GPR_SMEM_ITEM, GPR_HWLOCK,
    GPR_VOTE_MAX, GPR_VOTE_SUM
};
enum { GPR_IPCC_APERTURE=1u, GPR_IPCC_SHARED_ROUTE=2u };
enum gpr_result {
    GPR_OK=0, GPR_ARGUMENT, GPR_CAPACITY, GPR_CONFLICT, GPR_STALE,
    GPR_NOT_OWNED, GPR_DMA_ACTIVE, GPR_REMOTE, GPR_QUARANTINED,
    GPR_PENDING, GPR_OVERFLOW, GPR_UNCERTAIN, GPR_BUSY
};
enum { GPR_TRANSFER_DONE=0, GPR_TRANSFER_UNCHANGED=1, GPR_TRANSFER_UNCERTAIN=2 };
enum { GPR_FREE=0, GPR_HELD=1, GPR_TRANSFERRING=2, GPR_ISOLATED=3 };
struct gpr_key {
    uint32_t kind,domain,id,subid;
    uint64_t base,bytes;
};
struct gpr_token { unsigned slot; uint64_t serial; };
struct gpr_epoch { const void *owner; uint64_t epoch; unsigned active; };
struct gpr_lease {
    struct gpr_key key;
    const void *owner;
    uint64_t epoch,serial,vote;
    unsigned flags,state,dma_active,access_domain,target_domain;
};
struct gpr_broker {
    uint64_t serial;
    struct gpr_epoch owners[GPR_MAX_OWNERS];
    struct gpr_lease leases[GPR_MAX_LEASES];
};

/* Named key conventions:
 * IPCC: domain=protocol, id=client, subid=signal; base/bytes name the
 * complete controller aperture. APERTURE ignores channel numbers and
 * excludes every overlapping host channel/MMIO claim, including GMU aliases.
 * DMA_STREAM: base=SMMU physical address, domain=SMMU instance, id=SID,
 * subid=SMR mask. Overlapping masked stream matches are exclusive.
 * SMEM_ITEM: domain=remote host, id=item, subid=0, base=bytes=0. These logical
 * names grant no physical ownership. Actual allocation also requires the
 * shared hardware lock and validated private partition bounds.
 * HWLOCK: base=controller, id=hardware lock index; domain identifies controller.
 * QMP: base/bytes=one shared mailbox, id=subid=0; one transaction at a time.
 * SCM: one canonical all-zero key except kind; exclusive across services.
 * VOTE_*: domain=provider, id=resource, subid=vote class, base=bytes=0.
 * Vote aggregation computes desired state only. Hold the actual provider's
 * QMP/SCM transaction through completion before applying/withdrawing a vote;
 * never apply a stale aggregate snapshot after another owner's update.
 */
static inline int gpr_is_vote(unsigned kind)
{ return kind==GPR_VOTE_MAX || kind==GPR_VOTE_SUM; }
static inline int gpr_is_physical(unsigned kind)
{ return kind==GPR_MMIO || kind==GPR_MEMORY || kind==GPR_IPCC || kind==GPR_DMA_BUFFER; }
static inline int gpr_key_valid(const struct gpr_key *k,unsigned flags)
{
    if(!k || k->kind<GPR_MMIO || k->kind>GPR_VOTE_SUM ||
       (flags && (k->kind!=GPR_IPCC || (flags!=GPR_IPCC_APERTURE && flags!=GPR_IPCC_SHARED_ROUTE))))return 0;
    /* Only the APSS -> AOP route is shared. Every MMIO transaction on this
     * route additionally requires the single canonical GPR_QMP lease. */
    if(flags==GPR_IPCC_SHARED_ROUTE && (k->domain || k->id || k->subid ||
       k->base!=0x406000u || k->bytes!=0x1000u))return 0;
    if(k->bytes && k->base>UINT64_MAX-k->bytes)return 0;
    if(gpr_is_physical(k->kind) && (!k->base || !k->bytes))return 0;
    if(k->kind==GPR_IPCC && (k->id>65535u || k->subid>65535u))return 0;
    if(k->kind==GPR_DMA_STREAM && (!k->base || k->bytes || k->id>65535u ||
       k->subid>65535u || (k->id&k->subid)))return 0;
    if(k->kind==GPR_SMEM_ITEM && (k->domain>65535u || k->base || k->bytes || k->subid))return 0;
    if(k->kind==GPR_HWLOCK && (!k->base || k->bytes || k->subid))return 0;
    if(k->kind==GPR_QMP && (!k->base || !k->bytes || k->id || k->subid))return 0;
    if(k->kind==GPR_SCM && (k->domain || k->id || k->subid || k->base || k->bytes))return 0;
    if(gpr_is_vote(k->kind) && (k->base || k->bytes))return 0;
    return 1;
}
static inline int gpr_key_equal(const struct gpr_key *a,const struct gpr_key *b)
{ return a->kind==b->kind && a->domain==b->domain && a->id==b->id &&
    a->subid==b->subid && a->base==b->base && a->bytes==b->bytes; }
static inline int gpr_range_overlap(const struct gpr_key *a,const struct gpr_key *b)
{ return a->bytes && b->bytes && a->base<b->base+b->bytes && b->base<a->base+a->bytes; }
static inline int gpr_vote_resource(const struct gpr_key *a,const struct gpr_key *b)
{ return gpr_is_vote(a->kind) && gpr_is_vote(b->kind) && a->domain==b->domain &&
    a->id==b->id && a->subid==b->subid; }
static inline int gpr_conflicts(const struct gpr_key *a,unsigned af,const struct gpr_key *b,unsigned bf)
{
    if(gpr_is_physical(a->kind) && gpr_is_physical(b->kind) && gpr_range_overlap(a,b)) {
        if(a->kind==GPR_IPCC && b->kind==GPR_IPCC && af!=GPR_IPCC_APERTURE && bf!=GPR_IPCC_APERTURE &&
           a->base==b->base && a->bytes==b->bytes && a->domain==b->domain) {
            if(a->id!=b->id || a->subid!=b->subid)return 0;
            return af!=GPR_IPCC_SHARED_ROUTE || bf!=GPR_IPCC_SHARED_ROUTE;
        }
        return 1;
    }
    if(gpr_vote_resource(a,b))return a->kind!=b->kind;
    if(a->kind==GPR_DMA_STREAM && b->kind==GPR_DMA_STREAM && a->base==b->base)
        return !((a->id^b->id)&~(a->subid|b->subid)&65535u);
    if(a->kind==GPR_HWLOCK && b->kind==GPR_HWLOCK && a->base==b->base)
        return a->id==b->id;
    if(a->kind==GPR_QMP && b->kind==GPR_QMP && gpr_range_overlap(a,b))return 1;
    return gpr_key_equal(a,b) && !gpr_is_vote(a->kind);
}
static inline int gpr_epoch_live(const struct gpr_broker *b,const void *owner,uint64_t epoch)
{
    if(!b || !owner || !epoch)return 0;
    for(unsigned i=0;i<GPR_MAX_OWNERS;i++)
        if(b->owners[i].owner==owner)return b->owners[i].active && b->owners[i].epoch==epoch;
    return 0;
}
static inline int gpr_open_epoch(struct gpr_broker *b,const void *owner,uint64_t epoch)
{
    unsigned free_slot=GPR_MAX_OWNERS;
    if(!b || !owner || !epoch)return GPR_ARGUMENT;
    for(unsigned i=0;i<GPR_MAX_OWNERS;i++) {
        if(!b->owners[i].owner && free_slot==GPR_MAX_OWNERS)free_slot=i;
        if(b->owners[i].owner!=owner)continue;
        if(b->owners[i].active && b->owners[i].epoch==epoch)return GPR_OK;
        if(epoch<=b->owners[i].epoch)return GPR_STALE;
        for(unsigned j=0;j<GPR_MAX_LEASES;j++)
            if(b->leases[j].state && b->leases[j].owner==owner)return GPR_BUSY;
        b->owners[i].epoch=epoch;b->owners[i].active=1;return GPR_OK;
    }
    if(free_slot==GPR_MAX_OWNERS)return GPR_CAPACITY;
    b->owners[free_slot].owner=owner;b->owners[free_slot].epoch=epoch;
    b->owners[free_slot].active=1;return GPR_OK;
}
static inline int gpr_retire_epoch(struct gpr_broker *b,const void *owner,uint64_t epoch)
{
    if(!gpr_epoch_live(b,owner,epoch))return GPR_STALE;
    for(unsigned i=0;i<GPR_MAX_LEASES;i++)
        if(b->leases[i].state && b->leases[i].owner==owner)return GPR_BUSY;
    for(unsigned i=0;i<GPR_MAX_OWNERS;i++)if(b->owners[i].owner==owner)b->owners[i].active=0;
    return GPR_OK;
}
static inline int gpr_token_valid(const struct gpr_broker *b,const struct gpr_token *t,
    const void *owner,uint64_t epoch)
{
    return b && t && t->slot<GPR_MAX_LEASES && t->serial && gpr_epoch_live(b,owner,epoch) &&
        b->leases[t->slot].state && b->leases[t->slot].owner==owner &&
        b->leases[t->slot].epoch==epoch && b->leases[t->slot].serial==t->serial;
}
/* owns means an active software lease; it never authorizes CPU access to
 * remotely transferred memory. Inspect access_domain separately. */
static inline int gpr_owns(const struct gpr_broker *b,const struct gpr_token *t,
    const void *owner,uint64_t epoch)
{ return gpr_token_valid(b,t,owner,epoch) && b->leases[t->slot].state==GPR_HELD; }
static inline int gpr_aggregate(const struct gpr_broker *b,const struct gpr_key *key,uint64_t *value)
{
    uint64_t total=0;
    if(!b || !value || !gpr_key_valid(key,0) || !gpr_is_vote(key->kind))return GPR_ARGUMENT;
    for(unsigned i=0;i<GPR_MAX_LEASES;i++) {
        const struct gpr_lease *l=&b->leases[i];
        if(!l->state || !gpr_key_equal(key,&l->key))continue;
        if(key->kind==GPR_VOTE_MAX) {if(l->vote>total)total=l->vote;}
        else {if(l->vote>UINT64_MAX-total)return GPR_OVERFLOW;total+=l->vote;}
    }
    *value=total;return GPR_OK;
}
static inline int gpr_claim(struct gpr_broker *b,const struct gpr_key *key,
    const void *owner,uint64_t epoch,unsigned flags,uint64_t vote,struct gpr_token *token)
{
    unsigned free_slot=GPR_MAX_LEASES;uint64_t total=0;
    if(!b || !token || !gpr_key_valid(key,flags) || (!gpr_is_vote(key->kind) && vote))return GPR_ARGUMENT;
    if(!gpr_epoch_live(b,owner,epoch))return GPR_STALE;
    for(unsigned i=0;i<GPR_MAX_LEASES;i++) {
        const struct gpr_lease *l=&b->leases[i];
        if(!l->state) {if(free_slot==GPR_MAX_LEASES)free_slot=i;continue;}
        if(l->owner==owner && l->epoch==epoch && gpr_key_equal(key,&l->key)) {
            if(l->state!=GPR_HELD)return l->state==GPR_ISOLATED?GPR_QUARANTINED:GPR_PENDING;
            if(key->kind==GPR_QMP || key->kind==GPR_SCM || key->kind==GPR_HWLOCK)return GPR_BUSY;
            if(l->flags!=flags || l->vote!=vote)return GPR_CONFLICT;
            token->slot=i;token->serial=l->serial;return GPR_OK;
        }
        if(gpr_conflicts(key,flags,&l->key,l->flags))return GPR_CONFLICT;
    }
    if(free_slot==GPR_MAX_LEASES)return GPR_CAPACITY;
    if(b->serial==UINT64_MAX)return GPR_OVERFLOW;
    if(key->kind==GPR_VOTE_SUM) {
        int result=gpr_aggregate(b,key,&total);
        if(result)return result;
        if(vote>UINT64_MAX-total)return GPR_OVERFLOW;
    }
    struct gpr_lease *l=&b->leases[free_slot];
    l->key=*key;l->owner=owner;l->epoch=epoch;l->serial=++b->serial;l->vote=vote;
    l->flags=flags;l->state=GPR_HELD;l->dma_active=0;
    l->access_domain=GPR_LOCAL_DOMAIN;l->target_domain=GPR_LOCAL_DOMAIN;
    token->slot=free_slot;token->serial=l->serial;return GPR_OK;
}
static inline int gpr_release(struct gpr_broker *b,const struct gpr_token *t,const void *owner,uint64_t epoch)
{
    if(!gpr_token_valid(b,t,owner,epoch))return GPR_NOT_OWNED;
    struct gpr_lease *l=&b->leases[t->slot];
    if(l->state==GPR_ISOLATED)return GPR_QUARANTINED;
    if(l->state==GPR_TRANSFERRING)return GPR_PENDING;
    if(l->dma_active)return GPR_DMA_ACTIVE;
    if(l->access_domain!=GPR_LOCAL_DOMAIN)return GPR_REMOTE;
    l->state=GPR_FREE;l->owner=NULL;return GPR_OK;
}
static inline int gpr_mark_dma(struct gpr_broker *b,const struct gpr_token *t,const void *owner,uint64_t epoch)
{
    if(!gpr_owns(b,t,owner,epoch))return GPR_NOT_OWNED;
    unsigned kind=b->leases[t->slot].key.kind;
    if(kind!=GPR_MEMORY && kind!=GPR_DMA_BUFFER && kind!=GPR_DMA_STREAM && kind!=GPR_IPCC)return GPR_ARGUMENT;
    if(b->leases[t->slot].dma_active)return GPR_BUSY;
    b->leases[t->slot].dma_active=1;return GPR_OK;
}
/* Call only after the adapter observed real DMA drain/reset completion for
 * this exact epoch. The ledger cannot manufacture a hardware observation. */
static inline int gpr_quiesce(struct gpr_broker *b,const struct gpr_token *t,const void *owner,uint64_t epoch)
{
    if(!gpr_owns(b,t,owner,epoch))return GPR_NOT_OWNED;
    b->leases[t->slot].dma_active=0;return GPR_OK;
}
static inline int gpr_quarantine(struct gpr_broker *b,const struct gpr_token *t,const void *owner,uint64_t epoch)
{
    if(!gpr_token_valid(b,t,owner,epoch))return GPR_NOT_OWNED;
    b->leases[t->slot].state=GPR_ISOLATED;return GPR_OK;
}
/* Transfer keeps the reservation with the same software owner. Domain is
 * the recipient hardware/security world, not a new driver identity. The
 * complete original range stays excluded after partial/unknown assignment. */
static inline int gpr_transfer_begin(struct gpr_broker *b,const struct gpr_token *t,
    const void *owner,uint64_t epoch,unsigned target_domain)
{
    if(!gpr_owns(b,t,owner,epoch))return GPR_NOT_OWNED;
    struct gpr_lease *l=&b->leases[t->slot];
    if(l->key.kind!=GPR_MEMORY && l->key.kind!=GPR_DMA_BUFFER)return GPR_ARGUMENT;
    if(l->dma_active)return GPR_DMA_ACTIVE;
    if(target_domain==l->access_domain)return GPR_ARGUMENT;
    l->target_domain=target_domain;l->state=GPR_TRANSFERRING;return GPR_OK;
}
static inline int gpr_transfer_complete(struct gpr_broker *b,const struct gpr_token *t,
    const void *owner,uint64_t epoch,unsigned outcome)
{
    if(!gpr_token_valid(b,t,owner,epoch))return GPR_NOT_OWNED;
    struct gpr_lease *l=&b->leases[t->slot];
    if(l->state!=GPR_TRANSFERRING || outcome>GPR_TRANSFER_UNCERTAIN)return GPR_ARGUMENT;
    if(outcome==GPR_TRANSFER_UNCERTAIN) {l->state=GPR_ISOLATED;return GPR_UNCERTAIN;}
    if(outcome==GPR_TRANSFER_DONE)l->access_domain=l->target_domain;
    l->target_domain=l->access_domain;l->state=GPR_HELD;return GPR_OK;
}
static inline int gpr_vote_update(struct gpr_broker *b,const struct gpr_token *t,
    const void *owner,uint64_t epoch,uint64_t vote,uint64_t *aggregate)
{
    uint64_t total=0;
    if(!aggregate || !gpr_owns(b,t,owner,epoch))return GPR_NOT_OWNED;
    struct gpr_lease *l=&b->leases[t->slot];
    if(!gpr_is_vote(l->key.kind))return GPR_ARGUMENT;
    int result=gpr_aggregate(b,&l->key,&total);
    if(result)return result;
    if(l->key.kind==GPR_VOTE_SUM && vote>UINT64_MAX-(total-l->vote))return GPR_OVERFLOW;
    l->vote=vote;return gpr_aggregate(b,&l->key,aggregate);
}

#ifdef __cplusplus
extern "C" {
#endif
const char *giulia_platform_resources_identity(void);
/* Process-context bootstrap before publishing any broker client. This is
 * the only allocating wrapper. Every other wrapper is bounded/non-allocating;
 * owns before initialization returns false and never creates a lock. */
int gpr_global_initialize(void);
int giulia_platform_qmp_available(void);
int gpr_global_open_epoch(const void *,uint64_t);
int gpr_global_retire_epoch(const void *,uint64_t);
int gpr_global_claim(const struct gpr_key *,const void *,uint64_t,unsigned,uint64_t,struct gpr_token *);
int gpr_global_owns(const struct gpr_token *,const void *,uint64_t);
int gpr_global_release(const struct gpr_token *,const void *,uint64_t);
int gpr_global_mark_dma(const struct gpr_token *,const void *,uint64_t);
int gpr_global_quiesce(const struct gpr_token *,const void *,uint64_t);
int gpr_global_quarantine(const struct gpr_token *,const void *,uint64_t);
int gpr_global_transfer_begin(const struct gpr_token *,const void *,uint64_t,unsigned);
int gpr_global_transfer_complete(const struct gpr_token *,const void *,uint64_t,unsigned);
int gpr_global_vote_update(const struct gpr_token *,const void *,uint64_t,uint64_t,uint64_t *);
int gpr_global_aggregate(const struct gpr_key *,uint64_t *);
/* Compatibility bridge clients: QMP=1, audio=2, GPU aperture=3. Only the
 * bridge uses boot-lifetime epoch 1; modem adapters use their real epoch. */
int gpr_global_legacy_ipcc_claim(unsigned,const void *);
int gpr_global_legacy_ipcc_owns(unsigned,const void *);
int gpr_global_legacy_ipcc_release(unsigned,const void *);
#ifdef __cplusplus
}
#endif
#endif
