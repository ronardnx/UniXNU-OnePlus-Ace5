/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * TLMM open-drain lease for GPIO206/207. Caller first verifies gh=3, clocks
 * and the idle GENI/DMA state. No regulator, clock or GENI writes occur here.
 */
#ifndef GIULIA_SLIDER_PINS_H
#define GIULIA_SLIDER_PINS_H
#include <stdint.h>
struct gsl_pins_io {
    void *cookie;
    uint32_t (*read)(void *,unsigned,unsigned);
    void (*write)(void *,unsigned,unsigned,uint32_t);
};
struct gsl_pins {
    uint32_t old_cfg[2],old_io[2],current[2];unsigned claimed,fault;
};
static inline int gsl_pins_claim(const struct gsl_pins_io *io,struct gsl_pins *s)
{
    if(!io || !s || !io->read || !io->write || s->claimed)return 1;
    for(unsigned i=0;i<2;i++) {
        s->old_cfg[i]=io->read(io->cookie,i,0);s->old_io[i]=io->read(io->cookie,i,4);
        unsigned mux=(s->old_cfg[i]>>2)&15;
        if(s->old_cfg[i]==0xffffffff || s->old_io[i]==0xffffffff || mux>1 ||
           (s->old_cfg[i]&(1u<<9)))return 1;
    }
    for(unsigned i=0;i<2;i++) {
        /* Journal the latch before changing it, even when config fails. */
        s->current[i]=s->old_cfg[i];s->claimed|=1u<<i;
        /* Never enable an output with a high data latch. Bias disabled;
         * external I2C pullups must make the released bus go high. */
        io->write(io->cookie,i,4,s->old_io[i]&~2u);
        uint32_t latch=io->read(io->cookie,i,4);
        if(latch==0xffffffff || (latch&2)){s->fault=1;return 1;}
        uint32_t next=s->old_cfg[i]&~((15u<<2)|3u|(1u<<9));
        if(next&(1u<<11))next|=1u<<12;
        io->write(io->cookie,i,0,next);
        if(io->read(io->cookie,i,0)!=next){s->fault=1;return 1;}
        s->current[i]=next;
    }
    return 0;
}
static inline void gsl_pins_low(const struct gsl_pins_io *io,struct gsl_pins *s,
    unsigned pin,unsigned low)
{
    if(pin>1 || low>1 || !(s->claimed&(1u<<pin)) || s->fault){s->fault=1;return;}
    if(io->read(io->cookie,pin,0)!=s->current[pin]){s->fault=1;return;}
    uint32_t latch=io->read(io->cookie,pin,4);
    if(latch==0xffffffff || (latch&2)) {
        /* This lease still owns the verified mux: release its output before
         * refusing further transfers. Never enable OE with an unchecked latch. */
        uint32_t next=s->current[pin]&~(1u<<9);
        io->write(io->cookie,pin,0,next);
        if(io->read(io->cookie,pin,0)==next)s->current[pin]=next;
        s->fault=1;return;
    }
    uint32_t next=(s->current[pin]&~(1u<<9))|(low<<9);
    io->write(io->cookie,pin,0,next);
    if(io->read(io->cookie,pin,0)==next)s->current[pin]=next;
    else s->fault=1;
}
static inline int gsl_pins_level(const struct gsl_pins_io *io,struct gsl_pins *s,unsigned pin)
{
    if(pin>1 || s->fault || !(s->claimed&(1u<<pin)) ||
       io->read(io->cookie,pin,0)!=s->current[pin]){s->fault=1;return -1;}
    uint32_t value=io->read(io->cookie,pin,4);
    if(value==0xffffffff){s->fault=1;return -1;}
    return value&1;
}
static inline int gsl_pins_restore(const struct gsl_pins_io *io,struct gsl_pins *s)
{
    unsigned failed=0;
    for(unsigned i=0;i<2;i++)if(s->claimed&(1u<<i)) {
        /* An outside ownership/mux change revokes this lease. Do not undo it. */
        if(io->read(io->cookie,i,0)!=s->current[i]){failed=1;continue;}
        io->write(io->cookie,i,0,s->current[i]&~(1u<<9));
        /* A lost release must never be followed by restoring a high latch. */
        if(io->read(io->cookie,i,0)!=(s->current[i]&~(1u<<9))){failed=1;continue;}
        s->current[i]&=~(1u<<9);
        io->write(io->cookie,i,4,s->old_io[i]);
        uint32_t restored=io->read(io->cookie,i,4);
        if(restored==0xffffffff || ((restored^s->old_io[i])&2u)){failed=1;continue;}
        io->write(io->cookie,i,0,s->old_cfg[i]);
        if(io->read(io->cookie,i,0)!=s->old_cfg[i])failed=1;
        else s->claimed&=~(1u<<i);
    }
    return (int)failed;
}
#endif
