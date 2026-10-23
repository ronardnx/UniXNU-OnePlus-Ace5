/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * SM8650 cold clocks, validated through the USB hardware terminal.
 * Algorithm: UniXNU-sheng 251b291 GpuPowerBoard.h; PLL constants:
 * pinned gpucc-pineapple.c (520 MHz PLL1, GMU /2, HUB GPLL0 /3).
 * No reset release, SMMU translation writes, or voltage programming.
 */
#ifndef GIULIA_GPU_COLD_CLOCKS_H
#define GIULIA_GPU_COLD_CLOCKS_H
#include "GpuPowerPolicy.h"
struct gcc_io {
    void *cookie;
    GPP_U32 (*read)(void *,GPP_U32);
    void (*write)(void *,GPP_U32,GPP_U32);
    void (*delay)(void *,unsigned);
};
static inline int gcc_expect(const struct gcc_io *io,struct gpp_report *r,
    GPP_U32 address,GPP_U32 mask,GPP_U32 expected,unsigned polls)
{
    unsigned i;
    r->operation=GPP_OP_VALIDATE;r->expected=expected;r->mask=mask;
    r->flags|=GPP_EXPECTED_VALID|GPP_MASK_VALID;
    for(i=0;i<polls;i++) {
        r->observed=io->read(io->cookie,address);r->flags|=GPP_OBSERVED_VALID;
        if(r->observed==0xffffffffu)return !gpp_fail(r,GPP_ERR_READ,0);
        if((r->observed&mask)==expected)return 1;
        r->polls++;if(i+1<polls)io->delay(io->cookie,10);
    }
    (void)gpp_fail(r,polls==1?GPP_ERR_INITIAL:GPP_ERR_TIMEOUT,0);return 0;
}
static inline int gcc_write(const struct gcc_io *io,struct gpp_report *r,
    GPP_U32 address,GPP_U32 value,GPP_U32 mask)
{
    r->operation=GPP_OP_ENABLE;r->attempted|=1u<<r->resource;r->retained=1;
    io->write(io->cookie,address,value);
    return gcc_expect(io,r,address,mask,value&mask,1);
}
static inline int gcc_rcg(const struct gcc_io *io,struct gpp_report *r,
    GPP_U32 address,GPP_U32 cfg)
{
    GPP_U32 v;
    io->write(io->cookie,address,io->read(io->cookie,address)|2);
    if(!gcc_expect(io,r,address,0x80000000u,0,200))return 0;
    v=(io->read(io->cookie,address+4)&~(0x1fu|(7u<<8)|(3u<<12)|(1u<<20)))|cfg;
    if(!gcc_write(io,r,address+4,v,0xffffffffu))return 0;
    io->write(io->cookie,address,io->read(io->cookie,address)|1);
    if(!gcc_expect(io,r,address,1,0,200))return 0;
    io->write(io->cookie,address,io->read(io->cookie,address)&~2u);
    return gcc_expect(io,r,address+4,0xffffffffu,v,1);
}
static inline unsigned gcc_prepare(const struct gcc_io *io,struct gpp_report *r)
{
    static const GPP_U32 cold[]={0x3d91000,0x3d91004,0x3d99318,0x3d9931c,0x3d993ec,0x3d993f0};
    static const GPP_U32 pll[][2]={{0x10,0x4444001b},{0x14,0x1555},{0x20,0x20485699},
        {0x24,0x00182261},{0x28,0x82aa299c},{0x1c,5},{0x30,3},{0x34,0x9000},{0x38,0x34}};
    static const unsigned branches[]={GPP_CFG,GPP_MEMNOC_CLK,GPP_AHB_CLK,GPP_SMMU_CLK};
    static const unsigned regs[]={GPP_REG_CFG,GPP_REG_MEMNOC,GPP_REG_AHB,GPP_REG_SMMU};
    static const GPP_U32 final[]={0x3d99004,0x3d9900c,0x3d9913c,0x3d99144,0x3d99148};
    unsigned i;
    r->flags|=GPP_DIRECT_CLOCKS;
    gpp_action(r,GPP_PHASE_CLOCKS,GPP_OP_VALIDATE,GPP_GMU_RATE);
    for(i=0;i<6;i++)if(!gcc_expect(io,r,cold[i],0xffffffffu,i?0:0x100,1))return r->code;
    for(i=0;i<4;i++) {
        GPP_U32 address=(GPP_U32)gpp_register_address(regs[i]),v=io->read(io->cookie,address);
        r->resource=branches[i];
        if(i<2)v&=~2u;
        if(!gcc_write(io,r,address,v|1,i==2?0x80000000u:1u) ||
           !gcc_expect(io,r,address,i==2?0x80000000u:0x80000001u,i==2?0:1,200))return r->code;
        r->confirmed|=1u<<branches[i];
    }
    gpp_action(r,GPP_PHASE_RESET,GPP_OP_VALIDATE,GPP_GMU_CLK);
    if(!gcc_expect(io,r,0x3d7e000,0xffffffffu,1,1) ||
       !gcc_expect(io,r,0x3d7e070,0xffffffffu,0,1))return r->code;
    gpp_action(r,GPP_PHASE_GMU_RATE,GPP_OP_SET_RATE,GPP_GMU_RATE);
    if(!gcc_write(io,r,0x152000,io->read(io->cookie,0x152000)|0x18000,0xffffffffu))return r->code;
    for(i=0;i<9;i++)if(!gcc_write(io,r,0x3d91000+pll[i][0],pll[i][1],0xffffffffu))return r->code;
    if(!gcc_write(io,r,0x3d91000,io->read(io->cookie,0x3d91000)|4,7) ||
       !gcc_write(io,r,0x3d91004,1,0xffffffffu) ||
       !gcc_expect(io,r,0x3d91000,0x80000000u,0x80000000u,200) ||
       !gcc_write(io,r,0x3d91018,io->read(io->cookie,0x3d91018)|7,0xffffffffu) ||
       !gcc_write(io,r,0x3d91000,io->read(io->cookie,0x3d91000)|1,7))return r->code;
    gpp_action(r,GPP_PHASE_HUB_RATE,GPP_OP_SET_RATE,GPP_HUB_RATE);
    if(!gcc_rcg(io,r,0x3d993ec,0x505))return r->code;
    r->confirmed|=1u<<GPP_HUB_RATE;r->values[13]=200000000;r->valid_reads|=1u<<13;
    gpp_action(r,GPP_PHASE_GMU_RATE,GPP_OP_SET_RATE,GPP_GMU_RATE);
    if(!gcc_rcg(io,r,0x3d99318,0x303))return r->code;
    r->confirmed|=1u<<GPP_GMU_RATE;r->values[12]=260000000;r->valid_reads|=1u<<12;
    gpp_action(r,GPP_PHASE_CLOCKS,GPP_OP_ENABLE,GPP_GMU_CLK);
    for(i=0;i<5;i++)if(!gcc_write(io,r,final[i],io->read(io->cookie,final[i])|1,
        i?1u:0x80000000u))return r->code;
    if(!gcc_expect(io,r,0x3d9913c,0x80000001u,1,200))return r->code;
    r->confirmed|=1u<<GPP_GMU_CLK;
    /* Remaining branches are inherited observations, not resource leases. */
    for(i=0;i<GPP_REGISTER_COUNT;i++) {
        GPP_U32 v=io->read(io->cookie,(GPP_U32)gpp_register_address(i));
        if(v==0xffffffffu)return gpp_fail(r,GPP_ERR_READ,0);
        r->values[i]=v;r->valid_reads|=1u<<i;
    }
    if(!gcc_expect(io,r,0x3d7e000,0xffffffffu,1,1))return r->code;
    r->flags|=GPP_READY;r->phase=GPP_PHASE_DONE;r->operation=GPP_OP_NONE;
    gpp_report_seal(r);return GPP_OK;
}
#endif
