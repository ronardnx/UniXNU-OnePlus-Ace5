/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Explicit synthetic fixtures for the real resident receive functions.
 * Never attach these objects to a provider, FIFO, firmware or hardware owner.
 */
#ifndef GIULIA_AUDIO_FIXTURE_H
#define GIULIA_AUDIO_FIXTURE_H
#include "giulia_audio_runtime.h"
#include "giulia_audio_graph.h"

struct gaf_fixture {
    struct gat_transport transport;
    struct gag_graph graph;
    uint64_t now,epoch;
};
static inline uint64_t gaf_now(void *cookie)
{ return *(const uint64_t *)cookie; }
static inline size_t gaf_packet(uint8_t p[GAPT_PACKET_BYTES],unsigned header_words,
    unsigned payload_words,uint32_t token,uint32_t local_port,uint32_t opcode)
{
    size_t bytes=(header_words+payload_words)*4u;
    for(unsigned i=0;i<GAPT_PACKET_BYTES;i++)p[i]=0;
    gag_put(p,(uint32_t)(bytes<<8)|(header_words<<4));p[4]=3;p[5]=2;
    gag_put(p+8,1);gag_put(p+12,local_port);gag_put(p+16,token);gag_put(p+20,opcode);
    return bytes;
}
static inline void gaf_spf(struct gaf_fixture *f)
{
    gat_zero(&f->transport,sizeof(f->transport));
    f->transport.epoch=f->epoch;f->transport.spf_token=0x10203;f->transport.spf_pending=1;
}
static inline void gaf_apm(struct gaf_fixture *f,uint32_t opcode)
{
    gag_zero(&f->graph,sizeof(f->graph));f->now=1000;
    f->graph.valid=1;f->graph.epoch=f->epoch;f->graph.state=GAG_PREPARED;
    f->graph.src_port=0x2010;f->graph.pending_token=0x10203;f->graph.pending_opcode=opcode;
    f->graph.pending_state=opcode==GAG_CLOSE?GAG_CLOSED:GAG_STARTED;
    f->graph.open_possible=1;f->graph.deadline=2000;f->graph.last_time=f->now;
    f->graph.ops.ctx=&f->now;f->graph.ops.now_us=gaf_now;
}
/* Returns the passed-case bitmap. The caller compares against all 20 cases
 * and separately records GH adopted/fallback counts; this alone proves no HW. */
static inline uint32_t gaf_run(struct gaf_fixture *f,uint64_t epoch)
{
    uint8_t p[GAPT_PACKET_BYTES];size_t n;int r;uint32_t passed=0;
    f->epoch=epoch;
    for(unsigned test=0;test<8;test++) {
        gaf_spf(f);
        unsigned hs=test==7?15:6;
        n=gaf_packet(p,hs,test==1||test==2?2:1,0x10203,3,
            test==1||test==2?GAT_GPR_BASIC_RESPONSE:GAT_SPF_RESPONSE);
        if(test==1||test==2) {gag_put(p+hs*4,GAT_SPF_GET);gag_put(p+hs*4+4,test==1?1:0);}
        else gag_put(p+hs*4,test==3?0:1);
        if(test==4)gag_put(p+16,0x10204);
        if(test==5)p[6]=15;
        if(test==6)p[6]=16;
        gat_spf_receive(&f->transport,p,n);
        unsigned good=test==1||test==2?
            (f->transport.spf_pending&&!f->transport.spf_received&&!f->transport.spf_ready&&
             f->transport.spf_error==(test==1?1u:0u)&&f->transport.spf_basic_replies==1):
            test==3?(!f->transport.spf_pending&&f->transport.spf_received&&!f->transport.spf_ready):
            test==4||test==6?(f->transport.spf_pending&&!f->transport.spf_received&&!f->transport.spf_ready):
            (!f->transport.spf_pending&&f->transport.spf_received&&f->transport.spf_ready);
        if(good)passed|=1u<<test;
    }
    for(unsigned test=8;test<15;test++) {
        gaf_apm(f,GAG_START);unsigned hs=test==12?15:6;
        n=gaf_packet(p,hs,2,0x10203,0x2010,GAG_BASIC_RSP);
        gag_put(p+hs*4,test==11?GAG_STOP:GAG_START);
        gag_put(p+hs*4+4,test==9?0xf0000001u:0);
        if(test==10)p[6]=15;
        if(test==13)f->now=f->graph.deadline;
        if(test==14)p[4]=2;
        r=gag_receive(&f->graph,p,n);
        unsigned good=test==9?(r==GAG_EREMOTE&&f->graph.remote_status==0xf0000001u&&f->graph.open_possible):
            test==13?(r==GAG_ETIMEOUT&&f->graph.open_possible):
            test==11||test==14?(r==GAG_WAIT&&f->graph.pending_token==0x10203):
            (r==GAG_OK&&f->graph.state==GAG_STARTED&&!f->graph.pending_token);
        if(good)passed|=1u<<test;
    }
    gaf_spf(f);f->transport.spf_pending=0;
    n=gaf_packet(p,6,1,0x10203,3,GAT_SPF_RESPONSE);gag_put(p+24,1);
    gat_spf_receive(&f->transport,p,n);
    if(!f->transport.spf_received&&!f->transport.spf_ready)passed|=1u<<15;
    gaf_spf(f);gat_spf_receive(&f->transport,p,n-1);
    if(f->transport.spf_pending&&!f->transport.spf_received)passed|=1u<<16;
    gaf_apm(f,GAG_CLOSE);n=gaf_packet(p,6,2,0x10203,0x2010,GAG_BASIC_RSP);
    gag_put(p+24,GAG_CLOSE);gag_put(p+28,3);
    if(gag_receive(&f->graph,p,n)==GAG_EREMOTE&&f->graph.open_possible)passed|=1u<<17;
    gaf_apm(f,GAG_CLOSE);f->now=f->graph.deadline;
    if(gag_receive(&f->graph,p,n)==GAG_ETIMEOUT&&f->graph.open_possible)passed|=1u<<18;
    if(gag_timeout_us(GAG_OPEN)==4000000u&&gag_timeout_us(GAG_PREPARE)==2000000u&&
       gag_timeout_us(GAG_START)==5500000u&&gag_timeout_us(GAG_STOP)==5500000u&&
       gag_timeout_us(GAG_CLOSE)==1000000u)passed|=1u<<19;
    return passed;
}
#endif
