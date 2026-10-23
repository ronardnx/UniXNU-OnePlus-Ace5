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
    GGRM_MAX_PAYLOAD=63*232,GGRM_MAX_REPLY=63*232-4,GGRM_POLLS=4096,
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
/* Zero-initialize and allocate outside the kernel stack. The owner serializes
 * all access (busy only rejects callback reentry; it is not a lock), holds the
 * lease across the whole call and keeps state/callback storage alive for the
 * entire epoch, including ambiguous failure. Never reset or reuse this state
 * to recover an ambiguous exchange; establish a fresh owned channel epoch.
 * The complete u16 sequence range is valid on the wire. Refuse to reuse any
 * sequence in one epoch, so a late reply cannot complete a subsequent call. */
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
    s->epoch=epoch;s->next_sequence=0;return GGRM_OK;
}
static inline void ggrm_message_reset(struct ggrm_message *m)
{m->id=m->error=0;m->seq=0;m->active=m->done=m->fragments=m->received=0;m->bytes=0;}
static inline int ggrm_fail(struct ggrm_state *s,unsigned code)
{s->quarantined=1;s->busy=0;s->last_result=code;return (int)code;}
static inline int ggrm_live(const struct ggrm_io *io,const struct ggrm_state *s,uint64_t epoch)
{return s->epoch==epoch && io->valid(io->cookie,epoch)==1 && s->epoch==epoch;}
static inline int ggrm_check(const struct ggrm_io *io,const struct ggrm_state *s,
    uint64_t epoch,uint64_t start,uint64_t *last,unsigned timeout)
{
    if(!ggrm_live(io,s,epoch))return GGRM_LEASE;
    uint64_t now=io->now_us(io->cookie);
    if(now<*last || now-start>=timeout)return GGRM_TIMEOUT;
    *last=now;
    if(!ggrm_live(io,s,epoch))return GGRM_LEASE;
    /* The second validity callback may itself cross the deadline or observe
     * a clock discontinuity. Sample again after it before authorizing I/O or
     * returning a successful reply. */
    now=io->now_us(io->cookie);
    if(s->epoch!=epoch)return GGRM_LEASE;
    if(now<*last || now-start>=timeout)return GGRM_TIMEOUT;
    *last=now;return GGRM_OK;
}
static inline void ggrm_header(uint8_t *p,unsigned type,unsigned fragments,uint16_t seq,uint32_t id)
{p[0]=0x21;p[1]=(uint8_t)((fragments<<2)|type);p[2]=(uint8_t)seq;p[3]=(uint8_t)(seq>>8);ggrm_put32(p+4,id);}
static inline int ggrm_append(struct ggrm_message *m,const uint8_t *payload,size_t bytes)
{
    if(m->bytes>GGRM_MAX_PAYLOAD || bytes>GGRM_MAX_PAYLOAD-m->bytes)return 0;
    for(size_t i=0;i<bytes;i++)m->data[m->bytes+i]=payload[i];
    m->bytes+=bytes;return 1;
}
/* Accept one complete MSGQ frame. The pinned RM has one incoming assembly,
 * with a fixed continuation count, not a fragment ordinal. A new initial
 * message interrupts an unfinished assembly: quarantine instead of silently
 * discarding that reply/notification. Enforce sequence in continuations too
 * (the pinned source checks only id/count). Empty fragments are legal. */
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
        if((s->reply.active && !s->reply.done) ||
           (s->notification.active && !s->notification.done))return 0;
        m=type==2?&s->reply:&s->notification;
        if(m->active || m->done)return 0;
        if(type==2 && (incoming!=seq || id!=s->message_id || bytes<12))return 0;
        m->active=1;m->seq=incoming;m->id=id;m->fragments=fragments;
        if(type==2){m->error=ggrm_u32(p+8);header=12;}
    } else {
        struct ggrm_message *a=&s->reply,*b=&s->notification;
        int matches_a=a->active && !a->done && a->seq==incoming && a->id==id;
        int matches_b=b->active && !b->done && b->seq==incoming && b->id==id;
        if(matches_a==matches_b)return 0;
        m=matches_a?a:b;
        if(fragments!=m->fragments || m->received>=m->fragments)return 0;
        ++m->received;
    }
    if(!ggrm_append(m,p+header,bytes-header))return 0;
    m->done=m->received==m->fragments;
    return 1;
}
/* Time/poll limits bound returned FULL/EMPTY and packet flooding. They cannot
 * preempt a hypercall or a callback that itself never returns. Check time and
 * lease after callbacks too; a reply arriving after the deadline cannot make
 * an exchange successful. Explicit RM errors leave the channel usable. Any
 * ambiguous transport/protocol/lease/timeout failure quarantines this state.
 * No polling or notification dispatch occurs between calls; the channel owner
 * must separately service idle notifications before trusting cached state. */
static inline int ggrm_call(const struct ggrm_io *io,struct ggrm_state *s,
    uint32_t id,const void *payload,size_t bytes,unsigned timeout_us)
{
    if(!io || !s || !id || !s->epoch || !io->valid || !io->send || !io->receive ||
       !io->now_us || !io->delay_us || !io->notification || (!payload && bytes) ||
       bytes>GGRM_MAX_REQUEST || !timeout_us || timeout_us>3000000)return GGRM_ARGUMENT;
    if(s->quarantined)return GGRM_QUARANTINED;
    if(s->busy)return GGRM_BUSY;
    if(s->next_sequence>65535)return GGRM_SEQUENCE;
    uintptr_t from=(uintptr_t)payload,at=(uintptr_t)s;
    if(bytes && (bytes>UINTPTR_MAX-from || sizeof(*s)>UINTPTR_MAX-at ||
       (from<at+sizeof(*s) && at<from+bytes)))return GGRM_ARGUMENT;
    s->busy=1;s->message_id=id;s->sent_frames=s->received_frames=0;
    ggrm_message_reset(&s->reply);ggrm_message_reset(&s->notification);
    struct ggrm_io callbacks=*io;io=&callbacks;
    uint64_t epoch=s->epoch;
    uint16_t seq=(uint16_t)s->next_sequence++;
    if(!ggrm_live(io,s,epoch))return ggrm_fail(s,GGRM_LEASE);
    uint64_t start=io->now_us(io->cookie),last=start;unsigned polls=0;
    unsigned frames=bytes?(unsigned)((bytes+GGRM_CHUNK-1)/GGRM_CHUNK):1;
    size_t offset=0;
    for(unsigned frame=0;frame<frames;frame++) {
        size_t amount=bytes-offset;if(amount>GGRM_CHUNK)amount=GGRM_CHUNK;
        ggrm_header(s->tx,frame?0:1,frames-1,seq,id);
        for(size_t i=0;i<amount;i++)s->tx[8+i]=((const uint8_t *)payload)[offset+i];
        for(;;) {
            int check=ggrm_check(io,s,epoch,start,&last,timeout_us);
            if(check)return ggrm_fail(s,(unsigned)check);
            if(++polls>GGRM_POLLS)return ggrm_fail(s,GGRM_TIMEOUT);
            int status=io->send(io->cookie,s->tx,8+amount,frame+1==frames?1:0);
            if(!ggrm_live(io,s,epoch))return ggrm_fail(s,GGRM_LEASE);
            if(!status)++s->sent_frames;
            check=ggrm_check(io,s,epoch,start,&last,timeout_us);
            if(check)return ggrm_fail(s,(unsigned)check);
            if(!status)break;
            if(status!=1)return ggrm_fail(s,GGRM_TRANSPORT);
            if(io->delay_us(io->cookie,100))return ggrm_fail(s,GGRM_TRANSPORT);
        }
        offset+=amount;
    }
    for(;;) {
        int check=ggrm_check(io,s,epoch,start,&last,timeout_us);
        if(check)return ggrm_fail(s,(unsigned)check);
        if(++polls>GGRM_POLLS)return ggrm_fail(s,GGRM_TIMEOUT);
        size_t amount=0;int status=io->receive(io->cookie,s->rx,sizeof(s->rx),&amount);
        if(!ggrm_live(io,s,epoch))return ggrm_fail(s,GGRM_LEASE);
        if(!status)++s->received_frames;
        check=ggrm_check(io,s,epoch,start,&last,timeout_us);
        if(check)return ggrm_fail(s,(unsigned)check);
        if(status==1) {
            if(amount || io->delay_us(io->cookie,100))return ggrm_fail(s,GGRM_TRANSPORT);
            continue;
        }
        if(status)return ggrm_fail(s,GGRM_TRANSPORT);
        if(!ggrm_receive_frame(s,seq,amount))return ggrm_fail(s,GGRM_PROTOCOL);
        if(s->notification.done) {
            status=io->notification(io->cookie,s->notification.id,s->notification.data,s->notification.bytes);
            check=ggrm_check(io,s,epoch,start,&last,timeout_us);
            if(check)return ggrm_fail(s,(unsigned)check);
            if(status!=1)
                return ggrm_fail(s,GGRM_PROTOCOL);
            ggrm_message_reset(&s->notification);
        }
        if(s->reply.done && !s->notification.active) {
            s->busy=0;s->last_result=s->reply.error?GGRM_REMOTE:GGRM_OK;return (int)s->last_result;
        }
    }
}
#endif
