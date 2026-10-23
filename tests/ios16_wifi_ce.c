#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../ios16/giulia_wifi_ce.h"
struct fixture {struct gwc_state s;struct gwc_io io;uint32_t regs[0xa000/4];uint8_t ram[GWC_BYTES];unsigned reads,writes,received,deny,lose;};
static uint32_t readreg(void *v,uint32_t a){struct fixture *f=v;assert(a>=0x1b80000&&a<0x1b8a000&&!(a&3));f->reads++;return f->regs[(a-0x1b80000)/4];}
static void writereg(void *v,uint32_t a,uint32_t n){struct fixture *f=v;assert(a>=0x1b80000&&a<0x1b8a000&&!(a&3));f->writes++;if(a!=f->lose)f->regs[(a-0x1b80000)/4]=n;}
static int ready(void *v,uint64_t pa,size_t n){struct fixture *f=v;assert(pa==0x20000000&&n==GWC_BYTES);return !f->deny;}
static void syncmem(void *v,size_t off,size_t n,int to){(void)v;(void)to;assert(off<=GWC_BYTES&&n<=GWC_BYTES-off);}
static int receive(void *v,unsigned pipe,const void *p,size_t n){struct fixture *f=v;assert(pipe==1||pipe==2);assert(n==100&&gwq_u32(p)==f->received);f->received++;return 0;}
static struct fixture *make(void){struct fixture *f=calloc(1,sizeof(*f));assert(f);f->io=(struct gwc_io){f,readreg,writereg,ready,syncmem,receive,f->ram,0x20000000};return f;}
static void device_rx(struct fixture *f,unsigned index,unsigned sequence){
    struct gwc_ring *r=&f->s.ring[index],*st=&f->s.ring[index+1];
    unsigned consumer=gwq_u32(f->ram+r->remote)/2,producer=gwq_u32(f->ram+st->remote)/4;
    assert(consumer!=r->head);uint64_t pa=gwq_u64(f->ram+r->desc+consumer*8);
    assert(pa>=f->io.dma&&pa-f->io.dma<=GWC_BYTES-2048);
    gwq_w32(f->ram+(pa-f->io.dma),sequence);gwq_w32(f->ram+st->desc+producer*16,100u<<16);
    gwq_w32(f->ram+r->remote,((consumer+1)&(r->count-1))*2);
    gwq_w32(f->ram+st->remote,((producer+1)&(st->count-1))*4);
}
int main(void){
    struct fixture *f=make();assert(!gwc_init(&f->s,&f->io));
    for(unsigned i=0;i<1400;i++){device_rx(f,i&1?1:3,i);assert(!gwc_poll(&f->s,&f->io));}assert(f->received==1400);
    uint8_t msg[100]={0x12};for(unsigned i=0;i<31;i++)assert(!gwc_send(&f->s,&f->io,3,msg,sizeof(msg)));
    assert(gwc_send(&f->s,&f->io,3,msg,sizeof(msg))==GWC_FULL);
    struct gwc_ring *r=&f->s.ring[5];gwq_w32(f->ram+r->remote,r->head*4);
    for(unsigned i=0;i<100;i++){assert(!gwc_send(&f->s,&f->io,3,msg,sizeof(msg)));gwq_w32(f->ram+r->remote,r->head*4);}
    uint8_t htt[257]={0};assert(gwc_send(&f->s,&f->io,4,htt,sizeof(htt))==GWC_ARGUMENT);
    for(unsigned i=0;i<255;i++)assert(!gwc_send(&f->s,&f->io,4,htt,256));
    assert(gwc_send(&f->s,&f->io,4,htt,256)==GWC_FULL);
    r=&f->s.ring[6];gwq_w32(f->ram+r->remote,r->head*4);assert(!gwc_send(&f->s,&f->io,4,htt,256));
    assert(gwc_stop(&f->s,&f->io)&&!f->s.active);free(f);
    f=make();f->deny=1;assert(gwc_init(&f->s,&f->io)==GWC_DMA&&!f->writes);free(f);
    f=make();f->regs[0x10/4]=0x40;assert(gwc_init(&f->s,&f->io)==GWC_INHERITED&&!f->writes);free(f);
    f=make();f->lose=0x1b80010;assert(gwc_init(&f->s,&f->io)==GWC_REGISTER);assert(gwc_stop(&f->s,&f->io));free(f);
    f=make();assert(!gwc_init(&f->s,&f->io));gwq_w32(f->ram+f->s.ring[4].remote,3);assert(gwc_poll(&f->s,&f->io)==GWC_POINTER);free(f);
    f=make();assert(!gwc_init(&f->s,&f->io));device_rx(f,3,0);gwq_w32(f->ram+f->s.ring[4].desc,2049u<<16);
    assert(gwc_poll(&f->s,&f->io)==GWC_LENGTH&&!f->received);free(f);
    uint8_t pipes[180],svc[168];gwc_qmi_tables(pipes,svc);assert(gwq_u32(pipes+4)==2&&gwq_u32(pipes+24)==1);
    assert(gwq_u32(svc+4)==2&&gwq_u32(svc+16)==1&&gwq_u32(svc+11*12+8)==2);
    puts("Copy Engine model: 1,400 descriptor/buffer recycles, TX wrap/full, ownership and corruption checks passed");return 0;
}
