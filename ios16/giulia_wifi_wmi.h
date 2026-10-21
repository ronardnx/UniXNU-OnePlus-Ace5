/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * HTC and WMI TLV passive scan client for WCN7850. Wire definitions and
 * resource defaults: Linux v6.12 ath12k/{htc.h,htc.c,wmi.h,wmi.c,hw.h}.
 * Results originate only in received beacon/probe-response management frames.
 * No data queues, association, keys or transmit probes are supplied here.
 */
#ifndef GIULIA_WIFI_WMI_H
#define GIULIA_WIFI_WMI_H
#include "giulia_wifi_qmi.h"
#define GWW_BSS_MAX 64u
#define GWW_IE_MAX 1024u
static const uint16_t gww_scan_frequencies[]={2412,2417,2422,2427,2432,2437,2442,2447,2452,2457,2462,2467,2472,
    5180,5200,5220,5240,5745,5765,5785,5805,5825};
#define GWW_SCAN_CHANNELS (sizeof(gww_scan_frequencies)/sizeof(gww_scan_frequencies[0]))
enum gww_error { GWW_OK,GWW_ARGUMENT,GWW_FRAME,GWW_CREDITS,GWW_TRANSPORT,GWW_TIMEOUT,GWW_STATE,GWW_FIRMWARE,GWW_BUSY };
struct gww_bss { uint8_t bssid[6],ssid[32],ssid_len,rates[16],rate_count;
    uint16_t frequency,beacon,capability,ie_bytes;int16_t rssi,noise;uint64_t timestamp;
    uint8_t ie[GWW_IE_MAX]; };
struct gww_io {
    void *context;int (*send)(void *,unsigned,const void *,size_t); /* 0 sent, 2 retry, other failure */
    int (*pump)(void *,unsigned);uint64_t (*now_ms)(void *);
    void (*scan_done)(void *,unsigned,uint32_t);
};
struct gww_state {
    unsigned error,htc_ready,connecting,endpoint,credits,credit_total,credit_size,max_message,seq;
    unsigned service_ready,fw_ready,initializing,vdev,chains,bss_count,scanning,scan_started,scan_status;
    uint32_t scan_id,last_event;uint64_t scan_deadline;uint8_t mac[6],tx[2048];
    struct gww_bss bss[GWW_BSS_MAX];
};
static inline int gww_fail(struct gww_state *s,unsigned e){if(!s->error)s->error=e;return (int)s->error;}
static inline void gww_tlv(void *p,unsigned tag,unsigned bytes){gwq_w32(p,(uint32_t)tag<<16|bytes);}
static inline const uint8_t *gww_field(const uint8_t *p,size_t n,unsigned wanted,size_t *size) {
    const uint8_t *found=NULL;*size=0;
    while(n){if(n<4)return NULL;unsigned z=gwq_u16(p),tag=gwq_u16(p+2);if((z&3)||z>n-4)return NULL;
        if(tag==wanted){if(found)return NULL;found=p+4;*size=z;}p+=4+z;n-=4+z;}
    return found;
}
static inline unsigned gww_channel(unsigned frequency) {
    if(frequency>=2412&&frequency<=2472&&(frequency-2412)%5==0)return 1+(frequency-2412)/5;
    if(frequency>=5180&&frequency<=5825&&(frequency-5000)%5==0)return (frequency-5000)/5;
    return 0;
}
static inline void gww_complete(struct gww_state *s,const struct gww_io *io,unsigned status) {
    if(!s->scanning)return;s->scanning=0;s->scan_status=status;
    if(io->scan_done)io->scan_done(io->context,status,s->scan_id);
}
/* Over-the-air frames are untrusted: bad IE chains cannot partially replace
 * a previously valid BSS. Hidden SSIDs remain hidden until really observed. */
static inline void gww_beacon(struct gww_state *s,const struct gww_io *io,const uint8_t *frame,size_t n,unsigned freq,int rssi) {
    if(!s->scanning || n<36 || n>36+GWW_IE_MAX || !gww_channel(freq) ||
        (frame[0]!=0x80 && frame[0]!=0x50) || (frame[1]&0x40) || (frame[16]&1))return;
    unsigned nonzero=0;for(unsigned i=16;i<22;i++)nonzero|=frame[i];if(!nonzero)return;
    unsigned ssid_seen=0,ssid_len=0,rate_count=0;const uint8_t *ssid=NULL;uint8_t rates[16]={0};
    for(size_t off=36;off<n;) {
        if(n-off<2 || frame[off+1]>n-off-2)return;unsigned id=frame[off],z=frame[off+1];const uint8_t *v=frame+off+2;
        if(id==0){if(ssid_seen++ || z>32)return;ssid_len=z;ssid=v;}
        if(id==1 || id==50)for(unsigned i=0;i<z && rate_count<16;i++)rates[rate_count++]=v[i];
        off+=2+z;
    }
    if(!ssid_seen)return;
    unsigned index=s->bss_count;
    for(unsigned i=0;i<s->bss_count;i++) {unsigned same=1;for(unsigned j=0;j<6;j++)same&=s->bss[i].bssid[j]==frame[16+j];
        if(same && s->bss[i].frequency==freq){index=i;break;} }
    if(index==GWW_BSS_MAX)return;
    if(index==s->bss_count)s->bss_count++;
    struct gww_bss *b=&s->bss[index];gwq_copy(b->bssid,frame+16,6);
    if(ssid_len || !b->ssid_len){b->ssid_len=(uint8_t)ssid_len;for(unsigned i=0;i<32;i++)b->ssid[i]=i<ssid_len?ssid[i]:0;}
    b->rate_count=(uint8_t)rate_count;gwq_copy(b->rates,rates,16);b->frequency=(uint16_t)freq;
    b->beacon=gwq_u16(frame+32);b->capability=gwq_u16(frame+34);b->rssi=(int16_t)(rssi<=0&&rssi>=-127?rssi:-127);
    b->noise=-127; /* No noise-floor measurement in this management event. */
    b->timestamp=io->now_ms(io->context);b->ie_bytes=(uint16_t)(n-36);gwq_copy(b->ie,frame+36,n-36);
}
static inline int gww_wmi_receive(struct gww_state *s,const struct gww_io *io,const uint8_t *p,size_t n) {
    if(n<4)return gww_fail(s,GWW_FRAME);unsigned event=gwq_u32(p)&0xffffff;s->last_event=event;p+=4;n-=4;
    size_t z;const uint8_t *v;
    if(event==1) {
        v=gww_field(p,n,32,&z); /* WMI_SERVICE_READY_EVENT */
        if(!v || z<76 || s->service_ready)return gww_fail(s,GWW_FRAME);
        s->chains=gwq_u32(v+36);
        if(!s->chains || s->chains>2 || gwq_u32(v+72))return gww_fail(s,GWW_FIRMWARE);
        s->service_ready=1;
    } else if(event==2) {
        v=gww_field(p,n,35,&z);
        if(!v || z<36 || !s->initializing || gwq_u32(v+32))return gww_fail(s,GWW_FIRMWARE);
        if(v[24]&1)return gww_fail(s,GWW_FIRMWARE);s->fw_ready=1;
    } else if(event==0x3001) {
        v=gww_field(p,n,36,&z);if(!v || z<24)return gww_fail(s,GWW_FRAME);
        if(!s->scanning || gwq_u32(v+16)!=s->scan_id || gwq_u32(v+12)!=0xa001 || gwq_u32(v+20)!=0)return 0;
        unsigned kind=gwq_u32(v),reason=gwq_u32(v+4);
        if(kind&1)s->scan_started=1;
        if(kind&(2|16|32|64))gww_complete(s,io,(kind&2)&&!reason?0:reason+1);
    } else if(event==0x7001) {
        v=gww_field(p,n,44,&z);if(!v || z<68)return gww_fail(s,GWW_FRAME);
        unsigned len=gwq_u32(v+16),status=gwq_u32(v+20),freq=gwq_u32(v+64);int rssi=(int32_t)gwq_u32(v+44);
        const uint8_t *bytes=gww_field(p,n,17,&z);
        if(!bytes || len>z)return gww_fail(s,GWW_FRAME);
        if(!status)gww_beacon(s,io,bytes,len,freq,rssi);
    }
    return 0;
}
static inline int gww_receive(struct gww_state *s,const struct gww_io *io,unsigned pipe,const void *v,size_t n) {
    const uint8_t *p=(const uint8_t *)v;
    if(!s || !io || !io->now_ms || !p || n<8 || n>2048 || (pipe!=1&&pipe!=2))return GWW_ARGUMENT;
    if(s->error)return (int)s->error;
    unsigned endpoint=p[0],flags=p[1],len=gwq_u16(p+2),trailer=p[4];
    if(len>n-8 || n-8-len>3 || (flags&0xf0))return gww_fail(s,GWW_FRAME);
    p+=8;
    if(flags&2) {
        if(trailer<4 || trailer>len)return gww_fail(s,GWW_FRAME);
        const uint8_t *t=p+len-trailer;size_t left=trailer;
        while(left) {
            if(left<4 || t[1]>left-4)return gww_fail(s,GWW_FRAME);
            if(t[0]==1) {
                if(t[1]%4)return gww_fail(s,GWW_FRAME);
                for(unsigned i=4;i<4u+t[1];i+=4)if(t[i]==s->endpoint && s->endpoint) {
                    if(t[i+1]>s->credit_total-s->credits)return gww_fail(s,GWW_CREDITS);s->credits+=t[i+1];
                }
            }
            unsigned used=t[1]+4;left-=used;t+=used;
        }
        len-=trailer;
    }
    if(!len)return 0;
    if(!endpoint) {
        if(len<8)return gww_fail(s,GWW_FRAME);unsigned msg=gwq_u16(p);
        if(msg==1) {
            if(s->htc_ready)return gww_fail(s,GWW_STATE);
            s->credit_total=gwq_u16(p+2);s->credit_size=gwq_u16(p+4);
            if(!s->credit_total || s->credit_total>255 || !s->credit_size || s->credit_size>2048 || p[6]<2)return gww_fail(s,GWW_CREDITS);
            s->credits=s->credit_total;s->htc_ready=1;
        } else if(msg==3) {
            if(len<12 || !s->connecting || gwq_u16(p+2)!=0x100 || p[4] || !p[5] || p[5]>8)return gww_fail(s,GWW_FIRMWARE);
            s->endpoint=p[5];s->max_message=gwq_u16(p+6);s->connecting=0;
            if(s->max_message<512)return gww_fail(s,GWW_FIRMWARE);
        }
        return 0;
    }
    if(endpoint!=s->endpoint)return 0;
    return gww_wmi_receive(s,io,p,len);
}
static inline int gww_send(struct gww_state *s,const struct gww_io *io,unsigned endpoint,const void *payload,size_t n) {
    if(s->error)return (int)s->error;
    if(!io->send || !io->pump || n>sizeof(s->tx)-8 || (endpoint && (!s->credit_size || n>s->max_message)))return GWW_ARGUMENT;
    unsigned need=endpoint?(unsigned)((n+8+s->credit_size-1)/s->credit_size):0;
    for(unsigned i=0;need>s->credits && i<1000 && !s->error;i++)if(io->pump(io->context,1000))gww_fail(s,GWW_TRANSPORT);
    if(s->error)return (int)s->error;if(need>s->credits)return gww_fail(s,GWW_TIMEOUT);
    uint8_t *b=s->tx;b[0]=(uint8_t)endpoint;b[1]=endpoint?1:0;gwq_w16(b+2,(uint16_t)n);b[4]=0;b[5]=(uint8_t)s->seq++;b[6]=b[7]=0;
    gwq_copy(b+8,payload,n);s->credits-=need;
    for(unsigned i=0;i<1000;i++) {
        int e=io->send(io->context,endpoint?3:0,b,n+8);if(!e)return 0;
        if(e!=2){s->credits+=need;return gww_fail(s,GWW_TRANSPORT);}
        if(io->pump(io->context,1000)){s->credits+=need;return gww_fail(s,GWW_TRANSPORT);}
        if(s->error)return (int)s->error;
    }
    s->credits+=need;return gww_fail(s,GWW_TIMEOUT);
}
static inline int gww_wait(struct gww_state *s,const struct gww_io *io,const unsigned *flag) {
    for(unsigned i=0;i<10000 && !*flag && !s->error;i++)if(io->pump(io->context,1000))gww_fail(s,GWW_TRANSPORT);
    if(s->error)return (int)s->error;return *flag?0:gww_fail(s,GWW_TIMEOUT);
}
static inline int gww_initialize(struct gww_state *s,const struct gww_io *io,const uint8_t mac[6]) {
    if(!s || !io || !mac || (mac[0]&1) || s->vdev)return GWW_ARGUMENT;
    unsigned nonzero=0;for(unsigned i=0;i<6;i++)nonzero|=mac[i];if(!nonzero)return GWW_ARGUMENT;
    if(gww_wait(s,io,&s->htc_ready))return (int)s->error;
    uint8_t cmd[512]={0};gwq_w32(cmd,0x01000002);gwq_w32(cmd+4,s->credit_total<<8);s->connecting=1;
    if(gww_send(s,io,0,cmd,8) || gww_wait(s,io,&s->endpoint))return (int)s->error;
    gwq_w32(cmd,5);gwq_w32(cmd+4,0);gwq_w32(cmd+8,0);
    if(gww_send(s,io,0,cmd,12) || gww_wait(s,io,&s->service_ready))return (int)s->error;
    for(unsigned i=0;i<sizeof(cmd);i++)cmd[i]=0;
    gwq_w32(cmd,1);gww_tlv(cmd+4,74,28); /* ABI zero, host-memory chunk count zero */
    uint8_t *c=cmd+36;gww_tlv(c,75,288);
    const unsigned offsets[]={4,8,12,16,20,24,28,40,44,48,52,56,60,64,68,72,88,112,116,120,124,128,132,136,140,176,216,228,272};
    const unsigned values[]={4,16,3,3,2,32,16,100,100,100,40,1,4,3,3,8,1024,2,1024,10,1,8,2,32,22,32,1,1,512};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++)gwq_w32(c+offsets[i],values[i]);
    gwq_w32(c+32,(1u<<s->chains)-1);gwq_w32(c+36,(1u<<s->chains)-1);
    gww_tlv(cmd+328,18,0);s->initializing=1;
    if(gww_send(s,io,s->endpoint,cmd,332) || gww_wait(s,io,&s->fw_ready))return (int)s->error;
    for(unsigned i=0;i<sizeof(cmd);i++)cmd[i]=0;
    gwq_w32(cmd,0x5001);gww_tlv(cmd+4,86,44);gwq_w32(cmd+12,2);gwq_copy(cmd+20,mac,6);
    gwq_w32(cmd+28,2);gwq_w32(cmd+32,1);gwq_w32(cmd+48,0xff);gww_tlv(cmd+52,18,32);
    for(unsigned i=0;i<2;i++){uint8_t *p=cmd+56+i*16;gww_tlv(p,411,12);gwq_w32(p+4,i);gwq_w32(p+8,s->chains);gwq_w32(p+12,s->chains);}
    if(gww_send(s,io,s->endpoint,cmd,88))return (int)s->error;
    gwq_copy(s->mac,mac,6);s->vdev=1;return 0;
}
static inline int gww_scan(struct gww_state *s,const struct gww_io *io) {
    if(s->error || !s->vdev || !io->now_ms)return GWW_STATE;if(s->scanning)return GWW_BUSY;
    /* CN device, passive receive only; DFS/6 GHz are not requested by this
     * initial transport. Firmware's OEM regdb still decides what is allowed. */
    uint8_t cmd[280]={0};gwq_w32(cmd,0x3001);gww_tlv(cmd+4,77,156);uint8_t *p=cmd+4;
    s->scan_id=0xa000|((s->scan_id+1)&0xfff);gwq_w32(p+4,s->scan_id);gwq_w32(p+8,0xa001);
    gwq_w32(p+16,2);gwq_w32(p+20,0x7f);gwq_w32(p+24,60);gwq_w32(p+28,110);
    gwq_w32(p+32,50);gwq_w32(p+36,500);gwq_w32(p+48,50);gwq_w32(p+52,15000);
    gwq_w32(p+60,0x10001);gwq_w32(p+68,GWW_SCAN_CHANNELS);
    size_t off=164;gww_tlv(cmd+off,16,GWW_SCAN_CHANNELS*4);off+=4;
    for(unsigned i=0;i<GWW_SCAN_CHANNELS;i++,off+=4)gwq_w32(cmd+off,gww_scan_frequencies[i]);
    gww_tlv(cmd+off,19,0);off+=4;gww_tlv(cmd+off,19,0);off+=4;gww_tlv(cmd+off,17,0);off+=4;
    s->bss_count=0;for(unsigned i=0;i<GWW_BSS_MAX;i++)s->bss[i].ssid_len=0;
    s->scanning=1;s->scan_started=0;s->scan_deadline=io->now_ms(io->context)+20000;
    int e=gww_send(s,io,s->endpoint,cmd,off);if(e)gww_complete(s,io,(unsigned)e);return e;
}
static inline void gww_tick(struct gww_state *s,const struct gww_io *io) {
    if(s->scanning && (s->error || io->now_ms(io->context)>=s->scan_deadline))gww_complete(s,io,s->error?s->error:GWW_TIMEOUT);
}
#endif
