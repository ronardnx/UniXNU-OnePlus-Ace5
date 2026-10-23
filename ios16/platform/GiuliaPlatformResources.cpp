/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * The single kernel resource ledger. initialize() is process-context only.
 * All other operations allocate nothing and use an interrupt-disabled spin
 * lock around bounded ledger work. No hardware/provider call holds the lock.
 */
#include <pexpert/arm64/board_config.h>
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#ifndef UINT64_C
#define UINT64_C(x) x##ULL
#endif
#ifndef UINT64_MAX
#define UINT64_MAX (~(uint64_t)0)
#endif
#include "giulia_platform_resources.h"

static struct gpr_broker platformResources;
static IOSimpleLock *platformResourcesLock;

static IOSimpleLock *platformLock(void)
{
    return __atomic_load_n(&platformResourcesLock,__ATOMIC_ACQUIRE);
}

extern "C" int gpr_global_initialize(void)
{
    if(platformLock())return GPR_OK;
    /* IOSimpleLockAlloc may block: this explicit entry is called only from
     * service initialization, never from owns(), an IRQ or an atomic path. */
    IOSimpleLock *candidate=IOSimpleLockAlloc();
    if(!candidate)return GPR_CAPACITY;
    IOSimpleLock *empty=NULL;
    if(!__atomic_compare_exchange_n(&platformResourcesLock,&empty,candidate,false,
        __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))IOSimpleLockFree(candidate);
    return GPR_OK;
}

extern "C" const char *giulia_platform_resources_identity(void)
{ return "GIULIA PLATFORM: resource broker ABI=1;"; }

#define GPR_LOCKED(expression,failure) \
    IOSimpleLock *lock=platformLock(); \
    if(!lock)return failure; \
    IOInterruptState irqState=IOSimpleLockLockDisableInterrupt(lock); \
    int result=(expression); \
    IOSimpleLockUnlockEnableInterrupt(lock,irqState); \
    return result

extern "C" int gpr_global_open_epoch(const void *owner,uint64_t epoch)
{ GPR_LOCKED(gpr_open_epoch(&platformResources,owner,epoch),GPR_CAPACITY); }
extern "C" int gpr_global_retire_epoch(const void *owner,uint64_t epoch)
{ GPR_LOCKED(gpr_retire_epoch(&platformResources,owner,epoch),GPR_CAPACITY); }
extern "C" int gpr_global_claim(const struct gpr_key *key,const void *owner,uint64_t epoch,
    unsigned flags,uint64_t vote,struct gpr_token *token)
{ GPR_LOCKED(gpr_claim(&platformResources,key,owner,epoch,flags,vote,token),GPR_CAPACITY); }
extern "C" int gpr_global_owns(const struct gpr_token *token,const void *owner,uint64_t epoch)
{ GPR_LOCKED(gpr_owns(&platformResources,token,owner,epoch),0); }
extern "C" int gpr_global_release(const struct gpr_token *token,const void *owner,uint64_t epoch)
{ GPR_LOCKED(gpr_release(&platformResources,token,owner,epoch),GPR_CAPACITY); }
extern "C" int gpr_global_mark_dma(const struct gpr_token *token,const void *owner,uint64_t epoch)
{ GPR_LOCKED(gpr_mark_dma(&platformResources,token,owner,epoch),GPR_CAPACITY); }
extern "C" int gpr_global_quiesce(const struct gpr_token *token,const void *owner,uint64_t epoch)
{ GPR_LOCKED(gpr_quiesce(&platformResources,token,owner,epoch),GPR_CAPACITY); }
extern "C" int gpr_global_quarantine(const struct gpr_token *token,const void *owner,uint64_t epoch)
{ GPR_LOCKED(gpr_quarantine(&platformResources,token,owner,epoch),GPR_CAPACITY); }
extern "C" int gpr_global_transfer_begin(const struct gpr_token *token,const void *owner,uint64_t epoch,unsigned domain)
{ GPR_LOCKED(gpr_transfer_begin(&platformResources,token,owner,epoch,domain),GPR_CAPACITY); }
extern "C" int gpr_global_transfer_complete(const struct gpr_token *token,const void *owner,uint64_t epoch,unsigned outcome)
{ GPR_LOCKED(gpr_transfer_complete(&platformResources,token,owner,epoch,outcome),GPR_CAPACITY); }
extern "C" int gpr_global_vote_update(const struct gpr_token *token,const void *owner,uint64_t epoch,uint64_t vote,uint64_t *total)
{ GPR_LOCKED(gpr_vote_update(&platformResources,token,owner,epoch,vote,total),GPR_CAPACITY); }
extern "C" int gpr_global_aggregate(const struct gpr_key *key,uint64_t *total)
{ GPR_LOCKED(gpr_aggregate(&platformResources,key,total),GPR_CAPACITY); }
#undef GPR_LOCKED

static unsigned legacyKeys(unsigned client,struct gpr_key keys[2],unsigned *flags)
{
    if(client<1 || client>3)return 0;
    const struct gpr_key mproc={GPR_IPCC,GPR_IPCC_MPROC_DOMAIN,0,0,UINT64_C(0x406000),UINT64_C(0x1000)};
    keys[0]=mproc;*flags=0;
    if(client==1){*flags=GPR_IPCC_SHARED_ROUTE;return 1;} /* Transactions use the exclusive GPR_QMP lease. */
    if(client==2) {
        keys[0].id=3;keys[1]=keys[0];keys[1].subid=2;
        return 2; /* ADSP GLINK and SMP2P, distinct from modem client 2. */
    }
    keys[0].base=UINT64_C(0x400000);keys[0].bytes=UINT64_C(0x140000);
    *flags=GPR_IPCC_APERTURE;return 1; /* Complete existing GMU IPC mapping. */
}

static int legacyToken(const struct gpr_key *key,const void *owner,struct gpr_token *token)
{
    for(unsigned i=0;i<GPR_MAX_LEASES;i++) {
        const struct gpr_lease *lease=&platformResources.leases[i];
        if(lease->state && lease->owner==owner && lease->epoch==1 && gpr_key_equal(key,&lease->key)) {
            token->slot=i;token->serial=lease->serial;return 1;
        }
    }
    return 0;
}

extern "C" int gpr_global_legacy_ipcc_claim(unsigned client,const void *owner)
{
    struct gpr_key keys[2];struct gpr_token tokens[2];unsigned flags=0;
    unsigned count=legacyKeys(client,keys,&flags);
    if(!count || !owner)return 0;
    IOSimpleLock *lock=platformLock();if(!lock)return 0;
    IOInterruptState irqState=IOSimpleLockLockDisableInterrupt(lock);
    int result=gpr_open_epoch(&platformResources,owner,1);
    unsigned acquired=0;
    /* Unlike generic idempotent claims, a legacy claim must be new. */
    for(unsigned i=0;!result && i<count;i++)
        if(legacyToken(&keys[i],owner,&tokens[i]))result=GPR_CONFLICT;
    for(unsigned i=0;!result && i<count;i++) {
        result=gpr_claim(&platformResources,&keys[i],owner,1,flags,0,&tokens[i]);
        if(!result)acquired++;
    }
    if(result)while(acquired) {
        acquired--;(void)gpr_release(&platformResources,&tokens[acquired],owner,1);
    }
    IOSimpleLockUnlockEnableInterrupt(lock,irqState);return result==GPR_OK;
}

extern "C" int gpr_global_legacy_ipcc_owns(unsigned client,const void *owner)
{
    struct gpr_key keys[2];struct gpr_token token;unsigned flags=0;
    unsigned count=legacyKeys(client,keys,&flags);
    if(!count || !owner)return 0;
    IOSimpleLock *lock=platformLock();if(!lock)return 0;
    IOInterruptState irqState=IOSimpleLockLockDisableInterrupt(lock);int result=1;
    for(unsigned i=0;i<count;i++)if(!legacyToken(&keys[i],owner,&token) ||
        !gpr_owns(&platformResources,&token,owner,1))result=0;
    IOSimpleLockUnlockEnableInterrupt(lock,irqState);return result;
}

extern "C" int gpr_global_legacy_ipcc_release(unsigned client,const void *owner)
{
    struct gpr_key keys[2];struct gpr_token tokens[2];unsigned flags=0;
    unsigned count=legacyKeys(client,keys,&flags);
    if(!count || !owner)return 0;
    IOSimpleLock *lock=platformLock();if(!lock)return 0;
    IOInterruptState irqState=IOSimpleLockLockDisableInterrupt(lock);int result=1;
    /* Validate the whole bundle before mutating either audio channel. */
    for(unsigned i=0;i<count;i++) {
        if(!legacyToken(&keys[i],owner,&tokens[i]) ||
           !gpr_owns(&platformResources,&tokens[i],owner,1)) {result=0;continue;}
        const struct gpr_lease *lease=&platformResources.leases[tokens[i].slot];
        if(lease->dma_active || lease->access_domain!=GPR_LOCAL_DOMAIN)result=0;
    }
    if(result)for(unsigned i=0;i<count;i++)
        (void)gpr_release(&platformResources,&tokens[i],owner,1);
    IOSimpleLockUnlockEnableInterrupt(lock,irqState);return result;
}
#endif
