/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Ace 5 DSI0 inherited command FIFO. Proven transaction:
 * scripts/panel-display-terminal.py, DCS 28/29, controller 0xae94000.
 * The caller serializes the bus and owns the inherited splash display.
 */
#ifndef GIULIA_DISPLAY_POWER_H
#define GIULIA_DISPLAY_POWER_H
#include <stdint.h>
enum { GDP_OK, GDP_ARGUMENT, GDP_UNSUPPORTED, GDP_BUSY, GDP_IO,
       GDP_TIMEOUT, GDP_ACK, GDP_RESTORE, GDP_FROZEN };
struct gdp_io {
    void *cookie;
    uint32_t (*read)(void *,unsigned);
    void (*write)(void *,unsigned,uint32_t);
    void (*delay_us)(void *,unsigned);
};
struct gdp_state {
    unsigned known,on,requested,error,frozen,requests,transitions;
    uint32_t status,interrupts,ack;
};
/* DSI0 v2.5: only DMA_DONE is acknowledged here. Other W1C status fields
 * must be written zero while their interrupt-enable fields are retained. */
#define GDP_IRQ_STATUS_MASK 0x55115501u
static inline void gdp_ack_done(const struct gdp_io *io,uint32_t irq)
{io->write(io->cookie,0x110,(irq&~GDP_IRQ_STATUS_MASK)|1u);}
static inline int gdp_restore(const struct gdp_io *io,const uint32_t saved[3])
{
    io->write(io->cookie,0x15c,0);
    io->write(io->cookie,0x1ec,1);io->delay_us(io->cookie,1);io->write(io->cookie,0x1ec,0);
    const unsigned offsets[3]={0x3c,0x4c,0x15c};
    for(unsigned i=0;i<3;i++)io->write(io->cookie,offsets[i],saved[i]);
    for(unsigned i=0;i<3;i++)if(io->read(io->cookie,offsets[i])!=saved[i])return GDP_RESTORE;
    return GDP_OK;
}
static inline int gdp_power(const struct gdp_io *io,struct gdp_state *s,unsigned on)
{
    if(!io || !s || !io->read || !io->write || !io->delay_us || on>1)return GDP_ARGUMENT;
    s->requests++;s->requested=on;
    if(s->frozen)return (int)(s->error=GDP_FROZEN);
    if(s->known && s->on==on)return (int)(s->error=GDP_OK);
    if(io->read(io->cookie,4)!=0x1f5)return (int)(s->error=GDP_UNSUPPORTED);
    s->status=io->read(io->cookie,8);
    uint32_t fifo=io->read(io->cookie,0x15c),trigger=io->read(io->cookie,0x84);
    if(s->status==0xffffffff || fifo==0xffffffff || trigger==0xffffffff)return (int)(s->error=GDP_IO);
    if((s->status&2) || fifo)return (int)(s->error=GDP_BUSY);
    if((trigger&15)!=4)return (int)(s->error=GDP_UNSUPPORTED);
    uint32_t saved[3]={io->read(io->cookie,0x3c),io->read(io->cookie,0x4c),fifo};
    if(saved[0]==0xffffffff || saved[1]==0xffffffff)return (int)(s->error=GDP_IO);
    s->interrupts=io->read(io->cookie,0x110);
    if(s->interrupts==0xffffffff)return (int)(s->error=GDP_IO);
    gdp_ack_done(io,s->interrupts);
    s->interrupts=io->read(io->cookie,0x110);
    if(s->interrupts==0xffffffff || (s->interrupts&1))return (int)(s->error=GDP_IO);
    io->write(io->cookie,0x1ec,1);io->delay_us(io->cookie,1);io->write(io->cookie,0x1ec,0);
    io->write(io->cookie,0x15c,0x30006);
    io->write(io->cookie,0x17c,0x80050000u|(on?0x29u:0x28u));
    io->write(io->cookie,0x17c,0);
    io->write(io->cookie,0x3c,(saved[0]&~0xc0000000u)|0x14000000u);
    io->write(io->cookie,0x4c,4);
    if(io->read(io->cookie,0x15c)!=0x30006 || io->read(io->cookie,0x4c)!=4 ||
       io->read(io->cookie,0x3c)!=((saved[0]&~0xc0000000u)|0x14000000u)) {
        int cleanup=gdp_restore(io,saved);if(cleanup)s->frozen=1;
        return (int)(s->error=cleanup?cleanup:GDP_IO);
    }
    s->known=0;io->write(io->cookie,0x90,1);
    unsigned completed=0;
    for(unsigned i=0;i<10000;i++) {
        s->interrupts=io->read(io->cookie,0x110);
        s->status=io->read(io->cookie,8);
        if(s->interrupts==0xffffffff || s->status==0xffffffff)break;
        if((s->interrupts&1) && !(s->status&2)){completed=1;break;}
        io->delay_us(io->cookie,10);
    }
    int error=GDP_OK;
    if(!completed) {
        error=(s->interrupts==0xffffffff || s->status==0xffffffff)?GDP_IO:GDP_TIMEOUT;
        s->frozen=1;
    }
    else {
        s->ack=io->read(io->cookie,0x68);
        if(s->ack==0xffffffff)error=GDP_IO;
        else if(s->ack)error=GDP_ACK;
        else {s->known=1;s->on=on;s->transitions++;}
        if(!error) {
            gdp_ack_done(io,s->interrupts);
            s->interrupts=io->read(io->cookie,0x110);
            if(s->interrupts==0xffffffff || (s->interrupts&1))error=GDP_IO;
        }
        if(error)s->frozen=1;
    }
    /* Unknown activity is a fail-stop condition: do not reset a FIFO or
     * restore borrowed control registers underneath an in-flight command. */
    s->status=io->read(io->cookie,8);
    if(s->status==0xffffffff || (s->status&2))s->frozen=1;
    else if(!s->frozen) {
        int cleanup=gdp_restore(io,saved);
        if(cleanup){s->frozen=1;if(!error)error=cleanup;}
    }
    s->error=(unsigned)error;return error;
}
#endif
