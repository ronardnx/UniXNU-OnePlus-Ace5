#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/wifi/giulia_wifi_link.h"
#include "../uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe/WifiPcieReport.h"
#include "../uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe/WifiPcieEncoding.h"
static struct {
    uint32_t regs[GWL_REGIONS][0x51000/4];
    unsigned writes,config_reads,phy_reads,logs,failed_checks,link_delay,poll_count;
    unsigned timeout,invalid_link,drop_perst,drop_ltssm,drop_atu,atu_timeout;
    unsigned us;
} h;
static uint32_t rd(void *ctx,unsigned region,unsigned off) {
    (void)ctx;assert(region<GWL_REGIONS && !(off&3) && off<gwl_sizes[region]);
    if(region==GWL_PHY)h.phy_reads++;
    if(region==GWL_ROOT && off==0xf28) {
        if(!(h.regs[GWL_PARF][0x1b0/4]&256))return 1;
        if(h.invalid_link)return 0xffffffffu;
        h.poll_count++;
        return h.timeout || h.poll_count<=h.link_delay?1:0x11400;
    }
    if(region==GWL_ROOT && off==0x80)
        return h.invalid_link?0xffffffffu:h.timeout || h.poll_count<=h.link_delay?0x10110008:0x30110008;
    if(region==GWL_ROOT && off==0x1004 && h.atu_timeout)return 0;
    if(region==GWL_CFG) {
        assert(h.regs[GWL_ROOT][0x1004/4]&(1u<<31));
        assert(h.regs[GWL_ROOT][0x1014/4]==0x1000000);
        h.config_reads++;
    }
    return h.regs[region][off/4];
}
static void wr(void *ctx,unsigned region,unsigned off,uint32_t value) {
    (void)ctx;assert(region!=GWL_CFG);assert(!(off&3) && off<gwl_sizes[region]);
    assert(region==GWL_GPIO || region==GWL_PARF || region==GWL_ROOT);
    if(region==GWL_GPIO) {
        assert(off==0x4e004);
        if(h.drop_perst && (value&2))return;
        value=(value&~1u)|((value>>1)&1u);
    }
    if(region==GWL_PARF) {assert(off==0x1b0);if(h.drop_ltssm && (value&256))return;}
    if(region==GWL_ROOT && off==h.drop_atu)return;
    h.regs[region][off/4]=value;h.writes++;
}
static void pause_us(void *ctx,unsigned us) {(void)ctx;h.us+=us;assert(h.us<200000);}
static void log_value(void *ctx,const char *name,uint64_t pa,uint32_t value,uint32_t mask,uint32_t expected) {
    (void)ctx;assert(name && pa);h.logs++;
    if(mask && (value==0xffffffffu || (value&mask)!=expected))h.failed_checks++;
}
static const struct gwl_io io={NULL,rd,wr,pause_us,log_value};
static void reset(void) {
    memset(&h,0,sizeof(h));
    h.regs[GWL_GCC][8/4]=h.regs[GWL_GCC][0x1004/4]=0x4070000;
    h.regs[GWL_GPIO][0]=0x3c3;h.regs[GWL_GPIO][1]=3;h.regs[GWL_GPIO][0x4e000/4]=0x201;
    h.regs[GWL_GPIO][0x2008/4]=h.regs[GWL_GPIO][0x3008/4]=192;
    h.regs[GWL_ROOT][0]=0x11317cb;h.regs[GWL_ROOT][0x80c/4]=0x130190;
    h.regs[GWL_PHY][0x214/4]=0x86;h.regs[GWL_REFCLK][0x100/4]=1;
    h.regs[GWL_CFG][0]=0x110717cb;
}
int main(void) {
    struct gwl_state s;unsigned n=0;
    reset();assert(gwl_train(&io,&s)==0 && s.stage==6 && s.device==0x110717cb);
    assert(s.polls==1 && h.config_reads==2 && !h.failed_checks && h.logs>90);n++;
    reset();h.link_delay=20;assert(gwl_train(&io,&s)==0 && s.polls==21);n++;
    reset();h.timeout=1;assert(gwl_train(&io,&s)==GWL_LINK_TIMEOUT && s.polls==21);
    assert(!h.config_reads && !h.regs[GWL_GPIO][0x4e004/4] && !(h.regs[GWL_PARF][0x1b0/4]&256));n++;
    reset();h.invalid_link=1;assert(gwl_train(&io,&s)==GWL_LINK_TIMEOUT);assert(!h.config_reads);n++;
    reset();h.regs[GWL_GCC][8/4]=0;assert(gwl_train(&io,&s)==GWL_NOT_PREPARED);
    assert(!h.writes && !h.phy_reads && h.failed_checks==1);n++;
    reset();h.regs[GWL_GPIO][0x4e004/4]=3;assert(gwl_train(&io,&s)==GWL_NOT_PREPARED);assert(!h.writes);n++;
    reset();h.regs[GWL_GPIO][0x2008/4]=64;assert(gwl_train(&io,&s)==GWL_NOT_PREPARED);assert(!h.writes && !h.phy_reads);n++;
    reset();h.regs[GWL_GPIO][0x3008/4]=64;assert(gwl_train(&io,&s)==GWL_NOT_PREPARED);assert(!h.writes && !h.phy_reads);n++;
    reset();h.regs[GWL_PHY][0x214/4]=64;assert(gwl_train(&io,&s)==GWL_PHY_NOT_READY);assert(!h.writes);n++;
    reset();h.drop_perst=1;assert(gwl_train(&io,&s)==GWL_WRITE_FAILED);assert(!h.config_reads);n++;
    reset();h.drop_ltssm=1;assert(gwl_train(&io,&s)==GWL_WRITE_FAILED);assert(!h.config_reads);n++;
    reset();h.atu_timeout=1;assert(gwl_train(&io,&s)==GWL_ATU_TIMEOUT);assert(!h.config_reads);n++;
    reset();h.drop_atu=0x1014;assert(gwl_train(&io,&s)==GWL_WRITE_FAILED);assert(!h.config_reads);n++;
    reset();h.regs[GWL_CFG][0]=0xffffffffu;assert(gwl_train(&io,&s)==GWL_WRONG_DEVICE);n++;
    reset();h.regs[GWL_CFG][1]=4;assert(gwl_train(&io,&s)==GWL_UNEXPECTED_DMA);n++;
    struct gw_report r;char encoded[225];memset(&r,0,sizeof(r));
    r.version=5;r.stage=18;r.detail=GW_XNU_LINK_TAG;r.rails=511;r.clocks=4095;
    r.phy=0x86;r.root_device=0x11317cb;r.gpio_after=3;r.perst=0x2010000;r.ref_clock=1;
    assert(gw_report_base64(encoded,sizeof(encoded),&r));assert(gwl_handoff(encoded,1)==2 && !gwl_handoff(encoded,0));n++;
    r.error=7;assert(gw_report_base64(encoded,sizeof(encoded),&r));assert(!gwl_handoff(encoded,1));n++;
    r.error=0;r.perst|=3;assert(gw_report_base64(encoded,sizeof(encoded),&r));assert(!gwl_handoff(encoded,1));n++;
    encoded[30]='!';assert(!gwl_handoff(encoded,1));n++;
    r.stage=16;r.perst|=3;r.device=0x110717cb;r.link=1u<<29;r.elbi=1024;
    assert(gw_report_base64(encoded,sizeof(encoded),&r));assert(gwl_handoff(encoded,0)==1 && !gwl_handoff(encoded,1));n++;
    printf("XNU PCIe link: %u handoff, ownership, post-EBS training, dropped-write and no-DMA cases passed\n",n);
    return 0;
}
