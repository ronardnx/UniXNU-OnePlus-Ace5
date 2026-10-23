/* Host-only ABI oracle and deterministic changing-device-memory fixture. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_memstore.h"

static uint32_t backing[32768/4];
struct changes { unsigned calls; uint32_t context; unsigned mode; };
static void read_barrier(void *cookie)
{
    struct changes *change=(struct changes *)cookie;
    change->calls++;
    /* This host fixture changes values between the two production samples.
     * Target barriers are independently supplied by the kernel backend.
     */
    if(change->calls==2 && change->mode) {
        if(change->mode&1)backing[change->context*10]++;
        if(change->mode&2)backing[change->context*10+2]++;
    }
}
static void layouts(void)
{
    assert(GGMS_BYTES==32768 && GGMS_STRIDE==40 && GGMS_RECORDS==819);
    assert(GGMS_USER_FIRST==1 && GGMS_USER_MAX==811 && GGMS_GLOBAL_CONTEXT==812);
    for(unsigned context=0;context<=812;context++) {
        uint32_t at=UINT32_MAX;
        assert(ggms_offset(context,GGMS_SOP,&at) && at==context*40);
        assert(ggms_offset(context,GGMS_EOP,&at) && at==context*40+8);
        assert(at+4<=32768);
    }
    uint32_t at=123;
    for(unsigned context=813;context<820;context++) {
        assert(!ggms_offset(context,GGMS_SOP,&at) && at==123);
    }
    assert(!ggms_offset(UINT32_MAX,GGMS_SOP,&at) && at==123);
    for(unsigned field=0;field<44;field++) {
        if(field==0 || field==8)continue;
        assert(!ggms_offset(1,field,&at) && at==123);
    }
    assert(!ggms_offset(1,0,NULL));
}
static void requests(void)
{
    for(unsigned flags=0;flags<0x2000;flags++) {
        unsigned expected=(flags&7)==7 && !(flags&~0x37fu);
        assert(ggms_request(32768,0,flags,22)==(int)expected);
    }
    for(unsigned shift=0;shift<=22;shift++) {
        assert(ggms_request(32768,shift,0x33f,22));
        assert(ggms_request(32768,shift<<8,0x33f,22)==(shift<=15));
        assert(ggms_request(1,shift<<8,0x33f,22)==(shift<=15));
    }
    assert(ggms_request(32767,0x0f16,7,22));
    assert(!ggms_request(32768,0x1016,7,22));
    const uint32_t invalid_align[]={23,255,23u<<8,255u<<8,0x10000,UINT32_MAX};
    for(unsigned i=0;i<sizeof(invalid_align)/sizeof(invalid_align[0]);i++)
        assert(!ggms_request(1,invalid_align[i],7,22));
    const uint32_t invalid_bytes[]={0,32769,65536,UINT32_MAX};
    for(unsigned i=0;i<sizeof(invalid_bytes)/sizeof(invalid_bytes[0]);i++)
        assert(!ggms_request(invalid_bytes[i],0,7,22));
    for(unsigned kind=0;kind<27;kind++)
        assert(ggms_request(1,0,7,kind)==(kind==22));
    assert(!ggms_request(1,0,UINT32_MAX,22));
}
static void observations(void)
{
    for(unsigned context=0;context<=812;context++) {
        backing[context*10]=context*17u;
        backing[context*10+2]=context*13u;
        struct changes change={0,context,0};
        struct ggms_observation got={0};
        assert(ggms_snapshot(backing,sizeof(backing),context,&got,read_barrier,&change));
        assert(got.sop==context*17u && got.eop==context*13u && got.stable && change.calls==2);
    }
    /* Either changing field invalidates stability, including u32 rollover. */
    for(unsigned mode=1;mode<=3;mode++) {
        backing[10]=UINT32_MAX;backing[12]=UINT32_MAX;
        struct changes change={0,1,mode};
        struct ggms_observation got={0};
        assert(ggms_snapshot(backing,sizeof(backing),1,&got,read_barrier,&change));
        assert(!got.stable && change.calls==2);
        assert(got.sop==((mode&1)?0:UINT32_MAX));
        assert(got.eop==((mode&2)?0:UINT32_MAX));
    }
    struct changes change={0,1,0};
    struct ggms_observation got={123,456,789},before=got;
    assert(!ggms_snapshot(NULL,sizeof(backing),1,&got,read_barrier,&change));
    assert(!ggms_snapshot(backing,sizeof(backing)-1,1,&got,read_barrier,&change));
    assert(!ggms_snapshot(backing,sizeof(backing)+1,1,&got,read_barrier,&change));
    assert(!ggms_snapshot((unsigned char *)backing+1,sizeof(backing),1,&got,read_barrier,&change));
    assert(!ggms_snapshot((void *)(UINTPTR_MAX-3),sizeof(backing),1,&got,read_barrier,&change));
    assert(!ggms_snapshot(backing,sizeof(backing),813,&got,read_barrier,&change));
    assert(!ggms_snapshot(backing,sizeof(backing),UINT32_MAX,&got,read_barrier,&change));
    assert(!ggms_snapshot(backing,sizeof(backing),1,NULL,read_barrier,&change));
    assert(!ggms_snapshot(backing,sizeof(backing),1,&got,NULL,&change));
    assert(!memcmp(&got,&before,sizeof(got)) && !change.calls);
}
int main(void)
{
    layouts();requests();observations();
    puts("Dedicated memstore ABI, bounded request validation and ordered timestamp observations pass (host only)");
    return 0;
}
