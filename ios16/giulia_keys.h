/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * SM8650 SPMI v7 observer-only keys. Hardware evidence: sm8650-keys.json.
 * No PMIC writes, IRQ acknowledgements or reset configuration changes.
 */
#ifndef GIULIA_KEYS_H
#define GIULIA_KEYS_H
#include <stdint.h>
enum { GK_CORE, GK_OBSERVER };
enum { GK_OK, GK_VERSION, GK_CHANNEL, GK_CHANGED, GK_IO, GK_TIMEOUT, GK_IDENTITY };
struct gk_io {
    void *context;
    uint32_t (*read)(void *, unsigned, unsigned);
    void (*write)(void *, unsigned, unsigned, uint32_t);
    void (*delay)(void *, unsigned);
};
struct gk_channel { unsigned apid; uint32_t mapping; };
struct gk_hw { struct gk_channel pon, gpio; uint32_t version, status; };
static inline int gk_find(const struct gk_io *io, unsigned count, unsigned ppid,
    struct gk_channel *ch)
{
    unsigned found=0;
    for(unsigned i=0;i<count;i++) {
        uint32_t m=io->read(io->context,GK_CORE,0x2000+4*i);
        if(m==0xffffffffu)return GK_IO;
        if(m && ((m>>8)&0xfff)==ppid) { ch->apid=i;ch->mapping=m;found++; }
    }
    return found==1?GK_OK:GK_CHANNEL;
}
static inline int gk_resolve(const struct gk_io *io, struct gk_hw *hw)
{
    hw->version=io->read(io->context,GK_CORE,0);
    uint32_t features=io->read(io->context,GK_CORE,4);
    unsigned count=features&0x7ff;
    if((hw->version>>28)!=7 || features==0xffffffffu || !count || count>1024)return GK_VERSION;
    int e=gk_find(io,count,0x013,&hw->pon);
    return e?e:gk_find(io,count,0x18d,&hw->gpio);
}
static inline int gk_read(const struct gk_io *io, struct gk_hw *hw,
    const struct gk_channel *ch, unsigned offset, unsigned *value)
{
    if(offset>255 || ch->apid>=1024)return GK_CHANNEL;
    if(io->read(io->context,GK_CORE,0x2000+4*ch->apid)!=ch->mapping)return GK_CHANGED;
    unsigned base=ch->apid*0x20;
    io->write(io->context,GK_OBSERVER,base,0x08000000u|(offset<<4));
    for(unsigned i=0;i<1000;i++) {
        hw->status=io->read(io->context,GK_OBSERVER,base+8);
        if(hw->status==0xffffffffu || (hw->status&14))return GK_IO;
        if(hw->status&1) { *value=io->read(io->context,GK_OBSERVER,base+0x18)&255;return GK_OK; }
        io->delay(io->context,1);
    }
    return GK_TIMEOUT;
}
static inline int gk_identify(const struct gk_io *io, struct gk_hw *hw)
{
    const struct gk_channel *ch[2]={&hw->pon,&hw->gpio};
    const unsigned types[2]={1,0x10}, subtypes[2]={9,0x13};
    for(unsigned i=0;i<2;i++) {
        unsigned type,subtype;int e=gk_read(io,hw,ch[i],4,&type);
        if(e)return e;
        e=gk_read(io,hw,ch[i],5,&subtype);if(e)return e;
        if(type!=types[i] || subtype!=subtypes[i])return GK_IDENTITY;
    }
    return GK_OK;
}
/* Index: power, volume up, volume down. GPIO6's DT label is misleading. */
static inline unsigned gk_decode(unsigned pon,unsigned gpio)
{
    return ((pon>>7)&1) | (((pon>>6)&1)<<1) | ((!(gpio&1))<<2);
}
struct gk_filter { unsigned candidate, stable, delivered; uint64_t since[3]; };
static inline unsigned gk_filter_sample(struct gk_filter *s,unsigned raw,uint64_t ms)
{
    for(unsigned k=0;k<3;k++) {
        unsigned bit=1u<<k;
        if((raw^s->candidate)&bit) { s->candidate^=bit;s->since[k]=ms; }
        if(ms-s->since[k]>=40) s->stable=(s->stable&~bit)|(s->candidate&bit);
    }
    return s->stable^s->delivered;
}
#endif
