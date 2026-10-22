/* Fault injection for physical bring-up ordering, timeouts and ownership. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe/WifiPciePolicy.h"
#include "../uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe/WifiPciePhy.h"

static struct {
  unsigned gpio[0x7000/4],gcc[0x80000/4],parf[0x3000/4],phy[0x2000/4];
  unsigned ref_clock;
  unsigned testclk[0x50/4],measure_timeout,clear_timeout,bad_measure,drop_measure_mux,drop_restore_mux;
  unsigned dbi[0x2000/4],atu[0x1000/4],cfg[0x1000/4];
  unsigned enabled[4],calls,fail_call,reads_cfg,writes,stage,phy_writes,cesta_restore_fail,cleaning;
  unsigned phy_timeout,link_timeout,atu_timeout;
  unsigned drop_dbi_offset,ltssm_samples,transition_trace,denied_cap_writes,drop_reset,reset_writes;
  unsigned long long delay;
  unsigned posted, pending, pending_region, pending_offset, pending_value;
  unsigned rcg_timeout,rcg_drop,rcg_restore_drop,rcg_updates,rcg_dynamic_rate;
} h;
static void write_reg_now(void *,unsigned,unsigned,unsigned);
static unsigned *reg(unsigned region,unsigned offset)
{
  assert(!(offset&3));
  switch(region) {
    case GW_GPIO:assert(offset<sizeof(h.gpio));return &h.gpio[offset/4];
    case GW_GCC:assert(offset<sizeof(h.gcc));return &h.gcc[offset/4];
    case GW_PARF:assert(offset<sizeof(h.parf));return &h.parf[offset/4];
    case GW_PHY:assert(offset<sizeof(h.phy));return &h.phy[offset/4];
    case GW_DBI:assert(offset<sizeof(h.dbi));return &h.dbi[offset/4];
    case GW_ATU:assert(offset<sizeof(h.atu));return &h.atu[offset/4];
    case GW_CFG:assert(offset<sizeof(h.cfg));return &h.cfg[offset/4];
    case GW_REFCLK:assert(offset==0);return &h.ref_clock;
    case GW_TESTCLK:assert(offset<sizeof(h.testclk));return &h.testclk[offset/4];
    default:assert(0);return NULL;
  }
}
static unsigned read_reg(void *context,unsigned region,unsigned offset)
{
  (void)context;
  if(h.pending && region==h.pending_region && offset==h.pending_offset) {
    h.pending=0;
    write_reg_now(context,region,offset,h.pending_value);
  }
  if(region==GW_TESTCLK && offset==0x4c) {
    if(h.bad_measure) return 0xffffffffu;
    if(h.testclk[0x48/4]&0x200000u) return h.clear_timeout?10:0;
    if(h.testclk[0x48/4]&0x100000u) return h.measure_timeout?0:(1u<<25)|426700;
  }
  if(region==GW_CFG) {
    assert(h.stage==GW_IDENTIFY);
    assert(h.enabled[GW_RAIL]==511 && h.enabled[GW_DOMAIN]==3 && h.enabled[GW_CLOCK]==4095);
    assert(h.phy_writes==115 && h.atu[1]==(1u<<31));
    h.reads_cfg++;
  }
  if(region==GW_PHY && offset==0x214) return h.phy_timeout?0x40:0;
  if(region==GW_DBI && offset==0xf28) {
    unsigned port=h.dbi[0x710/4],gen2=h.dbi[0x80c/4],cap=h.dbi[0x7c/4];
    /* Model the stock boot configuration, including preserved x2 capability,
       initial one-lane training and the Gen3 settings from the vendor module. */
    if((port&0x3f00a0)!=0x30020 || (gen2&0x1f00)!=0x100 || (cap&0x3f0)!=0x20 ||
       (h.dbi[0x890/4]&0x3000001) || (h.dbi[0x8a8/4]&0xffff00) || (h.dbi[0x8ac/4]&31) ||
       h.dbi[0x10c/4]!=0x55555555 || (h.dbi[0x78/4]&0x7000)!=0x5000)
      return 0;
    if(h.transition_trace) {
      static const unsigned states[]={0,1,2,32,17};
      unsigned sample=h.ltssm_samples++;
      if(sample<4) return states[sample]<<12;
    }
    return h.link_timeout?0:0x11400;
  }
  if(region==GW_DBI && offset==0x80) return h.link_timeout?0:(1u<<29)|0x110000;
  if(region==GW_ATU && offset==4 && h.atu_timeout) return 0;
  return *reg(region,offset);
}
static void write_reg_now(void *context,unsigned region,unsigned offset,unsigned value)
{
  (void)context;
  assert(region!=GW_CFG); /* no BAR/BME/firmware or endpoint mutation */
  assert(region!=GW_REFCLK); /* reference gate read-only */
  assert(region!=GW_GPIO || offset<0x4000); /* BT_EN read-only */
  if(region==GW_ATU) assert(h.stage>=GW_CONFIG);
  if(region==GW_PHY && h.stage==GW_PHY_INIT) h.phy_writes++;
  if(region==GW_GCC && h.stage==GW_RESET && !h.cleaning) {
    assert(h.enabled[GW_CESTA]==1);
    if(offset==0x6b000 || offset==0x6c01c) {
      if(++h.reset_writes==h.drop_reset)return;
      if(offset==0x6c01c && (value&1))assert(!(h.gcc[0x6b000/4]&1));
    }
  }
  if(region==GW_GCC && offset==0x6b05c) {
    if(h.rcg_drop || (h.rcg_restore_drop && !(value&(1u<<20))))return;
  }
  if(region==GW_GCC && offset==0x6b058 && (value&1)) {
    h.rcg_updates++;
    *reg(region,offset)=h.rcg_timeout?value:value&~1u;
    return;
  }
  if(region==GW_DBI)assert(offset!=0x7c && offset!=0x710 && offset!=0xa0 && offset!=0xb40);
  if(region==GW_DBI && offset==0x10c) {
    /* Model the actual RO register ignoring writes without the override. */
    if(!(h.dbi[0x8bc/4]&1)) { h.denied_cap_writes++; return; }
  }
  if(region==GW_DBI && h.drop_dbi_offset==offset) return;
  if(region==GW_DBI && offset==0x78) {
    assert(!(value&0xffff0000u)); /* status is W1C, preserve its evidence */
    h.dbi[offset/4]=(h.dbi[offset/4]&0xffff0000u)|(value&0xffff);h.writes++;return;
  }
  if(region==GW_TESTCLK && offset==0x24 && h.drop_measure_mux) return;
  if(region==GW_TESTCLK && offset==0x24 && h.drop_restore_mux && value==0) return;
  *reg(region,offset)=value;
  h.writes++;
}
static void write_reg(void *context,unsigned region,unsigned offset,unsigned value)
{
  if(!h.posted) { write_reg_now(context,region,offset,value); return; }
  /* Posted writes do not reach this device until a same-address read.
     Overwriting a pending transaction exposes a missing completion read. */
  assert(!h.pending);
  h.pending=1;h.pending_region=region;h.pending_offset=offset;h.pending_value=value;
}
static void delay_us(void *context,unsigned delay)
{ (void)context; assert(delay<=100000); h.delay+=delay; assert(h.delay<2000000); }
static int resource(void *context,unsigned kind,unsigned index,int on)
{
  (void)context;
  assert(kind<=GW_CESTA);
  assert(index<(kind==GW_RAIL?9u:(kind==GW_DOMAIN?2u:(kind==GW_CLOCK?12u:1u))));
  if(on) {
    assert(!(h.enabled[kind]&(1u<<index)));
    if(++h.calls==h.fail_call) return 1;
    if(kind!=GW_RAIL) assert(h.enabled[GW_RAIL]==511);
    if(kind==GW_CLOCK) assert(h.enabled[GW_DOMAIN]==3);
    if(kind==GW_CESTA)assert(h.enabled[GW_DOMAIN]==3 && h.enabled[GW_CLOCK]==2047 && h.stage==GW_CESTA_READY);
    h.enabled[kind]|=1u<<index;
  } else {
    if(kind==GW_CESTA && h.cesta_restore_fail)return 1;
    if(kind==GW_CESTA && h.stage>=GW_PHY_INIT)assert(!(h.gcc[0x6b000/4]&1));
    assert(kind==GW_CESTA || (h.enabled[kind]&(1u<<index)));
    h.enabled[kind]&=~(1u<<index);
  }
  return 0;
}
static void step(void *context,struct gw_report *r)
{
  (void)context; h.stage=r->stage;if(r->error)h.cleaning=1;
  if(r->stage==GW_LINK && !r->error && h.rcg_dynamic_rate)
    h.gcc[0x6b05c/4]=(h.gcc[0x6b05c/4]&~0x371fu)|0x605;
}
static const struct gw_bus bus={NULL,read_reg,write_reg,delay_us,resource,step};
static void init(void)
{
  memset(&h,0,sizeof(h)); h.dbi[0]=0x100017cb; h.cfg[0]=0x110717cb;h.ref_clock=1;
  h.dbi[0x710/4]=0x30120; /* captured reset mode: x2, DLL enabled */
  h.dbi[0x80c/4]=0x2200; h.dbi[0x7c/4]=0x23;
  h.dbi[0x100/4]=0x00010019; /* Secondary PCIe extended capability */
  h.dbi[0x890/4]=0x01000001;h.dbi[0x8a8/4]=0x00ffff00;h.dbi[0x8ac/4]=31;
  h.dbi[0xa0/4]=3;h.dbi[0x78/4]=0x10000;
  h.gcc[0x6b05c/4]=1; /* native SetClockFreqHz selected XO/1 */
}
static int run(struct gw_report *r)
{
  memset(r,0,sizeof(*r));r->version=5;
  return gw_bringup(&bus,r,gw_phy_sequence,115);
}
static void released(void)
{
  assert(!h.enabled[0]&&!h.enabled[1]&&!h.enabled[2]&&!h.enabled[3]);
  for(unsigned i=0;i<4;i++) assert(h.gpio[i*0x400]==0 && h.gpio[i*0x400+1]==0);
}
int main(void)
{
  struct gw_report r;
  unsigned cases=0;
  init();memset(&r,0,sizeof(r));r.version=5;
  assert(gw_bringup_mode(&bus,&r,gw_phy_sequence,115,1)==GW_OK);
  assert(r.stage==GW_XNU_PENDING && r.detail==GW_XNU_LINK_TAG && !r.device);
  assert(!h.reads_cfg && !(h.gpio[0x1004/4]&3) && !(h.parf[0x1b0/4]&256));
  assert(h.enabled[GW_RAIL]==511 && h.enabled[GW_CLOCK]==4095 && h.enabled[GW_CESTA]);cases++;
  init();assert(run(&r)==GW_OK);
  assert(h.gcc[0x6b05c/4]==0x100001 && h.rcg_updates==1);cases++;
  init();h.link_timeout=1;assert(run(&r)==GW_LINK_TIMEOUT);
  assert(h.gcc[0x6b05c/4]==1 && h.rcg_updates==2);released();cases++;
  init();h.link_timeout=1;h.rcg_dynamic_rate=1;
  assert(run(&r)==GW_LINK_TIMEOUT && !r.cleanup_status);
  assert(h.gcc[0x6b05c/4]==0x605 && h.rcg_updates==2);released();cases++;
  init();h.gcc[0x6b05c/4]=0x100001;assert(run(&r)==GW_OK);
  assert(h.rcg_updates==0);cases++;
  init();h.gcc[0x6b05c/4]=0x605;assert(run(&r)==GW_BAD_REGISTER);
  assert(r.detail==0x6b05c && !h.rcg_updates && !h.phy_writes);released();cases++;
  init();h.gcc[0x6b058/4]=1;assert(run(&r)==GW_BAD_REGISTER);
  assert(!h.rcg_updates && !h.phy_writes);released();cases++;
  init();h.rcg_drop=1;assert(run(&r)==GW_BAD_REGISTER);
  assert(!h.phy_writes);released();cases++;
  init();h.rcg_timeout=1;assert(run(&r)==GW_BAD_REGISTER);
  assert(!h.phy_writes && r.retained_rails==511 && r.cleanup_status && h.enabled[GW_CLOCK]);cases++;
  init();h.rcg_restore_drop=1;h.link_timeout=1;assert(run(&r)==GW_LINK_TIMEOUT);
  assert(r.retained_rails==511 && r.cleanup_status && h.enabled[GW_CLOCK]==4095);cases++;
  init();h.posted=1;assert(run(&r)==GW_OK);assert(!h.pending);cases++;
  init();h.posted=1;h.link_timeout=1;
  assert(run(&r)==GW_LINK_TIMEOUT);assert(!h.pending);released();cases++;
  _Static_assert(sizeof(struct gw_report)==168,"report ABI");
  init();assert(run(&r)==GW_OK);assert(r.stage==GW_DONE && r.device==0x110717cb);
  assert(h.reads_cfg==4 && h.enabled[0]==511 && h.enabled[2]==4095);cases++;
  for(unsigned edge=1;edge<=4;edge++) {
    init();h.drop_reset=edge;assert(run(&r)==GW_BAD_REGISTER && r.stage==GW_RESET);
    assert(!h.phy_writes&&!h.reads_cfg);released();cases++;
  }
  init();assert(run(&r)==GW_OK);
  assert(r.pipe_measure_status==GW_MEASURE_OK && r.pipe_hz>124000000 && r.pipe_hz<126000000);
  for(unsigned i=0;i<sizeof(h.testclk)/sizeof(h.testclk[0]);i++) assert(!h.testclk[i]);
  init();h.measure_timeout=1;assert(run(&r)==GW_OK);
  assert(r.pipe_measure_status==GW_MEASURE_COUNT_TIMEOUT && !r.pipe_hz);cases++;
  init();h.clear_timeout=1;assert(run(&r)==GW_OK);
  assert(r.pipe_measure_status==GW_MEASURE_CLEAR_TIMEOUT && !r.pipe_hz);cases++;
  init();h.bad_measure=1;assert(run(&r)==GW_OK);
  assert(r.pipe_measure_status==GW_MEASURE_BAD_REGISTER && !r.pipe_hz);cases++;
  init();h.drop_measure_mux=1;assert(run(&r)==GW_OK);
  assert(r.pipe_measure_status==GW_MEASURE_BAD_REGISTER && !r.pipe_hz);cases++;
  init();h.testclk[0x48/4]=0x100000;assert(run(&r)==GW_OK);
  assert(r.pipe_measure_status==GW_MEASURE_BUSY && h.testclk[0x48/4]==0x100000);cases++;
  init();h.testclk[0]=0xffffffffu;assert(run(&r)==GW_OK);
  assert(r.pipe_measure_status==GW_MEASURE_BAD_REGISTER);cases++;
  init();h.testclk[0]=0x1000;h.testclk[0x24/4]=0x42;h.testclk[0x48/4]=0x8000;
  assert(run(&r)==GW_OK && r.pipe_measure_status==GW_MEASURE_OK);
  assert(h.testclk[0]==0x1000 && h.testclk[0x24/4]==0x42 && h.testclk[0x48/4]==0x8000);cases++;
  init();h.drop_restore_mux=1;assert(run(&r)==GW_BAD_REGISTER);
  assert(r.pipe_measure_status==GW_MEASURE_RESTORE_ERROR && !r.pipe_hz && !h.reads_cfg);
  released();cases++;
  init();assert(run(&r)==GW_OK);
  assert(r.port_link_before==0x30120 && r.port_link_after==0x30120 &&
         r.gen2_ctrl==0x2100 && r.link_cap==0x23 && h.dbi[0x8bc/4]==0);
  assert(h.dbi[0xa0/4]==3 && h.dbi[0x78/4]==0x15000 && !h.dbi[0xb40/4]);
  for(unsigned failure=0;failure<7;failure++) {
    static const unsigned offsets[]={0x80c,0x890,0x8a8,0x8ac,0x10c,0x8bc,0x78};
    init();h.drop_dbi_offset=offsets[failure];
    assert(run(&r)==GW_BAD_REGISTER);released();assert(!h.reads_cfg);cases++;
    assert(h.dbi[0x8bc/4]==0);
  }
  init();h.dbi[0x710/4]=0xffffffffu;assert(run(&r)==GW_BAD_REGISTER);
  released();assert(!h.reads_cfg);cases++;
  for(unsigned header=0;header<3;header++) {
    static const unsigned malformed[]={0,0xffffffffu,0x10010001};
    init();h.dbi[0x100/4]=malformed[header];assert(run(&r)==GW_BAD_REGISTER);
    assert(h.dbi[0x8bc/4]==0&&!h.reads_cfg);released();cases++;
  }
  init();h.transition_trace=1;assert(run(&r)==GW_OK);
  assert(r.ltssm_seen_lo==((1u<<17)|7u) && r.ltssm_seen_hi==1 && r.ltssm_changes==4);cases++;
  init();h.dbi[0x8bc/4]=0x100;assert(run(&r)==GW_OK);
  assert(h.dbi[0x8bc/4]==0x100);cases++;
  for(unsigned failure=1;failure<=24;failure++) {
    init();h.fail_call=failure;assert(run(&r)==GW_RESOURCE_ERROR);released();assert(!h.reads_cfg);cases++;
  }
  init();h.phy_timeout=1;assert(run(&r)==GW_PHY_TIMEOUT);released();assert(!h.reads_cfg);cases++;
  init();h.link_timeout=1;assert(run(&r)==GW_LINK_TIMEOUT);released();assert(!h.reads_cfg);cases++;
  assert(r.detail&256);
  init();h.link_timeout=1;h.gcc[0x6b000/4]=1;h.gcc[0x6c01c/4]=1;
  assert(run(&r)==GW_LINK_TIMEOUT);released();assert(h.gcc[0x6b000/4]==1&&h.gcc[0x6c01c/4]==1);cases++;
  init();h.link_timeout=1;h.cesta_restore_fail=1;assert(run(&r)==GW_LINK_TIMEOUT);
  assert(r.retained_rails==511 && h.enabled[GW_CESTA] && h.enabled[GW_CLOCK]==4095 &&
         h.enabled[GW_DOMAIN]==3 && h.enabled[GW_RAIL]==511 && !(h.gpio[1]&2) && !(h.gpio[0x1004/4]&2));cases++;
  init();h.ref_clock=0;assert(run(&r)==GW_BAD_REGISTER);released();assert(!h.reads_cfg);cases++;
  init();h.ref_clock=0xffffffffu;assert(run(&r)==GW_BAD_REGISTER);released();assert(!h.reads_cfg);cases++;
  init();h.atu_timeout=1;assert(run(&r)==GW_ATU_TIMEOUT);released();assert(!h.reads_cfg);cases++;
  init();h.cfg[0]=0xffffffffu;assert(run(&r)==GW_WRONG_DEVICE);released();assert(h.reads_cfg==1);cases++;
  init();h.cfg[1]=4;assert(run(&r)==GW_UNEXPECTED_DMA);released();cases++;
  init();h.gpio[0]=1u<<11;assert(run(&r)==GW_BAD_PIN);assert(!h.writes&&!h.calls);cases++;
  init();h.gpio[1]=3;assert(run(&r)==GW_ALREADY_ACTIVE);assert(!h.writes&&!h.calls);cases++;
  init();h.gpio[0x4004/4]=3;assert(run(&r)==GW_ALREADY_ACTIVE);assert(r.detail==4&&!h.writes&&!h.calls);cases++;
  init();h.gpio[0]=4;assert(run(&r)==GW_BAD_PIN);assert(!h.writes&&!h.calls);cases++;
  init();h.dbi[0]=0;assert(run(&r)==GW_BAD_REGISTER);released();assert(!h.reads_cfg);cases++;
  init();h.gcc[0x6b000/4]=0xffffffffu;assert(run(&r)==GW_BAD_REGISTER);released();assert(!h.reads_cfg);cases++;
  init();assert(gw_bringup(&bus,&r,gw_phy_sequence,114)==GW_BAD_SEQUENCE);assert(!h.calls&&!h.writes);cases++;
  printf("Wi-Fi PCIe policy: %u ordering, resource-failure, timeout and no-DMA cases passed\n",cases);
  return 0;
}
