/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * F2H_MSG_MEM_ALLOC responder for the gen7 HWSCHED HFI channel.
 *
 * The firmware asks the host for memory it wants to touch:
 *
 *   hdr, version, desc{ gpu_addr u64, flags, mem_kind, host_handle,
 *                       gmu_handle, gmu_addr, size, align (v>0) }
 *
 * and the host answers with the same descriptor where the addresses, the host
 * handle are filled in. The logical request size is preserved even when the
 * backing is rounded up. This unit implements the decision
 * logic only. Real memory still has to come from a provider that owns the
 * backing, its GPU/GMU mappings and its cache maintenance:
 *
 *   - `reserve` returns an owned block whose GPU (and, when required, GMU)
 *     address is already valid; it must never invent an address;
 *   - `publish` zeroes the block, reads it back and publishes the cache lines;
 *   - `release` is only called on a block that was never published, or after
 *     the caller confirmed the device no longer touches it.
 *
 * Rules mirrored from the pinned vendor source (see
 * hardware/gpu-hfi-allocation-source.json):
 *
 *   - the reply preserves the request flags, mem_kind and logical align field;
 *     only size becomes the real allocation size;
 *   - `host_mem_handle` in the reply is the request's `gmu_mem_handle`;
 *   - ordinary SYSRAM that does not ask for GFX access gets the GMU bits folded
 *     in (flags | 0x38) and always reports a GMU address;
 *   - GFX-accessible memory reports a GMU address only when GMU_ACC is set;
 *   - kinds 5/19/22/23 need their own backend and are refused without one.
 *
 * Reusing the same (mem_kind, gmu_mem_handle) key returns the recorded block,
 * but only when size, flags and align match. A different descriptor under the
 * same key is refused instead of silently sharing memory.
 */
#ifndef GIULIA_GPU_HFI_ALLOC_H
#define GIULIA_GPU_HFI_ALLOC_H
#include <stdint.h>
#include <stddef.h>

enum {
    GGA_MSG_ID=20,          /* F2H_MSG_MEM_ALLOC */
    GGA_ACK_TYPE=1,         /* HFI_MSG_ACK */
    GGA_HDR_WORDS=11,       /* hdr, version, desc[9] */
    GGA_LEGACY_WORDS=10,    /* hdr, reserved, desc_legacy[8] */
    GGA_MAX=32,             /* firmware allocates at most a handful per boot */
    GGA_MIN_ALIGN=4096,
    GGA_MAX_ALIGN_EXP=30,
    GGA_KIND_MAX=26
};

/* Request flags, verbatim from the pinned adreno_hfi.h. */
enum {
    GGA_FLAG_GFX_ACC=1u<<0,
    GGA_FLAG_GFX_PRIV=1u<<1,
    GGA_FLAG_GFX_WRITE=1u<<2,
    GGA_FLAG_GMU_ACC=1u<<3,
    GGA_FLAG_GMU_PRIV=1u<<4,
    GGA_FLAG_GMU_WRITE=1u<<5,
    GGA_FLAG_GMU_BUFFERABLE=1u<<6,
    GGA_FLAG_GMU_CACHEABLE=1u<<7,
    GGA_FLAG_HOST_ACC=1u<<8,
    GGA_FLAG_HOST_INIT=1u<<9,
    GGA_FLAG_GFX_SECURE=1u<<12,
    GGA_FLAG_KNOWN=0x13ffu,       /* bits 0..9 and 12; everything else refused */
    GGA_FLAG_GMU_ONLY_FOLD=0x38u  /* GMU_ACC|GMU_PRIV|GMU_WRITEABLE */
};

/* mem_kind values used by the decision logic. */
enum {
    GGA_KIND_GENERIC=0, GGA_KIND_RB=1, GGA_KIND_SCRATCH=2, GGA_KIND_CSW_SMMU_INFO=3,
    GGA_KIND_CSW_PRIV_NON_SECURE=4, GGA_KIND_CSW_PRIV_SECURE=5,
    GGA_KIND_CSW_NON_PRIV=6, GGA_KIND_CSW_COUNTER=7, GGA_KIND_CTXTREC_PREEMPT_CNTR=8,
    GGA_KIND_SYS_LOG=9, GGA_KIND_CRASH_DUMP=10, GGA_KIND_MMIO_DPU=11,
    GGA_KIND_MMIO_TCSR=12, GGA_KIND_MMIO_QDSS_STM=13, GGA_KIND_PROFILE=14,
    GGA_KIND_USER_PROFILE_IBS=15, GGA_KIND_CMD_BUFFER=16,
    GGA_KIND_GPU_BUSY_DATA_BUFFER=17, GGA_KIND_GPU_BUSY_CMD_BUFFER=18,
    GGA_KIND_MMIO_IPC_CORE=19, GGA_KIND_MMIO_IPCC_AOSS=20,
    GGA_KIND_CSW_LPAC_PRIV_NON_SECURE=21, GGA_KIND_MEMSTORE=22,
    GGA_KIND_HW_FENCE=23, GGA_KIND_PREEMPT_SCRATCH=24, GGA_KIND_AQE_BUFFER=25,
    GGA_RESERVED_MASK=(1u<<GGA_KIND_CSW_PRIV_SECURE)|(1u<<GGA_KIND_MMIO_IPC_CORE)|
        (1u<<GGA_KIND_MEMSTORE)|(1u<<GGA_KIND_HW_FENCE)
};

enum gga_code {
    GGA_OK=0, GGA_ARGUMENT, GGA_HEADER, GGA_KIND, GGA_FLAGS, GGA_SECURE,
    GGA_SIZE, GGA_ALIGN, GGA_RESERVED, GGA_CONFLICT, GGA_FULL, GGA_PROVIDER,
    GGA_PUBLISH, GGA_RELEASE, GGA_QUARANTINED
};

/* Derived resident-only plan. Never part of an HFI descriptor or NLH state.
 * valid=1 is installed only after the resident adapter validates every module
 * result bit. The resource path consumes these fields instead of recomputing
 * the same decision from the wire flags. */
struct gga_plan { uint32_t valid,effective_flags,rounded_bytes,masters; };
enum { GGA_PLAN_GFX=1u,GGA_PLAN_GMU=2u,GGA_PLAN_RESERVED=4u };
struct gga_request {
    uint32_t hdr,version;
    uint64_t gpu_addr;
    uint32_t flags,kind,host_handle,gmu_handle,gmu_addr,size,align;
    uint32_t gpu_alignment,size_alignment; /* bytes, never below 4096 */
    struct gga_plan plan;
};

struct gga_block {
    void *backing;          /* provider token; never interpreted here */
    uint64_t gpu_addr,gmu_addr;
    uint32_t bytes;         /* real allocation size, >= request size */
    uint32_t effective_flags;
};

struct gga_ops {
    void *cookie;
    /* Owned backing with valid GPU/GMU addresses. Must refuse rather than
     * substitute ordinary RAM, a queue alias or a fabricated handle. */
    int (*reserve)(void *cookie,const struct gga_request *request,uint32_t bytes,
        uint32_t gpu_alignment,uint32_t size_alignment,struct gga_block *out);
    /* Zero, read back and publish. Non-zero leaves the block unpublished. */
    int (*publish)(void *cookie,const struct gga_block *block);
    /* 0 only when the backing is confirmed released; backing may be NULL. */
    int (*release)(void *cookie,const struct gga_block *block);
    int (*barrier)(void *cookie);
    /* Dedicated backends for kinds 5/19/22/23. NULL refuses those kinds. */
    int (*reserved)(void *cookie,const struct gga_request *request,struct gga_block *out);
    /* Optional resident protocol adapter. It must populate the same bounded
     * request and preserve validation errors before any resource callback. */
    int (*decode)(void *cookie,const uint32_t *words,unsigned count,struct gga_request *out);
};

struct gga_entry {
    struct gga_request request;
    struct gga_block block;
    unsigned published;
};

struct gga_state {
    struct gga_entry entry[GGA_MAX];
    unsigned count,seq,allocated,published,refused,reused,quarantined,last;
    unsigned module_allocated,module_reused; /* Successful resource outcomes, not parser calls. */
};

static inline uint32_t gga_alignment(uint32_t exponent)
{
    return exponent>GGA_MAX_ALIGN_EXP?0u:
        (exponent>12?((uint32_t)1u<<exponent):(uint32_t)GGA_MIN_ALIGN);
}

static inline uint32_t gga_round_up(uint32_t bytes,uint32_t alignment)
{
    uint64_t rounded;
    if (!alignment || (alignment&(alignment-1))) return 0;
    rounded=((uint64_t)bytes+alignment-1)&~(uint64_t)(alignment-1);
    return rounded>UINT32_MAX?0u:(uint32_t)rounded;
}

static inline const char *gga_kind_name(uint32_t kind)
{
    switch (kind) {
    case GGA_KIND_GENERIC: return "generic";
    case GGA_KIND_RB: return "ringbuffer";
    case GGA_KIND_SCRATCH: return "scratch";
    case GGA_KIND_CSW_SMMU_INFO: return "csw-smmu-info";
    case GGA_KIND_CSW_PRIV_NON_SECURE: return "csw-priv-non-secure";
    case GGA_KIND_CSW_PRIV_SECURE: return "csw-priv-secure";
    case GGA_KIND_CSW_NON_PRIV: return "csw-non-priv";
    case GGA_KIND_CSW_COUNTER: return "csw-counter";
    case GGA_KIND_CTXTREC_PREEMPT_CNTR: return "ctxtrec-preempt-counter";
    case GGA_KIND_SYS_LOG: return "sys-log";
    case GGA_KIND_CRASH_DUMP: return "crash-dump";
    case GGA_KIND_MMIO_DPU: return "mmio-dpu";
    case GGA_KIND_MMIO_TCSR: return "mmio-tcsr";
    case GGA_KIND_MMIO_QDSS_STM: return "mmio-qdss-stm";
    case GGA_KIND_PROFILE: return "profile";
    case GGA_KIND_USER_PROFILE_IBS: return "user-profile-ibs";
    case GGA_KIND_CMD_BUFFER: return "cmd-buffer";
    case GGA_KIND_GPU_BUSY_DATA_BUFFER: return "gpu-busy-data";
    case GGA_KIND_GPU_BUSY_CMD_BUFFER: return "gpu-busy-cmd";
    case GGA_KIND_MMIO_IPC_CORE: return "mmio-ipc-core";
    case GGA_KIND_MMIO_IPCC_AOSS: return "mmio-ipcc-aoss";
    case GGA_KIND_CSW_LPAC_PRIV_NON_SECURE: return "csw-lpac-priv-non-secure";
    case GGA_KIND_MEMSTORE: return "memstore";
    case GGA_KIND_HW_FENCE: return "hw-fence";
    case GGA_KIND_PREEMPT_SCRATCH: return "preempt-scratch";
    case GGA_KIND_AQE_BUFFER: return "aqe-buffer";
    default: return "unknown";
    }
}

static inline const char *gga_code_name(unsigned code)
{
    const char *const names[]={"ok","argument","header","kind","flags","secure",
        "size","align","reserved-kind","conflict","full","provider","publish",
        "release","quarantined"};
    return code<sizeof(names)/sizeof(names[0])?names[code]:"invalid";
}

static inline int gga_ops_valid(const struct gga_ops *ops)
{
    return ops && ops->reserve && ops->publish && ops->release && ops->barrier;
}

/* Reserved kinds carry their own backend; the mask is indexed by kind value. */
static inline int gga_kind_reserved(uint32_t kind)
{
    return kind<GGA_KIND_MAX && ((GGA_RESERVED_MASK>>kind)&1u)!=0;
}

static inline uint32_t gga_effective_flags(uint32_t flags,uint32_t kind)
{
    if (gga_kind_reserved(kind)) return flags;
    /* Vendor: ordinary SYSRAM without GFX access is GMU-only and forced
     * GMU_ACC|GMU_PRIV|GMU_WRITEABLE, independent of the request bits. */
    return (flags&GGA_FLAG_GFX_ACC)?flags:(flags|GGA_FLAG_GMU_ONLY_FOLD);
}
static inline uint32_t gga_request_flags(const struct gga_request *request)
{
    return request->plan.valid?request->plan.effective_flags:
        gga_effective_flags(request->flags,request->kind);
}
/* Validation may recompute the fixed upstream envelope. Once checked, the
 * actual reserve/map path below uses the adopted plan fields themselves. */
static inline int gga_plan_valid(const struct gga_request *request)
{
    if(!request->plan.valid)return 1;
    unsigned reserved=(unsigned)gga_kind_reserved(request->kind);
    unsigned gfx=(request->flags&GGA_FLAG_GFX_ACC)!=0 || request->kind==GGA_KIND_HW_FENCE;
    unsigned gmu=reserved || !(request->flags&GGA_FLAG_GFX_ACC) || (request->flags&GGA_FLAG_GMU_ACC);
    unsigned masters=(gfx?GGA_PLAN_GFX:0u)|(gmu?GGA_PLAN_GMU:0u)|(reserved?GGA_PLAN_RESERVED:0u);
    uint32_t rounded=reserved?0u:gga_round_up(request->size,request->size_alignment);
    return request->plan.valid==1 && request->plan.effective_flags==gga_effective_flags(request->flags,request->kind) &&
        request->plan.masters==masters && request->plan.rounded_bytes==rounded && (reserved || rounded);
}

/* Which masters must see the block, decided only from the request. */
static inline int gga_needs_gfx(const struct gga_request *request)
{
    if(request && request->plan.valid)return (request->plan.masters&GGA_PLAN_GFX)!=0;
    return request && ((request->flags&GGA_FLAG_GFX_ACC)||(request->kind==GGA_KIND_HW_FENCE));
}

static inline int gga_needs_gmu(const struct gga_request *request)
{
    if (!request) return 0;
    if(request->plan.valid)return (request->plan.masters&GGA_PLAN_GMU)!=0;
    if (gga_kind_reserved(request->kind)) return 1;
    if (request->flags&GGA_FLAG_GFX_ACC) return (request->flags&GGA_FLAG_GMU_ACC)!=0;
    /* GMU-only SYSRAM is always reachable from the GMU. */
    return 1;
}

static inline int gga_block_valid(const struct gga_request *request,const struct gga_block *block)
{
    uint64_t align;
    int needs_gfx,needs_gmu;
    if (!request || !block) return 0;
    align=request->gpu_alignment;
    if (block->bytes<request->size) return 0;
    if (block->effective_flags!=gga_request_flags(request)) return 0;
    if(request->plan.valid && block->bytes<request->plan.rounded_bytes)return 0;
    /* Both addresses are page granular, and the GMU address honours the
     * requested VA alignment. A misaligned address is a provider bug. */
    if ((block->gpu_addr|block->gmu_addr)&(GGA_MIN_ALIGN-1)) return 0;
    if (block->gmu_addr && align && (block->gmu_addr&(align-1))) return 0;
    needs_gfx=gga_needs_gfx(request);
    needs_gmu=gga_needs_gmu(request);
    /* No unrequested alias: memory the GPU must not see keeps gpu_addr 0,
     * memory the GMU must not see keeps gmu_addr 0. */
    if (needs_gfx&&!block->gpu_addr) return 0;
    if (!needs_gfx&&block->gpu_addr) return 0;
    if (needs_gmu&&!block->gmu_addr) return 0;
    if (!needs_gmu&&block->gmu_addr) return 0;
    return 1;
}

static inline int gga_parse(const uint32_t *words,unsigned count,struct gga_request *out)
{
    struct gga_request request;
    unsigned index;
    if (!words || !out || count<2) return GGA_ARGUMENT;
    if ((words[0]&0xffu)!=GGA_MSG_ID || ((words[0]>>16)&0xfu)!=0) return GGA_HEADER;
    if (((words[0]>>8)&0xffu)!=count) return GGA_HEADER;
    request.hdr=words[0];
    request.version=words[1];
    if (request.version==0) {
        if (count!=GGA_LEGACY_WORDS) return GGA_HEADER;
        request.align=0;
    } else {
        if (count!=GGA_HDR_WORDS) return GGA_HEADER;
        request.align=words[10];
    }
    index=2;
    request.gpu_addr=(uint64_t)words[index]|((uint64_t)words[index+1]<<32);
    request.flags=words[index+2];
    request.kind=words[index+3];
    request.host_handle=words[index+4];
    request.gmu_handle=words[index+5];
    request.gmu_addr=words[index+6];
    request.size=words[index+7];
    if (request.kind>=GGA_KIND_MAX) return GGA_KIND;
    if (request.flags&~GGA_FLAG_KNOWN) return GGA_FLAGS;
    if (request.flags&GGA_FLAG_GFX_SECURE) return GGA_SECURE;
    if (!request.size) return GGA_SIZE;
    request.gpu_alignment=gga_alignment(request.align&0xffu);
    request.size_alignment=gga_alignment((request.align>>8)&0xffu);
    if (!request.gpu_alignment || !request.size_alignment) return GGA_ALIGN;
    if (request.size>UINT32_MAX-request.size_alignment) return GGA_SIZE;
    request.plan.valid=request.plan.effective_flags=request.plan.rounded_bytes=request.plan.masters=0;
    *out=request;
    return GGA_OK;
}

static inline int gga_find(const struct gga_state *state,uint32_t kind,uint32_t gmu_handle)
{
    unsigned i;
    if (!state) return -1;
    for (i=0;i<state->count;++i)
        if (state->entry[i].request.kind==kind && state->entry[i].request.gmu_handle==gmu_handle)
            return (int)i;
    return -1;
}

/* Reply layout, identical for both descriptor versions. */
static inline unsigned gga_build_reply(const struct gga_request *request,
    const struct gga_block *block,uint32_t seq,uint32_t *out)
{
    unsigned words;
    if (!request || !block || !out) return 0;
    /* The vendor always replies with the modern descriptor. A legacy request
     * is expanded with align=0 by hfi_get_mem_alloc_desc(). */
    words=GGA_HDR_WORDS;
    out[0]=GGA_MSG_ID|(GGA_ACK_TYPE<<16)|((seq&0xfffu)<<20)|((words&0xffu)<<8);
    out[1]=request->hdr;
    out[2]=(uint32_t)block->gpu_addr;
    out[3]=(uint32_t)(block->gpu_addr>>32);
    out[4]=request->flags;
    out[5]=request->kind;
    out[6]=request->gmu_handle;  /* host handle echoes the requested GMU handle */
    out[7]=request->gmu_handle;
    out[8]=block->gmu_addr;
    out[9]=request->size;
    out[10]=request->align;
    return words;
}

static inline int gga_handle(const struct gga_ops *ops,struct gga_state *state,
    const uint32_t *words,unsigned count,uint32_t *reply,unsigned capacity,
    unsigned *reply_words)
{
    struct gga_request request;
    struct gga_block block={0,0,0,0,0};
    uint32_t rounded;
    unsigned needed;
    int index,error;

    if (!reply_words) return GGA_ARGUMENT;
    *reply_words=0;
    if (!gga_ops_valid(ops) || !state || !words || !reply) return GGA_ARGUMENT;
    if (state->quarantined) return GGA_QUARANTINED;
    error=ops->decode?ops->decode(ops->cookie,words,count,&request):gga_parse(words,count,&request);
    if (error) {
        ++state->refused;
        state->last=(unsigned)error;
        return error;
    }
    if(!gga_plan_valid(&request)) {
        ++state->refused;state->last=GGA_PROVIDER;return GGA_PROVIDER;
    }
    /* A short reply buffer must fail before anything is allocated. */
    needed=GGA_HDR_WORDS;
    if (capacity<needed) return GGA_ARGUMENT;

    index=gga_find(state,request.kind,request.gmu_handle);
    if (index<0 && state->count>=GGA_MAX) {
        /* Refuse before reserving: nothing is allocated and nothing leaks. */
        ++state->refused;
        state->last=GGA_FULL;
        return GGA_FULL;
    }
    if (index<0) {
        /* Dedicated kinds remain selected by the immutable wire kind. A plan
         * can never redirect secure/IPC/memstore/fence requests to SYSRAM. */
        if (gga_kind_reserved(request.kind)) {
            if (!ops->reserved) {
                ++state->refused;
                state->last=GGA_RESERVED;
                return GGA_RESERVED;
            }
            error=ops->reserved(ops->cookie,&request,&block);
            if (error) {
                ++state->refused;
                state->last=GGA_PROVIDER;
                return GGA_PROVIDER;
            }
        } else {
            rounded=request.plan.valid?request.plan.rounded_bytes:gga_round_up(request.size,request.size_alignment);
            if (!rounded) {
                ++state->refused;
                state->last=GGA_SIZE;
                return GGA_SIZE;
            }
            error=ops->reserve(ops->cookie,&request,rounded,request.gpu_alignment,
                request.size_alignment,&block);
            if (error) {
                ++state->refused;
                state->last=GGA_PROVIDER;
                return GGA_PROVIDER;
            }
            if (!gga_block_valid(&request,&block)) {
                /* Release a block that was never published; a failed release
                 * means the provider may still own memory, so quarantine. */
                if (ops->release(ops->cookie,&block)) {
                    state->quarantined=1;
                    state->last=GGA_RELEASE;
                    return GGA_RELEASE;
                }
                ++state->refused;
                state->last=GGA_PROVIDER;
                return GGA_PROVIDER;
            }
            error=ops->publish(ops->cookie,&block);
            if (error) {
                if (ops->release(ops->cookie,&block)) {
                    state->quarantined=1;
                    state->last=GGA_RELEASE;
                    return GGA_RELEASE;
                }
                ++state->refused;
                state->last=GGA_PUBLISH;
                return GGA_PUBLISH;
            }
            ++state->published;
        }
        state->entry[state->count].request=request;
        state->entry[state->count].block=block;
        state->entry[state->count].published=gga_kind_reserved(request.kind)?0u:1u;
        ++state->count;
        ++state->allocated;
        if(request.plan.valid)++state->module_allocated;
    } else {
        const struct gga_entry *entry=&state->entry[index];
        /* Same key must mean the same descriptor. Anything else is refused so
         * two different requests cannot share one buffer silently. */
        if (entry->request.size!=request.size || entry->request.flags!=request.flags ||
            entry->request.align!=request.align || entry->request.version!=request.version) {
            ++state->refused;
            state->last=GGA_CONFLICT;
            return GGA_CONFLICT;
        }
        block=entry->block;
        /* Published ordinary backing is never reinitialized or released here.
         * Validate an adopted plan against the real cached mapping descriptor. */
        if(!gga_kind_reserved(request.kind) && !gga_block_valid(&request,&block)) {
            ++state->refused;state->last=GGA_PROVIDER;return GGA_PROVIDER;
        }
        ++state->reused;
        if(request.plan.valid)++state->module_reused;
    }
    state->seq=(state->seq+1)&0xfffu;
    if (!state->seq) state->seq=1;
    *reply_words=gga_build_reply(&request,&block,state->seq,reply);
    state->last=GGA_OK;
    return GGA_OK;
}
#endif
