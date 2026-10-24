/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_AUDIO_PROTO_H
#define GIULIA_AUDIO_PROTO_H
#include "giulia_audio_proto_wire.h"
#include "../common/giulia_hotload.h"
#if GAPT_WORDS > GH_MAX_WORDS
#error Audio protocol request exceeds the public bridge bound
#endif

#if defined(KERNEL) && GIULIA_HOTLOAD_ENABLED
#ifdef __cplusplus
extern "C" {
#endif
int giulia_audio_hotload_decide(uint64_t epoch,uint32_t operation,
    const uint64_t *request,uint32_t request_words,uint64_t *response,uint32_t response_words);
#ifdef __cplusplus
}
#endif
#else
static inline int giulia_audio_hotload_decide(uint64_t epoch,uint32_t operation,
    const uint64_t *request,uint32_t request_words,uint64_t *response,uint32_t response_words)
{
    return giulia_hotload_select(GH_DOMAIN_AUDIO,epoch,operation,request,request_words,response,response_words);
}
#endif

/* Per resident translation unit; the public bridge has domain-wide counters.
 * These counters make local validation/fallback visible without exporting any
 * transport, graph, provider or module pointer. */
static uint64_t gapt_counts[6]; /* attempted, adopted, fallback, rejected, changed, ignored */
static inline void gapt_snapshot(uint64_t out[6])
{ for(unsigned i=0;i<6;i++)out[i]=__atomic_load_n(&gapt_counts[i],__ATOMIC_RELAXED); }

/* Only resident code borrows packet. The public bridge copies all words;
 * the service receives an invocation-bound ticket, never this pointer. */
static inline uint64_t gapt_reply(uint64_t epoch,unsigned kind,unsigned pending,
    uint32_t token,uint32_t local_port,uint32_t remote_port,uint32_t opcode,
    const uint8_t *packet,size_t bytes)
{
    uint64_t request[GAPT_WORDS],baseline,response;
    gapt_request(request,kind,pending,token,local_port,remote_port,opcode,packet,bytes);
    baseline=gapt_classify(request,GAPT_WORDS);response=baseline;
    if(!epoch||!packet||bytes>GAPT_PACKET_BYTES)return baseline;
    __atomic_add_fetch(&gapt_counts[0],1u,__ATOMIC_RELAXED);
    if(giulia_audio_hotload_decide(epoch,kind,request,GAPT_WORDS,&response,1u)) {
        __atomic_add_fetch(&gapt_counts[2],1u,__ATOMIC_RELAXED);return baseline;
    }
    /* A policy may reject a packet more strictly. It cannot invent a status,
     * consume a mismatched token or turn an error into a successful ACK. */
    if(response!=GAPT_IGNORE&&response!=baseline) {
        __atomic_add_fetch(&gapt_counts[3],1u,__ATOMIC_RELAXED);
        __atomic_add_fetch(&gapt_counts[2],1u,__ATOMIC_RELAXED);return baseline;
    }
    __atomic_add_fetch(&gapt_counts[1],1u,__ATOMIC_RELAXED);
    if(response!=baseline)__atomic_add_fetch(&gapt_counts[4],1u,__ATOMIC_RELAXED);
    if(!response)__atomic_add_fetch(&gapt_counts[5],1u,__ATOMIC_RELAXED);
    return response;
}
#endif
