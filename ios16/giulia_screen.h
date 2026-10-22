/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * GSC1 screen chunks. Explicit LE fields, bounded payload and CRC32; no
 * pointers or packed/unaligned structs cross the USB debug stream.
 */
#ifndef GIULIA_SCREEN_H
#define GIULIA_SCREEN_H
#include <stdint.h>
#include <stddef.h>
#define GSC_WIDTH 316u
#define GSC_HEIGHT 695u
#define GSC_FRAME_BYTES (GSC_WIDTH*GSC_HEIGHT*2u)
#define GSC_HEADER_BYTES 44u
#define GSC_CHUNK_BYTES 1024u
static const uint8_t gsc_magic[8]={0,'G','S','C','1',13,10,26};
static inline uint32_t gsc_crc(const void *data,size_t bytes)
{
    const uint8_t *p=(const uint8_t *)data;uint32_t crc=0xffffffffu;
    for(size_t i=0;i<bytes;i++) {
        crc^=p[i];for(unsigned j=0;j<8;j++)crc=(crc>>1)^(0xedb88320u& (0u-(crc&1u)));
    }
    return ~crc;
}
static inline void gsc_u32(uint8_t *p,uint32_t v)
{for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static inline unsigned gsc_control(uint8_t *out,unsigned capacity,uint32_t id)
{
    if(!out || capacity<GSC_HEADER_BYTES)return 0;
    for(unsigned i=0;i<GSC_HEADER_BYTES;i++)out[i]=0;
    for(unsigned i=0;i<8;i++)out[i]=gsc_magic[i];
    gsc_u32(out+8,id?id:1);gsc_u32(out+12,2); /* verified stop/resync record */
    gsc_u32(out+40,gsc_crc(out+8,32));return GSC_HEADER_BYTES;
}
static inline unsigned gsc_packet(uint8_t *out,unsigned capacity,unsigned mps,
    const uint8_t *frame,uint32_t id,unsigned snapshot,uint32_t offset,uint32_t frame_crc)
{
    if(!out || !frame || !id || snapshot>1 || capacity<=GSC_HEADER_BYTES ||
       (mps!=64 && mps!=512) || offset>=GSC_FRAME_BYTES)return 0;
    unsigned n=GSC_FRAME_BYTES-offset;
    if(n>GSC_CHUNK_BYTES)n=GSC_CHUNK_BYTES;
    if(n>capacity-GSC_HEADER_BYTES)n=capacity-GSC_HEADER_BYTES;
    /* Every queued DWC3 transfer ends in a short packet, without padding. */
    if(!((GSC_HEADER_BYTES+n)%mps))n--;
    if(!n)return 0;
    for(unsigned i=0;i<8;i++)out[i]=gsc_magic[i];
    gsc_u32(out+8,id);gsc_u32(out+12,snapshot);
    out[16]=GSC_WIDTH&255;out[17]=GSC_WIDTH>>8;
    out[18]=GSC_HEIGHT&255;out[19]=GSC_HEIGHT>>8;
    gsc_u32(out+20,GSC_FRAME_BYTES);gsc_u32(out+24,offset);gsc_u32(out+28,n);
    gsc_u32(out+32,gsc_crc(frame+offset,n));gsc_u32(out+36,frame_crc);
    gsc_u32(out+40,gsc_crc(out+8,32));
    for(unsigned i=0;i<n;i++)out[GSC_HEADER_BYTES+i]=frame[offset+i];
    return GSC_HEADER_BYTES+n;
}
#endif
