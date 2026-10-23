/* Offline arithmetic tests; no pointer dereference or hardware operation. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../ios16/audio/giulia_audio_mapping.h"

int main(void)
{
    uint64_t pa=123,base=456;size_t bytes=789,bias=987,offset=654;
    assert(gamap_regions[GAMAP_RX].physical==0x06ac0000 && gamap_regions[GAMAP_RX].bytes==0x1000);
    assert(gamap_regions[GAMAP_RX_SWR].bytes==0x60a8);
    assert(gamap_regions[GAMAP_ADSP_RAM].physical==0x9df00000 && gamap_regions[GAMAP_ADSP_RAM].bytes==0x5280000);
    assert(gamap_devices[0].address==0x6c && gamap_devices[0].reset_gpio==173);
    assert(gamap_devices[1].address==0x6d && gamap_devices[1].reset_gpio==178);
    assert(gamap_devices[2].address==0x0e01170224ULL);
    for(unsigned i=0;i<GAMAP_REGIONS;i++) {
        const struct gamap_region *r=&gamap_regions[i];
        int usable=r->enabled && r->kind==GAMAP_MMIO;
        pa=123;
        assert(gamap_resolve(i,0,1,&pa)==usable);
        if(!usable) {assert(pa==123);continue;}
        assert(pa==r->physical);
        assert(gamap_resolve(i,r->bytes-1,1,&pa) && pa==r->physical+r->bytes-1);
        for(unsigned width=1;width<=8;width*=2) {
            assert(gamap_resolve(i,r->bytes-width,width,&pa));
            pa=123;
            assert(!gamap_resolve(i,r->bytes,width,&pa) && pa==123);
            assert(!gamap_resolve(i,SIZE_MAX,width,&pa) && pa==123);
            if(width>1)assert(!gamap_resolve(i,1,width,&pa));
        }
    }
    assert(!gamap_resolve(GAMAP_REGIONS,0,4,&pa));
    assert(!gamap_resolve((unsigned)-1,0,4,&pa));
    assert(!gamap_resolve(GAMAP_RX,0,0,&pa));
    assert(!gamap_resolve(GAMAP_RX,0,3,&pa));
    assert(!gamap_resolve(GAMAP_RX,0,16,&pa));
    assert(!gamap_resolve(GAMAP_RX,0,4,NULL));
    assert(!gamap_resolve(GAMAP_RX_SWR,0x60a8,4,&pa));
    assert(!gamap_resolve(GAMAP_ADSP_RAM,0,4,&pa));
    assert(!gamap_resolve(GAMAP_WSA,0,4,&pa));
    assert(!gamap_resolve(GAMAP_WSA_SWR,0,4,&pa));
    assert(gamap_page_window(GAMAP_IPCC,16384,&base,&bytes,&bias));
    assert(base==0x404000 && bytes==0x4000 && bias==0x2000);
    assert(gamap_page_window(GAMAP_RX_SWR,4096,&base,&bytes,&bias));
    assert(base==0x6ad0000 && bytes==0x7000 && bias==0);
    /* OS page rounding must not make the padding valid registers. */
    assert(!gamap_resolve(GAMAP_RX_SWR,0x60ac,4,&pa));
    base=456;bytes=789;bias=987;
    assert(!gamap_page_window(GAMAP_ADSP_RAM,4096,&base,&bytes,&bias));
    assert(!gamap_page_window(GAMAP_RX,0,&base,&bytes,&bias));
    assert(!gamap_page_window(GAMAP_RX,8192,&base,&bytes,&bias));
    assert(!gamap_page_window(GAMAP_RX,4096,NULL,&bytes,&bias));
    assert(base==456 && bytes==789 && bias==987);
    assert(gamap_iova_offset(0x10000000,0x10000000,&offset) && offset==0);
    assert(gamap_iova_offset(0x1fffffff,1,&offset) && offset==0xfffffff);
    offset=654;
    assert(!gamap_iova_offset(0xfffffff,1,&offset));
    assert(!gamap_iova_offset(0x1fffffff,2,&offset));
    assert(!gamap_iova_offset(0x20000000,1,&offset));
    assert(!gamap_iova_offset(UINT64_MAX,1,&offset));
    assert(!gamap_iova_offset(0x10000000,0,&offset));
    assert(!gamap_iova_offset(0x10000000,SIZE_MAX,&offset));
    assert(!gamap_iova_offset(0x10000000,1,NULL));
    assert(offset==654);
    puts("Audio resource boundaries and address-space isolation passed");
    return 0;
}
