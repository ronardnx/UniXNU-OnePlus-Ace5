/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * GPU carveout Gunyah memory payload codec; no HVC, SCM assignment or mapping.
 * Pinned definitions: kernel d763ed47deec55451366c1187f813f2f309d88a7,
 * gh_rm_iface.c, gh_rm_drv_private.h and gh_rm_drv.h. Primary sharing in
 * hw_fence_drv_utils.c (module 453d8875f815bf9dfad9035982861226a4823a6c)
 * first performs SCM assignment, then MEM_SHARE NORMAL/flags0, label5,
 * self+peer RW ACL and one exact carveout SGL. It does not call MEM_ACCEPT
 * or MEM_NOTIFY. The notify builders here are standalone ABI support and
 * do not add those operations to that primary path.
 */
#ifndef GIULIA_GPU_GUNYAH_MEMORY_H
#define GIULIA_GPU_GUNYAH_MEMORY_H
#include <stdint.h>
#include <stddef.h>
#include "giulia_gpu_fence.h"

#define GGMEM_SHARE UINT32_C(0x51000013)
#define GGMEM_NOTIFY UINT32_C(0x51000017)
#define GGMEM_SHARED UINT32_C(0x51100011)
#define GGMEM_RELEASED UINT32_C(0x51100012)
#define GGMEM_ACCEPTED UINT32_C(0x51100013)
enum { GGMEM_LABEL=5,GGMEM_RW=6,GGMEM_SHARE_BYTES=44,
    GGMEM_NOTIFY_SHARED_BYTES=20,GGMEM_NOTIFY_OWNER_BYTES=12,
    GGMEM_PARTICIPANT_BYTES=12,GGMEM_SHARED_PREFIX_BYTES=20,
    GGMEM_MAX_PAYLOAD=63*232,GGMEM_RECIPIENT_SHARED=1,
    GGMEM_OWNER_RELEASED=2,GGMEM_OWNER_ACCEPTED=4,
    GGMEM_OK=0,GGMEM_INVALID=-1,GGMEM_UNSUPPORTED=-2 };

static inline uint16_t gg_mem_u16(const uint8_t *p)
{return (uint16_t)((uint16_t)p[0]|((uint16_t)p[1]<<8));}
static inline uint32_t gg_mem_u32(const uint8_t *p)
{return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static inline void gg_mem_put16(uint8_t *p,uint16_t n)
{p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static inline void gg_mem_put32(uint8_t *p,uint32_t n)
{for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(i*8));}
static inline void gg_mem_put64(uint8_t *p,uint64_t n)
{for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(n>>(i*8));}
static inline void gg_mem_zero(uint8_t *p,size_t n)
{for(size_t i=0;i<n;i++)p[i]=0;}
static inline int gg_mem_vmid(uint16_t vmid)
{return vmid!=0 && vmid!=UINT16_MAX;}

/* Builders return their fixed serialized size, or zero without modifying
 * output. Resolve actual VMIDs in the current owned RM epoch: the board's
 * peer-name enum 3 is not a VMID fallback. No variable-sized append operation
 * is supported. The ACL includes the owner because this is SHARE, not LEND.
 * Producing these bytes proves neither current SCM ownership nor permission
 * to perform the operation; no caller is installed by this header. */
static inline size_t gg_mem_build_share(uint8_t *out,size_t capacity,uint16_t self,uint16_t peer)
{
    if(!out || capacity<GGMEM_SHARE_BYTES || !gg_mem_vmid(self) ||
       !gg_mem_vmid(peer) || self==peer)return 0;
    gg_mem_zero(out,GGMEM_SHARE_BYTES);gg_mem_put32(out+4,GGMEM_LABEL);
    gg_mem_put32(out+8,2);gg_mem_put16(out+12,self);out[14]=GGMEM_RW;
    gg_mem_put16(out+16,peer);out[18]=GGMEM_RW;
    gg_mem_put16(out+20,1);gg_mem_put64(out+24,GGF_CARVEOUT_PHYSICAL);
    gg_mem_put64(out+32,GGF_CARVEOUT_BYTES);
    /* Trailing memory-attribute descriptor is four zero bytes, even when
     * the caller supplies no attributes to gh_rm_mem_share. */
    return GGMEM_SHARE_BYTES;
}

/* mem_info_tag is caller-selected opaque u32 metadata, not automatically the
 * parcel label. Preserve all handle/tag bits, including zero and UINT32_MAX;
 * the pinned API defines no invalid sentinel for either value. */
static inline size_t gg_mem_build_notify_shared(uint8_t *out,size_t capacity,
    uint32_t handle,uint32_t mem_info_tag,uint16_t peer)
{
    if(!out || capacity<GGMEM_NOTIFY_SHARED_BYTES || !gg_mem_vmid(peer))return 0;
    gg_mem_zero(out,GGMEM_NOTIFY_SHARED_BYTES);gg_mem_put32(out,handle);
    out[4]=GGMEM_RECIPIENT_SHARED;gg_mem_put32(out+8,mem_info_tag);
    gg_mem_put16(out+12,1);gg_mem_put16(out+16,peer);
    return GGMEM_NOTIFY_SHARED_BYTES;
}
static inline size_t gg_mem_build_notify_owner(uint8_t *out,size_t capacity,
    uint32_t handle,uint32_t mem_info_tag,unsigned flag)
{
    if(!out || capacity<GGMEM_NOTIFY_OWNER_BYTES ||
       (flag!=GGMEM_OWNER_RELEASED && flag!=GGMEM_OWNER_ACCEPTED))return 0;
    gg_mem_zero(out,GGMEM_NOTIFY_OWNER_BYTES);gg_mem_put32(out,handle);
    out[4]=(uint8_t)flag;gg_mem_put32(out+8,mem_info_tag);
    /* Owner notifications have no VMID descriptor, not a zero-count one. */
    return GGMEM_NOTIFY_OWNER_BYTES;
}

/* Parse only an already assembled, authenticated, seq/id-matched RM payload
 * with successful RM status. Outputs remain untouched on every failure.
 * Neither a parsed response nor a memory notification establishes GPU fence
 * readiness, IRQ ownership, cache attributes or controller/queue ABI. */
static inline int gg_mem_parse_share_reply(const uint8_t *p,size_t n,uint32_t *handle)
{
    if(!p || !handle || n!=4)return GGMEM_INVALID;
    *handle=gg_mem_u32(p);return GGMEM_OK;
}
static inline int gg_mem_parse_notify_reply(const uint8_t *p,size_t n)
{(void)p;return n?GGMEM_INVALID:GGMEM_OK;}

struct gg_mem_participant {
    uint32_t message_id,handle,mem_info_tag;
    uint16_t participant_vmid;
};
/* Both ACCEPTED and RELEASED use the same exact twelve-byte structure.
 * Match the live parcel, dynamically resolved participant and explicit tag
 * before publishing an event. A stale event is not silently accepted. */
static inline int gg_mem_parse_participant(uint32_t message_id,const uint8_t *p,size_t n,
    uint32_t handle,uint16_t peer,uint32_t mem_info_tag,struct gg_mem_participant *out)
{
    if(!p || !out || n!=GGMEM_PARTICIPANT_BYTES || !gg_mem_vmid(peer) ||
       (message_id!=GGMEM_ACCEPTED && message_id!=GGMEM_RELEASED) ||
       gg_mem_u32(p)!=handle || gg_mem_u16(p+4)!=peer || gg_mem_u16(p+6) ||
       gg_mem_u32(p+8)!=mem_info_tag)return GGMEM_INVALID;
    struct gg_mem_participant result={message_id,handle,mem_info_tag,peer};
    *out=result;return GGMEM_OK;
}

struct gg_mem_shared_prefix {
    uint32_t handle,label,mem_info_tag;
    uint16_t owner_vmid;
    uint8_t mem_type,trans_type,flags;
    size_t unparsed_bytes;
};
/* Diagnostic prefix only. The pinned gh_rm_drv.h defines these twenty bytes
 * but explicitly leaves arrangement of the three trailing variable arrays
 * as TODO. No cached implementation defines their counts or serialization.
 * Consequently this function deliberately publishes NO ACL, range or shared
 * ownership evidence. Only the fixed NORMAL/SHARE/flags0 profile is supported.
 * unparsed_bytes is a warning to the caller, not an ignored valid extension. */
static inline int gg_mem_parse_shared_prefix(const uint8_t *p,size_t n,
    struct gg_mem_shared_prefix *out)
{
    if(!p || !out || n<GGMEM_SHARED_PREFIX_BYTES || n>GGMEM_MAX_PAYLOAD ||
       p[4]!=0 || p[5]!=2 || p[6] || p[7] || !gg_mem_vmid(gg_mem_u16(p+8)) ||
       gg_mem_u16(p+10) || gg_mem_u32(p+12)!=GGMEM_LABEL)return GGMEM_INVALID;
    struct gg_mem_shared_prefix result={gg_mem_u32(p),GGMEM_LABEL,gg_mem_u32(p+16),
        gg_mem_u16(p+8),p[4],p[5],p[6],n-GGMEM_SHARED_PREFIX_BYTES};
    *out=result;return GGMEM_OK;
}
/* Full MEM_SHARED acceptance fails closed until the variable tail ABI has
 * pinned evidence. Even a valid prefix is not a complete parcel descriptor.
 * Keep out unchanged on UNSUPPORTED, just as on malformed input. */
static inline int gg_mem_parse_shared(const uint8_t *p,size_t n,struct gg_mem_shared_prefix *out)
{
    struct gg_mem_shared_prefix prefix;
    if(!out || gg_mem_parse_shared_prefix(p,n,&prefix))return GGMEM_INVALID;
    return GGMEM_UNSUPPORTED;
}
#endif
