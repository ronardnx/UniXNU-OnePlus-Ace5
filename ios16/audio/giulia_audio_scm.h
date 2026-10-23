/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Native Qualcomm SMC64 PIL adapter, fixed to giulia ADSP PAS IDs 1/36.
 * ABI: OnePlusOSS kernel d763ed47deec55451366c1187f813f2f309d88a7,
 * drivers/firmware/qcom_scm{,-smc}.c. Reuses the already audited register
 * exchange (including interrupted/busy/waitqueue handling), not GPU actions.
 */
#ifndef GIULIA_AUDIO_SCM_H
#define GIULIA_AUDIO_SCM_H
#include "../platform/giulia_scm.h"

enum { GAS_OK, GAS_ARGUMENT, GAS_PREREQUISITE, GAS_CONVENTION,
       GAS_UNAVAILABLE, GAS_TRANSPORT, GAS_SECURE, GAS_QUARANTINED };
struct gas_ops {
    void *cookie;
    int (*call)(void *,struct ggs_regs *);
    int (*delay_ms)(void *,unsigned);
    /* A kernel platform owner checks its retained clock/bandwidth votes,
     * cold-boot epoch and metadata/range ownership before EVERY operation.
     * Never return true based on a boot argument or a mapped address alone. */
    int (*authorize)(void *,unsigned command,unsigned pas,uint64_t,uint64_t);
};
struct gas_state {
    struct ggs_state exchange;
    unsigned probed,result;
    uint64_t secure_result;
};
static inline int gas_exchange(const struct gas_ops *o,struct gas_state *s,
                              uint64_t fn,uint64_t info,uint64_t a,uint64_t b,uint64_t c)
{
    const struct ggs_ops transport={o->cookie,o->call,o->delay_ms,0};
    const struct ggs_regs frame={{fn,info,a,b,c,0,0,0}};
    int e=ggs_exchange_frame(&transport,&s->exchange,&frame);
    s->secure_result=s->exchange.last_output.x[1];
    return e?GAS_TRANSPORT:GAS_OK;
}
static inline int gas_probe(const struct gas_ops *o,struct gas_state *s)
{
    static const unsigned commands[]={1,2,5,6,7};
    if(!o || !s || !o->call || !o->delay_ms || !o->authorize)return GAS_ARGUMENT;
    if(s->exchange.quarantined)return GAS_QUARANTINED;
    if(o->authorize(o->cookie,0,1,0,0)!=1 || o->authorize(o->cookie,0,36,0,0)!=1)
        return GAS_PREREQUISITE;
    if(s->probed)return GAS_OK;
    if(gas_exchange(o,s,0xc2000601u,1,0x02000601u,0,0))return GAS_TRANSPORT;
    if(s->secure_result!=1)return GAS_CONVENTION;
    for(unsigned i=0;i<sizeof(commands)/sizeof(commands[0]);i++) {
        if(gas_exchange(o,s,0x42000601u,1,0x02000200u|commands[i],0,0))return GAS_TRANSPORT;
        if(!s->secure_result)return GAS_UNAVAILABLE;
    }
    for(unsigned i=0;i<2;i++) {
        if(gas_exchange(o,s,0x42000207u,1,i?36:1,0,0))return GAS_TRANSPORT;
        if(!s->secure_result)return GAS_UNAVAILABLE;
    }
    s->probed=1;return GAS_OK;
}
static inline int gas_pas(const struct gas_ops *o,struct gas_state *s,
                         unsigned command,unsigned id,uint64_t a,uint64_t b)
{
    uint64_t info=1;
    if(!s)return GAS_ARGUMENT;
    if(id!=1 && id!=36)return (int)(s->result=GAS_ARGUMENT);
    if(command==1) {
        if(!a || (a&4095) || a>=(1ULL<<36) || b)return (int)(s->result=GAS_ARGUMENT);
        info=0x82; /* QCOM_SCM_ARGS(2, VAL, RW); metadata is the second arg. */
    } else if(command==2) {
        uint64_t base=id==1?0x9df00000ULL:0x9de80000ULL;
        uint64_t capacity=id==1?0x5280000ULL:0x80000ULL;
        if(a!=base || !b || (b&4095) || b>capacity)return (int)(s->result=GAS_ARGUMENT);
        info=3;
    } else if((command!=5 && command!=6) || a || b)return (int)(s->result=GAS_ARGUMENT);
    int e=gas_probe(o,s);if(e)return (int)(s->result=e);
    if(o->authorize(o->cookie,command,id,a,b)!=1)return (int)(s->result=GAS_PREREQUISITE);
    e=gas_exchange(o,s,0x42000200u|command,info,id,a,b);
    if(e)return (int)(s->result=e);
    return (int)(s->result=s->secure_result?GAS_SECURE:GAS_OK);
}
#endif
