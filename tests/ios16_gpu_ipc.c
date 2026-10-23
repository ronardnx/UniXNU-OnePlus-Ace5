/* Host-only IPC resource oracle. No device, MMIO or physical memory access. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/giulia_gpu_ipc.h"
#include "../ios16/vendor/ArmSmmu4K/ArmSmmu4K.h"

static uint64_t words[128*512],before[128*512];
static unsigned char modified[128*512];
static struct ggm_tables gmu={words,sizeof(words),UINT64_C(0x880100000)};

static uint64_t *slot(uint64_t va)
{
    /* Independent PTE walk and literal expectations, not ggm_leaf/attrs. */
    uint64_t *page=words;
    for(unsigned shift=30;shift>=21;shift-=9) {
        uint64_t entry=page[(va>>shift)&511u];
        assert((entry&3)==3);
        uint64_t pa=entry&UINT64_C(0x0000fffffffff000);
        assert(pa>=gmu.physical && pa-gmu.physical<=gmu.bytes-4096);
        page=words+(pa-gmu.physical)/8;
    }
    return &page[(va>>12)&511u];
}
static uint64_t *parent(uint64_t va)
{
    uint64_t pa=words[va>>30]&UINT64_C(0x0000fffffffff000);
    assert(pa>=gmu.physical && pa-gmu.physical<=gmu.bytes-4096);
    return &words[(pa-gmu.physical)/8+((va>>21)&511u)];
}
static void build(void)
{
    /* Prebuild the whole reserved VA interval solely to detect any writes
     * outside the actual 320-page resource. The unused tail stays invalid. */
    ArmSmmu4KRange ranges[]={
        {0x60010000,UINT64_C(0x990000000),4096,15,0,2},
        {0x66fff000,UINT64_C(0x990001000),4096,15,0,2},
        {0x67000000,0x00400000,0x00400000,15,0,2},
        {0x67400000,UINT64_C(0x990002000),4096,15,0,2}
    };
    ArmSmmu4KConfig config={32,36,gmu.physical,32768};ArmSmmu4KResult result;
    memset(words,0,sizeof(words));memset(modified,0,sizeof(modified));
    assert(ArmSmmu4KBuild(&config,ranges,4,words,128,&result)==ArmSmmu4KSuccess);
    for(unsigned at=0;at<0x400000;at+=4096) {
        uint64_t *leaf=slot(UINT64_C(0x67000000)+at);
        *leaf=0;
        if(at<0x140000)modified[leaf-words]=1;
    }
}
static void save(void){memcpy(before,words,sizeof(words));}
static void unchanged(void){assert(!memcmp(before,words,sizeof(words)));}
static void only_owned_changed(void)
{
    for(unsigned i=0;i<sizeof(words)/sizeof(words[0]);i++)
        if(!modified[i])assert(words[i]==before[i]);
}
static void reject_corrupt(void)
{
    save();
    for(unsigned action=GGM_CHECK;action<=GGM_VERIFY;action++)
        assert(!ggipc_mapping(&gmu,0x38,action));
    assert(!ggipc_mapping(&gmu,0,GGM_CLEAR));
    assert(!ggipc_mapping(&gmu,0,GGM_VERIFY));
    unchanged();
}
static void requests(void)
{
    assert(GGIPC_PHYSICAL==0x00400000 && GGIPC_IOVA==0x67000000 &&
        GGIPC_BYTES==0x00140000 && GGIPC_KIND==19);
    for(unsigned flags=0;flags<0x2000;flags++) {
        int expected=(flags&8u) && !(flags&~0x78u);
        assert(ggipc_request(1,0,flags,19)==expected);
        assert(ggipc_request(0x140000,0x1216,flags,19)==expected);
    }
    for(unsigned shift=0;shift<256;shift++) {
        assert(ggipc_request(1,shift,8,19)==(shift<=22));
        assert(ggipc_request(0x140000,shift<<8,8,19)==(shift<=18));
        assert(ggipc_request(1,shift<<8,8,19)==(shift<=18));
    }
    for(unsigned bytes=1;bytes<=0x140000;bytes+=4095)
        assert(ggipc_request(bytes,0x1216,0x78,19));
    const uint32_t bad_bytes[]={0,0x140001,0x400000,UINT32_MAX};
    for(unsigned i=0;i<sizeof(bad_bytes)/sizeof(bad_bytes[0]);i++)
        assert(!ggipc_request(bad_bytes[i],0,8,19));
    for(unsigned kind=0;kind<32;kind++)
        assert(ggipc_request(1,0,8,kind)==(kind==19));
    assert(!ggipc_request(1,0x10000,8,19));
    assert(!ggipc_request(1,UINT32_MAX,8,19));
    assert(!ggipc_request(1,0,UINT32_MAX,19));
    assert(!ggipc_request(1,0,8,UINT32_MAX));
}
static void mappings(void)
{
    build();save();
    assert(ggipc_mapping(&gmu,0x38,GGM_APPLY));only_owned_changed();
    assert(ggipc_mapping(&gmu,0x38,GGM_VERIFY));
    build();save();
    assert(ggipc_mapping(&gmu,0,GGM_CHECK));unchanged();
    assert(!ggipc_mapping(&gmu,0,GGM_VERIFY));unchanged();
    assert(!ggipc_mapping(&gmu,0,GGM_APPLY));unchanged();
    assert(ggipc_mapping(&gmu,0,GGM_CLEAR));only_owned_changed();
    assert(ggipc_mapping(&gmu,0,GGM_VERIFY));
    for(unsigned at=0;at<0x140000;at+=4096)
        assert(*slot(UINT64_C(0x67000000)+at)==0x00400000u+at);
    for(unsigned flags=0;flags<128;flags++) {
        if(!(flags&8u) || (flags&~0x78u))continue;
        save();assert(ggipc_mapping(&gmu,flags,GGM_CHECK));unchanged();
        assert(ggipc_mapping(&gmu,flags,GGM_APPLY));only_owned_changed();
        assert(ggipc_mapping(&gmu,flags,GGM_VERIFY));
        uint64_t attrs=UINT64_C(0xe03)|(flags&16u?0u:64u)|(flags&32u?0u:128u);
        for(unsigned at=0;at<0x140000;at+=4096)
            assert(*slot(UINT64_C(0x67000000)+at)==((UINT64_C(0x00400000)+at)|attrs));
        save();assert(ggipc_mapping(&gmu,flags,GGM_CLEAR));only_owned_changed();
        assert(ggipc_mapping(&gmu,0,GGM_VERIFY));
    }
    save();
    for(unsigned flags=1;flags<0x2000;flags++) {
        if((flags&8u) && !(flags&~0x78u))continue;
        for(unsigned action=GGM_CHECK;action<=GGM_VERIFY;action++)
            assert(!ggipc_mapping(&gmu,flags,action));
    }
    assert(!ggipc_mapping(&gmu,8,GGM_VERIFY+1));
    assert(!ggipc_mapping(NULL,8,GGM_APPLY));
    unchanged();
    /* Exact VERIFY checks permission and execute bits as well as the PA. */
    assert(ggipc_mapping(&gmu,8,GGM_APPLY));
    *slot(0x6713f000)^=64;
    save();assert(!ggipc_mapping(&gmu,8,GGM_VERIFY));unchanged();
    *slot(0x6713f000)^=64;
    *slot(0x6713f000)|=UINT64_C(1)<<53;
    save();assert(!ggipc_mapping(&gmu,8,GGM_VERIFY));unchanged();
}
static void corruptions(void)
{
    const uint64_t bad[]={
        UINT64_C(0x00540000), /* One page beyond the exact resource. */
        UINT64_C(0x0053e000)|0xe03, /* Owned resource, wrong page. */
        UINT64_C(0x0053f000)|1,UINT64_C(0x0053f000)|2,
        UINT64_C(0x0053f000)|0x400, /* Invalid leaf must be a PA only. */
        UINT64_C(0x0053f000)|0xe03|(UINT64_C(1)<<52),
        UINT64_C(0x0053f000)|0xe03|(UINT64_C(1)<<63)
    };
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        build();*slot(0x6713f000)=bad[i];reject_corrupt();
    }
    build();*parent(0x67000000)=(gmu.physical+gmu.bytes)|3;reject_corrupt();
    build();words[3]=(gmu.physical+gmu.bytes)|3;reject_corrupt();
    build();*parent(0x67000000)=gmu.physical|3;reject_corrupt();
    build();*parent(0x60010000)=*parent(0x67000000);reject_corrupt();
    build();words[2]=words[1];reject_corrupt();
    build();*parent(0x67000000)=0;reject_corrupt();
    build();words[1]=0;reject_corrupt();
    build();struct ggm_tables bad_table=gmu;
    bad_table.bytes=4095;save();assert(!ggipc_mapping(&bad_table,8,GGM_APPLY));unchanged();
    bad_table=gmu;bad_table.words=(uint64_t *)((unsigned char *)words+1);
    save();assert(!ggipc_mapping(&bad_table,8,GGM_APPLY));unchanged();
    /* Internally valid tables still cannot occupy the physical resource. */
    uint64_t saved_physical=gmu.physical;gmu.physical=0x00400000;
    memset(words,0,sizeof(words));
    words[1]=(gmu.physical+4096)|3;
    words[512+((0x67000000u>>21)&511u)]=(gmu.physical+8192)|3;
    assert(ggm_structure_valid(&gmu));reject_corrupt();
    gmu.physical=saved_physical;
}
int main(void)
{
    requests();mappings();corruptions();
    puts("Fixed GMU IPC_CORE request bounds, exact 320-page mapping, permissions, tombstones and atomic corruption rejection pass (host only)");
    return 0;
}
