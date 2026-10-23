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
#define GWI_GET_CAPACITY 0x3c00u
#define GWI_SET_CAPACITY 0x3c000u
#define GWI_SCAN_REQUEST_BYTES 0x1550u
#define GWI_SCAN_RESULT_BYTES 0x48cu
#define GWI_CHANNELS_BYTES 0x12d8u
#define GWI_POWER_BYTES 24u
#define GWI_CAPABILITIES_BYTES 13u
#define GWI_VERSION_BYTES 256u
enum gwi_type {
    GWI_SSID=1,GWI_CHANNEL=4,GWI_POWERSAVE=5,GWI_RATE=8,GWI_BSSID=9,GWI_SCAN_REQUEST=10,GWI_SCAN_RESULT=11,
    GWI_CAPABILITIES=12,GWI_STATE=13,GWI_PHY_MODE=14,GWI_OP_MODE=15,
    GWI_RSSI=16,GWI_NOISE=17,GWI_INT_MIT=18,GWI_POWER=19,GWI_ASSOCIATE_RESULT=21,
    GWI_DEVICE_NAME=23,GWI_CHANNELS=27,GWI_DRIVER_VERSION=43,GWI_HARDWARE_VERSION=44,
    GWI_ASSOCIATION_STATUS=50,GWI_COUNTRY=51,GWI_RADIO_INFO=54,GWI_MCS=57,GWI_CURRENT_NETWORK=103,
    GWI_ASSOCIATE_RESULT_EXTENDED=371
};
enum gwi_status { GWI_OK,GWI_ARGUMENT,GWI_UNSUPPORTED,GWI_NOT_READY,GWI_BUSY,GWI_END };
struct gwi_cursor { unsigned index;uint32_t generation; };
/* The platform holds its runtime lock while taking and applying this view.
 * Only powered/scan_pending are writable; received BSS storage stays owned
 * by the WMI worker. A cursor is private to each IOUserClient. */
struct gwi_state { unsigned ready,powered,scan_pending;const struct gww_state *wmi; };
struct gwi_request {unsigned set,type,value;const uint8_t *data;size_t bytes;};
static inline int gwi_decode(const void *v,size_t n,struct gwi_request *r) {
    if(!v || !r || (n!=GWI_GET_BYTES && n!=GWI_SET_BYTES))return 0;
    const uint8_t *p=(const uint8_t *)v;unsigned command=gwq_u32(p);size_t cap;
    if(command==GWI_GET && n==GWI_GET_BYTES)cap=GWI_GET_CAPACITY;
    else if(command==GWI_SET && n==GWI_SET_BYTES)cap=GWI_SET_CAPACITY;
    else return 0;
    size_t z=gwq_u32(p+4);if(z>cap)return 0;
    const uint8_t *tail=p+8+cap;
    if(tail[0]!='e' || tail[1]!='n' || tail[2]!='0' || tail[3])return 0;
    r->set=command==GWI_SET;r->type=gwq_u32(tail+16);r->value=gwq_u32(tail+20);r->data=p+8;r->bytes=z;
    return 1;
}
static inline int gwi_scan_request(const struct gwi_request *r) {
    /* This verified iOS request is already expanded to its fixed-size ABI.
     * This is only envelope validation; gwn_scan_decode validates and
     * snapshots the SSID/BSSID/channel fields before the worker starts it. */
    return r && r->data && r->set==1 && r->type==GWI_SCAN_REQUEST &&
        r->bytes==GWI_SCAN_REQUEST_BYTES && gwq_u32(r->data)==1;
}
static inline int gwi_channels(void *out,size_t bytes) {
    /* 20H392 request 27 allocates 0x12d8 bytes, reads count at +4, then
     * channel/flags at +12/+16 with a 12-byte stride. Advertise exactly the
     * bounded passive list accepted by the transport, with ACTIVE unset. */
    if(!out || bytes!=GWI_CHANNELS_BYTES)return 0;
    uint8_t *p=(uint8_t *)out;for(size_t i=0;i<bytes;i++)p[i]=0;
    gwq_w32(p,1);gwq_w32(p+4,GWW_SCAN_CHANNELS);
    for(unsigned i=0;i<GWW_SCAN_CHANNELS;i++) {
        unsigned frequency=gww_scan_frequencies[i];uint8_t *c=p+8+i*12;
        gwq_w32(c,1);gwq_w32(c+4,gww_channel(frequency));gwq_w32(c+8,2|(frequency<3000?8:16));
    }
    return 1;
}
static inline int gwi_result(void *out,size_t bytes,const struct gww_bss *b,uint64_t now) {
    if(!out || bytes<GWI_SCAN_RESULT_BYTES || !b || b->ssid_len>32 || b->rate_count>16 || b->ie_bytes>1024 || !gww_channel(b->frequency))return 0;
    uint8_t *p=(uint8_t *)out;for(unsigned i=0;i<GWI_SCAN_RESULT_BYTES;i++)p[i]=0;
    gwq_w32(p,1);gwq_w32(p+4,1);gwq_w32(p+8,gww_channel(b->frequency));
    gwq_w32(p+12,2|(b->frequency<3000?8:16));
    gwq_w16(p+0x12,(uint16_t)b->noise);gwq_w16(p+0x14,0);gwq_w16(p+0x16,(uint16_t)b->rssi);
    gwq_w16(p+0x18,b->beacon);gwq_w16(p+0x1a,b->capability);gwq_copy(p+0x1c,b->bssid,6);
    /* 20H392's BSS consumer classifies these words as integer Mbps. Keep
     * on-air 500 kbps/basic-bit encoding in the BSS/IEs, convert only here. */
    p[0x22]=b->rate_count>15?15:b->rate_count;
    for(unsigned i=0;i<p[0x22];i++)gwq_w32(p+0x24+4*i,(b->rates[i]&0x7fu)/2u);
    p[0x60]=b->ssid_len;gwq_copy(p+0x61,b->ssid,b->ssid_len);
    uint64_t age=now>=b->timestamp?now-b->timestamp:0;gwq_w32(p+0x84,age>0xffffffffu?0xffffffffu:(uint32_t)age);
    gwq_w16(p+0x8a,b->ie_bytes);gwq_copy(p+0x8c,b->ie,b->ie_bytes);return 1;
}

/* The framework passes STATE/OP_MODE in req_val, with req_len == 0;
 * its proxy still requests a four-byte output. This is visible at
 * 20H392 IO80211 0x1d3ec4038 and 0x1d3ecb034..0x1d3ecb06c.
 * Other sizes come from the same image's GET dispatch jump table at
 * 0x1d3ef565e. Do not substitute desktop apple80211_* struct sizes. */
static inline size_t gwi_get_bytes(unsigned type) {
    switch(type) {
    case GWI_SSID:return 32;
    case GWI_CHANNEL:return 16;
    case GWI_BSSID:return 6;
    case GWI_SCAN_RESULT:case GWI_CURRENT_NETWORK:return GWI_SCAN_RESULT_BYTES;
    case GWI_CAPABILITIES:return GWI_CAPABILITIES_BYTES;
    case GWI_STATE:case GWI_OP_MODE:case GWI_ASSOCIATE_RESULT:case GWI_ASSOCIATION_STATUS:
    case GWI_ASSOCIATE_RESULT_EXTENDED:case GWI_POWERSAVE:case GWI_RATE:case GWI_INT_MIT:case GWI_MCS:return 4;
    case GWI_PHY_MODE:return 12;
    case GWI_RSSI:case GWI_NOISE:return 52;
    case GWI_POWER:return GWI_POWER_BYTES;
    case GWI_CHANNELS:return GWI_CHANNELS_BYTES;
    case GWI_DRIVER_VERSION:case GWI_HARDWARE_VERSION:return GWI_VERSION_BYTES;
    case GWI_COUNTRY:return 3;
    case GWI_RADIO_INFO:return 8;
    default:return 0;
    }
}
static inline int gwi_scalar(unsigned type) {
    return type==GWI_STATE || type==GWI_OP_MODE ||
        type==GWI_ASSOCIATE_RESULT || type==GWI_ASSOCIATION_STATUS || type==GWI_ASSOCIATE_RESULT_EXTENDED || type==GWI_POWERSAVE ||
        type==GWI_RATE || type==GWI_INT_MIT || type==GWI_MCS;
}
static inline void gwi_string(uint8_t *out,size_t bytes,const char *text) {
    size_t i=0;while(i+1<bytes && text[i]){out[i]=(uint8_t)text[i];i++;}
}
/* No IOKit calls or guessed connection state. A successful SET queues work
 * under the platform lock; it cannot issue MMIO, supply a key or associate.
 * On any failure, output length is zero and state/cursor/output are unchanged.
 * Input must be the kernel snapshot, never the embedded userspace pointer. */
static inline int gwi_query(struct gwi_state *s,const struct gwi_request *q,
    void *output,size_t *bytes,struct gwi_cursor *cursor,uint64_t now) {
    if(!bytes)return GWI_ARGUMENT;
    size_t capacity=*bytes;*bytes=0;
    if(!s || !s->wmi || !q || !cursor || q->set>1 ||
       q->bytes>(q->set?GWI_SET_CAPACITY:GWI_GET_CAPACITY) ||
       (q->bytes && !q->data) || capacity>GWI_SET_CAPACITY || (capacity && !output))return GWI_ARGUMENT;
    if(!s->ready)return GWI_NOT_READY;
    if(s->wmi->bss_count>GWW_BSS_MAX || s->wmi->chains>2)return GWI_ARGUMENT;
    /* The pinned stock 20H392 Core dispatch has no GET/SET18 handler: both
     * go directly to Unsupported. IRQ polling does not define INT_MIT. */
    if(q->type==GWI_INT_MIT)return GWI_UNSUPPORTED;
    unsigned busy=s->scan_pending || s->wmi->scanning;
    uint8_t *out=(uint8_t *)output;
    if(q->set) {
        if(q->type==GWI_SCAN_REQUEST) {
            if(!gwi_scan_request(q))return GWI_ARGUMENT;
            if(!s->powered)return GWI_NOT_READY;
            if(busy)return GWI_BUSY;
        } else if(q->type==GWI_POWER) {
            if(q->bytes!=GWI_POWER_BYTES || gwq_u32(q->data)!=1 ||
               gwq_u32(q->data+4)!=1 || gwq_u32(q->data+8)>1)return GWI_ARGUMENT;
            if(!gwq_u32(q->data+8) && busy)return GWI_BUSY;
        } else return GWI_UNSUPPORTED;
        /* A short echo must not succeed after already changing radio state. */
        if(capacity && capacity<q->bytes)return GWI_ARGUMENT;
        if(capacity) {
            gwq_copy(out,q->data,q->bytes);
            for(size_t i=q->bytes;i<capacity;i++)out[i]=0;
            *bytes=q->bytes;
        }
        if(q->type==GWI_SCAN_REQUEST) {
            s->scan_pending=1;cursor->index=0;cursor->generation=s->wmi->scan_id;
        } else s->powered=gwq_u32(q->data+8);
        return GWI_OK;
    }
    size_t required=gwi_get_bytes(q->type);
    /* Keep the original bounded device-name extension. It is not used by
     * Apple80211Get's table and does not imply an association capability. */
    if(q->type==GWI_DEVICE_NAME && q->bytes && q->bytes<=1024)required=q->bytes;
    if(!required)return GWI_UNSUPPORTED;
    /* Apple80211Bind builds its first DRIVER_VERSION GET directly with an
     * empty payload and 256-byte output (20H392 0x1d3ed4ef8..0x1d3ed4f7c).
     * The generic GET path supplies the 256-byte payload instead. */
    unsigned bind_version=q->type==GWI_DRIVER_VERSION && !q->bytes;
    if(capacity>GWI_GET_CAPACITY || capacity<required || !out ||
       (!bind_version && q->bytes!=(gwi_scalar(q->type)?0:required)))return GWI_ARGUMENT;
    switch(q->type) {
    case GWI_SSID:case GWI_CHANNEL:case GWI_BSSID:case GWI_RSSI:case GWI_NOISE:
    case GWI_ASSOCIATE_RESULT:case GWI_ASSOCIATION_STATUS:case GWI_CURRENT_NETWORK:case GWI_ASSOCIATE_RESULT_EXTENDED:case GWI_POWERSAVE:
    case GWI_RATE:case GWI_MCS:
        /* Cached scan BSSes are not the currently associated peer. */
        return GWI_NOT_READY;
    case GWI_SCAN_RESULT: {
        if(busy)return GWI_BUSY;
        unsigned index=cursor->generation==s->wmi->scan_id?cursor->index:0;
        if(index>=s->wmi->bss_count)return GWI_END;
        if(!gwi_result(out,required,&s->wmi->bss[index],now))return GWI_ARGUMENT;
        cursor->index=index+1;cursor->generation=s->wmi->scan_id;
        *bytes=required;return GWI_OK;
    }
    case GWI_RADIO_INFO:if(!s->wmi->chains)return GWI_NOT_READY;break;
    default:break;
    }
    for(size_t i=0;i<required;i++)out[i]=0;
    switch(q->type) {
    case GWI_CAPABILITIES:break; /* no association, crypto, AP or data-path bits */
    case GWI_STATE:gwq_w32(out,busy?1:0);break;
    case GWI_PHY_MODE:
        /* +4 is supported modes, +8 is active mode (not the reverse).
         * No PHY mode is active while the station has no association. */
        gwq_w32(out,1);gwq_w32(out+4,0x0e);break; /* legacy 11a/b/g; no HT rates */
    case GWI_OP_MODE:gwq_w32(out,1);break;
    case GWI_POWER:gwq_w32(out,1);gwq_w32(out+4,1);gwq_w32(out+8,s->powered);break;
    case GWI_DEVICE_NAME:gwi_string(out,required,"OnePlus Ace 5 WCN7850");break;
    case GWI_CHANNELS:(void)gwi_channels(out,required);break;
    case GWI_DRIVER_VERSION:gwi_string(out,required,"Giulia WCN7850 iOS16 passive-scan 1");break;
    case GWI_HARDWARE_VERSION:gwi_string(out,required,"Qualcomm WCN7850");break;
    case GWI_COUNTRY:out[0]='C';out[1]='N';break;
    case GWI_RADIO_INFO:gwq_w32(out,1);gwq_w32(out+4,s->wmi->chains);break;
    default:return GWI_UNSUPPORTED;
    }
    *bytes=required;return GWI_OK;
}
#endif
