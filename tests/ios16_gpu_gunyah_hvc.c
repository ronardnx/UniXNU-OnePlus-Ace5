/* Offline register fixtures only. The test build substitutes both HVCs;
 * it never executes a hypercall, maps a device or creates an owner lease. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_gunyah_hvc.h"

struct fixture {
    struct ggh_hvc msgq;
    struct ggh_hvc_doorbell bell;
    unsigned live_calls,bell_live_calls,calls,bell_calls;
    unsigned revoke_at,mutate_at,mutate_field,revoke_native;
    int live_result,bell_live_result;
    uint64_t status,ready,rx_bytes,old_flags;
    uint64_t expected_epoch,expected_tx,expected_rx,expected_bell;
    const void *input;
    size_t input_bytes;
    unsigned flags;
    uint64_t clear_flags;
    uint8_t payload[GGH_HVC_MAX_BYTES];
    struct ggh_hvc_regs input_regs;
};
static struct fixture f;

static int msgq_live(void *cookie,uint64_t epoch,uint64_t tx,uint64_t rx)
{
    assert(cookie==&f);
    assert(epoch==f.expected_epoch && tx==f.expected_tx && rx==f.expected_rx);
    ++f.live_calls;
    if(f.mutate_at==f.live_calls) {
        if(f.mutate_field==0)++f.msgq.epoch;
        if(f.mutate_field==1)++f.msgq.tx_cap;
        if(f.mutate_field==2)++f.msgq.rx_cap;
        if(f.mutate_field==3)f.msgq.lease_cookie=NULL;
        if(f.mutate_field==4)f.msgq.live=NULL;
    }
    if(f.revoke_at && f.live_calls>=f.revoke_at)return 0;
    return f.live_result;
}
static int bell_live(void *cookie,uint64_t epoch,uint64_t cap)
{
    assert(cookie==&f);
    assert(epoch==f.expected_epoch && cap==f.expected_bell);
    ++f.bell_live_calls;
    if(f.mutate_at==f.bell_live_calls) {
        if(f.mutate_field==0)++f.bell.epoch;
        if(f.mutate_field==1)++f.bell.rx_cap;
        if(f.mutate_field==2)f.bell.lease_cookie=NULL;
        if(f.mutate_field==3)f.bell.live=NULL;
    }
    if(f.revoke_at && f.bell_live_calls>=f.revoke_at)return 0;
    return f.bell_live_result;
}
static void init(void)
{
    memset(&f,0,sizeof(f));
    f.expected_epoch=UINT64_C(0x1234567800000001);
    f.expected_tx=UINT64_C(0x89abcdef01234567);
    f.expected_rx=UINT64_C(0x0123456789abcdef);
    f.expected_bell=UINT64_C(0xfedcba9876543210);
    f.msgq.epoch=f.bell.epoch=f.expected_epoch;
    f.msgq.tx_cap=f.expected_tx;f.msgq.rx_cap=f.expected_rx;
    f.bell.rx_cap=f.expected_bell;
    f.msgq.lease_cookie=f.bell.lease_cookie=&f;
    f.msgq.live=msgq_live;f.bell.live=bell_live;
    f.live_result=f.bell_live_result=1;
    f.rx_bytes=11;f.old_flags=UINT64_C(0xfedcba9876543210);
    f.clear_flags=UINT64_C(0x1234567887654321);
    for(unsigned i=0;i<sizeof(f.payload);++i)f.payload[i]=(uint8_t)(i*13+5);
}
void ggh_hvc_test_invoke(struct ggh_hvc_regs *r)
{
    f.input_regs=*r;++f.calls;
    assert(r->x[5]==0 && r->x[6]==0 && r->x[7]==0);
    if(r->x[0]==UINT64_C(0xc600801b)) {
        assert(r->x[1]==f.expected_tx && r->x[2]==f.input_bytes);
        assert(r->x[3]!=(uintptr_t)f.input && r->x[4]==f.flags);
        assert(!memcmp((const void *)(uintptr_t)r->x[3],f.input,f.input_bytes));
    } else {
        assert(r->x[0]==UINT64_C(0xc600801c));
        assert(r->x[1]==f.expected_rx && r->x[3]==f.input_bytes && r->x[4]==0);
        assert(r->x[2]!=(uintptr_t)f.input);
        /* A failed hypercall can scribble its private buffer; no caller
         * output may change until status/size/epoch checks all pass. */
        memcpy((void *)(uintptr_t)r->x[2],f.payload,(size_t)r->x[3]);
    }
    r->x[0]=f.status;
    r->x[1]=f.input_regs.x[0]==UINT64_C(0xc600801b)?f.ready:f.rx_bytes;
    r->x[2]=f.ready;
    for(unsigned i=3;i<8;i++)r->x[i]=UINT64_C(0xf000000000000000)+i;
    if(f.revoke_native)f.revoke_at=f.live_calls+1;
}
void ggh_hvc_test_doorbell_invoke(struct ggh_hvc_regs *r)
{
    ++f.bell_calls;f.input_regs=*r;
    assert(r->x[0]==f.expected_bell && r->x[1]==f.clear_flags);
    for(unsigned i=2;i<8;i++)assert(r->x[i]==0);
    r->x[0]=f.status;r->x[1]=f.old_flags;
    for(unsigned i=2;i<8;i++)r->x[i]=UINT64_C(0x8000000000000000)+i;
    if(f.revoke_native)f.revoke_at=f.bell_live_calls+1;
}
static int send(unsigned flags,size_t bytes)
{
    f.input=f.payload;f.input_bytes=bytes;f.flags=flags;
    return ggh_hvc_send(&f.msgq,f.input,bytes,flags);
}
static int receive(uint8_t *data,size_t capacity,size_t *received)
{
    f.input=data;f.input_bytes=capacity;
    return ggh_hvc_recv(&f.msgq,data,capacity,received);
}
static int doorbell(uint64_t *previous)
{return ggh_hvc_doorbell_read_clean(&f.bell,f.clear_flags,previous);}

static void message_registers(void)
{
    for(unsigned flags=0;flags<=1;flags++)for(size_t n=1;n<=GGH_HVC_MAX_BYTES;n++) {
        init();f.ready=UINT64_MAX;
        assert(send(flags,n)==GGH_HVC_OK && f.calls==1 && f.msgq.attempted);
        assert(f.live_calls==3 && f.msgq.native_code==0 && f.msgq.last.x[1]==UINT64_MAX);
        assert(f.msgq.last.x[7]==UINT64_C(0xf000000000000007));
    }
    /* A successful final queue slot is accepted even with no space left. */
    init();f.ready=0;assert(send(1,240)==GGH_HVC_OK && f.calls==1);
    uint8_t data[GGH_HVC_MAX_BYTES];
    for(size_t n=1;n<=sizeof(data);n++) {
        init();memset(data,0xaa,sizeof(data));f.rx_bytes=n;f.ready=UINT64_MAX;
        size_t received=SIZE_MAX;
        assert(receive(data,n,&received)==GGH_HVC_OK && received==n && f.calls==1);
        assert(!memcmp(data,f.payload,n) && f.msgq.last.x[2]==UINT64_MAX);
        if(n<sizeof(data))assert(data[n]==0xaa);
    }
    /* The invalid sentinel is all ones, not zero; the owner authenticates it. */
    init();f.msgq.tx_cap=f.expected_tx=0;f.msgq.rx_cap=f.expected_rx=0;
    assert(ggh_hvc_valid(&f.msgq,f.expected_epoch)==1);
    assert(send(0,1)==GGH_HVC_OK);
    /* The byte buffers themselves need no alignment; scalars do. */
    uint8_t unaligned[GGH_HVC_MAX_BYTES+1];
    init();memcpy(unaligned+1,f.payload,GGH_HVC_MAX_BYTES);
    f.input=unaligned+1;f.input_bytes=GGH_HVC_MAX_BYTES;
    assert(ggh_hvc_send(&f.msgq,f.input,f.input_bytes,0)==GGH_HVC_OK);
    init();size_t received=0;
    assert(receive(unaligned+1,GGH_HVC_MAX_BYTES,&received)==GGH_HVC_OK);
    assert(received==f.rx_bytes && !memcmp(unaligned+1,f.payload,received));
}
static void status_and_staging(void)
{
    static const uint64_t statuses[]={0,60,61,UINT64_MAX,UINT64_MAX-1,5,
        UINT64_C(0xffffffff00000000),UINT64_C(0x100000000)};
    uint8_t data[32],saved[32];size_t received;
    memset(saved,0xa5,sizeof(saved));
    for(unsigned i=0;i<sizeof(statuses)/sizeof(statuses[0]);i++) {
        init();f.status=statuses[i];
        int expected=f.status==0?GGH_HVC_OK:f.status==61?GGH_HVC_AGAIN:GGH_HVC_ERROR;
        assert(send(1,32)==expected && f.calls==1 && f.msgq.native_code==f.status);
        init();f.status=statuses[i];memcpy(data,saved,sizeof(data));received=999;
        expected=f.status==0?GGH_HVC_OK:f.status==60?GGH_HVC_AGAIN:GGH_HVC_ERROR;
        assert(receive(data,sizeof(data),&received)==expected && f.calls==1);
        assert(f.msgq.native_code==f.status);
        if(f.status)assert(received==999 && !memcmp(data,saved,sizeof(data)));
    }
    static const uint64_t sizes[]={0,33,UINT64_MAX,UINT64_C(0x100000000)};
    for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++) {
        init();f.rx_bytes=sizes[i];memcpy(data,saved,sizeof(data));received=999;
        assert(receive(data,sizeof(data),&received)==GGH_HVC_ERROR && f.calls==1);
        assert(received==999 && !memcmp(data,saved,sizeof(data)));
    }
    /* Diagnostics survive a subsequent rejected operation. No HVC/retry is
     * hidden by the adapter, including on the hypervisor RETRY status. */
    init();f.status=UINT64_MAX-1;assert(send(0,8)==GGH_HVC_ERROR && f.calls==1);
    struct ggh_hvc_regs last=f.msgq.last;
    assert(send(2,8)==GGH_HVC_ERROR && !f.msgq.attempted && f.calls==1);
    assert(f.msgq.native_code==UINT64_MAX-1 && !memcmp(&last,&f.msgq.last,sizeof(last)));
}
static void invalid_arguments(void)
{
    uint8_t data[240];size_t received=99;
    init();
    assert(!ggh_hvc_valid(NULL,1));assert(!ggh_hvc_valid(&f.msgq,0));
    assert(!ggh_hvc_valid(&f.msgq,f.expected_epoch+1));
    assert(ggh_hvc_send(NULL,data,1,0)==GGH_HVC_ERROR);
    assert(ggh_hvc_send(&f.msgq,NULL,1,0)==GGH_HVC_ERROR);
    assert(send(0,0)==GGH_HVC_ERROR);assert(send(0,241)==GGH_HVC_ERROR);
    assert(send(0,SIZE_MAX)==GGH_HVC_ERROR);assert(send(2,1)==GGH_HVC_ERROR);
    assert(ggh_hvc_send(&f.msgq,(void *)(UINTPTR_MAX-1),3,0)==GGH_HVC_ERROR);
    assert(ggh_hvc_send(&f.msgq,&f.msgq,1,0)==GGH_HVC_ERROR);
    assert(ggh_hvc_recv(NULL,data,1,&received)==GGH_HVC_ERROR);
    assert(receive(NULL,1,&received)==GGH_HVC_ERROR);
    assert(receive(data,1,NULL)==GGH_HVC_ERROR);
    assert(receive(data,0,&received)==GGH_HVC_ERROR);
    assert(receive(data,241,&received)==GGH_HVC_ERROR);
    assert(receive((uint8_t *)UINTPTR_MAX,2,&received)==GGH_HVC_ERROR);
    assert(receive(data,1,(size_t *)(UINTPTR_MAX-1))==GGH_HVC_ERROR);
    assert(receive(data,1,(size_t *)((uintptr_t)&received+1))==GGH_HVC_ERROR);
    assert(receive(data,1,(size_t *)data)==GGH_HVC_ERROR);
    assert(receive((uint8_t *)&f.msgq,1,&received)==GGH_HVC_ERROR);
    assert(receive(data,1,(size_t *)&f.msgq)==GGH_HVC_ERROR);
    assert(f.calls==0 && !f.msgq.attempted && received==99);
    for(unsigned field=0;field<4;field++) {
        init();if(field==0)f.msgq.epoch=0;if(field==1)f.msgq.tx_cap=UINT64_MAX;
        if(field==2)f.msgq.rx_cap=UINT64_MAX;
        if(field==3)f.msgq.live=NULL;
        assert(!ggh_hvc_valid(&f.msgq,f.expected_epoch));
        assert(send(0,1)==GGH_HVC_ERROR && !f.calls && !f.msgq.attempted);
    }
    init();f.live_result=2;
    assert(!ggh_hvc_valid(&f.msgq,f.expected_epoch));
    assert(send(0,1)==GGH_HVC_ERROR && !f.calls);
}
static void lease_epochs(void)
{
    uint8_t data[32],saved[32];memset(saved,0x55,sizeof(saved));
    for(unsigned at=1;at<=3;at++) {
        init();f.revoke_at=at;
        assert(send(1,16)==GGH_HVC_ERROR && f.calls==(at==3));
        assert(f.msgq.attempted==(at==3));
        init();f.revoke_at=at;memcpy(data,saved,sizeof(data));size_t received=77;
        assert(receive(data,sizeof(data),&received)==GGH_HVC_ERROR && f.calls==(at==3));
        assert(received==77 && !memcmp(data,saved,sizeof(data)));
    }
    for(unsigned field=0;field<5;field++)for(unsigned at=1;at<=3;at++) {
        init();f.mutate_field=field;f.mutate_at=at;
        assert(send(0,16)==GGH_HVC_ERROR && f.calls==(at==3));
    }
    init();f.revoke_native=1;memcpy(data,saved,sizeof(data));size_t received=88;
    assert(receive(data,sizeof(data),&received)==GGH_HVC_ERROR && f.calls==1);
    assert(received==88 && !memcmp(data,saved,sizeof(data)));
    init();assert(ggh_hvc_valid(&f.msgq,f.expected_epoch)==1);
    ++f.msgq.epoch;assert(!ggh_hvc_valid(&f.msgq,f.expected_epoch));
    f.expected_epoch=f.msgq.epoch;
    assert(ggh_hvc_valid(&f.msgq,f.expected_epoch)==1);
    for(unsigned field=0;field<5;field++) {
        init();f.mutate_field=field;f.mutate_at=1;
        assert(!ggh_hvc_valid(&f.msgq,f.expected_epoch) && !f.calls);
    }
}
static void doorbell_registers_and_errors(void)
{
    static const uint64_t statuses[]={0,60,61,UINT64_MAX,UINT64_MAX-1,5,
        UINT64_C(0xffffffff00000000)};
    uint64_t previous;
    for(unsigned i=0;i<sizeof(statuses)/sizeof(statuses[0]);i++) {
        init();f.status=statuses[i];previous=UINT64_C(0x1212121212121212);
        assert(doorbell(&previous)==(f.status?GGH_HVC_ERROR:GGH_HVC_OK));
        assert(f.bell_calls==1 && f.calls==0 && f.bell.attempted);
        assert(f.bell.native_code==f.status && f.bell.last.x[7]==UINT64_C(0x8000000000000007));
        assert(previous==(f.status?UINT64_C(0x1212121212121212):f.old_flags));
    }
    init();f.bell.rx_cap=f.expected_bell=0;
    assert(ggh_hvc_doorbell_valid(&f.bell,f.expected_epoch)==1);
    assert(doorbell(&previous)==GGH_HVC_OK);
    init();f.clear_flags=0;assert(doorbell(&previous)==GGH_HVC_OK);
    init();f.clear_flags=UINT64_MAX;assert(doorbell(&previous)==GGH_HVC_OK);
    init();assert(!ggh_hvc_doorbell_valid(NULL,1));
    assert(!ggh_hvc_doorbell_valid(&f.bell,0));
    assert(!ggh_hvc_doorbell_valid(&f.bell,f.expected_epoch+1));
    assert(ggh_hvc_doorbell_read_clean(NULL,0,&previous)==GGH_HVC_ERROR);
    assert(doorbell(NULL)==GGH_HVC_ERROR);
    assert(doorbell((uint64_t *)(UINTPTR_MAX-1))==GGH_HVC_ERROR);
    assert(doorbell((uint64_t *)((uintptr_t)&previous+1))==GGH_HVC_ERROR);
    assert(doorbell((uint64_t *)&f.bell)==GGH_HVC_ERROR);
    assert(!f.bell_calls);
    for(unsigned field=0;field<3;field++) {
        init();if(field==0)f.bell.epoch=0;if(field==1)f.bell.rx_cap=UINT64_MAX;
        if(field==2)f.bell.live=NULL;
        assert(!ggh_hvc_doorbell_valid(&f.bell,f.expected_epoch));
        assert(doorbell(&previous)==GGH_HVC_ERROR && !f.bell_calls && !f.bell.attempted);
    }
    init();f.bell_live_result=2;
    assert(!ggh_hvc_doorbell_valid(&f.bell,f.expected_epoch));
    assert(doorbell(&previous)==GGH_HVC_ERROR && !f.bell_calls);
    for(unsigned at=1;at<=3;at++)for(unsigned field=0;field<5;field++) {
        init();previous=99;
        if(field==4)f.revoke_at=at;else {f.mutate_at=at;f.mutate_field=field;}
        assert(doorbell(&previous)==GGH_HVC_ERROR && f.bell_calls==(at==3));
        assert(previous==99 && f.bell.attempted==(at==3));
    }
    init();f.revoke_native=1;previous=99;
    assert(doorbell(&previous)==GGH_HVC_ERROR && f.bell_calls==1 && previous==99);
    for(unsigned field=0;field<4;field++) {
        init();f.mutate_field=field;f.mutate_at=1;
        assert(!ggh_hvc_doorbell_valid(&f.bell,f.expected_epoch) && !f.bell_calls);
    }
}

#if !defined(GG_GUNYAH_HVC_NO_PROBE_HOOK)
struct probe_fixture {
    struct ggh_hvc_probe state;
    struct ggh_hvc_regs output[2];
    uint64_t epoch;
    unsigned calls,checks,live_result,revoke_at,mutate_at,mutate_field;
    unsigned revoke_native,reenter;
};
static struct probe_fixture pf;
static int probe_live(void *cookie,uint64_t epoch)
{
    assert(cookie==&pf && epoch==pf.epoch);++pf.checks;
    if(pf.reenter) {
        struct ggh_hvc_compatibility out,saved;memset(&out,0xaa,sizeof(out));saved=out;
        assert(ggh_hvc_probe_compatibility(&pf.state,&out)==GGH_PROBE_BUSY);
        assert(!memcmp(&out,&saved,sizeof(out)));
    }
    if(pf.mutate_at==pf.checks) {
        if(pf.mutate_field==0)++pf.state.epoch;
        if(pf.mutate_field==1)pf.state.lease_cookie=NULL;
        if(pf.mutate_field==2)pf.state.live=NULL;
        if(pf.mutate_field==3)pf.state.quarantined=1;
    }
    return pf.revoke_at && pf.checks>=pf.revoke_at?0:(int)pf.live_result;
}
static void probe_init(void)
{
    init();memset(&pf,0,sizeof(pf));pf.epoch=UINT64_C(0x123456789abcdef0);
    pf.state.epoch=pf.epoch;pf.state.lease_cookie=&pf;pf.state.live=probe_live;pf.live_result=1;
    /* Low32 words come from UUID_INIT byte order, not the printable fields.
     * Deliberately nonzero high32 bits test the source's lower_32_bits rule. */
    const uint64_t uid[]={UINT64_C(0x90123456cd8fd5c1),UINT64_C(0x01234567db5f53a4),
        UINT64_C(0xfedcba9836ce6592),UINT64_C(0x87654321145f3d67)};
    for(unsigned i=0;i<4;i++)pf.output[0].x[i]=uid[i];
    for(unsigned i=4;i<8;i++)pf.output[0].x[i]=UINT64_C(0xf123456780000000)+i;
    pf.output[1].x[0]=UINT64_C(0xab00000000008001);
    for(unsigned i=1;i<8;i++)pf.output[1].x[i]=UINT64_C(0xfedcba9876543200)+i;
}
void ggh_hvc_test_probe_invoke(struct ggh_hvc_regs *r)
{
    assert(pf.calls<2);
    assert(r->x[0]==(pf.calls?UINT64_C(0xc6008000):UINT64_C(0x8600ff01)));
    for(unsigned i=1;i<8;i++)assert(r->x[i]==0);
    assert(pf.state.busy && !pf.state.complete && !pf.state.quarantined);
    assert(!f.calls && !f.bell_calls); /* Probe never uses either other ABI. */
    *r=pf.output[pf.calls++];
    if(pf.revoke_native==pf.calls)pf.revoke_at=pf.checks+1;
    if(pf.reenter) {
        struct ggh_hvc_compatibility out;
        assert(ggh_hvc_probe_compatibility(&pf.state,&out)==GGH_PROBE_BUSY);
    }
}
static void probe_failed(unsigned result,unsigned calls)
{
    struct ggh_hvc_compatibility out,saved;memset(&out,0x5a,sizeof(out));saved=out;
    assert(ggh_hvc_probe_compatibility(&pf.state,&out)==(int)result);
    assert(!memcmp(&out,&saved,sizeof(out)));
    assert(pf.calls==calls && pf.state.attempted==calls && !pf.state.complete);
    assert(pf.state.quarantined && !pf.state.busy && pf.state.last_result==result);
    if(calls) {
        assert(!memcmp(&pf.state.observed.uid,&pf.output[0],sizeof(pf.output[0])));
        assert(!memcmp(&pf.state.last,&pf.output[calls-1],sizeof(pf.state.last)));
    }
    if(calls==2)assert(!memcmp(&pf.state.observed.identify,&pf.output[1],sizeof(pf.output[1])));
    unsigned checks=pf.checks;
    assert(ggh_hvc_probe_compatibility(&pf.state,&out)==GGH_PROBE_USED);
    assert(!memcmp(&out,&saved,sizeof(out)) && pf.checks==checks && pf.calls==calls);
}
static void probe_registers_and_identity(void)
{
    struct ggh_hvc_compatibility out;
    for(unsigned variant=0;variant<256;variant++) {
        probe_init();pf.output[1].x[0]=((uint64_t)variant<<56)|UINT64_C(0x8001);
        pf.reenter=variant==0;
        assert(ggh_hvc_probe_compatibility(&pf.state,&out)==GGH_PROBE_OK);
        assert(out.epoch==pf.epoch && pf.calls==2 && pf.checks==5);
        assert(!memcmp(&out.uid,&pf.output[0],sizeof(out.uid)));
        assert(!memcmp(&out.identify,&pf.output[1],sizeof(out.identify)));
        assert(pf.state.complete && !pf.state.quarantined && !pf.state.busy);
        assert(pf.state.attempted==2 && pf.state.last_result==GGH_PROBE_OK);
        assert(!memcmp(&pf.state.last,&pf.output[1],sizeof(pf.state.last)));
        struct ggh_hvc_compatibility saved;memset(&out,0x77,sizeof(out));saved=out;
        assert(ggh_hvc_probe_compatibility(&pf.state,&out)==GGH_PROBE_USED);
        assert(!memcmp(&out,&saved,sizeof(out)) && pf.calls==2 && pf.checks==5);
    }
    for(unsigned word=0;word<4;word++)for(unsigned bit=0;bit<32;bit++) {
        probe_init();pf.output[0].x[word]^=UINT64_C(1)<<bit;
        probe_failed(GGH_PROBE_IDENTITY,1);
    }
    probe_init();pf.output[0].x[0]=0xc1d58fcd;pf.output[0].x[1]=0xa4535fdb;
    probe_failed(GGH_PROBE_IDENTITY,1);
    probe_init();pf.output[0].x[0]=UINT64_MAX;probe_failed(GGH_PROBE_IDENTITY,1);
    /* Feature flags are full-width information, including all-ones values. */
    probe_init();for(unsigned i=1;i<8;i++)pf.output[1].x[i]=UINT64_MAX;
    assert(ggh_hvc_probe_compatibility(&pf.state,&out)==GGH_PROBE_OK);
    for(unsigned i=1;i<8;i++)assert(out.identify.x[i]==UINT64_MAX);
    static const uint64_t bad[]={0,1,0x8000,0x8002,0xc001,0x4001,UINT64_MAX,
        UINT64_C(0x100008001),UINT64_C(0xffffffff00008001),UINT64_C(0x80000000008001)};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        probe_init();pf.output[1].x[0]=bad[i];probe_failed(GGH_PROBE_ABI,2);
    }
    for(unsigned bit=16;bit<56;bit++) {
        probe_init();pf.output[1].x[0]|=UINT64_C(1)<<bit;probe_failed(GGH_PROBE_ABI,2);
    }
}
static void probe_arguments_and_lease(void)
{
    struct ggh_hvc_compatibility out,saved;memset(&out,0xab,sizeof(out));saved=out;
    probe_init();
    assert(ggh_hvc_probe_compatibility(NULL,&out)==GGH_PROBE_ARGUMENT);
    assert(ggh_hvc_probe_compatibility(&pf.state,NULL)==GGH_PROBE_ARGUMENT);
    assert(ggh_hvc_probe_compatibility((struct ggh_hvc_probe *)UINTPTR_MAX,&out)==GGH_PROBE_ARGUMENT);
    assert(ggh_hvc_probe_compatibility(&pf.state,(struct ggh_hvc_compatibility *)UINTPTR_MAX)==GGH_PROBE_ARGUMENT);
    assert(ggh_hvc_probe_compatibility((struct ggh_hvc_probe *)((uintptr_t)&pf.state+1),&out)==GGH_PROBE_ARGUMENT);
    assert(ggh_hvc_probe_compatibility(&pf.state,(struct ggh_hvc_compatibility *)((uintptr_t)&out+1))==GGH_PROBE_ARGUMENT);
    assert(ggh_hvc_probe_compatibility(&pf.state,&pf.state.observed)==GGH_PROBE_ARGUMENT);
    assert(ggh_hvc_probe_compatibility(&pf.state,(struct ggh_hvc_compatibility *)&pf.state)==GGH_PROBE_ARGUMENT);
    assert(!pf.calls && !pf.checks && !pf.state.attempted && !pf.state.quarantined);
    assert(!memcmp(&out,&saved,sizeof(out)));
    pf.state.busy=1;assert(ggh_hvc_probe_compatibility(&pf.state,&out)==GGH_PROBE_BUSY);
    assert(pf.state.busy && !pf.state.quarantined && !pf.calls);
    probe_init();pf.state.epoch=0;probe_failed(GGH_PROBE_LEASE,0);
    probe_init();pf.state.live=NULL;probe_failed(GGH_PROBE_LEASE,0);
    probe_init();pf.live_result=0;probe_failed(GGH_PROBE_LEASE,0);
    probe_init();pf.live_result=2;probe_failed(GGH_PROBE_LEASE,0);
    for(unsigned at=1;at<=5;at++) {
        unsigned calls=at<=2?at-1:at<=4?at-2:2;
        probe_init();pf.revoke_at=at;probe_failed(GGH_PROBE_LEASE,calls);
        for(unsigned field=0;field<4;field++) {
            probe_init();pf.mutate_at=at;pf.mutate_field=field;
            probe_failed(GGH_PROBE_LEASE,calls);
        }
    }
    for(unsigned native=1;native<=2;native++) {
        probe_init();pf.revoke_native=native;probe_failed(GGH_PROBE_LEASE,native);
    }
}
static void probe_compatibility_tests(void)
{probe_registers_and_identity();probe_arguments_and_lease();}
#else
static int absent_probe_live(void *cookie,uint64_t epoch)
{return cookie && epoch==7;}
static void probe_compatibility_tests(void)
{
    struct ggh_hvc_probe p={0};struct ggh_hvc_compatibility out,saved;
    p.epoch=7;p.live=absent_probe_live;p.lease_cookie=&p;
    memset(&out,0x5a,sizeof(out));saved=out;
    assert(ggh_hvc_probe_compatibility(&p,&out)==GGH_PROBE_UNAVAILABLE);
    assert(p.quarantined && !p.attempted && !p.complete && !p.busy);
    assert(!memcmp(&out,&saved,sizeof(out)));
    assert(ggh_hvc_probe_compatibility(&p,&out)==GGH_PROBE_USED);
}
#endif
int main(void)
{
    message_registers();status_and_staging();invalid_arguments();lease_epochs();
    doorbell_registers_and_errors();probe_compatibility_tests();
    puts("GPU Gunyah HVC: canonical UID/API1 probe, fixed MSGQ/legacy doorbell, full-width diagnostics, staging, bounded attempts and lease epochs passed (offline hooks only)");
    return 0;
}
