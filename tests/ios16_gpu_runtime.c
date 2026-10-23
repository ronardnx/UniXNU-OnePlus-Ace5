#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_pack.h"
#include "../ios16/gpu/giulia_gpu_memory.h"
#include "../ios16/gpu/giulia_gpu_power.h"

struct fake { unsigned calls, last_window, fail; uint32_t cfg,cx,clock; unsigned ahb_zero; };
static int read_reg(void *cookie,unsigned window,uint32_t offset,uint32_t *value)
{
    struct fake *f=cookie;
    f->last_window=window;
    if (++f->calls==f->fail) return 1;
    *value=1;
    if (window==GGP_GCC && offset==4) *value=f->cfg;
    if (window==GGP_GPUCC) {
        if (offset==0x9108) *value=0;
        else if (offset==0x9168) *value=f->cx;
        else if(offset==0x911c && f->ahb_zero)*value=0;
        else *value=f->clock;
    }
    return 0;
}

int main(int argc,char **argv)
{
    struct fake f={0,0,0,1,1u<<31,1,0};
    struct ggp_io io={&f,read_reg}; struct ggp_report report;
    unsigned calls,i;
    ggp_observe(&io,&report); assert(report.stage==GGP_OBSERVED && report.valid_fields==0x1fff); calls=f.calls;
    /* Captured Ace 5: AHB CBCR reads zero (including after enable), while
     * separately enabled SMMU/GCC gates allow actual SMMU and TCM access. */
    f.ahb_zero=1;ggp_observe(&io,&report);
    assert(report.stage==GGP_OBSERVED && report.ahb==0 && report.valid_fields==0x1fff);
    f.ahb_zero=0;
    for (i=1;i<=calls;++i) {
        f.calls=0;f.fail=i;ggp_observe(&io,&report);
        assert(report.stage==GGP_IO_ERROR && f.calls==i);
        assert((unsigned)__builtin_popcount(report.valid_fields)==i-1);
    }
    f.fail=0;f.calls=0;f.cfg=0;ggp_observe(&io,&report);
    assert(report.stage==GGP_CFG_OFF && f.last_window==GGP_GCC && report.valid_fields==7);
    f.cfg=1;f.cx=0;ggp_observe(&io,&report);
    assert(report.stage==GGP_CX_OFF && f.last_window==GGP_GPUCC);
    f.cx=1u<<31;f.clock=1u<<31;ggp_observe(&io,&report);
    assert(report.stage==GGP_CLOCK_OFF && f.last_window==GGP_GPUCC);
    f.cfg=UINT32_MAX;ggp_observe(&io,&report);assert(report.stage==GGP_IO_ERROR);
    {
        struct gg_fw_block blocks[3]={{0x4100,0,1,0x100,16,GG_ICACHE},
            {0x4f00,0,7,0x1100,32,GG_ICACHE},{0x4100,4,0,0,48,GG_ICACHE}};
        struct gg_reservation plan[3],before[3]; size_t count;
        assert(gg_memory_plan(blocks,3,plan,3,&count)==GG_FW_OK && count==1);
        assert(plan[0].iova==0x4000 && plan[0].bytes==0x2000 && plan[0].persistent==1);
        memcpy(before,plan,sizeof(plan)); blocks[2].address=0x7000;
        assert(gg_memory_plan(blocks,3,plan,3,&count)==GG_FW_RANGE && !count);
        assert(!memcmp(before,plan,sizeof(plan)));
        blocks[2].address=0x4100;
        assert(gg_memory_plan(blocks,3,plan,0,&count)==GG_FW_CAPACITY && !count);
        assert(!memcmp(before,plan,sizeof(plan)));
        blocks[0].region=GG_NONE;
        assert(gg_memory_plan(blocks,3,plan,3,&count)==GG_FW_RANGE);
    }
    if (argc==2) {
        FILE *fp=fopen(argv[1],"rb");
        uint8_t *bytes=malloc(GG_FIRMWARE_PACK_BYTES+1); struct gg_pack pack;
        struct gg_fw_block blocks[64]; struct gg_reservation plan[64];size_t count,n;
        assert(fp && bytes);
        assert(fread(bytes,1,GG_FIRMWARE_PACK_BYTES+1,fp)==GG_FIRMWARE_PACK_BYTES);
        fclose(fp);
        assert(gg_pack_open(bytes,GG_FIRMWARE_PACK_BYTES,&pack));
        assert(gg_fw_plan(pack.asset[0].data,pack.asset[0].size,blocks,64,&count)==GG_FW_OK);
        assert(gg_memory_plan(blocks,count,plan,64,&n)==GG_FW_OK && n==5);
        assert(plan[4].iova==0x60000000 && plan[4].bytes==8192 && plan[4].persistent);
        for (i=0;i<GG_ASSETS;++i) {
            size_t offset=(size_t)(pack.asset[i].data-bytes);
            bytes[offset]^=1;
            { struct gg_pack bad; assert(!gg_pack_open(bytes,GG_FIRMWARE_PACK_BYTES,&bad));
              assert(!bad.asset[0].data); }
            bytes[offset]^=1;
        }
        assert(!gg_pack_open(bytes,GG_FIRMWARE_PACK_BYTES-1,&pack));
        free(bytes);
        printf("Pinned pack accepted, all four asset corruptions rejected; GMU reservations=%zu\n",n);
    }
    printf("GPU memory planning and gated observation: passed (%u read failures injected)\n",calls);
    return 0;
}
