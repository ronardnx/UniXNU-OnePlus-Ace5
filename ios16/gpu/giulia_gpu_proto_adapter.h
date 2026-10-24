/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Resident HFI parser adapter. The service serializes this state on its worker;
 * tests and the kernel use this same admission/validation/fallback function.
 * Selection is nonblocking; module lifecycle code never takes the GPU lock.
 */
#ifndef GIULIA_GPU_PROTO_ADAPTER_H
#define GIULIA_GPU_PROTO_ADAPTER_H
#include "giulia_gpu_proto.h"

struct gg_proto_resident {
    uint64_t epoch,calls,selected,fallback,rejected;
};
typedef int (*gg_proto_select_fn)(uint32_t,uint64_t,uint32_t,
    const uint64_t *,uint32_t,uint64_t *,uint32_t);

struct gg_proto_transaction {
    struct gga_request baseline;
    uint64_t request[GG_PROTO_REQUEST_WORDS],expected,epoch;
    int baseline_code;
};

/* Shared preparation/commit code is used by the direct production adapter and
 * the portable callback harness. Only their bridge-call expression differs. */
static inline unsigned gg_proto_prepare(struct gg_proto_resident *state,
    const uint32_t *words,unsigned count,struct gg_proto_transaction *transaction)
{
    transaction->baseline_code=gga_parse(words,count,&transaction->baseline);
    ++state->calls;
    if(!words || !state->epoch)return 0;
    transaction->request[0]=GG_PROTO_SCHEMA;transaction->request[1]=count;
    for(unsigned i=0;i<GG_PROTO_RAW_WORDS;i++)transaction->request[i+2]=i<count?words[i]:0;
    transaction->expected=0;transaction->epoch=state->epoch;
    return gg_proto_plan_request(transaction->request,GG_PROTO_REQUEST_WORDS,&transaction->expected)==0;
}

static inline int gg_proto_fallback(struct gg_proto_resident *state,
    const struct gg_proto_transaction *transaction,struct gga_request *out)
{
    ++state->fallback;
    if(!transaction->baseline_code)*out=transaction->baseline;
    return transaction->baseline_code;
}

static inline int gg_proto_finish(struct gg_proto_resident *state,
    const struct gg_proto_transaction *transaction,int error,uint64_t response,
    struct gga_request *out)
{
    if(error || state->epoch!=transaction->epoch || response!=transaction->expected) {
        if(!error)++state->rejected;
        return gg_proto_fallback(state,transaction,out);
    }
    ++state->selected;
    int code=(int)(response&0xffu);
    if(!code && !transaction->baseline_code) {
        *out=transaction->baseline;
        /* These verified response fields are consumed by gga_handle's reserve
         * size, pool permissions, master map branches and block validation.
         * Raw wire fields, addresses and handles remain resident-owned. */
        out->plan.effective_flags=(uint32_t)((response>>8)&GGA_FLAG_KNOWN);
        out->plan.masters=(uint32_t)((response>>21)&7u);
        out->plan.rounded_bytes=(uint32_t)(response>>32);
        out->plan.valid=1;
    }
    return code;
}

static inline int gg_proto_decode(struct gg_proto_resident *state,
    const uint32_t *words,unsigned count,struct gga_request *out,
    gg_proto_select_fn select)
{
    if(!state || !out)return GGA_ARGUMENT;
    struct gg_proto_transaction transaction;
    if(!gg_proto_prepare(state,words,count,&transaction) || !select)
        return gg_proto_fallback(state,&transaction,out);
    uint64_t response=transaction.expected;
    int error=select(GG_PROTO_DOMAIN,transaction.epoch,GG_PROTO_OP_ALLOC_PLAN,
        transaction.request,GG_PROTO_REQUEST_WORDS,&response,GG_PROTO_RESPONSE_WORDS);
    return gg_proto_finish(state,&transaction,error,response,out);
}

#ifdef __cplusplus
extern "C" {
#endif
/* Production implementation contains a direct select call; the HFI boundary
 * calls it directly, preserving real B/BL evidence without devirtualization. */
int giulia_gpu_hotload_decide(struct gg_proto_resident *,const uint32_t *,unsigned,
    struct gga_request *);
#ifdef __cplusplus
}
#endif
#endif
