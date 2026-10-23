/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#if !defined(GG_GUNYAH_SHARE_STANDALONE)
#include <pexpert/arm64/board_config.h>
#include <sys/param.h>
#include <sys/systm.h>
#endif
#if defined(GG_GUNYAH_SHARE_STANDALONE) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "giulia_gpu_gunyah_share.h"

static int ggsh_lost(const struct ggsh_state *s)
{return __atomic_load_n(&s->loss,__ATOMIC_ACQUIRE)!=0;}
static int ggsh_clock(struct ggsh_state *s)
{
    if(ggsh_lost(s) || s->time_failed)return 0;
    if(!s->timing)return 1;
    uint64_t now=s->owner.rm.now_us(s->owner.rm.cookie);
    if(ggsh_lost(s))return 0;
    if(now<s->last_us || now-s->start_us>=s->timeout_us){s->time_failed=1;return 0;}
    s->last_us=now;return 1;
}
static int ggsh_valid(void *cookie,uint64_t epoch)
{
    struct ggsh_state *s=(struct ggsh_state *)cookie;
    struct ggrm_state *channel=s->channel;
    if(ggsh_lost(s) || s->invalid || s->epoch!=epoch ||
       channel->epoch!=epoch || channel->quarantined ||
       channel->next_sequence!=s->next_sequence || (channel->busy && !s->rpc_active))return 0;
    if(s->owner.valid(s->owner.cookie,epoch)!=1 || !ggsh_clock(s) ||
       s->channel!=channel || s->epoch!=epoch || channel->epoch!=epoch ||
       channel->quarantined || channel->next_sequence!=s->next_sequence ||
       (channel->busy && !s->rpc_active))return 0;
    return s->owner.rm.valid(s->owner.rm.cookie,epoch)==1 && ggsh_clock(s) &&
        s->channel==channel && s->epoch==epoch && channel->epoch==epoch &&
        !channel->quarantined && channel->next_sequence==s->next_sequence &&
        (!channel->busy || s->rpc_active) && !s->invalid && !ggsh_lost(s);
}
static void ggsh_loss(struct ggsh_state *s)
{
    __atomic_store_n(&s->loss,1u,__ATOMIC_RELEASE);s->invalid=1;
    s->shared=0;s->channel->quarantined=1;gg_rq_invalidate(&s->identity);
}
static int ggsh_leave(struct ggsh_state *s,unsigned result)
{
    if(ggsh_lost(s)){ggsh_loss(s);if(result==GGSH_OK)result=GGSH_LOST;}
    s->timing=0;s->last_result=result;
    __atomic_store_n(&s->busy,0u,__ATOMIC_RELEASE);return (int)result;
}
static int ggsh_fail(struct ggsh_state *s,unsigned result)
{ggsh_loss(s);return ggsh_leave(s,result);}
static int ggsh_send(void *cookie,const void *p,size_t n,unsigned flags)
{
    struct ggsh_state *s=(struct ggsh_state *)cookie;
    if(!ggsh_valid(s,s->epoch))return -1;
    int r=s->owner.rm.send(s->owner.rm.cookie,p,n,flags);return ggsh_clock(s)?r:-1;
}
static int ggsh_receive(void *cookie,void *p,size_t n,size_t *bytes)
{
    struct ggsh_state *s=(struct ggsh_state *)cookie;
    if(!ggsh_valid(s,s->epoch))return -1;
    int r=s->owner.rm.receive(s->owner.rm.cookie,p,n,bytes);return ggsh_clock(s)?r:-1;
}
static uint64_t ggsh_now(void *cookie)
{struct ggsh_state *s=(struct ggsh_state *)cookie;(void)ggsh_clock(s);return s->last_us;}
static int ggsh_delay(void *cookie,unsigned us)
{
    struct ggsh_state *s=(struct ggsh_state *)cookie;
    if(!ggsh_valid(s,s->epoch) || us>=s->timeout_us-(unsigned)(s->last_us-s->start_us))return -1;
    int r=s->owner.rm.delay_us(s->owner.rm.cookie,us);return ggsh_clock(s)?r:-1;
}
static int ggsh_notification(void *cookie,uint32_t id,const void *p,size_t n)
{
    struct ggsh_state *s=(struct ggsh_state *)cookie;
    int codec=gg_rq_notification(&s->identity,id,(const uint8_t *)p,n);
    int handled=s->owner.rm.notification(s->owner.rm.cookie,id,p,n);
    if(codec || handled!=1 || !ggsh_clock(s)){ggsh_loss(s);return 0;}return 1;
}
static int ggsh_rpc(struct ggsh_state *s,uint32_t id,size_t n)
{
    if(!ggsh_valid(s,s->epoch))return 0;
    struct ggrm_io io={s,ggsh_valid,ggsh_send,ggsh_receive,ggsh_now,ggsh_delay,ggsh_notification};
    ++s->next_sequence;s->rpc_active=1;
    int result=ggrm_call(&io,s->channel,id,s->request,n,
        s->timeout_us-(unsigned)(s->last_us-s->start_us));
    s->rpc_active=0;return result==GGRM_OK;
}
static int ggsh_authorize(struct ggsh_state *s)
{
    return ggsh_valid(s,s->epoch) && s->owner.authorize(s->owner.cookie,s->epoch,
        s->identity.self_vmid,s->identity.peer_vmid,GGF_CARVEOUT_PHYSICAL,GGF_CARVEOUT_BYTES)==1 &&
        ggsh_clock(s) && ggsh_valid(s,s->epoch);
}
static int ggsh_control(struct ggsh_state *s,int write,struct ggs_regs *frame)
{
    uint8_t *p=(uint8_t *)s->owner.control;
    uint64_t expected_words[40]={0},args[10]={0};
    uint8_t *expected=(uint8_t *)expected_words;
    gg_rq_put64(expected+128,GGF_CARVEOUT_PHYSICAL);gg_rq_put64(expected+136,GGF_CARVEOUT_BYTES);
    gg_rq_put32(expected+192,s->identity.self_vmid);
    gg_rq_put32(expected+256,s->identity.self_vmid);gg_rq_put32(expected+260,6);
    gg_rq_put32(expected+280,s->identity.peer_vmid);gg_rq_put32(expected+284,6);
    args[0]=s->owner.control_pa+128;args[1]=16;args[2]=s->owner.control_pa+192;
    args[3]=4;args[4]=s->owner.control_pa+256;args[5]=48;
    struct ggs_regs built;
    if(!ggs_build_frame(0x42000c16,0x1117,args,s->owner.control_pa,expected_words,&built))return 0;
    if(write){for(unsigned i=0;i<320;i++)p[i]=expected[i];*frame=built;return 1;}
    for(unsigned i=0;i<320;i++)if(p[i]!=expected[i])return 0;
    return 1;
}
static int ggsh_scm_call(void *cookie,struct ggs_regs *r)
{
    struct ggsh_state *s=(struct ggsh_state *)cookie;
    if(!ggsh_valid(s,s->epoch))return -1;
    if(s->owner.scm.prerequisites(s->owner.scm.cookie,s->phase)!=1 || !ggsh_clock(s))return -1;
    if(s->phase==GGSH_ASSIGN && !ggsh_authorize(s))return -1;
    if(!ggsh_valid(s,s->epoch))return -1;
    if(s->phase==GGSH_ASSIGN && !ggsh_control(s,0,NULL))return -1;
    int result=s->owner.scm.call(s->owner.scm.cookie,r);
    return ggsh_valid(s,s->epoch)?result:-1;
}
static int ggsh_scm_delay(void *cookie,unsigned ms)
{
    struct ggsh_state *s=(struct ggsh_state *)cookie;
    if(!ggsh_valid(s,s->epoch) || (uint64_t)ms*1000>=s->timeout_us-(s->last_us-s->start_us))return -1;
    int result=s->owner.scm.delay_ms(s->owner.scm.cookie,ms);
    return ggsh_valid(s,s->epoch)?result:-1;
}
static int ggsh_scm_prerequisites(void *cookie,unsigned operation)
{(void)operation;struct ggsh_state *s=(struct ggsh_state *)cookie;return ggsh_valid(s,s->epoch);}
static int ggsh_scm(struct ggsh_state *s,const struct ggs_regs *frame,int a1)
{
    struct ggs_ops io={s,ggsh_scm_call,ggsh_scm_delay,ggsh_scm_prerequisites};
    return ggs_exchange_frame(&io,&s->scm_state,frame)==GGS_OK &&
        (!a1 || !s->scm_state.last_output.x[1]) && ggsh_valid(s,s->epoch);
}
static int ggsh_disjoint(const void *a,size_t na,const void *b,size_t nb)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a && b && na<=UINTPTR_MAX-x && nb<=UINTPTR_MAX-y &&
        (x+na<=y || y+nb<=x);
}
int ggsh_init(struct ggsh_state *s,const struct ggsh_owner *o,struct ggrm_state *rm,uint64_t epoch)
{
    if(!s || !o || !rm || !epoch || (uintptr_t)s%__alignof__(*s) ||
       (uintptr_t)rm%__alignof__(*rm) || !ggsh_disjoint(s,sizeof(*s),o,sizeof(*o)) ||
       !ggsh_disjoint(s,sizeof(*s),rm,sizeof(*rm)) || !o->valid || !o->authorize || !o->publish ||
       !o->rm.valid || !o->rm.send || !o->rm.receive || !o->rm.now_us || !o->rm.delay_us ||
       !o->rm.notification || !o->scm.call || !o->scm.delay_ms || !o->scm.prerequisites ||
       o->control_bytes!=16384 || ((uintptr_t)o->control&16383) || !o->control_pa ||
       (o->control_pa&16383) || o->control_pa>(UINT64_C(1)<<36)-16384 ||
       (o->control_pa<GGF_CARVEOUT_PHYSICAL+GGF_CARVEOUT_BYTES &&
        GGF_CARVEOUT_PHYSICAL<o->control_pa+16384) ||
       !ggsh_disjoint(s,sizeof(*s),o->control,16384) ||
       !ggsh_disjoint(rm,sizeof(*rm),o->control,16384) ||
       !ggsh_disjoint(o,sizeof(*o),o->control,16384) ||
       rm->epoch!=epoch || rm->busy || rm->quarantined || rm->next_sequence>65529)return GGSH_ARGUMENT;
    unsigned expected=0;
    if(!__atomic_compare_exchange_n(&s->initialized,&expected,2u,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return GGSH_BUSY;
    if(s->epoch || s->attempted || s->invalid || ggsh_lost(s)) {
        __atomic_store_n(&s->initialized,0u,__ATOMIC_RELEASE);return GGSH_ARGUMENT;
    }
    s->owner=*o;s->channel=rm;s->epoch=epoch;s->next_sequence=rm->next_sequence;
    unsigned result=GGSH_OK;
    if(gg_rq_begin(&s->identity,epoch) || !ggsh_valid(s,epoch)){ggsh_loss(s);result=GGSH_LOST;}
    s->last_result=result;__atomic_store_n(&s->initialized,1u,__ATOMIC_RELEASE);return (int)result;
}
int ggsh_run(struct ggsh_state *s,unsigned timeout_us)
{
    if(!s || __atomic_load_n(&s->initialized,__ATOMIC_ACQUIRE)!=1)return GGSH_ARGUMENT;
    unsigned expected=0;
    if(!__atomic_compare_exchange_n(&s->busy,&expected,1u,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return GGSH_BUSY;
    if(!timeout_us || timeout_us>3000000)return ggsh_leave(s,GGSH_ARGUMENT);
    if(s->attempted || s->invalid || ggsh_lost(s))return ggsh_leave(s,GGSH_LOST);
    if(s->channel->busy)return ggsh_leave(s,GGSH_BUSY);
    s->attempted=1;s->phase=GGSH_IDENTIFY;s->timeout_us=timeout_us;
    s->start_us=s->last_us=s->owner.rm.now_us(s->owner.rm.cookie);s->timing=1;
    for(unsigned i=0;i<4;i++) {
        uint32_t id=0;size_t bytes=0;
        if(gg_rq_build_query(&s->identity,&id,s->request,sizeof(s->request),&bytes))return ggsh_fail(s,GGSH_CODEC);
        if(!ggsh_rpc(s,id,bytes))return ggsh_fail(s,GGSH_RPC);
        if(gg_rq_accept_reply(&s->identity,s->epoch,id,s->channel->reply.data,s->channel->reply.bytes))return ggsh_fail(s,GGSH_CODEC);
    }
    /* Match the source u64 VM bitmap domain; never shift by dynamic IDs>=64. */
    if(s->identity.self_vmid>=64 || s->identity.peer_vmid>=64 || !ggsh_authorize(s))return ggsh_fail(s,GGSH_AUTHORITY);
    s->phase=GGSH_PROBE;
    struct ggs_regs convention={{0xc2000601,1,0x02000601,0,0,0,0,0}};
    if(!ggsh_scm(s,&convention,0) || s->scm_state.last_output.x[1]!=1)return ggsh_fail(s,GGSH_SCM);
    struct ggs_regs available={{0x42000601,1,0x02000c16,0,0,0,0,0}};
    if(!ggsh_scm(s,&available,0) || s->scm_state.last_output.x[1]!=1)return ggsh_fail(s,GGSH_SCM);
    struct ggs_regs frame;
    if(!ggsh_authorize(s) || !ggsh_control(s,1,&frame))return ggsh_fail(s,GGSH_AUTHORITY);
    if(s->owner.publish(s->owner.cookie,s->owner.control,320)!=1 || !ggsh_valid(s,s->epoch))return ggsh_fail(s,GGSH_AUTHORITY);
    s->phase=GGSH_ASSIGN;s->retained=1;
    if(!ggsh_scm(s,&frame,1))return ggsh_fail(s,GGSH_SCM);
    s->assigned=1;s->phase=GGSH_SHARE;
    size_t bytes=gg_mem_build_share(s->request,sizeof(s->request),s->identity.self_vmid,s->identity.peer_vmid);
    if(!bytes || !ggsh_rpc(s,UINT32_C(0x51000013),bytes))return ggsh_fail(s,GGSH_RPC);
    if(gg_mem_parse_share_reply(s->channel->reply.data,s->channel->reply.bytes,&s->parcel))return ggsh_fail(s,GGSH_CODEC);
    s->phase=GGSH_LOOKUP;
    bytes=gg_rq_build_mem_lookup(s->request,sizeof(s->request),s->identity.self_vmid,s->identity.peer_vmid);
    if(!bytes || !ggsh_rpc(s,GGRQ_MEM_LOOKUP,bytes))return ggsh_fail(s,GGSH_RPC);
    uint32_t parcel;
    if(gg_rq_parse_mem_lookup(s->channel->reply.data,s->channel->reply.bytes,&parcel) || parcel!=s->parcel)return ggsh_fail(s,GGSH_CODEC);
    s->phase=GGSH_RECHECK;
    bytes=gg_rq_build_vmid(s->request,sizeof(s->request),s->identity.peer_vmid);
    if(!bytes || !ggsh_rpc(s,GGRQ_VM_GET_STATE,bytes))return ggsh_fail(s,GGSH_RPC);
    struct gg_rq_vm_state vm;
    if(gg_rq_parse_state(s->channel->reply.data,s->channel->reply.bytes,&vm) || vm.vm_status!=GGRQ_READY)return ggsh_fail(s,GGSH_CODEC);
    if(!ggsh_valid(s,s->epoch))return ggsh_fail(s,GGSH_LOST);
    s->phase=GGSH_COMPLETE;s->shared=1;return ggsh_leave(s,GGSH_OK);
}
int ggsh_invalidate(struct ggsh_state *s)
{
    if(!s)return GGSH_ARGUMENT;
    unsigned init=__atomic_load_n(&s->initialized,__ATOMIC_ACQUIRE);
    if(init!=1 && init!=2)return GGSH_ARGUMENT;
    __atomic_store_n(&s->loss,1u,__ATOMIC_RELEASE);
    if(init==1) {
        unsigned expected=0;
        if(__atomic_compare_exchange_n(&s->busy,&expected,1u,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) {
            ggsh_loss(s);return ggsh_leave(s,GGSH_LOST);
        }
    }
    return GGSH_LOST;
}
#endif
