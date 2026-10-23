/* Host-only provider-import page table oracle. No physical memory or device IO. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_fence_mapping.h"

static uint64_t words[128*512],before[128*512];
static struct ggm_tables gmu={words,sizeof(words),UINT64_C(0x880100000)};
static struct ggf_layout layout;

static uint64_t *walk(uint64_t va)
{
    /* Independent walk: do not use production helpers for expectations. */
    uint64_t *page=words;
    for(unsigned shift=30;shift>=21;shift-=9) {
        uint64_t entry=page[(va>>shift)&511u];
        assert((entry&UINT64_C(0xffff000000000fff))==3);
        uint64_t pa=entry&UINT64_C(0x0000fffffffff000);
        assert(pa>=gmu.physical && pa-gmu.physical<=gmu.bytes-4096);
        page=words+(pa-gmu.physical)/8;
    }
    return page+((va>>12)&511u);
}
static uint64_t *parent(void){return words+512+((0x68000000u>>21)&511u);}
static void save(void){memcpy(before,words,sizeof(words));}
static void unchanged(void){assert(!memcmp(before,words,sizeof(words)));}
static void build(void)
{
    memset(words,0,sizeof(words));
    words[1]=(gmu.physical+4096)|3;
    words[512+((0x60010000u>>21)&511u)]=(gmu.physical+8192)|3;
    words[1024+16]=UINT64_C(0x990000000)|0xe03;
}
static void prepared(void){build();assert(ggf_mapping_prepare(&gmu));}
static void aperture_only(void)
{
    uint64_t *leaves=walk(0x68000000);
    for(unsigned i=0;i<128*512;i++)
        if(&words[i]<leaves || &words[i]>=leaves+512)assert(words[i]==before[i]);
}
static void rejects_all(void)
{
    save();
    for(unsigned action=GGM_CHECK;action<=GGM_VERIFY;action++)
        assert(!ggf_mapping(&gmu,&layout,0x38,action));
    unchanged();
}
static void preparation(void)
{
    assert(GGF_IOVA==0x68000000 && GGF_APERTURE==0x200000);
    build();save();assert(ggf_mapping_prepare(&gmu));
    assert(*parent()==((gmu.physical+3*4096)|3));
    for(unsigned i=0;i<128*512;i++)if(&words[i]!=parent())assert(words[i]==before[i]);
    for(unsigned i=0;i<512;i++)assert(walk(0x68000000)[i]==0);
    save();assert(!ggf_mapping_prepare(&gmu));unchanged();
    assert(ggf_mapping(&gmu,NULL,0,GGM_CHECK));
    assert(ggf_mapping(&gmu,NULL,0,GGM_VERIFY));unchanged();

    /* Unreferenced does not imply available: skip dirty pages and pages
     * already referenced as data by an unrelated valid IOVA. */
    build();words[3*512+511]=0xdeadbeef;
    save();assert(ggf_mapping_prepare(&gmu));assert(*parent()==((gmu.physical+4*4096)|3));
    assert(words[3*512+511]==before[3*512+511]);
    build();words[1024+17]=(gmu.physical+3*4096)|0xe03;
    save();assert(ggf_mapping_prepare(&gmu));assert(*parent()==((gmu.physical+4*4096)|3));
    assert(words[1024+17]==before[1024+17]);

    /* Every table page is referenced, while the requested L2 slot is free. */
    build();memset(words+512,0,4096);
    for(unsigned i=0;i<126;i++)words[512+i]=(gmu.physical+(i+2)*4096)|3;
    save();assert(!ggf_mapping_prepare(&gmu));unchanged();
    build();for(unsigned i=3;i<128;i++)words[i*512+511]=1;
    save();assert(!ggf_mapping_prepare(&gmu));unchanged();
    build();gmu.bytes=3*4096;save();assert(!ggf_mapping_prepare(&gmu));unchanged();
    gmu.bytes=sizeof(words);

    build();words[1]=0;save();assert(!ggf_mapping_prepare(&gmu));unchanged();
    build();words[2]=words[1];save();assert(!ggf_mapping_prepare(&gmu));unchanged();
    build();words[512+257]=words[512+256];save();assert(!ggf_mapping_prepare(&gmu));unchanged();
    build();words[3]=(gmu.physical+gmu.bytes)|3;save();assert(!ggf_mapping_prepare(&gmu));unchanged();
    build();*parent()=1;save();assert(!ggf_mapping_prepare(&gmu));unchanged();
    save();assert(!ggf_mapping_prepare(NULL));unchanged();
}
static void imports(void)
{
    const unsigned page_abis[]={4096,16384,65536};
    for(unsigned abi=0;abi<3;abi++) {
        assert(ggf_layout(page_abis[abi],&layout));
        prepared();save();
        assert(ggf_mapping(&gmu,&layout,8,GGM_CHECK));unchanged();
        assert(!ggf_mapping(&gmu,&layout,8,GGM_VERIFY));unchanged();
        for(unsigned flags=0;flags<0x200;flags++) {
            if(!(flags&8) || (flags&~0x178u))continue;
            save();assert(ggf_mapping(&gmu,&layout,flags,GGM_APPLY));aperture_only();
            uint64_t attrs=UINT64_C(0xe03)|(flags&16?0:64)|(flags&32?0:128);
            for(unsigned i=0;i<512;i++) {
                uint64_t expected=i<layout.bytes/4096?(layout.physical+i*4096)|attrs:0;
                assert(walk(0x68000000)[i]==expected);
            }
            save();assert(ggf_mapping(&gmu,&layout,flags,GGM_VERIFY));unchanged();
            assert(ggf_mapping(&gmu,&layout,flags,GGM_CHECK));unchanged();
            assert(!ggf_mapping(&gmu,NULL,0,GGM_VERIFY));unchanged();
            assert(ggf_mapping(&gmu,&layout,flags,GGM_CLEAR));aperture_only();
            assert(ggf_mapping(&gmu,NULL,0,GGM_VERIFY));
        }
    }
    prepared();save();
    for(unsigned flags=0;flags<0x2000;flags++) {
        if((flags&8) && !(flags&~0x178u))continue;
        for(unsigned action=0;action<=GGM_VERIFY;action++)
            assert(!ggf_mapping(&gmu,&layout,flags,action));
    }
    assert(!ggf_mapping(&gmu,&layout,8,GGM_VERIFY+1));
    assert(!ggf_mapping(NULL,&layout,8,GGM_APPLY));
    assert(!ggf_mapping(&gmu,NULL,8,GGM_CHECK));
    assert(!ggf_mapping(&gmu,NULL,0,GGM_CLEAR));
    assert(!ggf_mapping(&gmu,NULL,0,GGM_APPLY));unchanged();
}
static void corruptions(void)
{
    assert(ggf_layout(4096,&layout));
    unsigned last=layout.bytes/4096-1;
    uint64_t last_pa=layout.physical+last*4096;
    const uint64_t bad[]={last_pa+4096,last_pa-4096,last_pa|1,last_pa|2,
        last_pa|0x400,last_pa|0xe03|(UINT64_C(1)<<52),
        last_pa|0xe03|(UINT64_C(1)<<63)};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        prepared();walk(0x68000000)[last]=bad[i];rejects_all();
    }
    prepared();walk(0x68000000)[511]=last_pa;rejects_all();
    save();assert(!ggf_mapping(&gmu,NULL,0,GGM_CHECK));
    assert(!ggf_mapping(&gmu,NULL,0,GGM_VERIFY));unchanged();
    prepared();walk(0x60010000)[1]=layout.physical|0xe03;rejects_all();
    prepared();walk(0x60010000)[1]=layout.physical;rejects_all();
    prepared();words[2]=words[1];rejects_all();
    prepared();words[512+257]=*parent();rejects_all();
    prepared();*parent()=gmu.physical|3;rejects_all();
    prepared();*parent()=(gmu.physical+gmu.bytes)|3;rejects_all();
    prepared();*parent()=0;rejects_all();
    prepared();words[1]=0;rejects_all();

    prepared();assert(ggf_mapping(&gmu,&layout,8,GGM_APPLY));
    walk(0x68000000)[last]^=64;save();
    assert(!ggf_mapping(&gmu,&layout,8,GGM_VERIFY));unchanged();
    walk(0x68000000)[last]^=64;walk(0x68000000)[last]|=UINT64_C(1)<<53;save();
    assert(!ggf_mapping(&gmu,&layout,8,GGM_VERIFY));unchanged();

    struct ggf_layout good=layout;
    prepared();layout.physical+=4096;rejects_all();layout=good;
    layout.bytes+=4096;rejects_all();layout=good;
    layout.bytes=0;rejects_all();layout=good;
    layout.tx_payload++;rejects_all();layout=good;
    layout.page_bytes=8192;rejects_all();layout=good;

    uint64_t saved_physical=gmu.physical;
    gmu.physical=layout.physical-4096;prepared();rejects_all();
    gmu.physical=saved_physical;prepared();
    struct ggm_tables invalid=gmu;invalid.bytes=4095;save();
    assert(!ggf_mapping_prepare(&invalid));assert(!ggf_mapping(&invalid,&layout,8,GGM_APPLY));
    invalid=gmu;invalid.words=(uint64_t *)((unsigned char *)words+1);
    assert(!ggf_mapping_prepare(&invalid));assert(!ggf_mapping(&invalid,&layout,8,GGM_APPLY));
    invalid=gmu;invalid.physical=UINT64_C(1)<<36;
    assert(!ggf_mapping_prepare(&invalid));assert(!ggf_mapping(&invalid,&layout,8,GGM_APPLY));
    unchanged();
}
int main(void)
{
    preparation();imports();corruptions();
    puts("Hardware-fence GMU import: empty reservation, all host page ABIs, exact permissions, no aliases and atomic rejection pass (host only)");
    return 0;
}
