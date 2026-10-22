/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Complete a prepared Ace 5 PCIe0 handoff after ExitBootServices.
 * UEFI owns power/clock setup; XNU owns PERST release, LTSSM and CFG0.
 * No endpoint writes, BAR assignment, DMA or interrupts in this layer.
 */
#ifndef GIULIA_WIFI_LINK_H
#define GIULIA_WIFI_LINK_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
enum {GWL_ROOT,GWL_PARF,GWL_PHY,GWL_GCC,GWL_GPIO,GWL_CFG,GWL_REFCLK,GWL_CRM,GWL_REGIONS};
static const uint64_t gwl_bases[GWL_REGIONS]={0x60000000,0x1c00000,0x1c06000,0x16b000,
    0xf110000,0x60100000,0x1ff1000,0x1d05000};
static const unsigned gwl_sizes[GWL_REGIONS]={0x2000,0x3000,0x2000,0x2000,0x51000,0x1000,0x1000,0x2000};
enum {GWL_OK,GWL_ARGUMENT,GWL_NOT_PREPARED,GWL_WRITE_FAILED,GWL_PHY_NOT_READY,
    GWL_LINK_TIMEOUT,GWL_ATU_TIMEOUT,GWL_WRONG_DEVICE,GWL_UNEXPECTED_DMA};
struct gwl_io {
    void *context;
    uint32_t (*read)(void *,unsigned,unsigned);
    void (*write)(void *,unsigned,unsigned,uint32_t);
    void (*delay)(void *,unsigned);
    void (*log)(void *,const char *,uint64_t,uint32_t,uint32_t,uint32_t);
};
struct gwl_state {unsigned stage,error,polls,changed,atu_changed;uint32_t elbi,link,device;};
static inline uint32_t gwl_u32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
/* Return 1 for enumerated v5, 2 for prepared/pending, 0 for invalid data.
   Prefix selection is explicit; a failed v5 report is never promoted. */
static inline unsigned gwl_handoff(const char *text,int pending)
{
    static const char abc[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t data[168];
    if(!text || strlen(text)!=224)return 0;
    for(unsigned i=0;i<56;i++) {
        uint32_t n=0;
        for(unsigned j=0;j<4;j++){const char *p=strchr(abc,text[4*i+j]);if(!p)return 0;n=(n<<6)|(unsigned)(p-abc);}
        data[3*i]=(uint8_t)(n>>16);data[3*i+1]=(uint8_t)(n>>8);data[3*i+2]=(uint8_t)n;
    }
    if(gwl_u32(data)!=5 || gwl_u32(data+12) || gwl_u32(data+16) || gwl_u32(data+20) ||
       gwl_u32(data+32)!=511 || gwl_u32(data+36)!=4095)return 0;
    if(!pending)return gwl_u32(data+4)==16 && (gwl_u32(data+44)&(1u<<29)) &&
        (gwl_u32(data+48)&1024) && gwl_u32(data+52)==0x110717cb && !(gwl_u32(data+56)&4);
    uint32_t perst=gwl_u32(data+96),phy=gwl_u32(data+40);
    return gwl_u32(data+4)==18 && gwl_u32(data+8)==0x584e554c &&
        phy!=0xffffffffu && !(phy&64) && (gwl_u32(data+76)&65535)==0x17cb &&
        !gwl_u32(data+52) && !gwl_u32(data+56) && (gwl_u32(data+72)&3)==3 &&
        ((perst>>16)&0x23c)==0x200 && !(perst&3) && gwl_u32(data+108)==1 ? 2u:0u;
}
static inline uint32_t gwl_read(const struct gwl_io *b,unsigned region,unsigned off)
{return b->read(b->context,region,off);}
static inline void gwl_write(const struct gwl_io *b,unsigned region,unsigned off,uint32_t value)
{b->write(b->context,region,off,value);(void)gwl_read(b,region,off);}
static inline int gwl_check(const struct gwl_io *b,const char *name,unsigned region,unsigned off,uint32_t mask,uint32_t expected)
{
    uint32_t value=gwl_read(b,region,off);
    b->log(b->context,name,gwl_bases[region]+off,value,mask,expected);
    return value!=0xffffffffu && (value&mask)==expected;
}
static inline void gwl_sample(const struct gwl_io *b,const char *name,unsigned region,unsigned off)
{b->log(b->context,name,gwl_bases[region]+off,gwl_read(b,region,off),0,0);}
static inline void gwl_dump(const struct gwl_io *b,const char *name)
{
    static const unsigned regs[][2]={
        {GWL_GCC,8},{GWL_GCC,0x1004},{GWL_GCC,0x58},{GWL_GCC,0x5c},{GWL_GCC,0x78},
        {GWL_GCC,0x48},{GWL_GCC,0x1014},{GWL_GCC,0x1020},{GWL_GCC,0x1028},
        {GWL_GPIO,0},{GWL_GPIO,4},{GWL_GPIO,0x1004},{GWL_GPIO,0x4e000},{GWL_GPIO,0x4e004},
        {GWL_GPIO,0x4f000},{GWL_GPIO,0x4f004},{GWL_REFCLK,0x100},{GWL_CRM,0xc},
        {GWL_PARF,0x24},{GWL_PARF,0x28},{GWL_PARF,0x40},{GWL_PARF,0x174},
        {GWL_PARF,0x1b0},{GWL_PARF,0x2b0},{GWL_PARF,0x2c4},{GWL_PARF,0x4d0},
        {GWL_PHY,0x1cc},{GWL_PHY,0x1d0},{GWL_PHY,0x1d4},{GWL_PHY,0x1f8},
        {GWL_PHY,0x214},{GWL_PHY,0x218},{GWL_PHY,0x21c},{GWL_PHY,0x240},{GWL_PHY,0x244},
        {GWL_PHY,0xed0},{GWL_PHY,0xedc},{GWL_PHY,0x11e0},{GWL_PHY,0x16d0},{GWL_PHY,0x16dc},{GWL_PHY,0x19e0},
        {GWL_ROOT,0x80},{GWL_ROOT,0x710},{GWL_ROOT,0x80c},{GWL_ROOT,0xf24},{GWL_ROOT,0xf28}};
    for(unsigned i=0;i<sizeof(regs)/sizeof(regs[0]);i++)gwl_sample(b,name,regs[i][0],regs[i][1]);
}
static inline int gwl_train(const struct gwl_io *b,struct gwl_state *s)
{
    if(!b || !s || !b->read || !b->write || !b->delay || !b->log)return GWL_ARGUMENT;
    memset(s,0,sizeof(*s));s->stage=1;
    /* Check power and reset before accessing the clocked PHY/DBI regions. */
    if(!gwl_check(b,"core-power",GWL_GCC,8,1u<<16,1u<<16) ||
       !gwl_check(b,"phy-power",GWL_GCC,0x1004,1u<<16,1u<<16) ||
       !gwl_check(b,"core-reset",GWL_GCC,0,1,0) ||
       !gwl_check(b,"phy-reset",GWL_GCC,0x101c,1,0) ||
       !gwl_check(b,"wlan-output",GWL_GPIO,0,0x23c,0x200) ||
       !gwl_check(b,"wlan-high",GWL_GPIO,4,3,3) ||
       !gwl_check(b,"perst-output",GWL_GPIO,0x4e000,0x23c,0x200) ||
       !gwl_check(b,"perst-held",GWL_GPIO,0x4e004,3,0))return s->error=GWL_NOT_PREPARED;
    gwl_dump(b,"before-training");
    if(!gwl_check(b,"phy-ready",GWL_PHY,0x214,64,0))return s->error=GWL_PHY_NOT_READY;
    if(!gwl_check(b,"root-vendor",GWL_ROOT,0,65535,0x17cb) ||
       !gwl_check(b,"ltssm-stopped",GWL_PARF,0x1b0,256,0) ||
       !gwl_check(b,"refclk",GWL_REFCLK,0x100,1,1) ||
       !gwl_check(b,"initial-lanes",GWL_ROOT,0x80c,0x1f00,0x100))return s->error=GWL_NOT_PREPARED;
    s->stage=2;s->changed=1;
    gwl_write(b,GWL_GPIO,0x4e004,gwl_read(b,GWL_GPIO,0x4e004)|2);
    b->delay(b->context,1000);
    if(!gwl_check(b,"perst-released",GWL_GPIO,0x4e004,3,3)){s->error=GWL_WRITE_FAILED;goto fail;}
    s->stage=3;
    gwl_write(b,GWL_PARF,0x1b0,gwl_read(b,GWL_PARF,0x1b0)|256);
    if(!gwl_check(b,"ltssm-enabled",GWL_PARF,0x1b0,256,256)){s->error=GWL_WRITE_FAILED;goto fail;}
    { unsigned last=0xffffffffu;
      for(unsigned i=0;i<21;i++) {
        b->delay(b->context,5000);
        s->polls=i+1;
        s->elbi=gwl_read(b,GWL_ROOT,0xf28);s->link=gwl_read(b,GWL_ROOT,0x80);
        if(s->elbi!=last || s->polls==21) {
          b->log(b->context,"ltssm-sample",0x60000f28,s->elbi,0,0);
          b->log(b->context,"link-sample",0x60000080,s->link,0,0);last=s->elbi;
        }
        if(s->elbi!=0xffffffffu && s->link!=0xffffffffu && (s->elbi&1024) && (s->link&(1u<<29)))break;
      }
    }
    if(s->elbi==0xffffffffu || s->link==0xffffffffu || !(s->elbi&1024) || !(s->link&(1u<<29))) {
        s->error=GWL_LINK_TIMEOUT;goto fail;
    }
    s->stage=4;
    gwl_write(b,GWL_ROOT,0x18,(gwl_read(b,GWL_ROOT,0x18)&~0xffffffu)|0x10100);
    s->atu_changed=1;
    gwl_write(b,GWL_ROOT,0x1004,0);gwl_write(b,GWL_ROOT,0x1000,4);
    gwl_write(b,GWL_ROOT,0x1008,0x60100000);gwl_write(b,GWL_ROOT,0x100c,0);
    gwl_write(b,GWL_ROOT,0x1010,0x60100fff);gwl_write(b,GWL_ROOT,0x1014,0x01000000);
    gwl_write(b,GWL_ROOT,0x1018,0);gwl_write(b,GWL_ROOT,0x1004,1u<<31);
    { unsigned i;
      for(i=0;i<100;i++){uint32_t v=gwl_read(b,GWL_ROOT,0x1004);if(v!=0xffffffffu && (v&(1u<<31)))break;b->delay(b->context,100);}
      if(i==100){s->error=GWL_ATU_TIMEOUT;goto fail;}
    }
    if(!gwl_check(b,"cfg0-type",GWL_ROOT,0x1000,0xffffffffu,4) ||
       !gwl_check(b,"cfg0-base",GWL_ROOT,0x1008,0xffffffffu,0x60100000) ||
       !gwl_check(b,"cfg0-base-high",GWL_ROOT,0x100c,0xffffffffu,0) ||
       !gwl_check(b,"cfg0-limit",GWL_ROOT,0x1010,0xffffffffu,0x60100fff) ||
       !gwl_check(b,"cfg0-target",GWL_ROOT,0x1014,0xffffffffu,0x01000000) ||
       !gwl_check(b,"cfg0-target-high",GWL_ROOT,0x1018,0xffffffffu,0)) {
        s->error=GWL_WRITE_FAILED;goto fail;
    }
    s->stage=5;s->device=gwl_read(b,GWL_CFG,0);
    b->log(b->context,"endpoint-id",0x60100000,s->device,0xffffffffu,0x110717cb);
    if(s->device!=0x110717cb){s->error=GWL_WRONG_DEVICE;goto fail;}
    if(!gwl_check(b,"endpoint-dma-off",GWL_CFG,4,4,0)){s->error=GWL_UNEXPECTED_DMA;goto fail;}
    s->stage=6;gwl_dump(b,"link-established");return GWL_OK;
fail:
    gwl_dump(b,"failure-before-cleanup");
    gwl_write(b,GWL_GPIO,0x4e004,gwl_read(b,GWL_GPIO,0x4e004)&~2u);
    gwl_write(b,GWL_PARF,0x1b0,gwl_read(b,GWL_PARF,0x1b0)&~256u);
    if(s->atu_changed)gwl_write(b,GWL_ROOT,0x1004,0);
    /* UEFI resource votes remain retained; XNU has no boot-services API
       to release them. No DMA was initiated by this layer. */
    return (int)s->error;
}
#endif
