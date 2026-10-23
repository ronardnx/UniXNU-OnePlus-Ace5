/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Firmware regulatory rules: Linux v6.12 ath12k/wmi.{h,c}; legacy layout
 * retained in ath11k/wmi.h. This table grants no permission without a valid
 * firmware event for the configured CN domain. Only 20 MHz 2.4/5 GHz is used.
 */
#ifndef GIULIA_WIFI_REGULATORY_H
#define GIULIA_WIFI_REGULATORY_H
#include "giulia_wifi_qmi.h"
#define GWREG_RULES 20u
struct gwreg_rule {unsigned start,end,bandwidth,power,gain,flags;};
struct gwreg_state {
    unsigned valid,error,count,min_bw[2],max_bw[2],generation;
    uint8_t country[2];struct gwreg_rule rules[GWREG_RULES];
};
static inline const uint8_t *gwreg_field(const uint8_t *p,size_t n,unsigned wanted,size_t *bytes) {
    const uint8_t *result=NULL;*bytes=0;
    while(n){if(n<4)return NULL;unsigned len=gwq_u16(p),tag=gwq_u16(p+2);if(len>n-4)return NULL;
        if(tag==wanted){if(result)return NULL;result=p+4;*bytes=len;}p+=len+4;n-=len+4;}
    return result;
}
static inline int gwreg_receive(struct gwreg_state *s,unsigned event,const uint8_t *p,size_t n) {
    if(!s || !p || (event!=0x3a001 && event!=0x3a003))return 0;
    size_t fixed=0,array=0;unsigned ext=event==0x3a003;
    const uint8_t *header=gwreg_field(p,n,ext?938:609,&fixed);
    const uint8_t *rules=gwreg_field(p,n,18,&array);
    /* This driver owns only PHY 0; events for another PHY do not replace it. */
    if(header && fixed>=8 && gwq_u32(header+4))return 0;
    s->valid=0;s->error=1;s->generation++;
    if(!header || fixed<(ext?312u:56u) || !rules || gwq_u32(header) ||
       !gwq_u32(header+12) || gwq_u32(header+12)>3 || header[8]!='C' || header[9]!='N')return 0;
    unsigned n2=gwq_u32(header+48),n5=gwq_u32(header+52),total=n2+n5;
    if(n2>10 || n5>10 || !total)return 0;
    if(ext) {
        for(unsigned at=252;at<312;at+=4) {
            unsigned count=gwq_u32(header+at);
            if(count>5)return 0;
            /* Four reserved client slots exist on the wire; Linux uses
             * the first two. Do not reinterpret unsupported client arrays. */
            if(at>=264 && ((at-264)/4)%4>=2 && count)return 0;
            total+=count;
        }
    }
    unsigned stride=ext?20:16;if(total>65 || array!=(size_t)total*stride)return 0;
    struct gwreg_state candidate;
    for(size_t i=0;i<sizeof(candidate);i++)((uint8_t *)&candidate)[i]=0;
    candidate.generation=s->generation;
    candidate.country[0]='C';candidate.country[1]='N';candidate.count=n2+n5;
    for(unsigned band=0;band<2;band++) {
        candidate.min_bw[band]=gwq_u32(header+32+band*8);candidate.max_bw[band]=gwq_u32(header+36+band*8);
        if((band?n5:n2) && (!candidate.min_bw[band] || candidate.min_bw[band]>candidate.max_bw[band]))return 0;
    }
    for(unsigned i=0;i<total;i++) {
        const uint8_t *r=rules+i*stride;
        if(gwq_u16(r)!=stride-4 || gwq_u16(r+2)!=(ext?937:608))return 0;
        if(i>=candidate.count)continue; /* 6 GHz is validated, never enabled. */
        struct gwreg_rule *out=&candidate.rules[i];uint32_t freq=gwq_u32(r+4),power=gwq_u32(r+8);
        out->start=freq&0xffff;out->end=freq>>16;out->bandwidth=power&0xffff;
        out->power=(power>>16)&255;out->gain=power>>24;out->flags=gwq_u32(r+12)&0xffff;
        if(out->start>=out->end || !out->bandwidth || out->power>127 || out->gain>127)return 0;
        /* PSD-only constraints require additional calculations; no 6 GHz
         * rule or PSD constraint can authorize this legacy transmit path. */
        if(ext && (gwq_u32(r+16)&4))out->flags|=1;
    }
    candidate.valid=1;candidate.error=0;*s=candidate;return 1;
}
static inline int gwreg_channel(const struct gwreg_state *s,unsigned frequency,
    unsigned *power,unsigned *gain) {
    if(!s || !s->valid || !power || !gain || s->count>GWREG_RULES ||
       !((frequency>=2412 && frequency<=2472 && !((frequency-2412)%5)) ||
         (frequency>=5180 && frequency<=5825 && !((frequency-5000)%5))))return 0;
    unsigned band=frequency>=3000,allowed=0,p=255,g=255;
    if(s->min_bw[band]>20 || s->max_bw[band]<20)return 0;
    for(unsigned i=0;i<s->count;i++) {
        const struct gwreg_rule *r=&s->rules[i];
        if(frequency-10<r->start || frequency+10>r->end)continue;
        /* Disabled, NO_IR, radar, NO_OFDM, indoor-only (environment is not
         * established), and NO_20MHz are not legal active TX permissions. */
        if(r->bandwidth<20 || (r->flags&(1|2|8|64|512|2048)))return 0;
        if(r->power*2<p)p=r->power*2;if(r->gain*2<g)g=r->gain*2;allowed=1;
    }
    if(!allowed)return 0;*power=p;*gain=g;return 1;
}
#endif
