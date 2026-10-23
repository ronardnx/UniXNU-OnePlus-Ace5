/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Import only an immutable kernel-registered descriptor. No carveout clearing,
 * synthetic client, controller ACK, queue consumption or completion signaling.
 */
static bool gpuFenceObserve(const struct ggf_provider *p,struct ggf_observation *out)
{
    if(!p || !giulia_gpu_fence_ipcc_owns(p->identity,p->epoch) || !ggfp_live(p))return false;
    return ggf_snapshot(p->memory,p->layout.bytes,p->layout.physical,&p->layout,out,p->barrier,p->cookie) &&
        ggfp_live(p) && giulia_gpu_fence_ipcc_owns(p->identity,p->epoch);
}
int GiuliaGPU::auditFenceMappings()
{
    struct ggm_tables gmu;struct ggf_observation observation;
    if(platformPoisoned || !gpuMappingTable(tableMemory,gmuRoot,&gmu))return 8;
    if(!fenceMapped) {
        if(fenceProvider || fenceFlags || !ggf_mapping(&gmu,NULL,0,GGM_VERIFY)) {
            platformPoisoned=true;return 8;
        }
        return 0;
    }
    if(!ipcMapped || !ggi_owns(&gpuIpcOwner,GGI_GPU,this) ||
       fenceProvider!=ggfp_get(&gpuFenceProvider) || !gpuFenceObserve(fenceProvider,&observation) ||
       !ggf_mapping(&gmu,&fenceProvider->layout,fenceFlags,GGM_VERIFY)) {
        platformPoisoned=true;return 8;
    }
    return 0;
}
int GiuliaGPU::fenceAllocate(uint32_t bytes,uint32_t alignment,uint32_t flags,uint32_t kind)
{
    if(kind!=23 || !bytes || !(flags&8u) || (flags&~0x178u) || alignment>>16 ||
       (alignment&255)>22 || ((alignment>>8)&255)>22)return 4;
    const struct ggf_provider *p=ggfp_get(&gpuFenceProvider);
    if(!p) {
        IOLog("GIULIA GPU FENCE: unavailable reason=provider-not-registered completion_verified=0\n");return 7;
    }
    if(!ggfp_request(bytes,alignment,flags,kind,&p->layout) ||
       (GGF_IOVA&((1u<<(alignment&255))-1)))return 4;
    if(!hostPrepared || !manualMemoryPinned || !devMappingsManaged || !ipcMapped || platformPoisoned ||
       !ggi_owns(&gpuIpcOwner,GGI_GPU,this))return 8;
    if(fenceMapped && (fenceProvider!=p || fenceFlags!=flags))return 4;
    if(!giulia_gpu_fence_ipcc_claim(p->identity,p->epoch))return 8;
    struct ggf_observation observation;
    if(!gpuFenceObserve(p,&observation)) {
        if(fenceMapped)platformPoisoned=true;
        IOLog("GIULIA GPU FENCE: unavailable reason=provider-epoch-or-ready-or-layout completion_verified=0\n");
        return fenceMapped?8:7;
    }
    if(auditFenceMappings() || syncDevMappings())return 8;
    bool reused=fenceMapped;
    if(!reused) {
        struct ggm_tables gmu;
        if(!gpuMappingTable(tableMemory,gmuRoot,&gmu) || !ggf_mapping(&gmu,&p->layout,flags,GGM_CHECK))return 8;
        /* Pin the exact source identity before any leaf becomes valid. Neither
         * stale READY nor a later registration can replace this allocation. */
        fenceProvider=p;fenceFlags=flags;fenceMapped=true;
        if(!ggf_mapping(&gmu,&p->layout,flags,GGM_APPLY) || syncDevMappings() || auditFenceMappings()) {
            platformPoisoned=true;return 8;
        }
    } else if(auditFenceMappings())return 8;
    IOLog("GIULIA GPU FENCE: bytes=%#x gpu=0 gmu=%#x flags=%#x kind=23 epoch=%#llx reused=%u cpu_offset=none mapped=1 completion_verified=0\n",
        p->layout.bytes,GGF_IOVA,flags,(unsigned long long)p->epoch,reused);
    return 0;
}
int GiuliaGPU::fenceStatus()
{
    const struct ggf_provider *p=ggfp_get(&gpuFenceProvider);
    struct ggf_observation observation;
    bool ready=p && gpuFenceObserve(p,&observation);
    if(fenceMapped && (!ready || auditFenceMappings())) {platformPoisoned=true;return 8;}
    IOLog("GIULIA GPU FENCE: registered=%u ready=%u mapped=%u completion_verified=0\n",p!=NULL,ready,fenceMapped);
    if(ready)IOLog("GIULIA GPU FENCE: epoch=%#llx tx_read=%u tx_write=%u rx_read=%u rx_write=%u stable=%u completion_verified=0\n",
        (unsigned long long)p->epoch,observation.tx_read,observation.tx_write,
        observation.rx_read,observation.rx_write,observation.stable);
    return 0;
}
