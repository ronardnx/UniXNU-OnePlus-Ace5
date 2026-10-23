/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * `gga_ops` backend over the existing development pool (giulia_gpu_dev.h).
 *
 * This is the bridge between the F2H_MSG_MEM_ALLOC responder and the memory the
 * service already owns: a bump allocator inside the 64 MiB pool, the GPU/GMU
 * mappings the service builds for each allocation, and its zero/readback/cache
 * publish path. Everything here is policy-free; the responder decides what is
 * allowed, this unit only answers with what actually exists.
 *
 * The IO callback receives the pool index, not an offset: the service owns the
 * allocation record (`offset`, `bytes`, `flags`) and must not let this unit
 * reinterpret it. A zero address or a failed map is a failure, never a hint to
 * compute an address here.
 *
 * Two limits are deliberate:
 *
 *   - the pool never frees, so a failed reserve can consume pool space. It
 *     never leaves a mapping behind: the adapter unmaps whatever it managed to
 *     map before returning failure;
 *   - `unmap` must report what it can actually confirm. When the service cannot
 *     take a per-allocation mapping back, `release` stays unconfirmed and the
 *     responder quarantines instead of pretending the range is free.
 */
#ifndef GIULIA_GPU_HFI_ALLOC_POOL_H
#define GIULIA_GPU_HFI_ALLOC_POOL_H
#include "giulia_gpu_hfi_alloc.h"
#include "giulia_gpu_dev.h"

/* Space 0 is the GPU window, space 1 the GMU window. */
struct gga_pool_io {
    void *cookie;
    /* Address of allocation `index` in one space, 0 when not mapped. */
    uint64_t (*address)(void *cookie,unsigned index,unsigned space);
    /* Establish the mapping for this allocation; 0 = mapped. */
    int (*map)(void *cookie,unsigned index,unsigned space);
    /* Drop one mapping; 0 = confirmed. */
    int (*unmap)(void *cookie,unsigned index,unsigned space);
    /* Zero, read back and publish the allocation; 0 = published. */
    int (*publish)(void *cookie,unsigned index);
    int (*barrier)(void *cookie);
    /* Fixed owners for kinds 5/19/22/23. NULL keeps those kinds refused. */
    int (*reserved)(void *cookie,const struct gga_request *request,struct gga_block *out);
};

struct gga_pool {
    struct ggd_pool *pool;   /* the service's pool, never a copy */
    struct gga_pool_io io;
    uint64_t base,cache_base;
    unsigned mapped_gpu,mapped_gmu,published,releases,failures;
};

static inline void *gga_pool_token(unsigned index)
{
    return (void *)(uintptr_t)(index+1);
}

static inline int gga_pool_index(const struct gga_pool *p,const struct gga_block *block)
{
    uintptr_t token;
    if (!p || !block) return -1;
    token=(uintptr_t)block->backing;
    if (!token) return -1;
    --token;
    return token<p->pool->count?(int)token:-1;
}

static inline int gga_pool_init(struct gga_pool *p,struct ggd_pool *pool,
    uint64_t base,uint64_t cache_base,const struct gga_pool_io *io)
{
    if (!p || !pool || !io || !io->address || !io->map || !io->unmap || !io->publish || !io->barrier)
        return GGA_ARGUMENT;
    if ((base&(GGA_MIN_ALIGN-1)) || (cache_base&(GGA_MIN_ALIGN-1))) return GGA_ARGUMENT;
    if (base>UINT32_MAX || cache_base>UINT32_MAX) return GGA_ARGUMENT;
    if (base+GGD_BYTES>UINT32_MAX || cache_base+GGD_CACHE_BYTES>UINT32_MAX) return GGA_ARGUMENT;
    pool->count=0;
    pool->next=GGD_CACHE_BYTES;
    pool->cached=0;
    p->pool=pool;
    p->io=*io;
    p->base=base;
    p->cache_base=cache_base;
    p->mapped_gpu=p->mapped_gmu=p->published=p->releases=p->failures=0;
    return ggd_pool_valid(pool)?GGA_OK:GGA_ARGUMENT;
}

static inline uint32_t gga_pool_flags(const struct gga_request *request)
{
    /* The pool keeps permission bits in 0x3ff; bit 0x80 also selects its
     * cacheable window, which matches HFI_MEMFLAG_GMU_CACHEABLE. */
    return gga_effective_flags(request->flags,request->kind)&0x3ffu;
}

static inline void gga_pool_unmap(struct gga_pool *p,unsigned index,unsigned space)
{
    if (p->io.unmap(p->io.cookie,index,space)) ++p->failures;
}

static inline int gga_pool_reserve(void *cookie,const struct gga_request *request,uint32_t bytes,
    uint32_t gpu_alignment,uint32_t size_alignment,struct gga_block *out)
{
    struct gga_pool *p=(struct gga_pool *)cookie;
    struct gga_block block={0,0,0,0,0};
    uint32_t alignment,flags;
    int index;

    if (!p || !request || !out) return 1;
    alignment=size_alignment>gpu_alignment?size_alignment:gpu_alignment;
    flags=gga_pool_flags(request);
    index=ggd_alloc(p->pool,bytes,alignment,flags,request->kind);
    if (index<0) {
        ++p->failures;
        return 1;
    }
    block.backing=gga_pool_token((unsigned)index);
    block.bytes=p->pool->allocations[index].bytes;
    block.effective_flags=gga_effective_flags(request->flags,request->kind);
    if (gga_needs_gfx(request)) {
        if (p->io.map(p->io.cookie,(unsigned)index,0)) {
            ++p->failures;
            return 1;
        }
        ++p->mapped_gpu;
        block.gpu_addr=p->io.address(p->io.cookie,(unsigned)index,0);
        if (!block.gpu_addr) {
            gga_pool_unmap(p,(unsigned)index,0);
            ++p->failures;
            return 1;
        }
    }
    if (gga_needs_gmu(request)) {
        if (p->io.map(p->io.cookie,(unsigned)index,1)) {
            gga_pool_unmap(p,(unsigned)index,0);
            ++p->failures;
            return 1;
        }
        ++p->mapped_gmu;
        block.gmu_addr=p->io.address(p->io.cookie,(unsigned)index,1);
        if (!block.gmu_addr) {
            gga_pool_unmap(p,(unsigned)index,1);
            gga_pool_unmap(p,(unsigned)index,0);
            ++p->failures;
            return 1;
        }
    }
    *out=block;
    return 0;
}

static inline int gga_pool_publish(void *cookie,const struct gga_block *block)
{
    struct gga_pool *p=(struct gga_pool *)cookie;
    int index;
    if (!p || !block) return 1;
    index=gga_pool_index(p,block);
    if (index<0) {
        ++p->failures;
        return 1;
    }
    if (p->io.publish(p->io.cookie,(unsigned)index)) {
        ++p->failures;
        return 1;
    }
    if (p->io.barrier(p->io.cookie)) {
        ++p->failures;
        return 1;
    }
    ++p->published;
    return 0;
}

/* Only called by the responder for a block that was never published, or after
 * a confirmed stop. Unmapping is the only release the bump pool can give, so an
 * unconfirmed unmap keeps the responder from reusing the range. */
static inline int gga_pool_release(void *cookie,const struct gga_block *block)
{
    struct gga_pool *p=(struct gga_pool *)cookie;
    int index,failed=0;
    if (!p || !block) return 1;
    index=gga_pool_index(p,block);
    if (index<0) return 1;
    if (block->gpu_addr && p->io.unmap(p->io.cookie,(unsigned)index,0)) failed=1;
    if (block->gmu_addr && p->io.unmap(p->io.cookie,(unsigned)index,1)) failed=1;
    ++p->releases;
    return failed?1:0;
}

static inline int gga_pool_barrier(void *cookie)
{
    struct gga_pool *p=(struct gga_pool *)cookie;
    return p && p->io.barrier?p->io.barrier(p->io.cookie):1;
}

static inline int gga_pool_reserved(void *cookie,const struct gga_request *request,
    struct gga_block *out)
{
    struct gga_pool *p=(struct gga_pool *)cookie;
    if (!p || !p->io.reserved) return 1;
    return p->io.reserved(p->io.cookie,request,out);
}

static inline struct gga_ops gga_pool_ops(struct gga_pool *p)
{
    struct gga_ops ops;
    ops.cookie=p;
    ops.reserve=gga_pool_reserve;
    ops.publish=gga_pool_publish;
    ops.release=gga_pool_release;
    ops.barrier=gga_pool_barrier;
    ops.reserved=p && p->io.reserved?gga_pool_reserved:0;
    return ops;
}
#endif
