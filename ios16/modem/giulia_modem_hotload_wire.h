/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_MODEM_HOTLOAD_WIRE_H
#define GIULIA_MODEM_HOTLOAD_WIRE_H
#include "giulia_modem_proto.h"

/* GH ABI 1 frozen scalar wire format. These local names deliberately do not
 * depend on the optional resident bridge header or expose kernel pointers. */
#define GMR_HOTLOAD_DOMAIN 4u
#define GMR_HOTLOAD_QMI_RESPONSE 1u
#define GMR_HOTLOAD_INTERFACE_ABI UINT32_C(0x47480001)
#define GMR_HOTLOAD_STATE_ABI 1u
#define GMR_HOTLOAD_HEADER_WORDS 4u
#define GMR_HOTLOAD_MAX_PAYLOAD 224u
#define GMR_HOTLOAD_MAX_WORDS 32u
#define GMR_HOTLOAD_ORIGIN_NATIVE 0u
#define GMR_HOTLOAD_ORIGIN_FIXTURE 1u

/* request[0]: kind U32, transaction U16, reserved U16.
 * request[1]: payload bytes U32, expected message U16, reserved U16.
 * request[2]: opaque operation number; request[3]: origin 0 or 1.
 * Remaining words contain complete payload bytes in little-endian order,
 * with zero padding in the final word. Response[0] is gmr_proto_check's
 * packed verdict. Oversized frames use the resident parser, never truncation.
 * A zero-byte non-null payload intentionally supports malformed-frame probes.
 * Output storage must not overlap payload. Invalid input leaves all words zero.
 */
static inline uint32_t gmr_hotload_pack(uint64_t request[32],uint32_t kind,
    uint16_t txn,uint64_t opid,uint32_t origin,const uint8_t *payload,size_t bytes)
{
    size_t i;
    if(!request)return 0;
    for(i=0;i<GMR_HOTLOAD_MAX_WORDS;i++)request[i]=0;
    if(!payload || bytes>GMR_HOTLOAD_MAX_PAYLOAD || origin>GMR_HOTLOAD_ORIGIN_FIXTURE)return 0;
    request[0]=(uint64_t)kind|((uint64_t)txn<<32);
    request[1]=(uint64_t)bytes|((uint64_t)gmr_proto_message(kind)<<32);
    request[2]=opid;request[3]=origin;
    for(i=0;i<bytes;i++)request[GMR_HOTLOAD_HEADER_WORDS+i/8u]|=(uint64_t)payload[i]<<((i%8u)*8u);
    return GMR_HOTLOAD_HEADER_WORDS+(uint32_t)((bytes+7u)/8u);
}
#endif
