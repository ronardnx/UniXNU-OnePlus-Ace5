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

static bool gpuMemstorePhysical(IOBufferMemoryDescriptor *memory,uint64_t *physical)
{
    if(!memory || !physical || memory->getLength()!=GGMS_BYTES)return false;
    IOByteCount span=0;*physical=memory->getPhysicalSegment(0,&span,kIOMemoryMapperNone);
    return gpuHostRangeValid(memory->getBytesNoCopy(),*physical,span,GGMS_BYTES);
}

bool GiuliaGPU::prepareInitialMappings()
{
    struct ggm_tables gpu,gmu;struct ggm_owned owned;uint64_t physical=0;
    if(!gpuMappingTable(gpuTableMemory,gpuRoot,&gpu) || !gpuMappingTable(tableMemory,gmuRoot,&gmu) ||
       !gpuMappingOwned(devMemory,&owned) || !gpuMemstorePhysical(memstoreMemory,&physical) ||
       !ggm_reset_pool(&gpu,&gmu,&owned,0) ||
       !ggms_mapping(&gpu,&gmu,&owned,physical,0,GGM_CHECK) ||
       !ggipc_mapping(&gmu,0,GGM_CHECK))return false;
    /* Neither root has been published or activated. Leave only topology and
     * owned PA tombstones for all ordinary and dedicated allocation leaves. */
    if(!ggm_reset_pool(&gpu,&gmu,&owned,1) ||
       !ggms_mapping(&gpu,&gmu,&owned,physical,0,GGM_CLEAR) ||
       !ggipc_mapping(&gmu,0,GGM_CLEAR) || !ggf_mapping_prepare(&gmu))return false;
    return giulia_gpu_cache_publish(gpu.words,gpu.bytes) && giulia_gpu_cache_publish(gmu.words,gmu.bytes);
}

int GiuliaGPU::auditMemstoreMappings()
{
    struct ggm_tables gpu,gmu;struct ggm_owned owned;uint64_t physical=0;
    if(platformPoisoned || (!memstoreMapped && memstoreFlags) ||
       (memstoreMapped && !ggms_request(GGMS_BYTES,0,memstoreFlags,22)) ||
       !gpuMappingTable(gpuTableMemory,gpuRoot,&gpu) || !gpuMappingTable(tableMemory,gmuRoot,&gmu) ||
       !gpuMappingOwned(devMemory,&owned) || !gpuMemstorePhysical(memstoreMemory,&physical) ||
       !ggms_mapping(&gpu,&gmu,&owned,physical,memstoreMapped?memstoreFlags:0,GGM_VERIFY)) {
        platformPoisoned=true;return 8;
    }
    return 0;
}

int GiuliaGPU::memstoreAllocate(uint32_t bytes,uint32_t alignment,uint32_t flags,uint32_t kind)
{
    if(!ggms_request(bytes,alignment,flags,kind) || (GGMS_IOVA&((1u<<(alignment&255))-1)))return 4;
    if(!hostPrepared || !manualMemoryPinned || !devMappingsManaged || platformPoisoned)return 8;
    if(memstoreMapped && memstoreFlags!=flags)return 4;
    if(auditMemstoreMappings() || syncDevMappings())return 8;
    bool reused=memstoreMapped;
    if(!reused) {
        struct ggm_tables gpu,gmu;struct ggm_owned owned;uint64_t physical=0;
        if(!gpuMappingTable(gpuTableMemory,gpuRoot,&gpu) || !gpuMappingTable(tableMemory,gmuRoot,&gmu) ||
           !gpuMappingOwned(devMemory,&owned) || !gpuMemstorePhysical(memstoreMemory,&physical) ||
           !ggms_mapping(&gpu,&gmu,&owned,physical,flags,GGM_CHECK))return 8;
        /* A first import changes only invalid leaves. Reserve the singleton
         * before publication; an ambiguous failure cannot return it to reuse. */
        memstoreMapped=true;memstoreFlags=flags;
        if(!ggms_mapping(&gpu,&gmu,&owned,physical,flags,GGM_APPLY) ||
           syncDevMappings() || auditMemstoreMappings()) {platformPoisoned=true;return 8;}
    }
    IOLog("GIULIA GPU MEMSTORE: bytes=%#x gpu=%#x gmu=%#x flags=%#x kind=22 reused=%u cpu_offset=none mapped=1\n",
        GGMS_BYTES,GGMS_IOVA,(flags&8)?GGMS_IOVA:0,flags,reused);
    return 0;
}

int GiuliaGPU::memstoreRead(uint32_t context)
{
    uint32_t offset;
    if(!ggms_offset(context,GGMS_SOP,&offset))return 4;
    uint64_t physical;struct ggms_observation value;
    if(!hostPrepared || !manualMemoryPinned || !memstoreMapped ||
       !gpuMemstorePhysical(memstoreMemory,&physical) ||
       !ggms_snapshot(memstoreMemory->getBytesNoCopy(),GGMS_BYTES,context,&value,gpuHfiBarrier,NULL))return 8;
    IOLog("GIULIA GPU MEMSTORE: context=%u sop=%#x eop=%#x stable=%u completion_verified=0\n",
        context,value.sop,value.eop,value.stable);
    return 0;
}

int GiuliaGPU::auditIpcMappings()
{
    struct ggm_tables gmu;
    if(platformPoisoned || (!ipcMapped && ipcFlags) ||
       (ipcMapped && !ggi_owns(&gpuIpcOwner,GGI_GPU,this)) ||
       !gpuMappingTable(tableMemory,gmuRoot,&gmu) ||
       !ggipc_mapping(&gmu,ipcMapped?ipcFlags:0,GGM_VERIFY)) {
        platformPoisoned=true;return 8;
    }
    return 0;
}

int GiuliaGPU::ipcAllocate(uint32_t bytes,uint32_t alignment,uint32_t flags,uint32_t kind)
{
    if(!ggipc_request(bytes,alignment,flags,kind))return 4;
    if(!hostPrepared || !manualMemoryPinned || !devMappingsManaged || platformPoisoned)return 8;
    if(ipcMapped && ipcFlags!=flags)return 4;
    if(auditIpcMappings() || syncDevMappings())return 8;
    bool reused=ipcMapped;
    if(!reused) {
        struct ggm_tables gmu;
        if(!gpuMappingTable(tableMemory,gmuRoot,&gmu) || !ggipc_mapping(&gmu,flags,GGM_CHECK))return 8;
        /* Firmware handoff must not retain an uncertain APSS channel. Host
         * mappings/providers hold broker membership for their actual lifetime;
         * claim and exclusion are atomic, not an availability-then-use test. */
        if(!giulia_gpu_qmp_available() || !ggi_claim(&gpuIpcOwner,GGI_GPU,this))return 3;
        ipcMapped=true;ipcFlags=flags; /* Any ambiguous publication retains the lease. */
        if(!ggipc_mapping(&gmu,flags,GGM_APPLY) || syncDevMappings() || auditIpcMappings()) {
            platformPoisoned=true;return 8;
        }
    }
    IOLog("GIULIA GPU IPC: bytes=%#x gpu=0 gmu=%#x flags=%#x kind=19 reused=%u cpu_offset=none mapped=1 host_excluded=1\n",
        GGIPC_BYTES,GGIPC_IOVA,flags,reused);
    return 0;
}

int GiuliaGPU::syncDevMappings()
{
    if(fenceMapped && auditFenceMappings())return 8;
    if(!platformPrepared || platformPoisoned || !manualMemoryPinned || !gpuTableMemory || !tableMemory || !platformPower())return 8;
    uint32_t power=platformRead(0x3d7e340);
    if(power==0xffffffffu)return 8;
    if((power&3)!=3 && ((power&3)!=0 || platformRead(0x3d8ec40) ||
       (platformRead(0x3d8ec30)&(1u<<23))))return 8;
    if(platformRead(0x3da0048))return 8;
    const uint64_t bases[2]={0x3dc0000,0x3dc2000},roots[2]={gpuRoot,gmuRoot};
    const unsigned banks[2]={0,2},slots[2]={1,0},sids[2]={0,5},asids[2]={0x752,0x750};
    bool active[2];
    for(unsigned i=0;i<2;i++) {
        uint32_t control=platformRead(bases[i]);
        active[i]=(control&1)!=0;
        if(active[i]) {
            /* Exact native 4K profile installed by gpu-native-hwsched.py.
             * A matching root alone does not establish translation format,
             * permissions, ASID or stream ownership. Never repair a foreign CB.
             */
            if(control!=0x27 || (platformRead(bases[i]+0x58)&0xc00001feu) || (platformRead(bases[i]+0x7f4)&1) ||
               platformRead(bases[i]+0x10)!=0x38071 || platformRead(bases[i]+0x30)!=0x80802020 ||
               platformRead(bases[i]+0x38)!=0xff44 || platformRead(bases[i]+0x3c)!=0 ||
               platformRead(0x3da1800+4*banks[i])!=1 || platformRead(0x3da1000+4*banks[i])!=0x1f300 ||
               platformRead(0x3da0800+4*slots[i])!=(0x80000000u|sids[i]) ||
               platformRead(0x3da0c00+4*slots[i])!=banks[i])return 8;
            uint64_t ttbr=(uint64_t)platformRead(bases[i]+0x20)|((uint64_t)platformRead(bases[i]+0x24)<<32);
            uint64_t ttbr1=(uint64_t)platformRead(bases[i]+0x28)|((uint64_t)platformRead(bases[i]+0x2c)<<32);
            if(ttbr!=(roots[i]|((uint64_t)asids[i]<<48)) || ttbr1!=((uint64_t)asids[i]<<48))return 8;
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
    if(devMappingsManaged) {
        int code=auditDevMappings();
        if(!code)IOLog("GIULIA GPU MAPPING: managed=1 allocations=%u already_configured=1\n",devPool.count);
        return code;
    }
    if(!platformPrepared || platformPoisoned || !manualMemoryPinned || !platformIdle() || !ggd_pool_valid(&devPool))return 8;
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

int GiuliaGPU::mapDevAllocation(unsigned index,bool clearOnly)
{
    if(!devMappingsManaged || platformPoisoned || !ggd_pool_valid(&devPool) || index>=devPool.count)return 8;
    struct ggm_tables gpu,gmu;struct ggm_owned owned;
    if(!gpuMappingTable(gpuTableMemory,gpuRoot,&gpu) || !gpuMappingTable(tableMemory,gmuRoot,&gmu) ||
       !gpuMappingOwned(devMemory,&owned) ||
       !ggm_allocation(&gpu,&gmu,&owned,&devPool.allocations[index],devUcode[index],GGM_CHECK))return 8;
    if(syncDevMappings())return 8;
    if(!ggm_allocation(&gpu,&gmu,&owned,&devPool.allocations[index],devUcode[index],GGM_CLEAR) ||
       syncDevMappings()) {platformPoisoned=true;return 8;}
    if(clearOnly)return 0; /* Revoke every alias before copying executable assets. */
    if(!ggm_allocation(&gpu,&gmu,&owned,&devPool.allocations[index],devUcode[index],GGM_APPLY) ||
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
    if(auditMemstoreMappings() || auditIpcMappings() || auditFenceMappings() || auditSecureMappings())return 8;
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
