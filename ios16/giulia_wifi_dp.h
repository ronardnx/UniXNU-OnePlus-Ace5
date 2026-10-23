/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * WCN7850 bounded single-station data transport. Hardware field definitions
 * are from Linux v6.12 ath12k/{hal,hal_desc,hal_tx,hal_rx,dp,dp_rx,dp_tx}.
 * CE4 carries HTT control only. Ethernet data uses TCL -> WBM and RXDMA -> REO.
 * All entry points run on the driver's serialized device worker. Firmware
 * addresses and cookies are validated against owned slots, never dereferenced.
 * Cookie conversion is explicitly disabled: the documented software-cookie
 * completion format avoids exporting kernel pointers through device CMEM.
 */
#ifndef GIULIA_WIFI_DP_H
#define GIULIA_WIFI_DP_H
#include "giulia_wifi_qmi.h"
#define GWD_BYTES 0x400000u
#define GWD_TX_COUNT 64u
#define GWD_RX_COUNT 512u
#define GWD_BUFFER 2048u
#define GWD_RX_DESC_BYTES 400u
#define GWD_TX_BUFFERS 0x40000u
#define GWD_RX_BUFFERS 0x60000u
#define GWD_LINK_DESC 0x160000u
#define GWD_QUEUES 0x180000u
#define GWD_MGMT 0x190000u
#define GWD_MGMT_BYTES 0x10000u
#define GWD_QUEUE_BANK_BYTES (17u*1024u)
#define GWD_QUEUE_BANKS 1u
#define GWD_REO_TIMEOUT_MS 3000u
#define GWD_REO_AGE_MS 1000u
enum gwd_error {GWD_OK,GWD_ARGUMENT,GWD_DMA,GWD_INHERITED,GWD_REGISTER,GWD_POINTER,
    GWD_COOKIE,GWD_FRAME,GWD_FULL,GWD_NOT_READY,GWD_TIMEOUT,GWD_TRANSPORT,GWD_FIRMWARE};
enum {GWD_TCL,GWD_TX_COMPLETE,GWD_REO,GWD_REFILL,GWD_MAC0,GWD_MAC1,GWD_RXDMA_ERROR,
    GWD_RX_ERROR,GWD_REO_ERROR,GWD_IDLE_LINK,GWD_LINK_RELEASE,GWD_REO_COMMAND,GWD_REO_STATUS,GWD_RINGS};
enum {GWD_RECLAIM_IDLE,GWD_RECLAIM_TX,GWD_RECLAIM_INVALIDATE,GWD_RECLAIM_AGE,GWD_RECLAIM_FLUSH,GWD_RECLAIM_DRAIN};
struct gwd_io {
    void *context;
    uint32_t (*read)(void *,uint32_t);
    void (*write)(void *,uint32_t,uint32_t);
    int (*ready)(void *,uint64_t,size_t);
    void (*sync)(void *,size_t,size_t,int);
    int (*send)(void *,unsigned,const void *,size_t); /* CE: 0 sent, 2 full */
    int (*pump)(void *,unsigned);
    uint64_t (*now_ms)(void *);
    int (*idle)(void *); /* proven endpoint bus master disabled */
    int (*receive)(void *,const void *,size_t);
    void (*complete)(void *,uint32_t,unsigned);
    void (*peer)(void *,unsigned,const uint8_t *,unsigned,int);
    uint8_t *memory;uint64_t dma;
};
struct gwd_ring {uint32_t reg,ptr,desc,remote,host,count,entry,head,tail;unsigned dst,id,lmac,active;};
struct gwd_saved {uint32_t address,value,mask;};
/* Last observed packet rate, never an AP capability or a configured maximum.
 * raw is TX rate_stats.info0 or RX MSDU_END.info12 respectively. kbps is
 * known only for the legacy 20 MHz modes this station negotiates. */
struct gwd_rate_sample {uint64_t observed_ms;uint32_t raw,kbps;unsigned valid;};
struct gwd_state {
    unsigned prepared,active,quarantine,error,stage,endpoint,max_message,connecting,version,seq;
    unsigned peer_selected,peer_mapped,peer_id,vdev,pmac,pdev,ast_index,ast_hash,encrypted,authorized,queues;
    unsigned tx_pending,rx_dropped,rx_packets,tx_packets,saved_count,firmware_rings;
    unsigned queue_bank,next_queue_bank;
    unsigned reclaim_phase,reclaim_tid,reclaim_part,reo_cmd_seq,reo_pending,reo_expected,reo_consumed,reo_ack;
    uint64_t reclaim_since,reo_since;
    uint8_t bssid[6],tx_owned[GWD_TX_COUNT],rx_owned[GWD_RX_COUNT];
    uint32_t tx_cookie[GWD_TX_COUNT],rx_cookie[GWD_RX_COUNT],tx_token[GWD_TX_COUNT];
    uint8_t key_valid[5];uint64_t pn[5][17];
    struct gwd_rate_sample tx_rate,rx_rate;
    struct gwd_ring ring[GWD_RINGS];struct gwd_saved saved[160];
};
static inline int gwd_fail(struct gwd_state *s,unsigned e) {
    if(!s->error)s->error=e;s->active=0;if(s->prepared)s->quarantine=1;return (int)s->error;
}
static inline int gwd_valid(const struct gwd_io *io) {
    return io && io->read && io->write && io->ready && io->sync && io->send && io->pump &&
        io->now_ms && io->idle && io->receive && io->memory && !((uintptr_t)io->memory&0x3fff) &&
        !(io->dma&0x3fff) && io->dma<=UINT64_C(0x100000000)-GWD_BYTES &&
        (uintptr_t)io->memory<=UINTPTR_MAX-GWD_BYTES;
}
static inline void gwd_zero(void *out,size_t n) {uint8_t *p=(uint8_t *)out;while(n--)*p++=0;}
static inline uint32_t gwd_legacy_rate_kbps(unsigned packet_type,unsigned code,unsigned bw) {
    /* Linux hw.h enum ath12k_hw_rate_{ofdm,cck}, resolved through mac.c's
     * ath12k_legacy_rates. CCK codes 4..6 select the short preamble. */
    static const uint16_t ofdm[8]={48000,24000,12000,6000,54000,36000,18000,9000};
    static const uint16_t cck[7]={11000,5500,2000,1000,11000,5500,2000};
    if(bw)return 0;
    if(packet_type==0 && code<8)return ofdm[code];
    if(packet_type==1 && code<7)return cck[code];
    return 0; /* HT/VHT/HE need information this completion does not provide. */
}
static inline void gwd_rate_observe(struct gwd_rate_sample *sample,const struct gwd_io *io,
                                   uint32_t raw,unsigned packet_type,unsigned code,unsigned bw) {
    sample->raw=raw;sample->kbps=gwd_legacy_rate_kbps(packet_type,code,bw);
    sample->valid=sample->kbps!=0;sample->observed_ms=io->now_ms(io->context);
}
static inline void gwd_rate_reset(struct gwd_state *s) {
    gwd_zero(&s->tx_rate,sizeof(s->tx_rate));gwd_zero(&s->rx_rate,sizeof(s->rx_rate));
}
static inline int gwd_same(const uint8_t *a,const uint8_t *b) {
    unsigned different=0;for(unsigned i=0;i<6;i++)different|=a[i]^b[i];return !different;
}
static inline int gwd_write_mask(struct gwd_state *s,const struct gwd_io *io,uint32_t a,uint32_t n,uint32_t mask) {
    unsigned i;for(i=0;i<s->saved_count;i++)if(s->saved[i].address==a)break;
    if(i==s->saved_count) {
        if(i>=sizeof(s->saved)/sizeof(s->saved[0]))return gwd_fail(s,GWD_ARGUMENT);
        uint32_t old=io->read(io->context,a);if(old==UINT32_MAX)return gwd_fail(s,GWD_REGISTER);
        s->saved[i].address=a;s->saved[i].value=old&mask;s->saved[i].mask=mask;s->saved_count++;
    }
    io->write(io->context,a,n);
    uint32_t got=io->read(io->context,a);
    return got!=UINT32_MAX && (got&mask)==(n&mask)?0:gwd_fail(s,GWD_REGISTER);
}
static inline int gwd_write(struct gwd_state *s,const struct gwd_io *io,uint32_t a,uint32_t n) {
    return gwd_write_mask(s,io,a,n,UINT32_MAX);
}
static inline int gwd_update(struct gwd_state *s,const struct gwd_io *io,uint32_t a,uint32_t clear,uint32_t set) {
    uint32_t old=io->read(io->context,a);if(old==UINT32_MAX)return gwd_fail(s,GWD_REGISTER);
    return gwd_write(s,io,a,(old&~clear)|set);
}
static inline void gwd_address(uint8_t *p,uint64_t pa,unsigned rbm,uint32_t cookie) {
    gwq_w32(p,(uint32_t)pa);gwq_w32(p+4,(uint32_t)(pa>>32)|(rbm<<8)|(cookie<<12));
}
static inline int gwd_ring_remote(struct gwd_state *s,const struct gwd_io *io,struct gwd_ring *r,unsigned *slot) {
    io->sync(io->context,r->remote,4,0);uint32_t words=gwq_u32(io->memory+r->remote);
    if(words>=r->count*r->entry || words%r->entry)return gwd_fail(s,GWD_POINTER);
    *slot=words/r->entry;return 0;
}
static inline void gwd_ring_publish(const struct gwd_io *io,struct gwd_ring *r) {
    unsigned value=(r->dst?r->tail:r->head)*r->entry;
    if(r->lmac){gwq_w32(io->memory+r->host,value);io->sync(io->context,r->host,4,1);}
    else io->write(io->context,r->ptr+(r->dst?4:0),value);
}
static inline void gwd_layout(struct gwd_state *s) {
    static const uint32_t regs[GWD_RINGS]={0xa44900,0xa34e08,0xa384e4,0,0,0,0,
        0xa34f70,0xa388a4,0xa34d3c,0xa3437c,0xa3828c,0xa38a84};
    static const uint32_t ptrs[GWD_RINGS]={0xa46000,0xa370c8,0xa3b048,0,0,0,0,
        0xa370e0,0xa3b088,0xa370b8,0xa37010,0xa3b020,0xa3b0a8};
    static const unsigned counts[GWD_RINGS]={64,128,512,512,512,512,128,128,128,1024,128,64,128};
    static const unsigned entries[GWD_RINGS]={8,8,8,2,2,2,8,8,8,2,8,12,28};
    static const unsigned ids[GWD_RINGS]={24,128,1,0,0,0,0,131,0,120,121,20,21};
    for(unsigned i=0;i<GWD_RINGS;i++) {
        struct gwd_ring *r=&s->ring[i];r->reg=regs[i];r->ptr=ptrs[i];r->count=counts[i];r->entry=entries[i];
        r->id=ids[i];r->desc=0x1000+i*0x4000;r->remote=i*64;r->host=i*64+4;
        r->lmac=i>=GWD_REFILL && i<=GWD_RXDMA_ERROR;
        r->dst=i==GWD_TX_COMPLETE || i==GWD_REO || i==GWD_RXDMA_ERROR || i==GWD_RX_ERROR || i==GWD_REO_ERROR || i==GWD_REO_STATUS;
    }
}
static inline int gwd_prepare(struct gwd_state *s,const struct gwd_io *io) {
    if(!s || !gwd_valid(io) || s->prepared || s->saved_count || s->quarantine || s->error)return GWD_ARGUMENT;
    if(!io->ready(io->context,io->dma,GWD_BYTES))return gwd_fail(s,GWD_DMA);
    gwd_layout(s);s->stage=1;
    /* Refuse inherited live UMAC rings before writing any descriptor. */
    for(unsigned i=0;i<GWD_RINGS;i++)if(!s->ring[i].lmac) {
        uint32_t misc=io->read(io->context,s->ring[i].reg+0x10);
        if(misc==UINT32_MAX || (misc&0x40))return gwd_fail(s,GWD_INHERITED);
    }
    gwd_zero(io->memory,GWD_BYTES);io->sync(io->context,0,GWD_BYTES,1);s->prepared=1;
    /* Disable both converters. Completion cookies remain numeric IDs. */
    if(gwd_update(s,io,0xa38054,0x180000,0) || gwd_update(s,io,0xa34094,0x13e,0) ||
       gwd_update(s,io,0xa34090,0x0b,0))return (int)s->error;
    /* Bank 0: Ethernet-II. Bank 1: native Wi-Fi for unprotected EAPOL,
     * including M4 sent after keys are installed. Both use ADDRY/vdev checks.
     * Linux hal_tx.h defines ENCAP_TYPE bits 2:1 and four-byte bank stride. */
    if(gwd_write(s,io,0xa4408c,0x4804) || gwd_write(s,io,0xa44090,0x4802) || gwd_update(s,io,0xa38000,0,0x0c) ||
       gwd_update(s,io,0xa38b7c,0x1fe0000,0))return (int)s->error;
    for(unsigned i=0;i<4;i++)if(gwd_write(s,io,0xa38b08+i*4,i==3?40000:100000))return (int)s->error;
    if(gwd_write(s,io,0xa3800c,0x11111111) || gwd_write(s,io,0xa38010,0x11111111))return (int)s->error;
    for(unsigned i=0;i<GWD_RINGS;i++) {
        struct gwd_ring *r=&s->ring[i];if(r->lmac)continue;
        uint64_t base=io->dma+r->desc,remote=io->dma+r->remote;uint32_t a=r->reg;
        if(gwd_write(s,io,a,(uint32_t)base) || gwd_write(s,io,a+4,(uint32_t)(base>>32)|(r->count*r->entry<<8)) ||
           gwd_write(s,io,a+8,r->entry|(r->dst?r->id<<8:0)) ||
           gwd_write(s,io,a+(r->dst?0x24:0x30),0) || gwd_write(s,io,a+0x4c,0))return (int)s->error;
        if(!r->dst && gwd_write(s,io,a+0x34,0))return (int)s->error;
        if(i!=GWD_IDLE_LINK && (gwd_write(s,io,a+(r->dst?0x14:0x1c),(uint32_t)remote) ||
           gwd_write(s,io,a+(r->dst?0x18:0x20),(uint32_t)(remote>>32))))return (int)s->error;
        if(gwd_write(s,io,r->ptr,0) || gwd_write(s,io,r->ptr+4,0))return (int)s->error;
        r->active=1;
        /* MISC also contains read-only live status; compare writable bits. */
        if(gwd_write_mask(s,io,a+0x10,r->dst?0x40:i==GWD_IDLE_LINK?0x43:0x42,0x7f))return (int)s->error;
    }
    struct gwd_ring *idle=&s->ring[GWD_IDLE_LINK];
    for(unsigned i=0;i<idle->count-1;i++)
        gwd_address(io->memory+idle->desc+i*8,io->dma+GWD_LINK_DESC+i*128,1,i);
    idle->head=idle->count-1;io->sync(io->context,idle->desc,idle->count*8,1);gwd_ring_publish(io,idle);
    return 0;
}
/* HTC control body, already separated from the outer header by WMI router. */
static inline int gwd_htc_control(struct gwd_state *s,const void *v,size_t n) {
    const uint8_t *p=(const uint8_t *)v;
    if(!s || !p || n<12 || gwq_u16(p)!=3 || gwq_u16(p+2)!=0x300 || !s->connecting ||
       p[4] || !p[5] || p[5]>8 || gwq_u16(p+6)<68)return s?gwd_fail(s,GWD_FIRMWARE):GWD_ARGUMENT;
    s->endpoint=p[5];s->max_message=gwq_u16(p+6);s->connecting=0;return 0;
}
static inline int gwd_wire(struct gwd_state *s,const struct gwd_io *io,unsigned endpoint,const void *v,size_t n) {
    if(n>248 || (endpoint && (!s->endpoint || n>s->max_message)))return GWD_ARGUMENT;
    uint8_t packet[256]={0};packet[0]=(uint8_t)endpoint;packet[5]=(uint8_t)s->seq++;
    gwq_w16(packet+2,(uint16_t)n);gwq_copy(packet+8,v,n);
    for(unsigned i=0;i<1000;i++) {
        int error=io->send(io->context,endpoint?4:0,packet,n+8);
        if(!error)return 0;if(error!=2 || io->pump(io->context,1000))return gwd_fail(s,GWD_TRANSPORT);
    }
    return gwd_fail(s,GWD_TIMEOUT);
}
static inline int gwd_connect(struct gwd_state *s,const struct gwd_io *io) {
    if(!s || !gwd_valid(io) || !s->prepared || s->endpoint || s->error)return GWD_ARGUMENT;
    uint8_t request[8]={0};gwq_w32(request,0x03000002);gwq_w32(request+4,8);s->connecting=1;
    if(gwd_wire(s,io,0,request,sizeof(request)))return (int)s->error;
    for(unsigned i=0;i<3000 && !s->endpoint && !s->error;i++)if(io->pump(io->context,1000))return gwd_fail(s,GWD_TRANSPORT);
    return s->endpoint?0:gwd_fail(s,GWD_TIMEOUT);
}
static inline int gwd_select_peer(struct gwd_state *s,unsigned vdev,unsigned pmac,unsigned pdev,const uint8_t mac[6],int encrypted) {
    if(!s || !mac || !s->active || s->peer_mapped || s->tx_pending || vdev>255 || pmac>1 || pdev>3 ||
       (mac[0]&1) || encrypted<0 || encrypted>1)return GWD_ARGUMENT;
    unsigned nz=0;for(unsigned i=0;i<6;i++)nz|=mac[i];if(!nz)return GWD_ARGUMENT;
    /* Peer unmap alone does not flush the REO descriptor cache. */
    if(s->queues || s->reclaim_phase || s->next_queue_bank)return GWD_NOT_READY;
    gwd_rate_reset(s);
    s->vdev=vdev;s->pmac=pmac;s->pdev=pdev;s->encrypted=(unsigned)encrypted;s->authorized=0;s->queues=0;
    gwd_zero(s->key_valid,sizeof(s->key_valid));gwd_zero(s->pn,sizeof(s->pn));gwq_copy(s->bssid,mac,6);
    s->peer_selected=1;return 0;
}
static inline int gwd_receive_htt(struct gwd_state *s,const struct gwd_io *io,unsigned endpoint,const void *v,size_t n) {
    const uint8_t *p=(const uint8_t *)v;if(!s || !io || !p || n<4 || endpoint!=s->endpoint)return GWD_ARGUMENT;
    uint32_t word=gwq_u32(p);unsigned type=word&255;
    if(type==0) {
        if(((word>>16)&255)!=3)return gwd_fail(s,GWD_FIRMWARE);s->version=word;return 0;
    }
    if(type==3 || type==0x1e || type==0x2b) {
        if(n<(type==3?12u:16u))return gwd_fail(s,GWD_FRAME);
        unsigned vdev=(word>>8)&255,peer=word>>16;uint8_t mac[6];gwq_copy(mac,p+4,4);gwq_copy(mac+4,p+8,2);
        if(!s->peer_selected || vdev!=s->vdev || !gwd_same(mac,s->bssid))return 0;
        if(s->peer_mapped && s->peer_id!=peer)return gwd_fail(s,GWD_FIRMWARE);
        s->peer_id=peer;s->peer_mapped=1;s->ast_index=type==0x1e?gwq_u16(p+10):type==0x2b?peer:0;
        s->ast_hash=type==0x1e?gwq_u16(p+12):0;
        if(io->peer)io->peer(io->context,vdev,mac,peer,1);return 0;
    }
    if(type==4 || type==0x1f) {
        if(s->peer_mapped && s->peer_id==word>>16) {
            s->peer_mapped=s->peer_selected=s->authorized=0;
            gwd_rate_reset(s);
            gwd_zero(s->key_valid,sizeof(s->key_valid));gwd_zero(s->pn,sizeof(s->pn));
            if(io->peer)io->peer(io->context,s->vdev,s->bssid,s->peer_id,0);
        }
    }
    return 0;
}
static inline int gwd_replenish(struct gwd_state *s,const struct gwd_io *io,unsigned slot) {
    struct gwd_ring *r=&s->ring[GWD_REFILL];unsigned remote=0;
    if(slot>=GWD_RX_COUNT || s->rx_owned[slot] || gwd_ring_remote(s,io,r,&remote))return gwd_fail(s,GWD_COOKIE);
    unsigned consumed=(remote-r->tail)&(r->count-1),pending=(r->head-r->tail)&(r->count-1);
    if(consumed>pending)return gwd_fail(s,GWD_POINTER);r->tail=remote;
    unsigned next=(r->head+1)&(r->count-1);if(next==remote)return GWD_FULL;
    /* Reuse a generation only after a real DMA completion returned this slot.
     * The hardware cookie is 20 bits; exhausting it must not stop networking. */
    uint32_t cookie=(s->rx_cookie[slot]+GWD_RX_COUNT)&0xfffff;
    s->rx_cookie[slot]=cookie;s->rx_owned[slot]=1;
    uint8_t *desc=io->memory+r->desc+r->head*8;gwd_address(desc,io->dma+GWD_RX_BUFFERS+slot*GWD_BUFFER,6,cookie);
    io->sync(io->context,GWD_RX_BUFFERS+slot*GWD_BUFFER,GWD_BUFFER,1);
    io->sync(io->context,r->desc+r->head*8,8,1);r->head=next;gwd_ring_publish(io,r);return 0;
}
static inline int gwd_start(struct gwd_state *s,const struct gwd_io *io) {
    if(!s || !gwd_valid(io) || !s->prepared || !s->endpoint || s->error || s->active)return GWD_ARGUMENT;
    uint8_t message[68]={0};s->stage=2;if(gwd_wire(s,io,s->endpoint,message,4))return (int)s->error;
    for(unsigned i=0;i<3000 && !s->version && !s->error;i++)if(io->pump(io->context,1000))return gwd_fail(s,GWD_TRANSPORT);
    if(!s->version)return gwd_fail(s,GWD_TIMEOUT);
    s->stage=3;
    for(unsigned i=GWD_REFILL;i<=GWD_RXDMA_ERROR;i++) {
        struct gwd_ring *r=&s->ring[i];gwd_zero(message,sizeof(message));
        unsigned type=i==GWD_REFILL?2:i==GWD_RXDMA_ERROR?0:1;
        unsigned ring=i==GWD_REFILL?5:i==GWD_RXDMA_ERROR?7:0;
        unsigned mac=i==GWD_REFILL?0:i==GWD_MAC1?2:1;
        gwq_w32(message,0xb|(mac<<8)|(ring<<16)|(type<<24));gwq_w64(message+4,io->dma+r->desc);
        gwq_w32(message+12,r->count*r->entry|(r->entry<<16)|(type==1?1u<<25:0));
        gwq_w64(message+16,io->dma+(r->dst?r->remote:r->host));gwq_w64(message+24,io->dma+(r->dst?r->host:r->remote));
        if(gwd_wire(s,io,s->endpoint,message,52))return (int)s->error;s->firmware_rings=1;r->active=1;
    }
    for(unsigned mac=1;mac<=2;mac++) {
        gwd_zero(message,sizeof(message));gwq_w32(message,0xc|(mac<<8)|(1u<<26));gwq_w32(message+4,GWD_BUFFER);
        gwq_w32(message+16,1u<<24);gwq_w32(message+20,(1u<<18)|(1u<<21)|(1u<<24));gwq_w32(message+24,0xd);
        gwq_w32(message+28,GWD_RX_DESC_BYTES|(272u<<16));gwq_w32(message+32,144u<<16);gwq_w32(message+36,0);
        if(gwd_wire(s,io,s->endpoint,message,sizeof(message)))return (int)s->error;
    }
    for(unsigned i=0;i<GWD_RX_COUNT;i++)s->rx_cookie[i]=i;
    for(unsigned i=0;i<GWD_RX_COUNT-1;i++)if(gwd_replenish(s,io,i))return gwd_fail(s,GWD_DMA);
    s->active=1;s->stage=4;return 0;
}
static inline int gwd_reo_queue(struct gwd_state *s,const struct gwd_io *io,unsigned tid,uint64_t *dma) {
    if(!s || !gwd_valid(io) || !dma || !s->active || !s->peer_mapped || tid>16)return GWD_ARGUMENT;
    if(!s->queues) {
        if(s->next_queue_bank>=GWD_QUEUE_BANKS)return GWD_NOT_READY;
        s->queue_bank=s->next_queue_bank++;
    }
    size_t offset=GWD_QUEUES+tid*1024;
    uint8_t *p=io->memory+offset;
    if(!(s->queues&(1u<<tid))) {
        unsigned ac=tid==1||tid==2?0:tid==4||tid==5?2:tid==6||tid==7?3:1;
        gwd_zero(p,1024);gwq_w32(p,0xddbeef84);gwq_w32(p+4,tid);
        gwq_w32(p+8,3|(ac<<5)|(tid==16?1u<<8:1u<<11)|(1u<<27));
        if(tid!=16)for(unsigned i=0;i<3;i++)gwq_w32(p+128+i*128,((0xadbeefu+(i<<20))<<8)|0x94);
        io->sync(io->context,offset,1024,1);s->queues|=1u<<tid;
    }
    *dma=io->dma+offset;return 0;
}
/* Software replay tracking is per key and TID. Install only after the real
 * firmware key completion; the caller never sends plaintext normal traffic. */
static inline int gwd_key_ready(struct gwd_state *s,unsigned key,int pairwise,uint64_t rsc) {
    if(!s || !s->peer_mapped || !s->encrypted || key>3 || (pairwise!=0 && pairwise!=1) || rsc>>48)return GWD_ARGUMENT;
    unsigned index=pairwise?4:key;s->key_valid[index]=1;for(unsigned i=0;i<17;i++)s->pn[index][i]=rsc;return 0;
}
static inline int gwd_authorize(struct gwd_state *s,int on) {
    if(!s || (on!=0 && on!=1))return GWD_ARGUMENT;
    if(on && (!s->active || !s->peer_mapped || s->queues!=0x1ffff ||
       (s->encrypted && (!s->key_valid[4] || !(s->key_valid[0]|s->key_valid[1]|s->key_valid[2]|s->key_valid[3])))))return GWD_NOT_READY;
    s->authorized=(unsigned)on;return 0;
}
static inline int gwd_send(struct gwd_state *s,const struct gwd_io *io,const void *v,size_t n,uint32_t token) {
    const uint8_t *frame=(const uint8_t *)v;
    if(!s || !gwd_valid(io) || !frame || n<14 || n>1518)return GWD_ARGUMENT;
    int eapol=frame[12]==0x88 && frame[13]==0x8e;
    if(!s->active || s->error || !s->peer_mapped || s->queues!=0x1ffff || (!s->authorized && !eapol))return GWD_NOT_READY;
    struct gwd_ring *r=&s->ring[GWD_TCL];unsigned tail=0;if(gwd_ring_remote(s,io,r,&tail))return (int)s->error;
    unsigned consumed=(tail-r->tail)&(r->count-1),pending=(r->head-r->tail)&(r->count-1);
    if(consumed>pending)return gwd_fail(s,GWD_POINTER);r->tail=tail;
    unsigned next=(r->head+1)&(r->count-1);if(next==tail)return GWD_FULL;
    unsigned slot;for(slot=0;slot<GWD_TX_COUNT;slot++)if(!s->tx_owned[slot])break;if(slot==GWD_TX_COUNT)return GWD_FULL;
    uint32_t cookie=(s->tx_cookie[slot]?s->tx_cookie[slot]+GWD_TX_COUNT:GWD_TX_COUNT+slot)&0xfffff;
    size_t buffer=GWD_TX_BUFFERS+slot*GWD_BUFFER,descriptor=r->desc+r->head*32;
    size_t bytes=n;uint8_t *payload=io->memory+buffer;
    if(eapol) {
        /* Native Wi-Fi preserves FC.Protected=0. Ethernet encapsulation would
         * choose encryption from the now-installed peer key for M4. Native
         * packets omit QoS control, as in ath12k_dp_tx_encap_nwifi(). */
        static const uint8_t snap[8]={0xaa,0xaa,3,0,0,0,0x88,0x8e};
        gwd_zero(payload,24);gwq_w16(payload,0x0108); /* Data, ToDS */
        gwq_copy(payload+4,s->bssid,6);gwq_copy(payload+10,frame+6,6);gwq_copy(payload+16,frame,6);
        gwq_copy(payload+24,snap,8);gwq_copy(payload+32,frame+14,n-14);bytes=n+18;
    } else gwq_copy(payload,frame,n);
    uint8_t *d=io->memory+descriptor;gwd_zero(d,32);
    gwd_address(d,io->dma+buffer,5,cookie);
    if(eapol)gwq_w32(d+8,1u<<2); /* HAL_TCL_DATA_CMD_INFO0_BANK_ID = 1 */
    gwq_w32(d+12,(1|(s->vdev<<2)|(s->pdev<<10))<<16);
    gwq_w32(d+16,(uint32_t)bytes);gwq_w32(d+20,1|(s->pmac<<9)|(s->vdev<<24));
    gwq_w32(d+24,(s->ast_index&0xfffff)|((s->ast_hash&15)<<20));
    s->tx_cookie[slot]=cookie;s->tx_token[slot]=token;s->tx_owned[slot]=1;s->tx_pending++;
    io->sync(io->context,buffer,bytes,1);io->sync(io->context,descriptor,32,1);r->head=next;gwd_ring_publish(io,r);return 0;
}
static inline int gwd_rx_frame(struct gwd_state *s,const struct gwd_io *io,unsigned slot,const uint8_t *desc) {
    uint32_t msdu=gwq_u32(desc+16);unsigned length=(msdu>>3)&0x3fff;
    if((msdu&7)!=3 || (msdu&(1u<<17)) || length<14 || length>1518)return 0;
    size_t offset=GWD_RX_BUFFERS+slot*GWD_BUFFER;io->sync(io->context,offset,GWD_BUFFER,0);
    const uint8_t *p=io->memory+offset;unsigned pad=(gwq_u16(p+50)>>10)&3;
    unsigned reported=gwq_u32(p+80)&0x3fff,decap=(gwq_u32(p+84)>>8)&3;
    uint32_t errors=gwq_u32(p+128),done=gwq_u32(p+132),mpdu=gwq_u32(p+196);
    if(((gwq_u32(p)>>1)&0x1ff)!=197 || ((gwq_u32(p+144)>>1)&0x1ff)!=194 ||
       !(done&0x80000000) || (errors&0xfcc31380u) || decap!=2 || reported!=length ||
       length>GWD_BUFFER-GWD_RX_DESC_BYTES-pad || !s->peer_mapped ||
       (mpdu&9)!=9 || (mpdu&0x7c00) || /* valid FC/ADDR2, no fragments */
       gwq_u16(p+194)!=s->peer_id || !gwd_same(p+218,s->bssid))return 0;
    const uint8_t *frame=p+GWD_RX_DESC_BYTES+pad;int eapol=frame[12]==0x88 && frame[13]==0x8e;
    if(!s->authorized && !eapol)return 0;
    if(s->encrypted && (mpdu&(1u<<18))) {
        /* WCN7850 hal.c reconstructs the IV as 0x20 | (KEY_ID << 6):
         * descriptor KEY_ID already contains the GTK index in its low bits.
         * Its MSDU_END MCBC flag describes the protected addr1/RA; Ethernet
         * DA may differ after decapsulation and cannot select the cipher key. */
        unsigned tid=(gwq_u32(p+180)>>15)&15,group=(errors>>2)&1,key=group?(gwq_u32(p+200)&3):4;
        if(!(mpdu&(1u<<7)))tid=16;
        uint64_t pn=(uint64_t)gwq_u32(p+164)|((uint64_t)(gwq_u32(p+168)&0xffff)<<32);
        if(!(mpdu&(1u<<9)) || ((gwq_u32(p+180)>>2)&15)!=6 || ((done>>10)&7) ||
           !s->key_valid[key] || pn<=s->pn[key][tid])return 0;
        s->pn[key][tid]=pn;
    } else if(s->encrypted && !eapol)return 0;
    uint32_t rate=gwq_u32(p+96); /* WCN7850 MSDU_END.info12, not PPDU timestamp */
    if(io->receive(io->context,frame,length))return 0;
    gwd_rate_observe(&s->rx_rate,io,rate,(rate>>8)&15,(rate>>14)&15,(rate>>18)&7);
    return 1;
}
static inline int gwd_rx_slot(struct gwd_state *s,const struct gwd_io *io,const uint8_t *d,unsigned *slot) {
    uint32_t hi=gwq_u32(d+4),cookie=hi>>12;*slot=cookie&(GWD_RX_COUNT-1);
    if((hi&0xfff)!=0x600 || !s->rx_owned[*slot] || s->rx_cookie[*slot]!=cookie ||
       gwq_u32(d)!=(uint32_t)(io->dma+GWD_RX_BUFFERS+*slot*GWD_BUFFER))return gwd_fail(s,GWD_COOKIE);
    return 0;
}
/* Error frames do not stop the interface. Validate the entire bounded link
 * chain before releasing any member. Firmware addresses are used only as
 * checked offsets into the driver-owned pool. Unknown ownership is fatal. */
static inline int gwd_drop_link(struct gwd_state *s,const struct gwd_io *io,const uint8_t *address) {
    uint8_t links[16][8];unsigned slots[16*6],nlinks=0,nslots=0;
    uint8_t next[8];gwq_copy(next,address,8);
    do {
        uint32_t lo=gwq_u32(next),hi=gwq_u32(next+4),index=hi>>12;
        if(nlinks==16 || index>=1023 || (hi&0xfff)!=0x100 ||
           lo!=(uint32_t)(io->dma+GWD_LINK_DESC+index*128))return gwd_fail(s,GWD_COOKIE);
        for(unsigned i=0;i<nlinks;i++)if(gwq_u32(links[i])==lo)return gwd_fail(s,GWD_COOKIE);
        gwq_copy(links[nlinks++],next,8);
        size_t offset=GWD_LINK_DESC+index*128;io->sync(io->context,offset,128,0);
        const uint8_t *p=io->memory+offset;
        for(unsigned i=0;i<6;i++) {
            const uint8_t *msdu=p+32+i*16;if(!gwq_u32(msdu) && !(gwq_u32(msdu+4)&255))break;
            unsigned slot;if(gwd_rx_slot(s,io,msdu,&slot))return (int)s->error;
            for(unsigned j=0;j<nslots;j++)if(slots[j]==slot)return gwd_fail(s,GWD_COOKIE);
            slots[nslots++]=slot;
        }
        gwq_copy(next,p+4,8);
    } while(gwq_u32(next) || (gwq_u32(next+4)&255));
    struct gwd_ring *r=&s->ring[GWD_LINK_RELEASE];unsigned remote;
    if(gwd_ring_remote(s,io,r,&remote))return (int)s->error;
    if(((remote-r->tail)&(r->count-1))>((r->head-r->tail)&(r->count-1)))return gwd_fail(s,GWD_POINTER);
    r->tail=remote;if(((remote-r->head-1)&(r->count-1))<nlinks)return GWD_FULL;
    for(unsigned i=0;i<nlinks;i++) {
        size_t at=r->desc+r->head*32;uint8_t *d=io->memory+at;gwd_zero(d,32);gwq_copy(d,links[i],8);
        gwq_w32(d+8,4|(1u<<6)); /* SW, PUT_IN_IDLE, MSDU_LINK */
        io->sync(io->context,at,32,1);r->head=(r->head+1)&(r->count-1);
    }
    gwd_ring_publish(io,r);
    for(unsigned i=0;i<nslots;i++){s->rx_owned[slots[i]]=0;s->rx_dropped++;}
    return 0;
}
/* REO owns a queue after WMI RX_REORDER_QUEUE_SETUP. Firmware peer deletion
 * and HTT unmap stop new references; neither invalidates cached descriptors.
 * Follow Linux dp_rx peer_tid_delete/cache_flush: invalidate VLD with a status,
 * age for 1000ms, then flush extensions in reverse order and finally the base.
 * Request a status for EVERY cache line so partial failure cannot release DMA.
 * One command is outstanding, with a changing 16-bit ID across ring wraps. */
static inline int gwd_reo_command(struct gwd_state *s,const struct gwd_io *io,unsigned tag,uint64_t address) {
    struct gwd_ring *r=&s->ring[GWD_REO_COMMAND];unsigned tail;
    if(s->reo_pending || (tag!=339 && tag!=283))return gwd_fail(s,GWD_ARGUMENT);
    if(gwd_ring_remote(s,io,r,&tail))return (int)s->error;
    if(((tail-r->tail)&(r->count-1))>((r->head-r->tail)&(r->count-1)))return gwd_fail(s,GWD_POINTER);
    r->tail=tail;unsigned next=(r->head+1)&(r->count-1);if(next==tail)return GWD_FULL;
    size_t at=r->desc+r->head*r->entry*4;uint8_t *p=io->memory+at;gwd_zero(p,r->entry*4);
    s->reo_cmd_seq=(s->reo_cmd_seq+1)&0xffff;if(!s->reo_cmd_seq)s->reo_cmd_seq=1;
    gwq_w64(p,(tag<<1)|(36u<<10)); /* hal_tlv_64_hdr + 36-byte command */
    gwq_w32(p+8,s->reo_cmd_seq|(1u<<16));gwq_w32(p+12,(uint32_t)address);
    gwq_w32(p+16,(uint32_t)(address>>32)|(tag==339?1u<<9:0)); /* UPDATE_VLD=1, VLD=0 */
    s->reo_pending=s->reo_cmd_seq;s->reo_expected=tag==339?172:287;
    s->reo_consumed=next;s->reo_ack=0;s->reo_since=io->now_ms(io->context);
    io->sync(io->context,at,r->entry*4,1);r->head=next;gwd_ring_publish(io,r);return 0;
}
static inline int gwd_reo_progress(struct gwd_state *s,const struct gwd_io *io) {
    struct gwd_ring *r=&s->ring[GWD_REO_STATUS];unsigned head;
    if(gwd_ring_remote(s,io,r,&head))return (int)s->error;
    while(r->tail!=head) {
        size_t at=r->desc+r->tail*r->entry*4;io->sync(io->context,at,r->entry*4,0);
        const uint8_t *p=io->memory+at;uint32_t tlv=gwq_u32(p),status=gwq_u32(p+8);
        unsigned tag=(tlv>>1)&0x1ff,length=(tlv>>10)&0xffff;
        if(!s->reo_pending || s->reo_ack || tag!=s->reo_expected ||
           (status&0xffff)!=s->reo_pending || length<8 || length>r->entry*4-8 ||
           ((status>>26)&3))return gwd_fail(s,GWD_FIRMWARE);
        /* FLUSH_ERR busy/locked is failure even when the uniform status says
         * SUCCESS. BLOCK_ERR_CODE is meaningful only when IS_ERR is set;
         * otherwise its detail bits cannot turn a successful flush into an
         * error. A cache miss with no error is a successful invalidation. */
        if(tag==287 && (length<100 || (gwq_u32(p+16)&0x30001)))return gwd_fail(s,GWD_FIRMWARE);
        s->reo_ack=1;r->tail=(r->tail+1)&(r->count-1);gwd_ring_publish(io,r);
    }
    uint64_t now=io->now_ms(io->context);
    if(s->reo_pending) {
        if(now-s->reo_since>=GWD_REO_TIMEOUT_MS)return gwd_fail(s,GWD_TIMEOUT);
        struct gwd_ring *cmd=&s->ring[GWD_REO_COMMAND];unsigned tail;
        if(gwd_ring_remote(s,io,cmd,&tail))return (int)s->error;
        if(((tail-cmd->tail)&(cmd->count-1))>((cmd->head-cmd->tail)&(cmd->count-1)))return gwd_fail(s,GWD_POINTER);
        cmd->tail=tail;
        /* Status and source-tail DMA writes may become visible separately. */
        if(!s->reo_ack || tail!=s->reo_consumed)return 0;
        s->reo_pending=s->reo_ack=0;s->reclaim_since=now;
        if(s->reclaim_phase==GWD_RECLAIM_INVALIDATE)s->reclaim_tid++;
        else if(s->reclaim_phase==GWD_RECLAIM_FLUSH) {
            if(s->reclaim_part)s->reclaim_part--;
            else {s->reclaim_tid++;s->reclaim_part=s->reclaim_tid<16?3:0;}
        } else return gwd_fail(s,GWD_FIRMWARE);
    }
    if(s->reclaim_phase==GWD_RECLAIM_TX) {
        if(s->tx_pending) {
            if(now-s->reclaim_since>=GWD_REO_TIMEOUT_MS)return gwd_fail(s,GWD_TIMEOUT);
            return 0;
        }
        s->reclaim_phase=GWD_RECLAIM_INVALIDATE;s->reclaim_tid=0;s->reclaim_since=now;
    }
    if(s->reclaim_phase==GWD_RECLAIM_INVALIDATE || s->reclaim_phase==GWD_RECLAIM_FLUSH) {
        while(s->reclaim_tid<17 && !(s->queues&(1u<<s->reclaim_tid))) {
            s->reclaim_tid++;s->reclaim_part=s->reclaim_tid<16?3:0;
        }
        if(s->reclaim_tid<17) {
            unsigned tag=s->reclaim_phase==GWD_RECLAIM_INVALIDATE?339:283;
            uint64_t address=io->dma+GWD_QUEUES+s->reclaim_tid*1024+
                (tag==283?s->reclaim_part*128:0);
            int error=gwd_reo_command(s,io,tag,address);
            if(error==GWD_FULL) {
                if(now-s->reclaim_since>=GWD_REO_TIMEOUT_MS)return gwd_fail(s,GWD_TIMEOUT);
                return 0;
            }
            return error;
        }
        s->reclaim_phase=s->reclaim_phase==GWD_RECLAIM_INVALIDATE?GWD_RECLAIM_AGE:GWD_RECLAIM_DRAIN;
        s->reclaim_since=now;
    }
    if(s->reclaim_phase==GWD_RECLAIM_AGE && now-s->reclaim_since>=GWD_REO_AGE_MS) {
        s->reclaim_phase=GWD_RECLAIM_FLUSH;s->reclaim_tid=0;s->reclaim_part=3;s->reclaim_since=now;
        return 0;
    }
    if(s->reclaim_phase==GWD_RECLAIM_DRAIN) {
        int drained=!s->tx_pending;
        for(unsigned i=GWD_TX_COMPLETE;i<=GWD_REO_ERROR;i++)if(s->ring[i].dst) {
            unsigned producer;if(gwd_ring_remote(s,io,&s->ring[i],&producer))return (int)s->error;
            if(producer!=s->ring[i].tail)drained=0;
        }
        struct gwd_ring *release=&s->ring[GWD_LINK_RELEASE];unsigned consumed;
        if(gwd_ring_remote(s,io,release,&consumed))return (int)s->error;
        if(((consumed-release->tail)&(release->count-1))>((release->head-release->tail)&(release->count-1)))return gwd_fail(s,GWD_POINTER);
        release->tail=consumed;if(consumed!=release->head)drained=0;
        if(!drained) {
            if(now-s->reclaim_since>=GWD_REO_TIMEOUT_MS)return gwd_fail(s,GWD_TIMEOUT);
            return 0;
        }
        /* No queue memory is cleared here. Only successful hardware statuses
         * grant the next association permission to initialize it again. */
        s->queues=s->queue_bank=s->next_queue_bank=0;s->reclaim_phase=GWD_RECLAIM_IDLE;
    }
    return 0;
}
/* Call only after WMI peer-delete response AND HTT unmap were both observed.
 * peer_deleted is the station state machine's explicit confirmation, not a
 * timeout substitute. Continue gwd_poll while this returns GWD_NOT_READY. */
static inline int gwd_reo_reclaim(struct gwd_state *s,const struct gwd_io *io,unsigned peer_deleted) {
    if(!s || !gwd_valid(io) || peer_deleted>1)return GWD_ARGUMENT;
    if(s->error)return (int)s->error;
    if(!s->active || !peer_deleted || s->peer_mapped || s->authorized)return GWD_NOT_READY;
    if(!s->reclaim_phase && (s->queues || s->tx_pending)) {
        s->reclaim_phase=GWD_RECLAIM_TX;s->reclaim_since=io->now_ms(io->context);
    }
    s->peer_selected=0;
    if(!s->reclaim_phase) {
        /* Even a failed association with no installed REO queue may have
         * old RX/error completions. Do not let those cross the next peer. */
        s->reclaim_phase=GWD_RECLAIM_DRAIN;s->reclaim_since=io->now_ms(io->context);
        int error=gwd_reo_progress(s,io);if(error)return error;
    }
    return s->reclaim_phase?GWD_NOT_READY:0;
}
static inline int gwd_poll(struct gwd_state *s,const struct gwd_io *io,unsigned budget) {
    if(!s || !gwd_valid(io) || !s->active || s->error || !budget || budget>1024)return GWD_ARGUMENT;
    for(unsigned which=GWD_TX_COMPLETE;which<=GWD_REO;which++) {
        struct gwd_ring *r=&s->ring[which];unsigned head=0;if(gwd_ring_remote(s,io,r,&head))return (int)s->error;
        for(unsigned count=0;r->tail!=head && count<budget;count++) {
            size_t at=r->desc+r->tail*32;io->sync(io->context,at,32,0);const uint8_t *d=io->memory+at;
            uint32_t cookie=gwq_u32(d+4)>>12;
            if(which==GWD_TX_COMPLETE) {
                unsigned slot=cookie&(GWD_TX_COUNT-1);uint32_t info=gwq_u32(d+8);
                if((info&0xc00001c0) || ((info&7)!=0 && (info&7)!=3) ||
                   (gwq_u32(d+4)&0xfff)!=0x500 ||
                   gwq_u32(d)!=(uint32_t)(io->dma+GWD_TX_BUFFERS+slot*GWD_BUFFER) ||
                   !s->tx_owned[slot] || s->tx_cookie[slot]!=cookie)return gwd_fail(s,GWD_COOKIE);
                /* dp_tx.c frees FW buffers only for OK/DROP/TTL/REINJ/INSPECT
                 * (0..4). MEC_NOTIFY (5) is a WDS event; unknown statuses do
                 * not establish DMA completion, even with a matching cookie. */
                if((info&7)==3 && ((info>>13)&15)>4)return gwd_fail(s,GWD_FIRMWARE);
                /* TQM ACKED(0) and REMOVE_TX(2) establish an actual transmit.
                 * FW completions overlay these words with HTT data. VALID
                 * qualifies rate_stats; drops/unsent buffers cannot set rate. */
                if((info&7)==0 && (((info>>13)&15)==0 || ((info>>13)&15)==2) && s->peer_mapped) {
                    uint32_t rate=gwq_u32(d+20);
                    if(rate&1)gwd_rate_observe(&s->tx_rate,io,rate,(rate>>4)&15,(rate>>12)&15,(rate>>1)&7);
                }
                s->tx_owned[slot]=0;s->tx_pending--;s->tx_packets++;
                if(io->complete)io->complete(io->context,s->tx_token[slot],(info>>13)&15);
            } else {
                unsigned slot;uint32_t info=gwq_u32(d+28);
                if((info&1) || ((info>>1)&3)>1)return gwd_fail(s,GWD_FRAME);
                if(gwd_rx_slot(s,io,d,&slot))return (int)s->error;
                if(((info>>1)&3)==1 && !(info&0x1000) && gwd_rx_frame(s,io,slot,d))s->rx_packets++;else s->rx_dropped++;
                s->rx_owned[slot]=0;
            }
            r->tail=(r->tail+1)&(r->count-1);gwd_ring_publish(io,r);
            if(which==GWD_REO) {
                int error=gwd_replenish(s,io,cookie&(GWD_RX_COUNT-1));if(error && error!=GWD_FULL)return (int)s->error;
            }
        }
    }
    for(unsigned i=GWD_RXDMA_ERROR;i<=GWD_REO_ERROR;i++) {
        struct gwd_ring *r=&s->ring[i];unsigned head=0;if(gwd_ring_remote(s,io,r,&head))return (int)s->error;
        for(unsigned count=0;r->tail!=head && count<budget;count++) {
            size_t at=r->desc+r->tail*32;io->sync(io->context,at,32,0);const uint8_t *d=io->memory+at;
            if(i==GWD_RX_ERROR) {
                uint32_t flags=gwq_u32(d+8);unsigned slot;
                if((flags&0x800101c0) || ((flags&7)!=1 && (flags&7)!=2))return gwd_fail(s,GWD_FRAME);
                if(gwd_rx_slot(s,io,d,&slot))return (int)s->error;
                s->rx_owned[slot]=0;s->rx_dropped++;
            } else {
                uint32_t flags=gwq_u32(d+(i==GWD_REO_ERROR?28:24));
                if(i==GWD_REO_ERROR?(!(flags&1) || ((flags>>1)&3)>1):((flags&3)>2))return gwd_fail(s,GWD_FRAME);
                int error=gwd_drop_link(s,io,d);if(error==GWD_FULL)break;if(error)return error;
            }
            r->tail=(r->tail+1)&(r->count-1);gwd_ring_publish(io,r);
        }
    }
    /* Refill can lag a completion. Keep unposted slots host-owned until the
     * firmware publishes space, instead of treating backpressure as damage. */
    for(unsigned i=0;i<GWD_RX_COUNT;i++)if(!s->rx_owned[i]) {
        int error=gwd_replenish(s,io,i);if(error==GWD_FULL)break;if(error)return error;
    }
    return gwd_reo_progress(s,io);
}
static inline int gwd_stop(struct gwd_state *s,const struct gwd_io *io) {
    if(!s || !gwd_valid(io))return 0;s->active=s->authorized=0;
    gwd_rate_reset(s);
    for(unsigned i=GWD_RINGS;i>0;i--) {
        struct gwd_ring *r=&s->ring[i-1];if(!r->active || r->lmac)continue;
        io->write(io->context,r->reg+0x10,0);
        if(io->read(io->context,r->reg+0x10)&0x40){s->quarantine=1;return 0;}
        r->active=0;
    }
    /* LMAC rings are firmware-owned and have no host MMIO stop register. */
    if(!io->idle(io->context)){s->quarantine=1;return 0;}
    while(s->saved_count) {
        struct gwd_saved *p=&s->saved[s->saved_count-1];io->write(io->context,p->address,p->value);
        uint32_t got=io->read(io->context,p->address);
        if(got==UINT32_MAX || (got&p->mask)!=p->value){s->quarantine=1;return 0;}s->saved_count--;
    }
    s->quarantine=0;s->prepared=0;s->firmware_rings=0;s->queues=0;
    s->queue_bank=s->next_queue_bank=0;s->reclaim_phase=s->reo_pending=s->reo_ack=0;return 1;
}
#endif
