/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_MODEM_IP_H
#define GIULIA_MODEM_IP_H
#include "giulia_modem_contract.h"
/* WDS Get Current Settings 0x002d, TLVs 0x25..0x29 and 0x2b.
 * Source: libqmi d9e8af5cae7fdf9749baf87606568d3af4d03082,
 * data/qmi-service-wds.json. IPv6 words are network order, not QMI LE. */
static inline int gmr_parse_ipv6(const void *v,size_t n,uint16_t txn,struct gmr_ipv6 *out) {
    struct gmq_reply r;struct gmr_ipv6 s;const uint8_t *p;size_t z;unsigned i;int e;
    uint8_t *fields[4];
    e=gmq_response(v,n,txn,GMQ_WDS_GET_CURRENT_SETTINGS,&r);if(e)return e;
    gmq_zero(&s,sizeof(s));fields[0]=s.address;fields[1]=s.gateway;
    fields[2]=s.dns1;fields[3]=s.dns2;
    for(i=0;i<4;i++) {
        p=gmq_field(&r,0x25u+i,&z);if(!p)continue;
        if(z!=(i<2?17u:16u))return GMQ_WIRE;
        if(i<2 && p[16]>128u)return GMQ_WIRE;
        gmq_copy(fields[i],p,16);s.present|=1u<<i;
        if(i==0)s.prefix=p[16];
        if(i==1)s.gateway_prefix=p[16];
    }
    p=gmq_field(&r,0x29,&z);if(p){if(z!=4)return GMQ_WIRE;s.mtu=gmq_u32(p);s.present|=16;}
    p=gmq_field(&r,0x2b,&z);if(p && (z!=1 || p[0]!=6))return GMQ_UNSUPPORTED;
    if(!(s.present&1u))return GMQ_MISSING;
    *out=s;return GMQ_OK;
}
static inline int gmr_ipv4_usable(const struct gmq_ipv4 *s) {
    uint32_t host=s->address,mask=s->netmask;
    if(!(s->present&GMQ_IPV4_ADDRESS) || !host || host==UINT32_MAX ||
        !(host>>24) || (host>>24)==127u || (host>>28)>=14u)return 0;
    if((s->present&GMQ_IPV4_MTU) && (s->mtu<576u || s->mtu>GMR_PAYLOAD))return 0;
    if(s->present&GMQ_IPV4_NETMASK){mask=~mask;if(mask&(mask+1u))return 0;}
    return 1;
}
static inline int gmr_ipv6_usable(const struct gmr_ipv6 *s) {
    unsigned i,nonzero=0;
    for(i=0;i<16;i++)nonzero|=s->address[i];
    return (s->present&1u) && nonzero && s->address[0]!=255u && s->prefix<=128 &&
        (!(s->present&16u) || (s->mtu>=1280u && s->mtu<=GMR_PAYLOAD));
}
static inline int gmr_packet_valid(const uint8_t *p,size_t n,unsigned family) {
    size_t h,total;
    if(!p || !n || n>GMR_PAYLOAD || (p[0]>>4)!=family)return 0;
    if(family==4) {
        if(n<20)return 0;
        h=(size_t)(p[0]&15u)*4u;
        total=(size_t)p[2]*256u+p[3];return h>=20 && h<=n && total==n;
    }
    if(family==6) {
        if(n<40)return 0;
        total=(size_t)p[4]*256u+p[5]+40u;return total==n;
    }
    return 0;
}
#endif
