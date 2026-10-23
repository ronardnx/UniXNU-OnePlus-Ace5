/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * 20H392 IO80211 station request ABI. Apple80211AssociateAsync at
 * 0x1d3ebf830 passes 0x250 bytes; 0x1d3ec0ab0 stores the derived 32-byte
 * PMK at +0x48 and {length=32,cipher=6} at +0x3c. This is not the desktop
 * apple80211_assoc_data size. Never log the key or follow an embedded pointer.
 */
#ifndef GIULIA_WIFI_NETWORK_H
#define GIULIA_WIFI_NETWORK_H
#include "giulia_wifi_apple.h"
#include "giulia_wifi_station.h"
#include "giulia_wifi_rsn.h"
#define GWN_ASSOCIATE 20u
#define GWN_DISASSOCIATE 22u
#define GWN_CIPHER_KEY 3u
#define GWN_AUTH_TYPE 2u
#define GWN_ASSOCIATE_BYTES 0x250u
#define GWN_KEY_BYTES 0x94u
/* __waitForJoinResult (20H392 0x1d3ed670c) keeps polling result 0;
 * __errorForAssociateResult (0x1d3ec1744, table 0x1d3ef90d0) maps 1 to success. */
enum {GWN_RESULT_PENDING=0,GWN_RESULT_SUCCESS=1,GWN_RESULT_FAILURE=2,
    GWN_RESULT_TIMEOUT=9,GWN_RESULT_RSN_TIMEOUT=22};
enum {GWN_JOIN_NONE,GWN_JOIN_SELECT,GWN_JOIN_SCAN,GWN_JOIN_LEAVE};
enum {GWN_SCAN_NONE,GWN_SCAN_USER,GWN_SCAN_JOIN,GWN_SCAN_ROAM};
struct gwn_join {
    unsigned security,ssid_bytes,has_bssid,has_pmk,rsn_bytes,password_bytes;
    uint8_t ssid[32],bssid[6],pmk[32],rsn[257],password[64];
};
struct gwn_view {
    unsigned available,phase,associated,authorized,security,result,status;
    const struct gww_bss *peer;
};
#define GWN_QUEUE_COUNT 32u
#define GWN_FRAME_BYTES 1518u
struct gwn_packet {size_t bytes;uint8_t data[GWN_FRAME_BYTES];};
struct gwn_queue {unsigned head,tail,count;struct gwn_packet packets[GWN_QUEUE_COUNT];};
/* CPU-owned queues isolate BSD callbacks and EAPOL parsing from device-ring
 * dispatch. The platform holds its runtime lock for every queue operation. */
static inline int gwn_enqueue(struct gwn_queue *q,const void *p,size_t n) {
    if(!q || !p || n<14 || n>GWN_FRAME_BYTES || q->count>=GWN_QUEUE_COUNT || q->head>=GWN_QUEUE_COUNT)return 0;
    struct gwn_packet *packet=&q->packets[q->head];gwq_copy(packet->data,p,n);packet->bytes=n;
    q->head=(q->head+1)%GWN_QUEUE_COUNT;q->count++;return 1;
}
static inline const struct gwn_packet *gwn_front(const struct gwn_queue *q) {
    return q && q->count && q->count<=GWN_QUEUE_COUNT && q->tail<GWN_QUEUE_COUNT?&q->packets[q->tail]:NULL;
}
static inline void gwn_pop(struct gwn_queue *q) {
    if(q && q->count && q->count<=GWN_QUEUE_COUNT && q->tail<GWN_QUEUE_COUNT){
        volatile uint8_t *p=q->packets[q->tail].data;
        for(size_t i=0;i<GWN_FRAME_BYTES;i++)p[i]=0;
        q->packets[q->tail].bytes=0;q->tail=(q->tail+1)%GWN_QUEUE_COUNT;q->count--;
    }
}
static inline void gwn_clear(void *p,size_t n) {
    volatile uint8_t *b=(volatile uint8_t *)p;while(n--)*b++=0;
}
static inline unsigned gwn_nonzero(const uint8_t *p,size_t n) {
    unsigned value=0;for(size_t i=0;i<n;i++)value|=p[i];return value;
}
static inline int gwn_mac(const uint8_t *p) {
    return p && !(p[0]&1) && gwn_nonzero(p,6);
}
/* Actual __getScanData in 20H392: primary SSID +0x10/+0x14,
 * channels +0x44/+0x48 (400 records, stride 12), optional ten network
 * SSIDs +0x1314/+0x1318 (stride 56, version/length/bytes). */
static inline int gwn_scan_decode(const struct gwi_request *q,struct gww_scan_request *out) {
    if(!q || !out || !gwi_scan_request(q))return GWI_ARGUMENT;
    const uint8_t *p=q->data;unsigned type=gwq_u32(p+0x34),ssid=gwq_u32(p+0x10);
    unsigned networks=gwq_u32(p+0x1314),channels=gwq_u32(p+0x44);
    if(type>4 || ssid>32 || networks>10 || channels>400 || (ssid && networks))return GWI_ARGUMENT;
    struct gww_scan_request scan;gwn_clear(&scan,sizeof(scan));scan.active=type==1 || type==4;
    if(ssid){scan.ssid_count=1;scan.ssids[0].length=(uint8_t)ssid;gwq_copy(scan.ssids[0].bytes,p+0x14,ssid);}
    else for(unsigned i=0;i<networks;i++) {
        const uint8_t *v=p+0x1318+56*i;unsigned n=gwq_u32(v+4);
        if(gwq_u32(v)!=1 || !n || n>32)return GWI_ARGUMENT;
        scan.ssids[i].length=(uint8_t)n;gwq_copy(scan.ssids[i].bytes,v+8,n);scan.ssid_count++;
    }
    if(gwn_nonzero(p+8,6)) {
        unsigned broadcast=1;for(unsigned i=0;i<6;i++)broadcast&=p[8+i]==255;
        if(!broadcast){if(!gwn_mac(p+8))return GWI_ARGUMENT;scan.bssid_count=1;gwq_copy(scan.bssids[0],p+8,6);}
    }
    for(unsigned i=0;i<channels;i++) {
        const uint8_t *v=p+0x48+12*i;unsigned channel=gwq_u32(v+4),flags=gwq_u32(v+8),frequency=0;
        if(gwq_u32(v)!=1)return GWI_ARGUMENT;
        if((flags&24)==8 && channel>=1 && channel<=13)frequency=2407+channel*5;
        else if((flags&24)==16 && channel>=36 && channel<=165)frequency=5000+channel*5;
        else if(!(flags&24) && channel>=1 && channel<=13)frequency=2407+channel*5;
        else if(!(flags&24) && channel>=36 && channel<=165)frequency=5000+channel*5;
        if(!gww_channel(frequency))return GWI_UNSUPPORTED;
        unsigned duplicate=0;for(unsigned j=0;j<scan.frequency_count;j++)duplicate|=scan.frequencies[j]==frequency;
        if(duplicate)continue;if(scan.frequency_count>=GWW_SCAN_FREQ_MAX)return GWI_ARGUMENT;
        scan.frequencies[scan.frequency_count++]=(uint16_t)frequency;
    }
    *out=scan;return GWI_OK;
}
static inline int gwn_join_decode(const struct gwi_request *q,struct gwn_join *out) {
    if(!q || !out || q->set!=1 || q->type!=GWN_ASSOCIATE ||
       q->bytes!=GWN_ASSOCIATE_BYTES || !q->data)return GWI_ARGUMENT;
    const uint8_t *p=q->data;
    if(gwq_u32(p)!=1 || gwq_u16(p+4)!=2 || gwq_u16(p+6)!=1)return GWI_UNSUPPORTED;
    unsigned auth=gwq_u16(p+8),ssid=gwq_u32(p+12),rsn=gwq_u16(p+204);
    if(!ssid || ssid>32 || rsn>257)return GWI_ARGUMENT;
    if(auth!=0 && auth!=8 && auth!=0x1000)return GWI_UNSUPPORTED;
    unsigned has_bssid=gwn_nonzero(p+48,6)!=0;
    if(has_bssid && !gwn_mac(p+48))return GWI_ARGUMENT;
    unsigned cipher=gwq_u32(p+64),length=gwq_u32(p+60);
    const uint8_t *pmk=NULL;
    if(auth==8) {
        if(gwq_u32(p+56)!=1)return GWI_ARGUMENT;
        if(cipher==6 && length==32)pmk=p+72;
        /* Framework also supplies a derived PMK with its password extension.
         * Read that PMK only; do not retain or reinterpret the password bytes. */
        else if(cipher==10 && length>=8 && length<=63 && gwq_u32(p+0x1d4)==32)pmk=p+0x1d8;
        else return GWI_UNSUPPORTED;
    } else if(auth==0x1000){
        /* 20H392 0x1d3ec0afc..0x1d3ec0b3c checks 1..64 bytes, stores
         * cipher 10 at +0x40, copies exactly length bytes at +0x48 and
         * stores length at +0x3c. SAE needs the password, not the PBKDF PMK. */
        if(gwq_u32(p+56)!=1 || cipher!=10 || !length || length>64)return GWI_UNSUPPORTED;
    } else if(length || cipher || rsn)return GWI_ARGUMENT;
    if(rsn && (p[206]!=48 || (unsigned)p[207]+2!=rsn))return GWI_ARGUMENT;
    struct gwn_join candidate;gwn_clear(&candidate,sizeof(candidate));candidate.security=auth==0x1000?GWT_WPA3_SAE:auth?GWT_WPA2_CCMP:GWT_OPEN;
    candidate.ssid_bytes=ssid;candidate.has_bssid=has_bssid;candidate.rsn_bytes=rsn;
    gwq_copy(candidate.ssid,p+16,ssid);gwq_copy(candidate.bssid,p+48,6);
    if(rsn)gwq_copy(candidate.rsn,p+206,rsn);
    if(pmk){gwq_copy(candidate.pmk,pmk,32);candidate.has_pmk=1;}
    if(auth==0x1000){candidate.password_bytes=length;gwq_copy(candidate.password,p+72,length);}
    *out=candidate;gwn_clear(&candidate,sizeof(candidate));return GWI_OK;
}
/* Choose only a real scan entry, binding security/SSID and optional BSSID.
 * The station layer validates the complete RSN IE and regulatory permission. */
static inline int gwn_matches(const struct gww_bss *b,const struct gwn_join *q) {
    if(!b || !q || !q->ssid_bytes || q->ssid_bytes>32 ||
       b->ssid_len!=q->ssid_bytes || !gwn_mac(b->bssid) || !gww_channel(b->frequency) ||
       ((b->capability&0x10)!=0)!=(q->security!=0))return 0;
    unsigned different=0;
    for(unsigned j=0;j<q->ssid_bytes;j++)different|=b->ssid[j]^q->ssid[j];
    if(q->has_bssid)for(unsigned j=0;j<6;j++)different|=b->bssid[j]^q->bssid[j];
    return !different;
}
/* Bind the advertised suites and PMF policy to the actual selected BSS.
 * An explicit framework RSN IE may require PMF but cannot downgrade SAE.
 * H2E is selected only when the AP advertises its RSNX capability. */
static inline int gwn_station_request(struct gwt_request *out,const struct gwn_join *join,const struct gww_bss *bss){
    if(!out || !join || !gwn_matches(bss,join) || bss->ie_bytes>GWW_IE_MAX || join->security>GWT_WPA3_SAE)return 0;
    struct gwt_request q;gwn_clear(&q,sizeof(q));q.bss=*bss;q.security=join->security;
    const uint8_t *ap_rsn=NULL;size_t ap_bytes=0;unsigned rsnxe=0;
    for(size_t at=0;at<bss->ie_bytes;){
        if(bss->ie_bytes-at<2 || bss->ie[at+1]>bss->ie_bytes-at-2)return 0;
        unsigned tag=bss->ie[at],n=bss->ie[at+1];
        if(tag==48){if(ap_rsn)return 0;ap_rsn=bss->ie+at;ap_bytes=n+2;}
        if(tag==244){if(rsnxe++ || !n || (bss->ie[at+2]&15)+1u!=n)return 0;q.h2e=!!(bss->ie[at+2]&0x20);}
        at+=n+2;
    }
    if(q.security){
        unsigned sae=q.security==GWT_WPA3_SAE;
        if(sae && (!join->password_bytes || join->password_bytes>64))return 0;
        if(!sae && !join->has_pmk)return 0;
        q.pmf=sae?2:0;q.h2e=sae?q.h2e:0;
        if(!sae){
            if(gwr_rsn_valid_secure(ap_rsn,ap_bytes,0,0,1))q.pmf=1;
            if(join->rsn_bytes){
                if(gwr_rsn_valid_secure(join->rsn,join->rsn_bytes,1,0,2))q.pmf=2;
                else if(gwr_rsn_valid_secure(join->rsn,join->rsn_bytes,1,0,1))q.pmf=1;
                else if(gwr_rsn_valid_secure(join->rsn,join->rsn_bytes,1,0,0))q.pmf=0;
                else return 0;
            }
        } else if(join->rsn_bytes && !gwr_rsn_valid_secure(join->rsn,join->rsn_bytes,1,1,2))return 0;
        if(!gwr_rsn_valid_secure(ap_rsn,ap_bytes,0,sae,q.pmf))return 0;
    }else if(join->rsn_bytes)return 0;
    uint8_t rates[16],count=0;if(!gwt_request_valid(&q,rates,&count))return 0;*out=q;return 1;
}
static inline const struct gww_bss *gwn_select(const struct gww_state *w,const struct gwn_join *q) {
    if(!w || !q || w->bss_count>GWW_BSS_MAX || !q->ssid_bytes || q->ssid_bytes>32)return NULL;
    const struct gww_bss *best=NULL;
    for(unsigned i=0;i<w->bss_count;i++) {
        const struct gww_bss *b=&w->bss[i];
        if(gwn_matches(b,q) && (!best || b->rssi>best->rssi))best=b;
    }
    return best;
}
/* Network getters are separate from scan iteration. A cached AP is exposed
 * as the current peer only after an actual association confirmation. */
static inline int gwn_get(const struct gwn_view *s,const struct gwi_request *q,void *output,size_t *bytes,int *handled) {
    if(!s || !q || !bytes || !handled)return GWI_ARGUMENT;
    *handled=0;if(q->set)return GWI_UNSUPPORTED;
    switch(q->type) {
    case GWI_CAPABILITIES:case GWI_STATE:case GWI_PHY_MODE:case GWI_SSID:
    case GWI_CHANNEL:case GWI_BSSID:case GWI_RSSI:case GWI_NOISE:
    case GWI_ASSOCIATE_RESULT:case GWI_ASSOCIATION_STATUS:case GWN_AUTH_TYPE:break;
    default:return GWI_UNSUPPORTED;
    }
    if(!s->available)return GWI_UNSUPPORTED;
    *handled=1;size_t capacity=*bytes;*bytes=0;
    size_t n=q->type==GWN_AUTH_TYPE?16:gwi_get_bytes(q->type);
    if(!output || q->bytes!=(gwi_scalar(q->type)?0:n) || capacity<n || capacity>GWI_GET_CAPACITY)return GWI_ARGUMENT;
    unsigned peer_query=q->type==GWI_SSID || q->type==GWI_CHANNEL || q->type==GWI_BSSID ||
        q->type==GWI_RSSI || q->type==GWI_NOISE || q->type==GWN_AUTH_TYPE;
    if(peer_query && (!s->associated || !s->peer))return GWI_NOT_READY;
    if(s->peer && (s->peer->ssid_len>32 || !gww_channel(s->peer->frequency)))return GWI_ARGUMENT;
    uint8_t *p=(uint8_t *)output;for(size_t i=0;i<n;i++)p[i]=0;
    switch(q->type) {
    case GWI_CAPABILITIES:p[0]=1u<<3;p[1]=1u<<6;break; /* CCMP + WPA2, no WPA1/SAE/TKIP */
    case GWI_STATE:gwq_w32(p,s->phase);break;
    case GWI_PHY_MODE:gwq_w32(p,1);gwq_w32(p+4,0x1e);
        if(s->associated && s->peer)gwq_w32(p+8,s->peer->frequency<3000?8:2);break;
    case GWI_SSID:gwq_copy(p,s->peer->ssid,s->peer->ssid_len);break;
    case GWI_BSSID:gwq_copy(p,s->peer->bssid,6);break;
    case GWI_CHANNEL:gwq_w32(p,1);gwq_w32(p+4,1);gwq_w32(p+8,gww_channel(s->peer->frequency));
        gwq_w32(p+12,2|(s->peer->frequency<3000?8:16));break;
    case GWI_RSSI:case GWI_NOISE: {
        uint32_t value=(uint32_t)(int32_t)(q->type==GWI_RSSI?s->peer->rssi:s->peer->noise);
        gwq_w32(p,1);gwq_w32(p+4,1); /* unit=0: dBm */
        gwq_w32(p+12,value);gwq_w32(p+28,value);gwq_w32(p+32,value);gwq_w32(p+48,value);break;
    }
    case GWN_AUTH_TYPE:gwq_w32(p,1);gwq_w32(p+4,1);gwq_w32(p+8,s->security==GWT_WPA3_SAE?0x1000:s->security?8:0);break;
    case GWI_ASSOCIATE_RESULT:gwq_w32(p,s->result);break;
    case GWI_ASSOCIATION_STATUS:gwq_w32(p,s->status);break;
    default:return GWI_UNSUPPORTED;
    }
    *bytes=n;return GWI_OK;
}
#endif
