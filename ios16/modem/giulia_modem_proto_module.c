/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Standalone NLH2 service giulia-modem-proto. Only pure QMI framing is
 * replaceable; MPSS resources, requests, IRQ/DMA and workers remain resident.
 * Build separately with NativeHotload/build-module.py, never as a resident TU.
 */
#include "native_hotload.h"
#include "giulia_modem_hotload_wire.h"

#ifndef NLH_FAIL_START
#define NLH_FAIL_START 0
#endif
#ifndef NLH_FAIL_IMPORT
#define NLH_FAIL_IMPORT 0
#endif
#ifndef NLH_FAIL_STOP
#define NLH_FAIL_STOP 0
#endif

/* Frozen GH ABI 1 operations; no access to hardware or resident addresses. */
#define GMR_MODULE_BRIDGE_ABI 1u
#define GMR_MODULE_GET_META 0x4700u
#define GMR_MODULE_READ_BASE 0x4720u
#define GMR_MODULE_WRITE_BASE 0x4760u
#define GMR_MODULE_META_DOMAIN 0u
#define GMR_MODULE_META_EPOCH 1u
#define GMR_MODULE_META_OPERATION 2u
#define GMR_MODULE_META_REQUEST_WORDS 3u
#define GMR_MODULE_META_RESPONSE_WORDS 4u
#define GMR_MODULE_META_ABI 5u
#define GMR_MODULE_META_COUNT 6u
#define GMR_MODULE_SNAPSHOT_MAGIC UINT32_C(0x31504d47)

struct gmr_module_state {
    uint64_t running,calls,native,fixture,rejected,remote,imports;
};
struct gmr_module_snapshot {
    uint32_t magic,bytes,domain,abi;
    uint64_t calls,native,fixture,rejected,remote,imports;
};
_Static_assert(sizeof(struct gmr_module_state)==56,"private modem protocol state ABI");
_Static_assert(sizeof(struct gmr_module_snapshot)==64,"serialized modem protocol state ABI");
_Static_assert(__atomic_always_lock_free(sizeof(uint64_t),0),"lock-free counters required");
_Static_assert(GMR_HOTLOAD_HEADER_WORDS+(GMR_HOTLOAD_MAX_PAYLOAD+7u)/8u==GMR_HOTLOAD_MAX_WORDS,
               "modem protocol mailbox capacity");
const uint32_t nlh_state_bytes=sizeof(struct gmr_module_state);

static int gmr_module_telemetry(const struct gmr_module_state *state,uint64_t argument,uint64_t *result)
{
    const uint64_t *counter;
    switch(argument) {
    case 0:counter=&state->calls;break;
    case 1:counter=&state->native;break;
    case 2:counter=&state->fixture;break;
    case 3:counter=&state->rejected;break;
    case 4:counter=&state->remote;break;
    case 5:counter=&state->imports;break;
    default:return 22;
    }
    *result=__atomic_load_n(counter,__ATOMIC_RELAXED);return 0;
}

static int gmr_module_call(const struct nlh_host *host,struct gmr_module_state *state,
    uint64_t ticket,uint64_t *result)
{
    uint64_t meta[GMR_MODULE_META_COUNT],request[GMR_HOTLOAD_MAX_WORDS],verdict=0,unused=0;
    uint8_t payload[GMR_HOTLOAD_MAX_PAYLOAD];
    uint32_t i,words,bytes,kind;uint16_t txn;int error;
    /* A small ticket is still a ticket. Ask the authenticated bridge first;
     * only failure to recognize it permits read-only USB scalar telemetry. */
    error=host->platform?host->platform(host->context,GMR_MODULE_GET_META,ticket,
                                      GMR_MODULE_META_DOMAIN,&meta[GMR_MODULE_META_DOMAIN]):95;
    if(error)return gmr_module_telemetry(state,ticket,result);
    for(i=1;i<GMR_MODULE_META_COUNT;i++) {
        error=host->platform(host->context,GMR_MODULE_GET_META,ticket,i,&meta[i]);
        if(error)goto rejected;
    }
    if(!meta[GMR_MODULE_META_REQUEST_WORDS] || meta[GMR_MODULE_META_REQUEST_WORDS]>GMR_HOTLOAD_MAX_WORDS) {
        error=22;goto rejected;
    }
    words=(uint32_t)meta[GMR_MODULE_META_REQUEST_WORDS];
    /* Read the complete bounded request before inspecting any protocol field.
     * The bridge records reads and refuses incomplete-response success. */
    for(i=0;i<words;i++) {
        error=host->platform(host->context,GMR_MODULE_READ_BASE+i,ticket,0,&request[i]);
        if(error)goto rejected;
    }
    if(meta[GMR_MODULE_META_DOMAIN]!=GMR_HOTLOAD_DOMAIN ||
        meta[GMR_MODULE_META_OPERATION]!=GMR_HOTLOAD_QMI_RESPONSE ||
        meta[GMR_MODULE_META_RESPONSE_WORDS]!=1u || meta[GMR_MODULE_META_ABI]!=GMR_MODULE_BRIDGE_ABI ||
        words<GMR_HOTLOAD_HEADER_WORDS || (request[0]>>48) || (request[1]>>48) ||
        request[3]>GMR_HOTLOAD_ORIGIN_FIXTURE) {error=22;goto rejected;}
    kind=(uint32_t)request[0];txn=(uint16_t)(request[0]>>32);bytes=(uint32_t)request[1];
    if(bytes>GMR_HOTLOAD_MAX_PAYLOAD ||
        words!=GMR_HOTLOAD_HEADER_WORDS+(bytes+7u)/8u ||
        (uint16_t)(request[1]>>32)!=gmr_proto_message(kind)) {error=22;goto rejected;}
    if((bytes%8u) && (request[words-1u]>>((bytes%8u)*8u))) {error=22;goto rejected;}
    for(i=0;i<bytes;i++)payload[i]=(uint8_t)(request[GMR_HOTLOAD_HEADER_WORDS+i/8u]>>((i%8u)*8u));
    /* opid and epoch are scalar correlation values only. The resident caller
     * owns source authentication and checks its epoch again before adoption. */
    verdict=gmr_proto_check(kind,txn,payload,bytes);
    if(!gmr_proto_result_valid(verdict)) {error=22;goto rejected;}
    error=host->platform(host->context,GMR_MODULE_WRITE_BASE,ticket,verdict,&unused);
    if(error)goto rejected;
    __atomic_fetch_add(&state->calls,1,__ATOMIC_RELAXED);
    if(request[3]==GMR_HOTLOAD_ORIGIN_NATIVE)__atomic_fetch_add(&state->native,1,__ATOMIC_RELAXED);
    else __atomic_fetch_add(&state->fixture,1,__ATOMIC_RELAXED);
    if((verdict&255u)==GMQ_REMOTE)__atomic_fetch_add(&state->remote,1,__ATOMIC_RELAXED);
    else if((verdict&255u)!=GMQ_OK)__atomic_fetch_add(&state->rejected,1,__ATOMIC_RELAXED);
    *result=0;return 0;
rejected:
    __atomic_fetch_add(&state->rejected,1,__ATOMIC_RELAXED);
    return error;
}

int nlh_entry(const struct nlh_host *host,void *opaque,uint32_t operation,
    uint64_t argument,uint64_t *result)
{
    struct gmr_module_state *state=opaque;
    if(!host || host->abi!=NLH_ABI || host->bytes!=sizeof(*host) || !state || !result)return 22;
    *result=0;
    switch(operation) {
    case NLH_PROBE:return 0;
    case NLH_START:
        /* A failing candidate exercises the loader's partial-START rollback. */
        __atomic_store_n(&state->running,1,__ATOMIC_RELEASE);
        return NLH_FAIL_START?5:0;
    case NLH_STOP:
        if(NLH_FAIL_STOP)return 5;
        __atomic_store_n(&state->running,0,__ATOMIC_RELEASE);return 0;
    case NLH_EXPORT: {
        struct nlh_transfer *transfer=(void *)(uintptr_t)argument;
        struct gmr_module_snapshot *snapshot;
        if(__atomic_load_n(&state->running,__ATOMIC_ACQUIRE) || !transfer ||
            transfer->abi!=GMR_HOTLOAD_STATE_ABI || transfer->reserved || !transfer->data ||
            ((uintptr_t)transfer->data&7u) || transfer->capacity<sizeof(*snapshot))return 22;
        snapshot=transfer->data;
        snapshot->magic=GMR_MODULE_SNAPSHOT_MAGIC;snapshot->bytes=sizeof(*snapshot);
        snapshot->domain=GMR_HOTLOAD_DOMAIN;snapshot->abi=GMR_HOTLOAD_STATE_ABI;
        snapshot->calls=__atomic_load_n(&state->calls,__ATOMIC_RELAXED);
        snapshot->native=__atomic_load_n(&state->native,__ATOMIC_RELAXED);
        snapshot->fixture=__atomic_load_n(&state->fixture,__ATOMIC_RELAXED);
        snapshot->rejected=__atomic_load_n(&state->rejected,__ATOMIC_RELAXED);
        snapshot->remote=__atomic_load_n(&state->remote,__ATOMIC_RELAXED);
        snapshot->imports=__atomic_load_n(&state->imports,__ATOMIC_RELAXED);
        transfer->bytes=sizeof(*snapshot);return 0;
    }
    case NLH_IMPORT: {
        const struct nlh_transfer *transfer=(const void *)(uintptr_t)argument;
        const struct gmr_module_snapshot *snapshot;
        if(__atomic_load_n(&state->running,__ATOMIC_ACQUIRE) || !transfer ||
            transfer->abi!=GMR_HOTLOAD_STATE_ABI || transfer->reserved || !transfer->data ||
            ((uintptr_t)transfer->data&7u) || transfer->bytes>transfer->capacity ||
            transfer->bytes!=sizeof(*snapshot))return 22;
        snapshot=transfer->data;
        if(snapshot->magic!=GMR_MODULE_SNAPSHOT_MAGIC || snapshot->bytes!=sizeof(*snapshot) ||
            snapshot->domain!=GMR_HOTLOAD_DOMAIN || snapshot->abi!=GMR_HOTLOAD_STATE_ABI ||
            snapshot->native>snapshot->calls || snapshot->fixture!=snapshot->calls-snapshot->native ||
            snapshot->remote>snapshot->calls)return 22;
        if(NLH_FAIL_IMPORT)return 5;
        __atomic_store_n(&state->calls,snapshot->calls,__ATOMIC_RELAXED);
        __atomic_store_n(&state->native,snapshot->native,__ATOMIC_RELAXED);
        __atomic_store_n(&state->fixture,snapshot->fixture,__ATOMIC_RELAXED);
        __atomic_store_n(&state->rejected,snapshot->rejected,__ATOMIC_RELAXED);
        __atomic_store_n(&state->remote,snapshot->remote,__ATOMIC_RELAXED);
        __atomic_store_n(&state->imports,snapshot->imports==UINT64_MAX?UINT64_MAX:snapshot->imports+1u,
                         __ATOMIC_RELAXED);
        return 0;
    }
    case NLH_CALL:
        if(!__atomic_load_n(&state->running,__ATOMIC_ACQUIRE))return 16;
        return gmr_module_call(host,state,argument,result);
    default:return 22;
    }
}
