/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * giulia-wifi-proto: real QMI/WMI shape parser, GH ABI1 / NLH2 service.
 * Hardware, packet values and callbacks remain resident. Only the bounded
 * invocation ticket enters NLH_CALL; USB scalar calls expose counters only.
 */
#include "native_hotload.h"
#include "../common/giulia_hotload.h"
#include "giulia_wifi_protocol.h"
#define GWPR_STATE_MAGIC 0x31505747u
#define GWPR_SNAPSHOT_MAGIC 0x31535747u
#define GWPR_CLOSED 0x80000000u
#define GWPR_INFLIGHT 0x7fffffffu
#define GWPR_RUNNING 1u
#define GWPR_STOPPED 2u
#define GWPR_IMPORTED 3u
#ifndef GWPR_FAIL_START
#define GWPR_FAIL_START 0
#endif
#ifndef GWPR_FAIL_IMPORT
#define GWPR_FAIL_IMPORT 0
#endif
#ifndef GWPR_FAIL_STOP
#define GWPR_FAIL_STOP 0
#endif
struct gwpr_state {
    uint32_t magic,abi,bytes,mode,admission,schema,last_error,reserved;
    uint64_t calls,accepts,rejects,bridge_errors,starts,stops,imports,selftest_digest;
    uint64_t reserved64[4];
};
struct gwpr_snapshot {
    uint32_t magic,abi,bytes,schema;
    uint64_t calls,accepts,rejects,bridge_errors,starts,stops,imports,reserved;
};
_Static_assert(sizeof(struct gwpr_state)==128,"Wi-Fi private state ABI");
_Static_assert(sizeof(struct gwpr_snapshot)==80,"Wi-Fi serialized state ABI");
_Static_assert(GWPR_MAX_WORDS==GH_MAX_WORDS,"GH request bound");
_Static_assert(GWPR_INTERFACE_ABI==GH_INTERFACE_ABI,"GH interface ABI");
_Static_assert(__atomic_always_lock_free(sizeof(uint64_t),0),"atomic counters");
const uint32_t nlh_state_bytes=sizeof(struct gwpr_state);
static int gwpr_state_valid(const struct gwpr_state *s) {
    return s->magic==GWPR_STATE_MAGIC && s->abi==GH_STATE_ABI && s->bytes==sizeof(*s) &&
        s->schema==GWPR_ABI && !s->reserved && !s->reserved64[0] && !s->reserved64[1] &&
        !s->reserved64[2] && !s->reserved64[3];
}
static uint64_t gwpr_selftest(void) {
    uint64_t q[5]={GWPR_ABI|((uint64_t)GWPR_QMI<<32),0x38|((uint64_t)4<<32),0,0,0};
    if(gwpr_status(gwpr_parse(q,3))!=GWPR_VALID)return 0;
    q[2]=6|((uint64_t)1<<32);q[3]=gwpr_shape(0x10,3,0,3);
    if(gwpr_status(gwpr_parse(q,4))!=GWPR_WIDTH)return 0;
    q[1]=0x37|((uint64_t)4<<32);q[2]=6|((uint64_t)2<<32);
    q[3]=gwpr_shape(0x20,0,0,3);q[4]=gwpr_shape(0x20,0,3,3);
    if(gwpr_status(gwpr_parse(q,5))!=GWPR_DUPLICATE)return 0;
    q[2]=3|((uint64_t)1<<32);q[3]=gwpr_shape(0x20,2,0,3);
    if(gwpr_status(gwpr_parse(q,4))!=GWPR_FRAMING)return 0;
    q[0]=GWPR_ABI|((uint64_t)GWPR_WMI<<32);q[1]=0x3001;q[2]=8|((uint64_t)2<<32);
    q[3]=gwpr_shape(18,0,0,4);q[4]=gwpr_shape(18,0,4,4);
    if(gwpr_status(gwpr_parse(q,5))!=GWPR_VALID)return 0;
    q[1]=0xffffff;q[2]=0;
    if(gwpr_status(gwpr_parse(q,3))!=GWPR_BYPASS)return 0;
    return UINT64_C(0x5750523100000006);
}
static int gwpr_enter(struct gwpr_state *s) {
    uint32_t value=__atomic_load_n(&s->admission,__ATOMIC_ACQUIRE);
    for(unsigned retry=0;retry<64;retry++) {
        if((value&GWPR_CLOSED) || (value&GWPR_INFLIGHT)==GWPR_INFLIGHT)return 16;
        if(__atomic_compare_exchange_n(&s->admission,&value,value+1u,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return 0;
    }
    return 16;
}
static void gwpr_leave(struct gwpr_state *s)
{(void)__atomic_fetch_sub(&s->admission,1u,__ATOMIC_RELEASE);}
static int gwpr_call(const struct nlh_host *host,struct gwpr_state *s,uint64_t ticket,uint64_t *result) {
    uint64_t meta[GH_META_COUNT]={0},request[GWPR_MAX_WORDS]={0};
    if(ticket<=6) {
        /* GH tickets carry the nonzero 0x4748 prefix. Scalar telemetry never
         * enters the bridge or increments its authorization-reject counter. */
        if(ticket==0)*result=__atomic_load_n(&s->calls,__ATOMIC_RELAXED);
        else if(ticket==1)*result=__atomic_load_n(&s->accepts,__ATOMIC_RELAXED);
        else if(ticket==2)*result=__atomic_load_n(&s->rejects,__ATOMIC_RELAXED);
        else if(ticket==3)*result=__atomic_load_n(&s->imports,__ATOMIC_RELAXED);
        else if(ticket==4)*result=__atomic_load_n(&s->starts,__ATOMIC_RELAXED);
        else if(ticket==5)*result=__atomic_load_n(&s->stops,__ATOMIC_RELAXED);
        else if(ticket==6)*result=__atomic_load_n(&s->bridge_errors,__ATOMIC_RELAXED);
        return 0;
    }
    int error=host->platform(host->context,GH_PLATFORM_GET_META,ticket,GH_META_DOMAIN,&meta[GH_META_DOMAIN]);
    if(error)return error;
    for(unsigned i=1;i<GH_META_COUNT;i++) {
        error=host->platform(host->context,GH_PLATFORM_GET_META,ticket,i,&meta[i]);if(error)goto failed;
    }
    if(meta[GH_META_DOMAIN]!=GH_DOMAIN_WIFI || !meta[GH_META_EPOCH] || meta[GH_META_ABI]!=GH_ABI ||
        (meta[GH_META_OPERATION]!=GWPR_QMI && meta[GH_META_OPERATION]!=GWPR_WMI) ||
        meta[GH_META_REQUEST_WORDS]<GWPR_META_WORDS || meta[GH_META_REQUEST_WORDS]>GWPR_MAX_WORDS ||
        meta[GH_META_RESPONSE_WORDS]!=1){error=22;goto failed;}
    for(unsigned i=0;i<(unsigned)meta[GH_META_REQUEST_WORDS];i++) {
        error=host->platform(host->context,GH_PLATFORM_READ_REQUEST_BASE+i,ticket,0,&request[i]);if(error)goto failed;
    }
    if((request[0]>>32)!=meta[GH_META_OPERATION]){error=22;goto failed;}
    *result=gwpr_parse(request,(unsigned)meta[GH_META_REQUEST_WORDS]);
    __atomic_fetch_add(&s->calls,UINT64_C(1),__ATOMIC_RELAXED);
    if(gwpr_status(*result)==GWPR_VALID)__atomic_fetch_add(&s->accepts,UINT64_C(1),__ATOMIC_RELAXED);
    else if(gwpr_status(*result)!=GWPR_BYPASS)__atomic_fetch_add(&s->rejects,UINT64_C(1),__ATOMIC_RELAXED);
    error=host->platform(host->context,GH_PLATFORM_WRITE_RESPONSE_BASE,ticket,*result,&meta[0]);
    if(error)goto failed;
    return 0;
failed:
    __atomic_fetch_add(&s->bridge_errors,UINT64_C(1),__ATOMIC_RELAXED);return error;
}
int nlh_entry(const struct nlh_host *host,void *opaque,uint32_t operation,uint64_t argument,uint64_t *result) {
    struct gwpr_state *s=(struct gwpr_state *)opaque;
    if(!host || host->abi!=NLH_ABI || host->bytes!=sizeof(*host) || !host->platform || !s || !result)return 22;
    switch(operation) {
    case NLH_PROBE:
        return gwpr_selftest()?0:5;
    case NLH_START:
        if(s->magic && !gwpr_state_valid(s))return 22;
        if(s->mode==GWPR_RUNNING || (__atomic_load_n(&s->admission,__ATOMIC_ACQUIRE)&GWPR_INFLIGHT))return 16;
        __atomic_store_n(&s->admission,GWPR_CLOSED,__ATOMIC_RELEASE);
        s->magic=GWPR_STATE_MAGIC;s->abi=GH_STATE_ABI;s->bytes=sizeof(*s);s->schema=GWPR_ABI;
        s->selftest_digest=gwpr_selftest();
        if(!s->selftest_digest || GWPR_FAIL_START){s->last_error=5;return 5;}
        __atomic_fetch_add(&s->starts,UINT64_C(1),__ATOMIC_RELAXED);s->last_error=0;s->mode=GWPR_RUNNING;
        __atomic_store_n(&s->admission,0u,__ATOMIC_RELEASE);return 0;
    case NLH_STOP: {
        uint32_t active=__atomic_fetch_or(&s->admission,GWPR_CLOSED,__ATOMIC_ACQ_REL);
        if((active&GWPR_INFLIGHT) || GWPR_FAIL_STOP){s->last_error=16;return 16;}
        if(s->magic && !gwpr_state_valid(s))return 22;
        s->mode=GWPR_STOPPED;
        __atomic_fetch_add(&s->stops,UINT64_C(1),__ATOMIC_RELAXED);return 0;
    }
    case NLH_EXPORT: {
        struct nlh_transfer *t=(struct nlh_transfer *)(uintptr_t)argument;
        if(!gwpr_state_valid(s) || s->mode!=GWPR_STOPPED || __atomic_load_n(&s->admission,__ATOMIC_ACQUIRE)!=GWPR_CLOSED ||
            !t || t->abi!=GH_STATE_ABI || t->reserved || !t->data || ((uintptr_t)t->data&7u) || t->capacity<sizeof(struct gwpr_snapshot))return 22;
        struct gwpr_snapshot *v=(struct gwpr_snapshot *)t->data;
        *v=(struct gwpr_snapshot){GWPR_SNAPSHOT_MAGIC,GH_STATE_ABI,sizeof(*v),GWPR_ABI,
            s->calls,s->accepts,s->rejects,s->bridge_errors,s->starts,s->stops,s->imports,0};
        t->bytes=sizeof(*v);return 0;
    }
    case NLH_IMPORT: {
        const struct nlh_transfer *t=(const struct nlh_transfer *)(uintptr_t)argument;
        if((s->magic && !gwpr_state_valid(s)) || s->mode==GWPR_RUNNING ||
            (__atomic_load_n(&s->admission,__ATOMIC_ACQUIRE)&GWPR_INFLIGHT) || !t || t->abi!=GH_STATE_ABI || t->reserved ||
            !t->data || ((uintptr_t)t->data&7u) || t->bytes>t->capacity || t->bytes!=sizeof(struct gwpr_snapshot))return 22;
        const struct gwpr_snapshot *v=(const struct gwpr_snapshot *)t->data;
        if(v->magic!=GWPR_SNAPSHOT_MAGIC || v->abi!=GH_STATE_ABI || v->bytes!=sizeof(*v) || v->schema!=GWPR_ABI ||
            v->reserved || v->accepts>v->calls || v->rejects>v->calls-v->accepts || v->imports==UINT64_MAX)return 22;
        if(GWPR_FAIL_IMPORT)return 5;
        s->magic=GWPR_STATE_MAGIC;s->abi=GH_STATE_ABI;s->bytes=sizeof(*s);s->schema=GWPR_ABI;s->mode=GWPR_IMPORTED;
        s->calls=v->calls;s->accepts=v->accepts;s->rejects=v->rejects;s->bridge_errors=v->bridge_errors;
        s->starts=v->starts;s->stops=v->stops;s->imports=v->imports+1;
        s->last_error=0;s->selftest_digest=0;
        __atomic_store_n(&s->admission,GWPR_CLOSED,__ATOMIC_RELEASE);return 0;
    }
    case NLH_CALL: {
        if(!gwpr_state_valid(s) || s->mode!=GWPR_RUNNING)return 22;
        int error=gwpr_enter(s);if(error)return error;
        error=gwpr_call(host,s,argument,result);gwpr_leave(s);return error;
    }
    default:return 22;
    }
}
