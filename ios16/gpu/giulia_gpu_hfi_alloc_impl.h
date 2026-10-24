/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Included after the GiuliaGPU definition. This is the only place where the
 * HFI allocation responder is bound to real service state: the development
 * pool, the GPU/GMU leaves the mapping unit installs, and the cache publish
 * path. Only a live native HFI client may consume requests and ring its GMU
 * doorbell; manual terminal control keeps the queue until that client exists.
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
    if(!self || index>=self->devPool->count)return 1;
    allocation=&self->devPool->allocations[index];
    if(!allocation->bytes || ((allocation->offset|allocation->bytes)&3u) || allocation->bytes<GGA_MIN_ALIGN ||
       !ggd_bounds(allocation->offset,allocation->bytes) || self->devUcode[index])return 1;
    /* Adjacent GPU VA chunks have independent CPU mappings and physical
     * descriptors. Never walk past one descriptor through its CPU pointer. */
    uint32_t at=allocation->offset,left=allocation->bytes;
    while(left) {
        uint32_t bytes=GGD_CHUNK-at%GGD_CHUNK;
        if(bytes>left)bytes=left;
        uint8_t *cpu=self->devPointer(at);
        if(!cpu)return 1;
        bzero(cpu,bytes);gpuHfiBarrier(NULL);
        if(*(volatile uint32_t *)cpu || *(volatile uint32_t *)(cpu+bytes-4))return 1;
        if(!giulia_gpu_cache_publish(cpu,bytes))return 1;
        at+=bytes;left-=bytes;
    }
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
    hfiAllocOps.decode=hfiPoolDecode;
    IOLog("GIULIA GPU HFI ALLOC: pool_ready=%u base=%#x cache_base=%#x next=%#x cached=%#x reserved_kinds=refused\n",
        hfiAllocReady,GGD_BASE,GGD_CACHE,devPool->next,devPool->cached);
}

int GiuliaGPU::hfiPoolDecode(void *cookie,const uint32_t *words,unsigned count,
    struct gga_request *out)
{
    struct gga_pool *pool=(struct gga_pool *)cookie;
    GiuliaGPU *self=pool?(GiuliaGPU *)pool->io.cookie:NULL;
    if(!self || !out)return GGA_ARGUMENT;
    return giulia_gpu_hotload_decide(&self->gpuProto,words,count,out);
}

int GiuliaGPU::hfiPumpReady(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self || !self->hfiAllocReady || !self->hostPrepared || !self->manualMemoryPinned ||
       !self->devMappingsManaged || !self->allocations[self->hfiIndex])return 0;
    if(!self->stackReady || !self->stackState.policy.allow_device_writes ||
       self->stackState.quarantined || !ggsk_entered(&self->stackState,GGSK_HFI))return 0;
    if(!self->gmuActive || !self->gmuState.hfi_ready || self->gmuState.quarantined ||
       !self->smmuActive || !self->smmuLeaseHeld || self->platformPoisoned || self->gmuFaulted)return 0;
    return 1;
}

int GiuliaGPU::hfiPumpNotify(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(hfiPumpReady(cookie)!=1)return 1;
    struct gg_gmu_ops ops=self->gmuOps();
    /* Pinned gen7_hfi_cmdq_write: packet/index publication -> wmb -> doorbell.
     * A write-only interrupt-set register is never verified by a readback. */
    return gg_gmu_barrier(&ops,&self->gmuState,0) ||
        gg_gmu_write(&ops,&self->gmuState,GGB_HOST2GMU_INTR_SET,1,0) ||
        gg_gmu_barrier(&ops,&self->gmuState,0);
}

int GiuliaGPU::hfiPumpOnce(unsigned budget)
{
    struct ggh_io io;
    int code;
    if(hfiPumpReady(this)!=1)return GGPP_ARGUMENT;
    if(hfiPump->quarantined)return GGPP_QUARANTINED;
    if(hfiPump->stalled)return GGPP_REFUSED;
    io.words=(volatile uint32_t *)allocations[hfiIndex]->getBytesNoCopy();
    io.bytes=GGH_BYTES;
    io.iova=GGH_IOVA;
    io.cookie=NULL;
    io.barrier=gpuHfiBarrier;
    const struct ggpp_transport transport={this,hfiPumpReady,hfiPumpNotify};
    code=ggpp_pump(&io,&hfiAllocOps,&transport,hfiPump,budget,hfiPumpScratch,64);
    if(code!=GGPP_OK)
        IOLog("GIULIA GPU HFI ALLOC: pump=%s received=%u replied=%u dropped=%u refused=%u quarantine=%u stalled=%u header=%#x\n",
            ggpp_code_name((unsigned)code),hfiPump->received,hfiPump->replied,hfiPump->dropped,
            hfiPump->refused,hfiPump->quarantined,hfiPump->stalled,hfiPump->last_header);
    return code;
}

void GiuliaGPU::hfiAllocReport()
{
    IOLog("GIULIA GPU PROTO: abi=1 epoch=%#llx calls=%llu selected=%llu fallback=%llu rejected=%llu source=resident-hfi-alloc hardware_reloadable=0\n",
        (unsigned long long)gpuProto.epoch,(unsigned long long)gpuProto.calls,
        (unsigned long long)gpuProto.selected,(unsigned long long)gpuProto.fallback,
        (unsigned long long)gpuProto.rejected);
    IOLog("GIULIA GPU PROTO: module_allocated=%u module_reused=%u selected_is_parser_result=1 hardware_validated=0\n",
        hfiPump->alloc.module_allocated,hfiPump->alloc.module_reused);
    IOLog("GIULIA GPU HFI ALLOC: ready=%u allocations=%u published=%u packets=%u received=%u replied=%u dropped=%u refused=%u\n",
        hfiAllocReady,hfiAllocPool.pool?hfiAllocPool.pool->count:0,hfiAllocPool.published,
        hfiPump->packets,hfiPump->received,hfiPump->replied,hfiPump->dropped,hfiPump->refused);
    IOLog("GIULIA GPU HFI ALLOC: quarantine=%u stalled=%u last_header=%#x failures=%u mapped_gpu=%u mapped_gmu=%u\n",
        hfiPump->quarantined,hfiPump->stalled,hfiPump->last_header,hfiAllocPool.failures,
        hfiAllocPool.mapped_gpu,hfiAllocPool.mapped_gmu);
}
#endif
