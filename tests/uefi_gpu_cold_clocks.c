#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe/GpuColdClocks.h"
#include "../ios16/giulia_gpu_power_handoff.h"
struct model { GPP_U32 gpu[0xa000/4],gcc[0x2000/4],reset,ready,vote;
    unsigned writes,pll_writes,stuck_pll,stuck_smmu,stuck_rcg; };
static GPP_U32 *ptr(struct model *m,GPP_U32 a)
{
    if(a>=0x3d90000 && a<0x3d9a000)return &m->gpu[(a-0x3d90000)/4];
    if(a>=0x171000 && a<0x173000)return &m->gcc[(a-0x171000)/4];
    if(a==0x3d7e000)return &m->reset;
    if(a==0x3d7e070)return &m->ready;
    assert(a==0x152000);return &m->vote;
}
static GPP_U32 rd(void *p,GPP_U32 a){return *ptr(p,a);}
static void wr(void *p,GPP_U32 a,GPP_U32 v)
{
    struct model *m=p;m->writes++;
    assert(a!=0x3d7e000 && !(a>=0x3da0000 && a<0x3de0000));
    if(a>=0x3d91000 && a<0x3d92000)m->pll_writes++;
    if(a==0x171004 || a==0x171010)v&=~0x80000000u;
    if(a==0x3d9911c || a==0x3d99004)v=0;
    if(a==0x3d97000)v=m->stuck_smmu?0x80000001u:1;
    if(a==0x3d91004 && v==1 && !m->stuck_pll)*ptr(m,0x3d91000)|=0x80000000u;
    if(a==0x3d91000 && (v&1))v|=0x20000000u;
    if((a==0x3d99318 || a==0x3d993ec) && !m->stuck_rcg)v&=~1u;
    *ptr(m,a)=v;
}
static void delay(void *p,unsigned us){(void)p;assert(us==10);}
static void init(struct model *m,struct gpp_report *r)
{
    memset(m,0,sizeof(*m));gpp_report_init(r);
    *ptr(m,0x171004)=*ptr(m,0x171010)=0x80000003u;
    *ptr(m,0x171018)=*ptr(m,0x171154)=1;
    *ptr(m,0x3d99168)=0x80000202u;*ptr(m,0x3d91000)=0x100;
    *ptr(m,0x3d97000)=0x80000000u;*ptr(m,0x3d9913c)=0x2001;
    *ptr(m,0x3d99144)=1;m->reset=1;
    r->phase=GPP_PHASE_CX;r->retained=1;r->flags=GPP_CFG_INHERITED|GPP_ACD_CONSUMED;
    r->attempted=r->confirmed=(1u<<GPP_CX)|(1u<<GPP_ACD);
}
int main(void)
{
    struct model m;struct gpp_report r,out;
    struct gcc_io io={&m,rd,wr,delay};
    init(&m,&r);assert(gcc_prepare(&io,&r)==GPP_OK);
    assert(ggph_decode(&r,sizeof(r),&out)==GGPH_PREPARED);
    assert(rd(&m,0x3d9931c)==0x303 && rd(&m,0x3d993f0)==0x505);
    assert(rd(&m,0x3d91000)==0xa0000105u);
    init(&m,&r);m.reset=0;assert(gcc_prepare(&io,&r)==GPP_ERR_INITIAL);assert(!m.pll_writes);
    init(&m,&r);m.stuck_smmu=1;assert(gcc_prepare(&io,&r)==GPP_ERR_TIMEOUT);assert(!m.pll_writes);
    init(&m,&r);m.stuck_pll=1;assert(gcc_prepare(&io,&r)==GPP_ERR_TIMEOUT);
    assert(!rd(&m,0x3d9931c));
    init(&m,&r);m.stuck_rcg=1;assert(gcc_prepare(&io,&r)==GPP_ERR_TIMEOUT);
    assert(!rd(&m,0x3d9931c));
    init(&m,&r);*ptr(&m,0x3d91000)=0xe0000005u;
    assert(gcc_prepare(&io,&r)==GPP_ERR_INITIAL && !m.writes);
    puts("UEFI cold clocks: real initial state, handoff, dynamic PLL, reset and bounded failure gates passed");
}
