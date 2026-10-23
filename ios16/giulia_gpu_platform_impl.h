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
    ArmSmmu4KRange ranges[GGS_SEC_CHUNKS];
    for(unsigned i=0;i<GGS_SEC_CHUNKS;i++) {
        secureData[i]=gpuAllocateHostMemory(GGS_SEC_CHUNK,true);
        securePA[i]=secureData[i]?secureData[i]->getPhysicalSegment(0,&span,kIOMemoryMapperNone):0;
        void *cpu=secureData[i]?secureData[i]->getBytesNoCopy():NULL;
        if(!gpuHostRangeValid(cpu,securePA[i],span,GGS_SEC_CHUNK))return false;
        bzero(cpu,GGS_SEC_CHUNK);
        ranges[i]={GGS_SEC_BASE+(uint64_t)i*GGS_SEC_CHUNK,securePA[i],GGS_SEC_CHUNK,15,0,2};
    }
    ArmSmmu4KConfig config={32,36,secureRoot,GGS_SEC_CHUNKS*GGS_SEC_CHUNK/4096};ArmSmmu4KResult built;
    bzero(secureTables->getBytesNoCopy(),GGS_SEC_TABLE_BYTES);
    if(ArmSmmu4KBuild(&config,ranges,GGS_SEC_CHUNKS,(uint64_t *)secureTables->getBytesNoCopy(),GGS_SEC_TABLE_BYTES/4096,&built)!=ArmSmmu4KSuccess)return false;
    const uint64_t bases[4]={0x3d00000,0x3d6c000,0x3d8c000,0x3da0000};
    const size_t sizes[4]={0x40000,0x18000,0x4000,0x40000};
    for(unsigned i=0;i<4;i++) {
        platformMemory[i]=IOMemoryDescriptor::withPhysicalAddress(bases[i],sizes[i],kIODirectionInOut);
        platformMaps[i]=platformMemory[i]?platformMemory[i]->map(kIOMapInhibitCache):NULL;
        if(!platformMaps[i])return false;
    }
    gpuHfiBarrier(NULL);platformPrepared=true;platformPoisoned=false;
    IOLog("GIULIA GPU PLATFORM: abi=2 secure_root=%#llx slots=%u slot_bytes=%#x secure_base=%#x bank=3 sid=2 cpu_access_after_assign=0 gmu_alias=0 user_gpu_alias=0\n",
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
int GiuliaGPU::platformCommand(unsigned action)
{
    if(action==0) {
        IOLog("GIULIA GPU PLATFORM: abi=2 prepared=%u secure_owned=%u secure_ready=%u active=%u poisoned=%u scm_quarantined=%u cx_call=%u zap=%u secure_root=%#llx slots=%u used=",
            platformPrepared,platformOwned,secureOwnershipReady,secureContextActive,platformPoisoned,scmState.quarantined,scmState.init_succeeded,zapAuthenticated,(unsigned long long)secureRoot,GGS_SEC_CHUNKS);
        unsigned used=0;for(unsigned i=0;i<GGS_SEC_CHUNKS;i++)used+=secureSlots[i].used!=0;IOLog("%u\n",used);return 0;
    }
    if(!platformPrepare() || !platformPower())return 8;
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
        for(unsigned i=0;i<GGS_SEC_CHUNKS;i++)for(unsigned at=0;at<GGS_SEC_CHUNK;at+=0x200000) {
            code=platformAssign(securePA[i]+at,0x200000,false);if(code)return code;
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
    if(!platformPrepared || !platformPower())return 8;
    unsigned sequence=++platformSnapshotSequence;
    const uint64_t ao[]={0x3d7e340,0x3d8ec40,0x3d7e648,0x3d8ec14,0x3d8ec4c,0x3d8ec54,0x3d8ec30,
        0x3da0048,0x3dc0058,0x3dc0060,0x3dc0064,0x3dc0068,0x3dc2058,0x3dc2060,0x3dc2064,0x3dc2068,
        0x3dc3058,0x3dc3060,0x3dc3064,0x3dc3068,0x3da1400,0x3da1408,0x3da140c};
    uint32_t values[sizeof(ao)/sizeof(ao[0])];
    for(unsigned i=0;i<sizeof(ao)/sizeof(ao[0]);i++)values[i]=platformRead(ao[i]);
    IOLog("GIULIA GPU SNAPSHOT: sequence=%u reason=%u polling=1 irq_handler=0\n",sequence,reason);
    for(unsigned i=0;i<sizeof(ao)/sizeof(ao[0]);i++)IOLog("GIULIA GPU SNAPSHOT: address=%#llx value=%#x\n",(unsigned long long)ao[i],values[i]);
    /* Read no GX register unless the caller holds acknowledged OOB. Its ACK
     * was cleared by host, so confirm power/fence at each read; no INT0_STATUS. */
    if(reason && (values[0]&3)==0 && values[1]==0) {
        const uint64_t gx[]={0x3d000e0,0x3d02084,0x3d0208c,0x3d02090,0x3d3d000};
        for(unsigned i=0;i<5;i++) {
            if((platformRead(0x3d7e340)&3) || platformRead(0x3d8ec40))break;
            IOLog("GIULIA GPU SNAPSHOT: address=%#llx value=%#x\n",(unsigned long long)gx[i],platformRead(gx[i]));
        }
    }
    return 0;
}
int GiuliaGPU::platformWatch(unsigned count,unsigned milliseconds)
{
    if(!platformPrepared || platformPoisoned || !secureContextActive || !scmState.init_succeeded ||
       !milliseconds || milliseconds>1000 || !count || count>255 || ((devPacket[0]>>8)&255)!=count)return 4;
    if((devPacket[0]&255)!=131 || !platformPower() || (platformRead(0x3d7e340)&3) || platformRead(0x3d8ec40))return 8;
    for(unsigned i=0;i<count;i++)if(!devWritten[i])return 5;
    struct ggh_io io={(volatile uint32_t *)allocations[hfiIndex]->getBytesNoCopy(),GGH_BYTES,GGH_IOVA,NULL,gpuHfiBarrier};
    volatile uint32_t *qh,*ring;unsigned r=0,w=0;
    if(ggh_queue(&io,1,&qh,&ring,&r,&w) || r!=w)return 8;
    if(platformSnapshot(1))return 8;
    int code=ggh_send(&io,devPacket,count);if(code)return code;
    bzero(devWritten,sizeof(devWritten));
    IOLog("GIULIA GPU WATCH: dispatch header=%#x timeout_ms=%u\n",devPacket[0],milliseconds);
    platformWrite(0x3d7e650,1);
    unsigned reason=0;
    for(unsigned i=0;i<milliseconds;i++) {
        gpuHfiBarrier(NULL);unsigned nr=0,nw=0;
        if(ggh_queue(&io,1,&qh,&ring,&nr,&nw)){reason=4;break;}
        if(nw!=w){reason=1;break;}
        if((platformRead(0x3d7e648)&0x8000) || (platformRead(0x3d8ec14)&0x29) ||
           platformRead(0x3da0048) || (platformRead(0x3dc0058)&0xc00001fe) ||
           (platformRead(0x3dc2058)&0xc00001fe) || (platformRead(0x3dc3058)&0xc00001fe)){reason=2;break;}
        IOSleep(1);
    }
    platformSnapshot(reason+10);
    IOLog("GIULIA GPU WATCH: end reason=%u header=%#x\n",reason,devPacket[0]);
    if(reason!=1){platformPoisoned=true;return 8;}return 0;
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
