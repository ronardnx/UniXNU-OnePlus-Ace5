/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_GPU_SMMU_H
#define GIULIA_GPU_SMMU_H
#include "../vendor/ArmSmmu4K/ArmSmmu4KStreams.h"
#include "giulia_gpu_diag.h"
/* Adapter must own the GPU SMMU (0x03da0000/0x40000), not apps SMMU.
 * It grants a globally exclusive bank/ASID/slot lease, validates ACTLR/security
 * and installs fault handling. The immutable config/table allocations remain
 * owned until retain_resources is cleared. No bank or ASID is inferred here.
 * The only supported master is the captured GMU SID 5; secure SID 2 is excluded.
 */
struct gg_smmu_ops {
    ArmSmmu4KIo io;
    int (*lease)(void *,unsigned,unsigned,unsigned); /* bank, ASID, slot; no MMIO writes */
    int (*quiescent)(void *);
    int (*publish_tables)(void *,const ArmSmmu4KConfig *,const ArmSmmu4KResult *);
    int (*release_lease)(void *); /* Keep exact SMR faulting after detach. */
    gg_trace_fn trace;
};
struct gg_smmu_state {
    unsigned attempted, leased, active, quarantined, retain_resources, bank, slot;
    ArmSmmu4KStatus error, cleanup;
    ArmSmmu4KStreamState streams;
    ArmSmmu4KProgramResult context;
    ArmSmmu4KStreamResult route;
    unsigned phase;
    struct gg_diagnostics diag;
};
static inline int gg_smmu_valid(const struct gg_smmu_ops *o)
{
    return o && o->io.register_bytes==0x40000 && o->lease && o->quiescent &&
        o->publish_tables && o->release_lease && o->io.read32 && o->io.read64 &&
        o->io.write32 && o->io.write64 && o->io.barrier;
}
/* Transparent I/O journal. Expectations are learned only from writes to
 * stable owned configuration registers; TLB commands are self-clearing.
 * First failure survives the library's internal cleanup attempts. */
struct gg_smmu_view {
    const struct gg_smmu_ops *ops; struct gg_smmu_state *state;
    unsigned cleanup, count, polls;
    struct { size_t at; uint64_t value; unsigned width; } expected[32];
};
static inline int gg_smmu_access(struct gg_smmu_view *v,size_t at,uint64_t *value,unsigned width,unsigned write)
{
    const ArmSmmu4KIo *io=&v->ops->io;
    struct gg_smmu_state *s=v->state;
    unsigned valid=GGD_ADDRESS,cleanup=v->cleanup || !!s->diag.failure.sequence,i;
    uint64_t expected=0; int error,native_error;
    if (write) {
        if (width==64) error=io->write64(io->cookie,at,*value);
        else error=io->write32(io->cookie,at,(uint32_t)*value);
        native_error=error;if(error)error=ArmSmmu4KIoError;
        expected=*value; valid|=GGD_EXPECTED;
        if ((at&0xfff)!=0x58 && (at&0xfff)!=0x610 && (at&0xfff)!=0x618 && (at&0xfff)!=0x7f0) {
            for(i=0;i<v->count;++i) if(v->expected[i].at==at && v->expected[i].width==width) break;
            if(i<32) {v->expected[i].at=at;v->expected[i].width=width;v->expected[i].value=*value;if(i==v->count)++v->count;}
        }
    } else {
        if(width==64) error=io->read64(io->cookie,at,value);
        else {uint32_t x=0;error=io->read32(io->cookie,at,&x);*value=x;}
        native_error=error;if(error)error=ArmSmmu4KIoError;
        if(!error) valid|=GGD_OBSERVED;
        for(i=0;i<v->count;++i) if(v->expected[i].at==at && v->expected[i].width==width) {
            expected=v->expected[i].value;valid|=GGD_EXPECTED;
            if(!error && expected!=*value) error=ArmSmmu4KStateMismatch;
            break;
        }
        if(!error && at>=0x2000 && (at&0xfff)==0x58 && (*value&0xc00001feu)) {
            error=ArmSmmu4KFault;expected=0;valid|=GGD_EXPECTED;
        }
        if(!error && (at&0xfff)==0x7f4 && (*value&1)) {
            if(++v->polls==1000) {error=ArmSmmu4KTimeout;expected=0;valid|=GGD_EXPECTED;}
        } else v->polls=0;
    }
    gg_record_full(&s->diag,v->ops->trace,io->cookie,GGD_SMMU,s->phase,write?GGD_WRITE:GGD_READ,
        0x03da0000ull+at,expected,write?0:*value,valid|GGD_NATIVE,width==32?0xffffffffu:~(uint64_t)0,error,native_error,cleanup);
    return error;
}
static inline int gg_smmu_r32(void *p,size_t at,uint32_t *out) {uint64_t v=0;int e=gg_smmu_access((struct gg_smmu_view *)p,at,&v,32,0);*out=(uint32_t)v;return e;}
static inline int gg_smmu_r64(void *p,size_t at,uint64_t *out) {return gg_smmu_access((struct gg_smmu_view *)p,at,out,64,0);}
static inline int gg_smmu_w32(void *p,size_t at,uint32_t value) {uint64_t v=value;return gg_smmu_access((struct gg_smmu_view *)p,at,&v,32,1);}
static inline int gg_smmu_w64(void *p,size_t at,uint64_t value) {return gg_smmu_access((struct gg_smmu_view *)p,at,&value,64,1);}
static inline int gg_smmu_barrier(void *p) {
    struct gg_smmu_view *v=(struct gg_smmu_view *)p;
    int e=v->ops->io.barrier(v->ops->io.cookie);
    gg_record_full(&v->state->diag,v->ops->trace,v->ops->io.cookie,GGD_SMMU,v->state->phase,GGD_BARRIER,
        0,0,0,GGD_NATIVE,0,e?ArmSmmu4KIoError:0,e,v->cleanup || !!v->state->diag.failure.sequence);
    return e;
}
static inline ArmSmmu4KIo gg_smmu_io(struct gg_smmu_view *view)
{
    ArmSmmu4KIo io={view,0x40000,gg_smmu_r32,gg_smmu_r64,gg_smmu_w32,gg_smmu_w64,gg_smmu_barrier};
    return io;
}
static inline ArmSmmu4KStatus gg_smmu_stop(const struct gg_smmu_ops *o,
    struct gg_smmu_state *s,const ArmSmmu4KContextConfig *c)
{
    ArmSmmu4KStatus e;
    struct gg_smmu_view view={o,s,1,0,0,{{0,0,0}}};
    ArmSmmu4KIo io=gg_smmu_io(&view);
    if (!gg_smmu_valid(o) || !s || !c) return ArmSmmu4KArgument;
    if (s->quarantined) return ArmSmmu4KBusy;
    if (!s->leased) return ArmSmmu4KSuccess;
    s->retain_resources=1;
    s->phase=5;
    if (o->quiescent(o->io.cookie)!=1) goto uncertain;
    if (s->active) {
        e=ArmSmmu4KObserveStreams(&io,&s->streams,c,&s->route);
        if (e) { s->cleanup=e; goto uncertain; }
        e=ArmSmmu4KFaultStreams(&io,&s->streams,&s->route);
        if (e) { s->cleanup=e; goto uncertain; }
        e=ArmSmmu4KDisableOwnedContext(&io,s->bank,c,1000,&s->context);
        if (e || s->context.must_retain_tables) { s->cleanup=e?e:ArmSmmu4KStateMismatch; goto uncertain; }
        s->active=0;
    }
    if (o->release_lease(o->io.cookie)) goto uncertain;
    s->leased=0; s->retain_resources=0; return ArmSmmu4KSuccess;
uncertain:
    s->quarantined=1;
    gg_record(&s->diag,o->trace,o->io.cookie,GGD_SMMU,s->phase,GGD_RELEASE,0,0,0,0,s->cleanup?s->cleanup:ArmSmmu4KBusy,1);
    return s->cleanup?s->cleanup:(s->cleanup=ArmSmmu4KBusy);
}
static inline ArmSmmu4KStatus gg_smmu_start(const struct gg_smmu_ops *o,
    struct gg_smmu_state *s,const ArmSmmu4KContextConfig *c,unsigned bank,unsigned slot)
{
    ArmSmmu4KStream stream={slot,5};
    ArmSmmu4KStatus e;
    struct gg_smmu_view view={o,s,0,0,0,{{0,0,0}}};
    ArmSmmu4KIo io=gg_smmu_io(&view);
    if (!gg_smmu_valid(o) || !s || !c || !c->lower || !c->lower_tables || c->upper ||
        c->coherent_walk || c->lower->input_bits!=32 || c->lower->output_bits!=36 ||
        !c->asid || c->asid>65535) return ArmSmmu4KArgument;
    if (s->attempted || s->leased || s->quarantined) return ArmSmmu4KBusy;
    s->attempted=1; s->phase=1; s->bank=bank; s->slot=slot;
    if (o->quiescent(o->io.cookie)!=1 || o->lease(o->io.cookie,bank,c->asid,slot))
        return s->error=ArmSmmu4KBusy;
    s->leased=1; s->retain_resources=1;
    e=ArmSmmu4KCheckFaultedStreams(&io,&stream,1,bank,&s->route);
    if (!e && o->publish_tables(o->io.cookie,c->lower,c->lower_tables)) e=ArmSmmu4KIoError;
    if (!e && o->quiescent(o->io.cookie)!=1) e=ArmSmmu4KBusy;
    if (e) { s->error=e; (void)gg_smmu_stop(o,s,c); return e; }
    s->phase=2;
    e=ArmSmmu4KProgramDisabledContext(&io,bank,c,1000,&s->context);
    if (e) {
        s->error=e;
        if (s->context.writes || s->context.must_retain_tables) s->quarantined=1;
        else (void)gg_smmu_stop(o,s,c);
        return e;
    }
    /* A programmed bank must stay leased even if attachment fails. */
    s->phase=3;
    e=ArmSmmu4KAttachStreams(&io,&s->streams,&stream,1,bank,c,&s->route);
    if (e) { s->error=e; s->quarantined=1; return e; }
    s->active=1; s->phase=4; return ArmSmmu4KSuccess;
}
#endif
