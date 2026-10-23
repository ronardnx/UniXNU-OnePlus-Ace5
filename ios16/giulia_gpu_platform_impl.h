/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Explicit, serialized worker operations. Secure data has no development-pool
 * offset, GPU-user mapping or GMU mapping; it is never CPU-accessed after assign.
 * All ownership/PAS attempts pin resources until reboot, including failures.
 */
bool GiuliaGPU::platformPrepare()
{
    if(platformPrepared)return true;
    if(platformPoisoned || !manualMemoryPinned || mode!=3)return false;
    platformPoisoned=true; /* Partial preparation is not reused. */
    scmMemory=gpuAllocateHostMemory(GGS_CONTROL_BYTES,true);
    secureTables=gpuAllocateHostMemory(GGS_SEC_TABLE_BYTES,true);
    zapMetadata=gpuAllocateHostMemory(16384,true);
    IOByteCount span=0;
    scmPA=scmMemory?scmMemory->getPhysicalSegment(0,&span,kIOMemoryMapperNone):0;
    if(!scmMemory || !gpuHostRangeValid(scmMemory->getBytesNoCopy(),scmPA,span,GGS_CONTROL_BYTES))return false;
    secureRoot=secureTables?secureTables->getPhysicalSegment(0,&span,kIOMemoryMapperNone):0;
    if(!secureTables || !gpuHostRangeValid(secureTables->getBytesNoCopy(),secureRoot,span,GGS_SEC_TABLE_BYTES))return false;
    zapMetadataPA=zapMetadata?zapMetadata->getPhysicalSegment(0,&span,kIOMemoryMapperNone):0;
    if(!zapMetadata || !gpuHostRangeValid(zapMetadata->getBytesNoCopy(),zapMetadataPA,span,16384))return false;
    bzero(scmMemory->getBytesNoCopy(),GGS_CONTROL_BYTES);bzero(zapMetadata->getBytesNoCopy(),16384);
    for(unsigned i=0;i<GGS_SEC_SEGMENTS;i++) {
        secureData[i]=gpuAllocateHostMemory(GGS_SEC_GRANULE,true);
        securePA[i]=secureData[i]?secureData[i]->getPhysicalSegment(0,&span,kIOMemoryMapperNone):0;
        void *cpu=secureData[i]?secureData[i]->getBytesNoCopy():NULL;
        if(!gpuHostRangeValid(cpu,securePA[i],span,GGS_SEC_GRANULE)) {
            IOLog("GIULIA GPU SECURE: allocation_failed segment=%u bytes=%#x ownership_attempted=0\n",i,GGS_SEC_GRANULE);return false;
        }
        bzero(cpu,GGS_SEC_GRANULE);
    }
    ArmSmmu4KRange *ranges=(ArmSmmu4KRange *)IOMalloc(sizeof(*ranges)*GGS_SEC_SEGMENTS);
    if(!ranges)return false;
    for(unsigned i=0;i<GGS_SEC_SEGMENTS;i++)
        ranges[i]={GGS_SEC_BASE+(uint64_t)i*GGS_SEC_GRANULE,securePA[i],GGS_SEC_GRANULE,11,0,2};
    ArmSmmu4KConfig config={32,36,secureRoot,GGS_SEC_CHUNKS*GGS_SEC_CHUNK/4096};ArmSmmu4KResult built;
    bzero(secureTables->getBytesNoCopy(),GGS_SEC_TABLE_BYTES);
    int build=ArmSmmu4KBuild(&config,ranges,GGS_SEC_SEGMENTS,(uint64_t *)secureTables->getBytesNoCopy(),GGS_SEC_TABLE_BYTES/4096,&built);
    IOFree(ranges,sizeof(*ranges)*GGS_SEC_SEGMENTS);
    if(build!=ArmSmmu4KSuccess)return false;
    const uint64_t bases[4]={0x3d00000,0x3d6c000,0x3d8c000,0x3da0000};
    const size_t sizes[4]={0x40000,0x18000,0x4000,0x40000};
    for(unsigned i=0;i<4;i++) {
        platformMemory[i]=IOMemoryDescriptor::withPhysicalAddress(bases[i],sizes[i],kIODirectionInOut);
        platformMaps[i]=platformMemory[i]?platformMemory[i]->map(kIOMapInhibitCache):NULL;
        if(!platformMaps[i])return false;
    }
    gpuHfiBarrier(NULL);platformPrepared=true;platformPoisoned=false;
    IOLog("GIULIA GPU PLATFORM: abi=3 secure_root=%#llx slots=%u slot_bytes=%#x secure_base=%#x bank=3 sid=2 cpu_access_after_assign=0 gmu_alias=0 user_gpu_alias=0 watch_ram_only=1\n",
        (unsigned long long)secureRoot,GGS_SEC_CHUNKS,GGS_SEC_CHUNK,GGS_SEC_BASE);
    return true;
}
uint32_t GiuliaGPU::platformRead(uint64_t address)
{
    const uint64_t bases[4]={0x3d00000,0x3d6c000,0x3d8c000,0x3da0000};
    const size_t sizes[4]={0x40000,0x18000,0x4000,0x40000};
    for(unsigned i=0;i<4;i++)if(platformMaps[i] && !(address&3) && address>=bases[i] && address-bases[i]<=sizes[i]-4) {
        gpuHfiBarrier(NULL);return *(volatile uint32_t *)(platformMaps[i]->getVirtualAddress()+address-bases[i]);
    }
    return 0xffffffffu;
}
void GiuliaGPU::platformWrite(uint64_t address,uint32_t value)
{
    const uint64_t bases[4]={0x3d00000,0x3d6c000,0x3d8c000,0x3da0000};
    const size_t sizes[4]={0x40000,0x18000,0x4000,0x40000};
    for(unsigned i=0;i<4;i++)if(platformMaps[i] && !(address&3) && address>=bases[i] && address-bases[i]<=sizes[i]-4) {
        *(volatile uint32_t *)(platformMaps[i]->getVirtualAddress()+address-bases[i])=value;gpuHfiBarrier(NULL);return;
    }
}
bool GiuliaGPU::platformPower()
{
    struct ggp_report r;struct ggp_io io={this,readRegister};ggp_observe(&io,&r);
    return r.stage==GGP_OBSERVED && r.id0==0x4c017e18;
}
bool GiuliaGPU::platformIdle()
{
    if(!platformPrepared || !platformPower() || platformRead(0x3da0048) || platformRead(0x3d7e000)!=1)return false;
    for(unsigned i=0;i<22;i++)if(platformRead(0x3dc0000+4096*i)&1)return false;
    for(unsigned i=0;i<24;i++)if(platformRead(0x3da0800+4*i)!=0 || platformRead(0x3da0c00+4*i)!=0x20000)return false;
    return true;
}
bool GiuliaGPU::platformPoll(uint64_t address,uint32_t mask,uint32_t expected)
{
    for(unsigned i=0;i<100;i++){if((platformRead(address)&mask)==expected)return true;IOSleep(1);}
    return false;
}
int GiuliaGPU::platformScmCall(void *cookie,struct ggs_regs *r)
{
    GiuliaGPU *s=(GiuliaGPU *)cookie;
    IOLog("GIULIA GPU SCM: begin epoch=%u function=%#llx arginfo=%#llx\n",s->platformEpoch,(unsigned long long)r->x[0],(unsigned long long)r->x[1]);
    gpuHfiBarrier(NULL);
    int result=ggs_aarch64_call(NULL,r);
    IOLog("GIULIA GPU SCM: return a0=%#llx a1=%#llx a2=%#llx a3=%#llx a6=%#llx\n",
        (unsigned long long)r->x[0],(unsigned long long)r->x[1],(unsigned long long)r->x[2],(unsigned long long)r->x[3],(unsigned long long)r->x[6]);
    return result;
}
int GiuliaGPU::platformScmDelay(void *,unsigned ms){IOSleep(ms);return 0;}
int GiuliaGPU::platformScmPrerequisites(void *cookie,unsigned)
{return ((GiuliaGPU *)cookie)->platformIdle()?1:0;}
int GiuliaGPU::platformCall(uint32_t fn,uint32_t info,const uint64_t *args,bool result_in_a1)
{
    if(!platformPrepared || platformPoisoned || scmState.quarantined)return 7;
    struct ggs_regs frame;
    if(!ggs_build_frame(fn,info,args,scmPA,(uint64_t *)scmMemory->getBytesNoCopy(),&frame))return 4;
    const struct ggs_ops ops={this,platformScmCall,platformScmDelay,platformScmPrerequisites};
    gpuHfiBarrier(NULL);int code=ggs_exchange_frame(&ops,&scmState,&frame);
    if(code || (result_in_a1 && scmState.last_output.x[1]))return code?code:7;
    return 0;
}
int GiuliaGPU::platformAvailable(uint32_t id)
{
    uint64_t args[10]={id};int code=platformCall(0x42000601,1,args,false);
    return code?code:scmState.last_output.x[1]?0:7;
}
int GiuliaGPU::platformAssign(uint64_t pa,uint64_t bytes,bool table)
{
    uint64_t args[10];
    if(!ggs_assign_descriptor(scmMemory->getBytesNoCopy(),scmPA,pa,bytes,table,args))return 4;
    platformOwned=true;
    int code=platformCall(0x42000c16,0x1117,args,true);
    if(code){platformPoisoned=true;IOLog("GIULIA GPU SECURE: ownership_unknown=1 retained_until_reboot=1\n");}
    return code;
}
int GiuliaGPU::platformAssignBatch(unsigned first,unsigned count)
{
    uint64_t args[10];
    if(first>=GGS_SEC_SEGMENTS || count>GGS_SEC_SEGMENTS-first ||
       !ggs_assign_batch(scmMemory->getBytesNoCopy(),scmPA,securePA+first,count,args))return 4;
    platformOwned=true;
    int code=platformCall(0x42000c16,0x1117,args,true);
    if(code){platformPoisoned=true;IOLog("GIULIA GPU SECURE: ownership_unknown=1 first_segment=%u retained_until_reboot=1\n",first);}
    return code;
}
int GiuliaGPU::platformCommand(unsigned action)
{
    if(action==0) {
        IOLog("GIULIA GPU PLATFORM: abi=3 prepared=%u secure_owned=%u secure_ready=%u active=%u poisoned=%u scm_quarantined=%u cx_call=%u zap=%u secure_root=%#llx slots=%u used=",
            platformPrepared,platformOwned,secureOwnershipReady,secureContextActive,platformPoisoned,scmState.quarantined,scmState.init_succeeded,zapAuthenticated,(unsigned long long)secureRoot,GGS_SEC_CHUNKS);
        unsigned used=0;for(unsigned i=0;i<GGS_SEC_CHUNKS;i++)used+=secureSlots[i].used!=0;IOLog("%u\n",used);return 0;
    }
    if(action==6 && !secureContextActive)return 0;
    if(!platformPrepare() || !platformPower())return 8;
    if(action==9)return configureDevMappings();
    if(action==6) {
        if(!secureContextActive)return 0;
        if(platformRead(0x3d7e000)!=1)return 8;
        platformWrite(0x3da0c08,0x20000);platformWrite(0x3dc3000,0);
        platformWrite(0x3dc3618,0);platformWrite(0x3dc37f0,0xffffffff);
        if(!platformPoll(0x3dc37f4,1,0)){platformPoisoned=true;return 8;}
        platformWrite(0x3da0808,0);platformWrite(0x3dc3020,0);platformWrite(0x3dc3024,0);
        if(platformRead(0x3da0808) || platformRead(0x3da0c08)!=0x20000 || platformRead(0x3dc3000))return 8;
        secureContextActive=false;IOLog("GIULIA GPU SECURE: stream_removed=1 context_disabled=1 tlb_synced=1 ownership_retained=1\n");return 0;
    }
    if(platformPoisoned || scmState.quarantined)return 7;
    if(action==1 || action==2) {
        const struct ggs_ops ops={this,platformScmCall,platformScmDelay,platformScmPrerequisites};
        if(!platformIdle())return 8;
        int code=ggs_run(&ops,&scmState,action==1?GGS_PROBE:GGS_INIT);
        if(code==GGS_ALREADY_INITIALIZED)code=0;
        if(!code && action==1)code=platformAvailable(0x02000c16);
        if(!code && action==1)code=platformAvailable(0x02000c1b);
        IOLog("GIULIA GPU PLATFORM: action=%u code=%d secure_cx_succeeded=%u\n",action,code,scmState.init_succeeded);return code;
    }
    if(action==3) {
        if(!scmState.init_succeeded || !(platformRead(0x3dc0000)&1))return 8;
        if(platformRead(0x3dc0020)!=(uint32_t)gpuRoot)return 8;
        uint64_t args[10]={0xffff0000u,0xffffffffu,0xffffffffu,0xffffffffu};
        int code=platformAvailable(0x02000c1b);return code?code:platformCall(0x42000c1b,4,args,false);
    }
    if(action==4) {
        if(!scmState.init_succeeded || !platformIdle())return 8;
        if(secureOwnershipReady)return 0;
        int code=platformAvailable(0x02000c16);if(code)return code;
        code=platformAssign(secureRoot,GGS_SEC_TABLE_BYTES,true);if(code)return code;
        for(unsigned i=0;i<GGS_SEC_SEGMENTS;i+=32) {
            code=platformAssignBatch(i,32);if(code)return code;
        }
        secureOwnershipReady=true;IOLog("GIULIA GPU SECURE: pool_owned=1 bytes=%#x tables_shared_hlos_rw_cp_r=1 cpu_data_access=0\n",GGS_SEC_CHUNKS*GGS_SEC_CHUNK);return 0;
    }
    if(action==5) {
        if(!secureOwnershipReady || secureContextActive || platformRead(0x3d7e000)!=1 ||
           platformRead(0x3dc3000)&1 || platformRead(0x3da0808) || platformRead(0x3da0c08)!=0x20000)return 8;
        if(!platformRead(0x3da6008) && !platformRead(0x3da600c))return 8;
        platformWrite(0x3da180c,1);platformWrite(0x3da100c,0x1f300);
        platformWrite(0x3dc3010,0x38011);platformWrite(0x3dc3030,0x80802020);
        platformWrite(0x3dc3020,(uint32_t)secureRoot);platformWrite(0x3dc3024,(uint32_t)(secureRoot>>32)|(0x753u<<16));
        platformWrite(0x3dc3028,0);platformWrite(0x3dc302c,0x753u<<16);
        platformWrite(0x3dc3038,0xff44);platformWrite(0x3dc303c,0);platformWrite(0x3dc3004,0x32b);
        platformWrite(0x3dc3058,0xffffffff);platformWrite(0x3dc3618,0);platformWrite(0x3dc37f0,0xffffffff);
        if(!platformPoll(0x3dc37f4,1,0))return 8;
        secureContextActive=true; /* Also clean up failed activation readback. */
        platformWrite(0x3dc3000,0x1027);
        if(platformRead(0x3dc3000)!=0x27 || platformRead(0x3dc3010)!=0x38071 || platformRead(0x3dc3020)!=(uint32_t)secureRoot)return 8;
        secureContextActive=true; /* Keep cleanup possible after partial routing. */
        platformWrite(0x3da0808,0x80000002);platformWrite(0x3da0c08,3);
        if(platformRead(0x3da0808)!=0x80000002 || platformRead(0x3da0c08)!=3)return 8;
        IOLog("GIULIA GPU SECURE: mapped=1 bank=3 sid=2 root=%#llx base=%#x bytes=%#x\n",(unsigned long long)secureRoot,GGS_SEC_BASE,GGS_SEC_CHUNKS*GGS_SEC_CHUNK);return 0;
    }
    if(action==7)return platformZap();
    if(action==8) {
        if(!platformIdle() || platformOwned)return 8;
        ++platformEpoch;bzero(&scmState,sizeof(scmState));return 0;
    }
    return 4;
}
int GiuliaGPU::secureAllocate(uint32_t bytes,uint32_t alignment,uint32_t flags,uint32_t kind)
{
    if(!ggs_secure_request(bytes,alignment,flags,kind))return 4;
    if(platformPoisoned || !secureOwnershipReady || !secureContextActive || platformRead(0x3da0c08)!=3)return 8;
    for(unsigned i=0;i<GGS_SEC_CHUNKS;i++)if(!secureSlots[i].used) {
        secureSlots[i]={bytes,flags,kind,1};
        IOLog("GIULIA GPU SECURE: allocation=%u bytes=%#x gpu=%#x gmu=0 flags=%#x kind=%u owned=1 mapped=1 cpu_access=0\n",
            i,bytes,GGS_SEC_BASE+i*GGS_SEC_CHUNK,flags,kind);return 0;
    }
    return 3;
}
int GiuliaGPU::platformSnapshot(unsigned reason)
{
    unsigned sequence=++platformSnapshotSequence;
    /* Console breadcrumbs reach USB asynchronously. A missing line alone is
     * never proof that the following read was not executed. Host checkpoints
     * and an explicit OP14 completion provide the independent boundary. */
    IOLog("GIULIA GPU SNAPSHOT: sequence=%u reason=%u stage=preflight_begin gx_reads=0\n",sequence,reason);
    if(!platformPrepared || !platformPower())return 8;
    auto read32=[&](uint64_t address)->uint32_t {
        IOLog("GIULIA GPU SNAPSHOT: sequence=%u stage=read_begin address=%#llx width=32\n",sequence,(unsigned long long)address);
        uint32_t value=platformRead(address);
        IOLog("GIULIA GPU SNAPSHOT: sequence=%u stage=read_done address=%#llx width=32 value=%#x\n",sequence,(unsigned long long)address,value);
        return value;
    };
    const uint64_t ao[]={0x3d7e340,0x3d8ec40,0x3d7e648,0x3d8ec14,0x3d8ec4c,0x3d8ec54,0x3d8ec30,
        0x3da0048,0x3dc0058,0x3dc2058,0x3dc3058};
    uint32_t values[sizeof(ao)/sizeof(ao[0])];
    for(unsigned i=0;i<sizeof(ao)/sizeof(ao[0]);i++)values[i]=read32(ao[i]);
    const unsigned banks[]={0,2,3};
    for(unsigned i=0;i<3;i++) {
        uint32_t fsr=values[8+i];
        if(fsr==0xffffffffu)return 8;
        if(!(fsr&0xc00001feu)) {
            IOLog("GIULIA GPU SNAPSHOT: sequence=%u bank=%u fault_valid=0 syndrome_reads=0\n",sequence,banks[i]);
            continue;
        }
        /* FAR is one aligned 64-bit register, not two independent 32-bit
         * transactions. platformPrepare owns the complete SMMU map (index 3).
         * Never read stale syndrome registers when FSR has no valid fault. */
        uint64_t base=0x3dc0000+(uint64_t)banks[i]*4096,address=base+0x60;
        if(!platformMaps[3])return 8;
        IOLog("GIULIA GPU SNAPSHOT: sequence=%u stage=read_begin address=%#llx width=64\n",sequence,(unsigned long long)address);
        gpuHfiBarrier(NULL);
        uint64_t far=*(volatile uint64_t *)(platformMaps[3]->getVirtualAddress()+address-0x3da0000);
        gpuHfiBarrier(NULL);
        IOLog("GIULIA GPU SNAPSHOT: sequence=%u stage=read_done address=%#llx width=64 value=%#llx\n",sequence,(unsigned long long)address,(unsigned long long)far);
        read32(base+0x68);read32(base+0x6c);read32(0x3da1400+4*banks[i]);
    }
    IOLog("GIULIA GPU SNAPSHOT: sequence=%u stage=complete gx_reads=0 irq_handler=0\n",sequence);
    return 0;
}
int GiuliaGPU::platformWatch(unsigned count,unsigned milliseconds)
{
    IOLog("GIULIA GPU WATCH: stage=preflight_begin header=%#x count=%u timeout_ms=%u\n",devPacket[0],count,milliseconds);
    if(!platformPrepared || platformPoisoned || !devMappingsManaged || !secureContextActive || !scmState.init_succeeded ||
       !milliseconds || milliseconds>1000 || !count || count>255 || ((devPacket[0]>>8)&255)!=count)return 4;
    if((devPacket[0]&255)!=131 || !platformPower())return 8;
    IOLog("GIULIA GPU WATCH: stage=gx_power_read_begin\n");
    uint32_t power=platformRead(0x3d7e340);
    IOLog("GIULIA GPU WATCH: stage=gx_power_read_done value=%#x\n",power);
    if(power&3)return 8;
    IOLog("GIULIA GPU WATCH: stage=fence_read_begin\n");
    uint32_t fence=platformRead(0x3d8ec40);
    IOLog("GIULIA GPU WATCH: stage=fence_read_done value=%#x\n",fence);
    if(fence)return 8;
    for(unsigned i=0;i<count;i++)if(!devWritten[i])return 5;
    struct ggh_io io={(volatile uint32_t *)allocations[hfiIndex]->getBytesNoCopy(),GGH_BYTES,GGH_IOVA,NULL,gpuHfiBarrier};
    volatile uint32_t *commandHeader,*commandRing,*qh,*ring;unsigned cr=0,cw=0,r=0,w=0;
    if(ggh_queue(&io,0,&commandHeader,&commandRing,&cr,&cw) || cr!=cw ||
       ggh_queue(&io,1,&qh,&ring,&r,&w) || r!=w)return 8;
    IOLog("GIULIA GPU WATCH: stage=queue_before header=%#x cmd_read=%u cmd_write=%u msg_read=%u msg_write=%u\n",devPacket[0],cr,cw,r,w);
    /* Publication itself can let firmware consume the command before a
     * doorbell. Never describe this as private packet staging or retry it. */
    int code=ggh_send(&io,devPacket,count);
    if(code){IOLog("GIULIA GPU WATCH: stage=queue_not_committed code=%d\n",code);return code;}
    bzero(devWritten,sizeof(devWritten));
    gpuHfiBarrier(NULL);
    IOLog("GIULIA GPU WATCH: stage=queue_committed header=%#x cmd_read=%u cmd_write=%u ack_verified=0\n",devPacket[0],commandHeader[10],commandHeader[11]);
    IOLog("GIULIA GPU WATCH: stage=doorbell_before header=%#x\n",devPacket[0]);
    platformWrite(0x3d7e650,1);
    IOLog("GIULIA GPU WATCH: stage=doorbell_after header=%#x write_returned=1 ack_verified=0\n",devPacket[0]);
    /* From this point onwards inspect only owned HFI RAM. No automatic
     * MMIO snapshots, even on timeout/failure; the host must request OP14
     * separately when its power and transport prerequisites remain known. */
    unsigned reason=0,nr=r,nw=w,firstHeader=0;
    for(unsigned i=0;i<milliseconds;i++) {
        gpuHfiBarrier(NULL);
        if(ggh_queue(&io,1,&qh,&ring,&nr,&nw)){reason=4;break;}
        if(nr!=nw){reason=1;firstHeader=ring[nr];break;}
        IOSleep(1);
    }
    gpuHfiBarrier(NULL);
    IOLog("GIULIA GPU WATCH: stage=wait_end reason=%u header=%#x cmd_read=%u cmd_write=%u msg_read=%u msg_write=%u first_header=%#x\n",reason,devPacket[0],commandHeader[10],commandHeader[11],nr,nw,firstHeader);
    if(reason!=1) {
        platformPoisoned=true;
        IOLog("GIULIA GPU WATCH: outcome=%s quarantined=1 ack_verified=0 automatic_mmio=0\n",reason==4?"queue_invalid":"timeout");
        return 8;
    }
    IOLog("GIULIA GPU WATCH: outcome=packet_arrived queue_consumed=0 ack_verified=0 host_validation_required=1\n");
    return 0;
}
int GiuliaGPU::platformZap()
{
    if(zapAuthenticated)return 0;
    if(!secureContextActive || !scmState.init_succeeded || (platformRead(0x3d7e340)&3) ||
       platformRead(0x3d8ec40) || (platformRead(0x3d8ec30)&(1u<<23)))return 8;
    struct gg_pack pack;struct ggs_zap_plan plan;
    if(!gg_pack_open((const void *)firmwareMap->getVirtualAddress(),firmwareMemory->getLength(),&pack) ||
       !ggs_zap_plan(pack.asset[GG_ASSET_ZAP].data,pack.asset[GG_ASSET_ZAP].size,&plan))return 4;
    if(platformAvailable(0x02000201) || platformAvailable(0x02000202) || platformAvailable(0x02000205))return 7;
    uint8_t *metadata=(uint8_t *)zapMetadata->getBytesNoCopy();
    memcpy(metadata,pack.asset[GG_ASSET_ZAP].data,plan.metadata_header);
    memcpy(metadata+plan.metadata_header,pack.asset[GG_ASSET_ZAP].data+plan.hash_offset,plan.hash_bytes);
    gpuHfiBarrier(NULL);platformOwned=true;
    uint64_t args[10]={13,zapMetadataPA};
    int code=platformCall(0x42000201,0x82,args,true);if(code){platformPoisoned=true;return code;}
    args[1]=0x9b09a000;args[2]=(plan.memory_bytes+4095u)&~4095u;
    code=platformCall(0x42000202,3,args,true);if(code){platformPoisoned=true;return code;}
    zapMemory=IOMemoryDescriptor::withPhysicalAddress(0x9b09a000,8192,kIODirectionInOut);
    zapMap=zapMemory?zapMemory->map(kIOMapWriteCombineCache):NULL;
    if(!zapMap){platformPoisoned=true;return 8;}
    /* Exact DT carveout; never mapped as ordinary host allocation or read back
     * after authentication. The embedded pack is independently SHA-verified. */
    uint8_t *dst=(uint8_t *)zapMap->getVirtualAddress();
    memcpy(dst,pack.asset[GG_ASSET_ZAP].data+plan.load_offset,plan.load_bytes);
    if(plan.memory_bytes>plan.load_bytes)bzero(dst+plan.load_bytes,plan.memory_bytes-plan.load_bytes);
    gpuHfiBarrier(NULL);args[1]=args[2]=0;
    code=platformCall(0x42000205,1,args,true);if(code){platformPoisoned=true;return code;}
    zapAuthenticated=true;IOLog("GIULIA GPU ZAP: pas=13 authenticated=1 carveout=0x9b09a000 bytes=%u\n",plan.memory_bytes);return 0;
}
