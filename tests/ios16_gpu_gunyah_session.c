/* Synthetic RM peer and HVC only. No device access or fence READY proof. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_gunyah_session.h"

static const uint32_t ids[]={0x56000024,0x56000013,0x56000010,0x56000017,
    0x5100001a,0x56000020,0x56000017};
struct fixture {
    struct ggrs_session *session;
    struct ggrm_io io;
    unsigned calls,receives,notifications,live,delay_fail,empty,full;
    unsigned fault_step,fault,notify_step,notify_sent,reenter,owner_reject;
    unsigned init_reenter,loss_valid,loss_notification,rewind,late_final,late_send;
    unsigned idle_mode,idle_frames,idle_index,idle_fault;
    uint8_t idle_wire[4][GGRM_FRAME];
    size_t idle_bytes[4];
    uint16_t seq;
    uint64_t tick;
};
static uint16_t u16(const uint8_t *p){return (uint16_t)(p[0]|((unsigned)p[1]<<8));}
static uint32_t u32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(uint8_t *p,uint32_t n)
{for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i));}
static int valid(void *cookie,uint64_t epoch)
{
    struct fixture *f=cookie;
    if(f->init_reenter && f->session->initialized==2) {
        struct gg_rq_snapshot out;
        assert(ggrs_init(f->session,&f->io,37)==GGRS_BUSY);
        assert(ggrs_probe(f->session,10000)==GGRS_BUSY);
        assert(ggrs_poll(f->session,8,10000)==GGRS_BUSY);
        assert(ggrs_copy_snapshot(f->session,&out)==GGRS_BUSY);
    }
    if(f->loss_valid) {
        f->loss_valid=0;assert(ggrs_invalidate(f->session)==GGRS_INVALID);
    }
    if(f->session->timing &&
       ((f->late_final && f->session->snapshot.step==GGRQ_STEP_COMPLETE) ||
        (f->late_send && f->session->rm.busy && !f->calls)))
        f->tick=f->session->start_us+f->session->timeout_us;
    return f->live && epoch==37;
}
static uint64_t now_us(void *cookie)
{
    struct fixture *f=cookie;
    if(f->rewind && f->session->snapshot.step==1) {
        f->rewind=0;return f->session->last_us-1;
    }
    return f->tick++;
}
static int delay_us(void *cookie,unsigned us)
{struct fixture *f=cookie;f->tick+=us;return (int)f->delay_fail;}
static int notification(void *cookie,uint32_t id,const void *p,size_t bytes)
{
    struct fixture *f=cookie;
    assert(p && bytes && id);++f->notifications;
    if(f->reenter) {
        struct gg_rq_snapshot out;
        assert(ggrs_probe(f->session,10000)==GGRS_BUSY);
        assert(ggrs_poll(f->session,8,10000)==GGRS_BUSY);
        assert(ggrs_copy_snapshot(f->session,&out)==GGRS_BUSY);
    }
    if(f->loss_notification)assert(ggrs_invalidate(f->session)==GGRS_INVALID);
    return !f->owner_reject;
}
static int send_frame(void *cookie,const void *data,size_t bytes,unsigned flags)
{
    struct fixture *f=cookie;const uint8_t *p=data;
    if(f->full){--f->full;return 1;}
    assert(f->calls<7 && p[0]==0x21 && p[1]==1 && flags==1);
    assert(u32(p+4)==ids[f->calls]);f->seq=u16(p+2);
    if(f->calls==0)assert(bytes==8);
    else if(f->calls==1) {
        const uint8_t lookup[]={13,0,0,0,'q','c','o','m',',','c','p','u','s','y','s','v','m',0,0,0};
        assert(bytes==28 && !memcmp(p+8,lookup,20));
    } else if(f->calls==4) {
        assert(bytes==52 && u32(p+12)==5 && u16(p+20)==0x45 && u16(p+24)==0x73);
        assert(u32(p+32)==0xd4e23000 && u32(p+40)==0x2dd000);
    } else assert(bytes==12 && u32(p+8)==(f->calls==5?0:0x73));
    ++f->calls;return 0;
}
static int receive_frame(void *cookie,void *data,size_t capacity,size_t *bytes)
{
    struct fixture *f=cookie;uint8_t *p=data;unsigned step=f->calls;
    assert(capacity==240);++f->receives;
    if(f->idle_mode) {
        if(f->idle_fault==1)return -1;
        if(f->idle_fault==2)f->live=0;
        if(f->idle_fault==3)f->tick=f->session->start_us+f->session->timeout_us;
        if(f->idle_index==f->idle_frames) {
            if(f->idle_fault==4)*bytes=1;
            return 1;
        }
        assert(f->idle_index<4);
        *bytes=f->idle_bytes[f->idle_index];
        memcpy(p,f->idle_wire[f->idle_index],*bytes);++f->idle_index;return 0;
    }
    if(f->empty)return 1;
    memset(p,0,capacity);p[0]=0x21;p[1]=2;p[2]=(uint8_t)f->seq;p[3]=(uint8_t)(f->seq>>8);
    put32(p+4,ids[step-1]);*bytes=16;
    if(f->notify_step==step && !f->notify_sent++) {
        p[1]=3;put32(p+4,0x56100008);p[8]=0x74;p[12]=2;*bytes=16;
        if(f->fault==7)p[8]=0x73;
        return 0;
    }
    switch(step) {
    case 1:p[12]=0x45;break;
    case 2:p[12]=1;p[16]=0x73;*bytes=20;break;
    case 3: {
        const uint8_t identity[]={1,0,0,0,2,0,13,0,
            'q','c','o','m',',','c','p','u','s','y','s','v','m',0,0,0};
        memcpy(p+12,identity,24);*bytes=36;break;
    }
    case 4:case 7:p[12]=2;p[13]=4;break;
    case 5:put32(p+12,0x12345678);break;
    case 6:
        put32(p+12,1);p[16]=1;p[18]=0x73;put32(p+20,0x5678);put32(p+24,6);
        put32(p+28,0x9abcdef0);put32(p+32,0x12345678);
        put32(p+36,0x9876);put32(p+40,456);*bytes=60;break;
    default:assert(0);
    }
    if(f->fault_step==step) {
        if(f->fault==1)put32(p+8,3);       /* Explicit remote error. */
        if(f->fault==2)--*bytes;            /* Well-framed invalid codec. */
        if(f->fault==3)p[2]^=1;            /* Reply from another request. */
        if(f->fault==4)f->live=0;          /* Loss after receive. */
        if(f->fault==5)return -1;          /* Uncertain transport. */
        if(f->fault==6)f->tick=1000000;    /* Whole transcript deadline. */
    }
    return 0;
}
static void setup(struct fixture *f)
{
    memset(f,0,sizeof(*f));f->live=1;
    f->session=calloc(1,sizeof(*f->session));assert(f->session);
    struct ggrm_io io={f,valid,send_frame,receive_frame,now_us,delay_us,notification};f->io=io;
}
static void start(struct fixture *f)
{setup(f);assert(ggrs_init(f->session,&f->io,37)==GGRS_OK);}
static void transcript(struct fixture *f)
{
    assert(ggrs_probe(f->session,100000)==GGRS_OK);
    assert(f->calls==7 && f->session->rm.next_sequence==7);
    struct gg_rq_snapshot out;
    assert(ggrs_copy_snapshot(f->session,&out)==GGRS_OK);
    assert(gg_rq_snapshot_complete(&out) && out.epoch==37 && out.self_vmid==0x45 &&
        out.peer_vmid==0x73 && out.parcel_handle==0x12345678 &&
        out.doorbell.capid==UINT64_C(0x123456789abcdef0));
}
static void check_failure(struct fixture *f)
{
    struct gg_rq_snapshot out,saved;memset(&out,0xa5,sizeof(out));saved=out;
    assert(ggrs_copy_snapshot(f->session,&out)!=GGRS_OK && !memcmp(&out,&saved,sizeof(out)));
    assert(f->session->invalid && f->session->rm.quarantined && !f->session->snapshot.peer_vmid);
    unsigned sent=f->calls,received=f->receives;
    assert(ggrs_probe(f->session,100000)==GGRS_INVALID);
    assert(f->calls==sent && f->receives==received);
}
static void normal_and_lifecycle(void)
{
    struct fixture f;start(&f);
    assert(ggrs_probe(f.session,0)==GGRS_ARGUMENT && !f.session->attempted);
    /* The mutable source descriptor is no longer consulted. */
    memset(&f.io,0,sizeof(f.io));f.notify_step=4;f.reenter=1;f.full=2;
    transcript(&f);assert(f.notifications==1);
    assert(ggrs_probe(f.session,100000)==GGRS_INVALID && f.calls==7);
    assert(ggrs_copy_snapshot(f.session,&f.session->snapshot)==GGRS_ARGUMENT);
    _Alignas(struct gg_rq_snapshot) uint8_t unaligned[sizeof(struct gg_rq_snapshot)+1];
    memset(unaligned,0x5c,sizeof(unaligned));
    assert(ggrs_copy_snapshot(f.session,(struct gg_rq_snapshot *)(unaligned+1))==GGRS_ARGUMENT);
    for(unsigned i=0;i<sizeof(unaligned);i++)assert(unaligned[i]==0x5c);
    uint8_t event[]={0x74,0,0,0,2,0,0,0};
    assert(ggrs_notification(f.session,0x56100008,event,8)==GGRS_OK);
    event[0]=0x73;
    assert(ggrs_notification(f.session,0x56100008,event,8)==GGRS_CODEC);
    assert(f.notifications==3);check_failure(&f);free(f.session);
    start(&f);transcript(&f);f.live=0;check_failure(&f);free(f.session);
}
static void faults(void)
{
    for(unsigned step=1;step<=7;step++)for(unsigned fault=1;fault<=6;fault++) {
        struct fixture f;start(&f);f.fault_step=step;f.fault=fault;
        assert(ggrs_probe(f.session,100000)!=GGRS_OK);check_failure(&f);free(f.session);
    }
    for(unsigned mode=0;mode<7;mode++) {
        struct fixture f;start(&f);
        if(mode==0)f.empty=1;
        if(mode==1){f.full=100;f.delay_fail=1;}
        if(mode==2)f.live=0;
        if(mode==3){f.notify_step=4;f.fault=7;}
        if(mode==4){f.notify_step=1;}
        if(mode==5){f.notify_step=4;f.owner_reject=1;}
        if(mode==6)f.full=100;
        assert(ggrs_probe(f.session,1000)!=GGRS_OK);check_failure(&f);free(f.session);
    }
    struct fixture f;start(&f);assert(ggrs_probe(f.session,10)!=GGRS_OK);
    assert(f.calls<7);check_failure(&f);free(f.session);
    start(&f);transcript(&f);assert(ggrs_invalidate(f.session)==GGRS_INVALID);
    check_failure(&f);free(f.session);
}
static struct fixture *native_fixture;
static void idle_status(struct fixture *f,unsigned index,uint16_t vmid)
{
    assert(index<4);f->idle_mode=1;
    if(f->idle_frames<=index)f->idle_frames=index+1;
    uint8_t *p=f->idle_wire[index];memset(p,0,GGRM_FRAME);
    ggrm_header(p,3,0,0x5566+index,GGRQ_VM_STATUS);
    p[8]=(uint8_t)vmid;p[9]=(uint8_t)(vmid>>8);p[12]=GGRQ_RESET;
    f->idle_bytes[index]=16;
}
void ggh_hvc_test_doorbell_invoke(struct ggh_hvc_regs *r)
{(void)r;assert(0);}
void ggh_hvc_test_invoke(struct ggh_hvc_regs *r)
{
    assert(native_fixture);uint64_t id=r->x[0];
    if(id==UINT64_C(0xc600801b)) {
        assert(r->x[1]==UINT64_C(0x100000002));
        int result=send_frame(native_fixture,(void *)(uintptr_t)r->x[3],(size_t)r->x[2],(unsigned)r->x[4]);
        r->x[0]=result==1?61:result?1:0;r->x[1]=0;
    } else {
        assert(id==UINT64_C(0xc600801c) && r->x[1]==UINT64_C(0x200000003));
        size_t bytes=0;int result=receive_frame(native_fixture,(void *)(uintptr_t)r->x[2],(size_t)r->x[3],&bytes);
        r->x[0]=result==1?60:result?1:0;r->x[1]=bytes;r->x[2]=0;
    }
}
static int native_live(void *cookie,uint64_t epoch,uint64_t tx,uint64_t rx)
{
    return valid(cookie,epoch) && tx==UINT64_C(0x100000002) && rx==UINT64_C(0x200000003);
}
static void native_bridge(void)
{
    struct fixture f;setup(&f);native_fixture=&f;
    struct ggh_hvc hvc={0};hvc.epoch=37;hvc.tx_cap=UINT64_C(0x100000002);
    hvc.rx_cap=UINT64_C(0x200000003);hvc.lease_cookie=&f;hvc.live=native_live;
    struct ggrs_events events={&f,now_us,delay_us,notification};
    assert(ggrs_init_hvc(f.session,&hvc,&events,38)==GGRS_ARGUMENT);
    f.init_reenter=1;assert(ggrs_init_hvc(f.session,&hvc,&events,37)==GGRS_OK);
    memset(&hvc,0,sizeof(hvc));memset(&events,0,sizeof(events));
    f.notify_step=4;f.full=1;transcript(&f);assert(f.notifications==1);
    assert(ggrs_copy_snapshot(f.session,(struct gg_rq_snapshot *)&f.session->native_hvc)==GGRS_ARGUMENT);
    f.idle_mode=1;
    assert(ggrs_poll(f.session,4,100000)==GGRS_IDLE && f.calls==7);
    idle_status(&f,0,0x73);
    assert(ggrs_poll(f.session,4,100000)==GGRS_CODEC);
    assert(f.calls==7 && f.notifications==2 && f.session->poll_notifications==1);
    check_failure(&f);free(f.session);native_fixture=NULL;
}
static void reentry_loss_and_time(void)
{
    struct fixture f;setup(&f);f.init_reenter=1;
    assert(ggrs_init(f.session,&f.io,37)==GGRS_OK);transcript(&f);free(f.session);
    setup(&f);f.loss_valid=1;
    assert(ggrs_init(f.session,&f.io,37)==GGRS_LEASE);
    assert(!f.calls);check_failure(&f);free(f.session);

    start(&f);f.loss_valid=1;
    assert(ggrs_probe(f.session,100000)==GGRS_LEASE);
    assert(!f.calls);check_failure(&f);free(f.session);
    start(&f);f.notify_step=4;f.loss_notification=1;
    assert(ggrs_probe(f.session,100000)!=GGRS_OK);
    assert(f.calls==4 && f.notifications==1);check_failure(&f);free(f.session);
    start(&f);transcript(&f);f.loss_valid=1;
    struct gg_rq_snapshot out,saved;memset(&out,0x6b,sizeof(out));saved=out;
    assert(ggrs_copy_snapshot(f.session,&out)==GGRS_LEASE && !memcmp(&out,&saved,sizeof(out)));
    check_failure(&f);free(f.session);

    start(&f);f.rewind=1;
    assert(ggrs_probe(f.session,100000)==GGRS_RPC && f.calls==1);
    check_failure(&f);free(f.session);
    start(&f);f.late_final=1;
    assert(ggrs_probe(f.session,100000)==GGRS_RPC && f.calls==7);
    check_failure(&f);free(f.session);
    start(&f);f.late_send=1;
    assert(ggrs_probe(f.session,100000)==GGRS_RPC && !f.calls);
    check_failure(&f);free(f.session);
}
static void idle_pump(void)
{
    struct fixture f;start(&f);
    assert(ggrs_poll(f.session,4,1000)==GGRS_INVALID && !f.receives);
    transcript(&f);f.idle_mode=1;
    unsigned received=f.receives;
    assert(ggrs_poll(f.session,0,1000)==GGRS_ARGUMENT);
    assert(ggrs_poll(f.session,GGRS_MAX_POLL_FRAMES+1,1000)==GGRS_ARGUMENT);
    assert(ggrs_poll(f.session,4,0)==GGRS_ARGUMENT && f.receives==received);
    assert(ggrs_poll(f.session,4,1000)==GGRS_IDLE && f.session->poll_frames==0);
    idle_status(&f,0,0x74);idle_status(&f,1,0x75);f.reenter=1;
    assert(ggrs_poll(f.session,1,1000)==GGRS_BUDGET);
    assert(f.idle_index==1 && f.notifications==1 && f.session->poll_frames==1);
    assert(ggrs_poll(f.session,4,1000)==GGRS_IDLE);
    assert(f.idle_index==2 && f.notifications==2 && f.session->poll_notifications==1);
    assert(f.calls==7 && f.session->rm.next_sequence==7);
    struct gg_rq_snapshot out;
    assert(ggrs_copy_snapshot(f.session,&out)==GGRS_OK);free(f.session);

    /* A legitimate two-frame notification is assembled exactly once. */
    start(&f);transcript(&f);idle_status(&f,0,0x74);
    uint8_t payload[8];memcpy(payload,f.idle_wire[0]+8,8);
    ggrm_header(f.idle_wire[0],3,1,0x5566,GGRQ_VM_STATUS);
    f.idle_bytes[0]=11;memcpy(f.idle_wire[0]+8,payload,3);
    ggrm_header(f.idle_wire[1],0,1,0x5566,GGRQ_VM_STATUS);
    f.idle_bytes[1]=13;memcpy(f.idle_wire[1]+8,payload+3,5);f.idle_frames=2;
    assert(ggrs_poll(f.session,4,1000)==GGRS_IDLE && f.notifications==1);
    assert(f.session->poll_frames==2 && f.session->poll_notifications==1);free(f.session);

    for(unsigned mode=0;mode<15;mode++) {
        start(&f);transcript(&f);idle_status(&f,0,0x74);
        unsigned budget=4;
        if(mode==0)idle_status(&f,0,0x73); /* Queued peer RESET. */
        if(mode==1)idle_status(&f,0,0x45); /* Queued self RESET. */
        if(mode==2)put32(f.idle_wire[0]+4,0x51100013); /* Unknown notification. */
        if(mode==3)f.idle_wire[0][1]=2; /* Unexpected late reply. */
        if(mode==4)f.idle_wire[0][0]^=1;
        if(mode==5){f.idle_wire[0][1]=(1u<<2)|3;budget=1;} /* Partial at budget. */
        if(mode==6)f.idle_wire[0][1]=(1u<<2)|3; /* Partial at EMPTY. */
        if(mode>=7 && mode<=9) {
            ggrm_header(f.idle_wire[0],3,1,0x5566,GGRQ_VM_STATUS);f.idle_bytes[0]=8;
            ggrm_header(f.idle_wire[1],0,1,0x5566,GGRQ_VM_STATUS);
            memcpy(f.idle_wire[1]+8,f.idle_wire[0]+8,8);f.idle_bytes[1]=16;f.idle_frames=2;
            if(mode==7)f.idle_wire[1][2]^=1;
            if(mode==8)f.idle_wire[1][4]^=1;
            if(mode==9)f.idle_wire[1][1]=0; /* Changed continuation count. */
        }
        if(mode>=10 && mode<=13)f.idle_fault=mode-9;
        if(mode==14)f.loss_notification=1;
        assert(ggrs_poll(f.session,budget,1000)!=GGRS_IDLE);
        check_failure(&f);
        received=f.receives;
        assert(ggrs_poll(f.session,4,1000)==GGRS_INVALID && f.receives==received);
        assert(f.calls==7);free(f.session);
    }
}
int main(void)
{
    normal_and_lifecycle();faults();native_bridge();reentry_loss_and_time();idle_pump();
    puts("GPU Gunyah session: actual HVC/RM/query bridge, frozen callbacks, dynamic identities, 49 RPC faults, monotonic deadline, initialization reentry, loss latching and bounded idle notification pump passed (host fixtures only)");
    return 0;
}
