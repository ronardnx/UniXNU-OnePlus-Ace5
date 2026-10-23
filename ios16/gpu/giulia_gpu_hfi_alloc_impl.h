/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Included after the GiuliaGPU definition. This is the only place where the
 * HFI allocation responder is bound to real service state: the development
 * pool, the GPU/GMU leaves the mapping unit installs, and the cache publish
 * path. It performs no device write and no SMC/HVC call of its own.
 *
 * Ownership rules:
 *   - `map` may only run while mappings are managed and the platform is not
 *     poisoned; `mapDevAllocation()` installs both masters' leaves;
 *   - `address` reports what those leaves actually expose, gated by the same
 *     ggm_attributes() the audit uses; a zero answer is a failure;
 *   - `unmap` cannot confirm anything: the pool is append-only until reboot,
 *     so a release stays unconfirmed and the responder quarantines instead of
 *     pretending the range is free;
 *   - `publish` zeroes through the CPU mapping, reads the range back, then
 *     publishes the cache lines and barriers.
 */
#ifndef GIULIA_GPU_HFI_ALLOC_IMPL_H
#define GIULIA_GPU_HFI_ALLOC_IMPL_H

uint64_t GiuliaGPU::hfiPoolAddress(void *cookie,unsigned index,unsigned space)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    const struct ggd_allocation *allocation;
    if(!self || !self->devMappingsManaged || space>1 || index>=self->devPool->count)return 0;
    allocation=&self->devPool->allocations[index];
    if(!ggm_attributes(allocation->flags,space,self->devUcode[index]?1u:0u))return 0;
    return (space?((allocation->flags&0x80u)?(uint64_t)GGD_CACHE:(uint64_t)GGD_BASE)
        :(uint64_t)GGD_BASE)+allocation->offset;
}

int GiuliaGPU::hfiPoolMap(void *cookie,unsigned index,unsigned space)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    (void)space; /* mapDevAllocation installs the leaves for both masters */
    if(!self || !self->devMappingsManaged || index>=self->devPool->count)return 1;
    return self->mapDevAllocation(index)?1:0;
}

int GiuliaGPU::hfiPoolUnmap(void *cookie,unsigned index,unsigned space)
{
    /* Append-only pool: no per-allocation unmap exists that could be
     * confirmed, so the release must stay unconfirmed. */
    (void)cookie;
    (void)index;
    (void)space;
    return 1;
}

int GiuliaGPU::hfiPoolPublish(void *cookie,unsigned index)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    const struct ggd_allocation *allocation;
    uint8_t *cpu;
    if(!self || index>=self->devPool->count)return 1;
    allocation=&self->devPool->allocations[index];
    if(!allocation->bytes || (allocation->bytes&3u) || allocation->bytes<GGA_MIN_ALIGN)return 1;
    cpu=self->devPointer(allocation->offset);
    if(!cpu)return 1;
    bzero(cpu,allocation->bytes);
    gpuHfiBarrier(NULL);
    /* Read the CPU view back: a non-zero word means this range is not the
     * untouched buffer the firmware is about to receive. */
    if(*(volatile uint32_t *)cpu || *(volatile uint32_t *)(cpu+allocation->bytes-4))return 1;
    if(!giulia_gpu_cache_publish(cpu,allocation->bytes))return 1;
    gpuHfiBarrier(NULL);
    return 0;
}

int GiuliaGPU::hfiPoolBarrier(void *cookie)
{
    (void)cookie;
    gpuHfiBarrier(NULL);
    return 0;
}

void GiuliaGPU::hfiAllocInit()
{
    struct gga_pool_io io;
    if(hfiAllocReady)return;
    io.cookie=this;
    io.address=hfiPoolAddress;
    io.map=hfiPoolMap;
    io.unmap=hfiPoolUnmap;
    io.publish=hfiPoolPublish;
    io.barrier=hfiPoolBarrier;
    io.reserved=NULL; /* kinds 5/19/22/23 stay refused until they have owners */
    hfiAllocReady=gga_pool_init(&hfiAllocPool,devPool,GGD_BASE,GGD_CACHE,&io)==GGA_OK;
    hfiAllocOps=gga_pool_ops(&hfiAllocPool);
    IOLog("GIULIA GPU HFI ALLOC: pool_ready=%u base=%#x cache_base=%#x next=%#x cached=%#x reserved_kinds=refused\n",
        hfiAllocReady,GGD_BASE,GGD_CACHE,devPool->next,devPool->cached);
}

int GiuliaGPU::hfiPumpOnce(unsigned budget)
{
    struct ggh_io io;
    int code;
    if(!hfiAllocReady)return GGPP_ARGUMENT;
    if(!hostPrepared || !manualMemoryPinned || !devMappingsManaged)return GGPP_ARGUMENT;
    if(platformPoisoned || gmuFaulted || !allocations[hfiIndex])return GGPP_ARGUMENT;
    io.words=(volatile uint32_t *)allocations[hfiIndex]->getBytesNoCopy();
    io.bytes=GGH_BYTES;
    io.iova=GGH_IOVA;
    io.cookie=NULL;
    io.barrier=gpuHfiBarrier;
    code=ggpp_pump(&io,&hfiAllocOps,hfiPump,budget,hfiPumpScratch,64);
    if(code!=GGPP_OK)
        IOLog("GIULIA GPU HFI ALLOC: pump=%s received=%u replied=%u dropped=%u refused=%u quarantine=%u stalled=%u header=%#x\n",
            ggpp_code_name((unsigned)code),hfiPump->received,hfiPump->replied,hfiPump->dropped,
            hfiPump->refused,hfiPump->quarantined,hfiPump->stalled,hfiPump->last_header);
    return code;
}

void GiuliaGPU::hfiAllocReport()
{
    IOLog("GIULIA GPU HFI ALLOC: ready=%u allocations=%u published=%u packets=%u received=%u replied=%u dropped=%u refused=%u\n",
        hfiAllocReady,hfiAllocPool.pool?hfiAllocPool.pool->count:0,hfiAllocPool.published,
        hfiPump->packets,hfiPump->received,hfiPump->replied,hfiPump->dropped,hfiPump->refused);
    IOLog("GIULIA GPU HFI ALLOC: quarantine=%u stalled=%u last_header=%#x failures=%u mapped_gpu=%u mapped_gmu=%u\n",
        hfiPump->quarantined,hfiPump->stalled,hfiPump->last_header,hfiAllocPool.failures,
        hfiAllocPool.mapped_gpu,hfiAllocPool.mapped_gmu);
}
#endif
