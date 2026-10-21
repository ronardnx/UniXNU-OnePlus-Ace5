/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * iOS 16.7.16 / 20H392 arm64 IO80211.framework proxy wire ABI, observed in
 * Apple80211ScanAsync, Apple80211Scan and the IOConnectCallStructMethod calls.
 * GET and SET have DIFFERENT envelope capacities. No user pointer in the
 * trailing request is ever followed. The platform must snapshot input first.
 */
#ifndef GIULIA_WIFI_APPLE_H
#define GIULIA_WIFI_APPLE_H
#include "giulia_wifi_wmi.h"
#define GWI_GET 0xc02869c9u
#define GWI_SET 0x802869c8u
#define GWI_GET_BYTES 0x3c30u
#define GWI_SET_BYTES 0x3c030u
#define GWI_SCAN_REQUEST_BYTES 0x1550u
#define GWI_SCAN_RESULT_BYTES 0x48cu
struct gwi_request {unsigned set,type,value;const uint8_t *data;size_t bytes;};
static inline int gwi_decode(const void *v,size_t n,struct gwi_request *r) {
    if(!v || !r || (n!=GWI_GET_BYTES && n!=GWI_SET_BYTES))return 0;
    const uint8_t *p=(const uint8_t *)v;unsigned command=gwq_u32(p);size_t cap;
    if(command==GWI_GET && n==GWI_GET_BYTES)cap=0x3c00;
    else if(command==GWI_SET && n==GWI_SET_BYTES)cap=0x3c000;
    else return 0;
    size_t z=gwq_u32(p+4);if(z>cap)return 0;
    const uint8_t *tail=p+8+cap;
    if(tail[0]!='e' || tail[1]!='n' || tail[2]!='0' || tail[3])return 0;
    r->set=command==GWI_SET;r->type=gwq_u32(tail+16);r->value=gwq_u32(tail+20);r->data=p+8;r->bytes=z;
    return 1;
}
static inline int gwi_scan_request(const struct gwi_request *r) {
    /* This verified iOS request is already expanded to its fixed-size ABI.
     * SSID/channel dictionaries are filtered by the framework on returned
     * results. The hardware scan remains passive and uses its bounded list. */
    return r->set && r->type==10 && r->bytes==GWI_SCAN_REQUEST_BYTES && gwq_u32(r->data)==1;
}
static inline int gwi_result(void *out,size_t bytes,const struct gww_bss *b,uint64_t now) {
    if(!out || bytes<GWI_SCAN_RESULT_BYTES || !b || b->ssid_len>32 || b->rate_count>16 || b->ie_bytes>1024 || !gww_channel(b->frequency))return 0;
    uint8_t *p=(uint8_t *)out;for(unsigned i=0;i<GWI_SCAN_RESULT_BYTES;i++)p[i]=0;
    gwq_w32(p,1);gwq_w32(p+4,1);gwq_w32(p+8,gww_channel(b->frequency));
    gwq_w32(p+12,2|(b->frequency<3000?8:16));
    gwq_w16(p+0x12,(uint16_t)b->noise);gwq_w16(p+0x14,0);gwq_w16(p+0x16,(uint16_t)b->rssi);
    gwq_w16(p+0x18,b->beacon);gwq_w16(p+0x1a,b->capability);gwq_copy(p+0x1c,b->bssid,6);
    p[0x22]=b->rate_count>15?15:b->rate_count;for(unsigned i=0;i<p[0x22];i++)gwq_w32(p+0x24+4*i,b->rates[i]);
    p[0x60]=b->ssid_len;gwq_copy(p+0x61,b->ssid,b->ssid_len);
    uint64_t age=now>=b->timestamp?now-b->timestamp:0;gwq_w32(p+0x84,age>0xffffffffu?0xffffffffu:(uint32_t)age);
    gwq_w16(p+0x8a,b->ie_bytes);gwq_copy(p+0x8c,b->ie,b->ie_bytes);return 1;
}
#endif
