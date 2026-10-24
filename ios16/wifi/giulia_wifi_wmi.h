/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * HTC and WMI TLV transport for WCN7850. Wire definitions and
 * resource defaults: Linux v6.12 ath12k/{htc.h,htc.c,wmi.h,wmi.c,hw.h}.
 * Results originate only in received beacon/probe-response management frames.
 * Station control and data services attach through optional event/HTC hooks.
 */
#ifndef GIULIA_WIFI_WMI_H
#define GIULIA_WIFI_WMI_H
#include "giulia_wifi_qmi.h"
#include "giulia_wifi_regulatory.h"
#define GWW_BSS_MAX 64u
#define GWW_IE_MAX 1024u
static const uint16_t gww_scan_frequencies[]={2412,2417,2422,2427,2432,2437,2442,2447,2452,2457,2462,2467,2472,
    5180,5200,5220,5240,5745,5765,5785,5805,5825};
#define GWW_SCAN_CHANNELS (sizeof(gww_scan_frequencies)/sizeof(gww_scan_frequencies[0]))
#define GWW_SCAN_SSID_MAX 10u
#define GWW_SCAN_BSSID_MAX 4u
#define GWW_SCAN_FREQ_MAX 64u
struct gww_scan_ssid {uint8_t length,bytes[32];};
struct gww_scan_request {
    unsigned active,connected,home_frequency,ssid_count,bssid_count,frequency_count;
    struct gww_scan_ssid ssids[GWW_SCAN_SSID_MAX];
    uint8_t bssids[GWW_SCAN_BSSID_MAX][6];uint16_t frequencies[GWW_SCAN_FREQ_MAX];
};
enum gww_error { GWW_OK,GWW_ARGUMENT,GWW_FRAME,GWW_CREDITS,GWW_TRANSPORT,GWW_TIMEOUT,GWW_STATE,GWW_FIRMWARE,GWW_BUSY };
struct gww_bss { uint8_t bssid[6],ssid[32],ssid_len,rates[16],rate_count;
    uint16_t frequency,beacon,capability,ie_bytes;int16_t rssi,noise;uint64_t timestamp;
    uint8_t ie[GWW_IE_MAX]; };
struct gww_io {
    void *context;int (*send)(void *,unsigned,const void *,size_t); /* 0 sent, 2 retry, other failure */
    int (*pump)(void *,unsigned);uint64_t (*now_ms)(void *);
    void (*scan_done)(void *,unsigned,uint32_t);
    gwpr_select_fn protocol;
};
struct gww_state {
    unsigned error,htc_ready,connecting,endpoint,credits,credit_total,credit_size,max_message,seq;
    unsigned service_ready,fw_ready,initializing,vdev,chains,bss_count,scanning,scan_started,scan_status;
    unsigned scan_cancel_pending,scan_failure,scan_reg_generation;
    uint32_t scan_id,last_event;uint64_t scan_deadline;uint8_t mac[6],tx[2048];
    struct gww_scan_request scan_request;
    uint32_t services[12];unsigned services_valid,service_ext,service_ext2;
    /* WCN7850 exposes one logical pdev (0), even when firmware advertises
     * multiple physical radios. Keep physical IDs separate from WMI pdev. */
    unsigned hw_valid,pdev,pmac,fw_pdev,phy,bands,tx_chain_mask,rx_chain_mask;
    struct gwreg_state regulatory;
    void *event_context;void (*event)(void *,unsigned,const uint8_t *,size_t);
    void *service_context;
    int (*connect_services)(void *);
    int (*htc_control)(void *,const uint8_t *,size_t);
    int (*htc_data)(void *,unsigned,const uint8_t *,size_t);
    struct gww_bss bss[GWW_BSS_MAX];
};
static inline int gww_service(const struct gww_state *s,unsigned bit) {
    return s && s->services_valid && bit<384 && (s->services[bit/32]&(1u<<(bit%32)))!=0;
}
static inline int gww_fail(struct gww_state *s,unsigned e){if(!s->error)s->error=e;return (int)s->error;}
static inline void gww_tlv(void *p,unsigned tag,unsigned bytes){gwq_w32(p,(uint32_t)tag<<16|bytes);}
static inline const uint8_t *gww_field(const uint8_t *p,size_t n,unsigned wanted,size_t *size) {
    const uint8_t *found=NULL;*size=0;
    while(n){if(n<4)return NULL;unsigned z=gwq_u16(p),tag=gwq_u16(p+2);if(z>n-4)return NULL;
        if(tag==wanted){if(found)return NULL;found=p+4;*size=z;}p+=4+z;n-=4+z;}
    return found;
}
/* ARRAY_STRUCT may occur repeatedly. Validate the entire outer stream before
 * choosing its indexed occurrence, just as the Linux TLV iterator does. */
static inline const uint8_t *gww_array(const uint8_t *p,size_t n,unsigned index,size_t *size) {
    const uint8_t *found=NULL;*size=0;
    while(n){if(n<4)return NULL;unsigned z=gwq_u16(p),tag=gwq_u16(p+2);if(z>n-4)return NULL;
        if(tag==18 && !index--){found=p+4;*size=z;}p+=4+z;n-=4+z;}
    return found;
}
static inline int gww_hardware(struct gww_state *s,const uint8_t *p,size_t n) {
    size_t z=0;const uint8_t *v=gww_field(p,n,428,&z);
    if(!v || z<76 || s->service_ext)return gww_fail(s,GWW_FRAME);
    v=gww_field(p,n,530,&z);if(!v || z<8)return gww_fail(s,GWW_FRAME);
    unsigned modes=gwq_u32(v),single=0,count=0;
    if(!modes || modes>6)return gww_fail(s,GWW_FIRMWARE);
    const uint8_t *a=gww_array(p,n,0,&z);if(!a)return gww_fail(s,GWW_FRAME);
    while(z){if(z<16 || gwq_u16(a)<12 || gwq_u16(a)>z-4 || gwq_u16(a+2)!=529)return gww_fail(s,GWW_FRAME);
        if(!gwq_u32(a+4)){if(single++ || gwq_u32(a+8)!=1)return gww_fail(s,GWW_FIRMWARE);}
        count++;unsigned used=4+gwq_u16(a);a+=used;z-=used;}
    if(count!=modes || !single)return gww_fail(s,GWW_FIRMWARE);
    v=gww_field(p,n,532,&z);if(!v || z<4 || !gwq_u32(v) || gwq_u32(v)>3)return gww_fail(s,GWW_FRAME);
    a=gww_array(p,n,1,&z);if(!a)return gww_fail(s,GWW_FRAME);unsigned found=0;
    while(z){if(z<4 || gwq_u16(a)<212 || gwq_u16(a)>z-4 || gwq_u16(a+2)!=528)return gww_fail(s,GWW_FRAME);
        v=a+4;
        if(!gwq_u32(v)) {
            if(found++ || gwq_u32(v+8) || gwq_u32(v+196)>1)return gww_fail(s,GWW_FIRMWARE);
            unsigned bands=gwq_u32(v+16),fw_pdev=gwq_u32(v+4)&0xffff;
            unsigned tx=gwq_u32(v+((bands&1)?48:80)),rx=gwq_u32(v+((bands&1)?52:84));
            if(!(bands&3) || !fw_pdev || fw_pdev>3 || !tx || !rx || tx>3 || rx>3)return gww_fail(s,GWW_FIRMWARE);
            s->fw_pdev=fw_pdev;s->phy=0;s->bands=bands;s->tx_chain_mask=tx;s->rx_chain_mask=rx;
            /* ath12k_wmi_ext_soc_hal_reg_caps_parse + single_pdev_only;
             * WCN7850 hw_ops maps host radio index 0 to lmac 0. */
            s->pdev=0;s->pmac=0;
        }
        unsigned used=4+gwq_u16(a);a+=used;z-=used;
    }
    if(!found)return gww_fail(s,GWW_FIRMWARE);
    s->hw_valid=1;s->service_ext=1;return GWW_OK;
}
static inline unsigned gww_channel(unsigned frequency) {
    if(frequency>=2412&&frequency<=2472&&(frequency-2412)%5==0)return 1+(frequency-2412)/5;
    if(frequency>=5180&&frequency<=5825&&(frequency-5000)%5==0)return (frequency-5000)/5;
    return 0;
}
static inline void gww_complete(struct gww_state *s,const struct gww_io *io,unsigned status) {
    if(!s->scanning)return;s->scanning=0;s->scan_cancel_pending=0;s->scan_status=s->scan_failure?s->scan_failure:status;
    if(io->scan_done)io->scan_done(io->context,s->scan_status,s->scan_id);
}
static inline int gww_same_bytes(const uint8_t *a,const uint8_t *b,size_t n) {
    for(size_t i=0;i<n;i++)if(a[i]!=b[i])return 0;return 1;
}
static inline int gww_scan_match(const struct gww_scan_request *q,unsigned frequency,
    const uint8_t bssid[6],const uint8_t *ssid,unsigned length) {
    unsigned match=0;for(unsigned i=0;i<q->frequency_count;i++)match|=q->frequencies[i]==frequency;
    if(!match)return 0;
    static const uint8_t broadcast[6]={255,255,255,255,255,255};
    match=!q->bssid_count;
    for(unsigned i=0;i<q->bssid_count;i++)match|=gww_same_bytes(q->bssids[i],broadcast,6)||gww_same_bytes(q->bssids[i],bssid,6);
    if(!match)return 0;match=!q->ssid_count;
    for(unsigned i=0;i<q->ssid_count;i++)match|=!q->ssids[i].length ||
        (q->ssids[i].length==length && gww_same_bytes(q->ssids[i].bytes,ssid,length));
    return match!=0;
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
    /* APs hide the name with either a zero-length or a zero-filled SSID IE.
     * Keep the original IE bytes, but never let either form overwrite an
     * actually observed name or satisfy a directed-name filter. */
    unsigned named=0;for(unsigned i=0;i<ssid_len;i++)named|=ssid[i];
    if(!named)ssid_len=0;
    if(!ssid_seen || !gww_scan_match(&s->scan_request,freq,frame+16,ssid,ssid_len))return;
    unsigned index=s->bss_count;
    for(unsigned i=0;i<s->bss_count;i++) {unsigned same=1;for(unsigned j=0;j<6;j++)same&=s->bss[i].bssid[j]==frame[16+j];
        if(same && s->bss[i].frequency==freq){index=i;break;} }
    if(index==GWW_BSS_MAX)return;
    if(index==s->bss_count)s->bss_count++;
    struct gww_bss *b=&s->bss[index];
    /* Retain the complete actually observed named response during this scan.
     * Mixing its SSID with a later hidden beacon's IE chain creates a BSS
     * snapshot that never existed on air. The next scan clears the cache. */
    if(!ssid_len && b->ssid_len)return;
    gwq_copy(b->bssid,frame+16,6);
    if(ssid_len || !b->ssid_len){b->ssid_len=(uint8_t)ssid_len;for(unsigned i=0;i<32;i++)b->ssid[i]=i<ssid_len?ssid[i]:0;}
    b->rate_count=(uint8_t)rate_count;gwq_copy(b->rates,rates,16);b->frequency=(uint16_t)freq;
    b->beacon=gwq_u16(frame+32);b->capability=gwq_u16(frame+34);b->rssi=(int16_t)(rssi<=0&&rssi>=-127?rssi:-127);
    b->noise=-127; /* No noise-floor measurement in this management event. */
    b->timestamp=io->now_ms(io->context);b->ie_bytes=(uint16_t)(n-36);gwq_copy(b->ie,frame+36,n-36);
}
static inline int gww_wmi_receive(struct gww_state *s,const struct gww_io *io,const uint8_t *p,size_t n) {
    if(n<4)return gww_fail(s,GWW_FRAME);unsigned event=gwq_u32(p)&0xffffff;s->last_event=event;p+=4;n-=4;
    /* Unknown events retain their regulatory/station dispatch semantics.
     * Repeated WMI array tags are legal; this gate checks outer framing. */
    if(gwpr_wmi_known(event)) {
        uint64_t verdict=io->protocol?io->protocol(io->context,GWPR_WMI,event,0,p,n):
            gwpr_baseline(GWPR_WMI,event,0,p,n);
        if(gwpr_status(verdict)!=GWPR_VALID)return gww_fail(s,event==2 || (event==0x4009 && !s->service_ready)?GWW_FIRMWARE:GWW_FRAME);
    }
    size_t z;const uint8_t *v;
    if(event==1) {
        v=gww_field(p,n,32,&z); /* WMI_SERVICE_READY_EVENT */
        if(!v || z<76 || s->service_ready)return gww_fail(s,GWW_FRAME);
        s->chains=gwq_u32(v+36);
        if(!s->chains || s->chains>2 || gwq_u32(v+72))return gww_fail(s,GWW_FIRMWARE);
        /* Linux v6.12 ath12k_wmi_service_bitmap_copy: only the low four
         * bits of each 32-bit service word are meaningful. The first
         * ARRAY_UINT32 is the legacy 128-service map, not a linear bitmap. */
        const uint8_t *at=p;size_t left=n;
        while(left) {
            unsigned bytes=gwq_u16(at),tag=gwq_u16(at+2);
            if(tag==16) {
                if(bytes<128)return gww_fail(s,GWW_FRAME);
                for(unsigned i=0;i<4;i++)s->services[i]=0;
                for(unsigned i=0;i<128;i++)if(gwq_u32(at+4+4*(i/4))&(1u<<(i%4)))s->services[i/32]|=1u<<(i%32);
                s->services_valid=1;break;
            }
            at+=4+bytes;left-=4+bytes;
        }
        if(!s->services_valid)return gww_fail(s,GWW_FRAME);
        s->service_ready=1;
    } else if(event==3) {
        v=gww_field(p,n,559,&z);if(!v || z<20 || gwq_u32(v)!=128)return gww_fail(s,GWW_FRAME);
        for(unsigned i=0;i<4;i++)s->services[i+4]=gwq_u32(v+4+4*i);
        v=gww_field(p,n,16,&z);
        if(v){if(z<16)return gww_fail(s,GWW_FRAME);for(unsigned i=0;i<4;i++)s->services[i+8]=gwq_u32(v+4*i);}
    } else if(event==0x4009) {
        if(!s->service_ready || gww_hardware(s,p,n))return gww_fail(s,GWW_FIRMWARE);
    } else if(event==0x4022) {
        v=gww_field(p,n,820,&z);
        if(!s->service_ext || s->service_ext2 || !v || z<52)return gww_fail(s,GWW_FRAME);
        s->service_ext2=1;
    } else if(event==2) {
        v=gww_field(p,n,35,&z);
        if(!v || z<36 || !s->initializing || gwq_u32(v+32))return gww_fail(s,GWW_FIRMWARE);
        if(v[24]&1)return gww_fail(s,GWW_FIRMWARE);s->fw_ready=1;
    } else if(event==0x3001) {
        v=gww_field(p,n,36,&z);if(!v || z<28)return gww_fail(s,GWW_FRAME);
        if(!s->scanning || gwq_u32(v+16)!=s->scan_id || gwq_u32(v+12)!=0xa001 || gwq_u32(v+20)!=0)return 0;
        unsigned kind=gwq_u32(v),reason=gwq_u32(v+4);
        /* wmi_scan_event_type is a single enum value, not a received mask.
         * ath12k_scan_event keeps PREEMPTED/RESTARTED alive: firmware still
         * owns that scan and may resume it. A normal COMPLETED before STARTED
         * is also not proof of quiescence (firmware state-machine recovery).
         * Do not allow malformed reason arithmetic to wrap into success. */
        if(kind==1 && !s->scan_cancel_pending)s->scan_started=1;
        else if(kind==2 && (s->scan_started || s->scan_cancel_pending))
            gww_complete(s,io,!reason?GWW_OK:reason==3?GWW_TIMEOUT:GWW_FIRMWARE);
        else if(kind==16 || (kind==64 && !s->scan_started))gww_complete(s,io,GWW_FIRMWARE);
    } else if(event==0x7001) {
        v=gww_field(p,n,44,&z);if(!v || z<68)return gww_fail(s,GWW_FRAME);
        unsigned len=gwq_u32(v+16),status=gwq_u32(v+20),freq=gwq_u32(v+64);int rssi=(int32_t)gwq_u32(v+44);
        const uint8_t *bytes=gww_field(p,n,17,&z);
        if(!bytes || len>z)return gww_fail(s,GWW_FRAME);
        if(!status)gww_beacon(s,io,bytes,len,freq,rssi);
    }
    if(event==0x3a001 || event==0x3a003)(void)gwreg_receive(&s->regulatory,event,p,n);
    if(s->event)s->event(s->event_context,event,p,n);
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
            if(len<12)return gww_fail(s,GWW_FRAME);
            if(gwq_u16(p+2)!=0x100) {
                if(s->htc_control && s->htc_control(s->service_context,p,len))return gww_fail(s,GWW_TRANSPORT);
                return 0;
            }
            if(!s->connecting || p[4] || !p[5] || p[5]>8)return gww_fail(s,GWW_FIRMWARE);
            s->endpoint=p[5];s->max_message=gwq_u16(p+6);s->connecting=0;
            if(s->max_message<512)return gww_fail(s,GWW_FIRMWARE);
        }
        return 0;
    }
    if(endpoint!=s->endpoint) {
        if(s->htc_data && s->htc_data(s->service_context,endpoint,p,len))return gww_fail(s,GWW_TRANSPORT);
        return 0;
    }
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
    if(s->connect_services && s->connect_services(s->service_context))return gww_fail(s,GWW_TRANSPORT);
    gwq_w32(cmd,5);gwq_w32(cmd+4,0);gwq_w32(cmd+8,0);
    if(gww_send(s,io,0,cmd,12) || gww_wait(s,io,&s->service_ready))return (int)s->error;
    if(gww_wait(s,io,&s->service_ext) || (gww_service(s,220) && gww_wait(s,io,&s->service_ext2)))return (int)s->error;
    for(unsigned i=0;i<sizeof(cmd);i++)cmd[i]=0;
    gwq_w32(cmd,1);gww_tlv(cmd+4,74,28); /* ABI zero, host-memory chunk count zero */
    uint8_t *c=cmd+36;gww_tlv(c,75,288);
    const unsigned offsets[]={4,8,12,16,20,24,28,40,44,48,52,56,60,64,68,72,88,112,116,120,124,128,132,136,140,176,216,228,272};
    const unsigned values[]={4,16,3,3,2,32,16,100,100,100,40,1,4,3,3,8,1024,2,1024,10,1,8,2,32,22,32,1,1,512};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++)gwq_w32(c+offsets[i],values[i]);
    gwq_w32(c+32,s->tx_chain_mask);gwq_w32(c+36,s->rx_chain_mask);
    gwq_w32(c+276,gww_service(s,281)?16:0);
    gww_tlv(cmd+328,18,0);s->initializing=1;
    if(gww_send(s,io,s->endpoint,cmd,332) || gww_wait(s,io,&s->fw_ready))return (int)s->error;
    for(unsigned i=0;i<sizeof(cmd);i++)cmd[i]=0;
    gwq_w32(cmd,0x5001);gww_tlv(cmd+4,86,44);gwq_w32(cmd+12,2);gwq_copy(cmd+20,mac,6);
    gwq_w32(cmd+28,2);gwq_w32(cmd+32,s->pdev);gwq_w32(cmd+48,0xff);gww_tlv(cmd+52,18,32);
    for(unsigned i=0;i<2;i++){uint8_t *p=cmd+56+i*16;gww_tlv(p,411,12);gwq_w32(p+4,i);gwq_w32(p+8,s->chains);gwq_w32(p+12,s->chains);}
    if(gww_send(s,io,s->endpoint,cmd,88))return (int)s->error;
    /* ath12k_mac_update_vif_offload: INIT defaults to Native Wi-Fi, while
     * this datapath consumes Ethernet RX. Select the per-vdev offloads before
     * any association. TCL's explicit native bank still overrides TX for
     * EAPOL, as upstream's per-packet encapsulation selection does. */
    const unsigned encap_parameters[]={0x30,0x5b};
    for(unsigned i=0;i<2;i++) {
        for(unsigned j=0;j<20;j++)cmd[j]=0;
        gwq_w32(cmd,0x5008);gww_tlv(cmd+4,95,12);gwq_w32(cmd+12,encap_parameters[i]);gwq_w32(cmd+16,2);
        int error=gww_send(s,io,s->endpoint,cmd,20);if(error)return error;
    }
    gwq_copy(s->mac,mac,6);s->vdev=1;return 0;
}
/* Request the configured country from firmware's OEM regulatory DB. A valid
 * legacy CN event is already sufficient; never invent fallback TX limits. */
static inline int gww_set_country_cn(struct gww_state *s,const struct gww_io *io) {
    if(!s || !io || !io->pump || !io->now_ms || !s->fw_ready || !s->hw_valid)return GWW_STATE;
    if(s->regulatory.valid)return GWW_OK;
    if(!gww_service(s,117))return GWW_STATE;
    unsigned generation=s->regulatory.generation;
    /* A submitted country command grants no transmit permission. Wait for
     * this request's newly received, fully validated PHY0 CN rules. */
    s->regulatory.valid=0;
    uint8_t p[20]={0};gwq_w32(p,0x3a004);gww_tlv(p+4,647,12);
    gwq_w32(p+8,s->pdev);p[16]='C';p[17]='N';
    int error=gww_send(s,io,s->endpoint,p,sizeof(p));if(error)return error;
    uint64_t deadline=io->now_ms(io->context)+5000;
    for(unsigned i=0;i<5000 && !s->error;i++) {
        if(s->regulatory.generation!=generation) {
            if(s->regulatory.valid)return GWW_OK;
            if(s->regulatory.error)return gww_fail(s,GWW_FIRMWARE);
        }
        if(io->now_ms(io->context)>=deadline)break;
        if(io->pump(io->context,1000))return gww_fail(s,GWW_TRANSPORT);
    }
    if(s->error)return (int)s->error;
    return s->regulatory.valid && s->regulatory.generation!=generation?GWW_OK:gww_fail(s,GWW_TIMEOUT);
}
static inline int gww_scan_begin(struct gww_state *s,const struct gww_io *io,const struct gww_scan_request *request) {
    if(!s || !io || !request)return GWW_ARGUMENT;
    if(s->error || !s->vdev || !s->fw_ready || !io->now_ms)return GWW_STATE;if(s->scanning)return GWW_BUSY;
    if(request->active>1 || request->connected>1 || request->ssid_count>GWW_SCAN_SSID_MAX ||
       request->bssid_count>GWW_SCAN_BSSID_MAX || request->frequency_count>GWW_SCAN_FREQ_MAX ||
       (request->connected?!gww_channel(request->home_frequency):request->home_frequency!=0))return GWW_ARGUMENT;
    struct gww_scan_request q=*request;unsigned power=0,gain=0;
    if(q.connected && !gwreg_channel(&s->regulatory,q.home_frequency,&power,&gain))return GWW_STATE;
    for(unsigned i=0;i<q.ssid_count;i++) {
        if(q.ssids[i].length>32)return GWW_ARGUMENT;
        for(unsigned j=0;j<i;j++)if(q.ssids[i].length==q.ssids[j].length &&
            gww_same_bytes(q.ssids[i].bytes,q.ssids[j].bytes,q.ssids[i].length))return GWW_ARGUMENT;
    }
    static const uint8_t broadcast[6]={255,255,255,255,255,255};
    for(unsigned i=0;i<q.bssid_count;i++) {
        unsigned nz=0;for(unsigned j=0;j<6;j++)nz|=q.bssids[i][j];
        if(!nz || ((q.bssids[i][0]&1) && !gww_same_bytes(q.bssids[i],broadcast,6)))return GWW_ARGUMENT;
        for(unsigned j=0;j<i;j++)if(gww_same_bytes(q.bssids[i],q.bssids[j],6))return GWW_ARGUMENT;
    }
    if(!q.frequency_count){q.frequency_count=GWW_SCAN_CHANNELS;for(unsigned i=0;i<GWW_SCAN_CHANNELS;i++)q.frequencies[i]=gww_scan_frequencies[i];}
    for(unsigned i=0;i<q.frequency_count;i++) {
        if(!gww_channel(q.frequencies[i]))return GWW_ARGUMENT;
        for(unsigned j=0;j<i;j++)if(q.frequencies[i]==q.frequencies[j])return GWW_ARGUMENT;
    }
    unsigned channels=0;
    for(unsigned i=0;i<q.frequency_count;i++)
        if(!q.active || gwreg_channel(&s->regulatory,q.frequencies[i],&power,&gain))q.frequencies[channels++]=q.frequencies[i];
    if(!channels)return GWW_STATE;q.frequency_count=channels;
    /* Firmware constructs real probe requests. A wildcard BSSID must be all
     * ones; zero would put an invalid receiver/address on the wire. */
    if(q.active && !q.bssid_count){q.bssid_count=1;gwq_copy(q.bssids[0],broadcast,6);}
    if(q.active && !q.ssid_count){q.ssid_count=1;q.ssids[0].length=0;}
    unsigned flags=0x10000; /* STRICT_PASSIVE_ON_PCHN; never FORCE_ACTIVE_DFS. */
    if(!q.active)flags|=1;
    else {
        flags|=4|8|0x800; /* CCK/OFDM rates and DS IE for probe requests. */
        for(unsigned i=0;i<q.ssid_count;i++)if(!q.ssids[i].length)flags|=2;
    }
    uint8_t cmd[1024]={0};gwq_w32(cmd,0x3001);gww_tlv(cmd+4,77,156);uint8_t *p=cmd+4;
    uint32_t id=0xa000|((s->scan_id+1)&0xfff);gwq_w32(p+4,id);gwq_w32(p+8,0xa001);
    gwq_w32(p+16,q.connected?1:2);gwq_w32(p+20,0x7f);gwq_w32(p+24,q.connected?40:60);gwq_w32(p+28,110);
    gwq_w32(p+32,q.connected?100:50);gwq_w32(p+36,500);gwq_w32(p+48,50);gwq_w32(p+52,15000);
    gwq_w32(p+56,q.active?5:0);gwq_w32(p+60,flags);gwq_w32(p+64,q.connected?110:0);
    gwq_w32(p+68,q.frequency_count);gwq_w32(p+72,q.bssid_count);gwq_w32(p+76,q.ssid_count);gwq_w32(p+84,q.active?2:0);
    size_t off=164;gww_tlv(cmd+off,16,q.frequency_count*4);off+=4;
    for(unsigned i=0;i<q.frequency_count;i++,off+=4)gwq_w32(cmd+off,q.frequencies[i]);
    gww_tlv(cmd+off,19,q.ssid_count*36);off+=4;
    for(unsigned i=0;i<q.ssid_count;i++,off+=36){gwq_w32(cmd+off,q.ssids[i].length);gwq_copy(cmd+off+4,q.ssids[i].bytes,q.ssids[i].length);}
    gww_tlv(cmd+off,19,q.bssid_count*8);off+=4;
    for(unsigned i=0;i<q.bssid_count;i++,off+=8)gwq_copy(cmd+off,q.bssids[i],6);
    gww_tlv(cmd+off,17,0);off+=4;
    s->scan_request=q;s->scan_id=id;s->scan_reg_generation=s->regulatory.generation;s->scan_failure=0;s->scan_cancel_pending=0;
    s->bss_count=0;for(unsigned i=0;i<GWW_BSS_MAX;i++)s->bss[i].ssid_len=0;
    s->scanning=1;s->scan_started=0;s->scan_deadline=io->now_ms(io->context)+20000;
    int e=gww_send(s,io,s->endpoint,cmd,off);if(e)gww_complete(s,io,(unsigned)e);return e;
}
static inline int gww_scan(struct gww_state *s,const struct gww_io *io) {
    struct gww_scan_request q;for(size_t i=0;i<sizeof(q);i++)((uint8_t *)&q)[i]=0;
    return gww_scan_begin(s,io,&q);
}
static inline int gww_scan_cancel(struct gww_state *s,const struct gww_io *io) {
    if(!s || !io || !io->now_ms)return GWW_ARGUMENT;
    if(!s->scanning || s->scan_cancel_pending)return GWW_OK;
    uint8_t p[28]={0};gwq_w32(p,0x3002);gww_tlv(p+4,78,20);
    gwq_w32(p+8,0xa001);gwq_w32(p+12,s->scan_id);gwq_w32(p+24,s->pdev);
    s->scan_cancel_pending=1;if(!s->scan_failure)s->scan_failure=GWW_STATE;
    s->scan_deadline=io->now_ms(io->context)+5000;
    int e=gww_send(s,io,s->endpoint,p,sizeof(p));
    /* A failed STOP cannot release firmware ownership, including local
     * argument/endpoint failures that gww_send does not itself latch. */
    if(e){(void)gww_fail(s,(unsigned)e);gww_complete(s,io,(unsigned)e);}return e;
}
static inline void gww_tick(struct gww_state *s,const struct gww_io *io) {
    if(!s || !io || !io->now_ms || !s->scanning)return;
    if(s->error){gww_complete(s,io,s->error);return;}
    if(!s->scan_cancel_pending && s->scan_reg_generation!=s->regulatory.generation) {
        unsigned power=0,gain=0;s->scan_reg_generation=s->regulatory.generation;
        if(s->scan_request.connected && !gwreg_channel(&s->regulatory,s->scan_request.home_frequency,&power,&gain)) {
            s->scan_failure=GWW_STATE;(void)gww_scan_cancel(s,io);return;
        }
        for(unsigned i=0;s->scan_request.active && i<s->scan_request.frequency_count;i++)
            if(!gwreg_channel(&s->regulatory,s->scan_request.frequencies[i],&power,&gain)) {
                s->scan_failure=GWW_STATE;(void)gww_scan_cancel(s,io);return;
            }
    }
    if(io->now_ms(io->context)<s->scan_deadline)return;
    if(!s->scan_cancel_pending){s->scan_failure=GWW_TIMEOUT;(void)gww_scan_cancel(s,io);return;}
    /* Firmware may still own the off-channel operation: prevent a new join
     * until transport recovery establishes quiescence. */
    (void)gww_fail(s,GWW_TIMEOUT);gww_complete(s,io,GWW_TIMEOUT);
}
#endif
