/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Included after GiuliaGPU, platform methods and gpuHfiBarrier definitions.
 * The development worker is the sole caller; all backing stays pinned.
 */
static bool gpuMappingTable(IOBufferMemoryDescriptor *memory,uint64_t root,struct ggm_tables *out)
{
    if(!memory || !out)return false;
    IOByteCount span=0;
    uint64_t physical=memory->getPhysicalSegment(0,&span,kIOMemoryMapperNone);
    out->words=(uint64_t *)memory->getBytesNoCopy();out->bytes=(size_t)memory->getLength();out->physical=root;
    return physical==root && span>=out->bytes && ggm_table_valid(out);
}
static bool gpuMappingOwned(IOBufferMemoryDescriptor *const *memory,struct ggm_owned *out)
{
    for(unsigned i=0;i<GGD_CHUNKS;i++) {
        if(!memory[i] || memory[i]->getLength()<GGD_CHUNK)return false;
        IOByteCount span=0;out->physical[i]=memory[i]->getPhysicalSegment(0,&span,kIOMemoryMapperNone);
        if(span<GGD_CHUNK)return false;
    }
    return true;
}

int GiuliaGPU::syncDevMappings()
{
    if(!platformPrepared || platformPoisoned || !manualMemoryPinned || !gpuTableMemory || !tableMemory || !platformPower())return 8;
    uint32_t power=platformRead(0x3d7e340);
    if(power==0xffffffffu)return 8;
    if((power&3)!=3 && ((power&3)!=0 || platformRead(0x3d8ec40) ||
       (platformRead(0x3d8ec30)&(1u<<23))))return 8;
    const uint64_t bases[2]={0x3dc0000,0x3dc2000},roots[2]={gpuRoot,gmuRoot};
    bool active[2];
    for(unsigned i=0;i<2;i++) {
        active[i]=(platformRead(bases[i])&1)!=0;
        if(active[i]) {
            if(platformRead(bases[i]+0x58)&0xc00001feu)return 8;
            uint64_t ttbr=(uint64_t)platformRead(bases[i]+0x20)|((uint64_t)platformRead(bases[i]+0x24)<<32);
            if((ttbr&GGM_ADDRESS)!=roots[i])return 8;
        }
    }
    if(!giulia_gpu_cache_publish(gpuTableMemory->getBytesNoCopy(),(size_t)gpuTableMemory->getLength()) ||
       !giulia_gpu_cache_publish(tableMemory->getBytesNoCopy(),(size_t)tableMemory->getLength()))return 8;
    gpuHfiBarrier(NULL);
    for(unsigned i=0;i<2;i++)if(active[i]) {
        platformWrite(bases[i]+0x618,0);platformWrite(bases[i]+0x7f0,0xffffffff);
        if(!platformPoll(bases[i]+0x7f4,1,0))return 8;
    }
    return 0;
}

int GiuliaGPU::configureDevMappings()
{
    if(devMappingsManaged)return platformPoisoned?8:0;
    if(!platformPrepared || platformPoisoned || !manualMemoryPinned || !platformIdle() || devPool.count>GGD_MAX)return 8;
    struct ggm_tables gpu,gmu;struct ggm_owned owned;
    if(!gpuMappingTable(gpuTableMemory,gpuRoot,&gpu) || !gpuMappingTable(tableMemory,gmuRoot,&gmu) ||
       !gpuMappingOwned(devMemory,&owned) || !ggm_reset_pool(&gpu,&gmu,&owned,0))return 8;
    for(unsigned i=0;i<devPool.count;i++) {
        if(!ggm_allocation(&gpu,&gmu,&owned,&devPool.allocations[i],devUcode[i],GGM_CHECK))return 8;
        for(unsigned j=0;j<i;j++) {
            const struct ggd_allocation &a=devPool.allocations[i],&b=devPool.allocations[j];
            if(a.offset<b.offset+b.bytes && b.offset<a.offset+a.bytes)return 8;
        }
    }
    if(syncDevMappings())return 8;
    /* Initial configuration requires disabled contexts, so no live BBM needed. */
    if(!ggm_reset_pool(&gpu,&gmu,&owned,1)){platformPoisoned=true;return 8;}
    for(unsigned i=0;i<devPool.count;i++)
        if(!ggm_allocation(&gpu,&gmu,&owned,&devPool.allocations[i],devUcode[i],GGM_APPLY)) {
            platformPoisoned=true;return 8;
        }
    if(syncDevMappings()){platformPoisoned=true;return 8;}
    devMappingsManaged=true;
    IOLog("GIULIA GPU MAPPING: managed=1 allocations=%u unallocated_aliases=0 gpu_data_nx=1 separate_gpu_gmu_permissions=1\n",devPool.count);
    return 0;
}

int GiuliaGPU::mapDevAllocation(unsigned index)
{
    if(!devMappingsManaged || platformPoisoned || index>=devPool.count || devPool.count>GGD_MAX)return 8;
    struct ggm_tables gpu,gmu;struct ggm_owned owned;
    if(!gpuMappingTable(gpuTableMemory,gpuRoot,&gpu) || !gpuMappingTable(tableMemory,gmuRoot,&gmu) ||
       !gpuMappingOwned(devMemory,&owned) ||
       !ggm_allocation(&gpu,&gmu,&owned,&devPool.allocations[index],devUcode[index],GGM_CHECK))return 8;
    if(syncDevMappings())return 8;
    if(!ggm_allocation(&gpu,&gmu,&owned,&devPool.allocations[index],devUcode[index],GGM_CLEAR) ||
       syncDevMappings() ||
       !ggm_allocation(&gpu,&gmu,&owned,&devPool.allocations[index],devUcode[index],GGM_APPLY) ||
       syncDevMappings()) {platformPoisoned=true;return 8;}
    const struct ggd_allocation &a=devPool.allocations[index];
    IOLog("GIULIA GPU MAPPING: allocation=%u offset=%#x bytes=%#x flags=%#x ucode=%u gpu_attrs=%#llx gmu_attrs=%#llx synced=1\n",
        index,a.offset,a.bytes,a.flags,devUcode[index],(unsigned long long)ggm_attributes(a.flags,0,devUcode[index]),
        (unsigned long long)ggm_attributes(a.flags,1,devUcode[index]));
    return 0;
}

int GiuliaGPU::auditDevMappings()
{
    if(!devMappingsManaged || platformPoisoned || !manualMemoryPinned)return 8;
    struct ggm_tables gpu,gmu;struct ggm_owned owned;unsigned char ucode[GGD_MAX];
    for(unsigned i=0;i<GGD_MAX;i++)ucode[i]=devUcode[i]?1:0;
    if(!gpuMappingTable(gpuTableMemory,gpuRoot,&gpu) || !gpuMappingTable(tableMemory,gmuRoot,&gmu) ||
       !gpuMappingOwned(devMemory,&owned) || !ggm_audit_pool(&gpu,&gmu,&owned,&devPool,ucode)) {
        platformPoisoned=true;return 8;
    }
    for(unsigned i=0;i<devPool.count;i++) {
        const struct ggd_allocation &a=devPool.allocations[i];
        uint64_t ga=ggm_attributes(a.flags,0,devUcode[i]),ma=ggm_attributes(a.flags,1,devUcode[i]);
        IOLog("GIULIA GPU MAPPING: audit allocation=%u gpu=%#x gmu=%#x gpu_attrs=%#llx gmu_attrs=%#llx bytes=%#x ucode=%u verified_pages=%u\n",
            i,ga?GGD_BASE+a.offset:0,ma?((a.flags&0x80u)?GGD_CACHE:GGD_BASE)+a.offset:0,
            (unsigned long long)ga,(unsigned long long)ma,a.bytes,devUcode[i],a.bytes/4096);
    }
    IOLog("GIULIA GPU MAPPING: audit_complete=1 owned_pages=%u secure_pool_untouched=1 unused_aliases_invalid=1 hardware_translation_verified=0\n",GGD_BYTES/4096);
    return 0;
}
