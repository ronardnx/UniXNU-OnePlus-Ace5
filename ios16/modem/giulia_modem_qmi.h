/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * MPSS QMI payload codec; no transport, hardware access or radio side effects.
 * Wire definitions: linux-mobile-broadband/libqmi commit
 * d9e8af5cae7fdf9749baf87606568d3af4d03082, data/qmi-service-{uim,nas,wds}.json,
 * qmi-common.json, src/libqmi-glib/qmi-message.c and qmi-enums*.h.
 * See docs/MODEM_PROTOCOL_MAP.md. QRTR routing/authentication belongs to caller.
 */
#ifndef GIULIA_MODEM_QMI_H
#define GIULIA_MODEM_QMI_H
#include <stddef.h>
#include <stdint.h>

#define GMQ_HEADER 7u
#define GMQ_MAX_CARDS 8u
#define GMQ_MAX_APPS 16u
#define GMQ_MAX_RADIOS 16u
#define GMQ_MAX_APN 100u
#define GMQ_SERVICE_WDS 0x01u
#define GMQ_SERVICE_NAS 0x03u
#define GMQ_SERVICE_UIM 0x0bu
#define GMQ_UIM_GET_CARD_STATUS 0x002fu
#define GMQ_NAS_GET_SERVING_SYSTEM 0x0024u
#define GMQ_WDS_START_NETWORK 0x0020u
#define GMQ_WDS_STOP_NETWORK 0x0021u
#define GMQ_WDS_GET_CURRENT_SETTINGS 0x002du
#define GMQ_SETTINGS_MASK UINT32_C(0x0000a310)

enum gmq_error { GMQ_OK,GMQ_ARGUMENT,GMQ_WIRE,GMQ_TRANSACTION,GMQ_REMOTE,
    GMQ_MISSING,GMQ_LIMIT,GMQ_UNSUPPORTED };
enum gmq_ipv4_field { GMQ_IPV4_ADDRESS=1u,GMQ_IPV4_GATEWAY=2u,GMQ_IPV4_NETMASK=4u,
    GMQ_IPV4_DNS1=8u,GMQ_IPV4_DNS2=16u,GMQ_IPV4_MTU=32u,GMQ_IPV4_FAMILY=64u };
struct gmq_reply { const uint8_t *tlvs; size_t bytes; uint16_t remote_error; };
struct gmq_app {
    uint8_t type,state,personalization_state,personalization_feature;
    uint8_t personalization_retries,personalization_unblock_retries;
    uint8_t upin_replaces_pin1,pin1_state,pin1_retries,puk1_retries;
    uint8_t pin2_state,pin2_retries,puk2_retries;
};
struct gmq_card {
    uint8_t card_state,upin_state,upin_retries,upuk_retries,error,app_count;
    struct gmq_app apps[GMQ_MAX_APPS];
};
struct gmq_sim_status {
    uint16_t primary_gw,primary_1x,secondary_gw,secondary_1x;
    uint8_t card_count;
    struct gmq_card cards[GMQ_MAX_CARDS];
};
struct gmq_serving {
    uint8_t registration,cs_attach,ps_attach,network,radio_count;
    uint8_t radios[GMQ_MAX_RADIOS],roaming_present,roaming;
};
/* IPv4 fields are numeric host values: 0x0a000001 means 10.0.0.1. Not byte
 * arrays and not network-order integers. Check present before using a field. */
struct gmq_ipv4 {
    uint32_t present,address,gateway,netmask,dns1,dns2,mtu;
    uint8_t family;
};
static inline uint16_t gmq_u16(const void *v) {
    const uint8_t *p=(const uint8_t *)v;
    return (uint16_t)((uint16_t)p[0]|(uint16_t)((uint16_t)p[1]<<8));
}
static inline uint32_t gmq_u32(const void *v) {
    const uint8_t *p=(const uint8_t *)v;
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static inline void gmq_w16(void *v,uint16_t n) {
    uint8_t *p=(uint8_t *)v;p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);
}
static inline void gmq_w32(void *v,uint32_t n) {
    uint8_t *p=(uint8_t *)v;unsigned i;
    for(i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i));
}
static inline void gmq_zero(void *v,size_t n) {
    uint8_t *p=(uint8_t *)v;size_t i;for(i=0;i<n;i++)p[i]=0;
}
static inline void gmq_copy(void *v,const void *s,size_t n) {
    uint8_t *p=(uint8_t *)v;const uint8_t *q=(const uint8_t *)s;size_t i;
    for(i=0;i<n;i++)p[i]=q[i];
}
/* Internal header writer. All builders preflight capacity; *used=0 on error.
 * Input and output buffers must not overlap. No QMUX header or QRTR padding. */
static inline int gmq_request(void *v,size_t cap,uint16_t txn,uint16_t msg,
    size_t body,size_t *used) {
    uint8_t *p=(uint8_t *)v;
    if(!used)return GMQ_ARGUMENT;
    *used=0;
    if(!p)return GMQ_ARGUMENT;
    if(body>65535u || cap<GMQ_HEADER || body>cap-GMQ_HEADER)return GMQ_LIMIT;
    p[0]=0;gmq_w16(p+1,txn);gmq_w16(p+3,msg);gmq_w16(p+5,(uint16_t)body);
    *used=GMQ_HEADER+body;return GMQ_OK;
}
static inline int gmq_build_uim_status(void *p,size_t cap,uint16_t txn,size_t *used) {
    return gmq_request(p,cap,txn,GMQ_UIM_GET_CARD_STATUS,0,used);
}
static inline int gmq_build_nas_serving(void *p,size_t cap,uint16_t txn,size_t *used) {
    return gmq_request(p,cap,txn,GMQ_NAS_GET_SERVING_SYSTEM,0,used);
}
/* Explicit IPv4 APN; no authentication TLV or profile/autoconnect/mux policy.
 * Omission of authentication is not a promise of a modem's default behavior.
 * Require ASCII DNS-style labels, at most 100 bytes (local supported limit). */
static inline int gmq_build_wds_start(void *v,size_t cap,uint16_t txn,
    const char *apn,size_t *used) {
    uint8_t *p=(uint8_t *)v;size_t n=0,label=0;int e;unsigned char ch;
    if(!used)return GMQ_ARGUMENT;
    *used=0;
    if(!p || !apn)return GMQ_ARGUMENT;
    while(n<=GMQ_MAX_APN && apn[n]) {
        ch=(unsigned char)apn[n];
        if(ch=='.') {
            if(!label || apn[n-1]=='-')return GMQ_ARGUMENT;
            label=0;
        } else {
            if(!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||
                (ch>='0'&&ch<='9')||ch=='-') || (!label && ch=='-'))return GMQ_ARGUMENT;
            if(++label>63u)return GMQ_ARGUMENT;
        }
        n++;
    }
    if(!n || n>GMQ_MAX_APN || !label || apn[n-1]=='-')return GMQ_ARGUMENT;
    e=gmq_request(p,cap,txn,GMQ_WDS_START_NETWORK,n+7u,used);if(e)return e;
    p[7]=0x14;gmq_w16(p+8,(uint16_t)n);gmq_copy(p+10,apn,n);
    p[10+n]=0x19;gmq_w16(p+11+n,1);p[13+n]=4;return GMQ_OK;
}
static inline int gmq_build_wds_stop(void *v,size_t cap,uint16_t txn,
    uint32_t handle,size_t *used) {
    uint8_t *p=(uint8_t *)v;int e;
    if(!used)return GMQ_ARGUMENT;
    *used=0;
    if(!handle || handle==UINT32_MAX)return GMQ_ARGUMENT;
    e=gmq_request(p,cap,txn,GMQ_WDS_STOP_NETWORK,7,used);if(e)return e;
    p[7]=1;gmq_w16(p+8,4);gmq_w32(p+10,handle);return GMQ_OK;
}
static inline int gmq_build_wds_settings(void *v,size_t cap,uint16_t txn,size_t *used) {
    uint8_t *p=(uint8_t *)v;int e;
    e=gmq_request(p,cap,txn,GMQ_WDS_GET_CURRENT_SETTINGS,7,used);if(e)return e;
    p[7]=0x10;gmq_w16(p+8,4);gmq_w32(p+10,GMQ_SETTINGS_MASK);return GMQ_OK;
}
/* Validate every TLV first, including unknown ones and all duplicates. */
static inline int gmq_tlvs(const uint8_t *p,size_t n) {
    uint8_t seen[32];size_t at=0,z;unsigned t;gmq_zero(seen,sizeof(seen));
    if(n && !p)return GMQ_ARGUMENT;
    while(at<n) {
        if(n-at<3u)return GMQ_WIRE;
        t=p[at];z=gmq_u16(p+at+1);
        if(z>n-at-3u || (seen[t/8]&(1u<<(t%8))))return GMQ_WIRE;
        seen[t/8]|=(uint8_t)(1u<<(t%8));at+=3u+z;
    }
    return GMQ_OK;
}
/* Lookup only on an already validated gmq_reply. */
static inline const uint8_t *gmq_field(const struct gmq_reply *r,unsigned type,size_t *bytes) {
    size_t at=0,z;*bytes=0;
    while(at<r->bytes) {
        z=gmq_u16(r->tlvs+at+1);
        if(r->tlvs[at]==type){*bytes=z;return r->tlvs+at+3;}
        at+=3u+z;
    }
    return NULL;
}
/* Caller must first validate source QRTR node/port. On OK or REMOTE, reply
 * views the input buffer and exposes remote_error; on other errors unchanged.
 * No decoded application output is committed on a failed response. */
static inline int gmq_response(const void *v,size_t n,uint16_t txn,uint16_t msg,
    struct gmq_reply *reply) {
    const uint8_t *p=(const uint8_t *)v,*result;struct gmq_reply r;size_t z;
    uint16_t status;int e;
    if(!p || !reply)return GMQ_ARGUMENT;
    if(n<GMQ_HEADER || n-GMQ_HEADER>65535u || p[0]!=2 ||
        gmq_u16(p+5)!=n-GMQ_HEADER)return GMQ_WIRE;
    if(gmq_u16(p+1)!=txn || gmq_u16(p+3)!=msg)return GMQ_TRANSACTION;
    e=gmq_tlvs(p+GMQ_HEADER,n-GMQ_HEADER);if(e)return e;
    r.tlvs=p+GMQ_HEADER;r.bytes=n-GMQ_HEADER;r.remote_error=0;
    result=gmq_field(&r,2,&z);if(!result)return GMQ_MISSING;
    if(z!=4)return GMQ_WIRE;
    status=gmq_u16(result);r.remote_error=gmq_u16(result+2);
    if(status>1u || (!status && r.remote_error))return GMQ_WIRE;
    *reply=r;return status?GMQ_REMOTE:GMQ_OK;
}
static inline int gmq_index_valid(const struct gmq_sim_status *s,uint16_t ix) {
    unsigned c=ix>>8,a=ix&255u;
    return ix==65535u || (c<s->card_count && c<GMQ_MAX_CARDS && a<s->cards[c].app_count && a<GMQ_MAX_APPS);
}
/* AID bytes are bounded and skipped. No ICCID, IMSI or PIN values are read. */
static inline int gmq_parse_uim_status(const void *v,size_t n,uint16_t txn,
    struct gmq_sim_status *out) {
    struct gmq_reply r;struct gmq_sim_status s;const uint8_t *p;size_t z,at,aid;
    unsigned c,a;int e;
    if(!out)return GMQ_ARGUMENT;
    e=gmq_response(v,n,txn,GMQ_UIM_GET_CARD_STATUS,&r);if(e)return e;
    p=gmq_field(&r,0x10,&z);if(!p)return GMQ_MISSING;
    if(z<9)return GMQ_WIRE;
    gmq_zero(&s,sizeof(s));s.primary_gw=gmq_u16(p);s.primary_1x=gmq_u16(p+2);
    s.secondary_gw=gmq_u16(p+4);s.secondary_1x=gmq_u16(p+6);s.card_count=p[8];at=9;
    if(s.card_count>GMQ_MAX_CARDS)return GMQ_LIMIT;
    for(c=0;c<s.card_count;c++) {
        struct gmq_card *card=&s.cards[c];
        if(z-at<6)return GMQ_WIRE;
        card->card_state=p[at];card->upin_state=p[at+1];card->upin_retries=p[at+2];
        card->upuk_retries=p[at+3];card->error=p[at+4];card->app_count=p[at+5];at+=6;
        if(card->app_count>GMQ_MAX_APPS)return GMQ_LIMIT;
        for(a=0;a<card->app_count;a++) {
            struct gmq_app *app=&card->apps[a];
            if(z-at<7)return GMQ_WIRE;
            app->type=p[at];app->state=p[at+1];app->personalization_state=p[at+2];
            app->personalization_feature=p[at+3];app->personalization_retries=p[at+4];
            app->personalization_unblock_retries=p[at+5];aid=p[at+6];at+=7;
            if(aid>z-at)return GMQ_WIRE;
            at+=aid;if(z-at<7)return GMQ_WIRE;
            app->upin_replaces_pin1=p[at];app->pin1_state=p[at+1];app->pin1_retries=p[at+2];
            app->puk1_retries=p[at+3];app->pin2_state=p[at+4];app->pin2_retries=p[at+5];
            app->puk2_retries=p[at+6];at+=7;
            if(app->upin_replaces_pin1>1)return GMQ_WIRE;
        }
    }
    if(at!=z || !gmq_index_valid(&s,s.primary_gw) || !gmq_index_valid(&s,s.primary_1x) ||
        !gmq_index_valid(&s,s.secondary_gw) || !gmq_index_valid(&s,s.secondary_1x))return GMQ_WIRE;
    *out=s;return GMQ_OK;
}
/* Primary GW only. Does not choose/switch subscriptions or unlock a PIN. */
static inline int gmq_sim_primary_ready(const struct gmq_sim_status *s) {
    const struct gmq_card *c;const struct gmq_app *a;unsigned pin;
    if(!s || s->primary_gw==65535u || !gmq_index_valid(s,s->primary_gw))return 0;
    c=&s->cards[s->primary_gw>>8];a=&c->apps[s->primary_gw&255u];
    pin=a->upin_replaces_pin1?c->upin_state:a->pin1_state;
    return c->card_state==1 && (a->type==1 || a->type==2) && a->state==7 &&
        (pin==2 || pin==3);
}
static inline int gmq_parse_nas_serving(const void *v,size_t n,uint16_t txn,
    struct gmq_serving *out) {
    struct gmq_reply r;struct gmq_serving s;const uint8_t *p;size_t z;int e;
    if(!out)return GMQ_ARGUMENT;
    e=gmq_response(v,n,txn,GMQ_NAS_GET_SERVING_SYSTEM,&r);if(e)return e;
    p=gmq_field(&r,1,&z);if(!p)return GMQ_MISSING;
    if(z<5 || z!=5u+p[4])return GMQ_WIRE;
    if(p[4]>GMQ_MAX_RADIOS)return GMQ_LIMIT;
    gmq_zero(&s,sizeof(s));s.registration=p[0];s.cs_attach=p[1];s.ps_attach=p[2];
    s.network=p[3];s.radio_count=p[4];gmq_copy(s.radios,p+5,s.radio_count);
    p=gmq_field(&r,0x10,&z);
    if(p){if(z!=1)return GMQ_WIRE;s.roaming_present=1;s.roaming=p[0];}
    *out=s;return GMQ_OK;
}
static inline int gmq_parse_wds_start(const void *v,size_t n,uint16_t txn,uint32_t *handle) {
    struct gmq_reply r;const uint8_t *p;size_t z;uint32_t h;int e;
    if(!handle)return GMQ_ARGUMENT;
    e=gmq_response(v,n,txn,GMQ_WDS_START_NETWORK,&r);if(e)return e;
    p=gmq_field(&r,1,&z);if(!p)return GMQ_MISSING;
    if(z!=4)return GMQ_WIRE;
    h=gmq_u32(p);if(!h || h==UINT32_MAX)return GMQ_WIRE;
    *handle=h;return GMQ_OK;
}
static inline int gmq_parse_wds_stop(const void *v,size_t n,uint16_t txn) {
    struct gmq_reply r;return gmq_response(v,n,txn,GMQ_WDS_STOP_NETWORK,&r);
}
/* Require an IPv4 address, expose presence of other optional fields. Caller
 * must validate usable addresses/routes/MTU before configuring its interface.
 * A successful parse does not prove data transport or Internet connectivity. */
static inline int gmq_parse_wds_settings(const void *v,size_t n,uint16_t txn,
    struct gmq_ipv4 *out) {
    struct gmq_reply r;struct gmq_ipv4 s;const uint8_t *p;size_t z;unsigned i;int e;
    static const uint8_t types[6]={0x1e,0x20,0x21,0x15,0x16,0x29};
    uint32_t *fields[6];
    if(!out)return GMQ_ARGUMENT;
    e=gmq_response(v,n,txn,GMQ_WDS_GET_CURRENT_SETTINGS,&r);if(e)return e;
    gmq_zero(&s,sizeof(s));fields[0]=&s.address;fields[1]=&s.gateway;fields[2]=&s.netmask;
    fields[3]=&s.dns1;fields[4]=&s.dns2;fields[5]=&s.mtu;
    for(i=0;i<6;i++) {
        p=gmq_field(&r,types[i],&z);
        if(p){if(z!=4)return GMQ_WIRE;*fields[i]=gmq_u32(p);s.present|=1u<<i;}
    }
    p=gmq_field(&r,0x2b,&z);
    if(p) {
        if(z!=1)return GMQ_WIRE;
        if(p[0]!=4)return GMQ_UNSUPPORTED;
        s.family=4;s.present|=GMQ_IPV4_FAMILY;
    }
    if(!(s.present&GMQ_IPV4_ADDRESS))return GMQ_MISSING;
    *out=s;return GMQ_OK;
}
#endif
