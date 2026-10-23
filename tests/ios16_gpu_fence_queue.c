/* Synthetic RAM/owner fixture only: no Gunyah, GPU, physical mapping or IPC. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../ios16/gpu/giulia_gpu_fence_queue.h"

struct fixture {
    struct ggf_registration registration;
    struct ggfq_queue queue;
    struct ggf_layout layout;
    struct ggf_proof proof;
    struct ggfq_owner owner;
    struct ggfq_fence fence;
    uint32_t *memory;
    unsigned observations,barriers,acquires,releases,validates,signals;
    unsigned lose_observation,lose_barrier,change_writer_barrier,corrupt_header_barrier;
    unsigned advance_reader_barrier,reenter;
    unsigned bad_validation,change_handle,signal_fails,signal_loses,lease_fails;
    unsigned leased,token,expected_write;
};
static int observe(void *cookie,struct ggf_proof *out)
{
    struct fixture *f=cookie;
    if(++f->observations==f->lose_observation)f->proof.epoch++;
    *out=f->proof;return 1;
}
static void barrier(void *cookie)
{
    struct fixture *f=cookie;assert(f->leased);
    if(++f->barriers==f->lose_barrier)f->proof.epoch++;
    if(f->barriers==f->change_writer_barrier)
        f->memory[f->layout.tx_header/4+13]=GGFQ_DWORDS;
    if(f->barriers==f->corrupt_header_barrier)f->memory[0]=1;
    if(f->barriers==f->advance_reader_barrier)
        f->memory[f->layout.tx_header/4+12]=GGFQ_DWORDS;
}
static int acquire(void *cookie,const struct ggf_provider *p)
{
    struct fixture *f=cookie;assert(p==ggfp_get(&f->registration));
    ++f->acquires;assert(!f->leased);
    if(f->lease_fails)return 0;
    if(f->reenter)assert(ggfq_tx_push(&f->queue,&f->token,0,0,0)==GGFQ_BUSY);
    f->leased=1;return 1;
}
static void release(void *cookie,const struct ggf_provider *p)
{
    struct fixture *f=cookie;assert(p==ggfp_get(&f->registration));
    assert(f->leased);f->leased=0;++f->releases;
}
static int validate(void *cookie,const struct ggf_provider *p,const void *handle,
    struct ggfq_fence *out)
{
    struct fixture *f=cookie;assert(p==ggfp_get(&f->registration));
    assert(f->leased);++f->validates;
    if(handle!=&f->token || f->validates==f->bad_validation)return 0;
    *out=f->fence;
    if(f->validates==f->change_handle)out->seqno++;
    return 1;
}
static int signal_peer(void *cookie,const struct ggf_provider *p)
{
    struct fixture *f=cookie;assert(p==ggfp_get(&f->registration));
    assert(f->leased);assert(f->validates==2);assert(f->barriers==6);
    assert(f->memory[f->layout.tx_header/4+13]==f->expected_write);
    ++f->signals;
    if(f->signal_loses)f->proof.epoch++;
    return !f->signal_fails;
}
static void init(struct fixture *f,uint32_t page_bytes)
{
    memset(f,0,sizeof(*f));assert(ggf_layout(page_bytes,&f->layout));
    f->memory=calloc(1,f->layout.bytes);assert(f->memory);
    uint32_t *w=f->memory,*tx=w+f->layout.tx_header/4,*rx=w+f->layout.rx_header/4;
    w[1]=f->layout.bytes;w[2]=24;w[3]=56;w[4]=2;w[5]=2;
    tx[1]=(uint32_t)(f->layout.physical+f->layout.tx_payload);tx[2]=1;
    tx[3]=f->layout.queue_bytes;tx[4]=64;
    rx[1]=(uint32_t)(f->layout.physical+f->layout.rx_payload);rx[2]=2;
    rx[3]=f->layout.queue_bytes;rx[4]=64;
    f->proof=(struct ggf_proof){7,GGF_CARVEOUT_PHYSICAL,f->layout.physical,
        GGF_CARVEOUT_BYTES,f->layout.bytes,page_bytes,3,5,6,1,GGFP_REQUIRED};
    struct ggf_provider p={GGFP_ABI,sizeof(p),&f->token,f,7,f->layout,f->memory,observe,barrier};
    assert(ggfp_register(&f->registration,&p));
    f->owner=(struct ggfq_owner){&f->registration,&f->token,7,f->memory,f,
        acquire,release,validate,signal_peer};
    f->fence=(struct ggfq_fence){UINT64_C(0x123456789abcdef0),UINT64_C(0xfedcba9876543210),0};
    f->expected_write=GGFQ_DWORDS;
    assert(ggfq_bind(&f->queue,&f->owner)==GGFQ_OK);
    assert(f->observations==1 && !f->barriers && !f->acquires);
    f->observations=0;
}
static void finish(struct fixture *f)
{ assert(!f->leased);assert(f->releases<=f->acquires);free(f->memory); }
static int push(struct fixture *f)
{ return ggfq_tx_push(&f->queue,&f->token,UINT64_C(0x8877665544332211),19,UINT64_C(0xabcdef1234567890)); }
static void codec(void)
{
    struct ggfq_payload p={{UINT64_C(0x0123456789abcdef),UINT64_C(0xfedcba9876543210),8191},
        UINT64_C(0x8877665544332211),UINT64_C(0x0f1e2d3c4b5a6978),UINT64_C(0xaabbccddeeff0011),0xfedcba98};
    uint32_t wire[16],expected[16]={64,0x01000001,0x89abcdef,0x01234567,0x76543210,0xfedcba98,
        8191,0,0x44332211,0x88776655,0x4b5a6978,0x0f1e2d3c,0xfedcba98,0xeeff0011,0xaabbccdd,0};
    struct ggfq_payload out={0};assert(ggfq_encode(wire,&p));
    assert(!memcmp(wire,expected,sizeof(wire)));assert(ggfq_decode(wire,&out));
    uint32_t roundtrip[16];assert(ggfq_encode(roundtrip,&out));assert(!memcmp(wire,roundtrip,sizeof(wire)));
    wire[15]=0xffffffff;assert(ggfq_decode(wire,&out));
    for(unsigned bad=0;bad<5;bad++) {
        memcpy(wire,expected,sizeof(wire));
        if(bad==0)wire[0]=60;
        if(bad==1)wire[1]=0x01000002;
        if(bad==2)wire[1]=0x01010001;
        if(bad==3)wire[6]=8192;
        if(bad==4)wire[7]=1;
        unsigned char before[sizeof(out)];memcpy(before,&out,sizeof(out));
        assert(!ggfq_decode(wire,&out));assert(!memcmp(before,&out,sizeof(out)));
    }
    p.fence.hash=8192;memcpy(wire,expected,sizeof(wire));
    assert(!ggfq_encode(wire,&p));assert(!memcmp(wire,expected,sizeof(wire)));
}
static void tx_and_wrap(void)
{
    const uint32_t pages[3]={4096,16384,65536};
    for(unsigned i=0;i<3;i++) {
        struct fixture f;init(&f,pages[i]);
        assert(push(&f)==GGFQ_OK);assert(f.signals==1 && f.queue.state==GGFQ_READY);
        struct ggfq_payload out;assert(ggfq_decode(f.memory+f.layout.tx_payload/4,&out));
        assert(out.fence.context==f.fence.context && out.fence.seqno==f.fence.seqno && out.fence.hash==0);
        assert(out.client_data==0 && out.error==19 && out.flags==UINT64_C(0x8877665544332211));
        assert(!f.memory[f.layout.rx_header/4+12] && !f.memory[f.layout.rx_header/4+13]);
        finish(&f);
    }
    struct fixture f;init(&f,4096);
    f.memory[f.layout.tx_header/4+12]=GGFQ_DWORDS;
    f.memory[f.layout.tx_header/4+13]=(GGF_QUEUE_ENTRIES-1)*GGFQ_DWORDS;
    f.expected_write=0;assert(push(&f)==GGFQ_OK);
    assert(f.memory[f.layout.tx_header/4+13]==0);finish(&f);
    init(&f,4096);f.memory[f.layout.tx_header/4+13]=2*GGFQ_DWORDS;
    f.advance_reader_barrier=2;f.expected_write=3*GGFQ_DWORDS;f.reenter=1;
    assert(push(&f)==GGFQ_OK && f.acquires==1);finish(&f);
}
static void bind_refusal(void)
{
    struct fixture f;init(&f,4096);
    for(unsigned bad=0;bad<9;bad++) {
        struct ggfq_queue q={0};struct ggfq_owner o=f.owner;
        struct ggf_registration unregistered={0};
        if(bad==0)o.registration=NULL;
        if(bad==1)o.identity=&q;
        if(bad==2)o.epoch++;
        if(bad==3)o.memory=(volatile void *)&q;
        if(bad==4)o.acquire=NULL;
        if(bad==5)o.release=NULL;
        if(bad==6)o.validate_handle=NULL;
        if(bad==7)o.signal=NULL;
        if(bad==8)o.registration=&unregistered;
        assert(ggfq_bind(&q,&o)==GGFQ_INVALID && q.state==GGFQ_UNBOUND);
        assert(!f.observations && !f.barriers);
    }
    assert(ggfq_bind((struct ggfq_queue *)f.memory,&f.owner)==GGFQ_INVALID);
    assert(ggfq_bind(&f.queue,&f.owner)==GGFQ_BUSY);
    struct ggfq_queue q={0};f.proof.epoch++;
    assert(ggfq_bind(&q,&f.owner)==GGFQ_LOST && q.state==GGFQ_UNBOUND);
    assert(!f.barriers && !f.acquires);finish(&f);
}
static void refusals(void)
{
    struct fixture f;init(&f,4096);
    unsigned char *before=malloc(f.layout.bytes);assert(before);
    f.memory[f.layout.tx_header/4+12]=GGFQ_DWORDS;memcpy(before,f.memory,f.layout.bytes);
    assert(push(&f)==GGFQ_FULL && !f.signals);assert(!memcmp(before,f.memory,f.layout.bytes));
    free(before);finish(&f);
    for(unsigned bad=0;bad<8;bad++) {
        init(&f,4096);
        if(bad==0)f.memory[0]=1;
        if(bad==1)f.memory[f.layout.tx_header/4+1]++;
        if(bad==2)f.memory[f.layout.rx_header/4+4]=32;
        if(bad==3)f.memory[f.layout.tx_header/4+12]=1;
        if(bad==4)f.memory[f.layout.tx_header/4+13]=GGF_QUEUE_ENTRIES*GGFQ_DWORDS;
        if(bad==5)f.memory[f.layout.rx_header/4+12]=0xffffffff;
        if(bad==6)f.memory[1]--;
        if(bad==7)f.memory[f.layout.tx_header/4+3]-=64;
        before=malloc(f.layout.bytes);assert(before);memcpy(before,f.memory,f.layout.bytes);
        assert(push(&f)==GGFQ_LOST && f.queue.state==GGFQ_BROKEN && !f.signals);
        assert(!memcmp(before,f.memory,f.layout.bytes));assert(push(&f)==GGFQ_QUARANTINED);
        free(before);finish(&f);
    }
    for(unsigned fail=1;fail<=2;fail++) {
        init(&f,4096);f.bad_validation=fail;
        assert(push(&f)==GGFQ_HANDLE && !f.signals && !f.memory[f.layout.tx_header/4+13]);finish(&f);
    }
    init(&f,4096);f.change_handle=2;
    assert(push(&f)==GGFQ_HANDLE && !f.memory[f.layout.tx_payload/4]);finish(&f);
    init(&f,4096);f.fence.hash=8192;assert(push(&f)==GGFQ_HANDLE);finish(&f);
    init(&f,4096);f.lease_fails=1;
    assert(push(&f)==GGFQ_BUSY && !f.barriers && !f.validates && !f.releases);finish(&f);
    init(&f,4096);
    assert(ggfq_tx_push(&f.queue,(void *)(uintptr_t)7,0,0,0)==GGFQ_HANDLE);
    assert(!f.barriers && !f.signals);finish(&f);
}
static void loss_and_commit(void)
{
    struct fixture f;init(&f,4096);assert(push(&f)==GGFQ_OK);
    unsigned observations=f.observations;assert(observations>5 && observations<20);finish(&f);
    for(unsigned i=1;i<=observations;i++) {
        init(&f,4096);f.lose_observation=i;int result=push(&f);
        assert(result==GGFQ_LOST || result==GGFQ_QUARANTINED);
        assert(f.queue.state==GGFQ_BROKEN);
        uint32_t index=f.memory[f.layout.tx_header/4+13];assert(index==0 || index==GGFQ_DWORDS);
        if(index==GGFQ_DWORDS)assert(result==GGFQ_QUARANTINED);
        unsigned calls=f.observations;assert(push(&f)==GGFQ_QUARANTINED && f.observations==calls);
        assert(ggfq_bind(&f.queue,&f.owner)==GGFQ_BUSY);finish(&f);
    }
    for(unsigned i=1;i<=6;i++) {
        init(&f,4096);f.lose_barrier=i;int result=push(&f);
        assert(result==GGFQ_LOST || result==GGFQ_QUARANTINED);
        assert(!f.signals && f.queue.state==GGFQ_BROKEN);
        assert(f.memory[f.layout.tx_header/4+13]==(i==6?GGFQ_DWORDS:0));finish(&f);
    }
    for(unsigned bad=0;bad<2;bad++) {
        init(&f,4096);f.signal_fails=!bad;f.signal_loses=bad;
        assert(push(&f)==GGFQ_QUARANTINED && f.signals==1);
        assert(f.memory[f.layout.tx_header/4+13]==GGFQ_DWORDS && f.queue.state==GGFQ_BROKEN);
        finish(&f);
    }
    init(&f,4096);f.change_writer_barrier=3;
    assert(push(&f)==GGFQ_LOST && !f.signals && f.queue.state==GGFQ_BROKEN);finish(&f);
    init(&f,4096);f.corrupt_header_barrier=3;
    assert(push(&f)==GGFQ_LOST && !f.signals && !f.memory[f.layout.tx_header/4+13]);finish(&f);
}
static void rx_observation(void)
{
    struct fixture f;init(&f,4096);
    struct ggfq_payload out;memset(&out,0xa5,sizeof(out));
    unsigned char before[sizeof(out)];memcpy(before,&out,sizeof(out));
    assert(ggfq_rx_peek(&f.queue,&out)==GGFQ_EMPTY);assert(!memcmp(before,&out,sizeof(out)));
    struct ggfq_payload p={{11,22,33},44,55,66,77};
    assert(ggfq_encode(f.memory+f.layout.rx_payload/4,&p));
    f.memory[f.layout.rx_header/4+13]=GGFQ_DWORDS;
    unsigned char *memory=malloc(f.layout.bytes);assert(memory);memcpy(memory,f.memory,f.layout.bytes);
    assert(ggfq_rx_peek(&f.queue,&out)==GGFQ_OK);
    assert(out.fence.context==11 && out.fence.seqno==22 && out.fence.hash==33);
    assert(out.flags==44 && out.client_data==55 && out.timestamp==66 && out.error==77);
    assert(!memcmp(memory,f.memory,f.layout.bytes));assert(!f.signals && !f.validates);
    assert(ggfq_rx_peek(&f.queue,(struct ggfq_payload *)f.memory)==GGFQ_INVALID);
    assert(ggfq_rx_peek(&f.queue,(struct ggfq_payload *)&f.queue)==GGFQ_INVALID);
    f.memory[f.layout.rx_payload/4+1]=0x01010001;
    memcpy(before,&out,sizeof(out));assert(ggfq_rx_peek(&f.queue,&out)==GGFQ_LOST);
    assert(!memcmp(before,&out,sizeof(out)) && !f.memory[f.layout.rx_header/4+12]);
    free(memory);finish(&f);
}
int main(void)
{
    codec();bind_refusal();tx_and_wrap();refusals();loss_and_commit();rx_observation();
    puts("Fence queue: exact 64-byte ABI, real-handle/owner refusal, exclusive TX fallback, full/wrap, epoch quarantine and read-only RX passed (synthetic RAM only)");
    return 0;
}
