/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Bounded, read-only Gunyah RM query payload codec. Pinned wire definitions:
 * OnePlus kernel d763ed47deec55451366c1187f813f2f309d88a7,
 * drivers/virt/gunyah/gh_rm_{drv_private.h,iface.c,core.c} and
 * include/linux/gunyah/{gh_common.h,gh_rm_drv.h}. These functions perform no
 * hypercalls and never accept, share, assign, initialize or signal memory.
 */
#ifndef GIULIA_GPU_GUNYAH_QUERIES_H
#define GIULIA_GPU_GUNYAH_QUERIES_H
#include <stdint.h>
#include <stddef.h>
#include "giulia_gpu_fence.h"

#define GGRQ_VM_GET_ID UINT32_C(0x56000010)
#define GGRQ_VM_LOOKUP_NAME UINT32_C(0x56000013)
#define GGRQ_VM_GET_STATE UINT32_C(0x56000017)
#define GGRQ_VM_GET_RESOURCES UINT32_C(0x56000020)
#define GGRQ_VM_GET_VMID UINT32_C(0x56000024)
#define GGRQ_MEM_LOOKUP UINT32_C(0x5100001a)
#define GGRQ_VM_STATUS UINT32_C(0x56100008)
#define GGRQ_PEER_NAME "qcom,cpusysvm"
enum { GGRQ_NAME_BYTES=13, GGRQ_READY=2, GGRQ_RESETTING=10, GGRQ_RESET=11,
    GGRQ_RESOURCE_BYTES=44, GGRQ_MAX_PAYLOAD=16384 };

static inline uint16_t gg_rq_u16(const uint8_t *p)
{return (uint16_t)((uint16_t)p[0]|((uint16_t)p[1]<<8));}
static inline uint32_t gg_rq_u32(const uint8_t *p)
{return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static inline uint64_t gg_rq_u64(const uint8_t *p)
{return (uint64_t)gg_rq_u32(p)|((uint64_t)gg_rq_u32(p+4)<<32);}
static inline void gg_rq_put16(uint8_t *p,uint16_t n)
{p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static inline void gg_rq_put32(uint8_t *p,uint32_t n)
{for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(i*8));}
static inline void gg_rq_put64(uint8_t *p,uint64_t n)
{for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(n>>(i*8));}
static inline void gg_rq_zero(uint8_t *p,size_t n)
{for(size_t i=0;i<n;i++)p[i]=0;}
static inline int gg_rq_vmid_valid(uint16_t id)
{return id!=0 && id!=UINT16_MAX;}

/* Builders return their payload length; zero means insufficient space or
 * invalid inputs. VM_GET_VMID is the sole supported empty-payload query. */
static inline size_t gg_rq_build_lookup_name(uint8_t *out,size_t capacity)
{
    static const char name[]=GGRQ_PEER_NAME;
    if(!out || capacity<20)return 0;
    gg_rq_zero(out,20);gg_rq_put16(out,GGRQ_NAME_BYTES);
    for(unsigned i=0;i<GGRQ_NAME_BYTES;i++)out[4+i]=(uint8_t)name[i];
    return 20;
}
static inline size_t gg_rq_build_vmid(uint8_t *out,size_t capacity,uint16_t vmid)
{
    if(!out || capacity<4 || vmid==UINT16_MAX)return 0;
    gg_rq_put16(out,vmid);gg_rq_put16(out+2,0);return 4;
}
static inline size_t gg_rq_build_mem_lookup(uint8_t *out,size_t capacity,
    uint16_t self,uint16_t peer)
{
    if(!out || capacity<44 || !gg_rq_vmid_valid(self) ||
       !gg_rq_vmid_valid(peer) || self==peer)return 0;
    gg_rq_zero(out,44);gg_rq_put32(out+4,5);gg_rq_put32(out+8,2);
    gg_rq_put16(out+12,self);out[14]=6;
    gg_rq_put16(out+16,peer);out[18]=6;
    gg_rq_put16(out+20,1);gg_rq_put64(out+24,GGF_CARVEOUT_PHYSICAL);
    gg_rq_put64(out+32,GGF_CARVEOUT_BYTES);return 44;
}

/* Parsers return 0 on success, -1 on malformed/unsupported input. Outputs
 * remain untouched on failure. Payloads must already have authenticated RM
 * transport provenance, successful RM error status and matching query seq/id. */
static inline int gg_rq_parse_self(const uint8_t *p,size_t n,uint16_t *out)
{
    if(!p || !out || n!=4 || gg_rq_u16(p+2) || !gg_rq_vmid_valid(gg_rq_u16(p)))return -1;
    *out=gg_rq_u16(p);return 0;
}
static inline int gg_rq_parse_lookup(const uint8_t *p,size_t n,uint16_t *out)
{
    if(!p || !out || n!=8 || gg_rq_u32(p)!=1 || gg_rq_u16(p+6) ||
       !gg_rq_vmid_valid(gg_rq_u16(p+4)))return -1;
    *out=gg_rq_u16(p+4);return 0;
}
static inline int gg_rq_parse_identity(const uint8_t *p,size_t n)
{
    static const char name[]=GGRQ_PEER_NAME;
    if(!p || n<4 || n>GGRQ_MAX_PAYLOAD)return -1;
    uint32_t count=gg_rq_u32(p),seen=0;size_t offset=4;
    if(!count || count>(n-4)/4)return -1;
    for(uint32_t i=0;i<count;i++) {
        if(n-offset<4)return -1;
        uint8_t type=p[offset];size_t bytes=gg_rq_u16(p+offset+2);
        size_t aligned=(bytes+3u)&~(size_t)3u;
        if(type>3 || p[offset+1] || (seen&(1u<<type)) || !bytes || aligned>n-offset-4)return -1;
        seen|=1u<<type;
        if(type==0 && bytes!=16)return -1;
        if(type==2) {
            /* Exact string, optionally including one terminal NUL. */
            if(bytes!=GGRQ_NAME_BYTES && bytes!=GGRQ_NAME_BYTES+1)return -1;
            for(size_t j=0;j<GGRQ_NAME_BYTES;j++)if(p[offset+4+j]!=(uint8_t)name[j])return -1;
            if(bytes==GGRQ_NAME_BYTES+1 && p[offset+4+GGRQ_NAME_BYTES])return -1;
        }
        offset+=4+aligned;
    }
    return offset==n && (seen&(1u<<2)) ? 0 : -1;
}
struct gg_rq_vm_state { uint8_t vm_status,os_status;uint16_t app_status; };
static inline int gg_rq_parse_state(const uint8_t *p,size_t n,struct gg_rq_vm_state *out)
{
    if(!p || !out || n!=4 || p[0]>11 || p[0]==7 || p[1]>4)return -1;
    struct gg_rq_vm_state v={p[0],p[1],gg_rq_u16(p+2)};*out=v;return 0;
}
static inline int gg_rq_parse_status_notification(const uint8_t *p,size_t n,
    uint16_t *vmid,struct gg_rq_vm_state *out)
{
    struct gg_rq_vm_state v;
    if(!p || !vmid || !out || n!=8 || gg_rq_u16(p+2) ||
       !gg_rq_vmid_valid(gg_rq_u16(p)) || gg_rq_parse_state(p+4,4,&v))return -1;
    *vmid=gg_rq_u16(p);*out=v;return 0;
}
static inline int gg_rq_parse_mem_lookup(const uint8_t *p,size_t n,uint32_t *handle)
{
    if(!p || !handle || n!=4)return -1;
    /* No sentinel semantics are invented for this opaque u32. */
    *handle=gg_rq_u32(p);return 0;
}
struct gg_rq_doorbell {
    uint64_t capid;uint32_t resource_handle,virq_handle,virq;uint16_t partner_vmid;
};
static inline int gg_rq_parse_resources(const uint8_t *p,size_t n,uint16_t peer,
    struct gg_rq_doorbell *out)
{
    if(!p || !out || !gg_rq_vmid_valid(peer) || n<4 || n>GGRQ_MAX_PAYLOAD ||
       (n-4)%GGRQ_RESOURCE_BYTES || gg_rq_u32(p)!=(n-4)/GGRQ_RESOURCE_BYTES)return -1;
    unsigned found=0;struct gg_rq_doorbell result={0,0,0,0,0};
    for(size_t offset=4;offset<n;offset+=GGRQ_RESOURCE_BYTES) {
        const uint8_t *e=p+offset;
        if(e[1])return -1;
        if(e[0]!=1 || gg_rq_u32(e+8)!=6)continue;
        /* A second RX resource with this label is ambiguous even if its peer
         * differs. RX capabilities are never inferred from a TX entry. */
        if(found++ || gg_rq_u16(e+2)!=peer || gg_rq_u64(e+12)==UINT64_MAX)return -1;
        result.capid=gg_rq_u64(e+12);result.resource_handle=gg_rq_u32(e+4);
        result.virq_handle=gg_rq_u32(e+20);result.virq=gg_rq_u32(e+24);
        result.partner_vmid=gg_rq_u16(e+2);
    }
    if(found!=1)return -1;
    *out=result;return 0;
}

enum { GGRQ_STEP_SELF, GGRQ_STEP_LOOKUP, GGRQ_STEP_IDENTITY,
    GGRQ_STEP_STATE, GGRQ_STEP_PARCEL, GGRQ_STEP_RESOURCES,
    GGRQ_STEP_RECHECK, GGRQ_STEP_COMPLETE };
struct gg_rq_snapshot {
    uint64_t epoch;
    uint16_t self_vmid,peer_vmid;
    uint8_t step,invalid;
    uint32_t parcel_handle;
    struct gg_rq_doorbell doorbell;
};
static inline void gg_rq_invalidate(struct gg_rq_snapshot *s)
{
    if(!s)return;
    uint64_t epoch=s->epoch;struct gg_rq_snapshot empty={0,0,0,0,0,0,{0,0,0,0,0}};
    *s=empty;s->epoch=epoch;s->invalid=1;
}
/* Zero-initialize before first use. Epochs are local receive-stream generations,
 * not values supplied by RM. Caller serializes access and advances generation
 * on reconnect, loss, timeout, malformed messages or receiver discontinuity. */
static inline int gg_rq_begin(struct gg_rq_snapshot *s,uint64_t epoch)
{
    if(!s || !epoch || epoch<=s->epoch)return -1;
    struct gg_rq_snapshot empty={0,0,0,0,0,0,{0,0,0,0,0}};
    *s=empty;s->epoch=epoch;return 0;
}
static inline uint32_t gg_rq_expected_query(const struct gg_rq_snapshot *s)
{
    if(!s || !s->epoch || s->invalid)return 0;
    switch(s->step) {
    case GGRQ_STEP_SELF:return GGRQ_VM_GET_VMID;
    case GGRQ_STEP_LOOKUP:return GGRQ_VM_LOOKUP_NAME;
    case GGRQ_STEP_IDENTITY:return GGRQ_VM_GET_ID;
    case GGRQ_STEP_STATE:case GGRQ_STEP_RECHECK:return GGRQ_VM_GET_STATE;
    case GGRQ_STEP_PARCEL:return GGRQ_MEM_LOOKUP;
    case GGRQ_STEP_RESOURCES:return GGRQ_VM_GET_RESOURCES;
    default:return 0;
    }
}
static inline int gg_rq_build_query(const struct gg_rq_snapshot *s,uint32_t *msgid,
    uint8_t *out,size_t capacity,size_t *bytes)
{
    uint32_t id=gg_rq_expected_query(s);size_t n;
    if(!id || !msgid || !bytes)return -1;
    if(id==GGRQ_VM_GET_VMID)n=0;
    else if(id==GGRQ_VM_LOOKUP_NAME)n=gg_rq_build_lookup_name(out,capacity);
    else if(id==GGRQ_MEM_LOOKUP)n=gg_rq_build_mem_lookup(out,capacity,s->self_vmid,s->peer_vmid);
    else n=gg_rq_build_vmid(out,capacity,id==GGRQ_VM_GET_RESOURCES ? 0 : s->peer_vmid);
    if(!n && id!=GGRQ_VM_GET_VMID)return -1;
    *msgid=id;*bytes=n;return 0;
}
static inline int gg_rq_accept_reply(struct gg_rq_snapshot *s,uint64_t epoch,
    uint32_t msgid,const uint8_t *payload,size_t bytes)
{
    if(!s)return -1;
    if(epoch!=s->epoch || !gg_rq_expected_query(s) || msgid!=gg_rq_expected_query(s)) {
        gg_rq_invalidate(s);return -1;
    }
    int error=-1;struct gg_rq_vm_state vm;
    switch(s->step) {
    case GGRQ_STEP_SELF:error=gg_rq_parse_self(payload,bytes,&s->self_vmid);break;
    case GGRQ_STEP_LOOKUP:
        error=gg_rq_parse_lookup(payload,bytes,&s->peer_vmid);
        if(!error && s->peer_vmid==s->self_vmid)error=-1;
        break;
    case GGRQ_STEP_IDENTITY:error=gg_rq_parse_identity(payload,bytes);break;
    case GGRQ_STEP_STATE:case GGRQ_STEP_RECHECK:
        error=gg_rq_parse_state(payload,bytes,&vm);
        if(!error && vm.vm_status!=GGRQ_READY)error=-1;
        break;
    case GGRQ_STEP_PARCEL:error=gg_rq_parse_mem_lookup(payload,bytes,&s->parcel_handle);break;
    case GGRQ_STEP_RESOURCES:error=gg_rq_parse_resources(payload,bytes,s->peer_vmid,&s->doorbell);break;
    default:break;
    }
    if(error){gg_rq_invalidate(s);return -1;}
    ++s->step;return 0;
}
/* Any status event for the peer invalidates the in-flight snapshot, including
 * READY: that event may start a different lifecycle. It never re-arms evidence.
 * A malformed/unknown notification or an event before peer lookup also fails
 * closed. An unrelated, well-formed VM_STATUS event leaves the snapshot alone. */
static inline int gg_rq_notification(struct gg_rq_snapshot *s,uint32_t msgid,
    const uint8_t *payload,size_t bytes)
{
    struct gg_rq_vm_state vm;uint16_t vmid;
    if(!s)return -1;
    if(!s->epoch || s->invalid || msgid!=GGRQ_VM_STATUS ||
       gg_rq_parse_status_notification(payload,bytes,&vmid,&vm) ||
       !s->peer_vmid || vmid==s->peer_vmid || vmid==s->self_vmid) {
        gg_rq_invalidate(s);return -1;
    }
    return 0;
}
/* This reports only a completed query sequence. It is NOT memory ownership,
 * accepted sharing, IRQ binding, controller/queue ABI, CPU cache coherence,
 * fence completion or authorization to register a GPU fence provider. */
static inline int gg_rq_snapshot_complete(const struct gg_rq_snapshot *s)
{return s && s->epoch && !s->invalid && s->step==GGRQ_STEP_COMPLETE;}
#endif
