/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * WCN7850 Copy Engine 0/1/2/3/4, polled HTC/WMI/HTT control transport. Register/descriptor
 * layouts: Linux v6.12 ath12k/{hal.c,hal.h,hal_desc.h,ce.c,hw.c}.
 * This owns a separate 2 MiB slice of the established SMMU arena. CE4 sends
 * bounded HTT control messages; network data uses the separate TCL/REO rings.
 * The caller serializes
 * MMIO/window access and keeps the MHI wake vote asserted.
 */
#ifndef GIULIA_WIFI_CE_H
#define GIULIA_WIFI_CE_H
#include "giulia_wifi_qmi.h"
#define GWC_BYTES 0x200000u
#define GWC_BUFFER 2048u
enum gwc_error { GWC_OK,GWC_ARGUMENT,GWC_INHERITED,GWC_DMA,GWC_REGISTER,GWC_POINTER,GWC_LENGTH,GWC_FULL };
struct gwc_io {
    void *context;uint32_t (*read)(void *,uint32_t);void (*write)(void *,uint32_t,uint32_t);
    int (*ready)(void *,uint64_t,size_t);void (*sync)(void *,size_t,size_t,int);
    int (*receive)(void *,unsigned,const void *,size_t);
    uint8_t *memory;uint64_t dma;
};
struct gwc_ring { uint32_t reg,ptr,desc,remote,buffers,count,entry,head,tail,status_tail,rx_buffer;unsigned pipe,status,active; };
struct gwc_state { struct gwc_ring ring[7];unsigned active,error,quarantine; };
static inline unsigned gwc_reg(unsigned pipe,int rx) { return 0x1b80000u+pipe*0x2000u+(rx?0x1000:0); }
static inline int gwc_fail(struct gwc_state *s,unsigned e) { if(!s->error)s->error=e;return (int)s->error; }
static inline void gwc_reg64(const struct gwc_io *io,uint32_t reg,uint64_t n) {
    io->write(io->context,reg,(uint32_t)n);io->write(io->context,reg+4,(uint32_t)(n>>32));
}
static inline int gwc_init(struct gwc_state *s,const struct gwc_io *io) {
    if(!s || !io || !io->read || !io->write || !io->sync || !io->ready || !io->receive || !io->memory ||
        (io->dma&4095) || io->dma>0x100000000ull-GWC_BYTES || s->active || s->quarantine)return GWC_ARGUMENT;
    if(!io->ready(io->context,io->dma,GWC_BYTES))return gwc_fail(s,GWC_DMA);
    const unsigned pipes[7]={0,1,1,2,2,3,4},counts[7]={16,512,512,64,64,32,256};
    const unsigned entries[7]={4,2,4,2,4,4,4};size_t buffer=0x20000;
    for(unsigned i=0;i<7;i++) {
        struct gwc_ring *r=&s->ring[i];r->pipe=pipes[i];r->count=counts[i];r->entry=entries[i];r->status=i==2 || i==4;
        int rx=r->pipe==1 || r->pipe==2;
        r->reg=gwc_reg(r->pipe,rx)+(r->status?0x58:0);r->ptr=gwc_reg(r->pipe,rx)+(r->status?0x408:0x400);
        r->desc=0x2000+i*0x2000;r->remote=i*64;
        if(!r->status){r->buffers=(uint32_t)buffer;buffer+=r->count*GWC_BUFFER;}
        uint32_t misc=io->read(io->context,r->reg+0x10);
        if(misc==0xffffffffu || (misc&0x40))return gwc_fail(s,GWC_INHERITED);
    }
    if(buffer>GWC_BYTES)return gwc_fail(s,GWC_ARGUMENT);
    for(size_t i=0;i<GWC_BYTES;i++)((volatile uint8_t *)io->memory)[i]=0;
    io->sync(io->context,0,GWC_BYTES,1);
    for(unsigned i=0;i<7;i++) {
        struct gwc_ring *r=&s->ring[i];uint64_t base=io->dma+r->desc,remote=io->dma+r->remote;
        io->write(io->context,r->reg,(uint32_t)base);
        io->write(io->context,r->reg+4,(uint32_t)(base>>32)|(r->count*r->entry<<8));
        io->write(io->context,r->reg+8,r->entry|(r->status?(100+r->pipe)<<8:0));
        if(r->status) {
            gwc_reg64(io,r->reg+0x14,remote);io->write(io->context,r->reg+0x24,0);
            io->write(io->context,r->reg+0x4c,0); /* MSI disabled: polled producer */
        } else {
            gwc_reg64(io,r->reg+0x1c,remote);io->write(io->context,r->reg+0x30,0);
            io->write(io->context,r->reg+0x34,0);io->write(io->context,r->reg+0x4c,0);
        }
        io->write(io->context,r->ptr,0);io->write(io->context,r->ptr+4,0);
        if(r->entry==2){uint32_t v=io->read(io->context,gwc_reg(r->pipe,1)+0xb0);
            if(v==0xffffffffu)return gwc_fail(s,GWC_REGISTER);
            io->write(io->context,gwc_reg(r->pipe,1)+0xb0,(v&0xffff0000u)|GWC_BUFFER);}
        r->active=1;s->active=1;
        io->write(io->context,r->reg+0x10,r->status?0x40:0x42);
        if((io->read(io->context,r->reg+0x10)&0x7f)!=(r->status?0x40u:0x42u) ||
            io->read(io->context,r->reg)!=(uint32_t)base ||
            io->read(io->context,r->reg+4)!=((uint32_t)(base>>32)|(r->count*r->entry<<8)))return gwc_fail(s,GWC_REGISTER);
    }
    /* Leave one descriptor empty so empty/full never have the same pointer. */
    for(unsigned i=1;i<=3;i+=2) {
        struct gwc_ring *r=&s->ring[i];
        for(unsigned slot=0;slot<r->count-1;slot++)gwq_w64(io->memory+r->desc+slot*8,io->dma+r->buffers+slot*GWC_BUFFER);
        r->head=r->count-1;io->sync(io->context,r->desc,r->count*8,1);
        io->write(io->context,r->ptr,r->head*2);
    }
    return 0;
}
/* Called before MHI reset. Failure keeps the entire arena pinned. */
static inline int gwc_stop(struct gwc_state *s,const struct gwc_io *io) {
    for(unsigned i=0;i<7;i++)if(s->ring[i].active) {
        struct gwc_ring *r=&s->ring[i];io->write(io->context,r->reg+0x10,0);
        uint32_t v=io->read(io->context,r->reg+0x10);
        if(v==0xffffffffu || (v&0x40))s->quarantine=1;else r->active=0;
    }
    if(!s->quarantine)s->active=0;return !s->quarantine;
}
static inline int gwc_remote(struct gwc_state *s,const struct gwc_io *io,struct gwc_ring *r,uint32_t *slot) {
    io->sync(io->context,r->remote,4,0);uint32_t v=gwq_u32(io->memory+r->remote);
    if(v>=r->count*r->entry || v%r->entry)return gwc_fail(s,GWC_POINTER);
    *slot=v/r->entry;return 0;
}
static inline int gwc_send(struct gwc_state *s,const struct gwc_io *io,unsigned pipe,const void *v,size_t n) {
    if(!s->active || s->error || (pipe!=0 && pipe!=3 && pipe!=4) || !v || !n ||
       n>(pipe==4?256u:GWC_BUFFER))return GWC_ARGUMENT;
    struct gwc_ring *r=&s->ring[pipe==0?0:pipe==3?5:6];uint32_t tail;
    if(gwc_remote(s,io,r,&tail))return (int)s->error;
    unsigned pending=(r->head-r->tail)&(r->count-1),done=(tail-r->tail)&(r->count-1);
    if(done>pending)return gwc_fail(s,GWC_POINTER);r->tail=tail;
    unsigned next=(r->head+1)&(r->count-1);if(next==tail)return GWC_FULL;
    size_t buffer=r->buffers+r->head*GWC_BUFFER,descriptor=r->desc+r->head*16;
    gwq_copy(io->memory+buffer,v,n);gwq_w32(io->memory+descriptor,(uint32_t)(io->dma+buffer));
    gwq_w32(io->memory+descriptor+4,(uint32_t)((io->dma+buffer)>>32)|((uint32_t)n<<16));
    gwq_w32(io->memory+descriptor+8,r->head);gwq_w32(io->memory+descriptor+12,0);
    io->sync(io->context,buffer,n,1);io->sync(io->context,descriptor,16,1);
    r->head=next;io->write(io->context,r->ptr,r->head*4);return 0;
}
static inline int gwc_poll(struct gwc_state *s,const struct gwc_io *io) {
    if(!s->active || s->error)return s->error?(int)s->error:GWC_ARGUMENT;
    for(unsigned index=1;index<=3;index+=2) {
        struct gwc_ring *r=&s->ring[index],*status=&s->ring[index+1];uint32_t head;
        if(gwc_remote(s,io,status,&head))return (int)s->error;
        for(unsigned budget=0;status->tail!=head && budget<r->count-1;budget++) {
            unsigned slot=r->rx_buffer;size_t descriptor=status->desc+status->tail*16;
            io->sync(io->context,descriptor,16,0);uint32_t flags=gwq_u32(io->memory+descriptor);unsigned n=flags>>16;
            if(!n || n>GWC_BUFFER || (flags&0xe00u))return gwc_fail(s,GWC_LENGTH);
            /* Completion implies this posted buffer was consumed. Never
             * derive a CPU pointer from a firmware descriptor address. */
            size_t buffer=r->buffers+slot*GWC_BUFFER;io->sync(io->context,buffer,n,0);
            if(io->receive(io->context,r->pipe,io->memory+buffer,n))return gwc_fail(s,GWC_LENGTH);
            gwq_w32(io->memory+descriptor,0);io->sync(io->context,descriptor,16,1);
            status->tail=(status->tail+1)&(status->count-1);io->write(io->context,status->ptr+4,status->tail*4);
            /* The recycled buffer moves to the formerly empty ring slot;
             * rx_buffer describes buffer ownership, independently of head. */
            unsigned next=(r->head+1)&(r->count-1);uint32_t consumed;
            if(gwc_remote(s,io,r,&consumed))return (int)s->error;
            if(next==consumed)return gwc_fail(s,GWC_POINTER);
            gwq_w64(io->memory+r->desc+r->head*8,io->dma+buffer);
            io->sync(io->context,buffer,GWC_BUFFER,1);io->sync(io->context,r->desc+r->head*8,8,1);
            r->head=next;io->write(io->context,r->ptr,r->head*2);
            r->rx_buffer=(slot+1)%(r->count-1);
        }
    }
    return 0;
}
static inline void gwc_qmi_tables(uint8_t pipes[9*20],uint8_t services[14*12]) {
    const uint32_t target[9][5]={
        {0,2,32,2048,0},{1,1,32,2048,0},{2,1,32,2048,0},{3,2,32,2048,0},
        {4,2,256,256,8},{5,1,32,2048,0},{6,3,32,16384,0},{7,4,0,0,8},{8,3,32,16384,0}};
    const uint32_t service[7]={0x104,0x102,0x101,0x103,0x100,1,0x300};
    for(unsigned i=0;i<9;i++)for(unsigned j=0;j<5;j++)gwq_w32(pipes+i*20+j*4,target[i][j]);
    for(unsigned i=0;i<7;i++)for(unsigned j=0;j<2;j++) {
        unsigned pipe=i==5?(j?2:0):i==6?(j?1:4):(j?2:3);
        uint8_t *p=services+(i*2+j)*12;gwq_w32(p,service[i]);gwq_w32(p+4,j?1:2);gwq_w32(p+8,pipe);
    }
}
#endif
