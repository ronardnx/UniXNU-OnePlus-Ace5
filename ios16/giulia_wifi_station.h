/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * WCN7850 station control: Linux v6.12 ath12k wmi.h/wmi.c/mac.c/hw.c.
 * Legacy 20 MHz, open authentication, PSK or SAE with CCMP-128. The kernel
 * supplicant owns EAPOL and authorizes the port after the four-way handshake;
 * iOS supplies a PMK, never a passphrase, to that supplicant.
 * All calls are serialized by the platform worker. Event callbacks record
 * progress only; gwt_tick sends commands outside the RX dispatch stack.
 */
#ifndef GIULIA_WIFI_STATION_H
#define GIULIA_WIFI_STATION_H
#include "giulia_wifi_wmi.h"
#include "giulia_wifi_sha256.h"
enum { GWT_OPEN,GWT_WPA2_CCMP,GWT_WPA3_SAE };
#define GWT_MGMT_MAX 1024u
#define GWT_ASSOC_MAX 192u
enum { GWT_IDLE,GWT_PEER_WAIT,GWT_START_WAIT,GWT_AUTH_WAIT,GWT_ASSOC_WAIT,
       GWT_ASSOC_CONFIRM,GWT_KEY_WAIT,GWT_RUN,GWT_STOPPING,GWT_FAILED };
enum { GWT_OK,GWT_ARGUMENT,GWT_UNSUPPORTED,GWT_UNAVAILABLE,GWT_BUSY,
       GWT_TRANSPORT,GWT_TIMEOUT,GWT_REJECTED,GWT_PROTOCOL,GWT_DATAPATH };
struct gwt_channel { unsigned max_power,max_reg_power,max_antenna_gain; };
static inline int gwt_channel_allowed(const struct gww_state *wmi,unsigned frequency,struct gwt_channel *out) {
    unsigned power=0,gain=0;
    if(!wmi || !out || !gwreg_channel(&wmi->regulatory,frequency,&power,&gain))return GWT_UNAVAILABLE;
    out->max_power=power;out->max_reg_power=power;out->max_antenna_gain=gain;return GWT_OK;
}
struct gwt_request { struct gww_bss bss;unsigned security,pmf,h2e,has_pmkid;uint8_t pmkid[16]; }; /* PMF: 0 disabled, 1 capable, 2 required */
struct gwt_security_ops {
    void *context;
    int (*auth_start)(void *);
    /* Receive callback only queues auth bodies; the worker performs crypto. */
    void (*auth_rx)(void *,const uint8_t *,size_t);
    /* Zero returns an authenticated/normalized RX frame, or a WMI-ready TX
     * frame. Nonzero means dropped/handled. Output length is capacity in. */
    int (*mgmt_rx)(void *,const uint8_t *,size_t,unsigned,unsigned,uint8_t *,size_t *);
    int (*mgmt_tx)(void *,const uint8_t *,size_t,uint8_t *,size_t *);
    int (*protected_ready)(void *);
};
struct gwt_key { unsigned index,pairwise;uint64_t rsc;uint8_t data[16]; };
struct gwt_ops {
    void *context;
    /* Zero means ready/success. channel must enforce the current regdb,
     * NO_IR/DFS and power limits (half-dBm), not just convert frequency. */
    int (*ready)(void *,unsigned encrypted);
    int (*channel)(void *,unsigned frequency,struct gwt_channel *);
    /* Copy, map and publish the full management frame until completion.
     * A timeout does NOT release the DMA lease. */
    int (*mgmt_hold)(void *,const uint8_t *,size_t,uint32_t,uint64_t *);
    void (*mgmt_done)(void *,uint32_t,unsigned);
    int (*reo_queue)(void *,unsigned tid,uint64_t *);
    /* Install replay protection and TX/RX cipher metadata after the FW
     * completion. EAPOL remains permitted while the data port is closed. */
    int (*key_ready)(void *,unsigned peer,unsigned index,unsigned pairwise,uint64_t rsc);
    int (*port)(void *,unsigned peer,unsigned open);
    void (*notify)(void *,unsigned phase,unsigned error);
};
struct gwt_state {
    struct gww_state *wmi;const struct gww_io *io;struct gwt_ops ops;struct gwt_security_ops security_ops;
    struct gwt_request request;struct gwt_channel channel;
    unsigned phase,error,status,peer_id,peer_mapped,peer_created,start_sent,vdev_started,vdev_up;
    unsigned start_confirm,auth_confirm,assoc_confirm,peer_confirm,aid,capability;
    unsigned stop_stage,stop_confirm,delete_confirm,reason,seq,authorized,installed;
    unsigned tx_pending,tx_failed,key_pending,key_confirm,key_index,key_pairwise;
    uint32_t cookie,tx_cookie;uint64_t deadline,tx_deadline,key_deadline,key_rsc;
    uint8_t rates[16],rate_count,key_hash[32],installed_hash[5][32];
};
static inline void gwt_zero(void *p,size_t n) {volatile uint8_t *v=(volatile uint8_t *)p;while(n--)*v++=0;}
static inline int gwt_equal(const uint8_t *a,const uint8_t *b,size_t n) {
    unsigned diff=0;for(size_t i=0;i<n;i++)diff|=a[i]^b[i];return !diff;
}
static inline uint64_t gwt_now(const struct gwt_state *s){return s->io->now_ms(s->io->context);}
static inline void gwt_phase(struct gwt_state *s,unsigned phase) {
    s->phase=phase;s->deadline=gwt_now(s)+((phase==GWT_KEY_WAIT || (phase==GWT_AUTH_WAIT && s->request.security==GWT_WPA3_SAE))?30000:5000);
    if(s->ops.notify)s->ops.notify(s->ops.context,phase,s->error);
}
static inline int gwt_disconnect(struct gwt_state *s,unsigned reason) {
    if(!s || !s->wmi || reason>65535)return GWT_ARGUMENT;
    if(s->phase==GWT_IDLE || s->phase==GWT_STOPPING)return GWT_OK;
    if(s->phase==GWT_FAILED)return GWT_UNAVAILABLE;
    s->reason=reason?reason:3;s->authorized=0;s->stop_stage=0;s->stop_confirm=0;s->delete_confirm=0;
    if(s->peer_mapped && s->ops.port)s->ops.port(s->ops.context,s->peer_id,0);
    gwt_phase(s,GWT_STOPPING);return GWT_OK;
}
static inline void gwt_abort(struct gwt_state *s,unsigned error,unsigned status) {
    if(!s->error)s->error=error;s->status=status;(void)gwt_disconnect(s,3);
}
static inline int gwt_send(struct gwt_state *s,const uint8_t *p,size_t n) {
    if(gww_send(s->wmi,s->io,s->wmi->endpoint,p,n)){gwt_abort(s,GWT_TRANSPORT,0);return GWT_TRANSPORT;}
    return GWT_OK;
}
static inline size_t gwt_command(uint8_t *p,size_t n,unsigned id,unsigned tag) {
    for(size_t i=0;i<n;i++)p[i]=0;gwq_w32(p,id);gww_tlv(p+4,tag,(unsigned)n-8);return n;
}
static inline int gwt_vdev_command(struct gwt_state *s,unsigned id,unsigned tag) {
    uint8_t p[12];gwt_command(p,sizeof(p),id,tag);return gwt_send(s,p,sizeof(p));
}
static inline int gwt_peer_command(struct gwt_state *s,unsigned id,unsigned tag) {
    uint8_t p[24];size_t n=id==0x6001?24:20;gwt_command(p,n,id,tag);
    gwq_copy(p+12,s->request.bss.bssid,6);return gwt_send(s,p,n);
}
static inline int gwt_peer_parameter(struct gwt_state *s,unsigned parameter,unsigned value) {
    uint8_t p[28];gwt_command(p,sizeof(p),0x6004,100);gwq_copy(p+12,s->request.bss.bssid,6);
    gwq_w32(p+20,parameter);gwq_w32(p+24,value);return gwt_send(s,p,sizeof(p));
}
static inline int gwt_reo(struct gwt_state *s,unsigned tid,uint64_t iova) {
    if(tid>16 || !iova || (iova&127) || iova>=(UINT64_C(1)<<40))return GWT_ARGUMENT;
    uint8_t p[44];gwt_command(p,sizeof(p),0x6013,549);gwq_copy(p+12,s->request.bss.bssid,6);
    gwq_w32(p+20,tid);gwq_w32(p+24,(uint32_t)iova);gwq_w32(p+28,(uint32_t)(iova>>32));
    gwq_w32(p+32,tid);gwq_w32(p+36,1);gwq_w32(p+40,1);return gwt_send(s,p,sizeof(p));
}
static inline int gwt_start(struct gwt_state *s) {
    uint8_t p[152]={0};gwq_w32(p,0x5003);gww_tlv(p+4,88,112);uint8_t *c=p+4;
    gwq_w32(c+12,s->request.bss.beacon);gwq_w32(c+16,1);
    gwq_w32(c+20,s->request.pmf?2:0); /* WMI_VDEV_START_PMF_ENABLED */
    gwq_w32(c+76,1);gwq_w32(c+80,1);gwq_w32(c+100,1);gwq_w32(c+112,0xffffffffu);
    c=p+120;gww_tlv(c,80,24);gwq_w32(c+4,s->request.bss.frequency);
    gwq_w32(c+8,s->request.bss.frequency);gwq_w32(c+16,s->request.bss.frequency<3000?1:0);
    gwq_w32(c+20,(s->channel.max_power<<8)|(s->channel.max_reg_power<<16));
    gwq_w32(c+24,s->channel.max_antenna_gain|(s->channel.max_power<<8));gww_tlv(p+148,18,0);
    s->start_sent=1;return gwt_send(s,p,sizeof(p));
}
static inline int gwt_up(struct gwt_state *s) {
    uint8_t p[40];gwt_command(p,sizeof(p),0x5005,92);gwq_w32(p+12,s->aid);
    gwq_copy(p+16,s->request.bss.bssid,6);return gwt_send(s,p,sizeof(p));
}
static inline int gwt_peer_assoc(struct gwt_state *s) {
    /* 248-byte v6.12 fixed structure, legacy rate array, empty HT array,
     * 24-byte VHT rate TLV, then HE/MLO/EHT/partner arrays (all empty). */
    uint8_t p[320]={0};gwq_w32(p,0x6005);gww_tlv(p+4,101,244);uint8_t *c=p+4;
    gwq_copy(c+4,s->request.bss.bssid,6);gwq_w32(c+16,1);gwq_w32(c+20,s->aid);
    gwq_w32(c+24,(s->request.security==GWT_OPEN?1:4)|(s->request.pmf?0x08000000u:0));gwq_w32(c+28,s->capability);
    gwq_w32(c+32,1);gwq_w32(c+52,1);gwq_w32(c+60,s->request.bss.frequency<3000?1:0);
    gwq_w32(c+72,s->rate_count);gwq_w32(c+180,0xffffffffu);
    size_t aligned=(s->rate_count+3u)&~3u,at=252;gww_tlv(p+at,17,(unsigned)aligned);at+=4;
    /* WMI rates are 500 kbit/s values, not the on-air basic-rate bitmap. */
    for(unsigned i=0;i<s->rate_count;i++)p[at+i]=s->rates[i]&0x7f;
    at+=aligned;gww_tlv(p+at,17,0);at+=4;
    gww_tlv(p+at,102,20);at+=24;
    for(unsigned i=0;i<4;i++){gww_tlv(p+at,18,0);at+=4;}
    return gwt_send(s,p,at);
}
static inline void gwt_frame_header(struct gwt_state *s,uint8_t *p,unsigned subtype) {
    for(unsigned i=0;i<24;i++)p[i]=0;p[0]=(uint8_t)subtype;
    gwq_copy(p+4,s->request.bss.bssid,6);gwq_copy(p+10,s->wmi->mac,6);gwq_copy(p+16,s->request.bss.bssid,6);
    gwq_w16(p+22,(uint16_t)((s->seq++&0xfff)<<4));
}
static inline size_t gwt_assoc_frame(struct gwt_state *s,uint8_t p[GWT_ASSOC_MAX]) {
    gwt_frame_header(s,p,0);gwq_w16(p+24,(uint16_t)(1|(s->request.security?0x10:0)));
    gwq_w16(p+26,1);size_t at=28;p[at++]=0;p[at++]=s->request.bss.ssid_len;
    gwq_copy(p+at,s->request.bss.ssid,s->request.bss.ssid_len);at+=s->request.bss.ssid_len;
    unsigned first=s->rate_count>8?8:s->rate_count;p[at++]=1;p[at++]=(uint8_t)first;
    gwq_copy(p+at,s->rates,first);at+=first;
    if(s->rate_count>8){p[at++]=50;p[at++]=s->rate_count-8;gwq_copy(p+at,s->rates+8,s->rate_count-8);at+=s->rate_count-8;}
    if(s->request.security) {
        uint8_t rsn[44]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0,0,0,0,15,172,6};
        if(s->request.security==GWT_WPA3_SAE)rsn[19]=8;
        unsigned n=s->request.pmf?28:22;if(s->request.pmf){rsn[1]=26;rsn[20]=s->request.pmf==2?0xc0:0x80;}
        if(s->request.security==GWT_WPA3_SAE && s->request.has_pmkid){
            rsn[1]=42;rsn[22]=1;gwq_copy(rsn+24,s->request.pmkid,16);rsn[40]=0;rsn[41]=15;rsn[42]=172;rsn[43]=6;n=44;
        }
        gwq_copy(p+at,rsn,n);at+=n;
        if(s->request.h2e){p[at++]=244;p[at++]=1;p[at++]=0x20;}
    }
    return at;
}
static inline int gwt_mgmt_send(struct gwt_state *s,const uint8_t *frame,size_t bytes) {
    if(!s || !frame || bytes<24 || bytes>GWT_MGMT_MAX)return GWT_ARGUMENT;
    if(s->tx_pending)return GWT_BUSY;
    uint8_t protected_frame[GWT_MGMT_MAX];
    if(s->request.pmf && s->security_ops.mgmt_tx) {
        size_t n=sizeof(protected_frame);
        if(s->security_ops.mgmt_tx(s->security_ops.context,frame,bytes,protected_frame,&n) || n<24 || n>sizeof(protected_frame))return GWT_DATAPATH;
        frame=protected_frame;bytes=n;
    }
    uint32_t cookie=++s->cookie;if(!cookie)cookie=++s->cookie;uint64_t iova=0;
    if(s->ops.mgmt_hold(s->ops.context,frame,bytes,cookie,&iova))return GWT_DATAPATH;
    if(!iova || iova>=(UINT64_C(1)<<40) || bytes>(UINT64_C(1)<<40)-iova) {
        s->ops.mgmt_done(s->ops.context,cookie,GWT_ARGUMENT);return GWT_ARGUMENT;
    }
    uint8_t p[108]={0};unsigned prefix=bytes>64?64:(unsigned)bytes;
    gwq_w32(p,0x7008);gww_tlv(p+4,422,32);gwq_w32(p+12,cookie);
    /* This is on-channel management, not remain-on-channel/off-channel TX. */
    gwq_w32(p+20,(uint32_t)iova);gwq_w32(p+24,(uint32_t)(iova>>32));
    gwq_w32(p+28,(uint32_t)bytes);gwq_w32(p+32,prefix);gww_tlv(p+40,17,prefix);gwq_copy(p+44,frame,prefix);
    s->tx_cookie=cookie;s->tx_pending=1;s->tx_failed=0;s->tx_deadline=gwt_now(s)+2000;
    /* Transport failure can be ambiguous: retain the DMA lease until an
     * actual TX completion or explicit platform quiescence. */
    return gwt_send(s,p,44+((prefix+3)&~3u));
}
static inline int gwt_rsn(const uint8_t *p,size_t n,unsigned security,unsigned pmf) {
    static const uint8_t ccmp[4]={0,15,172,4},bip[4]={0,15,172,6};
    uint8_t akm[4]={0,15,172,(uint8_t)(security==GWT_WPA3_SAE?8:2)};
    if(n<18 || gwq_u16(p)!=1 || !gwt_equal(p+2,ccmp,4))return 0;
    size_t at=6;unsigned count=gwq_u16(p+at);at+=2;
    if(!count || count>(n-at)/4)return 0;unsigned found=0;
    for(unsigned i=0;i<count;i++,at+=4)found|=gwt_equal(p+at,ccmp,4);
    if(!found || n-at<2)return 0;count=gwq_u16(p+at);at+=2;
    if(!count || count>(n-at)/4)return 0;found=0;
    for(unsigned i=0;i<count;i++,at+=4)found|=gwt_equal(p+at,akm,4);
    if(!found || (n-at && n-at<2))return 0;
    unsigned caps=0;if(n-at>=2){caps=gwq_u16(p+at);at+=2;}
    if((caps&0x40) && !pmf)return 0;if(pmf && !(caps&0x80))return 0;
    if(at==n)return 1;if(n-at<2)return 0;count=gwq_u16(p+at);at+=2;
    if(count>(n-at)/16)return 0;at+=count*16;
    return at==n || (n-at==4 && (!pmf || gwt_equal(p+at,bip,4)));
}
static inline int gwt_request_valid(const struct gwt_request *q,uint8_t rates[16],uint8_t *count) {
    if(!q || !count || q->security>GWT_WPA3_SAE || q->pmf>2 || q->h2e>1 || q->has_pmkid>1 ||
       ((q->h2e || q->has_pmkid) && q->security!=GWT_WPA3_SAE) || (!q->security && q->pmf) ||
       (q->security==GWT_WPA3_SAE && q->pmf!=2) || !q->bss.ssid_len || q->bss.ssid_len>32 ||
       !q->bss.beacon || !q->bss.rate_count || q->bss.rate_count>16 || q->bss.ie_bytes>GWW_IE_MAX ||
       !gww_channel(q->bss.frequency) || (q->bss.bssid[0]&1) || !(q->bss.capability&1) || (q->bss.capability&2))return 0;
    unsigned nz=0;for(unsigned i=0;i<6;i++)nz|=q->bss.bssid[i];if(!nz)return 0;
    if(!!(q->bss.capability&0x10)!=!!q->security)return 0;
    unsigned rsn=0,wpa=0;
    for(size_t at=0;at<q->bss.ie_bytes;) {
        if(q->bss.ie_bytes-at<2)return 0;unsigned tag=q->bss.ie[at],n=q->bss.ie[at+1];at+=2;
        if(n>q->bss.ie_bytes-at)return 0;
        if(tag==48){if(rsn++ || !gwt_rsn(q->bss.ie+at,n,q->security,q->pmf))return 0;}
        if(tag==221 && n>=4 && !q->bss.ie[at] && q->bss.ie[at+1]==80 && q->bss.ie[at+2]==242 && q->bss.ie[at+3]==1)wpa=1;
        at+=n;
    }
    if((q->security && !rsn) || (!q->security && (rsn||wpa)))return 0;
    *count=0;
    for(unsigned i=0;i<q->bss.rate_count;i++) {
        unsigned rate=q->bss.rates[i]&0x7f;
        unsigned supported=rate==12||rate==18||rate==24||rate==36||rate==48||rate==72||rate==96||rate==108;
        if(q->bss.frequency<3000)supported|=rate==2||rate==4||rate==11||rate==22;
        if(!supported){if(q->bss.rates[i]&0x80)return 0;continue;}
        unsigned duplicate=0;for(unsigned j=0;j<*count;j++)duplicate|=(rates[j]&0x7f)==rate;
        if(!duplicate)rates[(*count)++]=q->bss.rates[i];
    }
    return *count!=0;
}
static inline void gwt_peer_map(struct gwt_state *s,unsigned vdev,const uint8_t *mac,unsigned peer,unsigned mapped) {
    if(!s || !mac || vdev || peer>65535 || mapped>1 || !gwt_equal(mac,s->request.bss.bssid,6))return;
    /* PEER_MAP can arrive after a cancel but before PEER_DELETE completes.
     * Keep tracking that DMA owner until the matching HTT PEER_UNMAP. */
    if(mapped && s->peer_created && (s->phase==GWT_PEER_WAIT || s->phase==GWT_STOPPING)) {
        if(s->peer_mapped && s->peer_id!=peer){gwt_abort(s,GWT_PROTOCOL,0);return;}
        s->peer_id=peer;s->peer_mapped=1;
    }
    else if(!mapped && s->peer_mapped && s->peer_id==peer) {
        s->peer_mapped=0;if(s->phase!=GWT_STOPPING)gwt_abort(s,GWT_DATAPATH,0);
    }
}
static inline void gwt_mgmt(struct gwt_state *s,const uint8_t *p,size_t n,unsigned frequency) {
    if(!s || !s->wmi || !p || n<24 || frequency!=s->request.bss.frequency || (p[1]&0xc7) || (gwq_u16(p+22)&15) ||
       !gwt_equal(p+10,s->request.bss.bssid,6) || !gwt_equal(p+16,s->request.bss.bssid,6))return;
    unsigned type=p[0]&0xfc;if(p[0]&3)return;
    static const uint8_t broadcast[6]={255,255,255,255,255,255};
    unsigned local=gwt_equal(p+4,s->wmi->mac,6);
    if((type==0xc0 || type==0xa0) && n>=26 && s->phase!=GWT_IDLE &&
       (local || gwt_equal(p+4,broadcast,6))) {
        s->status=gwq_u16(p+24);(void)gwt_disconnect(s,s->status);return;
    }
    if(!local)return;
    if(type==0xb0 && s->phase==GWT_AUTH_WAIT) {
        if(s->request.security==GWT_WPA3_SAE) {
            if(n>=30 && gwq_u16(p+24)==3 && s->security_ops.auth_rx)s->security_ops.auth_rx(s->security_ops.context,p+24,n-24);
            return;
        }
        if(n<30 || gwq_u16(p+24)!=0 || gwq_u16(p+26)!=2)return;
        if(gwq_u16(p+28))gwt_abort(s,GWT_REJECTED,gwq_u16(p+28));else s->auth_confirm=1;
    } else if(type==0x10 && s->phase==GWT_ASSOC_WAIT) {
        if(n<30)return;if(gwq_u16(p+26)){gwt_abort(s,GWT_REJECTED,gwq_u16(p+26));return;}
        unsigned aid=gwq_u16(p+28)&0x3fff;if(!aid || aid>2007)return;
        for(size_t at=30;at<n;){if(n-at<2 || p[at+1]>n-at-2)return;at+=2+p[at+1];}
        s->aid=aid;s->capability=gwq_u16(p+24);s->assoc_confirm=1;
    }
}
static inline void gwt_event(void *context,unsigned event,const uint8_t *p,size_t n) {
    struct gwt_state *s=(struct gwt_state *)context;size_t z=0;const uint8_t *v;
    if(!s || !p)return;
    if(event==0x7006) {
        v=gww_field(p,n,423,&z);if(!v || z<12){gwt_abort(s,GWT_PROTOCOL,0);return;}
        if(s->tx_pending && gwq_u32(v)==s->tx_cookie) {
            unsigned status=gwq_u32(v+4);s->tx_pending=0;s->tx_failed=status!=0;
            s->ops.mgmt_done(s->ops.context,s->tx_cookie,status);
            if(status && s->phase!=GWT_STOPPING)gwt_abort(s,GWT_TRANSPORT,status);
        }
    } else if(event==0x7001) {
        v=gww_field(p,n,44,&z);if(!v || z<68)return;
        unsigned bytes=gwq_u32(v+16),status=gwq_u32(v+20),freq=gwq_u32(v+64);
        const uint8_t *frame=gww_field(p,n,17,&z);if(!frame || bytes>z)return;
        if(s->request.pmf && s->security_ops.mgmt_rx) {
            uint8_t normalized[GWT_MGMT_MAX];size_t used=sizeof(normalized);
            if(!s->security_ops.mgmt_rx(s->security_ops.context,frame,bytes,status,freq,normalized,&used) && used<=sizeof(normalized))
                gwt_mgmt(s,normalized,used,freq);
        } else if(!status)gwt_mgmt(s,frame,bytes,freq);
    } else if(event==0x5001 && s->phase==GWT_START_WAIT) {
        v=gww_field(p,n,40,&z);if(!v || z<36){gwt_abort(s,GWT_PROTOCOL,0);return;}
        if(gwq_u32(v) || gwq_u32(v+4) || gwq_u32(v+8))return;
        if(gwq_u32(v+12)){s->start_sent=0;gwt_abort(s,GWT_REJECTED,gwq_u32(v+12));}else s->start_confirm=1;
    } else if(event==0x6006 && s->phase==GWT_ASSOC_CONFIRM) {
        v=gww_field(p,n,434,&z);if(!v || z<12){gwt_abort(s,GWT_PROTOCOL,0);return;}
        if(!gwq_u32(v) && gwt_equal(v+4,s->request.bss.bssid,6))s->peer_confirm=1;
    } else if(event==0x5003 && s->key_pending && s->phase==GWT_KEY_WAIT) {
        v=gww_field(p,n,42,&z);if(!v || z<24){gwt_abort(s,GWT_PROTOCOL,0);return;}
        if(gwq_u32(v) || !gwt_equal(v+4,s->request.bss.bssid,6) || gwq_u32(v+12)!=s->key_index ||
           gwq_u32(v+16)!=(s->key_pairwise?0u:1u))return;
        if(gwq_u32(v+20))gwt_abort(s,GWT_REJECTED,gwq_u32(v+20));else s->key_confirm=1;
    } else if(event==0x5002 && s->phase==GWT_STOPPING && s->stop_stage==2) {
        v=gww_field(p,n,41,&z);if(v && z>=4 && !gwq_u32(v))s->stop_confirm=1;
    } else if(event==0x6007 && s->phase==GWT_STOPPING && s->stop_stage==3) {
        v=gww_field(p,n,451,&z);if(v && z>=12 && !gwq_u32(v) && gwt_equal(v+4,s->request.bss.bssid,6))s->delete_confirm=1;
    } else if(event==0x6001 && s->phase!=GWT_IDLE) {
        v=gww_field(p,n,43,&z);if(v && z>=8 && gwt_equal(v,s->request.bss.bssid,6))gwt_abort(s,GWT_REJECTED,0);
    } else if(event==0xb001 && s->phase!=GWT_IDLE) {
        v=gww_field(p,n,48,&z);if(v && z>=12 && !gwq_u32(v) && (gwq_u32(v+4)&15)==2)gwt_abort(s,GWT_TIMEOUT,0);
    }
}
static inline int gwt_init(struct gwt_state *s,struct gww_state *wmi,const struct gww_io *io,const struct gwt_ops *ops) {
    if(!s || !wmi || !io || !io->now_ms || !ops || !ops->ready || !ops->channel || !ops->mgmt_hold ||
       !ops->mgmt_done || !ops->reo_queue || !ops->port || wmi->event)return GWT_ARGUMENT;
    gwt_zero(s,sizeof(*s));s->wmi=wmi;s->io=io;s->ops=*ops;wmi->event_context=s;wmi->event=gwt_event;return GWT_OK;
}
static inline int gwt_set_security_ops(struct gwt_state *s,const struct gwt_security_ops *ops) {
    if(!s || !ops || s->phase!=GWT_IDLE)return GWT_ARGUMENT;s->security_ops=*ops;return GWT_OK;
}
static inline int gwt_auth_send(struct gwt_state *s,const uint8_t *body,size_t bytes) {
    if(!s || !body || bytes<6 || bytes>GWT_MGMT_MAX-24 || s->phase!=GWT_AUTH_WAIT || s->request.security!=GWT_WPA3_SAE)return GWT_ARGUMENT;
    uint8_t p[GWT_MGMT_MAX];gwt_frame_header(s,p,0xb0);gwq_copy(p+24,body,bytes);return gwt_mgmt_send(s,p,bytes+24);
}
static inline int gwt_auth_complete(struct gwt_state *s,unsigned status) {
    if(!s || s->phase!=GWT_AUTH_WAIT || s->request.security!=GWT_WPA3_SAE)return GWT_ARGUMENT;
    if(status){gwt_abort(s,GWT_REJECTED,status);return GWT_REJECTED;}s->auth_confirm=1;return GWT_OK;
}
static inline int gwt_connect(struct gwt_state *s,const struct gwt_request *request) {
    uint8_t rates[16]={0},count=0;struct gwt_channel channel={0,0,0};
    if(!s || !s->wmi || !gwt_request_valid(request,rates,&count))return GWT_ARGUMENT;
    if(s->phase!=GWT_IDLE || s->tx_pending || s->wmi->scanning)return GWT_BUSY;
    if(!s->wmi->vdev || !s->wmi->fw_ready || s->wmi->error || !s->wmi->hw_valid ||
       !gww_service(s->wmi,76) || !gww_service(s->wmi,79))return GWT_UNAVAILABLE;
    if(request->security && !s->ops.key_ready)return GWT_UNSUPPORTED;
    if(request->pmf && (!s->security_ops.mgmt_rx || !s->security_ops.mgmt_tx || !s->security_ops.protected_ready))return GWT_UNSUPPORTED;
    if(request->security==GWT_WPA3_SAE && (!s->security_ops.auth_start || !s->security_ops.auth_rx))return GWT_UNSUPPORTED;
    if(s->ops.ready(s->ops.context,request->security!=GWT_OPEN) || s->ops.channel(s->ops.context,request->bss.frequency,&channel) ||
       channel.max_power>255 || channel.max_reg_power>255 || channel.max_antenna_gain>255)return GWT_UNAVAILABLE;
    struct gww_state *wmi=s->wmi;const struct gww_io *io=s->io;struct gwt_ops ops=s->ops;struct gwt_security_ops security_ops=s->security_ops;uint32_t cookie=s->cookie;
    gwt_zero(s,sizeof(*s));s->wmi=wmi;s->io=io;s->ops=ops;s->security_ops=security_ops;s->cookie=cookie;s->request=*request;s->channel=channel;
    gwq_copy(s->rates,rates,count);s->rate_count=count;gwt_phase(s,GWT_PEER_WAIT);
    if(request->pmf) {
        /* ath12k_mac_start: enable firmware PMF QoS processing on the
         * logical pdev before creating a protected-management peer. */
        uint8_t p[20];gwt_command(p,sizeof(p),0x4003,82);gwq_w32(p+8,s->wmi->pdev);
        gwq_w32(p+12,0x22);gwq_w32(p+16,1);if(gwt_send(s,p,sizeof(p)))return GWT_TRANSPORT;
    }
    s->peer_created=1;
    return gwt_peer_command(s,0x6001,97);
}
static inline int gwt_install_key(struct gwt_state *s,const struct gwt_key *key) {
    if(!s || !key || key->pairwise>1 || key->index>3 || (key->pairwise && key->index) || key->rsc>UINT64_C(0xffffffffffff))return GWT_ARGUMENT;
    if(!s->request.security)return GWT_UNSUPPORTED;
    if(s->phase!=GWT_KEY_WAIT && s->phase!=GWT_RUN)return GWT_UNAVAILABLE;
    if(s->key_pending)return GWT_BUSY;
    uint8_t digest[32];gwh_sha256(key->data,16,digest);unsigned slot=key->pairwise?0:key->index+1;
    /* Reinstalling the same key must not reset hardware replay/nonce state. */
    if((s->installed&(1u<<slot)) && gwh_equal(digest,s->installed_hash[slot])){gwt_zero(digest,sizeof(digest));return GWT_OK;}
    uint8_t p[128]={0};gwq_w32(p,0x5009);gww_tlv(p+4,96,100);gwq_copy(p+12,s->request.bss.bssid,6);
    gwq_w32(p+20,key->index);gwq_w32(p+24,key->pairwise?0:1);gwq_w32(p+28,4);gwq_w64(p+32,key->rsc);
    gwq_w32(p+88,16);gww_tlv(p+108,17,16);gwq_copy(p+112,key->data,16);
    s->key_pending=1;s->key_confirm=0;s->key_index=key->index;s->key_pairwise=key->pairwise;s->key_rsc=key->rsc;
    gwq_copy(s->key_hash,digest,32);gwt_zero(digest,sizeof(digest));s->key_deadline=gwt_now(s)+2000;
    s->authorized=0;
    if(s->ops.port(s->ops.context,s->peer_id,0)) {
        s->key_pending=0;gwt_zero(p,sizeof(p));gwt_zero(s->key_hash,32);
        gwt_abort(s,GWT_DATAPATH,0);return GWT_DATAPATH;
    }
    gwt_phase(s,GWT_KEY_WAIT);
    int error=gwt_send(s,p,sizeof(p));gwt_zero(p,sizeof(p));gwt_zero(s->wmi->tx,sizeof(s->wmi->tx));return error;
}
static inline int gwt_authorize(struct gwt_state *s) {
    if(!s || (s->phase!=GWT_KEY_WAIT && !(s->phase==GWT_RUN && s->authorized)) ||
       s->key_pending || !(s->installed&1) || !(s->installed&30))return GWT_UNAVAILABLE;
    if(s->ops.ready(s->ops.context,1))return GWT_DATAPATH;
    if(s->request.pmf && (!s->security_ops.protected_ready || s->security_ops.protected_ready(s->security_ops.context)))return GWT_UNAVAILABLE;
    /* A same-key EAPOL retry may have closed the host port without a new
     * firmware key command. Re-open that port even when FW remains AUTH. */
    if(!s->authorized && gwt_peer_parameter(s,3,1))return GWT_TRANSPORT;
    if(s->ops.port(s->ops.context,s->peer_id,1)){gwt_abort(s,GWT_DATAPATH,0);return GWT_DATAPATH;}
    s->authorized=1;gwt_phase(s,GWT_RUN);return GWT_OK;
}
static inline void gwt_tick(struct gwt_state *s) {
    if(!s || !s->wmi || s->phase==GWT_IDLE || s->phase==GWT_FAILED)return;
    uint64_t now=gwt_now(s);
    if(s->phase!=GWT_STOPPING) {
        if(s->wmi->error)gwt_abort(s,GWT_TRANSPORT,0);
        else if(s->phase>=GWT_START_WAIT) {
            struct gwt_channel current={0,0,0};
            if(s->ops.channel(s->ops.context,s->request.bss.frequency,&current) ||
               current.max_power<s->channel.max_power || current.max_reg_power<s->channel.max_reg_power ||
               current.max_antenna_gain<s->channel.max_antenna_gain)gwt_abort(s,GWT_UNAVAILABLE,0);
        }
        if(s->phase!=GWT_STOPPING &&
            ((s->phase!=GWT_RUN && now>=s->deadline) || (s->tx_pending && now>=s->tx_deadline) ||
             (s->key_pending && now>=s->key_deadline)))gwt_abort(s,GWT_TIMEOUT,0);
    }
    if(s->phase==GWT_STOPPING) {
        if(now>=s->deadline || s->wmi->error){if(!s->error)s->error=GWT_TIMEOUT;gwt_phase(s,GWT_FAILED);return;}
        if(!s->stop_stage) {
            s->stop_stage=1;
            struct gwt_channel current={0,0,0};
            /* Regulatory revocation also forbids a final deauthentication
             * transmission. VDEV_DOWN/STOP still tear down the old channel. */
            if(s->vdev_up && !s->tx_pending &&
               !s->ops.channel(s->ops.context,s->request.bss.frequency,&current) &&
               current.max_power>=s->channel.max_power && current.max_reg_power>=s->channel.max_reg_power &&
               current.max_antenna_gain>=s->channel.max_antenna_gain) {
                uint8_t p[26];gwt_frame_header(s,p,0xc0);gwq_w16(p+24,(uint16_t)s->reason);
                if(gwt_mgmt_send(s,p,sizeof(p)))s->tx_failed=1;return;
            }
        }
        if(s->stop_stage==1) {
            if(s->tx_pending && now<s->tx_deadline)return;
            if(s->vdev_up && gwt_vdev_command(s,0x5007,94))return;s->vdev_up=0;
            s->stop_stage=2;
            if(s->vdev_started || s->start_sent) {if(gwt_vdev_command(s,0x5006,93))return;}
            else s->stop_confirm=1;
        }
        if(s->stop_stage==2 && s->stop_confirm) {
            s->vdev_started=0;s->start_sent=0;s->stop_stage=3;
            if(s->peer_created){if(gwt_peer_command(s,0x6002,98))return;
                /* Old FW completes peer removal with HTT PEER_UNMAP only. */
                if(!gww_service(s->wmi,92))s->delete_confirm=1;
            }else s->delete_confirm=1;
        }
        if(s->stop_stage==3 && s->delete_confirm && !s->peer_mapped && !s->tx_pending) {
            s->peer_created=0;s->key_pending=0;s->installed=0;gwt_zero(s->installed_hash,sizeof(s->installed_hash));gwt_zero(s->key_hash,32);
            gwt_phase(s,GWT_IDLE);
        }
        return;
    }
    if(s->key_pending && s->key_confirm) {
        if(s->ops.key_ready(s->ops.context,s->peer_id,s->key_index,s->key_pairwise,s->key_rsc)){gwt_abort(s,GWT_DATAPATH,0);return;}
        unsigned slot=s->key_pairwise?0:s->key_index+1;s->installed|=1u<<slot;
        gwq_copy(s->installed_hash[slot],s->key_hash,32);gwt_zero(s->key_hash,32);s->key_pending=0;s->key_confirm=0;
        if(s->ops.notify)s->ops.notify(s->ops.context,s->phase,GWT_OK);
    }
    if(s->phase==GWT_PEER_WAIT && s->peer_mapped) {
        /* Linux dp.c default routing: hash enable | REO2SW1 << 1. */
        if(gwt_peer_parameter(s,19,3))return;
        for(unsigned tid=0;tid<17;tid++){uint64_t iova=0;
            if(s->ops.reo_queue(s->ops.context,tid,&iova) || gwt_reo(s,tid,iova)){gwt_abort(s,GWT_DATAPATH,0);return;}}
        gwt_phase(s,GWT_START_WAIT);(void)gwt_start(s);
    } else if(s->phase==GWT_START_WAIT && s->start_confirm) {
        s->vdev_started=1;gwt_phase(s,GWT_AUTH_WAIT);
        if(s->request.security==GWT_WPA3_SAE) {
            if(s->security_ops.auth_start(s->security_ops.context))gwt_abort(s,GWT_PROTOCOL,0);return;
        }
        uint8_t p[30]={0};gwt_frame_header(s,p,0xb0);gwq_w16(p+26,1);
        if(gwt_mgmt_send(s,p,sizeof(p)))gwt_abort(s,GWT_DATAPATH,0);
    } else if(s->phase==GWT_AUTH_WAIT && s->auth_confirm && !s->tx_pending) {
        uint8_t p[GWT_ASSOC_MAX]={0};size_t n=gwt_assoc_frame(s,p);gwt_phase(s,GWT_ASSOC_WAIT);
        if(gwt_mgmt_send(s,p,n))gwt_abort(s,GWT_DATAPATH,0);
    } else if(s->phase==GWT_ASSOC_WAIT && s->assoc_confirm && !s->tx_pending) {
        gwt_phase(s,GWT_ASSOC_CONFIRM);(void)gwt_peer_assoc(s);
    } else if(s->phase==GWT_ASSOC_CONFIRM && s->peer_confirm) {
        if(gwt_up(s))return;s->vdev_up=1;
        if(s->request.security)gwt_phase(s,GWT_KEY_WAIT);
        else {
            if(s->ops.ready(s->ops.context,0) || s->ops.port(s->ops.context,s->peer_id,1)){gwt_abort(s,GWT_DATAPATH,0);return;}
            s->authorized=1;gwt_phase(s,GWT_RUN);
        }
    }
}
/* Only call after the platform has proved bus-master/DMA quiescence and
 * discarded the old peer/vdev. This is not a timeout recovery shortcut. */
static inline void gwt_quiesced(struct gwt_state *s) {
    if(!s || !s->wmi)return;
    if(s->tx_pending)s->ops.mgmt_done(s->ops.context,s->tx_cookie,GWT_UNAVAILABLE);
    struct gww_state *wmi=s->wmi;const struct gww_io *io=s->io;struct gwt_ops ops=s->ops;struct gwt_security_ops security_ops=s->security_ops;uint32_t cookie=s->cookie;
    /* Preserve the cookie generation so a delayed old completion cannot
     * release a new management frame after the platform restarts. */
    gwt_zero(s,sizeof(*s));s->wmi=wmi;s->io=io;s->ops=ops;s->security_ops=security_ops;s->cookie=cookie;gwt_phase(s,GWT_IDLE);
}
#endif
