/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Bounded, polling-only SM8650 PCIe0 bring-up. Register provenance and the
 * DT PHY sequence are recorded in hardware/sm8650-wifi.json. No DMA, MSI,
 * endpoint BAR writes, firmware execution, or network-interface emulation.
 * This header is shared with host fault-injection tests.
 */
#ifndef GIULIA_WIFI_PCIE_POLICY_H
#define GIULIA_WIFI_PCIE_POLICY_H
#include "WifiPcieReport.h"

enum gw_region { GW_GPIO, GW_GCC, GW_PARF, GW_PHY, GW_DBI, GW_ATU, GW_CFG, GW_REFCLK, GW_TESTCLK };
enum gw_resource { GW_RAIL, GW_DOMAIN, GW_CLOCK };
enum gw_stage {
  GW_START=1, GW_ABI=2, GW_RESOLVE=3, GW_MAP=4, GW_PINS=5,
  GW_POWER=6, GW_ENABLE=7, GW_DOMAINS=8, GW_CLOCKS=9, GW_RESET=10,
  GW_PHY_INIT=11, GW_PHY_READY=12, GW_LINK=13, GW_CONFIG=14,
  GW_IDENTIFY=15, GW_DONE=16
};
enum gw_error {
  GW_OK=0, GW_RESOURCE_ERROR=1, GW_BAD_PIN=2, GW_ALREADY_ACTIVE=3,
  GW_BAD_SEQUENCE=4, GW_BAD_REGISTER=5, GW_PHY_TIMEOUT=6,
  GW_LINK_TIMEOUT=7, GW_ATU_TIMEOUT=8, GW_WRONG_DEVICE=9,
  GW_UNEXPECTED_DMA=10
};
#define GW_RAILS 9u
#define GW_DOMAINS_COUNT 2u
#define GW_CLOCKS_COUNT 12u
#define GW_PIPE_CLOCK 11u
struct gw_phy_word { unsigned offset, value, delay_us; };
struct gw_bus {
  void *context;
  unsigned (*read)(void *, unsigned, unsigned);
  void (*write)(void *, unsigned, unsigned, unsigned);
  void (*delay)(void *, unsigned);
  int (*resource)(void *, unsigned, unsigned, int);
  void (*step)(void *, struct gw_report *);
};

static void gw_step(const struct gw_bus *b, struct gw_report *r, unsigned stage, unsigned detail)
{
  r->stage=stage; r->detail=detail; b->step(b->context,r);
}
static unsigned gw_read(const struct gw_bus *b,unsigned region,unsigned offset)
{ return b->read(b->context,region,offset); }
static void gw_write(const struct gw_bus *b,unsigned region,unsigned offset,unsigned value)
{ b->write(b->context,region,offset,value); }
static void gw_mask(const struct gw_bus *b,unsigned region,unsigned offset,unsigned clear,unsigned set)
{ gw_write(b,region,offset,(gw_read(b,region,offset)&~clear)|set); }
static int gw_pin_owned(unsigned value)
{ return value!=0xffffffffu && (!(value&(1u<<11)) || (value&(1u<<12))); }

/* GPIO indices: WLAN_EN16, PERST94, CLKREQ95, WAKE96; read-only BT17, SW18, WLAN_SW19. */
static void gw_gpio(const struct gw_bus *b,unsigned index,unsigned value)
{ gw_mask(b,GW_GPIO,index*0x1000+4,2,value?2u:0u); }

#include "WifiClockMeasure.h"

/* Match dw_pcie_setup/link_set_max_link_width (Linux v6.12): enable DLL,
 * disable fast-link simulation, and configure all three x2 width fields.
 * SM8650 PCIe0 and the captured PHY sequence have two physical lanes.
 * DBI RO access is scoped to the capability write and always restored.
 */
static int gw_configure_link(const struct gw_bus *b,struct gw_report *r)
{
  unsigned misc=gw_read(b,GW_DBI,0x8bc),cap=gw_read(b,GW_DBI,0x7c);
  unsigned gen2=gw_read(b,GW_DBI,0x80c),port=gw_read(b,GW_DBI,0x710);
  r->port_link_before=port;
  r->link_cap=cap; r->gen2_ctrl=gen2;
  if(misc==0xffffffffu || cap==0xffffffffu || gen2==0xffffffffu ||
     port==0xffffffffu || !(cap&0x3f0u)) return 0;
  gw_write(b,GW_DBI,0x710,(port&~(0x3f0000u|0x80u))|0x30020u);
  gw_write(b,GW_DBI,0x80c,(gen2&~0x1f00u)|0x200u);
  gw_write(b,GW_DBI,0x8bc,misc|1u);
  gw_write(b,GW_DBI,0x7c,(cap&~0x3f0u)|0x20u);
  gw_write(b,GW_DBI,0x8bc,misc);
  r->port_link_after=gw_read(b,GW_DBI,0x710);
  r->gen2_ctrl=gw_read(b,GW_DBI,0x80c);
  r->link_cap=gw_read(b,GW_DBI,0x7c);
  return r->port_link_after!=0xffffffffu && r->gen2_ctrl!=0xffffffffu &&
    r->link_cap!=0xffffffffu &&
    (r->port_link_after&(0x3f0000u|0xa0u))==0x30020u &&
    (r->gen2_ctrl&0x1f00u)==0x200u && (r->link_cap&0x3f0u)==0x20u &&
    gw_read(b,GW_DBI,0x8bc)==misc;
}

static void gw_link_snapshot(const struct gw_bus *b,struct gw_report *r)
{
  /* All values are sampled before failure cleanup changes clocks or pins. */
  r->pipe_gate=gw_read(b,GW_GCC,0x6b048);
  r->phy_common=gw_read(b,GW_PHY,0x1f8);
  r->phy_status2=gw_read(b,GW_PHY,0x218);
  r->phy_status3=gw_read(b,GW_PHY,0x21c);
  r->clkreq=((gw_read(b,GW_GPIO,0x2000)&0xffff)<<16)|
    (gw_read(b,GW_GPIO,0x2004)&0xffff);
}

static int gw_sequence_valid(const struct gw_phy_word *seq,unsigned count)
{
  unsigned i;
  if (!seq || count!=115 || seq[0].offset!=0x240 || seq[0].value!=3 ||
      seq[count-1].offset!=0x244 || seq[count-1].value!=3) return 0;
  for(i=0;i<count;i++) if ((seq[i].offset&3) || seq[i].offset>0x1ffc ||
      seq[i].value>255 || seq[i].delay_us>1000) return 0;
  return 1;
}

static int gw_bringup(const struct gw_bus *b, struct gw_report *r,
                     const struct gw_phy_word *seq,unsigned count)
{
  unsigned cfg[4],io[4],i,j,domains=0,rails=0,clocks=0,last_ltssm=64;
  unsigned core_reset=0,phy_reset=0,reset_changed=0,pins_changed=0,controller=0,atu_changed=0;
  int result=GW_OK;
  if (!b || !r || !b->read || !b->write || !b->delay || !b->resource || !b->step)
    return GW_RESOURCE_ERROR;
  if (!gw_sequence_valid(seq,count)) { r->error=GW_BAD_SEQUENCE; return r->error; }
  gw_step(b,r,GW_PINS,0);
  for(i=0;i<4;i++) {
    cfg[i]=gw_read(b,GW_GPIO,i*0x1000); io[i]=gw_read(b,GW_GPIO,i*0x1000+4);
    if (!gw_pin_owned(cfg[i]) || io[i]==0xffffffffu ||
        ((cfg[i]>>2)&15)>(i==2?1u:0u)) { r->detail=i; r->error=GW_BAD_PIN; return r->error; }
  }
  r->gpio_before=io[0];
  /* A live inherited radio must be investigated instead of resetting it. */
  if (io[0]&3) { r->error=GW_ALREADY_ACTIVE; return r->error; }
  i=gw_read(b,GW_GPIO,0x4004);
  if(i==0xffffffffu || (i&3)) {
    r->detail=4; r->error=GW_ALREADY_ACTIVE; return r->error;
  }
  for(i=0;i<GW_RAILS;i++) {
    gw_step(b,r,GW_POWER,i);
    if(b->resource(b->context,GW_RAIL,i,1)) { result=GW_RESOURCE_ERROR; goto fail; }
    rails|=1u<<i; r->rails=rails;
  }
  gw_step(b,r,GW_ENABLE,0);
  pins_changed=1;
  gw_gpio(b,1,0);
  gw_write(b,GW_GPIO,0x1000,(cfg[1]&~0x3ffu)|0x201u); /* PERST low */
  gw_write(b,GW_GPIO,0x2000,(cfg[2]&~0x3ffu)|7u); /* CLKREQ mux1, pullup */
  gw_write(b,GW_GPIO,0x3000,(cfg[3]&~0x3ffu)|3u); /* WAKE input, pullup */
  gw_gpio(b,0,0);
  gw_write(b,GW_GPIO,0,(cfg[0]&~0x3ffu)|0x3c3u); /* 16mA GPIO output */
  gw_gpio(b,0,1); b->delay(b->context,50000);
  r->gpio_after=gw_read(b,GW_GPIO,4);
  if (!(r->gpio_after&2)) { result=GW_BAD_REGISTER; goto fail; }
  for(i=0;i<GW_DOMAINS_COUNT;i++) {
    gw_step(b,r,GW_DOMAINS,i);
    if(b->resource(b->context,GW_DOMAIN,i,1)) { result=GW_RESOURCE_ERROR; goto fail; }
    domains|=1u<<i;
  }
  for(i=0;i<GW_PIPE_CLOCK;i++) {
    gw_step(b,r,GW_CLOCKS,i);
    if(b->resource(b->context,GW_CLOCK,i,1)) { result=GW_RESOURCE_ERROR; goto fail; }
    clocks|=1u<<i; r->clocks=clocks;
  }
  r->ref_clock=gw_read(b,GW_REFCLK,0);
  if(r->ref_clock==0xffffffffu || !(r->ref_clock&1)) { result=GW_BAD_REGISTER; goto fail; }
  gw_step(b,r,GW_RESET,0);
  core_reset=gw_read(b,GW_GCC,0x6b000); phy_reset=gw_read(b,GW_GCC,0x6c01c);
  if(core_reset==0xffffffffu || phy_reset==0xffffffffu) { result=GW_BAD_REGISTER; goto fail; }
  reset_changed=1;
  gw_write(b,GW_GCC,0x6b000,core_reset|1); gw_write(b,GW_GCC,0x6c01c,phy_reset|1);
  b->delay(b->context,1000);
  gw_write(b,GW_GCC,0x6c01c,phy_reset&~1u); gw_write(b,GW_GCC,0x6b000,core_reset&~1u);
  b->delay(b->context,1000);
  gw_step(b,r,GW_PHY_INIT,0);
  if(gw_read(b,GW_PARF,0x40)==0xffffffffu) { result=GW_BAD_REGISTER; goto fail; }
  controller=1;
  gw_write(b,GW_PARF,0x1000,4); gw_mask(b,GW_PARF,0x40,1,0);
  gw_write(b,GW_PARF,0x350,0); gw_write(b,GW_PARF,0,0x365e);
  gw_mask(b,GW_PARF,0x174,0,1u<<4);
  gw_write(b,GW_PARF,0x22c,0); gw_write(b,GW_PARF,0x508,0); /* no IRQs yet */
  gw_write(b,GW_PARF,0x358,0x4000000);
  for(i=0;i<count;i++) {
    gw_write(b,GW_PHY,seq[i].offset,seq[i].value);
    if(seq[i].delay_us) b->delay(b->context,seq[i].delay_us);
  }
  b->delay(b->context,1000);
  gw_step(b,r,GW_PHY_READY,GW_PIPE_CLOCK);
  if(b->resource(b->context,GW_CLOCK,GW_PIPE_CLOCK,1)) { result=GW_RESOURCE_ERROR; goto fail; }
  clocks|=1u<<GW_PIPE_CLOCK; r->clocks=clocks;
  for(i=0;i<100;i++) {
    r->phy=gw_read(b,GW_PHY,0x214);
    if(r->phy!=0xffffffffu && !(r->phy&(1u<<6))) break;
    b->delay(b->context,1000);
  }
  if(i==100) { result=GW_PHY_TIMEOUT; goto fail; }
  r->root_device=gw_read(b,GW_DBI,0);
  if((r->root_device&0xffff)!=0x17cb) { result=GW_BAD_REGISTER; goto fail; }
  gw_step(b,r,GW_LINK,0x710);
  if(!gw_configure_link(b,r)) { result=GW_BAD_REGISTER; goto fail; }
  r->detail=0;
  gw_mask(b,GW_PARF,0x20,0,1u<<5); /* leave low-power states disabled */
  gw_mask(b,GW_PARF,0x2c4,1,1u<<8);
  gw_mask(b,GW_DBI,0x714,0,1u<<5);
  gw_mask(b,GW_DBI,0xa0,15,1); /* Gen1 for initial enumeration */
  gw_write(b,GW_DBI,0xb40,20);
  gw_mask(b,GW_DBI,0x18,0xffffff,0x10100);
  b->delay(b->context,100000);
  gw_gpio(b,1,1); b->delay(b->context,100000);
  r->perst=((gw_read(b,GW_GPIO,0x1000)&0xffff)<<16)|(gw_read(b,GW_GPIO,0x1004)&0xffff);
  r->sw_ctrl=((gw_read(b,GW_GPIO,0x5000)&0xffff)<<16)|(gw_read(b,GW_GPIO,0x5004)&0xffff);
  r->wlan_sw_ctrl=((gw_read(b,GW_GPIO,0x6000)&0xffff)<<16)|(gw_read(b,GW_GPIO,0x6004)&0xffff);
  if(!(r->perst&2)) { result=GW_BAD_REGISTER; goto fail; }
  gw_mask(b,GW_PARF,0x1b0,0,1u<<8);
  for(i=0;i<100;i++) {
    r->elbi=gw_read(b,GW_DBI,0xf28); r->link=gw_read(b,GW_DBI,0x80);
    if(r->elbi!=0xffffffffu) {
      unsigned state=(r->elbi>>12)&63;
      if(state<32) r->ltssm_seen_lo|=1u<<state;
      else r->ltssm_seen_hi|=1u<<(state-32);
      if(last_ltssm!=64 && state!=last_ltssm) r->ltssm_changes++;
      last_ltssm=state;
    }
    if(r->elbi!=0xffffffffu && r->link!=0xffffffffu &&
        (r->elbi&(1u<<10)) && (r->link&(1u<<29))) break;
    b->delay(b->context,5000);
  }
  gw_link_snapshot(b,r);
  gw_measure_pipe(b,r);
  if(r->pipe_measure_status==GW_MEASURE_RESTORE_ERROR) {
    r->detail=0x62000; result=GW_BAD_REGISTER; goto fail;
  }
  if(i==100) { r->detail=gw_read(b,GW_PARF,0x1b0); result=GW_LINK_TIMEOUT; goto fail; }
  gw_step(b,r,GW_CONFIG,0);
  atu_changed=1;
  gw_write(b,GW_ATU,4,0); gw_write(b,GW_ATU,0,4);
  gw_write(b,GW_ATU,8,0x60100000); gw_write(b,GW_ATU,12,0);
  gw_write(b,GW_ATU,16,0x60100fff); gw_write(b,GW_ATU,20,0x01000000);
  gw_write(b,GW_ATU,24,0); gw_write(b,GW_ATU,4,1u<<31);
  for(i=0;i<100;i++) {
    unsigned value=gw_read(b,GW_ATU,4);
    if(value!=0xffffffffu && (value&(1u<<31))) break;
    b->delay(b->context,100);
  }
  if(i==100) { result=GW_ATU_TIMEOUT; goto fail; }
  gw_step(b,r,GW_IDENTIFY,0);
  r->device=gw_read(b,GW_CFG,0);
  if(r->device!=0x110717cbu) { result=GW_WRONG_DEVICE; goto fail; }
  r->command=gw_read(b,GW_CFG,4); r->bar0=gw_read(b,GW_CFG,0x10); r->bar1=gw_read(b,GW_CFG,0x14);
  if(r->command&4) { result=GW_UNEXPECTED_DMA; goto fail; }
  gw_step(b,r,GW_DONE,0);
  r->error=GW_OK;
  return GW_OK;
fail:
  r->error=(unsigned)result;
  b->step(b->context,r); /* Preserve the failing stage before cleanup. */
  if(pins_changed) { gw_gpio(b,1,0); gw_gpio(b,0,0); }
  if(controller) { gw_mask(b,GW_PARF,0x1b0,1u<<8,0); if(atu_changed) gw_write(b,GW_ATU,4,0); gw_write(b,GW_PHY,0x240,0); }
  if(reset_changed) { gw_write(b,GW_GCC,0x6b000,core_reset); gw_write(b,GW_GCC,0x6c01c,phy_reset); }
  for(j=GW_CLOCKS_COUNT;j>0;j--) if(clocks&(1u<<(j-1))) (void)b->resource(b->context,GW_CLOCK,j-1,0);
  for(j=GW_DOMAINS_COUNT;j>0;j--) if(domains&(1u<<(j-1))) (void)b->resource(b->context,GW_DOMAIN,j-1,0);
  for(j=GW_RAILS;j>0;j--) if(rails&(1u<<(j-1))) (void)b->resource(b->context,GW_RAIL,j-1,0);
  if(pins_changed) for(i=0;i<4;i++) { gw_write(b,GW_GPIO,i*0x1000+4,io[i]); gw_write(b,GW_GPIO,i*0x1000,cfg[i]); }
  return result;
}
#endif
