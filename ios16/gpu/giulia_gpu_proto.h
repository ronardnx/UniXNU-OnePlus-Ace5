/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Pointer-free HFI allocation planning shared by the resident adapter and its
 * replaceable protocol module. Resource callbacks and all HFI I/O stay resident.
 */
#ifndef GIULIA_GPU_PROTO_H
#define GIULIA_GPU_PROTO_H
#include "giulia_gpu_hfi_alloc.h"

#define GG_PROTO_NAME "giulia-gpu-proto"
#define GG_PROTO_INTERFACE UINT32_C(0x47480001)
#define GG_PROTO_STATE_ABI 1u
enum {
    GG_PROTO_DOMAIN=1,
    GG_PROTO_OP_ALLOC_PLAN=1,
    GG_PROTO_REQUEST_WORDS=13,
    GG_PROTO_RESPONSE_WORDS=1,
    GG_PROTO_SCHEMA=1,
    GG_PROTO_RAW_WORDS=11
};

/* Envelope: schema, actual packet dword count, then eleven raw u32 words.
 * Missing packet words are zero-filled; supplied words must have no high bits.
 * Packet lengths above eleven are representable so the parser can reject the
 * original length before reading any descriptor fields.
 *
 * Plan: code[7:0], effective flags[20:8], GPU[21], GMU[22], reserved kind[23],
 * zero[31:24], rounded backing bytes[63:32]. Error plans contain only code.
 * A reserved kind has no ordinary backing-size plan: its real owner decides.
 */
static inline int gg_proto_plan_request(const uint64_t *words,unsigned count,uint64_t *out)
{
    uint32_t packet[GG_PROTO_RAW_WORDS],actual;
    struct gga_request request;
    uint32_t rounded=0;
    unsigned i,reserved;
    int error;
    uint64_t plan;
    if(!words || !out || count!=GG_PROTO_REQUEST_WORDS ||
       words[0]!=GG_PROTO_SCHEMA || words[1]>UINT32_MAX)return 22;
    actual=(uint32_t)words[1];
    for(i=0;i<GG_PROTO_RAW_WORDS;++i) {
        if(words[i+2]>UINT32_MAX || (i>=actual && words[i+2]))return 22;
        packet[i]=(uint32_t)words[i+2];
    }
    error=gga_parse(packet,actual,&request);
    if(error) {
        *out=(uint64_t)(unsigned)error;
        return 0;
    }
    reserved=(unsigned)gga_kind_reserved(request.kind);
    if(!reserved) {
        rounded=gga_round_up(request.size,request.size_alignment);
        if(!rounded) {
            *out=GGA_SIZE;
            return 0;
        }
    }
    plan=((uint64_t)gga_effective_flags(request.flags,request.kind)<<8) |
        ((uint64_t)(unsigned)gga_needs_gfx(&request)<<21) |
        ((uint64_t)(unsigned)gga_needs_gmu(&request)<<22) |
        ((uint64_t)reserved<<23) | ((uint64_t)rounded<<32);
    *out=plan;
    return 0;
}
#endif
