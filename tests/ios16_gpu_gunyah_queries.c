/* Offline RM fixtures only: no device, hypercall, ownership or READY producer. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_gunyah_queries.h"

static const uint8_t self_reply[]={0x45,0,0,0};
static const uint8_t peer_reply[]={1,0,0,0,0x73,0,0,0};
static const uint8_t identity_reply[]={1,0,0,0,2,0,13,0,
    'q','c','o','m',',','c','p','u','s','y','s','v','m',0,0,0};
static const uint8_t ready_reply[]={2,4,0x34,0x12};
static const uint8_t parcel_reply[]={0x78,0x56,0x34,0x12};
static uint8_t resources[48];
static void resources_init(void)
{
    memset(resources,0,sizeof(resources));
    gg_rq_put32(resources,1);resources[4]=1;gg_rq_put16(resources+6,0x73);
    gg_rq_put32(resources+8,0x5678);gg_rq_put32(resources+12,6);
    gg_rq_put64(resources+16,UINT64_C(0x123456789abcdef0));
    gg_rq_put32(resources+24,0x9876);gg_rq_put32(resources+28,456);
}
static void builders(void)
{
    uint8_t p[80],saved[80];memset(p,0x5a,sizeof(p));memcpy(saved,p,sizeof(p));
    assert(!gg_rq_build_lookup_name(p,19));assert(!memcmp(p,saved,sizeof(p)));
    assert(gg_rq_build_lookup_name(p,sizeof(p))==20);
    const uint8_t lookup[]={13,0,0,0,'q','c','o','m',',','c','p','u','s','y','s','v','m',0,0,0};
    assert(!memcmp(p,lookup,sizeof(lookup)));assert(p[20]==0x5a);
    assert(!gg_rq_build_vmid(p,3,0x73));assert(!gg_rq_build_vmid(p,sizeof(p),UINT16_MAX));
    assert(gg_rq_build_vmid(p,sizeof(p),0)==4);assert(gg_rq_u32(p)==0);
    assert(!gg_rq_build_mem_lookup(p,43,0x45,0x73));
    assert(!gg_rq_build_mem_lookup(p,sizeof(p),0,0x73));
    assert(!gg_rq_build_mem_lookup(p,sizeof(p),0x45,UINT16_MAX));
    assert(!gg_rq_build_mem_lookup(p,sizeof(p),0x45,0x45));
    assert(gg_rq_build_mem_lookup(p,sizeof(p),0x45,0x73)==44);
    const uint8_t memory[]={0,0,0,0,5,0,0,0,2,0,0,0,0x45,0,6,0,0x73,0,6,0,
        1,0,0,0,0,0x30,0xe2,0xd4,0,0,0,0,0,0xd0,0x2d,0,0,0,0,0,0,0,0,0};
    assert(sizeof(memory)==44 && !memcmp(p,memory,44));
}
static void parsing(void)
{
    uint16_t vmid=0xabcd;uint8_t b[160];
    assert(!gg_rq_parse_self(self_reply,4,&vmid) && vmid==0x45);
    assert(gg_rq_parse_self(self_reply,3,&vmid) && vmid==0x45);
    assert(!gg_rq_parse_lookup(peer_reply,8,&vmid) && vmid==0x73);
    for(size_t n=0;n<8;n++)assert(gg_rq_parse_lookup(peer_reply,n,&vmid));
    for(unsigned i=0;i<4;i++) {
        memcpy(b,peer_reply,8);
        if(i==0)b[0]=2;if(i==1)b[4]=0;if(i==2){b[4]=0xff;b[5]=0xff;}if(i==3)b[6]=1;
        assert(gg_rq_parse_lookup(b,8,&vmid));
    }
    /* VMID 3 may be a real dynamic result; never substitute peer-name enum 3
     * for a missing lookup. Zero/unknown cache state cannot satisfy the chain. */
    memcpy(b,peer_reply,8);b[4]=3;
    assert(!gg_rq_parse_lookup(b,8,&vmid) && vmid==3);
    assert(!gg_rq_parse_identity(identity_reply,sizeof(identity_reply)));
    for(size_t n=0;n<sizeof(identity_reply);n++)assert(gg_rq_parse_identity(identity_reply,n));
    memcpy(b,identity_reply,24);b[6]=14;
    assert(!gg_rq_parse_identity(b,24));b[21]='x';assert(gg_rq_parse_identity(b,24));
    memcpy(b,identity_reply,24);b[8]='Q';assert(gg_rq_parse_identity(b,24));
    memcpy(b,identity_reply,24);b[6]=12;assert(gg_rq_parse_identity(b,24));
    memcpy(b,identity_reply,24);b[6]=255;b[7]=255;assert(gg_rq_parse_identity(b,24));
    memcpy(b,identity_reply,24);b[0]=255;assert(gg_rq_parse_identity(b,24));
    memcpy(b,identity_reply,24);b[5]=1;assert(gg_rq_parse_identity(b,24));
    memcpy(b,identity_reply,24);b[4]=4;assert(gg_rq_parse_identity(b,24));
    memcpy(b,identity_reply,24);memcpy(b+24,identity_reply+4,20);b[0]=2;
    assert(gg_rq_parse_identity(b,44));
    /* A second different identity descriptor has its own bounded padded size. */
    memcpy(b,identity_reply,24);b[0]=2;b[24]=0;b[25]=0;b[26]=16;b[27]=0;memset(b+28,0x42,16);
    assert(!gg_rq_parse_identity(b,44));b[26]=15;assert(gg_rq_parse_identity(b,44));

    struct gg_rq_vm_state state;
    assert(!gg_rq_parse_state(ready_reply,4,&state) && state.vm_status==2 && state.app_status==0x1234);
    for(unsigned status=0;status<256;status++) {
        uint8_t input[]={ (uint8_t)status,0,0,0 };
        assert((gg_rq_parse_state(input,4,&state)==0)==(status<12 && status!=7));
    }
    memcpy(b,ready_reply,4);b[1]=5;assert(gg_rq_parse_state(b,4,&state));
    uint32_t handle=0;
    assert(!gg_rq_parse_mem_lookup(parcel_reply,4,&handle) && handle==0x12345678);
    assert(gg_rq_parse_mem_lookup(parcel_reply,3,&handle) && handle==0x12345678);
    struct gg_rq_doorbell db;
    assert(!gg_rq_parse_resources(resources,48,0x73,&db));
    assert(db.capid==UINT64_C(0x123456789abcdef0) && db.resource_handle==0x5678 &&
        db.virq_handle==0x9876 && db.virq==456 && db.partner_vmid==0x73);
    for(size_t n=0;n<48;n++)assert(gg_rq_parse_resources(resources,n,0x73,&db));
    for(unsigned i=0;i<7;i++) {
        memcpy(b,resources,48);
        if(i==0)b[0]=2;if(i==1)b[4]=0;if(i==2)b[6]++;if(i==3)b[12]=5;
        if(i==4)memset(b+16,0xff,8);if(i==5)b[5]=1;
        if(i==6){b[0]=2;memcpy(b+48,b+4,44);}
        assert(gg_rq_parse_resources(b,i==6?92:48,0x73,&db));
    }
}
static void step(struct gg_rq_snapshot *s,uint32_t id,const uint8_t *p,size_t n)
{
    uint8_t request[64];uint32_t built=0;size_t bytes=99;
    assert(gg_rq_expected_query(s)==id);
    assert(!gg_rq_build_query(s,&built,request,sizeof(request),&bytes) && built==id);
    if(id==GGRQ_VM_GET_VMID)assert(bytes==0);
    else if(id==GGRQ_VM_LOOKUP_NAME)assert(bytes==20);
    else if(id==GGRQ_MEM_LOOKUP)assert(bytes==44 && gg_rq_u16(request+12)==0x45 && gg_rq_u16(request+16)==0x73);
    else assert(bytes==4 && gg_rq_u16(request)==(id==GGRQ_VM_GET_RESOURCES?0:0x73));
    assert(!gg_rq_accept_reply(s,s->epoch,id,p,n));
}
static void complete(struct gg_rq_snapshot *s,uint64_t epoch)
{
    assert(!gg_rq_begin(s,epoch));
    step(s,GGRQ_VM_GET_VMID,self_reply,4);step(s,GGRQ_VM_LOOKUP_NAME,peer_reply,8);
    step(s,GGRQ_VM_GET_ID,identity_reply,24);step(s,GGRQ_VM_GET_STATE,ready_reply,4);
    step(s,GGRQ_MEM_LOOKUP,parcel_reply,4);step(s,GGRQ_VM_GET_RESOURCES,resources,48);
    assert(!gg_rq_snapshot_complete(s));step(s,GGRQ_VM_GET_STATE,ready_reply,4);
    assert(gg_rq_snapshot_complete(s));
}
static void lifecycle(void)
{
    struct gg_rq_snapshot s={0,0,0,0,0,0,{0,0,0,0,0}};
    assert(!gg_rq_snapshot_complete(&s));assert(gg_rq_begin(&s,0));
    complete(&s,1);assert(s.peer_vmid==0x73 && s.self_vmid==0x45);
    assert(gg_rq_begin(&s,1));assert(gg_rq_snapshot_complete(&s));
    uint8_t event[]={0x74,0,0,0,2,0,0,0};
    assert(!gg_rq_notification(&s,GGRQ_VM_STATUS,event,8) && gg_rq_snapshot_complete(&s));
    for(unsigned status=0;status<12;status++) {
        if(status==7)continue;
        event[0]=0x73;event[4]=(uint8_t)status;
        assert(gg_rq_notification(&s,GGRQ_VM_STATUS,event,8));
        assert(!gg_rq_snapshot_complete(&s) && !s.peer_vmid && !s.parcel_handle && !s.doorbell.capid);
        complete(&s,s.epoch+1);
    }
    event[0]=0x45;event[4]=2;
    assert(gg_rq_notification(&s,GGRQ_VM_STATUS,event,8));complete(&s,s.epoch+1);
    assert(gg_rq_notification(&s,UINT32_C(0x51100011),event,8));complete(&s,s.epoch+1);
    assert(gg_rq_notification(&s,GGRQ_VM_STATUS,event,7));complete(&s,s.epoch+1);
    assert(gg_rq_accept_reply(&s,s.epoch,GGRQ_VM_GET_STATE,ready_reply,4));
    assert(!gg_rq_snapshot_complete(&s));
    assert(!gg_rq_begin(&s,s.epoch+1));
    assert(gg_rq_accept_reply(&s,s.epoch-1,GGRQ_VM_GET_VMID,self_reply,4));
    assert(!gg_rq_expected_query(&s));assert(!gg_rq_begin(&s,s.epoch+1));
    assert(gg_rq_accept_reply(&s,s.epoch,GGRQ_VM_LOOKUP_NAME,peer_reply,8));
    assert(!gg_rq_begin(&s,s.epoch+1));
    step(&s,GGRQ_VM_GET_VMID,self_reply,4);step(&s,GGRQ_VM_LOOKUP_NAME,peer_reply,8);
    step(&s,GGRQ_VM_GET_ID,identity_reply,24);
    uint8_t running[]={3,4,0,0};
    assert(gg_rq_accept_reply(&s,s.epoch,GGRQ_VM_GET_STATE,running,4));
    assert(!gg_rq_snapshot_complete(&s));
    assert(!gg_rq_begin(&s,s.epoch+1));event[0]=0x73;
    assert(gg_rq_notification(&s,GGRQ_VM_STATUS,event,8));
    assert(!gg_rq_expected_query(&s));
}
static uint32_t random_state=0x13579bdf;
static uint32_t next_random(void)
{random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state;}
static void malformed_fuzz(void)
{
    /* Exercise every offset/length guard with unaligned random wire data. */
    uint8_t storage[514];uint16_t vmid;uint32_t handle;
    struct gg_rq_vm_state vm;struct gg_rq_doorbell db;
    for(unsigned k=0;k<20000;k++) {
        for(unsigned j=0;j<sizeof(storage);j++)storage[j]=(uint8_t)next_random();
        size_t n=next_random()%513;const uint8_t *p=storage+1;
        (void)gg_rq_parse_self(p,n,&vmid);(void)gg_rq_parse_lookup(p,n,&vmid);
        (void)gg_rq_parse_identity(p,n);(void)gg_rq_parse_state(p,n,&vm);
        (void)gg_rq_parse_status_notification(p,n,&vmid,&vm);
        (void)gg_rq_parse_mem_lookup(p,n,&handle);(void)gg_rq_parse_resources(p,n,0x73,&db);
    }
    assert(gg_rq_parse_identity(NULL,24));assert(gg_rq_parse_state(NULL,4,&vm));
    assert(gg_rq_parse_resources(NULL,48,0x73,&db));
}
int main(void)
{
    resources_init();builders();parsing();lifecycle();malformed_fuzz();
    puts("GPU Gunyah query codec: exact requests, dynamic VM identity, invalidation and 20000 malformed fixtures passed");
    return 0;
}
