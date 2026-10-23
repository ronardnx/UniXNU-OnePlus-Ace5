/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * PSK/SAE CCMP-128 station RSN. Serialized on the Wi-Fi worker, including
 * receive/tick/reset. All wire frames are EAPOL (without Ethernet headers).
 * The platform must copy a successful send and expose actual FW key ACKs.
 */
#ifndef GIULIA_WIFI_RSN_H
#define GIULIA_WIFI_RSN_H
#include <stddef.h>
#include <stdint.h>
#define GWR_FRAME_MAX 1024u
enum {GWR_IDLE,GWR_WAIT_M1,GWR_WAIT_M3,GWR_PTK_WAIT,GWR_GTK_WAIT,GWR_WAIT_GROUP,
      GWR_RUN,GWR_GROUP_WAIT,GWR_FAILED};
enum {GWR_OK,GWR_DROP,GWR_ARGUMENT,GWR_CRYPTO,GWR_TRANSPORT,GWR_KEY,GWR_TIMEOUT,GWR_PROTOCOL};
struct gwr_ops {
    void *context;
    int (*random32)(void *,uint8_t [32]);
    int (*hmac_sha1)(void *,const uint8_t *,size_t,const uint8_t *,size_t,uint8_t [20]);
    int (*unwrap)(void *,const uint8_t [16],const uint8_t *,size_t,uint8_t *,size_t *);
    int (*send_eapol)(void *,const uint8_t *,size_t);
    /* A successful submission may close the controlled port. key_ready must
     * confirm this submission (or an already-installed identical key). */
    int (*install_key)(void *,unsigned index,unsigned pairwise,uint64_t rsc,const uint8_t [16]);
    /* 1: firmware completion processed; 0: pending; negative: failed. */
    int (*key_ready)(void *,unsigned index,unsigned pairwise);
    /* Idempotent, including when install_key accepted an identical key. */
    int (*authorize)(void *,unsigned open);
    uint64_t (*now_ms)(void *);
};
/* SAE uses SHA256 PRF-384 and AKM-defined AES-CMAC EAPOL MICs, per hostap
 * src/common/wpa_common.c. IGTK installation is synchronous software BIP;
 * the caller must preserve PN on an identical key and reject install errors. */
struct gwr_security {
    unsigned sae,pmf,h2e;
    int (*hmac_sha256)(void *,const uint8_t *,size_t,const uint8_t *,size_t,uint8_t [32]);
    int (*cmac)(void *,const uint8_t [16],const uint8_t *,size_t,uint8_t [16]);
    int (*install_igtk)(void *,unsigned,uint64_t,const uint8_t [16]);
};
struct gwr_igtk {unsigned present,index;uint64_t ipn;uint8_t key[16];};
struct gwr_state {
    struct gwr_ops ops;struct gwr_security security;
    unsigned phase,error,authorized,have_replay,m3_seen,key_submitted,ptk_installed;
    unsigned gtk_valid,gtk_index,pending_has_gtk,pending_index,pending_kind;
    unsigned assoc_bytes,ap_bytes,last_tx_bytes,retries;
    uint64_t replay,deadline,retry_at,pending_rsc;
    uint8_t local[6],ap[6],pmk[32],ptk[64],snonce[32],anonce[32];
    uint8_t assoc_rsn[257],ap_rsn[257],gtk[4][16],pending_gtk[16],m3_gtk[16];
    unsigned m3_has_gtk,m3_index;
    unsigned igtk_valid;struct gwr_igtk pending_igtk,m3_igtk;
    uint8_t last_mic[16],last_tx[359];
};
static inline void gwr_zero(void *p,size_t n){volatile uint8_t *b=(volatile uint8_t *)p;while(n--)*b++=0;}
static inline void gwr_copy(void *out,const void *in,size_t n){uint8_t *a=(uint8_t *)out;const uint8_t *b=(const uint8_t *)in;while(n--)*a++=*b++;}
static inline int gwr_equal(const uint8_t *a,const uint8_t *b,size_t n){unsigned d=0;for(size_t i=0;i<n;i++)d|=a[i]^b[i];return !d;}
static inline unsigned gwr_nonzero(const uint8_t *a,size_t n){unsigned d=0;for(size_t i=0;i<n;i++)d|=a[i];return d;}
static inline unsigned gwr_be16(const uint8_t *p){return ((unsigned)p[0]<<8)|p[1];}
static inline unsigned gwr_le16(const uint8_t *p){return p[0]|((unsigned)p[1]<<8);}
static inline void gwr_w16(uint8_t *p,unsigned v){p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v;}
static inline uint64_t gwr_be64(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v=(v<<8)|p[i];return v;}
static inline void gwr_w64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[7-i]=(uint8_t)(v>>(8*i));}
static inline int gwr_less(const uint8_t *a,const uint8_t *b,size_t n){for(size_t i=0;i<n;i++)if(a[i]!=b[i])return a[i]<b[i];return 0;}
static inline uint64_t gwr_now(const struct gwr_state *s){return s->ops.now_ms(s->ops.context);}
static inline void gwr_clear(struct gwr_state *s){if(!s)return;if(s->ops.authorize)(void)s->ops.authorize(s->ops.context,0);gwr_zero(s,sizeof(*s));}
static inline int gwr_fail(struct gwr_state *s,unsigned error){
    struct gwr_ops ops=s->ops;(void)ops.authorize(ops.context,0);gwr_zero(s,sizeof(*s));
    s->ops=ops;s->phase=GWR_FAILED;s->error=error;return (int)error;
}
/* Validate a complete RSN IE and its optional fields. Only the station IE
 * is restricted to one pairwise cipher/AKM; the AP may advertise more. */
static inline int gwr_rsn_valid_secure(const uint8_t *p,size_t n,unsigned station,unsigned sae,unsigned pmf){
    static const uint8_t ccmp[4]={0,15,172,4},bip[4]={0,15,172,6};
    uint8_t akm[4]={0,15,172,(uint8_t)(sae?8:2)};
    if(sae>1 || pmf>2 || (sae && pmf!=2))return 0;
    if(!p || n<20 || n>257 || p[0]!=48 || p[1]+2u!=n || gwr_le16(p+2)!=1 || !gwr_equal(p+4,ccmp,4))return 0;
    size_t at=8;unsigned count=gwr_le16(p+at);at+=2;
    if(!count || count>(n-at)/4 || (station && count!=1))return 0;
    unsigned found=0;for(unsigned i=0;i<count;i++,at+=4)found|=gwr_equal(p+at,ccmp,4);
    if(!found || n-at<2)return 0;count=gwr_le16(p+at);at+=2;
    if(!count || count>(n-at)/4 || (station && count!=1))return 0;
    found=0;for(unsigned i=0;i<count;i++,at+=4)found|=gwr_equal(p+at,akm,4);
    if(!found)return 0;if(at==n)return !pmf;if(n-at<2)return 0;
    unsigned capabilities=gwr_le16(p+at);at+=2;
    if((capabilities&0x40) && !(capabilities&0x80))return 0;
    if((capabilities&0x40) && !pmf)return 0;
    if(pmf && !(capabilities&0x80))return 0;
    if(station && ((!pmf && (capabilities&0xc0)) || (pmf==2 && !(capabilities&0x40))))return 0;
    if(at==n)return 1;if(n-at<2)return 0;count=gwr_le16(p+at);at+=2;
    if(count>(n-at)/16)return 0;at+=16*count;
    return at==n || (n-at==4 && (!pmf || gwr_equal(p+at,bip,4)));
}
static inline int gwr_rsn_valid(const uint8_t *p,size_t n,unsigned station){return gwr_rsn_valid_secure(p,n,station,0,0);}
static inline int gwr_init_secure(struct gwr_state *s,const struct gwr_ops *ops,const struct gwr_security *security,const uint8_t pmk[32],
    const uint8_t local[6],const uint8_t ap[6],const uint8_t *assoc_rsn,size_t assoc_bytes,
    const uint8_t *ap_rsn,size_t ap_bytes){
    if(!s || !ops || !security || !pmk || !local || !ap || (local[0]&1) || (ap[0]&1) ||
       !gwr_nonzero(local,6) || !gwr_nonzero(ap,6) || gwr_equal(local,ap,6) ||
       !ops->random32 || !ops->hmac_sha1 || !ops->unwrap || !ops->send_eapol || !ops->install_key ||
       !ops->key_ready || !ops->authorize || !ops->now_ms ||
       security->h2e>1 || (security->h2e && !security->sae) ||
       (security->sae && (!security->hmac_sha256 || !security->cmac)) || (security->pmf && !security->install_igtk) ||
       !gwr_rsn_valid_secure(assoc_rsn,assoc_bytes,1,security->sae,security->pmf) ||
       !gwr_rsn_valid_secure(ap_rsn,ap_bytes,0,security->sae,security->pmf))return GWR_ARGUMENT;
    gwr_zero(s,sizeof(*s));s->ops=*ops;s->security=*security;gwr_copy(s->pmk,pmk,32);gwr_copy(s->local,local,6);gwr_copy(s->ap,ap,6);
    gwr_copy(s->assoc_rsn,assoc_rsn,assoc_bytes);s->assoc_bytes=(unsigned)assoc_bytes;
    gwr_copy(s->ap_rsn,ap_rsn,ap_bytes);s->ap_bytes=(unsigned)ap_bytes;
    s->phase=GWR_WAIT_M1;s->deadline=gwr_now(s)+10000;
    if(s->ops.authorize(s->ops.context,0))return gwr_fail(s,GWR_KEY);return GWR_OK;
}
static inline int gwr_init(struct gwr_state *s,const struct gwr_ops *ops,const uint8_t pmk[32],
    const uint8_t local[6],const uint8_t ap[6],const uint8_t *assoc_rsn,size_t assoc_bytes,const uint8_t *ap_rsn,size_t ap_bytes){
    struct gwr_security security={0,0,0,NULL,NULL,NULL};return gwr_init_secure(s,ops,&security,pmk,local,ap,assoc_rsn,assoc_bytes,ap_rsn,ap_bytes);
}
/* IEEE 802.11 PRF-512: label including its NUL, sorted MACs/nonces,
 * then a one-byte counter beginning at zero. KCK/KEK/TK are bytes 0/16/32. */
static inline int gwr_ptk(struct gwr_state *s){
    static const uint8_t label[]="Pairwise key expansion";uint8_t data[100],digest[20];
    size_t at=sizeof(label);gwr_copy(data,label,at);
    const uint8_t *first=gwr_less(s->local,s->ap,6)?s->local:s->ap,*second=first==s->local?s->ap:s->local;
    gwr_copy(data+at,first,6);gwr_copy(data+at+6,second,6);at+=12;
    first=gwr_less(s->snonce,s->anonce,32)?s->snonce:s->anonce;second=first==s->snonce?s->anonce:s->snonce;
    gwr_copy(data+at,first,32);gwr_copy(data+at+32,second,32);at+=64;
    if(s->security.sae){
        /* Counter/bit length are LE16; the label excludes NUL. SAE CCMP
         * has a 16-byte KCK, 16-byte KEK, 16-byte TK (384 bits total). */
        uint8_t input[sizeof(label)+79],hash[32];size_t label_bytes=sizeof(label)-1;
        input[0]=1;input[1]=0;gwr_copy(input+2,label,label_bytes);gwr_copy(input+2+label_bytes,data+sizeof(label),76);
        input[2+label_bytes+76]=0x80;input[3+label_bytes+76]=1;
        int error=s->security.hmac_sha256(s->ops.context,s->pmk,32,input,sizeof(input),hash);
        if(!error){gwr_copy(s->ptk,hash,32);input[0]=2;error=s->security.hmac_sha256(s->ops.context,s->pmk,32,input,sizeof(input),hash);}
        if(!error){gwr_copy(s->ptk+32,hash,16);gwr_zero(s->ptk+48,16);}
        gwr_zero(input,sizeof(input));gwr_zero(hash,sizeof(hash));gwr_zero(data,sizeof(data));gwr_zero(digest,sizeof(digest));return error;
    }
    int result=0;for(unsigned i=0;i<4;i++){
        data[at]=(uint8_t)i;if(s->ops.hmac_sha1(s->ops.context,s->pmk,32,data,at+1,digest)){result=-1;break;}
        gwr_copy(s->ptk+20*i,digest,i==3?4:20);
    }
    gwr_zero(data,sizeof(data));gwr_zero(digest,sizeof(digest));return result;
}
static inline int gwr_mic(struct gwr_state *s,uint8_t *p,size_t n){
    if(s->security.sae){uint8_t digest[16];gwr_zero(p+81,16);int result=s->security.cmac(s->ops.context,s->ptk,p,n,digest);
        if(!result)gwr_copy(p+81,digest,16);gwr_zero(digest,sizeof(digest));return result;}
    uint8_t digest[20];gwr_zero(p+81,16);int result=s->ops.hmac_sha1(s->ops.context,s->ptk,16,p,n,digest);
    if(!result)gwr_copy(p+81,digest,16);gwr_zero(digest,sizeof(digest));return result;
}
static inline int gwr_response(struct gwr_state *s,unsigned kind){
    uint8_t *p=s->last_tx;gwr_zero(p,sizeof(s->last_tx));unsigned bytes=99;
    p[0]=2;p[1]=3;p[4]=2;unsigned info=kind==2?0x010a:kind==4?0x030a:0x0302;
    if(s->security.sae)info&=~7u;gwr_w16(p+5,info);
    /* Key Length is zero in RSN responses (802.11-2020 12.7.6.2). */
    gwr_w64(p+9,s->replay);
    if(kind==2){gwr_copy(p+17,s->snonce,32);gwr_copy(p+99,s->assoc_rsn,s->assoc_bytes);bytes+=s->assoc_bytes;
        if(s->security.h2e){p[bytes++]=244;p[bytes++]=1;p[bytes++]=0x20;}gwr_w16(p+97,bytes-99);}
    gwr_w16(p+2,bytes-4);if(gwr_mic(s,p,bytes))return gwr_fail(s,GWR_CRYPTO);
    s->last_tx_bytes=bytes;s->retry_at=gwr_now(s)+1000;s->retries=0;
    if(s->ops.send_eapol(s->ops.context,p,bytes))return gwr_fail(s,GWR_TRANSPORT);return GWR_OK;
}
static inline int gwr_resend(struct gwr_state *s){
    if(s->last_tx_bytes && s->ops.send_eapol(s->ops.context,s->last_tx,s->last_tx_bytes))return gwr_fail(s,GWR_TRANSPORT);
    return GWR_OK;
}
/* Parse decrypted KDEs. The AP RSN IE must match the selected scan entry,
 * not the station's narrower association RSN IE. */
static inline int gwr_key_data(struct gwr_state *s,const uint8_t *p,size_t n,unsigned pairwise,
    uint8_t gtk[16],unsigned *has_gtk,unsigned *index,struct gwr_igtk *igtk){
    unsigned rsn=0,rsnxe=0;*has_gtk=0;*index=0;gwr_zero(igtk,sizeof(*igtk));
    for(size_t at=0;at<n;){
        if(p[at]==221 && (n-at==1 || !p[at+1])){
            for(size_t i=at+1;i<n;i++)if(p[i])return 0;break;
        }
        if(n-at<2 || p[at+1]>n-at-2)return 0;unsigned tag=p[at],bytes=p[at+1];
        const uint8_t *v=p+at+2;
        if(tag==48){if(rsn++ || !pairwise || bytes+2u!=s->ap_bytes || !gwr_equal(p+at,s->ap_rsn,s->ap_bytes))return 0;}
        if(tag==244){if(rsnxe++ || !pairwise || !bytes || (v[0]&15)+1u!=bytes || (s->security.h2e && !(v[0]&0x20)))return 0;}
        if(tag==221 && bytes>=4 && !v[0] && v[1]==15 && v[2]==172 && v[3]==1){
            if(*has_gtk || bytes!=22 || (v[4]&0xf8) || v[5])return 0;
            *index=v[4]&3;*has_gtk=1;gwr_copy(gtk,v+6,16);
        }
        if(tag==221 && bytes>=4 && !v[0] && v[1]==15 && v[2]==172 && v[3]==9){
            if(!s->security.pmf || igtk->present || bytes!=28)return 0;
            igtk->index=gwr_le16(v+4);if(igtk->index<4 || igtk->index>5)return 0;
            igtk->present=1;for(unsigned i=0;i<6;i++)igtk->ipn|=(uint64_t)v[6+i]<<(8*i);
            gwr_copy(igtk->key,v+12,16);
        }
        at+=2+bytes;
    }
    return pairwise?(rsn==1 && (!s->security.h2e || rsnxe)):(*has_gtk || igtk->present);
}
static inline int gwr_finish(struct gwr_state *s){
    if(s->pending_igtk.present){
        if(s->security.install_igtk(s->ops.context,s->pending_igtk.index,s->pending_igtk.ipn,s->pending_igtk.key))return gwr_fail(s,GWR_KEY);
        s->igtk_valid=1;gwr_zero(&s->pending_igtk,sizeof(s->pending_igtk));
    }
    unsigned kind=s->pending_kind;int rc=gwr_response(s,kind==3?4:6);if(rc)return rc;
    if(!s->gtk_valid || (s->security.pmf && !s->igtk_valid)){s->phase=GWR_WAIT_GROUP;s->deadline=gwr_now(s)+10000;return GWR_OK;}
    if(!s->authorized && s->ops.authorize(s->ops.context,1))return gwr_fail(s,GWR_KEY);
    s->authorized=1;s->phase=GWR_RUN;s->deadline=0;gwr_zero(s->ptk+32,32);gwr_zero(s->pending_gtk,16);
    return GWR_OK;
}
static inline int gwr_receive(struct gwr_state *s,const uint8_t source[6],const uint8_t *frame,size_t bytes){
    if(!s || !source || !frame)return GWR_ARGUMENT;
    if(s->phase==GWR_IDLE || s->phase==GWR_FAILED || !gwr_equal(source,s->ap,6) || bytes<99 || bytes>GWR_FRAME_MAX ||
       (frame[0]!=1 && frame[0]!=2 && frame[0]!=3) || frame[1]!=3 || frame[4]!=2 ||
       gwr_be16(frame+2)!=bytes-4 || gwr_be16(frame+97)!=bytes-99)return GWR_DROP;
    unsigned info=gwr_be16(frame+5),key_length=gwr_be16(frame+7);uint64_t replay=gwr_be64(frame+9);
    if((info&7)!=(s->security.sae?0u:2u))return GWR_DROP;info=(info&~7u)|2u;
    if(key_length!=16 || (s->have_replay && replay<s->replay))return GWR_DROP;
    if(info==0x008a){
        if(!gwr_nonzero(frame+17,32) || s->phase==GWR_PTK_WAIT || s->phase==GWR_GTK_WAIT ||
           s->phase==GWR_GROUP_WAIT || s->phase==GWR_WAIT_GROUP)return GWR_DROP;
        unsigned same=s->phase==GWR_WAIT_M3 && gwr_equal(s->anonce,frame+17,32);
        if(s->have_replay && replay==s->replay && !same)return GWR_DROP;
        if(!same){
            if(s->ops.random32(s->ops.context,s->snonce) || !gwr_nonzero(s->snonce,32))return gwr_fail(s,GWR_CRYPTO);
            gwr_copy(s->anonce,frame+17,32);if(gwr_ptk(s))return gwr_fail(s,GWR_CRYPTO);
            s->m3_seen=0;s->ptk_installed=0;s->deadline=gwr_now(s)+10000;
        }
        s->replay=replay;s->have_replay=1;s->phase=GWR_WAIT_M3;return gwr_response(s,2);
    }
    unsigned pairwise=info==0x13ca || info==0x03ca,group=info==0x1382;
    if((!pairwise && !group) || !s->have_replay || s->phase==GWR_WAIT_M1 ||
       (pairwise && (!gwr_equal(s->anonce,frame+17,32) || s->phase==GWR_GROUP_WAIT)) ||
       (group && s->phase!=GWR_RUN && s->phase!=GWR_WAIT_GROUP && s->phase!=GWR_GROUP_WAIT))return GWR_DROP;
    uint8_t copy[GWR_FRAME_MAX],gtk[16]={0};struct gwr_igtk igtk;gwr_zero(&igtk,sizeof(igtk));gwr_copy(copy,frame,bytes);
    int mic=gwr_mic(s,copy,bytes);unsigned valid=!mic && gwr_equal(copy+81,frame+81,16);
    gwr_zero(copy,bytes);if(mic)return gwr_fail(s,GWR_CRYPTO);if(!valid)return GWR_DROP;
    if(replay==s->replay){
        if(s->pending_kind!=(pairwise?3u:1u) || !gwr_equal(s->last_mic,frame+81,16))return GWR_DROP;
        if(s->phase==GWR_RUN || s->phase==GWR_WAIT_GROUP)return gwr_resend(s);
        return GWR_OK; /* Same authenticated request: never reset install/PN. */
    }
    size_t plain=sizeof(copy);unsigned has_gtk=0,index=0;
    if(!(info&0x1000)){plain=bytes-99;gwr_copy(copy,frame+99,plain);}
    else if(s->ops.unwrap(s->ops.context,s->ptk+16,frame+99,bytes-99,copy,&plain) || plain>sizeof(copy)){
        gwr_zero(copy,sizeof(copy));return GWR_DROP;
    }
    valid=gwr_key_data(s,copy,plain,pairwise,gtk,&has_gtk,&index,&igtk);gwr_zero(copy,sizeof(copy));
    if(!valid || ((has_gtk || igtk.present) && !(info&0x1000)) || frame[71] || frame[72]){gwr_zero(gtk,16);gwr_zero(&igtk,sizeof(igtk));return GWR_DROP;}
    if(pairwise && s->m3_seen){
        if(has_gtk!=s->m3_has_gtk || (has_gtk && (index!=s->m3_index || !gwr_equal(gtk,s->m3_gtk,16))) ||
           igtk.present!=s->m3_igtk.present || (igtk.present && (igtk.index!=s->m3_igtk.index || !gwr_equal(igtk.key,s->m3_igtk.key,16)))){
            gwr_zero(gtk,16);gwr_zero(&igtk,sizeof(igtk));return GWR_DROP;
        }
        /* Authenticator retries may increment replay; update only the reply. */
        s->pending_kind=3;s->replay=replay;gwr_copy(s->last_mic,frame+81,16);gwr_zero(gtk,16);gwr_zero(&igtk,sizeof(igtk));
        if(s->phase==GWR_RUN || s->phase==GWR_WAIT_GROUP)return gwr_response(s,4);return GWR_OK;
    }
    if(group && s->phase==GWR_GROUP_WAIT){
        /* Keep the original install/RSC and timeout while replying to the
         * latest authenticated retry once firmware acknowledges the key. */
        valid=has_gtk==s->pending_has_gtk && index==s->pending_index && gwr_equal(gtk,s->pending_gtk,16) &&
            igtk.present==s->pending_igtk.present && (!igtk.present || (igtk.index==s->pending_igtk.index && gwr_equal(igtk.key,s->pending_igtk.key,16)));
        gwr_zero(gtk,16);gwr_zero(&igtk,sizeof(igtk));if(!valid)return GWR_DROP;
        s->replay=replay;gwr_copy(s->last_mic,frame+81,16);return GWR_OK;
    }
    /* A key reused at a different index would also reset that key's PN. */
    for(unsigned i=0;i<4;i++)if(has_gtk && i!=index && (s->gtk_valid&(1u<<i)) && gwr_equal(s->gtk[i],gtk,16)){
        gwr_zero(gtk,16);gwr_zero(&igtk,sizeof(igtk));return GWR_DROP;
    }
    s->pending_has_gtk=has_gtk;s->pending_index=index;s->pending_rsc=0;
    for(unsigned i=0;i<6;i++)s->pending_rsc|=(uint64_t)frame[65+i]<<(8*i);
    gwr_copy(s->pending_gtk,gtk,16);gwr_zero(gtk,16);s->pending_igtk=igtk;gwr_zero(&igtk,sizeof(igtk));s->pending_kind=pairwise?3:1;
    s->replay=replay;gwr_copy(s->last_mic,frame+81,16);s->key_submitted=0;s->deadline=gwr_now(s)+5000;
    if(pairwise){
        s->m3_seen=1;s->m3_has_gtk=has_gtk;s->m3_index=index;gwr_copy(s->m3_gtk,s->pending_gtk,16);s->m3_igtk=s->pending_igtk;
        if(s->authorized && s->ops.authorize(s->ops.context,0))return gwr_fail(s,GWR_KEY);
        s->authorized=0;s->phase=GWR_PTK_WAIT;
    }else s->phase=GWR_GROUP_WAIT;
    return GWR_OK;
}
static inline int gwr_tick(struct gwr_state *s){
    if(!s)return GWR_ARGUMENT;if(s->phase==GWR_IDLE || s->phase==GWR_FAILED)return GWR_OK;
    uint64_t now=gwr_now(s);
    if(s->deadline && now>=s->deadline)return gwr_fail(s,GWR_TIMEOUT);
    if(s->phase==GWR_WAIT_M3){
        if(now>=s->retry_at && s->retries<3){s->retries++;s->retry_at=now+1000;return gwr_resend(s);}
        return GWR_OK;
    }
    if(s->phase!=GWR_PTK_WAIT && s->phase!=GWR_GTK_WAIT && s->phase!=GWR_GROUP_WAIT)return GWR_OK;
    unsigned pairwise=s->phase==GWR_PTK_WAIT,index=pairwise?0:s->pending_index;
    if(!pairwise && (!s->pending_has_gtk || ((s->gtk_valid&(1u<<index)) && gwr_equal(s->gtk[index],s->pending_gtk,16))))return gwr_finish(s);
    if(!s->key_submitted){
        if(s->ops.install_key(s->ops.context,index,pairwise,pairwise?0:s->pending_rsc,pairwise?s->ptk+32:s->pending_gtk))return gwr_fail(s,GWR_KEY);
        /* Firmware installation may close the port for GTK rekey too.
         * Reopen only after its completion, including successful no-ops. */
        s->authorized=0;s->key_submitted=1;return GWR_OK;
    }
    int ready=s->ops.key_ready(s->ops.context,index,pairwise);if(ready<0)return gwr_fail(s,GWR_KEY);if(!ready)return GWR_OK;
    s->key_submitted=0;
    if(pairwise){s->ptk_installed=1;s->phase=GWR_GTK_WAIT;gwr_zero(s->ptk+32,32);return GWR_OK;}
    gwr_copy(s->gtk[index],s->pending_gtk,16);s->gtk_valid|=1u<<index;s->gtk_index=index;return gwr_finish(s);
}
#endif
