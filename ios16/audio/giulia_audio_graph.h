#ifndef GIULIA_AUDIO_GRAPH_H
#define GIULIA_AUDIO_GRAPH_H
/* AudioReach APM in-band control for the stock LL speaker graph.
 * Wire/API reference: AudioReach/audioreach-graphservices
 * 76d04ffb30caa1fe609b94f9f6081210cb962fc1: spf/api/apm/apm_api.h,
 * apm_sub_graph_api.h; gsl/src/gsl_main.c. No MMIO, allocation or PCM DMA.
 * Caller serializes calls and owns immutable authenticated bundle lifetimes.
 * send must not synchronously reenter receive; success means queued, never ACK.
 * A START ACK establishes graph state only, not audible/calibrated playback.
 */
#include <stdint.h>
#include <stddef.h>

#define GAG_MAX_PACKET 4096u
#define GAG_MAX_PAYLOAD (GAG_MAX_PACKET-40u)
#define GAG_TIMEOUT_US 1000000u
#define GAG_OPEN 0x01001000u
#define GAG_PREPARE 0x01001001u
#define GAG_START 0x01001002u
#define GAG_STOP 0x01001003u
#define GAG_CLOSE 0x01001004u
#define GAG_SET_CFG 0x01001006u
#define GAG_BASIC_RSP 0x02001005u
#define GAG_MODULES_READY 1u
#define GAG_MEDIA_CONFIGURED 2u
#define GAG_SPEAKER_CALIBRATED 4u
#define GAG_DMA_READY 8u
#define GAG_REQUIRED 15u
enum { GAG_WAIT=0, GAG_OK=1, GAG_EINVAL=-1, GAG_EAUTH=-2,
       GAG_ESTATE=-3, GAG_ELIMIT=-4, GAG_ETIMEOUT=-5, GAG_EREMOTE=-6,
       GAG_EIO=-7, GAG_ENOTREADY=-8 };
enum { GAG_CLOSED=0, GAG_OPENED, GAG_CONFIGURING, GAG_CONFIGURED,
       GAG_PREPARED, GAG_STARTED, GAG_FAILED };
struct gag_asset { const uint8_t *data; size_t bytes; };
struct gag_bundle { struct gag_asset open, connections, baseline_cal; };
struct gag_ops {
    void *ctx;
    /* 1=queued, 0=backpressure/no queueing, negative=I/O failure. */
    int (*send)(void *, const uint8_t *, size_t);
    uint64_t (*now_us)(void *);
    /* Compute SHA256 and compare against all 64 hex digits. Zero=verified. */
    int (*verify_sha256)(void *, const uint8_t *, size_t, const char *expected_hex);
};
struct gag_graph {
    struct gag_bundle bundle;
    struct gag_ops ops;
    uint8_t packet[GAG_MAX_PACKET];
    uint32_t src_port, next_token, pending_token, pending_opcode, remote_status;
    size_t cal_offset, pending_advance;
    uint64_t deadline, last_time;
    unsigned valid, state, pending_state, open_possible;
    int error;
};
static inline uint32_t gag_u32(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static inline void gag_put(uint8_t *p,uint32_t n)
{ for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i)); }
static inline void gag_zero(void *p,size_t n)
{ uint8_t *b=(uint8_t *)p;for(size_t i=0;i<n;i++)b[i]=0; }
static inline void gag_copy(void *d,const void *s,size_t n)
{ uint8_t *to=(uint8_t *)d;const uint8_t *from=(const uint8_t *)s;for(size_t i=0;i<n;i++)to[i]=from[i]; }
static inline int gag_parameters(const uint8_t *p,size_t n,size_t max_record)
{
    size_t off=0;
    if(!p||!n)return GAG_EINVAL;
    while(off<n) {
        if(n-off<16)return GAG_EINVAL;
        uint32_t bytes=gag_u32(p+off+8);
        /* Stock volume calibration has a 2-byte value; the record including
         * trailing padding, rather than param_size itself, is 8-byte aligned. */
        if(bytes>UINT32_MAX-7u||gag_u32(p+off+12))return GAG_EINVAL;
        size_t padded=((size_t)bytes+7u)&~(size_t)7u;
        if(padded>n-off-16)return GAG_EINVAL;
        if(padded+16>max_record)return GAG_ELIMIT;
        off+=16+padded;
    }
    return GAG_OK;
}
static inline int gag_fail(struct gag_graph *g,int error)
{ g->state=GAG_FAILED;g->error=error;g->pending_token=0;return error; }
static inline int gag_init(struct gag_graph *g,const struct gag_bundle *b,
                           const struct gag_ops *ops,uint32_t src_port,uint32_t first_token)
{
    static const char *const pins[3]={
        "5329daa2e9fd850db679ee7c4ddd65479be13c4e936ca1b9739a98ea391a99ed",
        "2bb6463e6425921f6dd362a2c4e2c45e6a901205cfc451633e07653b3fb8c87c",
        "692062db86e03476f53a6608a944a2621886f9dab84aea73159c3d9306d5a21f"};
    if(!g)return GAG_EINVAL;
    gag_zero(g,sizeof(*g));
    if(!b||!ops||!ops->send||!ops->now_us||!ops->verify_sha256||
       src_port<0x2010||src_port>0x2100||!first_token||first_token==UINT32_MAX||
       b->open.bytes!=2792||b->connections.bytes!=120||b->baseline_cal.bytes!=6400)
        return GAG_EINVAL;
    const struct gag_asset *assets[3]={&b->open,&b->connections,&b->baseline_cal};
    for(unsigned i=0;i<3;i++) {
        if(!assets[i]->data)return GAG_EINVAL;
        if(ops->verify_sha256(ops->ctx,assets[i]->data,assets[i]->bytes,pins[i]))return GAG_EAUTH;
        int r=gag_parameters(assets[i]->data,assets[i]->bytes,GAG_MAX_PAYLOAD);
        if(r!=GAG_OK)return r;
    }
    g->bundle=*b;g->ops=*ops;g->src_port=src_port;g->next_token=first_token;
    g->last_time=ops->now_us(ops->ctx);g->state=GAG_CLOSED;g->valid=1;
    return GAG_OK;
}
/* Poll this while awaiting an ACK, including when transport has no packets.
 * After timeout the DSP state is unknown; only CLOSE may recover this graph.
 * A fresh init after ADSP reset needs a non-reused token range. */
static inline int gag_poll(struct gag_graph *g)
{
    if(!g||!g->valid)return GAG_EINVAL;
    uint64_t now=g->ops.now_us(g->ops.ctx);
    if(now<g->last_time)return gag_fail(g,GAG_ETIMEOUT);
    g->last_time=now;
    if(g->pending_token&&now>=g->deadline)return gag_fail(g,GAG_ETIMEOUT);
    return g->pending_token?GAG_WAIT:(g->error?g->error:GAG_OK);
}
/* Call on transport loss/SSR. It never turns a lost response into success. */
static inline void gag_invalidate(struct gag_graph *g)
{ if(g&&g->valid)(void)gag_fail(g,GAG_EIO); }

static inline int gag_submit(struct gag_graph *g,uint32_t opcode,size_t bytes,
                             unsigned next_state,size_t advance)
{
    if(!g||!g->valid||g->pending_token)return GAG_ESTATE;
    if(bytes>GAG_MAX_PAYLOAD||!bytes)return GAG_ELIMIT;
    if(!g->next_token||g->next_token==UINT32_MAX)return GAG_ELIMIT;
    uint32_t token=g->next_token++;
    uint64_t now=g->ops.now_us(g->ops.ctx);
    if(now<g->last_time)return gag_fail(g,GAG_ETIMEOUT);
    g->last_time=now;
    if(now>UINT64_MAX-GAG_TIMEOUT_US)return gag_fail(g,GAG_ETIMEOUT);
    gag_zero(g->packet,40);
    gag_put(g->packet,(uint32_t)((40+bytes)<<8)|(6u<<4));
    g->packet[4]=2;g->packet[5]=3;
    gag_put(g->packet+8,g->src_port);gag_put(g->packet+12,1);
    gag_put(g->packet+16,token);gag_put(g->packet+20,opcode);
    gag_put(g->packet+36,(uint32_t)bytes); /* In-band APM header, map=0. */
    int r=g->ops.send(g->ops.ctx,g->packet,40+bytes);
    if(r<0)return gag_fail(g,GAG_EIO);
    if(r==0)return GAG_WAIT;
    if(r!=GAG_OK)return gag_fail(g,GAG_EIO);
    g->pending_token=token;g->pending_opcode=opcode;g->pending_state=next_state;
    g->pending_advance=advance;g->deadline=now+GAG_TIMEOUT_US;
    if(opcode==GAG_OPEN)g->open_possible=1;
    return GAG_OK;
}
static inline int gag_open(struct gag_graph *g)
{
    if(!g||!g->valid||g->state!=GAG_CLOSED||g->pending_token)return GAG_ESTATE;
    gag_copy(g->packet+40,g->bundle.open.data,g->bundle.open.bytes);
    gag_copy(g->packet+40+g->bundle.open.bytes,g->bundle.connections.data,g->bundle.connections.bytes);
    g->cal_offset=0;
    return gag_submit(g,GAG_OPEN,g->bundle.open.bytes+g->bundle.connections.bytes,GAG_OPENED,0);
}
/* Each chunk ends on a complete 16-byte APM parameter header + padded body.
 * The caller repeats after each ACK until state==GAG_CONFIGURED. */
static inline int gag_config_next(struct gag_graph *g)
{
    if(!g||!g->valid||g->pending_token||
       (g->state!=GAG_OPENED&&g->state!=GAG_CONFIGURING))return GAG_ESTATE;
    size_t off=g->cal_offset,n=0,total=g->bundle.baseline_cal.bytes;
    const uint8_t *p=g->bundle.baseline_cal.data;
    while(off+n<total) {
        size_t record=16+(((size_t)gag_u32(p+off+n+8)+7u)&~(size_t)7u);
        if(record>GAG_MAX_PAYLOAD-n)break;
        n+=record;
    }
    if(!n)return GAG_ELIMIT;
    gag_copy(g->packet+40,p+off,n);
    return gag_submit(g,GAG_SET_CFG,n,off+n==total?GAG_CONFIGURED:GAG_CONFIGURING,n);
}
/* Dynamic media/protection configuration must come from independently verified
 * runtime parameters. This API does not assert their semantic correctness. */
static inline int gag_set_config(struct gag_graph *g,const uint8_t *p,size_t bytes)
{
    if(!g||!g->valid||g->pending_token||g->state!=GAG_CONFIGURED)return GAG_ESTATE;
    if(bytes>GAG_MAX_PAYLOAD)return GAG_ELIMIT;
    int r=gag_parameters(p,bytes,GAG_MAX_PAYLOAD);if(r!=GAG_OK)return r;
    gag_copy(g->packet+40,p,bytes);
    return gag_submit(g,GAG_SET_CFG,bytes,GAG_CONFIGURED,0);
}
static inline int gag_control(struct gag_graph *g,uint32_t opcode,unsigned prerequisites)
{
    static const uint32_t ids[4]={0xb0000002,0xb0000025,0xb0000024,0xb0000023};
    unsigned next;
    if(!g||!g->valid||g->pending_token)return GAG_ESTATE;
    switch(opcode) {
    case GAG_PREPARE:
        if(g->state!=GAG_CONFIGURED)return GAG_ESTATE;
        if((prerequisites&GAG_REQUIRED)!=GAG_REQUIRED)return GAG_ENOTREADY;
        next=GAG_PREPARED;break;
    case GAG_START:
        if(g->state!=GAG_PREPARED)return GAG_ESTATE;
        if((prerequisites&GAG_REQUIRED)!=GAG_REQUIRED)return GAG_ENOTREADY;
        next=GAG_STARTED;break;
    case GAG_STOP:
        if(g->state!=GAG_STARTED&&g->state!=GAG_PREPARED)return GAG_ESTATE;
        next=GAG_CONFIGURED;break;
    case GAG_CLOSE:
        if(!g->open_possible)return GAG_ESTATE;
        next=GAG_CLOSED;break;
    default:return GAG_EINVAL;
    }
    uint8_t *p=g->packet+40;
    gag_zero(p,40);gag_put(p,1);gag_put(p+4,0x08001005);gag_put(p+8,20);
    gag_put(p+16,4);for(unsigned i=0;i<4;i++)gag_put(p+20+4*i,ids[i]);
    /* Payload 20 bytes is followed by four zero bytes of 8-byte alignment. */
    return gag_submit(g,opcode,40,next,0);
}
static inline int gag_receive(struct gag_graph *g,const uint8_t *p,size_t bytes)
{
    if(!g||!g->valid||!p)return GAG_EINVAL;
    if(!g->pending_token)return GAG_WAIT;
    if(gag_poll(g)!=GAG_WAIT)return g->error;
    if(bytes!=32||gag_u32(p)!=((32u<<8)|(6u<<4))||p[4]!=3||p[5]!=2||p[6]||p[7]||
       gag_u32(p+8)!=1||gag_u32(p+12)!=g->src_port||
       gag_u32(p+16)!=g->pending_token||gag_u32(p+20)!=GAG_BASIC_RSP||
       gag_u32(p+24)!=g->pending_opcode)return GAG_WAIT;
    g->remote_status=gag_u32(p+28);
    if(g->remote_status)return gag_fail(g,GAG_EREMOTE);
    g->state=g->pending_state;g->cal_offset+=g->pending_advance;
    g->pending_token=0;g->error=0;
    if(g->state==GAG_CLOSED){g->open_possible=0;g->cal_offset=0;}
    return GAG_OK;
}
#endif
