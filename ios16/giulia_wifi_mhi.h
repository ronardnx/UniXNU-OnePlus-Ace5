/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Polling MHI/BHI/BHIE transport for WCN7850. Wire layout and sequencing:
 * Linux v6.12 drivers/bus/mhi/{common.h,host/boot.c,host/pm.c} and ath12k/mhi.c.
 * Zero-initialize gwm_state before its first use; never reuse a quarantined
 * arena. One caller serializes access. The platform must establish an isolated DMA
 * arena and an owned MSI sink before master() may grant bus mastering.
 */
#ifndef GIULIA_WIFI_MHI_H
#define GIULIA_WIFI_MHI_H
#include <stdint.h>
#include <stddef.h>

#define GWM_CHANNELS 128u
#define GWM_CTX_SIZE 44u
#define GWM_CCA 0u
#define GWM_ECA 0x1800u
#define GWM_CRC 0x1880u
#define GWM_COMMAND 0x2000u
#define GWM_CONTROL 0x3000u
#define GWM_DATA 0x4000u
#define GWM_TX_RING 0x5000u
#define GWM_RX_RING 0x5400u
#define GWM_MSI_SINK 0x6000u
#define GWM_VECTOR 0x7000u
#define GWM_RX_DATA 0x10000u
#define GWM_TX_DATA 0x90000u
#define GWM_FIRMWARE 0x200000u
#define GWM_PACKET 8192u
#define GWM_SEGMENT 0x80000u
#define GWM_TIMEOUT_MS 10000u
enum gwm_error { GWM_OK,GWM_ARGUMENT,GWM_DMA_NOT_READY,GWM_IMAGE_REJECTED,
    GWM_REGISTER,GWM_INHERITED,GWM_BHI_ERROR,GWM_BHI_TIMEOUT,GWM_READY_TIMEOUT,
    GWM_STATE_TIMEOUT,GWM_BHIE_ERROR,GWM_BHIE_TIMEOUT,GWM_EVENT_CORRUPT,
    GWM_RING_FULL,GWM_COMMAND_ERROR,GWM_COMMAND_TIMEOUT,GWM_SHUTDOWN_FAILED };
enum gwm_stage { GWM_PROBE=1,GWM_BHI,GWM_READY,GWM_M0,GWM_BHIE,GWM_MISSION,GWM_CHANNEL_START,GWM_RUNNING,GWM_STOPPED };
struct gwm_io {
    void *context;
    uint32_t (*read)(void *,uint32_t);
    void (*write)(void *,uint32_t,uint32_t);
    void (*delay)(void *,unsigned);
    int (*dma_ready)(void *,uint64_t,size_t);
    int (*master)(void *,int);
    int (*verify_image)(void *,const void *,size_t,unsigned);
    void (*sync)(void *,size_t,size_t,int);
    void (*receive)(void *,const void *,size_t);
    unsigned char *memory;
    uint64_t dma_base;
    size_t dma_bytes;
    uint32_t bar_bytes;
};
struct gwm_state {
    uint32_t stage,error,soc,major,bhi,bhie,chdb,erdb,ee,bhi_status,bhie_status;
    uint32_t last_register,last_value,sequence;
    unsigned bus_master,quarantined,running;
    unsigned ev_cons[2],ev_free[2],cmd_wp,cmd_pending,cmd_slot,cmd_channel,cmd_result;
    unsigned tx_wp,tx_rp,rx_wp,rx_rp;
};
/* The uncached DMA mapping has device-memory alignment rules. Even a byte loop
 * can become an unaligned word/vector access without volatile (observed in the
 * BHIE vector length's zero bytes). Keep the wire helpers byte-exact. */
static inline void gwm_copy(void *to,const void *from,size_t n)
{ volatile unsigned char *d=(volatile unsigned char *)to;const volatile unsigned char *s=(const volatile unsigned char *)from;while(n--) *d++=*s++; }
static inline void gwm_zero(void *p,size_t n)
{ volatile unsigned char *d=(volatile unsigned char *)p;while(n--) *d++=0; }
static inline uint32_t gwm_u32(const volatile unsigned char *p)
{ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static inline uint64_t gwm_u64(const volatile unsigned char *p)
{ return (uint64_t)gwm_u32(p)|((uint64_t)gwm_u32(p+4)<<32); }
static inline void gwm_put32(volatile unsigned char *p,uint32_t v)
{ for(unsigned i=0;i<4;i++) p[i]=(unsigned char)(v>>(8*i)); }
static inline void gwm_put64(volatile unsigned char *p,uint64_t v)
{ for(unsigned i=0;i<8;i++) p[i]=(unsigned char)(v>>(8*i)); }
static inline int gwm_reg_valid(const struct gwm_io *b,uint32_t reg,unsigned bytes)
{ return !(reg&3) && bytes<=b->bar_bytes && reg<=b->bar_bytes-bytes; }
static inline uint32_t gwm_read(const struct gwm_io *b,struct gwm_state *s,uint32_t reg)
{
    s->last_register=reg;
    s->last_value=gwm_reg_valid(b,reg,4)?b->read(b->context,reg):0xffffffffu;
    return s->last_value;
}
static inline void gwm_write(const struct gwm_io *b,uint32_t reg,uint32_t v)
{ if(gwm_reg_valid(b,reg,4)) b->write(b->context,reg,v); }
static inline void gwm_write64(const struct gwm_io *b,uint32_t reg,uint64_t v)
{ if(gwm_reg_valid(b,reg,8)) {gwm_write(b,reg+4,(uint32_t)(v>>32));gwm_write(b,reg,(uint32_t)v);} }
static inline int gwm_memory(const struct gwm_io *b,uint64_t address,size_t bytes)
{ return address>=b->dma_base && bytes<=b->dma_bytes && address-b->dma_base<=b->dma_bytes-bytes; }
static inline void gwm_sync(const struct gwm_io *b,size_t offset,size_t bytes,int to_device)
{ if(offset<=b->dma_bytes && bytes<=b->dma_bytes-offset) b->sync(b->context,offset,bytes,to_device); }
static inline void gwm_context(const struct gwm_io *b,unsigned at,unsigned cfg,unsigned type,
                               unsigned event,unsigned ring,unsigned count)
{
    unsigned char *p=b->memory+at;gwm_zero(p,GWM_CTX_SIZE);
    gwm_put32(p,cfg);gwm_put32(p+4,type);gwm_put32(p+8,event);
    gwm_put64(p+12,b->dma_base+ring);gwm_put64(p+20,count*16u);
    gwm_put64(p+28,b->dma_base+ring);gwm_put64(p+36,b->dma_base+ring);
}
static inline int gwm_stop(const struct gwm_io *b,struct gwm_state *s)
{
    s->running=0;
    if(!s->bus_master) {s->stage=GWM_STOPPED;return 1;}
    uint32_t control=gwm_read(b,s,0x38);
    if(control!=0xffffffffu) gwm_write(b,0x38,control|2u);
    /* master(0) must read back BME=0 and drain PCIe pending transactions. */
    if(!b->master(b->context,0)) {s->quarantined=1;return 0;}
    s->bus_master=0;s->stage=GWM_STOPPED;return 1;
}
static inline int gwm_fail(const struct gwm_io *b,struct gwm_state *s,unsigned error)
{
    unsigned stage=s->stage;s->error=error;
    if(!gwm_stop(b,s)) s->quarantined=1;
    s->stage=stage;return (int)error;
}
static inline int gwm_wait(const struct gwm_io *b,struct gwm_state *s,uint32_t reg,
                           uint32_t mask,uint32_t expected,unsigned timeout_ms)
{
    for(unsigned i=0;i<timeout_ms;i++) {
        uint32_t value=gwm_read(b,s,reg);
        if(value==0xffffffffu) return -1;
        if((value&mask)==expected) return 1;
        b->delay(b->context,1000);
    }
    return 0;
}
static inline int gwm_probe(const struct gwm_io *b,struct gwm_state *s)
{
    if(!b || !s || !b->read || !b->write || !b->delay || !b->master ||
       !b->dma_ready || !b->verify_image || !b->sync || !b->memory ||
       b->dma_bytes<0x1400000 || b->dma_bytes>0x4000000 ||
       (b->dma_base&0x3fff) || b->dma_base>0xffffffffffull-b->dma_bytes || b->bar_bytes<0x1000)
        return GWM_ARGUMENT;
    if(s->bus_master || s->quarantined || s->running) return GWM_INHERITED;
    gwm_zero(s,sizeof(*s));s->stage=GWM_PROBE;
    s->soc=gwm_read(b,s,0x224);s->major=(s->soc>>8)&15u;
    s->bhi=gwm_read(b,s,0x28);s->bhie=gwm_read(b,s,0x2c);
    if(s->soc==0xffffffffu || !s->soc || (s->major!=1 && s->major!=2) ||
       s->bhi<0xb0 || s->bhie<0xb0 || !gwm_reg_valid(b,s->bhi,0xc8) ||
       !gwm_reg_valid(b,s->bhie,0x7c)) {s->error=GWM_REGISTER;return GWM_REGISTER;}
    s->ee=gwm_read(b,s,s->bhi+0x28);
    if(s->ee!=0) {s->error=GWM_INHERITED;return GWM_INHERITED;}
    return GWM_OK;
}
static inline int gwm_configure(const struct gwm_io *b,struct gwm_state *s)
{
    uint32_t cfg=gwm_read(b,s,0x10);
    s->chdb=gwm_read(b,s,0x18);s->erdb=gwm_read(b,s,0x20);
    if(cfg==0xffffffffu || s->chdb<0x100 || s->erdb<0x100 || (s->chdb&7) || (s->erdb&7) ||
       !gwm_reg_valid(b,s->chdb,128*8) || !gwm_reg_valid(b,s->erdb,2*8)) return 0;
    gwm_context(b,GWM_CRC,0,0,0,GWM_COMMAND,128);
    gwm_context(b,GWM_ECA,0,1,0,GWM_CONTROL,32);
    gwm_context(b,GWM_ECA+44,0,1,0,GWM_DATA,256);
    gwm_context(b,20*44,0,1,1,GWM_TX_RING,64);
    gwm_context(b,21*44,0,2,1,GWM_RX_RING,64);
    s->ev_free[0]=31;s->ev_free[1]=255;
    gwm_put64(b->memory+GWM_ECA+36,b->dma_base+GWM_CONTROL+31*16);
    gwm_put64(b->memory+GWM_ECA+44+36,b->dma_base+GWM_DATA+255*16);
    gwm_sync(b,0,GWM_FIRMWARE,1);
    gwm_write64(b,0x58,b->dma_base+GWM_CCA);gwm_write64(b,0x60,b->dma_base+GWM_ECA);
    gwm_write64(b,0x68,b->dma_base+GWM_CRC);
    gwm_write64(b,0x80,b->dma_base);gwm_write64(b,0x88,b->dma_base+b->dma_bytes-1);
    gwm_write64(b,0x98,b->dma_base);gwm_write64(b,0xa0,b->dma_base+b->dma_bytes-1);
    gwm_write(b,0x10,(cfg&0xffffu)|(2u<<16));
    gwm_write64(b,s->erdb,b->dma_base+GWM_CONTROL+31*16);
    gwm_write64(b,s->erdb+8,b->dma_base+GWM_DATA+255*16);
    return 1;
}

/* Upload the exact signed image. Authentication stays with the chip's PBL/SBL;
 * verify_image additionally binds the host bytes to the original ROM catalog. */
static inline int gwm_boot(const struct gwm_io *b,struct gwm_state *s,
                           const void *firmware,size_t bytes,uint32_t sequence)
{
    int rc;uint32_t value;unsigned entries;
    rc=gwm_probe(b,s);if(rc) return rc;
    if(!firmware || bytes<52 || bytes>16*1024*1024 ||
       !b->verify_image(b->context,firmware,bytes,s->major)) {
        s->error=GWM_IMAGE_REJECTED;return GWM_IMAGE_REJECTED;
    }
    if(!b->dma_ready(b->context,b->dma_base,b->dma_bytes)) {
        s->error=GWM_DMA_NOT_READY;return GWM_DMA_NOT_READY;
    }
    gwm_zero(b->memory,GWM_FIRMWARE);gwm_copy(b->memory+GWM_FIRMWARE,firmware,bytes);
    entries=(unsigned)((bytes+GWM_SEGMENT-1)/GWM_SEGMENT);
    for(unsigned i=0;i<entries;i++) {
        size_t offset=(size_t)i*GWM_SEGMENT,length=bytes-offset;
        if(length>GWM_SEGMENT) length=GWM_SEGMENT;
        gwm_put64(b->memory+GWM_VECTOR+i*16,b->dma_base+GWM_FIRMWARE+offset);
        gwm_put64(b->memory+GWM_VECTOR+i*16+8,length);
    }
    gwm_sync(b,0,GWM_FIRMWARE+bytes,1);
    /* Treat a failed enable as potentially partial; always attempt quiescence. */
    s->bus_master=1;
    if(!b->master(b->context,1)) return gwm_fail(b,s,GWM_DMA_NOT_READY);
    s->sequence=(sequence&0x3fffffffu);if(!s->sequence) s->sequence=1;
    s->stage=GWM_BHI;
    gwm_write(b,s->bhi+0x20,0);gwm_write(b,s->bhi+0x2c,0);
    gwm_write64(b,s->bhi+8,b->dma_base+GWM_FIRMWARE);
    gwm_write(b,s->bhi+0x10,(uint32_t)(bytes<GWM_SEGMENT?bytes:GWM_SEGMENT));
    gwm_write(b,s->bhi+0x18,s->sequence);
    for(unsigned i=0;;i++) {
        s->bhi_status=gwm_read(b,s,s->bhi+0x2c);
        if(s->bhi_status==0xffffffffu || (s->bhi_status>>30)==3) return gwm_fail(b,s,GWM_BHI_ERROR);
        if((s->bhi_status>>30)==2) break;
        if(i==GWM_TIMEOUT_MS) return gwm_fail(b,s,GWM_BHI_TIMEOUT);
        b->delay(b->context,1000);
    }
    s->stage=GWM_READY;
    if(gwm_wait(b,s,0x38,2,0,GWM_TIMEOUT_MS)!=1 || gwm_wait(b,s,0x48,1,1,GWM_TIMEOUT_MS)!=1)
        return gwm_fail(b,s,GWM_READY_TIMEOUT);
    if(!gwm_configure(b,s)) return gwm_fail(b,s,GWM_REGISTER);
    value=gwm_read(b,s,0x38);if(value==0xffffffffu) return gwm_fail(b,s,GWM_REGISTER);
    s->stage=GWM_M0;gwm_write(b,0x38,(value&~0xff00u)|0x200u);
    if(gwm_wait(b,s,0x48,0xff04,0x200,GWM_TIMEOUT_MS)!=1) return gwm_fail(b,s,GWM_STATE_TIMEOUT);
    gwm_write64(b,s->chdb+127*8,1);
    s->stage=GWM_BHIE;
    if(gwm_wait(b,s,s->bhi+0x28,0xffffffffu,1,GWM_TIMEOUT_MS)!=1)
        return gwm_fail(b,s,GWM_STATE_TIMEOUT);
    gwm_write64(b,s->bhie+0x2c,b->dma_base+GWM_VECTOR);
    gwm_write(b,s->bhie+0x34,entries*16);
    value=gwm_read(b,s,s->bhie+0x3c);
    if(value==0xffffffffu) return gwm_fail(b,s,GWM_REGISTER);
    s->sequence=(s->sequence+1)&0x3fffffffu;if(!s->sequence)s->sequence=1;
    gwm_write(b,s->bhie+0x3c,(value&0xc0000000u)|s->sequence);
    for(unsigned i=0;;i++) {
        s->bhie_status=gwm_read(b,s,s->bhie+0x44);
        if(s->bhie_status==0xffffffffu || (s->bhie_status>>30)==3) return gwm_fail(b,s,GWM_BHIE_ERROR);
        if((s->bhie_status>>30)==2) break;
        if(i==GWM_TIMEOUT_MS) return gwm_fail(b,s,GWM_BHIE_TIMEOUT);
        b->delay(b->context,1000);
    }
    s->stage=GWM_MISSION;
    if(gwm_wait(b,s,s->bhi+0x28,0xffffffffu,2,GWM_TIMEOUT_MS)!=1) return gwm_fail(b,s,GWM_STATE_TIMEOUT);
    s->ee=2;return GWM_OK;
}
static inline unsigned gwm_ring_index(uint64_t pointer,uint64_t base,unsigned count)
{
    if(pointer<base || pointer-base>=count*16u || ((pointer-base)&15u)) return count;
    return (unsigned)((pointer-base)/16);
}
static inline int gwm_poll(const struct gwm_io *b,struct gwm_state *s,unsigned budget)
{
    if(!s->bus_master || !budget) return GWM_ARGUMENT;
    uint32_t status=gwm_read(b,s,0x48);
    if(status==0xffffffffu || (status&4)) return gwm_fail(b,s,GWM_REGISTER);
    for(unsigned ring=0;ring<2;ring++) {
        unsigned count=ring?256:32,at=ring?GWM_DATA:GWM_CONTROL;
        unsigned ctx=GWM_ECA+ring*44;
        gwm_sync(b,ctx,44,0);
        uint64_t producer=gwm_u64(b->memory+ctx+28),base=b->dma_base+at;
        unsigned next=gwm_ring_index(producer,base,count);
        if(next==count) return gwm_fail(b,s,GWM_EVENT_CORRUPT);
        while(s->ev_cons[ring]!=next && budget) {
            unsigned pos=s->ev_cons[ring],offset=at+pos*16;
            gwm_sync(b,offset,16,0);
            const unsigned char *event=b->memory+offset;
            uint64_t pointer=gwm_u64(event);
            uint32_t d0=gwm_u32(event+8),d1=gwm_u32(event+12);
            unsigned type=(d1>>16)&255,channel=d1>>24,code=d0>>24,length=d0&65535;
            if(type==0x21) {
                if(ring || !s->cmd_pending || pointer!=b->dma_base+GWM_COMMAND+s->cmd_slot*16)
                    return gwm_fail(b,s,GWM_EVENT_CORRUPT);
                s->cmd_result=code;s->cmd_pending=0;
            } else if(type==0x22) {
                if(!ring || (code!=2 && code!=4)) return gwm_fail(b,s,GWM_EVENT_CORRUPT);
                if(channel==20) {
                    unsigned index=gwm_ring_index(pointer,b->dma_base+GWM_TX_RING,64);
                    unsigned outstanding=(s->tx_wp-s->tx_rp)&63;
                    if(index==64 || !outstanding || ((index-s->tx_rp)&63)>=outstanding)
                        return gwm_fail(b,s,GWM_EVENT_CORRUPT);
                    s->tx_rp=(index+1)&63;
                } else if(channel==21) {
                    unsigned index=gwm_ring_index(pointer,b->dma_base+GWM_RX_RING,64);
                    if(index!=s->rx_rp || !length || length>GWM_PACKET)
                        return gwm_fail(b,s,GWM_EVENT_CORRUPT);
                    uint64_t address=gwm_u64(b->memory+GWM_RX_RING+index*16);
                    uint64_t lower=b->dma_base+GWM_RX_DATA;
                    if(address<lower || address-lower>=64u*GWM_PACKET || ((address-lower)%GWM_PACKET))
                        return gwm_fail(b,s,GWM_EVENT_CORRUPT);
                    unsigned data=(unsigned)(address-b->dma_base);
                    gwm_sync(b,data,length,0);
                    if(b->receive) b->receive(b->context,b->memory+data,length);
                    if(!s->bus_master) return s->error ? (int)s->error : GWM_ARGUMENT;
                    unsigned free_slot=GWM_RX_RING+s->rx_wp*16;
                    gwm_put64(b->memory+free_slot,address);
                    gwm_put32(b->memory+free_slot+8,GWM_PACKET);
                    gwm_put32(b->memory+free_slot+12,0x20200);
                    gwm_sync(b,data,GWM_PACKET,1);gwm_sync(b,free_slot,16,1);
                    s->rx_wp=(s->rx_wp+1)&63;s->rx_rp=(s->rx_rp+1)&63;
                    gwm_put64(b->memory+21*44+36,b->dma_base+GWM_RX_RING+s->rx_wp*16);
                    gwm_sync(b,21*44,44,1);
                    gwm_write64(b,s->chdb+21*8,b->dma_base+GWM_RX_RING+s->rx_wp*16);
                } else return gwm_fail(b,s,GWM_EVENT_CORRUPT);
            } else if(type==0x40) {
                if(ring || code>2) return gwm_fail(b,s,GWM_STATE_TIMEOUT);
                s->ee=code;
            } else if(type==0x20) {
                if(ring || code==0xff || code==0) return gwm_fail(b,s,GWM_STATE_TIMEOUT);
                /* Keep the boot/scan transport awake; no unmanaged M2 transition. */
                if(code==3 || code==4) {
                    gwm_write64(b,s->chdb+127*8,1);
                    uint32_t control=gwm_read(b,s,0x38);
                    if(control==0xffffffffu) return gwm_fail(b,s,GWM_REGISTER);
                    gwm_write(b,0x38,(control&~0xff00u)|0x200u);
                }
            } else return gwm_fail(b,s,GWM_EVENT_CORRUPT);
            s->ev_cons[ring]=(pos+1)%count;s->ev_free[ring]=(s->ev_free[ring]+1)%count;
            gwm_put64(b->memory+ctx+36,base+s->ev_free[ring]*16);
            gwm_sync(b,ctx,44,1);
            gwm_write64(b,s->erdb+ring*8,base+s->ev_free[ring]*16);
            budget--;
        }
    }
    return GWM_OK;
}
static inline int gwm_start_channels(const struct gwm_io *b,struct gwm_state *s)
{
    if(s->stage!=GWM_MISSION || s->ee!=2 || !s->bus_master) return GWM_ARGUMENT;
    s->stage=GWM_CHANNEL_START;
    for(unsigned channel=20;channel<=21;channel++) {
        s->cmd_slot=s->cmd_wp;s->cmd_channel=channel;s->cmd_pending=1;s->cmd_result=0;
        unsigned at=GWM_COMMAND+s->cmd_slot*16;
        gwm_put32(b->memory+channel*44,1);
        gwm_put64(b->memory+at,0);gwm_put32(b->memory+at+8,0);
        gwm_put32(b->memory+at+12,(channel<<24)|(18u<<16));
        s->cmd_wp=(s->cmd_wp+1)%128;
        gwm_put64(b->memory+GWM_CRC+36,b->dma_base+GWM_COMMAND+s->cmd_wp*16);
        gwm_sync(b,channel*44,44,1);gwm_sync(b,at,16,1);gwm_sync(b,GWM_CRC,44,1);
        gwm_write64(b,0x70,b->dma_base+GWM_COMMAND+s->cmd_wp*16);
        for(unsigned i=0;s->cmd_pending;i++) {
            int error=gwm_poll(b,s,64);if(error) return error;
            if(!s->cmd_pending) break;
            if(i==GWM_TIMEOUT_MS) return gwm_fail(b,s,GWM_COMMAND_TIMEOUT);
            b->delay(b->context,1000);
        }
        if(s->cmd_result!=1) return gwm_fail(b,s,GWM_COMMAND_ERROR);
    }
    for(unsigned i=0;i<63;i++) {
        unsigned at=GWM_RX_RING+i*16;
        gwm_put64(b->memory+at,b->dma_base+GWM_RX_DATA+i*GWM_PACKET);
        gwm_put32(b->memory+at+8,GWM_PACKET);gwm_put32(b->memory+at+12,0x20200);
    }
    s->rx_wp=63;
    gwm_put64(b->memory+21*44+36,b->dma_base+GWM_RX_RING+63*16);
    gwm_sync(b,GWM_RX_RING,64*16,1);gwm_sync(b,21*44,44,1);
    gwm_write64(b,s->chdb+21*8,b->dma_base+GWM_RX_RING+63*16);
    s->running=1;s->stage=GWM_RUNNING;return GWM_OK;
}
static inline int gwm_send(const struct gwm_io *b,struct gwm_state *s,const void *data,size_t bytes)
{
    if(!s->running || !s->bus_master || !data || !bytes || bytes>GWM_PACKET) return GWM_ARGUMENT;
    unsigned next=(s->tx_wp+1)&63;
    if(next==s->tx_rp) return GWM_RING_FULL;
    unsigned buffer=GWM_TX_DATA+s->tx_wp*GWM_PACKET,descriptor=GWM_TX_RING+s->tx_wp*16;
    gwm_copy(b->memory+buffer,data,bytes);
    gwm_put64(b->memory+descriptor,b->dma_base+buffer);
    gwm_put32(b->memory+descriptor+8,(uint32_t)bytes);gwm_put32(b->memory+descriptor+12,0x20200);
    s->tx_wp=next;
    gwm_put64(b->memory+20*44+36,b->dma_base+GWM_TX_RING+next*16);
    gwm_sync(b,buffer,bytes,1);gwm_sync(b,descriptor,16,1);gwm_sync(b,20*44,44,1);
    gwm_write64(b,s->chdb+20*8,b->dma_base+GWM_TX_RING+next*16);
    return GWM_OK;
}
#endif
