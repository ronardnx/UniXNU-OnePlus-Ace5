/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Bounded SAE group 19 station, H&P or H2E. Serialized worker only: receive
 * authenticated-management bodies here after leaving device RX dispatch.
 * PMK/PMKID are usable only in GSA_ACCEPTED. clear on disassociation.
 */
#ifndef GIULIA_WIFI_SAE_H
#define GIULIA_WIFI_SAE_H
#include "giulia_wifi_sae_crypto.h"
#define GSA_AUTH_MAX 360u
enum {GSA_IDLE,GSA_READY,GSA_COMMITTED,GSA_CONFIRMED,GSA_ACCEPTED,GSA_FAILED};
enum {GSA_OK,GSA_DROP,GSA_ARGUMENT,GSA_CRYPTO,GSA_TRANSPORT,GSA_TIMEOUT,GSA_REJECTED};
struct gsa_ops {
    void *context;
    int (*random)(void *,uint8_t *,size_t);
    /* Copy successful sends. 0 accepted, 1 temporarily busy, negative fatal. */
    int (*send_auth)(void *,const uint8_t *,size_t);
    uint64_t (*now_ms)(void *);
};
struct gsa_state {
    struct gsa_ops ops;unsigned phase,error,h2e,pending,retries,last_bytes,token_bytes;
    uint64_t deadline,retry_at;
    uint8_t local[6],peer[6],pwe[65],rand[32],scalar[32],element[65];
    uint8_t peer_scalar[32],peer_element[65],kck[32],pmk[32],pmkid[16];
    uint8_t last[GSA_AUTH_MAX],token[253],peer_confirm[34];
};
static inline void gsa_zero(void *p,size_t n){volatile uint8_t *b=(volatile uint8_t *)p;while(n--)*b++=0;}
static inline void gsa_copy(void *o,const void *v,size_t n){uint8_t *p=(uint8_t *)o;const uint8_t *q=(const uint8_t *)v;while(n--)*p++=*q++;}
static inline int gsa_equal(const void *a,const void *b,size_t n){const uint8_t *p=(const uint8_t *)a,*q=(const uint8_t *)b;unsigned d=0;while(n--)d|=*p++^*q++;return !d;}
static inline unsigned gsa_le16(const uint8_t *p){return p[0]|((unsigned)p[1]<<8);}
static inline void gsa_w16(uint8_t *p,unsigned v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static inline void gsa_clear(struct gsa_state *s){if(s)gsa_zero(s,sizeof(*s));}
static inline int gsa_fail(struct gsa_state *s,unsigned error){struct gsa_ops ops=s->ops;gsa_clear(s);s->ops=ops;s->phase=GSA_FAILED;s->error=error;return (int)error;}
static inline int gsa_flush(struct gsa_state *s){
    int e=s->ops.send_auth(s->ops.context,s->last,s->last_bytes);if(e<0)return gsa_fail(s,GSA_TRANSPORT);
    s->pending=e!=0;if(!e)s->retry_at=s->ops.now_ms(s->ops.context)+1000;return GSA_OK;
}
static inline void gsa_commit_body(struct gsa_state *s){
    uint8_t *p=s->last;gsa_zero(p,sizeof(s->last));gsa_w16(p,3);gsa_w16(p+2,1);gsa_w16(p+4,s->h2e?126:0);gsa_w16(p+6,19);size_t at=8;
    if(!s->h2e && s->token_bytes){gsa_copy(p+at,s->token,s->token_bytes);at+=s->token_bytes;}
    gsa_copy(p+at,s->scalar,32);gsa_copy(p+at+32,s->element+1,64);at+=96;
    if(s->h2e && s->token_bytes){p[at++]=255;p[at++]=(uint8_t)(1+s->token_bytes);p[at++]=93;gsa_copy(p+at,s->token,s->token_bytes);at+=s->token_bytes;}
    s->last_bytes=(unsigned)at;s->pending=1;
}
static inline int gsa_init(struct gsa_state *s,const struct gsa_ops *ops,const uint8_t local[6],const uint8_t peer[6],
    const uint8_t *password,size_t password_bytes,const uint8_t *ssid,size_t ssid_bytes,unsigned h2e){
    if(!s||!ops||!ops->random||!ops->send_auth||!ops->now_ms||!local||!peer||(local[0]&1)||(peer[0]&1)||gsa_equal(local,peer,6)||
       !password||!password_bytes||password_bytes>64||!ssid||!ssid_bytes||ssid_bytes>32||h2e>1)return GSA_ARGUMENT;
    uint8_t zero[6]={0};if(gsa_equal(local,zero,6)||gsa_equal(peer,zero,6))return GSA_ARGUMENT;
    gsa_clear(s);s->ops=*ops;s->h2e=h2e;gsa_copy(s->local,local,6);gsa_copy(s->peer,peer,6);
    if(gsa_crypto_pwe(local,peer,password,password_bytes,ssid,ssid_bytes,h2e,s->pwe))return gsa_fail(s,GSA_CRYPTO);
    uint8_t mask[32];unsigned valid=0;
    for(unsigned i=0;i<64;i++){
        if(ops->random(ops->context,s->rand,32)||ops->random(ops->context,mask,32)){gsa_zero(mask,32);return gsa_fail(s,GSA_CRYPTO);}
        if(!gsa_crypto_commit(s->pwe,s->rand,mask,s->scalar,s->element)){valid=1;break;}
    }
    gsa_zero(mask,32);if(!valid)return gsa_fail(s,GSA_CRYPTO);s->phase=GSA_READY;return GSA_OK;
}
static inline int gsa_start(struct gsa_state *s){
    if(!s||s->phase!=GSA_READY)return GSA_ARGUMENT;s->phase=GSA_COMMITTED;s->deadline=s->ops.now_ms(s->ops.context)+10000;gsa_commit_body(s);return gsa_flush(s);
}
static inline int gsa_confirm_mac(struct gsa_state *s,const uint8_t count[2],unsigned peer,uint8_t out[32]){
    uint8_t p[194];gsa_copy(p,count,2);gsa_copy(p+2,peer?s->peer_scalar:s->scalar,32);gsa_copy(p+34,(peer?s->peer_element:s->element)+1,64);
    gsa_copy(p+98,peer?s->scalar:s->peer_scalar,32);gsa_copy(p+130,(peer?s->element:s->peer_element)+1,64);
    int e=gsa_crypto_hmac(NULL,s->kck,32,p,sizeof(p),out);gsa_zero(p,sizeof(p));return e;
}
static inline int gsa_receive(struct gsa_state *s,const uint8_t source[6],const uint8_t *p,size_t n){
    if(!s||!source||!p)return GSA_ARGUMENT;if(s->phase<GSA_COMMITTED||s->phase==GSA_FAILED||!gsa_equal(source,s->peer,6)||n<6||n>GSA_AUTH_MAX||gsa_le16(p)!=3)return GSA_DROP;
    unsigned transaction=gsa_le16(p+2),status=gsa_le16(p+4);
    if(transaction==1 && status==76 && s->phase==GSA_COMMITTED){
        if(n<9||gsa_le16(p+6)!=19)return GSA_DROP;const uint8_t *token=p+8;size_t bytes=n-8;
        if(s->h2e){if(bytes<4||token[0]!=255||token[2]!=93||token[1]+2u!=bytes)return GSA_DROP;bytes-=3;token+=3;}
        if(bytes>sizeof(s->token))return GSA_DROP;s->token_bytes=(unsigned)bytes;gsa_copy(s->token,token,bytes);gsa_commit_body(s);return gsa_flush(s);
    }
    if(transaction==1 && status==(s->h2e?126u:0u)){
        /* Group 19 is mandatory; password identifiers, rejected-group lists
         * and SAE-PK are not silently discarded or downgraded. */
        if(n!=104||gsa_le16(p+6)!=19||!gsa_crypto_scalar(p+8))return GSA_DROP;
        if(gsa_equal(p+8,s->scalar,32)&&gsa_equal(p+40,s->element+1,64))return GSA_DROP;
        if(s->phase!=GSA_COMMITTED){
            if(!gsa_equal(p+8,s->peer_scalar,32)||!gsa_equal(p+40,s->peer_element+1,64))return GSA_DROP;
            s->pending=1;return gsa_flush(s);
        }
        uint8_t point[65],k[32],seed[32],sum[32],keys[64],zero[32]={0};point[0]=4;gsa_copy(point+1,p+40,64);
        if(gsa_crypto_shared(s->pwe,s->rand,p+8,point,k)){gsa_zero(point,65);gsa_zero(k,32);return GSA_DROP;}
        gsa_crypto_hmac(NULL,zero,32,k,32,seed);gsa_crypto_add(s->scalar,p+8,sum);gsa_crypto_kdf(seed,"SAE KCK and PMK",sum,32,keys,64);
        gsa_copy(s->kck,keys,32);gsa_copy(s->pmk,keys+32,32);gsa_copy(s->pmkid,sum,16);gsa_copy(s->peer_scalar,p+8,32);gsa_copy(s->peer_element,point,65);
        gsa_zero(k,32);gsa_zero(seed,32);gsa_zero(keys,64);gsa_zero(sum,32);gsa_zero(point,65);
        gsa_zero(s->last,sizeof(s->last));gsa_w16(s->last,3);gsa_w16(s->last+2,2);gsa_w16(s->last+6,1);
        if(gsa_confirm_mac(s,s->last+6,0,s->last+8))return gsa_fail(s,GSA_CRYPTO);
        s->last_bytes=40;s->phase=GSA_CONFIRMED;s->pending=1;s->retries=0;return gsa_flush(s);
    }
    if(transaction==2 && !status && n==40 && (s->phase==GSA_CONFIRMED||s->phase==GSA_ACCEPTED)){
        unsigned count=gsa_le16(p+6);if(!count||count==65535)return GSA_DROP;
        if(s->phase==GSA_ACCEPTED){if(!gsa_equal(s->peer_confirm,p+6,34))return GSA_DROP;return gsa_flush(s);}
        uint8_t digest[32];if(gsa_confirm_mac(s,p+6,1,digest))return gsa_fail(s,GSA_CRYPTO);
        unsigned valid=gsa_equal(digest,p+8,32);gsa_zero(digest,32);if(!valid)return GSA_DROP;
        if(s->pending)return GSA_DROP; /* Our confirmation must be copied first. */
        gsa_copy(s->peer_confirm,p+6,34);s->phase=GSA_ACCEPTED;s->deadline=0;
        gsa_zero(s->rand,32);gsa_zero(s->pwe,65);gsa_zero(s->kck,32);gsa_zero(s->token,sizeof(s->token));s->token_bytes=0;return GSA_OK;
    }
    if(transaction==1 && status && status!=76 && status!=126 && s->phase==GSA_COMMITTED)return gsa_fail(s,GSA_REJECTED);
    return GSA_DROP;
}
static inline int gsa_tick(struct gsa_state *s){
    if(!s)return GSA_ARGUMENT;if(s->phase<GSA_COMMITTED||s->phase==GSA_FAILED||s->phase==GSA_ACCEPTED)return GSA_OK;
    uint64_t now=s->ops.now_ms(s->ops.context);if(now>=s->deadline)return gsa_fail(s,GSA_TIMEOUT);
    if(s->pending)return gsa_flush(s);if(now>=s->retry_at){if(s->retries>=5)return gsa_fail(s,GSA_TIMEOUT);s->retries++;return gsa_flush(s);}return GSA_OK;
}
#endif
