/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Independent QMAP data framing. Wire reference: OnePlus modules commit
 * 453d8875f815bf9dfad9035982861226a4823a6c, datarmnet/core/rmnet_map.h and
 * rmnet_map_data.c. No Linux implementation code is included.
 * Supported negotiated modes: basic QMAP, or QMAPv5 checksum-header framing
 * with software-completed TX checksums. Coalescing/TSO and command frames are
 * separate mechanisms and are explicitly rejected, never treated as IP.
 */
#ifndef GIULIA_MODEM_QMAP_H
#define GIULIA_MODEM_QMAP_H
#include <stddef.h>
#include <stdint.h>

enum { GMAP_OK,GMAP_ARGUMENT,GMAP_TRUNCATED,GMAP_WIRE,GMAP_UNSUPPORTED,GMAP_FULL };
struct gmap_packet {
    const uint8_t *ip;
    size_t bytes,consumed;
    uint8_t mux,checksum_valid;
};
static inline int gmap_encode(uint8_t *out,size_t capacity,const uint8_t *ip,
    size_t bytes,uint8_t mux,unsigned version,size_t *used)
{
    size_t extra=version==5?4u:0u,padding=(4u-(bytes&3u))&3u,total,i;
    if(!used)return GMAP_ARGUMENT;
    *used=0;
    if(!out || !ip || !bytes)return GMAP_ARGUMENT;
    if(version!=1 && version!=5)return GMAP_UNSUPPORTED;
    if(bytes>65535u-padding)return GMAP_FULL;
    total=4u+extra+bytes+padding;
    if(total>capacity)return GMAP_FULL;
    out[0]=(uint8_t)(padding|(extra?0x40u:0u));out[1]=mux;
    out[2]=(uint8_t)((bytes+padding)>>8);out[3]=(uint8_t)(bytes+padding);
    if(extra){out[4]=4;out[5]=out[6]=out[7]=0;}
    for(i=0;i<bytes;i++)out[4u+extra+i]=ip[i];
    for(i=0;i<padding;i++)out[4u+extra+bytes+i]=0;
    *used=total;return GMAP_OK;
}
/* Return one frame and consumed length so callers can process an aggregation
 * without interpreting padding as a second IP packet. A failure commits no
 * output and does not consume input. IP validation belongs to the same bearer
 * runtime that selected the mux, family and negotiated MTU. */
static inline int gmap_decode(const uint8_t *p,size_t bytes,unsigned version,
    struct gmap_packet *out)
{
    size_t payload,padding,extra=0;
    struct gmap_packet packet;
    if(!p || !out)return GMAP_ARGUMENT;
    if(version!=1 && version!=5)return GMAP_UNSUPPORTED;
    if(bytes<4)return GMAP_TRUNCATED;
    if(p[0]&0x80u)return GMAP_UNSUPPORTED;
    payload=(size_t)p[2]*256u+p[3];padding=p[0]&63u;
    if(!payload || padding>=payload)return GMAP_WIRE;
    packet.checksum_valid=0;
    if(p[0]&0x40u) {
        if(version!=5)return GMAP_UNSUPPORTED;
        if(bytes<8)return GMAP_TRUNCATED;
        if(p[4]!=4u)return GMAP_UNSUPPORTED; /* one terminal checksum header */
        if((p[5]&0x4fu) || p[6] || p[7])return GMAP_WIRE;
        packet.checksum_valid=(uint8_t)(p[5]>>7);extra=4;
    }
    if(payload>bytes-4u-extra)return GMAP_TRUNCATED;
    packet.ip=p+4u+extra;packet.bytes=payload-padding;
    packet.consumed=4u+extra+payload;packet.mux=p[1];
    *out=packet;return GMAP_OK;
}
#endif
