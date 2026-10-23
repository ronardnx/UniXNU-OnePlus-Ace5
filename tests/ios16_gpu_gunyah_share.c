/* Offline owned RM/SCM peer fixtures. No SMC, HVC or carveout access occurs. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_gunyah_share.h"

static const uint32_t query_ids[]={GGRQ_VM_GET_VMID,GGRQ_VM_LOOKUP_NAME,
    GGRQ_VM_GET_ID,GGRQ_VM_GET_STATE,GGMEM_SHARE,GGRQ_MEM_LOOKUP,GGRQ_VM_GET_STATE};
enum { EPOCH=71,FIRST_SEQ=41,MAX_SCM=96 };
struct fixture {
    struct ggsh_state *s;
    struct ggrm_state *rm;
    struct ggsh_owner owner;
    uint8_t *control;
    uint64_t tick;
    uint16_t self,peer,seq,first_seq;
    uint32_t parcel;
    unsigned live,rm_calls,receives,scm_calls,assign_calls,probe_slot,authorizes,publishes;
    unsigned full,empty,rm_fault_step,rm_fault,notify_step,notify_sent,notify_peer,notifications;
    unsigned authorize_fail_at,publish_fail,corrupt_offset,corrupt_control,prereq_fail_phase;
    unsigned transport_fail_at,delay_fail,reenter,late_phase,rewind_phase,loss_phase;
    unsigned late_scm_at,loss_scm_at;
    unsigned valid_mutation,mutate_phase,mutated,assign_valids,control_on_valid;
    uint64_t last_status,last_cookie;
    unsigned scripted[MAX_SCM];
    struct ggs_regs response[MAX_SCM],input[MAX_SCM];
};
static uint16_t u16(const uint8_t *p){return (uint16_t)(p[0]|((unsigned)p[1]<<8));}
static uint32_t u32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t u64(const uint8_t *p){return u32(p)|((uint64_t)u32(p+4)<<32);}
static void put32(uint8_t *p,uint32_t x)
{for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(x>>(8*i));}
static int valid(void *cookie,uint64_t epoch)
{
    struct fixture *f=cookie;
    if(f->s->phase==GGSH_ASSIGN) {
        ++f->assign_valids;
        if(f->control_on_valid==f->assign_valids)f->control[284]^=1;
    }
    if(f->valid_mutation && !f->mutated && f->s->phase==f->mutate_phase) {
        f->mutated=1;
        if(f->valid_mutation==1)++f->rm->epoch;
        if(f->valid_mutation==2)f->rm->quarantined=1;
        if(f->valid_mutation==3)++f->rm->next_sequence;
        if(f->valid_mutation==4)f->rm->busy=1;
        if(f->valid_mutation==5)++f->s->epoch;
    }
    if(f->loss_phase && f->s->phase==f->loss_phase) {
        f->loss_phase=0;assert(ggsh_invalidate(f->s)==GGSH_LOST);
    }
    return f->live && epoch==EPOCH;
}
static uint64_t now(void *cookie)
{
    struct fixture *f=cookie;
    if(f->s->timing && f->late_phase && f->s->phase==f->late_phase)
        f->tick=f->s->start_us+f->s->timeout_us;
    if(f->s->timing && f->rewind_phase && f->s->phase==f->rewind_phase) {
        f->rewind_phase=0;return f->s->last_us-1;
    }
    return f->tick++;
}
static int delay_us(void *cookie,unsigned us)
{struct fixture *f=cookie;f->tick+=us;return (int)f->delay_fail;}
static int delay_ms(void *cookie,unsigned ms)
{assert(ms==30);return delay_us(cookie,ms*1000);}
static int authorize(void *cookie,uint64_t epoch,uint16_t self,uint16_t peer,uint64_t pa,uint32_t bytes)
{
    struct fixture *f=cookie;++f->authorizes;
    assert(epoch==EPOCH && self==f->self && peer==f->peer);
    assert(pa==GGF_CARVEOUT_PHYSICAL && bytes==GGF_CARVEOUT_BYTES);
    return f->authorize_fail_at!=f->authorizes;
}
static int publish(void *cookie,const void *p,size_t bytes)
{
    struct fixture *f=cookie;++f->publishes;
    assert(p==f->control && bytes==320 && !f->s->assigned && !f->s->retained);
    if(f->corrupt_control)f->control[f->corrupt_offset]^=1;
    return !f->publish_fail;
}
static int notification(void *cookie,uint32_t id,const void *p,size_t bytes)
{
    struct fixture *f=cookie;
    assert(id==GGRQ_VM_STATUS && p && bytes==8);++f->notifications;return 1;
}
static void request_payload(struct fixture *f,const uint8_t *p,size_t bytes,unsigned step)
{
    if(step==0)assert(bytes==8);
    else if(step==1) {
        const uint8_t name[]={13,0,0,0,'q','c','o','m',',','c','p','u','s','y','s','v','m',0,0,0};
        assert(bytes==28 && !memcmp(p+8,name,sizeof(name)));
    } else if(step==4 || step==5) {
        assert(bytes==52 && u32(p+8)==0 && u32(p+12)==5 && u32(p+16)==2);
        assert(u16(p+20)==f->self && p[22]==6 && !p[23]);
        assert(u16(p+24)==f->peer && p[26]==6 && !p[27]);
        assert(u32(p+28)==1 && u64(p+32)==GGF_CARVEOUT_PHYSICAL);
        assert(u64(p+40)==GGF_CARVEOUT_BYTES && u32(p+48)==0);
        assert(f->s->assigned && f->s->retained);
    } else assert(bytes==12 && u32(p+8)==f->peer);
}
static int send_frame(void *cookie,const void *data,size_t bytes,unsigned flags)
{
    struct fixture *f=cookie;const uint8_t *p=data;
    if(f->full){--f->full;return 1;}
    assert(f->rm_calls<7 && p[0]==0x21 && p[1]==1 && flags==1);
    assert(u32(p+4)==query_ids[f->rm_calls]);f->seq=u16(p+2);
    assert(f->seq==f->first_seq+f->rm_calls);
    request_payload(f,p,bytes,f->rm_calls);++f->rm_calls;
    if(f->reenter)assert(ggsh_run(f->s,100000)==GGSH_BUSY);
    return 0;
}
static int receive_frame(void *cookie,void *data,size_t capacity,size_t *bytes)
{
    struct fixture *f=cookie;uint8_t *p=data;unsigned step=f->rm_calls;
    assert(capacity==240 && step && step<=7);++f->receives;
    if(f->empty)return 1;
    memset(p,0,capacity);p[0]=0x21;p[1]=2;
    p[2]=(uint8_t)f->seq;p[3]=(uint8_t)(f->seq>>8);put32(p+4,query_ids[step-1]);*bytes=16;
    if(f->notify_step==step && !f->notify_sent++) {
        p[1]=3;put32(p+4,GGRQ_VM_STATUS);p[8]=(uint8_t)(f->notify_peer?f->peer:9);p[12]=2;
        return 0;
    }
    switch(step) {
    case 1:p[12]=(uint8_t)f->self;break;
    case 2:p[12]=1;p[16]=(uint8_t)f->peer;*bytes=20;break;
    case 3: {
        const uint8_t identity[]={1,0,0,0,2,0,13,0,
            'q','c','o','m',',','c','p','u','s','y','s','v','m',0,0,0};
        memcpy(p+12,identity,sizeof(identity));*bytes=36;break;
    }
    case 4:case 7:p[12]=2;p[13]=4;break;
    case 5:case 6:put32(p+12,f->parcel);break;
    default:assert(0);
    }
    if(f->rm_fault_step==step) {
        if(f->rm_fault==1)put32(p+8,3);
        if(f->rm_fault==2)--*bytes;
        if(f->rm_fault==3)p[2]^=1;
        if(f->rm_fault==4)f->live=0;
        if(f->rm_fault==5)return -1;
        if(f->rm_fault==6)f->tick=f->s->start_us+f->s->timeout_us;
        if(f->rm_fault==7)put32(p+12,f->parcel+1);
        if(f->rm_fault==8)p[12]=3;
    }
    return 0;
}
static int prerequisites(void *cookie,unsigned phase)
{
    struct fixture *f=cookie;
    assert(phase==GGSH_PROBE || phase==GGSH_ASSIGN);
    return f->prereq_fail_phase!=phase;
}
static void control_exact(const struct fixture *f)
{
    uint8_t expected[320]={0};
    /* Verify the extended argument area, all alignment padding, 16-byte
     * region, u32 source and both 24-byte RW destination records. */
    for(unsigned i=0;i<8;i++) {
        expected[i]=(uint8_t)(UINT64_C(4)>>(8*i));
        expected[8+i]=(uint8_t)((f->owner.control_pa+256)>>(8*i));
        expected[16+i]=(uint8_t)(UINT64_C(48)>>(8*i));
        expected[128+i]=(uint8_t)((uint64_t)GGF_CARVEOUT_PHYSICAL>>(8*i));
        expected[136+i]=(uint8_t)((uint64_t)GGF_CARVEOUT_BYTES>>(8*i));
    }
    put32(expected+192,f->self);put32(expected+256,f->self);put32(expected+260,6);
    put32(expected+280,f->peer);put32(expected+284,6);
    assert(!memcmp(f->control,expected,sizeof(expected)));
    for(unsigned i=320;i<16384;i++)assert(f->control[i]==0xa7);
}
static int scm_call(void *cookie,struct ggs_regs *r)
{
    struct fixture *f=cookie;assert(f->scm_calls<MAX_SCM);
    unsigned at=f->scm_calls++;f->input[at]=*r;
    uint64_t original;
    if(f->s->phase==GGSH_ASSIGN) {
        ++f->assign_calls;assert(f->s->retained && !f->s->assigned && f->publishes==1);
        assert(f->authorizes>=3);control_exact(f);original=0x42000c16;
        assert(r->x[1]==0x1117 && r->x[2]==f->owner.control_pa+128 && r->x[3]==16);
        assert(r->x[4]==f->owner.control_pa+192 && r->x[5]==f->owner.control_pa);
    } else {
        assert(f->s->phase==GGSH_PROBE && f->probe_slot<2 && !f->s->retained);
        original=f->probe_slot?0x42000601:0xc2000601;
        assert(r->x[1]==1 && r->x[2]==(f->probe_slot?0x02000c16:0x02000601));
        assert(!r->x[3] && !r->x[4] && !r->x[5]);
    }
    assert(r->x[0]==(f->last_status==1?1:original));
    assert(r->x[6]==(f->last_status==1?f->last_cookie:0) && !r->x[7]);
    struct ggs_regs out={{0,f->s->phase==GGSH_ASSIGN?0:1,
        UINT64_C(0xabcdef0012345678),0x33,0x44,0x55,0x66,UINT64_C(0xfedcba9876543210)}};
    if(f->scripted[at])out=f->response[at];
    *r=out;f->last_status=out.x[0];f->last_cookie=out.x[6];
    if(!out.x[0] && f->s->phase==GGSH_PROBE)++f->probe_slot;
    if(f->late_scm_at==at+1)f->tick=f->s->start_us+f->s->timeout_us;
    if(f->loss_scm_at==at+1)assert(ggsh_invalidate(f->s)==GGSH_LOST);
    if(f->reenter)assert(ggsh_run(f->s,100000)==GGSH_BUSY);
    return f->transport_fail_at==at+1?-1:0;
}
static void setup(struct fixture *f)
{
    memset(f,0,sizeof(*f));f->self=3;f->peer=5;f->live=1;f->parcel=0x98765432;
    f->s=calloc(1,sizeof(*f->s));f->rm=calloc(1,sizeof(*f->rm));
    f->control=aligned_alloc(16384,16384);assert(f->s && f->rm && f->control);
    memset(f->control,0xa7,16384);assert(ggrm_init(f->rm,EPOCH)==GGRM_OK);
    f->rm->next_sequence=f->first_seq=FIRST_SEQ;
    struct ggsh_owner owner={f,valid,authorize,publish,
        {f,valid,send_frame,receive_frame,now,delay_us,notification},
        {f,scm_call,delay_ms,prerequisites},f->control,0x24000000,16384};
    f->owner=owner;
}
static void start(struct fixture *f)
{setup(f);assert(ggsh_init(f->s,&f->owner,f->rm,EPOCH)==GGSH_OK);}
static void cleanup(struct fixture *f)
{free(f->control);free(f->rm);free(f->s);}
static void reply(struct fixture *f,unsigned at,uint64_t status,uint64_t value,uint64_t cookie)
{
    assert(at<MAX_SCM);f->scripted[at]=1;
    const struct ggs_regs out={{status,value,UINT64_C(0xabcdef0012345678),0x33,0x44,0x55,cookie,
        UINT64_C(0xfedcba9876543210)}};f->response[at]=out;
}
static void failed(struct fixture *f,unsigned retained)
{
    assert(f->s->invalid && f->rm->quarantined && f->s->retained==retained && !f->s->shared);
    unsigned rm=f->rm_calls,scm=f->scm_calls;
    assert(ggsh_run(f->s,1000000)==GGSH_LOST);
    assert(f->rm_calls==rm && f->scm_calls==scm); /* No rollback/retry RPC. */
}
static void success_and_identity(void)
{
    struct fixture f;start(&f);f.reenter=1;f.full=2;f.notify_step=4;
    /* Init freezes all callback descriptors; later caller-side mutation
     * cannot replace the RM/SCM owner used by this operation. */
    f.owner.valid=NULL;f.owner.authorize=NULL;f.owner.publish=NULL;
    f.owner.rm.send=NULL;f.owner.scm.call=NULL;
    assert(ggsh_run(f.s,1000000)==GGSH_OK);
    assert(f.rm_calls==7 && f.scm_calls==3 && f.assign_calls==1 && f.notifications==1);
    assert(f.rm->next_sequence==FIRST_SEQ+7 && f.s->phase==GGSH_COMPLETE);
    assert(f.s->assigned && f.s->shared && f.s->retained && f.s->parcel==f.parcel);
    assert(!f.rm->quarantined && !f.s->invalid);
    unsigned old=f.rm_calls;assert(ggsh_run(f.s,1000000)==GGSH_LOST && f.rm_calls==old);
    cleanup(&f);
    const uint16_t pairs[][2]={{7,11},{31,63},{63,2}};
    for(unsigned i=0;i<sizeof(pairs)/sizeof(pairs[0]);i++) {
        setup(&f);f.self=pairs[i][0];f.peer=pairs[i][1];f.parcel=i==0?0:UINT32_MAX;
        assert(ggsh_init(f.s,&f.owner,f.rm,EPOCH)==GGSH_OK);
        assert(ggsh_run(f.s,1000000)==GGSH_OK && f.s->parcel==f.parcel);cleanup(&f);
    }
    setup(&f);f.peer=64;assert(ggsh_init(f.s,&f.owner,f.rm,EPOCH)==GGSH_OK);
    assert(ggsh_run(f.s,1000000)==GGSH_AUTHORITY && f.rm_calls==4 && !f.scm_calls);
    failed(&f,0);cleanup(&f);
    start(&f);assert(ggsh_run(f.s,1000000)==GGSH_OK);
    assert(ggsh_invalidate(f.s)==GGSH_LOST);
    assert(!f.s->shared && !f.s->identity.self_vmid && !f.s->identity.peer_vmid);
    failed(&f,1);cleanup(&f);
}
static void rm_failures(void)
{
    for(unsigned step=1;step<=7;step++)for(unsigned fault=1;fault<=6;fault++) {
        struct fixture f;start(&f);f.rm_fault_step=step;f.rm_fault=fault;
        assert(ggsh_run(f.s,1000000)!=GGSH_OK);
        assert(f.rm_calls==step && f.assign_calls==(step>=5));failed(&f,step>=5);cleanup(&f);
    }
    struct fixture f;start(&f);f.rm_fault_step=6;f.rm_fault=7;
    assert(ggsh_run(f.s,1000000)==GGSH_CODEC && f.rm_calls==6);failed(&f,1);cleanup(&f);
    start(&f);f.rm_fault_step=7;f.rm_fault=8;
    assert(ggsh_run(f.s,1000000)==GGSH_CODEC);failed(&f,1);cleanup(&f);
    for(unsigned step=1;step<=7;step++) {
        start(&f);f.notify_step=step;f.notify_peer=1;
        assert(ggsh_run(f.s,1000000)!=GGSH_OK && f.notifications==1);
        assert(f.rm_calls==step);failed(&f,step>=5);cleanup(&f);
    }
    start(&f);f.empty=1;
    assert(ggsh_run(f.s,1000)!=GGSH_OK && f.rm_calls==1 && !f.scm_calls);
    failed(&f,0);cleanup(&f);
    start(&f);f.full=10000;
    assert(ggsh_run(f.s,1000)!=GGSH_OK && !f.rm_calls && !f.scm_calls);
    failed(&f,0);cleanup(&f);
}
static void scm_statuses_and_retries(void)
{
    static const uint64_t bad[]={2,3,UINT64_MAX,UINT64_C(0x100000000),UINT64_C(0x100000001)};
    for(unsigned stage=0;stage<3;stage++)for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        struct fixture f;start(&f);reply(&f,stage,bad[i],0x1234,0x123456789abcdef0);
        assert(ggsh_run(f.s,1000000)==GGSH_SCM && f.scm_calls==stage+1);
        assert(f.s->scm_state.last_output.x[0]==bad[i] && f.s->scm_state.last_output.x[1]==0x1234);
        assert(f.s->scm_state.last_output.x[6]==UINT64_C(0x123456789abcdef0));
        assert(f.s->scm_state.last_output.x[7]==UINT64_C(0xfedcba9876543210));
        failed(&f,stage==2);cleanup(&f);
    }
    for(unsigned stage=0;stage<3;stage++) {
        struct fixture f;start(&f);f.transport_fail_at=stage+1;
        assert(ggsh_run(f.s,1000000)==GGSH_SCM);failed(&f,stage==2);cleanup(&f);
        start(&f);reply(&f,stage,0,stage==2?0x100000000:0,0);
        assert(ggsh_run(f.s,1000000)==GGSH_SCM);failed(&f,stage==2);cleanup(&f);
    }
    struct fixture f;start(&f);reply(&f,2,1,0xbad,0x123456789abcdef0);
    reply(&f,3,(uint64_t)(int64_t)-12,0xbeef,0x9999);
    assert(ggsh_run(f.s,1000000)==GGSH_OK && f.assign_calls==3);
    assert(f.s->scm_state.interrupt_resumes==1 && f.s->scm_state.busy_retries==1);
    assert(f.input[3].x[0]==1 && f.input[3].x[6]==UINT64_C(0x123456789abcdef0));
    assert(f.input[4].x[0]==0x42000c16 && !f.input[4].x[6]);cleanup(&f);
    for(unsigned stage=0;stage<2;stage++) {
        start(&f);reply(&f,stage,1,0xbad,0x123456789abcdef0);
        reply(&f,stage+1,(uint64_t)(int64_t)-12,0xbeef,0x9999);
        assert(ggsh_run(f.s,1000000)==GGSH_OK && f.scm_calls==5 && f.assign_calls==1);
        assert(f.s->scm_state.interrupt_resumes==1 && f.s->scm_state.busy_retries==1);
        cleanup(&f);
    }
    start(&f);reply(&f,2,(uint64_t)(int64_t)-12,0,0);f.authorize_fail_at=4;
    assert(ggsh_run(f.s,1000000)==GGSH_SCM && f.assign_calls==1 && !f.s->assigned);
    failed(&f,1);cleanup(&f);
    for(unsigned kind=0;kind<2;kind++) {
        start(&f);unsigned bound=kind?GGS_BUSY_RETRIES:GGS_INTERRUPT_RESUMES;
        for(unsigned i=0;i<=bound;i++)reply(&f,2+i,kind?UINT64_C(0xfffffff4):1,0,i+100);
        assert(ggsh_run(f.s,2000000)==GGSH_SCM);
        assert(f.assign_calls==bound+1 && f.s->scm_state.quarantined);failed(&f,1);cleanup(&f);
    }
    start(&f);reply(&f,2,UINT64_C(0xfffffff4),0,0);f.delay_fail=1;
    assert(ggsh_run(f.s,1000000)==GGSH_SCM);failed(&f,1);cleanup(&f);
}
static void authority_control_and_time(void)
{
    struct fixture f;
    for(unsigned at=1;at<=3;at++) {
        start(&f);f.authorize_fail_at=at;
        assert(ggsh_run(f.s,1000000)!=GGSH_OK && !f.assign_calls);
        failed(&f,at==3);cleanup(&f);
    }
    start(&f);f.publish_fail=1;
    assert(ggsh_run(f.s,1000000)==GGSH_AUTHORITY && !f.assign_calls);failed(&f,0);cleanup(&f);
    const unsigned offsets[]={0,8,16,24,55,56,127,128,135,136,143,191,192,195,196,
        255,256,260,264,272,276,280,284,288,304,319};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
        start(&f);f.corrupt_control=1;f.corrupt_offset=offsets[i];
        assert(ggsh_run(f.s,1000000)==GGSH_SCM && !f.assign_calls);failed(&f,1);cleanup(&f);
    }
    for(unsigned phase=GGSH_PROBE;phase<=GGSH_RECHECK;phase++) {
        start(&f);f.late_phase=phase;assert(ggsh_run(f.s,1000000)!=GGSH_OK);
        failed(&f,phase>=GGSH_ASSIGN);cleanup(&f);
        start(&f);f.rewind_phase=phase;assert(ggsh_run(f.s,1000000)!=GGSH_OK);
        failed(&f,phase>=GGSH_ASSIGN);cleanup(&f);
        start(&f);f.loss_phase=phase;assert(ggsh_run(f.s,1000000)!=GGSH_OK);
        failed(&f,phase>=GGSH_ASSIGN);cleanup(&f);
    }
    for(unsigned at=1;at<=3;at++) {
        start(&f);f.late_scm_at=at;
        assert(ggsh_run(f.s,1000000)==GGSH_SCM);failed(&f,at==3);cleanup(&f);
        start(&f);f.loss_scm_at=at;
        assert(ggsh_run(f.s,1000000)==GGSH_SCM);failed(&f,at==3);cleanup(&f);
    }
    for(unsigned phase=GGSH_PROBE;phase<=GGSH_ASSIGN;phase++) {
        start(&f);f.prereq_fail_phase=phase;
        assert(ggsh_run(f.s,1000000)==GGSH_SCM);failed(&f,phase==GGSH_ASSIGN);cleanup(&f);
    }
    /* Host callbacks returning 1 must not conceal a changed epoch, channel
     * quarantine, sequence consumed by another caller or competing RPC. */
    for(unsigned phase=GGSH_PROBE;phase<=GGSH_ASSIGN;phase++)for(unsigned mutation=1;mutation<=5;mutation++) {
        start(&f);f.valid_mutation=mutation;f.mutate_phase=phase;
        assert(ggsh_run(f.s,1000000)!=GGSH_OK && f.mutated && !f.assign_calls);
        failed(&f,phase==GGSH_ASSIGN);cleanup(&f);
    }
    /* The final RM lease callback before native assignment corrupts the
     * published peer permissions. A check performed only before that last
     * callback would incorrectly allow the SCM side effect. */
    start(&f);f.control_on_valid=8;
    assert(ggsh_run(f.s,1000000)==GGSH_SCM && f.assign_valids>=8 && !f.assign_calls);
    failed(&f,1);cleanup(&f);
}
static void arguments_and_sequence(void)
{
    struct fixture f;start(&f);
    assert(ggsh_run(f.s,0)==GGSH_ARGUMENT && !f.s->attempted);
    assert(ggsh_run(f.s,3000001)==GGSH_ARGUMENT && !f.s->attempted);
    f.rm->busy=1;assert(ggsh_run(f.s,1000000)==GGSH_BUSY && !f.s->attempted);
    f.rm->busy=0;assert(ggsh_run(f.s,1000000)==GGSH_OK);cleanup(&f);
    assert(ggsh_run(NULL,100)==GGSH_ARGUMENT && ggsh_invalidate(NULL)==GGSH_ARGUMENT);
    for(unsigned mode=0;mode<9;mode++) {
        setup(&f);
        if(mode==0)f.owner.valid=NULL;
        if(mode==1)f.owner.authorize=NULL;
        if(mode==2)f.owner.publish=NULL;
        if(mode==3)f.owner.control_bytes=320;
        if(mode==4)f.owner.control_pa=GGF_CARVEOUT_PHYSICAL&~UINT64_C(16383);
        if(mode==5)f.owner.control_pa++;
        if(mode==6)f.owner.control=(uint8_t *)f.owner.control+1;
        if(mode==7)f.rm->epoch++;
        if(mode==8)f.rm->next_sequence=65530;
        int result=ggsh_init(f.s,&f.owner,f.rm,EPOCH);
        if(result!=GGSH_ARGUMENT)fprintf(stderr,"invalid init mode=%u result=%d\n",mode,result);
        assert(result==GGSH_ARGUMENT && !f.rm_calls && !f.scm_calls);
        cleanup(&f);
    }
    setup(&f);f.rm->next_sequence=f.first_seq=65529;
    assert(ggsh_init(f.s,&f.owner,f.rm,EPOCH)==GGSH_OK);
    assert(ggsh_run(f.s,1000000)==GGSH_OK && f.rm->next_sequence==65536);
    cleanup(&f);
}
int main(void)
{
    success_and_identity();rm_failures();scm_statuses_and_retries();
    authority_control_and_time();arguments_and_sequence();
    puts("GPU Gunyah primary share: actual RM/SCM wire flow, exact descriptors, dynamic identities, retention, bounded retries, errors and invalidation passed (offline only)");
    return 0;
}
