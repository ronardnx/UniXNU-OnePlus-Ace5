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
  unsigned enabled[3],calls,fail_call,reads_cfg,writes,stage,phy_writes;
  unsigned phy_timeout,link_timeout,atu_timeout;
  unsigned drop_dbi_offset,ltssm_samples,transition_trace,denied_cap_writes;
  unsigned long long delay;
} h;
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
    /* A DLL held disabled, simulation mode, or mismatched lane configuration
       must not train just because the mock endpoint has the expected ID. */
    if((port&0x3f00a0)!=0x30020 || (gen2&0x1f00)!=0x200 || (cap&0x3f0)!=0x20)
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
static void write_reg(void *context,unsigned region,unsigned offset,unsigned value)
{
  (void)context;
  assert(region!=GW_CFG); /* no BAR/BME/firmware or endpoint mutation */
  assert(region!=GW_REFCLK); /* reference gate read-only */
  assert(region!=GW_GPIO || offset<0x4000); /* BT_EN read-only */
  if(region==GW_ATU) assert(h.stage>=GW_CONFIG);
  if(region==GW_PHY && h.stage==GW_PHY_INIT) h.phy_writes++;
  if(region==GW_DBI && offset==0x7c) {
    /* Model the actual RO register ignoring writes without the override. */
    if(!(h.dbi[0x8bc/4]&1)) { h.denied_cap_writes++; return; }
  }
  if(region==GW_DBI && h.drop_dbi_offset==offset) return;
  if(region==GW_TESTCLK && offset==0x24 && h.drop_measure_mux) return;
  if(region==GW_TESTCLK && offset==0x24 && h.drop_restore_mux && value==0) return;
  *reg(region,offset)=value;
  h.writes++;
}
static void delay_us(void *context,unsigned delay)
{ (void)context; assert(delay<=100000); h.delay+=delay; assert(h.delay<2000000); }
static int resource(void *context,unsigned kind,unsigned index,int on)
{
  (void)context;
  assert(kind<=GW_CLOCK);
  assert(index<(kind==GW_RAIL?9u:(kind==GW_DOMAIN?2u:12u)));
  if(on) {
    assert(!(h.enabled[kind]&(1u<<index)));
    if(++h.calls==h.fail_call) return 1;
    if(kind!=GW_RAIL) assert(h.enabled[GW_RAIL]==511);
    if(kind==GW_CLOCK) assert(h.enabled[GW_DOMAIN]==3);
    h.enabled[kind]|=1u<<index;
  } else {
    assert(h.enabled[kind]&(1u<<index));
    h.enabled[kind]&=~(1u<<index);
  }
  return 0;
}
static void step(void *context,struct gw_report *r)
{ (void)context; h.stage=r->stage; }
static const struct gw_bus bus={NULL,read_reg,write_reg,delay_us,resource,step};
static void init(void)
{
  memset(&h,0,sizeof(h)); h.dbi[0]=0x100017cb; h.cfg[0]=0x110717cb;h.ref_clock=1;
  h.dbi[0x710/4]=0x10080; /* x1, fast simulation, DLL disabled */
  h.dbi[0x80c/4]=0x2100; h.dbi[0x7c/4]=0x13; /* x1 / Gen3 capability */
}
static int run(struct gw_report *r)
{
  memset(r,0,sizeof(*r));r->version=5;
  return gw_bringup(&bus,r,gw_phy_sequence,115);
}
static void released(void)
{
  assert(!h.enabled[0]&&!h.enabled[1]&&!h.enabled[2]);
  for(unsigned i=0;i<4;i++) assert(h.gpio[i*0x400]==0 && h.gpio[i*0x400+1]==0);
}
int main(void)
{
  struct gw_report r;
  unsigned cases=0;
  _Static_assert(sizeof(struct gw_report)==168,"report ABI");
  init();assert(run(&r)==GW_OK);assert(r.stage==GW_DONE && r.device==0x110717cb);
  assert(h.reads_cfg==4 && h.enabled[0]==511 && h.enabled[2]==4095);cases++;
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
  assert(r.port_link_before==0x10080 && r.port_link_after==0x30020 &&
         r.gen2_ctrl==0x2200 && r.link_cap==0x23 && h.dbi[0x8bc/4]==0);
  for(unsigned failure=0;failure<4;failure++) {
    static const unsigned offsets[]={0x710,0x80c,0x7c,0x8bc};
    init();h.drop_dbi_offset=offsets[failure];
    assert(run(&r)==GW_BAD_REGISTER);released();assert(!h.reads_cfg);cases++;
    assert(h.dbi[0x8bc/4]==0);
    if(offsets[failure]==0x8bc) assert(h.denied_cap_writes==1);
  }
  init();h.dbi[0x710/4]=0xffffffffu;assert(run(&r)==GW_BAD_REGISTER);
  released();assert(!h.reads_cfg);cases++;
  init();h.transition_trace=1;assert(run(&r)==GW_OK);
  assert(r.ltssm_seen_lo==((1u<<17)|7u) && r.ltssm_seen_hi==1 && r.ltssm_changes==4);cases++;
  init();h.dbi[0x8bc/4]=0x100;assert(run(&r)==GW_OK);
  assert(h.dbi[0x8bc/4]==0x100);cases++;
  for(unsigned failure=1;failure<=23;failure++) {
    init();h.fail_call=failure;assert(run(&r)==GW_RESOURCE_ERROR);released();assert(!h.reads_cfg);cases++;
  }
  init();h.phy_timeout=1;assert(run(&r)==GW_PHY_TIMEOUT);released();assert(!h.reads_cfg);cases++;
  init();h.link_timeout=1;assert(run(&r)==GW_LINK_TIMEOUT);released();assert(!h.reads_cfg);cases++;
  assert(r.detail&256);
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
