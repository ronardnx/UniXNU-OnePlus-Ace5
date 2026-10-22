/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Included after GiuliaGPU definitions. This is an explicit development pool,
 * not a production task-isolated allocator. All mappings are privileged RWX,
 * Normal NC and remain pinned until reboot after gpu.hfi.pin. No MMIO here.
 */
bool GiuliaGPU::prepareDevMemory(ArmSmmu4KRange *ranges,size_t *count)
{
    ArmSmmu4KRange gpu[GGD_CHUNKS];
    if(*count+GGD_CHUNKS+1>GG_FW_MAX_BLOCKS)return false;
    for(unsigned i=0;i<GGD_CHUNKS;i++) {
        devMemory[i]=gpuAllocateHostMemory(GGD_CHUNK,true);
        IOByteCount contiguous=0;
        uint64_t pa=devMemory[i]?devMemory[i]->getPhysicalSegment(0,&contiguous,kIOMemoryMapperNone):0;
        void *cpu=devMemory[i]?devMemory[i]->getBytesNoCopy():NULL;
        if(!gpuHostRangeValid(cpu,pa,contiguous,GGD_CHUNK))return false;
        bzero(cpu,GGD_CHUNK);
        gpu[i]={GGD_BASE+(uint64_t)i*GGD_CHUNK,pa,GGD_CHUNK,15,0,2};
        ranges[(*count)++]=gpu[i];
        if(i==0) {ArmSmmu4KRange cached=gpu[i];cached.iova=GGD_CACHE;ranges[(*count)++]=cached;}
    }
    for(size_t i=1;i<*count;i++)for(size_t j=i;j>0 && ranges[j].iova<ranges[j-1].iova;j--) {
        ArmSmmu4KRange tmp=ranges[j];ranges[j]=ranges[j-1];ranges[j-1]=tmp;
    }
    gpuTableMemory=gpuAllocateHostMemory(128*4096);
    IOByteCount contiguous=0;
    gpuRoot=gpuTableMemory?gpuTableMemory->getPhysicalSegment(0,&contiguous,kIOMemoryMapperNone):0;
    void *cpu=gpuTableMemory?gpuTableMemory->getBytesNoCopy():NULL;
    if(!gpuHostRangeValid(cpu,gpuRoot,contiguous,128*4096))return false;
    bzero(cpu,128*4096);
    ArmSmmu4KConfig config={32,36,gpuRoot,65536};ArmSmmu4KResult result;
    if(ArmSmmu4KBuild(&config,gpu,GGD_CHUNKS,(uint64_t *)cpu,128,&result)!=ArmSmmu4KSuccess)return false;
    return giulia_gpu_cache_publish(cpu,128*4096)!=0;
}
uint8_t *GiuliaGPU::devPointer(uint32_t at)
{
    if(at>=GGD_BYTES)return NULL;
    return (uint8_t *)devMemory[at/GGD_CHUNK]->getBytesNoCopy()+at%GGD_CHUNK;
}
void GiuliaGPU::devAllocation(unsigned n)
{
    const struct ggd_allocation &a=devPool.allocations[n];
    uint32_t gmu=(a.flags&0x80?GGD_CACHE:GGD_BASE)+a.offset;
    IOLog("GIULIA GPU DEV: allocation=%u offset=%#x bytes=%#x flags=%#x kind=%u gpu=%#x gmu=%#x\n",
        n,a.offset,a.bytes,a.flags,a.kind,GGD_BASE+a.offset,gmu);
}
void GiuliaGPU::devCommand(const uint32_t *v)
{
    unsigned token=v[0],op=v[1];uint32_t a=v[2],b=v[3],c=v[4],d=v[5];int code=0;
    if(!hostPrepared)code=1;
    else if(op!=0 && !manualMemoryPinned)code=2;
    else switch(op) {
    case 0:
        IOLog("GIULIA GPU DEV: abi=1 gmu_root=%#llx gpu_root=%#llx base=%#x bytes=%#x cache_base=%#x pinned=%u\n",
            (unsigned long long)gmuRoot,(unsigned long long)gpuRoot,GGD_BASE,GGD_BYTES,GGD_CACHE,manualMemoryPinned);
        IOLog("GIULIA GPU DEV: mappings=privileged-rwx-normal-nc native_page=4096 lifecycle=until-reboot device_writes=0\n");break;
    case 1: {
        int n=ggd_alloc(&devPool,a,b,c,d);
        if(n<0){code=3;break;}
        devAllocation((unsigned)n);break;
    }
    case 2:
        if(!b || b>32 || (a&3) || !ggd_bounds(a,b*4)){code=4;break;}
        __asm__ volatile("dsb sy" ::: "memory");
        for(unsigned i=0;i<b;i++)IOLog("GIULIA GPU DEV: word=%u value=%#x\n",i,*(volatile uint32_t *)devPointer(a+i*4));
        break;
    case 3:
        if((a&3)||!ggd_bounds(a,4)){code=4;break;}
        *(volatile uint32_t *)devPointer(a)=b;__asm__ volatile("dsb sy" ::: "memory");break;
    case 4:
        if(!ggd_bounds(a,b)){code=4;break;}
        while(b) {uint32_t n=GGD_CHUNK-a%GGD_CHUNK;if(n>b)n=b;bzero(devPointer(a),n);a+=n;b-=n;}
        __asm__ volatile("dsb sy" ::: "memory");break;
    case 5:
        if(a>=255){code=4;break;}devPacket[a]=b;devWritten[a]=true;break;
    case 6: {
        if(!a || a>255 || ((devPacket[0]>>8)&255)!=a){code=4;break;}
        for(unsigned i=0;i<a;i++)if(!devWritten[i])code=5;
        if(code)break;
        struct ggh_io io={(volatile uint32_t *)allocations[hfiIndex]->getBytesNoCopy(),GGH_BYTES,GGH_IOVA,NULL,gpuHfiBarrier};
        code=ggh_send(&io,devPacket,a);
        if(!code)bzero(devWritten,sizeof(devWritten));
        break;
    }
    case 7: {
        struct gg_pack pack={};
        if(a>=GG_ASSETS || !gg_pack_open((const void *)firmwareMap->getVirtualAddress(),firmwareMemory->getLength(),&pack) ||
           !ggd_bounds(b,(uint32_t)pack.asset[a].size)){code=4;break;}
        size_t left=pack.asset[a].size,offset=0;
        while(left){size_t n=GGD_CHUNK-b%GGD_CHUNK;if(n>left)n=left;memcpy(devPointer(b),pack.asset[a].data+offset,n);b+=n;offset+=n;left-=n;}
        __asm__ volatile("dsb sy" ::: "memory");
        IOLog("GIULIA GPU DEV: asset=%u bytes=%lu\n",a,(unsigned long)pack.asset[a].size);break;
    }
    case 8:for(unsigned i=0;i<devPool.count;i++)devAllocation(i);break;
    case 9:
        if(!ggd_bounds(a,1)){code=4;break;}
        IOLog("GIULIA GPU DEV: offset=%#x pa=%#llx\n",a,(unsigned long long)
            devMemory[a/GGD_CHUNK]->getPhysicalSegment(a%GGD_CHUNK,NULL,kIOMemoryMapperNone));break;
    case 10:bzero(devWritten,sizeof(devWritten));break;
    default:code=6;break;
    }
    IOLog("GIULIA GPU DEV: done token=%u op=%u code=%d\n",token,op,code);
}
