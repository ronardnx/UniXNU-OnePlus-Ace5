/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * PMF for one CCMP-128 station: BIP-CMAC-128 IGTK, replay checks and SA Query.
 * WCN7850 WMI management TX/RX format follows Linux v6.12 ath12k mac.c/wmi.c:
 * TX uses host-generated CCMP IV and MIC tailroom, firmware encrypts/authenticates;
 * successful RX has plaintext with the Protected flag, IV and MIC retained.
 * This module never labels unverified software plaintext as protected RX.
 */
#ifndef GIULIA_WIFI_PMF_H
#define GIULIA_WIFI_PMF_H
#include "giulia_wifi_sha256.h"
#define GPM_FRAME_MAX 1024u
enum {GPM_OK,GPM_DROP,GPM_ARGUMENT,GPM_CRYPTO,GPM_TIMEOUT,GPM_KEY};
struct gpm_ops {
    void *context;
    int (*cmac)(void *,const uint8_t [16],const uint8_t *,size_t,uint8_t [16]);
    int (*random)(void *,uint8_t *,size_t);
    uint64_t (*now_ms)(void *);
};
struct gpm_state {
    struct gpm_ops ops;uint8_t local[6],peer[6],igtk[2][16],ptk_hash[32];
    uint64_t ipn[2],rx_pn,tx_pn,deadline,retry_at;
    unsigned initialized,ptk_valid,igtk_valid,error,query_active,pending,action,sequence;
    uint8_t transaction[2],pending_transaction[2];
};
static inline void gpm_zero(void *p,size_t n){volatile uint8_t *b=(volatile uint8_t *)p;while(n--)*b++=0;}
static inline void gpm_copy(void *v,const void *q,size_t n){uint8_t *a=(uint8_t *)v;const uint8_t *b=(const uint8_t *)q;while(n--)*a++=*b++;}
static inline int gpm_equal(const uint8_t *a,const uint8_t *b,size_t n){unsigned d=0;for(size_t i=0;i<n;i++)d|=a[i]^b[i];return !d;}
static inline unsigned gpm_u16(const uint8_t *p){return p[0]|((unsigned)p[1]<<8);}
static inline void gpm_w16(uint8_t *p,unsigned n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static inline uint64_t gpm_u48(const uint8_t *p){uint64_t n=0;for(unsigned i=0;i<6;i++)n|=(uint64_t)p[i]<<(8*i);return n;}
static inline void gpm_clear(struct gpm_state *s){if(s)gpm_zero(s,sizeof(*s));}
static inline int gpm_init(struct gpm_state *s,const struct gpm_ops *ops,const uint8_t local[6],const uint8_t peer[6]){
    if(!s || !ops || !ops->cmac || !ops->random || !ops->now_ms || !local || !peer ||
       (local[0]&1) || (peer[0]&1) || gpm_equal(local,peer,6))return GPM_ARGUMENT;
    unsigned a=0,b=0;for(unsigned i=0;i<6;i++){a|=local[i];b|=peer[i];}if(!a || !b)return GPM_ARGUMENT;
    gpm_clear(s);s->ops=*ops;gpm_copy(s->local,local,6);gpm_copy(s->peer,peer,6);s->initialized=1;return GPM_OK;
}
/* Call only after the matching firmware PTK completion. Same-key EAPOL
 * retries preserve management PN state, including an active SA Query. */
static inline int gpm_install_pairwise(struct gpm_state *s,const uint8_t key[16]){
    if(!s || !s->initialized || !key)return GPM_ARGUMENT;uint8_t hash[32];gwh_sha256(key,16,hash);
    if(!s->ptk_valid || !gpm_equal(hash,s->ptk_hash,32)){
        gpm_copy(s->ptk_hash,hash,32);s->rx_pn=s->tx_pn=0;s->query_active=s->pending=0;s->ptk_valid=1;
    }
    gpm_zero(hash,32);return GPM_OK;
}
/* KDE was authenticated/decrypted by the RSN four-way/group handshake. */
static inline int gpm_install_igtk(struct gpm_state *s,unsigned index,uint64_t ipn,const uint8_t key[16]){
    if(!s || !s->initialized || !key || index<4 || index>5 || ipn>UINT64_C(0xffffffffffff))return GPM_ARGUMENT;
    unsigned slot=index-4;
    if((s->igtk_valid&(1u<<(slot^1))) && gpm_equal(key,s->igtk[slot^1],16))return GPM_KEY;
    if((s->igtk_valid&(1u<<slot)) && gpm_equal(key,s->igtk[slot],16)){
        if(ipn>s->ipn[slot])s->ipn[slot]=ipn;return GPM_OK;
    }
    gpm_copy(s->igtk[slot],key,16);s->ipn[slot]=ipn;s->igtk_valid|=1u<<slot;return GPM_OK;
}
static inline int gpm_ready(const struct gpm_state *s){return s && s->initialized && !s->error && s->ptk_valid && s->igtk_valid;}
static inline void gpm_queue(struct gpm_state *s,unsigned action,const uint8_t transaction[2]){
    /* Responses take precedence over the next retry of our own query. */
    if(s->pending && s->action==1 && !action)return;
    s->pending=1;s->action=action;gpm_copy(s->pending_transaction,transaction,2);
}
static inline int gpm_query_start(struct gpm_state *s){
    if(s->query_active)return GPM_OK;
    if(s->ops.random(s->ops.context,s->transaction,2)){s->error=GPM_CRYPTO;return GPM_CRYPTO;}
    uint64_t now=s->ops.now_ms(s->ops.context);s->query_active=1;s->deadline=now+1000;s->retry_at=now+200;
    gpm_queue(s,0,s->transaction);return GPM_OK;
}
static inline int gpm_tick(struct gpm_state *s){
    if(!s || !s->initialized)return GPM_ARGUMENT;if(s->error)return (int)s->error;if(!s->query_active)return GPM_OK;
    uint64_t now=s->ops.now_ms(s->ops.context);
    if(now>=s->deadline){s->error=GPM_TIMEOUT;s->pending=0;return GPM_TIMEOUT;}
    if(now>=s->retry_at){s->retry_at=now+200;gpm_queue(s,0,s->transaction);}return GPM_OK;
}
/* Dequeues a plaintext SA Query for the worker. The worker retains it on a
 * busy management queue and sends it through gwt_mgmt_send/gpm_protect. */
static inline int gpm_next_tx(struct gpm_state *s,uint8_t *out,size_t *bytes){
    if(!s || !out || !bytes || *bytes<28 || !s->initialized)return GPM_ARGUMENT;
    *bytes=0;if(!s->pending || s->error)return GPM_DROP;gpm_zero(out,28);out[0]=0xd0;
    gpm_copy(out+4,s->peer,6);gpm_copy(out+10,s->local,6);gpm_copy(out+16,s->peer,6);
    gpm_w16(out+22,(s->sequence++&0xfff)<<4);out[24]=8;out[25]=(uint8_t)s->action;gpm_copy(out+26,s->pending_transaction,2);
    s->pending=0;*bytes=28;return GPM_OK;
}
static inline int gpm_protect(struct gpm_state *s,const uint8_t *frame,size_t n,uint8_t *out,size_t *bytes){
    if(!s || !s->initialized || !frame || !out || !bytes || n<24 || n>GPM_FRAME_MAX || *bytes<n)return GPM_ARGUMENT;
    size_t capacity=*bytes;*bytes=0;unsigned type=frame[0]&0xfc;
    if(type!=0xc0 && type!=0xa0 && !(type==0xd0 && n>=25 && frame[24]==8)){
        gpm_copy(out,frame,n);*bytes=n;return GPM_OK;
    }
    if(s->error || !gpm_equal(frame+4,s->peer,6) || !gpm_equal(frame+10,s->local,6) ||
       !gpm_equal(frame+16,s->peer,6) || (frame[1]&0xc7))return GPM_DROP;
    if(!s->ptk_valid){gpm_copy(out,frame,n);*bytes=n;return GPM_OK;}
    if(capacity<n+16 || n+16>GPM_FRAME_MAX || s->tx_pn==UINT64_C(0xffffffffffff))return GPM_KEY;
    uint64_t pn=++s->tx_pn;gpm_copy(out,frame,24);out[1]|=0x40;
    out[24]=(uint8_t)pn;out[25]=(uint8_t)(pn>>8);out[26]=0;out[27]=0x20;
    for(unsigned i=0;i<4;i++)out[28+i]=(uint8_t)(pn>>(16+8*i));
    gpm_copy(out+32,frame+24,n-24);gpm_zero(out+n+8,8);*bytes=n+16;return GPM_OK;
}
static inline int gpm_filter(struct gpm_state *s,const uint8_t *frame,size_t n,unsigned fw_status,uint8_t *out,size_t *bytes){
    if(!s || !s->initialized || !frame || !out || !bytes || n<24 || n>GPM_FRAME_MAX || *bytes<n)return GPM_ARGUMENT;
    *bytes=0;if(s->error || fw_status || (frame[0]&0x0f) || (frame[1]&0x87) || (frame[22]&15) ||
       !gpm_equal(frame+10,s->peer,6) || !gpm_equal(frame+16,s->peer,6))return GPM_DROP;
    unsigned type=frame[0]&0xfc,group=frame[4]&1;
    if(!group && !gpm_equal(frame+4,s->local,6))return GPM_DROP;
    if(type!=0xc0 && type!=0xa0 && type!=0xd0){if(frame[1]&0x40)return GPM_DROP;gpm_copy(out,frame,n);*bytes=n;return GPM_OK;}
    if(n<26)return GPM_DROP;
    if(!s->ptk_valid){if(frame[1]&0x40)return GPM_DROP;gpm_copy(out,frame,n);*bytes=n;return GPM_OK;}
    size_t used=n;
    if(group){
        if(type==0xd0 || (frame[1]&0x40) || n<44)return GPM_DROP;
        const uint8_t *mmie=frame+n-18;if(mmie[0]!=76 || mmie[1]!=16)return GPM_DROP;
        unsigned index=gpm_u16(mmie+2);if(index<4 || index>5 || !(s->igtk_valid&(1u<<(index-4))))return GPM_DROP;
        unsigned slot=index-4;uint64_t ipn=gpm_u48(mmie+4);if(ipn<=s->ipn[slot])return GPM_DROP;
        uint8_t auth[GPM_FRAME_MAX],mic[16];auth[0]=frame[0];auth[1]=frame[1]&0xc7;gpm_copy(auth+2,frame+4,18);
        gpm_copy(auth+20,frame+24,n-24);gpm_zero(auth+n-12,8);
        int error=s->ops.cmac(s->ops.context,s->igtk[slot],auth,n-4,mic);gpm_zero(auth,sizeof(auth));
        unsigned match=!error && gpm_equal(mic,mmie+10,8);gpm_zero(mic,16);if(!match)return GPM_DROP;
        s->ipn[slot]=ipn;used=n-18;gpm_copy(out,frame,used);
    } else if(frame[1]&0x40){
        if(n<42 || frame[26] || frame[27]!=0x20)return GPM_DROP;
        uint64_t pn=(uint64_t)gpm_u16(frame+24);for(unsigned i=0;i<4;i++)pn|=(uint64_t)frame[28+i]<<(16+8*i);
        if(!pn || pn<=s->rx_pn)return GPM_DROP;
        used=n-16;gpm_copy(out,frame,24);gpm_copy(out+24,frame+32,n-40);out[1]&=(uint8_t)~0x40;
        s->rx_pn=pn;
    } else {
        if(type!=0xd0 && (gpm_u16(frame+24)==6 || gpm_u16(frame+24)==7))(void)gpm_query_start(s);
        return GPM_DROP;
    }
    if(type==0xd0){
        if(used!=28 || out[24]!=8 || out[25]>1)return GPM_DROP;
        if(!out[25])gpm_queue(s,1,out+26);
        else if(s->query_active && gpm_equal(out+26,s->transaction,2)){
            s->query_active=0;if(s->pending && !s->action)s->pending=0;
        }
        return GPM_DROP; /* consumed; never reinterpret action as disconnect */
    }
    *bytes=used;return GPM_OK;
}
#endif
