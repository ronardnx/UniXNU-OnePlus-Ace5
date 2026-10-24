/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Bounded AP<->ADSP SMEM/GLINK native transport. Wire layouts checked against
 * OnePlusOSS android_kernel_oneplus_sm8650 d763ed47deec55451366c1187f813f2f309d88a7:
 * drivers/soc/qcom/smem.c, drivers/rpmsg/qcom_glink_{smem,native}.c.
 * GPR/SPF layouts: audio-kernel/{include/ipc/gpr-lite.h,dsp/spf-core.c},
 * modules/devicetree commit 453d8875f815bf9dfad9035982861226a4823a6c.
 * This is an independent implementation of the on-wire protocol. No Linux
 * implementation code, allocation, clocks, PAS, DMA mapping or MMIO is here.
 * Serialize all entry points. Map SMEM noncached/coherent, provide real memory
 * barriers and IPCC notification, and establish exclusive fresh-edge ownership.
 */
#ifndef GIULIA_AUDIO_TRANSPORT_H
#define GIULIA_AUDIO_TRANSPORT_H
#include <stdint.h>
#include <stddef.h>
#include "giulia_audio_proto.h"

#define GAT_PACKET_MAX 4096u
#define GAT_INTENTS_MAX 16u
#define GAT_SMEM_DESC 478u
#define GAT_SMEM_TX 479u
#define GAT_SMEM_RX 480u
#define GAT_SMEM_TX_BYTES 16384u
#define GAT_SMEM_GLOBAL_HOST 0xfffeu
#define GAT_SPF_GET 0x01001021u
#define GAT_SPF_RESPONSE 0x02001007u
#define GAT_GPR_BASIC_RESPONSE 0x02001005u
enum { GAT_WAIT=0, GAT_OK=1, GAT_INVALID=-1, GAT_PROTOCOL=-2,
       GAT_OWNERSHIP=-3, GAT_CLOSED=-4, GAT_LIMIT=-5 };

static inline uint16_t gat_le16(const volatile void *p)
{ const volatile uint8_t *b=(const volatile uint8_t *)p; return (uint16_t)(b[0]|((uint16_t)b[1]<<8)); }
static inline uint32_t gat_le32(const volatile void *p)
{ const volatile uint8_t *b=(const volatile uint8_t *)p; return b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24); }
static inline void gat_put16(volatile void *p,uint16_t x)
{ volatile uint8_t *b=(volatile uint8_t *)p; b[0]=(uint8_t)x;b[1]=(uint8_t)(x>>8); }
static inline void gat_put32(volatile void *p,uint32_t x)
{ volatile uint8_t *b=(volatile uint8_t *)p; for(unsigned i=0;i<4;i++)b[i]=(uint8_t)(x>>(8*i)); }
static inline void gat_zero(void *p,size_t n)
{ uint8_t *b=(uint8_t *)p;for(size_t i=0;i<n;i++)b[i]=0; }
static inline size_t gat_align8(size_t n) { return (n+7u)&~(size_t)7u; }

struct gat_smem { volatile uint8_t *base; size_t bytes,cacheline; };
/* Partition table lives in the last 4KiB of the reserved SMEM region. Select
 * the AP(0)/requested-host private partition, or explicitly request the global
 * 0xfffe/0xfffe partition. Missing/invalid private partitions are distinct. */
static inline int gat_smem_partition(volatile void *base,size_t bytes,unsigned host,struct gat_smem *out)
{
    volatile uint8_t *b=(volatile uint8_t *)base,*toc;
    uint32_t count;size_t found=0;
    struct gat_smem result={0,0,0};
    if(!b||!out||((uintptr_t)b&3)||bytes<4096||(bytes&3)||!host||host>65535)return GAT_INVALID;
    toc=b+bytes-4096;
    if(gat_le32(toc)!=0x434f5424u||gat_le32(toc+4)!=1)return GAT_INVALID;
    count=gat_le32(toc+8);
    if(count>(4096u-32u)/48u)return GAT_INVALID;
    for(uint32_t i=0;i<count;i++) {
        volatile uint8_t *e=toc+32+48*i;
        unsigned h0=gat_le16(e+12),h1=gat_le16(e+14);
        if(host==GAT_SMEM_GLOBAL_HOST) {
            if(h0!=GAT_SMEM_GLOBAL_HOST||h1!=GAT_SMEM_GLOBAL_HOST)continue;
        } else if(!((h0==0&&h1==host)||(h1==0&&h0==host)))continue;
        size_t off=gat_le32(e),n=gat_le32(e+4),line=gat_le32(e+16);
        /* Unused table slots may carry host ids with zero offset/size. */
        if(!off||!n)continue;
        if(++found!=1||off<4096||n<32||n>bytes-4096||off>bytes-4096-n||
           (off&7)||!line||(line&(line-1))||line>n)return GAT_INVALID;
        volatile uint8_t *p=b+off;
        if(gat_le32(p)!=0x54525024u||gat_le16(p+4)!=h0||gat_le16(p+6)!=h1||
           gat_le32(p+8)!=n||gat_le32(p+12)<32||gat_le32(p+12)>gat_le32(p+16)||
           gat_le32(p+16)>n)return GAT_INVALID;
        result.base=p;result.bytes=n;result.cacheline=line;
    }
    if(!found)return GAT_WAIT;
    *out=result;return GAT_OK;
}
/* Matches qcom_smem_get/alloc: use global only when the private partition is
 * absent, never when a private item is absent or private metadata is invalid.
 * Legacy global heaps (SMEM v11) are deliberately unsupported. */
static inline int gat_smem_resolve_partition(volatile void *base,size_t bytes,unsigned host,struct gat_smem *out)
{
    int r=gat_smem_partition(base,bytes,host,out);
    if(r==GAT_WAIT&&host!=GAT_SMEM_GLOBAL_HOST)
        r=gat_smem_partition(base,bytes,GAT_SMEM_GLOBAL_HOST,out);
    return r;
}
/* Caller supplies an acquire barrier before inspection. A missing item is WAIT;
 * malformed headers never produce a pointer. Cached and uncached lists are
 * bounded by their published allocation offsets, not by an untrusted count. */
static inline int gat_smem_get(const struct gat_smem *p,unsigned item,volatile void **ptr,size_t *bytes)
{
    size_t u,c,off;volatile uint8_t *b;
    if(!p||!p->base||((uintptr_t)p->base&3)||p->bytes<32||!ptr||!bytes||item>65535||
       !p->cacheline||(p->cacheline&(p->cacheline-1))||p->cacheline>p->bytes)return GAT_INVALID;
    b=p->base;
    /* Allocation offsets are single-copy publications. Byte reads can tear
     * while another processor allocates across a byte boundary. */
    u=*(const volatile uint32_t *)(b+12);c=*(const volatile uint32_t *)(b+16);
    if(u<32||u>c||c>p->bytes)return GAT_INVALID;
    off=32;
    while(off<u) {
        size_t n,pad,hpad,next;
        if(u-off<16||gat_le16(b+off)!=0xa5a5)return GAT_INVALID;
        n=gat_le32(b+off+4);pad=gat_le16(b+off+8);hpad=gat_le16(b+off+10);
        if(!n||(n&7)||pad>=n||hpad>u-off-16||n>u-off-16-hpad)return GAT_INVALID;
        next=off+16+hpad+n;
        if(gat_le16(b+off+2)==item) {*ptr=b+off+16+hpad;*bytes=n-pad;return GAT_OK;}
        off=next;
    }
    if(c==p->bytes)return GAT_WAIT;
    size_t hs=(16+p->cacheline-1)&~(p->cacheline-1);
    if(hs>p->bytes-c)return GAT_INVALID;
    off=p->bytes-hs;
    while(off>c) {
        size_t n,pad;
        if(p->bytes-off<16||gat_le16(b+off)!=0xa5a5)return GAT_INVALID;
        n=gat_le32(b+off+4);pad=gat_le16(b+off+8);
        if(!n||(n&(p->cacheline-1))||pad>=n||n>off-c)return GAT_INVALID;
        if(gat_le16(b+off+2)==item) {*ptr=b+off-n;*bytes=n-pad;return GAT_OK;}
        if(off-n==c)return GAT_WAIT;
        if(hs>off-n-c)return GAT_INVALID;
        off-=n+hs;
    }
    return GAT_WAIT;
}
/* The caller MUST hold Qualcomm's interprocessor hardware spinlock. A local
 * lock/interrupt disable is insufficient. This routine cannot acquire it. */
static inline int gat_smem_alloc_locked(struct gat_smem *p,unsigned item,size_t bytes,
                                       void (*barrier)(void *),void *ctx)
{
    volatile void *old;size_t oldbytes,u,c,n;int r;
    if(!p||!p->base||!barrier||!bytes||bytes>UINT32_MAX-7u||item>65535)return GAT_INVALID;
    barrier(ctx);r=gat_smem_get(p,item,&old,&oldbytes);
    if(r==GAT_OK)return oldbytes==bytes?GAT_OK:GAT_INVALID;
    if(r<0)return r;
    u=gat_le32(p->base+12);c=gat_le32(p->base+16);n=gat_align8(bytes);
    if(u>c||c>p->bytes||c-u<16||n>c-u-16)return GAT_LIMIT;
    volatile uint8_t *e=p->base+u;
    gat_put16(e,0xa5a5);gat_put16(e+2,(uint16_t)item);gat_put32(e+4,(uint32_t)n);
    gat_put16(e+8,(uint16_t)(n-bytes));gat_put16(e+10,0);gat_put32(e+12,0);
    for(size_t i=0;i<n;i++)e[16+i]=0;
    barrier(ctx);*(volatile uint32_t *)(p->base+12)=(uint32_t)(u+16+n);barrier(ctx);
    return GAT_OK;
}

/* SMP2P item 429 (outbound), 443 (inbound); its IPCC signal is 2, whereas
 * GLINK uses signal 0. Publication and notification belong to the caller. */
#define GAT_SMP2P_BYTES 340u
#define GAT_SMP2P_FATAL (1u<<0)
#define GAT_SMP2P_READY (1u<<1)
static inline int gat_smp2p_init_outbound(volatile void *ptr,size_t bytes,
                                         void (*barrier)(void *),void *ctx,int owns_fresh_edge)
{
    volatile uint8_t *p=(volatile uint8_t *)ptr;
    static const char name[]="master-kernel";
    if(!p||bytes!=GAT_SMP2P_BYTES||!barrier)return GAT_INVALID;
    if(!owns_fresh_edge)return GAT_OWNERSHIP;
    /* Version zero invalidates the old item before rebuilding it. No SSR-ACK
     * feature is offered: automatic crash/restart recovery is not supported. */
    p[4]=0;barrier(ctx);
    for(size_t i=0;i<bytes;i++)p[i]=0;
    gat_put32(p,0x504d5324);gat_put16(p+8,0);gat_put16(p+10,2);
    gat_put16(p+12,16);gat_put16(p+14,1);
    for(size_t i=0;i<sizeof(name);i++)p[20+i]=(uint8_t)name[i];
    barrier(ctx);p[4]=1;barrier(ctx);return GAT_OK;
}
/* Returns the raw slave-kernel state, not a ready verdict. The caller must
 * observe a fresh ready transition after its own PAS boot. An old ready bit
 * remaining in SMEM cannot establish successful startup. */
static inline int gat_smp2p_slave(volatile const void *ptr,size_t bytes,uint32_t *state,uint32_t *flags)
{
    const volatile uint8_t *p=(const volatile uint8_t *)ptr;
    static const char name[]="slave-kernel";
    if(!p||!state||!flags||bytes!=GAT_SMP2P_BYTES||((uintptr_t)p&3))return GAT_INVALID;
    if(p[4]!=1)return GAT_WAIT;
    if(gat_le32(p)!=0x504d5324||gat_le16(p+8)!=2||gat_le16(p+10)!=0||gat_le16(p+12)!=16)return GAT_INVALID;
    unsigned valid=gat_le16(p+14);if(valid>16)return GAT_INVALID;
    unsigned found=0;uint32_t result=0;
    for(unsigned i=0;i<valid;i++) {
        const volatile uint8_t *e=p+20+20*i;unsigned match=1;
        for(size_t j=0;j<sizeof(name);j++)if(e[j]!=(uint8_t)name[j])match=0;
        if(match) {if(++found!=1)return GAT_INVALID;result=*(const volatile uint32_t *)(e+16);}
    }
    if(!found)return GAT_WAIT;
    *state=result;*flags=*(const volatile uint32_t *)(p+16);return GAT_OK;
}

/* Capture before PAS auth/reset while the caller owns a stopped ADSP. Arm only
 * after its PAS call succeeds. A retained ready=1 is not boot evidence: a low
 * sample or an unpublished inbound item must precede the new ready=1 sample.
 * WAIT means a successfully inspected absent/unpublished item, NOT a timeout,
 * failed map, skipped poll, or failed SMEM read. Feed other failures as errors.
 * Caller must supply an acquire barrier before every shared-memory snapshot. */
struct gat_smp2p_watch { unsigned captured,armed,saw_low,ready;int error; };
static inline int gat_smp2p_watch_capture(struct gat_smp2p_watch *w,int result,uint32_t state)
{
    if(!w||(result!=GAT_OK&&result!=GAT_WAIT))return result<0?result:GAT_INVALID;
    gat_zero(w,sizeof(*w));w->captured=1;
    w->saw_low=result==GAT_WAIT||!(state&GAT_SMP2P_READY);return GAT_OK;
}
static inline int gat_smp2p_watch_arm(struct gat_smp2p_watch *w)
{
    if(!w||!w->captured||w->armed||w->error)return GAT_INVALID;
    w->armed=1;return GAT_OK;
}
static inline int gat_smp2p_watch_observe(struct gat_smp2p_watch *w,int result,uint32_t state)
{
    if(!w||!w->captured||!w->armed)return GAT_INVALID;
    if(w->error)return w->error;
    if(result<0) { w->error=result;w->ready=0;return result; }
    if(result!=GAT_OK&&result!=GAT_WAIT)return GAT_INVALID;
    if(result==GAT_OK&&(state&GAT_SMP2P_FATAL)) {
        w->error=GAT_CLOSED;w->ready=0;return w->error;
    }
    if(result==GAT_WAIT||!(state&GAT_SMP2P_READY)) {
        /* Recovery after a previously ready DSP requires a new PAS lifecycle. */
        if(w->ready) { w->error=GAT_CLOSED;w->ready=0;return w->error; }
        w->saw_low=1;return GAT_WAIT;
    }
    if(!w->saw_low)return GAT_WAIT;
    w->ready=1;return GAT_OK;
}

struct gat_ring { volatile uint8_t *fifo;volatile uint32_t *head,*tail;size_t bytes; };
struct gat_ops {
    void (*barrier)(void *); /* full device/shared-memory barrier (e.g. dsb sy) */
    void (*kick)(void *);    /* IPCC client 3/signal 0, after published indices */
    void (*packet)(void *,const uint8_t *,size_t); /* borrowed; must not reenter */
};
struct gat_intent {uint32_t id,bytes;unsigned used,busy;};
struct gat_transport {
    struct gat_ring tx,rx;struct gat_ops ops;void *ctx;
    unsigned started,version_ack,open_sent,open_ack,remote_open,local_intent;
    uint16_t lcid,rcid;uint32_t liid;
    int error;unsigned intent_requested;
    struct gat_intent remote[GAT_INTENTS_MAX];
    uint8_t packet[GAT_PACKET_MAX];size_t packet_bytes,remaining;
    uint32_t spf_token,spf_status,spf_error;unsigned spf_pending,spf_received,spf_ready;
    uint32_t spf_queries,spf_basic_replies,spf_timeouts;
    /* Set only by the resident provider from its current native lease. */
    uint64_t epoch;
};
/* Atomic aligned little-endian index accesses. Supported targets are LE ARM64
 * and LE host tests. A byte-at-a-time read could tear an index at rollover. */
static inline int gat_ring_indices(struct gat_transport *t,const struct gat_ring *r,uint32_t *h,uint32_t *tail)
{
    if(!r->fifo||!r->head||!r->tail||r->bytes<64||(r->bytes&7)||r->bytes>UINT32_MAX)return GAT_INVALID;
    *h=*r->head;*tail=*r->tail;t->ops.barrier(t->ctx);
    if(*h>=r->bytes||*tail>=r->bytes||((*h|*tail)&7))return GAT_PROTOCOL;
    return GAT_OK;
}
static inline int gat_tx_space(struct gat_transport *t,size_t *space)
{
    uint32_t h,tail;int r=gat_ring_indices(t,&t->tx,&h,&tail);if(r<0)return r;
    size_t freebytes=tail<=h?t->tx.bytes-h+tail:tail-h;
    *space=freebytes<16?0:freebytes-16;return GAT_OK;
}
static inline int gat_emit(struct gat_transport *t,const uint8_t *hdr,size_t hn,const uint8_t *data,size_t dn)
{
    size_t space,total;int r;
    if(!t||t->error||!hdr||(!data&&dn)||hn>GAT_PACKET_MAX||dn>GAT_PACKET_MAX||hn+dn>GAT_PACKET_MAX+16u)return GAT_INVALID;
    total=gat_align8(hn+dn);r=gat_tx_space(t,&space);if(r<0)return r;
    if(total>space)return GAT_WAIT;
    size_t h=*t->tx.head;
    for(size_t i=0;i<total;i++)t->tx.fifo[(h+i)%t->tx.bytes]=i<hn?hdr[i]:(i<hn+dn?data[i-hn]:0);
    t->ops.barrier(t->ctx);*t->tx.head=(uint32_t)((h+total)%t->tx.bytes);
    t->ops.barrier(t->ctx);t->ops.kick(t->ctx);
    /* A native notification callback may latch an IRQ/MMIO fault. Never turn
     * that failure into success or let the caller consume another RX frame. */
    return t->error?t->error:GAT_OK;
}
static inline int gat_command(struct gat_transport *t,unsigned cmd,unsigned p1,uint32_t p2)
{
    uint8_t b[8];gat_put16(b,(uint16_t)cmd);gat_put16(b+2,(uint16_t)p1);gat_put32(b+4,p2);
    return gat_emit(t,b,8,0,0);
}
static inline int gat_attach_rx(struct gat_transport *t,volatile void *fifo,size_t bytes)
{
    if(!t||!fifo||bytes<64||(bytes&7)||bytes>UINT32_MAX||t->rx.fifo)return GAT_INVALID;
    t->rx.fifo=(volatile uint8_t *)fifo;t->rx.bytes=bytes;return GAT_OK;
}
static inline int gat_init(struct gat_transport *t,volatile void *descriptor,size_t desc_bytes,
                           volatile void *tx,size_t tx_bytes,volatile void *rx,size_t rx_bytes,
                           const struct gat_ops *ops,void *ctx,int owns_fresh_edge)
{
    if(!t||!descriptor||desc_bytes!=32||((uintptr_t)descriptor&3)||!tx||tx_bytes<64||
       (tx_bytes&7)||tx_bytes>UINT32_MAX||!ops||!ops->barrier||!ops->kick||
       (rx&&(rx_bytes<64||(rx_bytes&7)||rx_bytes>UINT32_MAX))||(!rx&&rx_bytes))return GAT_INVALID;
    if(!owns_fresh_edge)return GAT_OWNERSHIP;
    gat_zero(t,sizeof(*t));t->ops=*ops;t->ctx=ctx;t->lcid=1;t->liid=1;
    volatile uint32_t *d=(volatile uint32_t *)descriptor;
    t->tx.fifo=(volatile uint8_t *)tx;t->tx.bytes=tx_bytes;t->tx.tail=d;t->tx.head=d+1;
    t->rx.tail=d+2;t->rx.head=d+3;t->rx.fifo=(volatile uint8_t *)rx;t->rx.bytes=rx_bytes;
    /* Never change DSP-owned tx_tail or rx_head. */
    *t->tx.head=0;*t->rx.tail=0;t->ops.barrier(ctx);return GAT_OK;
}
static inline int gat_start(struct gat_transport *t)
{
    if(!t||t->error)return GAT_INVALID;
    if(t->started)return GAT_OK;
    int r=gat_command(t,0,1,0);if(r==GAT_OK)t->started=1;return r;
}
static inline int gat_channel_ready(const struct gat_transport *t)
{ return t&&!t->error&&t->version_ack&&t->open_sent&&t->open_ack&&t->remote_open; }
static inline int gat_advertise(struct gat_transport *t)
{
    uint8_t b[16];int r;
    if(t->local_intent)return GAT_OK;
    gat_put16(b,5);gat_put16(b+2,t->lcid);gat_put32(b+4,1);
    gat_put32(b+8,GAT_PACKET_MAX);gat_put32(b+12,t->liid);
    r=gat_emit(t,b,16,0,0);if(r==GAT_OK)t->local_intent=1;return r;
}
static inline int gat_progress(struct gat_transport *t)
{
    if(t->version_ack&&!t->open_sent) {
        const uint8_t name[]="adsp_apps";uint8_t b[8];
        gat_put16(b,2);gat_put16(b+2,t->lcid);gat_put32(b+4,sizeof(name));
        int r=gat_emit(t,b,8,name,sizeof(name));if(r!=GAT_OK)return r;t->open_sent=1;
    }
    if(gat_channel_ready(t))return gat_advertise(t);
    return GAT_OK;
}
static inline int gat_send(struct gat_transport *t,const uint8_t *data,size_t bytes)
{
    if(!t||!data||!bytes||bytes>GAT_PACKET_MAX)return GAT_INVALID;
    if(!gat_channel_ready(t))return t->error?t->error:GAT_WAIT;
    for(unsigned i=0;i<GAT_INTENTS_MAX;i++) {
        struct gat_intent *in=&t->remote[i];
        if(in->used&&!in->busy&&in->bytes>=bytes) {
            uint8_t b[16];gat_put16(b,9);gat_put16(b+2,t->lcid);gat_put32(b+4,in->id);
            gat_put32(b+8,(uint32_t)bytes);gat_put32(b+12,0);
            int r=gat_emit(t,b,16,data,bytes);if(r==GAT_OK)in->busy=1;return r;
        }
    }
    if(!t->intent_requested) {
        int r=gat_command(t,7,t->lcid,(uint32_t)bytes);
        if(r<0)return r;
        if(r==GAT_OK)t->intent_requested=1;
    }
    return GAT_WAIT;
}
static inline void gat_spf_receive(struct gat_transport *t,const uint8_t *b,size_t n)
{
    if(!t||!b||!t->spf_pending)return;
    uint64_t epoch=t->epoch;uint32_t token=t->spf_token;
    uint64_t selected=gapt_reply(epoch,GAPT_SPF_REPLY,1,token,3,1,GAT_SPF_GET,b,n);
    if(t->epoch!=epoch||!t->spf_pending||t->spf_token!=token)return;
    unsigned kind=(unsigned)selected;uint32_t status=(uint32_t)(selected>>32);
    if(kind==GAPT_SPF_READY||kind==GAPT_SPF_STATE) {
        t->spf_status=status;t->spf_received=1;t->spf_pending=0;
        t->spf_ready=kind==GAPT_SPF_READY;
    } else if(kind==GAPT_SPF_BASIC) {
        /* GSL ignores bootstrap BASIC_RSP, including status=0, until this
         * query times out; an unregistered APM service is not a fatal boot. */
        t->spf_error=status;++t->spf_basic_replies;
    }
}
static inline int gat_spf_query(struct gat_transport *t,uint32_t token)
{
    uint8_t b[24];int r;
    if(!t||!token||t->spf_pending)return GAT_INVALID;
    gat_zero(b,sizeof(b));gat_put32(b,(24u<<8)|(6u<<4));b[4]=2;b[5]=3;
    gat_put32(b+8,3);gat_put32(b+12,1);gat_put32(b+16,token);gat_put32(b+20,GAT_SPF_GET);
    r=gat_send(t,b,sizeof(b));if(r==GAT_OK) {
        t->spf_token=token;t->spf_pending=1;t->spf_received=0;t->spf_ready=0;++t->spf_queries;
    }
    return r;
}
static inline void gat_spf_expire(struct gat_transport *t)
{
    t->spf_pending=0;t->spf_received=0;t->spf_ready=0;++t->spf_timeouts;
}
static inline void gat_peek(const struct gat_ring *r,size_t offset,uint8_t *b,size_t n)
{ size_t tail=*r->tail;for(size_t i=0;i<n;i++)b[i]=r->fifo[(tail+offset+i)%r->bytes]; }

/* Process one complete frame. Incomplete frames and TX backpressure leave
 * rx_tail unchanged. Unsupported zero-copy/intentless modes are never offered.
 * Commands for unrelated channels are drained, without claiming those services.
 */
static inline int gat_poll_one(struct gat_transport *t)
{
    uint8_t h[16];uint32_t head,tail,p2,chunk=0,left=0;unsigned cmd,cid;
    size_t avail,frame=8,space;int r;
    /* Check the producer ring even when RX is idle, so corrupt peer indices
     * cannot remain hidden until the next command happens to require a reply. */
    r=gat_tx_space(t,&space);if(r<0)return r;
    if(!t->rx.fifo)return GAT_WAIT;
    r=gat_ring_indices(t,&t->rx,&head,&tail);if(r<0)return r;
    avail=head>=tail?head-tail:t->rx.bytes-tail+head;if(avail<8)return GAT_WAIT;
    gat_peek(&t->rx,0,h,8);cmd=gat_le16(h);cid=gat_le16(h+2);p2=gat_le32(h+4);
    if(cmd==2) { p2&=0xffffu; if(!p2||p2>256)return GAT_PROTOCOL;frame=gat_align8(8+p2); }
    else if(cmd==5) { if(p2>(t->rx.bytes-16)/8)return GAT_PROTOCOL;frame=8+8*(size_t)p2; }
    else if(cmd==9||cmd==12) {
        if(avail<16)return GAT_WAIT;
        gat_peek(&t->rx,8,h+8,8);chunk=gat_le32(h+8);left=gat_le32(h+12);
        if(chunk>t->rx.bytes-24)return GAT_PROTOCOL;frame=gat_align8(16+(size_t)chunk);
    }
    if(frame>t->rx.bytes-8)return GAT_PROTOCOL;
    if(avail<frame)return GAT_WAIT;
    /* Every immediate reply below is 8 bytes. Intent advertisement/open are
     * separate progress steps. Nonreply commands must drain even with full TX. */
    if(space<8&&(cmd==0||(cmd==1&&p2)||cmd==2||cmd==7||
       ((cmd==9||cmd==12)&&!left)||cmd==3))return GAT_WAIT;
    switch(cmd) {
    case 0: /* No optional features; remote must negotiate zero as well. */
        if(!cid)return GAT_PROTOCOL;
        /* A new negotiation on an established channel is a remote restart;
         * old intent ownership and SPF readiness cannot survive that edge. */
        if(gat_channel_ready(t))return GAT_CLOSED;
        r=gat_command(t,1,1,0);if(r!=GAT_OK)return r;break;
    case 1:
        if(!t->started||cid!=1)return GAT_PROTOCOL;
        if(p2) { r=gat_command(t,0,1,0);if(r!=GAT_OK)return r; }
        else t->version_ack=1;
        break;
    case 2: {
        uint8_t name[256];gat_peek(&t->rx,8,name,p2);
        if(!cid||name[p2-1])return GAT_PROTOCOL;
        static const uint8_t expected[]="adsp_apps";unsigned match=p2==sizeof(expected);
        if(match)for(size_t i=0;i<sizeof(expected);i++)if(name[i]!=expected[i])match=0;
        if(match) {
            if(t->remote_open&&t->rcid!=cid)return GAT_PROTOCOL;
            r=gat_command(t,4,cid,0);if(r!=GAT_OK)return r;
            t->rcid=(uint16_t)cid;t->remote_open=1;
        }
        break;
    }
    case 4: if(cid==t->lcid) { if(!t->open_sent)return GAT_PROTOCOL;t->open_ack=1; }break;
    case 5:
        if(t->remote_open&&cid==t->rcid) {
            /* Validate whole batch first so retry cannot partially insert it. */
            if(p2>GAT_INTENTS_MAX)return GAT_LIMIT;
            uint32_t ids[GAT_INTENTS_MAX],sizes[GAT_INTENTS_MAX];unsigned slots=0;
            for(unsigned i=0;i<GAT_INTENTS_MAX;i++)if(!t->remote[i].used)slots++;
            if(p2>slots)return GAT_LIMIT;
            for(uint32_t i=0;i<p2;i++) {
                gat_peek(&t->rx,8+8*i,h,8);sizes[i]=gat_le32(h);ids[i]=gat_le32(h+4);
                if(!sizes[i])return GAT_PROTOCOL;
                for(uint32_t j=0;j<i;j++)if(ids[i]==ids[j])return GAT_PROTOCOL;
                for(unsigned j=0;j<GAT_INTENTS_MAX;j++)if(t->remote[j].used&&ids[i]==t->remote[j].id)return GAT_PROTOCOL;
            }
            for(uint32_t i=0;i<p2;i++)for(unsigned j=0;j<GAT_INTENTS_MAX;j++)if(!t->remote[j].used) {
                t->remote[j].used=1;t->remote[j].id=ids[i];t->remote[j].bytes=sizes[i];break;
            }
            t->intent_requested=0;
        }
        break;
    case 6:case 14:
        if(t->remote_open&&cid==t->rcid) {
            unsigned found=0;
            for(unsigned i=0;i<GAT_INTENTS_MAX;i++)if(t->remote[i].used&&t->remote[i].id==p2) {
                if(!t->remote[i].busy)return GAT_PROTOCOL;
                t->remote[i].busy=0;if(cmd==6)t->remote[i].used=0;found=1;break;
            }
            if(!found)return GAT_PROTOCOL;
        }
        break;
    case 7:
        if(t->remote_open&&cid==t->rcid) {
            unsigned grant=p2<=GAT_PACKET_MAX&&t->local_intent&&!t->packet_bytes;
            r=gat_command(t,8,t->lcid,grant);if(r!=GAT_OK)return r;
        }
        break;
    case 8:
        if(t->remote_open&&cid==t->rcid&&!p2)t->intent_requested=0;
        break;
    case 9:case 12:
        if(t->remote_open&&cid==t->rcid) {
            if(!gat_channel_ready(t)||!t->local_intent||p2!=t->liid||!chunk||
               chunk>GAT_PACKET_MAX-t->packet_bytes||left>GAT_PACKET_MAX-t->packet_bytes-chunk||
               (cmd==9&&t->packet_bytes)||(cmd==12&&!t->packet_bytes)||
               (cmd==12&&t->remaining!=chunk+left))return GAT_PROTOCOL;
            gat_peek(&t->rx,16,t->packet+t->packet_bytes,chunk);
            t->packet_bytes+=chunk;t->remaining=left;
            if(!left) {
                r=gat_command(t,6,t->lcid,t->liid);if(r!=GAT_OK)return r;
                gat_spf_receive(t,t->packet,t->packet_bytes);
                if(t->ops.packet)t->ops.packet(t->ctx,t->packet,t->packet_bytes);
                t->packet_bytes=0;t->local_intent=0;
                if(++t->liid==0)t->liid=1;
            }
        }
        break;
    case 13: break; /* notify once, after publishing the consumed RX tail */
    case 15: break; /* modem-control signals irrelevant to GPR */
    case 3:
        if(t->remote_open&&cid==t->rcid) {
            r=gat_command(t,11,cid,0);if(r!=GAT_OK)return r;
            t->spf_ready=0;t->spf_pending=0;t->remote_open=0;return GAT_CLOSED;
        }
        break;
    case 11: if(cid==t->lcid)return GAT_CLOSED;break;
    default:return GAT_PROTOCOL;
    }
    t->ops.barrier(t->ctx);*t->rx.tail=(uint32_t)((tail+frame)%t->rx.bytes);
    t->ops.barrier(t->ctx);t->ops.kick(t->ctx);return GAT_OK;
}
static inline int gat_poll(struct gat_transport *t,unsigned budget)
{
    if(!t||!t->started||!budget)return GAT_INVALID;
    if(t->error)return t->error;
    for(unsigned i=0;i<budget;i++) {
        int r=gat_progress(t);if(r==GAT_WAIT)return r;
        if(r<0) {t->error=r;t->spf_ready=0;return r;}
        r=gat_poll_one(t);
        if(t->error)r=t->error;
        if(r<0) {t->error=r;t->spf_ready=0;t->spf_pending=0;return r;}
        if(r==GAT_WAIT)return r;
    }
    return GAT_OK;
}
#endif
