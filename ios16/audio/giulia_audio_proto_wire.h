/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Pure GPR response classification, no device state or external pointers.
 * OnePlus audio-kernel 453d8875f815bf9dfad9035982861226a4823a6c:
 * include/ipc/gpr-lite.h, ipc/gpr-lite.c, dsp/spf-core.c; AudioReach GPR
 * gpr_packet.h and gsl_main.c at 76d04ffb30caa1fe609b94f9f6081210cb962fc1.
 */
#ifndef GIULIA_AUDIO_PROTO_WIRE_H
#define GIULIA_AUDIO_PROTO_WIRE_H
#include <stdint.h>
#include <stddef.h>

#define GAPT_WIRE_ABI 1u
#define GAPT_WORDS 17u
#define GAPT_PACKET_BYTES 68u /* 15 header words + 8 payload bytes */
#define GAPT_SPF_REPLY 1u
#define GAPT_APM_REPLY 2u
#define GAPT_IGNORE 0u
#define GAPT_SPF_READY 1u
#define GAPT_SPF_STATE 2u
#define GAPT_SPF_BASIC 3u /* transient bootstrap response, never ready */
#define GAPT_APM_ACK 4u
#define GAPT_APM_ERROR 5u

/* Words: ABI, kind, token, local/remote port, original opcode, packet bytes,
 * pending, then nine LE byte-packed words. The GH invocation owns epoch. */
static inline void gapt_request(uint64_t r[GAPT_WORDS],unsigned kind,unsigned pending,
    uint32_t token,uint32_t local_port,uint32_t remote_port,uint32_t opcode,
    const uint8_t *packet,size_t bytes)
{
    for(unsigned i=0;i<GAPT_WORDS;i++)r[i]=0;
    r[0]=GAPT_WIRE_ABI;r[1]=kind;r[2]=token;r[3]=local_port;r[4]=remote_port;
    r[5]=opcode;r[6]=bytes;r[7]=pending;
    if(packet&&bytes<=GAPT_PACKET_BYTES)
        for(size_t i=0;i<bytes;i++)r[8+i/8]|=(uint64_t)packet[i]<<((i%8)*8);
}
static inline uint32_t gapt_packet_word(const uint64_t *r,unsigned word)
{ return (uint32_t)(r[8+word/2]>>((word%2)*32)); }
static inline uint64_t gapt_value(unsigned verdict,uint32_t status)
{ return ((uint64_t)status<<32)|verdict; }
static inline uint64_t gapt_classify(const uint64_t *r,unsigned words)
{
    uint32_t h,domain,opcode,hs,status;
    if(!r||words!=GAPT_WORDS||r[0]!=GAPT_WIRE_ABI||
       (r[1]!=GAPT_SPF_REPLY&&r[1]!=GAPT_APM_REPLY)||r[7]!=1||
       r[2]>UINT32_MAX||r[3]>UINT32_MAX||r[4]>UINT32_MAX||r[5]>UINT32_MAX||
       r[6]<28||r[6]>GAPT_PACKET_BYTES)return GAPT_IGNORE;
    h=gapt_packet_word(r,0);hs=((h>>4)&15u)*4u;
    domain=gapt_packet_word(r,1);
    /* Client data's low nibble is valid. Its high nibble and byte 7 are
     * reserved. Optional GPR header words belong before the payload. */
    if((h&15u)||hs<24||h>>8!=r[6]||(domain&0xffu)!=3||
       ((domain>>8)&0xffu)!=2||(domain&0xfff00000u)||
       gapt_packet_word(r,2)!=r[4]||gapt_packet_word(r,3)!=r[3]||
       gapt_packet_word(r,4)!=r[2])return GAPT_IGNORE;
    opcode=gapt_packet_word(r,5);
    if(r[1]==GAPT_SPF_REPLY&&opcode==0x02001007u&&r[6]==hs+4u) {
        status=gapt_packet_word(r,hs/4);
        return gapt_value(status==1?GAPT_SPF_READY:GAPT_SPF_STATE,status);
    }
    if(opcode!=0x02001005u||r[6]!=hs+8u||gapt_packet_word(r,hs/4)!=r[5])
        return GAPT_IGNORE;
    status=gapt_packet_word(r,hs/4+1);
    return gapt_value(r[1]==GAPT_SPF_REPLY?GAPT_SPF_BASIC:
        (status?GAPT_APM_ERROR:GAPT_APM_ACK),status);
}
#endif
