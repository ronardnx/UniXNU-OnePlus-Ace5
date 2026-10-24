/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Pure HFI allocation decisions. NLH_CALL carries a bridge ticket, never a
 * pointer; only framework EXPORT/IMPORT borrow a nlh_transfer address.
 */
#include "native_hotload.h"
#include "../giulia_gpu_proto.h"
#include "../../common/giulia_hotload.h"

#ifndef GG_PROTO_FAIL_START
#define GG_PROTO_FAIL_START 0
#endif
#ifndef GG_PROTO_FAIL_IMPORT
#define GG_PROTO_FAIL_IMPORT 0
#endif
#ifndef GG_PROTO_FAIL_STOP
#define GG_PROTO_FAIL_STOP 0
#endif

enum {
    GG_PROTO_GH_ABI=GH_ABI,
    GG_PROTO_GET_META=GH_PLATFORM_GET_META,
    GG_PROTO_READ_REQUEST=GH_PLATFORM_READ_REQUEST_BASE,
    GG_PROTO_WRITE_RESPONSE=GH_PLATFORM_WRITE_RESPONSE_BASE,
    GG_PROTO_SNAPSHOT_MAGIC=0x31504747 /* GGP1 */
};
struct gg_proto_state {
    uint64_t calls,errors,imports;
    uint32_t running,active;
};
struct gg_proto_snapshot {
    uint32_t magic,bytes,domain,reserved;
    uint64_t calls,errors,imports;
};
_Static_assert(sizeof(struct gg_proto_state)==32,"GPU private state ABI");
_Static_assert(sizeof(struct gg_proto_snapshot)==40,"GPU snapshot ABI");
_Static_assert(__atomic_always_lock_free(sizeof(uint64_t),0),"lock-free counters required");
_Static_assert(__atomic_always_lock_free(sizeof(uint32_t),0),"lock-free admission required");
_Static_assert(GG_PROTO_DOMAIN==GH_DOMAIN_GPU && GG_PROTO_INTERFACE==GH_INTERFACE_ABI &&
    GG_PROTO_STATE_ABI==GH_STATE_ABI,"GPU protocol must match the resident GH contract");
const uint32_t nlh_state_bytes=sizeof(struct gg_proto_state);

static void gg_proto_count(uint64_t *counter)
{
    uint64_t value=__atomic_load_n(counter,__ATOMIC_RELAXED);
    while(value!=UINT64_MAX && !__atomic_compare_exchange_n(counter,&value,value+1,
        0,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
}

static int gg_proto_admit(struct gg_proto_state *state)
{
    uint32_t active;
    if(!__atomic_load_n(&state->running,__ATOMIC_ACQUIRE))return 16;
    active=__atomic_load_n(&state->active,__ATOMIC_RELAXED);
    do {
        if(active==UINT32_MAX)return 16;
    } while(!__atomic_compare_exchange_n(&state->active,&active,active+1,
        0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED));
    if(!__atomic_load_n(&state->running,__ATOMIC_ACQUIRE)) {
        __atomic_fetch_sub(&state->active,1u,__ATOMIC_RELEASE);
        return 16;
    }
    return 0;
}

static int gg_proto_call(const struct nlh_host *host,struct gg_proto_state *state,
    uint64_t ticket,uint64_t *result)
{
    uint64_t meta[6],request[GG_PROTO_REQUEST_WORDS],plan=0,ignored=0;
    unsigned i;
    int error=gg_proto_admit(state);
    if(error)return error;
    gg_proto_count(&state->calls);
    if(!host->platform) {error=22;goto done;}
    for(i=0;i<6;++i) {
        error=host->platform(host->context,GG_PROTO_GET_META,ticket,i,&meta[i]);
        if(error)goto done;
    }
    if(meta[0]!=GG_PROTO_DOMAIN || !meta[1] || meta[2]!=GG_PROTO_OP_ALLOC_PLAN ||
       meta[3]!=GG_PROTO_REQUEST_WORDS || meta[4]!=GG_PROTO_RESPONSE_WORDS ||
       meta[5]!=GG_PROTO_GH_ABI) {error=22;goto done;}
    for(i=0;i<GG_PROTO_REQUEST_WORDS;++i) {
        error=host->platform(host->context,GG_PROTO_READ_REQUEST+i,ticket,0,&request[i]);
        if(error)goto done;
    }
    error=gg_proto_plan_request(request,GG_PROTO_REQUEST_WORDS,&plan);
    if(error)goto done;
    error=host->platform(host->context,GG_PROTO_WRITE_RESPONSE,ticket,plan,&ignored);
    if(!error)*result=plan;
done:
    if(error || (plan&UINT64_C(0xff)))gg_proto_count(&state->errors);
    __atomic_fetch_sub(&state->active,1u,__ATOMIC_RELEASE);
    return error;
}

int nlh_entry(const struct nlh_host *host,void *opaque,uint32_t operation,
    uint64_t argument,uint64_t *result)
{
    struct gg_proto_state *state=(struct gg_proto_state *)opaque;
    if(!host || host->abi!=NLH_ABI || host->bytes!=sizeof(*host) || !state || !result)
        return 22;
    switch(operation) {
    case NLH_PROBE:
        return 0; /* No gateway call or private-state mutation. */
    case NLH_START:
        if(__atomic_load_n(&state->running,__ATOMIC_ACQUIRE) ||
           __atomic_load_n(&state->active,__ATOMIC_ACQUIRE))return 16;
        __atomic_store_n(&state->running,1u,__ATOMIC_RELEASE);
        return GG_PROTO_FAIL_START?5:0; /* Exercises partial-start cleanup. */
    case NLH_STOP:
        __atomic_store_n(&state->running,0u,__ATOMIC_RELEASE);
        if(__atomic_load_n(&state->active,__ATOMIC_ACQUIRE))return 16;
        return GG_PROTO_FAIL_STOP?5:0;
    case NLH_CALL:
        return gg_proto_call(host,state,argument,result);
    case NLH_EXPORT: {
        struct nlh_transfer *transfer=(void *)(uintptr_t)argument;
        struct gg_proto_snapshot *snapshot;
        if(__atomic_load_n(&state->running,__ATOMIC_ACQUIRE) ||
           __atomic_load_n(&state->active,__ATOMIC_ACQUIRE))return 16;
        if(!transfer || ((uintptr_t)transfer&7u) || transfer->abi!=GG_PROTO_STATE_ABI ||
           transfer->reserved || !transfer->data || ((uintptr_t)transfer->data&7u) ||
           transfer->capacity<sizeof(struct gg_proto_snapshot))return 22;
        snapshot=(struct gg_proto_snapshot *)transfer->data;
        *snapshot=(struct gg_proto_snapshot){GG_PROTO_SNAPSHOT_MAGIC,sizeof(*snapshot),
            GG_PROTO_DOMAIN,0,__atomic_load_n(&state->calls,__ATOMIC_RELAXED),
            __atomic_load_n(&state->errors,__ATOMIC_RELAXED),
            __atomic_load_n(&state->imports,__ATOMIC_RELAXED)};
        transfer->bytes=sizeof(*snapshot);
        return 0;
    }
    case NLH_IMPORT: {
        const struct nlh_transfer *transfer=(const void *)(uintptr_t)argument;
        const struct gg_proto_snapshot *snapshot;
        if(__atomic_load_n(&state->running,__ATOMIC_ACQUIRE) ||
           __atomic_load_n(&state->active,__ATOMIC_ACQUIRE))return 16;
        if(!transfer || ((uintptr_t)transfer&7u) || transfer->abi!=GG_PROTO_STATE_ABI ||
           transfer->reserved || !transfer->data || ((uintptr_t)transfer->data&7u) ||
           transfer->bytes>transfer->capacity || transfer->bytes!=sizeof(struct gg_proto_snapshot))
            return 22;
        snapshot=(const struct gg_proto_snapshot *)transfer->data;
        if(snapshot->magic!=GG_PROTO_SNAPSHOT_MAGIC || snapshot->bytes!=sizeof(*snapshot) ||
           snapshot->domain!=GG_PROTO_DOMAIN || snapshot->reserved ||
           snapshot->errors>snapshot->calls || snapshot->imports==UINT64_MAX)return 22;
        if(GG_PROTO_FAIL_IMPORT)return 5;
        __atomic_store_n(&state->calls,snapshot->calls,__ATOMIC_RELAXED);
        __atomic_store_n(&state->errors,snapshot->errors,__ATOMIC_RELAXED);
        __atomic_store_n(&state->imports,snapshot->imports+1,__ATOMIC_RELAXED);
        return 0;
    }
    default:
        return 22;
    }
}
