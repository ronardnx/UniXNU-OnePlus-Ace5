/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * giulia-audio-proto: bounded pure GPR classification; hardware stays resident.
 * Interface 0x47480001, state ABI 1, kind=service, no dependencies.
 */
#include "native_hotload.h"
#include "giulia_audio_proto_wire.h"
#include "../common/giulia_hotload.h"

#ifndef GH_MODULE_VERSION
#define GH_MODULE_VERSION 1
#endif
#ifndef GH_FAIL_START
#define GH_FAIL_START 0
#endif
#ifndef GH_FAIL_IMPORT
#define GH_FAIL_IMPORT 0
#endif
#ifndef GH_FAIL_STOP
#define GH_FAIL_STOP 0
#endif
#ifndef GAPT_FAIL_START
#define GAPT_FAIL_START GH_FAIL_START
#endif
#ifndef GAPT_FAIL_IMPORT
#define GAPT_FAIL_IMPORT GH_FAIL_IMPORT
#endif
#ifndef GAPT_FAIL_STOP
#define GAPT_FAIL_STOP GH_FAIL_STOP
#endif
#ifndef GAPT_BAD_REPLY
#define GAPT_BAD_REPLY 0
#endif

struct gapt_private {
    uint64_t calls,ignored,remote_errors,imports;
    uint32_t running,active;
};
const uint32_t nlh_state_bytes=sizeof(struct gapt_private);

int nlh_entry(const struct nlh_host *host,void *opaque,uint32_t operation,
    uint64_t argument,uint64_t *result)
{
    struct gapt_private *s=(struct gapt_private *)opaque;
    if(!host||host->abi!=NLH_ABI||host->bytes!=sizeof(*host)||!s)return 22;
    switch(operation) {
    case NLH_PROBE:return 0;
    case NLH_START:
#if GAPT_FAIL_START
        return 5;
#endif
        if(__atomic_load_n(&s->active,__ATOMIC_ACQUIRE))return 16;
        __atomic_store_n(&s->running,1u,__ATOMIC_RELEASE);return 0;
    case NLH_STOP:
#if GAPT_FAIL_STOP
        return 5;
#endif
        if(__atomic_load_n(&s->active,__ATOMIC_ACQUIRE))return 16;
        __atomic_store_n(&s->running,0u,__ATOMIC_RELEASE);return 0;
    case NLH_EXPORT: {
        struct nlh_transfer *t=(struct nlh_transfer *)(uintptr_t)argument;
        if(!t||t->abi!=1||t->reserved||t->capacity<32||!t->data||s->running||s->active)return 22;
        uint64_t values[4]={s->calls,s->ignored,s->remote_errors,s->imports};
        uint8_t *p=(uint8_t *)t->data;
        for(unsigned i=0;i<32;i++)p[i]=(uint8_t)(values[i/8]>>((i%8)*8));
        t->bytes=32;return 0;
    }
    case NLH_IMPORT: {
#if GAPT_FAIL_IMPORT
        return 5;
#endif
        const struct nlh_transfer *t=(const struct nlh_transfer *)(uintptr_t)argument;
        if(!t||t->abi!=1||t->reserved||t->bytes!=32||t->bytes>t->capacity||
           !t->data||s->running||s->active)return 22;
        uint64_t values[4]={0,0,0,0};const uint8_t *p=(const uint8_t *)t->data;
        for(unsigned i=0;i<32;i++)values[i/8]|=(uint64_t)p[i]<<((i%8)*8);
        s->calls=values[0];s->ignored=values[1];s->remote_errors=values[2];s->imports=values[3]+1;
        return 0;
    }
    case NLH_CALL: {
        uint64_t meta[GH_META_COUNT],request[GAPT_WORDS],response;int error=0;
        if(!result||!host->platform||!__atomic_load_n(&s->running,__ATOMIC_ACQUIRE))return 16;
        __atomic_add_fetch(&s->active,1u,__ATOMIC_ACQ_REL);
        if(!__atomic_load_n(&s->running,__ATOMIC_ACQUIRE)) {error=16;goto done;}
        for(unsigned i=0;i<GH_META_COUNT;i++) {
            error=host->platform(host->context,GH_PLATFORM_GET_META,argument,i,&meta[i]);
            if(error) {
                /* Only after a denied GET_META, generic CALL 0..3 may inspect
                 * private counters. A valid small resident ticket always
                 * takes the actual request path; no ticket range is assumed. */
                if(i==0&&argument<5) {
                    if(argument==4)*result=GH_MODULE_VERSION;
                    else {
                        const uint64_t *value=argument==0?&s->calls:argument==1?&s->imports:
                            argument==2?&s->ignored:&s->remote_errors;
                        *result=__atomic_load_n(value,__ATOMIC_RELAXED);
                    }
                    error=0;
                }
                goto done;
            }
        }
        if(meta[GH_META_DOMAIN]!=GH_DOMAIN_AUDIO||!meta[GH_META_EPOCH]||
           meta[GH_META_OPERATION]<GAPT_SPF_REPLY||meta[GH_META_OPERATION]>GAPT_APM_REPLY||
           meta[GH_META_REQUEST_WORDS]!=GAPT_WORDS||meta[GH_META_RESPONSE_WORDS]!=1||
           meta[GH_META_ABI]!=GH_ABI) {error=22;goto done;}
        for(unsigned i=0;i<GAPT_WORDS;i++) {
            error=host->platform(host->context,GH_PLATFORM_READ_REQUEST_BASE+i,argument,0,&request[i]);if(error)goto done;
        }
        if(request[1]!=meta[GH_META_OPERATION]) {error=22;goto done;}
        response=gapt_classify(request,GAPT_WORDS);
#if GAPT_BAD_REPLY
        response=0xffffffffffffffffULL;
#endif
        error=host->platform(host->context,GH_PLATFORM_WRITE_RESPONSE_BASE,argument,response,result);
        if(!error) {
            __atomic_add_fetch(&s->calls,1u,__ATOMIC_RELAXED);
            if(!response)__atomic_add_fetch(&s->ignored,1u,__ATOMIC_RELAXED);
            if((unsigned)response==GAPT_APM_ERROR)__atomic_add_fetch(&s->remote_errors,1u,__ATOMIC_RELAXED);
            *result=0;
        }
done:
        __atomic_sub_fetch(&s->active,1u,__ATOMIC_ACQ_REL);return error;
    }
    default:return 22;
    }
}
