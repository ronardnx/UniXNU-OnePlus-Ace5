/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Bounded, serialized Gunyah RM RPC framing over an owned message queue.
 * Source ABI: hardware/gpu-gunyah-source.json. No channel discovery, queue
 * draining, VM creation, memory assignment or implicit notification ACK.
 */
#ifndef GIULIA_GPU_GUNYAH_RM_H
#define GIULIA_GPU_GUNYAH_RM_H
#include <stdint.h>
#include <stddef.h>
enum { GGRM_FRAME=240,GGRM_CHUNK=232,GGRM_MAX_REQUEST=62*232,
    GGRM_MAX_PAYLOAD=63*232,GGRM_POLLS=4096,
    GGRM_OK=0,GGRM_ARGUMENT=1,GGRM_BUSY=2,GGRM_QUARANTINED=3,GGRM_LEASE=4,
    GGRM_TRANSPORT=5,GGRM_TIMEOUT=6,GGRM_PROTOCOL=7,GGRM_REMOTE=8,GGRM_SEQUENCE=9 };
struct ggrm_io {
    void *cookie;
    int (*valid)(void *,uint64_t); /* actual boot epoch and exclusive RM owner */
    int (*send)(void *,const void *,size_t,unsigned); /* 0 sent; 1 definitely full; <0 uncertain */
    int (*receive)(void *,void *,size_t,size_t *); /* 0 received; 1 empty; <0 uncertain */
    uint64_t (*now_us)(void *);
    int (*delay_us)(void *,unsigned);
    int (*notification)(void *,uint32_t,const void *,size_t); /* 1 handled; no silent drops */
};
struct ggrm_message {
    uint32_t id,error;uint16_t seq;
    unsigned active,done,fragments,received;
    size_t bytes;
    uint8_t data[GGRM_MAX_PAYLOAD];
};
/* Allocate outside the kernel stack. Keep this state and callback storage
 * alive for the entire transport epoch, including ambiguous failure. */
struct ggrm_state {
    uint64_t epoch;
    uint32_t next_sequence,message_id;
    unsigned busy,quarantined,last_result,sent_frames,received_frames;
    struct ggrm_message reply,notification;
    uint8_t tx[GGRM_FRAME],rx[GGRM_FRAME];
};
static inline uint16_t ggrm_u16(const uint8_t *p){return (uint16_t)p[0]|(uint16_t)p[1]<<8;}
static inline uint32_t ggrm_u32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static inline void ggrm_put32(uint8_t *p,uint32_t v)
{for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static inline int ggrm_init(struct ggrm_state *s,uint64_t epoch)
{
    if(!s || !epoch || s->epoch || s->busy || s->quarantined)return GGRM_ARGUMENT;
    s->epoch=epoch;s->next_sequence=1;return GGRM_OK;
}
static inline void ggrm_message_reset(struct ggrm_message *m)
{m->id=m->error=0;m->seq=0;m->active=m->done=m->fragments=m->received=0;m->bytes=0;}
static inline int ggrm_fail(struct ggrm_state *s,unsigned code)
{s->quarantined=1;s->busy=0;s->last_result=code;return (int)code;}
static inline int ggrm_elapsed(const struct ggrm_io *io,uint64_t start,unsigned timeout)
{uint64_t now=io->now_us(io->cookie);return now<start || now-start>=timeout;}
static inline void ggrm_header(uint8_t *p,unsigned type,unsigned fragments,uint16_t seq,uint32_t id)
{p[0]=0x21;p[1]=(uint8_t)((fragments<<2)|type);p[2]=(uint8_t)seq;p[3]=(uint8_t)(seq>>8);ggrm_put32(p+4,id);}
static inline int ggrm_append(struct ggrm_message *m,const uint8_t *payload,size_t bytes)
{
    if(bytes>GGRM_MAX_PAYLOAD-m->bytes)return 0;
    for(size_t i=0;i<bytes;i++)m->data[m->bytes+i]=payload[i];
    m->bytes+=bytes;return 1;
}
/* Accept one complete MSGQ frame. Reply and one notification may interleave,
 * but each continuation must preserve its exact seq/id/fragment count. */
static inline int ggrm_receive_frame(struct ggrm_state *s,uint16_t seq,size_t bytes)
{
    const uint8_t *p=s->rx;
    if(bytes<8 || bytes>GGRM_FRAME || p[0]!=0x21)return 0;
    unsigned type=p[1]&3u,fragments=p[1]>>2;
    uint16_t incoming=ggrm_u16(p+2);uint32_t id=ggrm_u32(p+4);
    if(!id || fragments>62 || type==1)return 0;
    struct ggrm_message *m=NULL;
    size_t header=8;
    if(type==2 || type==3) {
        m=type==2?&s->reply:&s->notification;
        if(m->active || m->done)return 0;
        if(type==2 && (incoming!=seq || id!=s->message_id || bytes<12))return 0;
        m->active=1;m->seq=incoming;m->id=id;m->fragments=fragments;
        if(type==2){m->error=ggrm_u32(p+8);header=12;}
    } else {
        struct ggrm_message *a=&s->reply,*b=&s->notification;
        int matches_a=a->active && !a->done && a->seq==incoming && a->id==id;
        int matches_b=b->active && !b->done && b->seq==incoming && b->id==id;
        if(matches_a==matches_b || bytes==8)return 0;
        m=matches_a?a:b;
        if(fragments!=m->fragments || m->received>=m->fragments)return 0;
        ++m->received;
    }
    if(!ggrm_append(m,p+header,bytes-header))return 0;
    m->done=m->received==m->fragments;
    return 1;
}
/* Time/poll limits bound returned FULL/EMPTY and packet flooding. They cannot
 * preempt a hypercall or a callback that itself never returns. Explicit RM
 * error replies remain distinguishable from an unconfirmed transport result. */
static inline int ggrm_call(const struct ggrm_io *io,struct ggrm_state *s,
    uint32_t id,const void *payload,size_t bytes,unsigned timeout_us)
{
    if(!io || !s || !id || !s->epoch || !io->valid || !io->send || !io->receive ||
       !io->now_us || !io->delay_us || !io->notification || (!payload && bytes) ||
       bytes>GGRM_MAX_REQUEST || !timeout_us || timeout_us>3000000)return GGRM_ARGUMENT;
    if(s->quarantined)return GGRM_QUARANTINED;
    if(s->busy)return GGRM_BUSY;
    if(!s->next_sequence || s->next_sequence>65535)return GGRM_SEQUENCE;
    uintptr_t from=(uintptr_t)payload,at=(uintptr_t)s;
    if(bytes && (bytes>UINTPTR_MAX-from || sizeof(*s)>UINTPTR_MAX-at ||
       (from<at+sizeof(*s) && at<from+bytes)))return GGRM_ARGUMENT;
    s->busy=1;s->message_id=id;s->sent_frames=s->received_frames=0;
    ggrm_message_reset(&s->reply);ggrm_message_reset(&s->notification);
    uint16_t seq=(uint16_t)s->next_sequence++;
    uint64_t start=io->now_us(io->cookie);unsigned polls=0;
    unsigned frames=bytes?(unsigned)((bytes+GGRM_CHUNK-1)/GGRM_CHUNK):1;
    size_t offset=0;
    for(unsigned frame=0;frame<frames;frame++) {
        size_t amount=bytes-offset;if(amount>GGRM_CHUNK)amount=GGRM_CHUNK;
        ggrm_header(s->tx,frame?0:1,frames-1,seq,id);
        for(size_t i=0;i<amount;i++)s->tx[8+i]=((const uint8_t *)payload)[offset+i];
        for(;;) {
            if(io->valid(io->cookie,s->epoch)!=1)return ggrm_fail(s,GGRM_LEASE);
            if(++polls>GGRM_POLLS || ggrm_elapsed(io,start,timeout_us))return ggrm_fail(s,GGRM_TIMEOUT);
            int status=io->send(io->cookie,s->tx,8+amount,frame+1==frames?1:0);
            if(io->valid(io->cookie,s->epoch)!=1)return ggrm_fail(s,GGRM_LEASE);
            if(!status){++s->sent_frames;break;}
            if(status!=1)return ggrm_fail(s,GGRM_TRANSPORT);
            if(io->delay_us(io->cookie,100))return ggrm_fail(s,GGRM_TRANSPORT);
        }
        offset+=amount;
    }
    for(;;) {
        if(io->valid(io->cookie,s->epoch)!=1)return ggrm_fail(s,GGRM_LEASE);
        if(++polls>GGRM_POLLS || ggrm_elapsed(io,start,timeout_us))return ggrm_fail(s,GGRM_TIMEOUT);
        size_t amount=0;int status=io->receive(io->cookie,s->rx,sizeof(s->rx),&amount);
        if(io->valid(io->cookie,s->epoch)!=1)return ggrm_fail(s,GGRM_LEASE);
        if(status==1) {
            if(amount || io->delay_us(io->cookie,100))return ggrm_fail(s,GGRM_TRANSPORT);
            continue;
        }
        if(status)return ggrm_fail(s,GGRM_TRANSPORT);
        ++s->received_frames;
        if(!ggrm_receive_frame(s,seq,amount))return ggrm_fail(s,GGRM_PROTOCOL);
        if(s->notification.done) {
            if(io->notification(io->cookie,s->notification.id,s->notification.data,s->notification.bytes)!=1)
                return ggrm_fail(s,GGRM_PROTOCOL);
            ggrm_message_reset(&s->notification);
        }
        if(s->reply.done && !s->notification.active) {
            if(io->valid(io->cookie,s->epoch)!=1)return ggrm_fail(s,GGRM_LEASE);
            s->busy=0;s->last_result=s->reply.error?GGRM_REMOTE:GGRM_OK;return (int)s->last_result;
        }
    }
}
#endif
