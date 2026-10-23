/* Synthetic peer exercises the real wire parser; no hardware success inferred. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/giulia_audio_transport.h"

struct fixture {
    uint32_t desc[8];uint8_t tx[16384],rx[16384];
    struct gat_transport t;unsigned kicks,barriers,packets;size_t last_bytes;int kick_error;
};
static void barrier(void *ctx) {((struct fixture *)ctx)->barriers++;}
static void kick(void *ctx)
{struct fixture *f=(struct fixture *)ctx;f->kicks++;if(f->kick_error)f->t.error=f->kick_error;}
static void packet(void *ctx,const uint8_t *b,size_t n)
{struct fixture *f=(struct fixture *)ctx;assert(b&&n);f->packets++;f->last_bytes=n;}
static const struct gat_ops ops={barrier,kick,packet};
static void fresh(struct fixture *f)
{
    memset(f,0,sizeof(*f));
    assert(gat_init(&f->t,f->desc,32,f->tx,sizeof(f->tx),f->rx,sizeof(f->rx),&ops,f,1)==1);
    assert(gat_start(&f->t)==1);assert(gat_le16(f->tx)==0&&gat_le16(f->tx+2)==1&&gat_le32(f->tx+4)==0);
    f->desc[0]=f->desc[1];
}
static void peer(struct fixture *f,unsigned cmd,unsigned cid,uint32_t p2,const uint8_t *data,size_t n)
{
    uint8_t h[8];gat_put16(h,(uint16_t)cmd);gat_put16(h+2,(uint16_t)cid);gat_put32(h+4,p2);
    size_t at=f->desc[3],total=gat_align8(8+n);
    assert(total<sizeof(f->rx));
    for(size_t i=0;i<total;i++)f->rx[(at+i)%sizeof(f->rx)]=i<8?h[i]:i<8+n?data[i-8]:0;
    f->desc[3]=(uint32_t)((at+total)%sizeof(f->rx));
}
static void drain(struct fixture *f)
{int r=gat_poll(&f->t,64);assert(r>=0);f->desc[0]=f->desc[1];}
static void connected(struct fixture *f)
{
    fresh(f);peer(f,0,1,5,0,0);peer(f,1,1,0,0,0);drain(f);
    assert(f->t.version_ack&&f->t.open_sent&&!gat_channel_ready(&f->t));
    peer(f,2,9,10,(const uint8_t *)"adsp_apps",10);peer(f,4,1,0,0,0);drain(f);
    assert(gat_channel_ready(&f->t)&&f->t.local_intent&&f->t.rcid==9);
}
static void remote_intent(struct fixture *f,uint32_t id)
{uint8_t b[8];gat_put32(b,4096);gat_put32(b+4,id);peer(f,5,9,1,b,8);drain(f);}
static void response(uint8_t b[28],uint32_t token,uint32_t status)
{
    memset(b,0,28);gat_put32(b,(28u<<8)|(6u<<4));b[4]=3;b[5]=2;
    gat_put32(b+8,1);gat_put32(b+12,3);gat_put32(b+16,token);
    gat_put32(b+20,GAT_SPF_RESPONSE);gat_put32(b+24,status);
}
static void peer_data(struct fixture *f,unsigned cmd,const uint8_t *b,size_t n,uint32_t left)
{
    uint8_t buf[4104];assert(n<=4096);gat_put32(buf,(uint32_t)n);gat_put32(buf+4,left);
    memcpy(buf+8,b,n);peer(f,cmd,9,f->t.liid,buf,n+8);
}
static void test_protocol(void)
{
    struct fixture f;uint8_t b[28];connected(&f);
    assert(!f.t.spf_ready&&gat_spf_query(&f.t,123)==GAT_WAIT);
    assert(f.t.intent_requested);remote_intent(&f,50);
    size_t txhead=f.desc[1];assert(gat_spf_query(&f.t,123)==GAT_OK);
    assert(gat_le16(f.tx+txhead)==9&&gat_le32(f.tx+txhead+4)==50);
    assert(gat_le32(f.tx+txhead+16)==0x1860&&f.tx[txhead+20]==2&&f.tx[txhead+21]==3);
    assert(gat_le32(f.tx+txhead+24)==3&&gat_le32(f.tx+txhead+28)==1);
    assert(gat_le32(f.tx+txhead+32)==123&&gat_le32(f.tx+txhead+36)==GAT_SPF_GET);
    assert(!f.t.spf_ready&&f.t.spf_pending);
    response(b,122,1);peer_data(&f,9,b,28,0);drain(&f);assert(!f.t.spf_ready&&f.t.spf_pending);
    response(b,123,1);b[5]=3;peer_data(&f,9,b,28,0);drain(&f);assert(!f.t.spf_ready);
    response(b,123,1);peer_data(&f,9,b,12,16);drain(&f);assert(!f.t.spf_ready);
    peer_data(&f,12,b+12,16,0);drain(&f);assert(f.t.spf_ready&&!f.t.spf_pending&&f.t.spf_received);
    assert(f.packets==3&&f.last_bytes==28);
    peer(&f,6,9,50,0,0);drain(&f);assert(!f.t.remote[0].used);
    remote_intent(&f,51);assert(gat_spf_query(&f.t,124)==1&&!f.t.spf_ready);
    response(b,124,0);peer_data(&f,9,b,28,0);drain(&f);
    assert(!f.t.spf_ready&&f.t.spf_received&&f.t.spf_status==0);
    peer(&f,3,9,0,0,0);assert(gat_poll(&f.t,10)==GAT_CLOSED&&!f.t.spf_ready);
    connected(&f);peer(&f,0,1,0,0,0);assert(gat_poll(&f.t,10)==GAT_CLOSED);
}
static void test_wire_bounds(void)
{
    struct fixture f;uint8_t b[28];connected(&f);
    /* Backpressure cannot consume a complete remote packet or mark it ready. */
    response(b,5,1);peer_data(&f,9,b,28,0);uint32_t oldtail=f.desc[2];
    f.desc[0]=(f.desc[1]+16)%sizeof(f.tx);
    assert(gat_poll(&f.t,5)==GAT_WAIT&&f.desc[2]==oldtail&&!f.packets);
    f.desc[0]=f.desc[1];drain(&f);assert(f.packets==1);
    /* Complete header with incomplete body stays queued. */
    connected(&f);peer_data(&f,9,b,28,0);f.desc[3]=(f.desc[3]+sizeof(f.rx)-8)%sizeof(f.rx);
    oldtail=f.desc[2];assert(gat_poll(&f.t,5)==GAT_WAIT&&f.desc[2]==oldtail);
    connected(&f);f.desc[3]=3;assert(gat_poll(&f.t,5)==GAT_PROTOCOL);
    connected(&f);f.desc[0]=sizeof(f.tx);assert(gat_poll(&f.t,5)==GAT_PROTOCOL);
    connected(&f);peer(&f,2,8,0xffffffff,0,0);assert(gat_poll(&f.t,5)==GAT_PROTOCOL);
    connected(&f);peer(&f,5,9,0xffffffff,0,0);assert(gat_poll(&f.t,5)==GAT_PROTOCOL);
    connected(&f);uint8_t bad[8];gat_put32(bad,0xfffffff0);gat_put32(bad+4,0);
    peer(&f,9,9,f.t.liid,bad,8);assert(gat_poll(&f.t,5)==GAT_PROTOCOL);
    connected(&f);peer_data(&f,12,b,28,0);assert(gat_poll(&f.t,5)==GAT_PROTOCOL);
    connected(&f);peer_data(&f,9,b,8,20);drain(&f);peer_data(&f,12,b+8,12,0);
    assert(gat_poll(&f.t,5)==GAT_PROTOCOL);
    connected(&f);remote_intent(&f,77);uint8_t dup[8];gat_put32(dup,100);gat_put32(dup+4,77);
    peer(&f,5,9,1,dup,8);assert(gat_poll(&f.t,5)==GAT_PROTOCOL);
    /* Wrap both descriptor-indexed FIFOs. */
    connected(&f);f.desc[2]=f.desc[3]=sizeof(f.rx)-8;f.desc[0]=f.desc[1]=sizeof(f.tx)-8;
    remote_intent(&f,10);assert(gat_spf_query(&f.t,42)==1);
    response(b,42,1);peer_data(&f,9,b,28,0);drain(&f);assert(f.t.spf_ready);
    /* Existing Android edge cannot be reset; DSP indices stay untouched. */
    f.desc[0]=64;f.desc[1]=72;f.desc[2]=80;f.desc[3]=88;
    assert(gat_init(&f.t,f.desc,32,f.tx,sizeof(f.tx),0,0,&ops,&f,0)==GAT_OWNERSHIP);
    assert(f.desc[0]==64&&f.desc[1]==72&&f.desc[2]==80&&f.desc[3]==88);
    assert(gat_init(&f.t,f.desc,32,f.tx,sizeof(f.tx),0,0,&ops,&f,1)==1);
    assert(f.desc[0]==64&&f.desc[1]==0&&f.desc[2]==0&&f.desc[3]==88);
    assert(gat_attach_rx(&f.t,f.rx,sizeof(f.rx))==1&&gat_attach_rx(&f.t,f.rx,sizeof(f.rx))<0);
}
static void test_smem(void)
{
    uint32_t words[16384];uint8_t *mem=(uint8_t *)words;size_t n=sizeof(words);
    struct gat_smem p;volatile void *ptr=0;size_t bytes=0;struct fixture f;memset(&f,0,sizeof(f));
    memset(mem,0,n);uint8_t *toc=mem+n-4096,*e=toc+32,*part=mem+4096;
    gat_put32(toc,0x434f5424);gat_put32(toc+4,1);gat_put32(toc+8,1);
    gat_put32(e,4096);gat_put32(e+4,49152);gat_put16(e+12,0);gat_put16(e+14,2);gat_put32(e+16,32);
    gat_put32(part,0x54525024);gat_put16(part+4,0);gat_put16(part+6,2);
    gat_put32(part+8,49152);gat_put32(part+12,32);gat_put32(part+16,49152);
    assert(gat_smem_partition(mem,n,2,&p)==1&&p.base==part);
    assert(gat_smem_get(&p,478,&ptr,&bytes)==GAT_WAIT);
    assert(gat_smem_alloc_locked(&p,478,32,barrier,&f)==1);
    assert(gat_smem_get(&p,478,&ptr,&bytes)==1&&bytes==32);
    assert(gat_smem_alloc_locked(&p,478,33,barrier,&f)==GAT_INVALID);
    gat_put32(ptr,123);size_t before=gat_le32(part+12);
    assert(gat_smem_alloc_locked(&p,478,32,barrier,&f)==1);
    assert(gat_le32(ptr)==123&&gat_le32(part+12)==before);
    assert(gat_smem_alloc_locked(&p,479,16384,barrier,&f)==1);
    assert(gat_smem_get(&p,479,&ptr,&bytes)==1&&bytes==16384);
    assert(gat_smem_alloc_locked(&p,480,16384,barrier,&f)==1);
    assert(gat_smem_alloc_locked(&p,481,16384,barrier,&f)==GAT_LIMIT);
    /* Cached reverse list uses cacheline-rounded header preceding data. */
    uint8_t *cached=part+49152-32;gat_put16(cached,0xa5a5);gat_put16(cached+2,900);
    gat_put32(cached+4,32);gat_put16(cached+8,3);gat_put32(part+16,49152-64);
    assert(gat_smem_get(&p,900,&ptr,&bytes)==1&&ptr==cached-32&&bytes==29);
    assert(gat_smem_get(&p,901,&ptr,&bytes)==GAT_WAIT);
    gat_put16(cached+8,32);assert(gat_smem_get(&p,900,&ptr,&bytes)==GAT_INVALID);
    gat_put16(cached+8,3);gat_put32(part+36,0xffffffff);
    assert(gat_smem_get(&p,479,&ptr,&bytes)==GAT_INVALID);
    gat_put32(e,0xfffffff0);assert(gat_smem_partition(mem,n,2,&p)==GAT_INVALID);
    gat_put32(toc+8,0xffffffff);assert(gat_smem_partition(mem,n,2,&p)==GAT_INVALID);
}
static void test_smp2p(void)
{
    uint32_t words[85];uint8_t *b=(uint8_t *)words;struct fixture f;uint32_t s=123,flags=456;memset(&f,0,sizeof(f));
    assert(gat_smp2p_init_outbound(b,sizeof(words),barrier,&f,0)==GAT_OWNERSHIP);
    assert(gat_smp2p_init_outbound(b,sizeof(words),barrier,&f,1)==1);
    assert(b[4]==1&&gat_le32(b)==0x504d5324&&gat_le16(b+14)==1);
    assert(gat_smp2p_slave(b,sizeof(words),&s,&flags)==GAT_INVALID);
    gat_put16(b+8,2);gat_put16(b+10,0);memset(b+20,0,16);memcpy(b+20,"slave-kernel",13);
    gat_put32(b+36,1u<<1);gat_put32(b+16,1);
    assert(gat_smp2p_slave(b,sizeof(words),&s,&flags)==1&&s==2&&flags==1);
    gat_put16(b+14,17);assert(gat_smp2p_slave(b,sizeof(words),&s,&flags)==GAT_INVALID);
    gat_put16(b+14,1);b[4]=0;assert(gat_smp2p_slave(b,sizeof(words),&s,&flags)==GAT_WAIT);
}
static void test_global_partition(void)
{
    uint32_t words[4096];uint8_t *m=(uint8_t *)words;
    uint8_t *toc=m+sizeof(words)-4096,*e=toc+32,*g=m+4096,*p=m+8192;
    struct gat_smem part;volatile void *ptr=0;size_t bytes=0;struct fixture f;memset(&f,0,sizeof(f));
    memset(m,0,sizeof(words));gat_put32(toc,0x434f5424);gat_put32(toc+4,1);gat_put32(toc+8,1);
    gat_put32(e,4096);gat_put32(e+4,4096);gat_put16(e+12,0xfffe);gat_put16(e+14,0xfffe);gat_put32(e+16,32);
    gat_put32(g,0x54525024);gat_put16(g+4,0xfffe);gat_put16(g+6,0xfffe);
    gat_put32(g+8,4096);gat_put32(g+12,32);gat_put32(g+16,4096);
    assert(gat_smem_partition(m,sizeof(words),2,&part)==GAT_WAIT);
    assert(gat_smem_resolve_partition(m,sizeof(words),2,&part)==GAT_OK&&part.base==g);
    assert(gat_smem_alloc_locked(&part,443,340,barrier,&f)==GAT_OK);
    assert(gat_smem_get(&part,443,&ptr,&bytes)==GAT_OK&&bytes==340);
    /* A real private partition wins even when the desired item is global. */
    gat_put32(toc+8,2);e+=48;
    gat_put32(e,8192);gat_put32(e+4,4096);gat_put16(e+12,2);gat_put16(e+14,0);gat_put32(e+16,32);
    gat_put32(p,0x54525024);gat_put16(p+4,2);gat_put16(p+6,0);
    gat_put32(p+8,4096);gat_put32(p+12,32);gat_put32(p+16,4096);
    assert(gat_smem_resolve_partition(m,sizeof(words),2,&part)==GAT_OK&&part.base==p);
    assert(gat_smem_get(&part,443,&ptr,&bytes)==GAT_WAIT);
    gat_put32(p,0);assert(gat_smem_resolve_partition(m,sizeof(words),2,&part)==GAT_INVALID);
    /* A zeroed/unused table slot does not hide the valid global partition. */
    gat_put32(e,0);assert(gat_smem_resolve_partition(m,sizeof(words),2,&part)==GAT_OK&&part.base==g);
    assert(gat_smem_partition(m,sizeof(words),GAT_SMEM_GLOBAL_HOST,&part)==GAT_OK);
    assert(gat_smem_partition(m+1,sizeof(words)-1,2,&part)==GAT_INVALID);
    /* Duplicate global entries are ambiguous and must not produce a handle. */
    memcpy(e,toc+32,48);assert(gat_smem_resolve_partition(m,sizeof(words),2,&part)==GAT_INVALID);
}
static void test_fresh_ready(void)
{
    struct gat_smp2p_watch w={0,0,0,0,0};
    assert(gat_smp2p_watch_arm(&w)==GAT_INVALID);
    assert(gat_smp2p_watch_observe(&w,GAT_OK,GAT_SMP2P_READY)==GAT_INVALID);
    assert(gat_smp2p_watch_capture(&w,GAT_OK,GAT_SMP2P_READY)==GAT_OK);
    assert(gat_smp2p_watch_observe(&w,GAT_OK,0)==GAT_INVALID);
    assert(gat_smp2p_watch_arm(&w)==GAT_OK&&gat_smp2p_watch_arm(&w)==GAT_INVALID);
    for(unsigned i=0;i<20;i++)assert(gat_smp2p_watch_observe(&w,GAT_OK,GAT_SMP2P_READY)==GAT_WAIT&&!w.ready);
    assert(gat_smp2p_watch_observe(&w,GAT_OK,0)==GAT_WAIT);
    assert(gat_smp2p_watch_observe(&w,GAT_OK,GAT_SMP2P_READY)==GAT_OK&&w.ready);
    assert(gat_smp2p_watch_observe(&w,GAT_OK,GAT_SMP2P_READY)==GAT_OK);
    assert(gat_smp2p_watch_observe(&w,GAT_OK,0)==GAT_CLOSED&&!w.ready);
    assert(gat_smp2p_watch_observe(&w,GAT_OK,GAT_SMP2P_READY)==GAT_CLOSED);
    assert(gat_smp2p_watch_capture(&w,GAT_WAIT,0)==GAT_OK&&gat_smp2p_watch_arm(&w)==GAT_OK);
    assert(gat_smp2p_watch_observe(&w,GAT_OK,GAT_SMP2P_READY)==GAT_OK);
    assert(gat_smp2p_watch_observe(&w,GAT_OK,GAT_SMP2P_READY|GAT_SMP2P_FATAL)==GAT_CLOSED);
    assert(gat_smp2p_watch_capture(&w,GAT_OK,GAT_SMP2P_READY)==GAT_OK&&gat_smp2p_watch_arm(&w)==GAT_OK);
    assert(gat_smp2p_watch_observe(&w,GAT_WAIT,0)==GAT_WAIT);
    assert(gat_smp2p_watch_observe(&w,GAT_OK,GAT_SMP2P_READY)==GAT_OK);
    assert(gat_smp2p_watch_observe(&w,GAT_WAIT,0)==GAT_CLOSED);
    assert(gat_smp2p_watch_capture(&w,GAT_OK,0)==GAT_OK&&gat_smp2p_watch_arm(&w)==GAT_OK);
    assert(gat_smp2p_watch_observe(&w,GAT_INVALID,0)==GAT_INVALID&&!w.ready);
    assert(gat_smp2p_watch_observe(&w,GAT_OK,GAT_SMP2P_READY)==GAT_INVALID);
}
static void test_output_lengths_and_priority(void)
{
    struct fixture f;uint8_t payload[GAT_PACKET_MAX];
    fresh(&f);peer(&f,1,1,0,0,0);drain(&f);
    peer(&f,2,9,(7u<<16)|10u,(const uint8_t *)"adsp_apps",10);peer(&f,4,1,0,0,0);drain(&f);
    assert(gat_channel_ready(&f.t));remote_intent(&f,20);
    for(size_t i=0;i<sizeof(payload);i++)payload[i]=(uint8_t)i;
    size_t at=f.desc[1];assert(gat_send(&f.t,payload,sizeof(payload))==GAT_OK);
    assert(f.desc[1]==(at+16+sizeof(payload))%sizeof(f.tx));
    assert(gat_le32(f.tx+at+8)==sizeof(payload)&&gat_le32(f.tx+at+12)==0);
    for(size_t i=0;i<sizeof(payload);i++)assert(f.tx[(at+16+i)%sizeof(f.tx)]==payload[i]);
    assert(gat_send(&f.t,payload,sizeof(payload)+1)==GAT_INVALID);
    /* Reply only reserves its actual 8 bytes; a 64-byte ring can progress. */
    connected(&f);f.t.tx.bytes=64;f.desc[0]=f.desc[1]=0;
    peer_data(&f,9,payload,1,0);drain(&f);assert(f.packets==1&&f.desc[1]==24);
    /* RX_DONE needs no reply and still releases its intent under TX pressure. */
    connected(&f);remote_intent(&f,30);assert(gat_send(&f.t,payload,1)==GAT_OK);
    f.desc[0]=(f.desc[1]+16)%sizeof(f.tx);peer(&f,6,9,30,0,0);
    assert(gat_poll(&f.t,8)==GAT_WAIT&&!f.t.remote[0].used);
}
static void test_spf_errors(void)
{
    struct fixture f;uint8_t b[32];connected(&f);remote_intent(&f,80);
    assert(gat_spf_query(&f.t,0)==GAT_INVALID&&gat_spf_query(&f.t,81)==GAT_OK);
    assert(gat_spf_query(&f.t,82)==GAT_INVALID);
    response(b,81,1);gat_put32(b,(29u<<8)|(6u<<4));
    peer_data(&f,9,b,28,0);drain(&f);assert(f.t.spf_pending&&!f.t.spf_ready);
    response(b,81,1);gat_put32(b+8,2);
    peer_data(&f,9,b,28,0);drain(&f);assert(f.t.spf_pending&&!f.t.spf_ready);
    response(b,81,1);gat_put32(b,(32u<<8)|(6u<<4));gat_put32(b+20,GAT_GPR_BASIC_RESPONSE);
    gat_put32(b+24,GAT_SPF_GET);gat_put32(b+28,7);
    peer_data(&f,9,b,32,0);drain(&f);
    assert(!f.t.spf_pending&&!f.t.spf_ready&&f.t.spf_received&&f.t.spf_error==7);
}
static void test_bounded_malformed_frames(void)
{
    /* Deterministic hostile-wire sweep under ASan/UBSan: arbitrary command,
     * length, intent and state bytes must never read/write outside the rings. */
    struct fixture f;uint32_t random=0xc001d00du;
    for(unsigned trial=0;trial<2048;trial++) {
        connected(&f);
        for(unsigned i=0;i<128;i++) {
            random=random*1664525u+1013904223u;f.rx[(f.desc[2]+i)%sizeof(f.rx)]=(uint8_t)(random>>24);
        }
        f.desc[3]=(f.desc[2]+128)%sizeof(f.rx);
        (void)gat_poll(&f.t,8);
        assert(f.desc[2]<sizeof(f.rx)&&!(f.desc[2]&7));
        assert(f.desc[1]<sizeof(f.tx)&&!(f.desc[1]&7));
        assert(f.t.packet_bytes<=GAT_PACKET_MAX);
    }
}
static void test_notification_faults(void)
{
    struct fixture f;uint8_t b[28];connected(&f);
    response(b,20,1);peer_data(&f,9,b,28,0);
    uint32_t tail=f.desc[2];f.kick_error=GAT_CLOSED;
    assert(gat_poll(&f.t,16)==GAT_CLOSED&&f.desc[2]==tail&&!f.packets);
    /* A fault from the final tail notification stops before a second frame. */
    connected(&f);peer(&f,15,9,0,0,0);peer(&f,15,9,0,0,0);
    tail=f.desc[2];f.kick_error=GAT_CLOSED;
    assert(gat_poll(&f.t,16)==GAT_CLOSED&&f.desc[2]==(tail+8)%sizeof(f.rx));
    connected(&f);peer(&f,13,0,0,0,0);tail=f.desc[2];unsigned kicks=f.kicks;
    f.kick_error=GAT_CLOSED;
    assert(gat_poll(&f.t,16)==GAT_CLOSED&&f.desc[2]==(tail+8)%sizeof(f.rx)&&f.kicks==kicks+1);
}
int main(void)
{
    test_protocol();test_wire_bounds();test_smem();test_smp2p();
    test_global_partition();test_fresh_ready();test_output_lengths_and_priority();
    test_spf_errors();test_bounded_malformed_frames();test_notification_faults();
    puts("audio transport: SMEM, SMP2P, GLINK and token-correlated SPF tests passed");return 0;
}
