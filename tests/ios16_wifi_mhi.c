/* An endpoint model reads the actual DMA descriptors and complete image. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ios16/wifi/giulia_wifi_mhi.h"
static struct {
    uint32_t reg[4096/4];
    unsigned ready,master,verified,writes,enable_fail,disable_fail,bhi_fail,bhie_fail;
    unsigned bhi_timeout,bhie_timeout,ready_timeout,m0_timeout,sbl_timeout,amss_timeout;
    unsigned bad_chdb,bar_dead;
    unsigned producer[2],command_fail,command_timeout,received,rx_head,tx_noauto;
    unsigned long long delays;
    unsigned char *arena,*image;size_t image_bytes;
} h;
static struct gwm_io io;
static uint32_t read_reg(void *p,uint32_t reg) {(void)p;assert(reg<4096);return h.bar_dead?0xffffffffu:h.reg[reg/4];}
static uint64_t mmio64(unsigned reg) {return h.reg[reg/4]|((uint64_t)h.reg[reg/4+1]<<32);}
static unsigned char *from_dma(uint64_t addr,size_t n) {
    assert(addr>=io.dma_base && n<=io.dma_bytes && addr-io.dma_base<=io.dma_bytes-n);
    return h.arena+(size_t)(addr-io.dma_base);
}
static void post_event(unsigned ring,unsigned type,unsigned channel,unsigned code,uint64_t pointer,unsigned length) {
    unsigned count=ring?256:32,base=ring?GWM_DATA:GWM_CONTROL;
    unsigned char *e=h.arena+base+h.producer[ring]*16;
    gwm_put64(e,pointer);gwm_put32(e+8,(code<<24)|length);gwm_put32(e+12,(channel<<24)|(type<<16));
    h.producer[ring]=(h.producer[ring]+1)%count;
    gwm_put64(h.arena+GWM_ECA+ring*44+28,io.dma_base+base+h.producer[ring]*16);
}
static void write_reg(void *p,uint32_t reg,uint32_t value) {
    (void)p;assert(reg<4096);h.writes++;h.reg[reg/4]=value;
    if(reg==0x218) {
        assert(h.master && h.verified && h.ready && value && !(value&0xc0000000u));
        size_t n=h.reg[0x210/4];assert(n==GWM_SEGMENT);
        assert(!memcmp(from_dma(mmio64(0x208),n),h.image,n));
        h.reg[0x22c/4]=h.bhi_timeout?0:(h.bhi_fail?0xc0000000u:0x80000000u);
        h.reg[0x48/4]=h.ready_timeout?0:1;
        h.reg[0x228/4]=h.sbl_timeout?0:1;
    }
    if(reg==0x38 && !(value&2) && (value&0xff00)==0x200) {
        assert(h.master && h.reg[0x22c/4]==0x80000000u);
        assert(mmio64(0x58)==io.dma_base && mmio64(0x60)==io.dma_base+GWM_ECA);
        assert(mmio64(0x68)==io.dma_base+GWM_CRC);
        assert(mmio64(0x80)==io.dma_base && mmio64(0x88)==io.dma_base+io.dma_bytes-1);
        unsigned char *event=from_dma(mmio64(0x60),88);
        assert(gwm_u32(event+4)==1 && gwm_u64(event+20)==32*16);
        assert(gwm_u64(event+36)==io.dma_base+GWM_CONTROL+31*16);
        assert(gwm_u64(event+44+36)==io.dma_base+GWM_DATA+255*16);
        h.reg[0x48/4]=h.m0_timeout?1:0x201;
    }
    if(reg==0x43c) {
        assert(h.master && h.reg[0x228/4]==1 && (h.reg[0x48/4]&0xff00)==0x200);
        unsigned n=h.reg[0x434/4];assert(n==3*16);
        unsigned char *vectors=from_dma(mmio64(0x42c),n);
        size_t copied=0;
        for(unsigned i=0;i<n/16;i++) {
            uint64_t addr=gwm_u64(vectors+i*16),bytes=gwm_u64(vectors+i*16+8);
            assert(bytes>0 && bytes<=GWM_SEGMENT && copied+bytes<=h.image_bytes);
            assert(!memcmp(from_dma(addr,(size_t)bytes),h.image+copied,(size_t)bytes));
            copied+=(size_t)bytes;
        }
        assert(copied==h.image_bytes);
        h.reg[0x444/4]=h.bhie_timeout?0:(h.bhie_fail?0xc0000000u:0x80000000u)|value;
        h.reg[0x228/4]=h.amss_timeout?1:2;
    }
    if(reg==0x70 && !h.command_timeout) {
        uint64_t wp=mmio64(0x70),base=io.dma_base+GWM_COMMAND;
        assert(wp>=base && wp<base+128*16 && !((wp-base)&15));
        unsigned slot=((unsigned)((wp-base)/16)+127)%128;
        unsigned char *cmd=h.arena+GWM_COMMAND+slot*16;
        uint32_t command=gwm_u32(cmd+12);unsigned channel=command>>24;
        assert((channel==20 || channel==21) && ((command>>16)&255)==18);
        assert(gwm_u32(h.arena+channel*44)==1);
        post_event(0,0x21,0,h.command_fail?0x11:1,base+slot*16,0);
    }
    if(reg==0x800+20*8 && !h.tx_noauto) {
        uint64_t wp=mmio64(reg),base=io.dma_base+GWM_TX_RING;
        assert(wp>=base && wp<base+64*16 && !((wp-base)&15));
        unsigned slot=((unsigned)((wp-base)/16)+63)%64;
        unsigned char *tre=h.arena+GWM_TX_RING+slot*16;
        unsigned length=gwm_u32(tre+8);assert(length && length<=GWM_PACKET);
        assert(from_dma(gwm_u64(tre),length)>=h.arena+GWM_TX_DATA);
        post_event(1,0x22,20,2,base+slot*16,length);
    }
}
static void delay(void *p,unsigned us) {(void)p;assert(us==1000);h.delays+=us;assert(h.delays<=30000000);}
static int dma_ready(void *p,uint64_t addr,size_t bytes) {(void)p;assert(addr==io.dma_base && bytes==io.dma_bytes);return h.ready;}
static int master(void *p,int on) {(void)p;if(on) {h.master=1;return !h.enable_fail;} if(h.disable_fail)return 0;h.master=0;return 1;}
static int verify(void *p,const void *bytes,size_t n,unsigned major) {(void)p;assert(major==2);return h.verified && n==h.image_bytes && bytes==h.image;}
static void sync_dma(void *p,size_t off,size_t n,int to_device) {(void)p;(void)to_device;assert(off<=io.dma_bytes && n<=io.dma_bytes-off);}
static void receive(void *p,const void *data,size_t n) {
    (void)p;assert(n==4 && gwm_u32(data)==h.received);h.received++;
}
static void inject_rx(void) {
    uint64_t ptr=gwm_u64(h.arena+GWM_RX_RING+h.rx_head*16);
    unsigned char *data=from_dma(ptr,4);gwm_put32(data,h.received);
    post_event(1,0x22,21,2,io.dma_base+GWM_RX_RING+h.rx_head*16,4);
    h.rx_head=(h.rx_head+1)%64;
}
static void init(void) {
    unsigned char *arena=h.arena,*image=h.image;
    memset(&h,0,sizeof(h));h.arena=arena;h.image=image;h.image_bytes=GWM_SEGMENT*2+31;
    h.ready=h.verified=1;
    h.reg[0x224/4]=0x12000201;h.reg[0x28/4]=0x200;h.reg[0x2c/4]=0x400;
    h.reg[0x18/4]=0x800;h.reg[0x20/4]=0xc00;h.reg[0x10/4]=128;
    io=(struct gwm_io){0,read_reg,write_reg,delay,dma_ready,master,verify,sync_dma,receive,
                       h.arena,0x98000000,0x1400000,4096};
}
static int run(struct gwm_state *s) {gwm_zero(s,sizeof(*s));return gwm_boot(&io,s,h.image,h.image_bytes,0x3fffffff);}
int main(void) {
    struct gwm_state s;unsigned cases=0;
    h.arena=malloc(0x1400000);h.image=malloc(GWM_SEGMENT*2+31);assert(h.arena&&h.image);
    for(size_t i=0;i<GWM_SEGMENT*2+31;i++) h.image[i]=(unsigned char)(i*37+11);
    init();assert(run(&s)==GWM_OK && s.stage==GWM_MISSION && s.ee==2 && h.master);
    assert(s.sequence==1);assert(gwm_stop(&io,&s) && !h.master);cases++;
    init();h.ready=0;assert(run(&s)==GWM_DMA_NOT_READY && !h.writes && !h.master);cases++;
    init();h.verified=0;assert(run(&s)==GWM_IMAGE_REJECTED && !h.writes);cases++;
    init();h.reg[0x228/4]=2;assert(run(&s)==GWM_INHERITED && !h.writes);cases++;
    init();h.reg[0x28/4]=0xfffffff0;assert(run(&s)==GWM_REGISTER && !h.writes);cases++;
    init();h.reg[0x2c/4]=0x3f0;assert(run(&s)!=GWM_OK && !h.master);cases++;
    init();h.enable_fail=1;assert(run(&s)==GWM_DMA_NOT_READY && !h.master);cases++;
    init();h.bhi_fail=1;assert(run(&s)==GWM_BHI_ERROR && !h.master);cases++;
    init();h.bhi_timeout=1;assert(run(&s)==GWM_BHI_TIMEOUT && !h.master && h.delays==10000000);cases++;
    init();h.ready_timeout=1;assert(run(&s)==GWM_READY_TIMEOUT && !h.master);cases++;
    init();h.m0_timeout=1;assert(run(&s)==GWM_STATE_TIMEOUT && !h.master);cases++;
    init();h.sbl_timeout=1;assert(run(&s)==GWM_STATE_TIMEOUT && !h.master);cases++;
    init();h.bhie_fail=1;assert(run(&s)==GWM_BHIE_ERROR && !h.master);cases++;
    init();h.bhie_timeout=1;assert(run(&s)==GWM_BHIE_TIMEOUT && !h.master);cases++;
    init();h.amss_timeout=1;assert(run(&s)==GWM_STATE_TIMEOUT && !h.master);cases++;
    init();h.reg[0x18/4]=0xfff;assert(run(&s)==GWM_REGISTER && !h.master);cases++;
    init();h.bar_dead=1;assert(run(&s)==GWM_REGISTER && !h.writes);cases++;
    init();h.bhi_fail=1;h.disable_fail=1;
    assert(run(&s)==GWM_BHI_ERROR && s.quarantined && h.master);cases++;
    init();io.dma_base=0xffffffffffull;assert(run(&s)==GWM_ARGUMENT && !h.writes);cases++;
    init();assert(run(&s)==GWM_OK);unsigned writes=h.writes;
    assert(gwm_boot(&io,&s,h.image,h.image_bytes,1)==GWM_INHERITED && h.writes==writes && s.bus_master);cases++;
    assert(gwm_start_channels(&io,&s)==GWM_OK && s.running && s.rx_wp==63);cases++;
    for(unsigned i=0;i<300;i++) {inject_rx();assert(gwm_poll(&io,&s,32)==GWM_OK);}
    assert(h.received==300 && s.rx_rp==h.rx_head);cases++;
    uint32_t packet=0x12345678;
    for(unsigned i=0;i<140;i++) {assert(gwm_send(&io,&s,&packet,sizeof(packet))==GWM_OK);assert(gwm_poll(&io,&s,32)==GWM_OK);}
    assert(s.tx_wp==s.tx_rp);cases++;
    h.tx_noauto=1;
    for(unsigned i=0;i<63;i++) assert(gwm_send(&io,&s,&packet,4)==GWM_OK);
    assert(gwm_send(&io,&s,&packet,4)==GWM_RING_FULL);cases++;
    post_event(1,0x22,20,2,io.dma_base+GWM_TX_RING+((s.tx_wp+63)&63)*16,4);
    assert(gwm_poll(&io,&s,32)==GWM_OK && s.tx_rp==s.tx_wp);cases++;
    post_event(1,0x22,21,2,io.dma_base+GWM_COMMAND,4);
    assert(gwm_poll(&io,&s,32)==GWM_EVENT_CORRUPT && !h.master);cases++;
    init();assert(run(&s)==GWM_OK);h.command_fail=1;
    assert(gwm_start_channels(&io,&s)==GWM_COMMAND_ERROR && !h.master);cases++;
    init();assert(run(&s)==GWM_OK);h.command_timeout=1;
    assert(gwm_start_channels(&io,&s)==GWM_COMMAND_TIMEOUT && !h.master);cases++;
    init();assert(run(&s)==GWM_OK && gwm_start_channels(&io,&s)==GWM_OK);
    gwm_put64(h.arena+GWM_ECA+44+28,io.dma_base+io.dma_bytes);
    assert(gwm_poll(&io,&s,32)==GWM_EVENT_CORRUPT && !h.master);cases++;
    free(h.arena);free(h.image);
    printf("Wi-Fi MHI boot: %u firmware-byte, vector, state, timeout and DMA-lifetime cases passed\n",cases);
}
