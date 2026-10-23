"""Exercise actual platform methods with owned-memory/SCM doubles; no device."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'ios16/GiuliaGPU.cpp').read_text()
members=source[source.index('    IOBufferMemoryDescriptor *secureData'):source.index('    bool prepareDevMemory')]
fixture=r'''
#define GG_HOST_TEST 1
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
#include <memory>
#include "giulia_gpu_pack.h"
#include "giulia_gpu_hfi.h"
#include "giulia_gpu_dev.h"
#include "giulia_gpu_platform.h"
#include "giulia_gpu_power.h"
#include "vendor/ArmSmmu4K/ArmSmmu4K.h"
using IOByteCount=uint64_t;
enum { kIOMemoryMapperNone=0x800,kIODirectionInOut=3,kIOMapInhibitCache=0x100,kIOMapWriteCombineCache=0x500 };
static void IOLog(const char *,...){}
static void IOSleep(unsigned){}
static void gpuHfiBarrier(void *){__sync_synchronize();}
static void *IOMalloc(size_t n){return malloc(n);}
static void IOFree(void *p,size_t){free(p);}
struct IOMemoryMap {uintptr_t address;uintptr_t getVirtualAddress(){return address;}};
struct IOMemoryDescriptor {
    uint64_t pa;size_t bytes;unsigned char *cpu;bool cp_only=false;
    IOMemoryMap mapping;
    IOMemoryDescriptor(uint64_t a,size_t n):pa(a),bytes(n) {
        assert(!posix_memalign((void **)&cpu,16384,(n+16383)&~16383ul));memset(cpu,0,n);mapping.address=(uintptr_t)cpu;
    }
    virtual ~IOMemoryDescriptor(){free(cpu);}
    IOMemoryMap *map(unsigned){return &mapping;}
    uint64_t getPhysicalSegment(unsigned offset,IOByteCount *span,unsigned options) {
        assert(options==kIOMemoryMapperNone && offset<bytes);if(span)*span=bytes-offset;return pa+offset;
    }
    void *getBytesNoCopy(){assert(!cp_only && "CPU accessed a CP_PIXEL-only buffer");return cpu;}
    size_t getLength(){return bytes;}
    static IOMemoryDescriptor *withPhysicalAddress(uint64_t,size_t,unsigned);
};
struct IOBufferMemoryDescriptor: IOMemoryDescriptor {using IOMemoryDescriptor::IOMemoryDescriptor;};
static std::vector<std::unique_ptr<IOMemoryDescriptor>> memories;
IOMemoryDescriptor *IOMemoryDescriptor::withPhysicalAddress(uint64_t pa,size_t n,unsigned) {
    auto *p=new IOMemoryDescriptor(pa,n);memories.emplace_back(p);return p;
}
static uint64_t nextPA=0x880000000ull;
static IOBufferMemoryDescriptor *gpuAllocateHostMemory(size_t n,bool=true) {
    auto *p=new IOBufferMemoryDescriptor(nextPA,n);nextPA+=((n+0xffff)&~0xffffull)+0x4000;memories.emplace_back(p);return p;
}
static bool gpuHostRangeValid(void *p,uint64_t pa,uint64_t span,size_t n)
{return p && !(pa&16383) && span>=n && pa+n<(1ull<<36);}
static IOMemoryDescriptor *find(uint64_t pa) {
    for(auto &p:memories)if(pa>=p->pa && pa-p->pa<p->bytes)return p.get();assert(false);return nullptr;
}
static void *pointer(uint64_t pa) {auto *p=find(pa);return (unsigned char *)p->getBytesNoCopy()+pa-p->pa;}
static unsigned assignments,failAssignment;
static int ggs_aarch64_call(void *,struct ggs_regs *r) {
    uint64_t fn=r->x[0];
    if(fn==0xc2000601 || fn==0x42000601){r->x[0]=0;r->x[1]=1;return 0;}
    if(fn==0x42000c16) {
        assert(r->x[1]==0x1117 && (r->x[3]==16 || r->x[3]==512));
        auto *regions=(ggs_mem_region *)pointer(r->x[2]);
        assert(*(uint32_t *)pointer(r->x[4])==3);
        auto *ext=(uint64_t *)pointer(r->x[5]);assert(ext[0]==4 && !ext[3]);
        auto *dest=(ggs_vmperm *)pointer(ext[1]);
        if(ext[2]==48){assert(dest[0].vmid==3 && dest[0].perm==6 && dest[1].vmid==10 && dest[1].perm==4);}
        for(unsigned i=0;i<r->x[3]/16;i++) {
            auto region=regions[i];auto *memory=find(region.address);assert(region.bytes<=memory->bytes-(region.address-memory->pa));
            if(ext[2]==24) {assert(dest[0].vmid==10 && dest[0].perm==6);memory->cp_only=true;}
        }
        ++assignments;r->x[0]=0;r->x[1]=assignments==failAssignment?1:0;return 0;
    }
    assert(fn==0x42002801 || fn==0x42000c1b);r->x[0]=r->x[1]=0;return 0;
}
class GiuliaGPU {
public:
'''+members+r'''
    bool manualMemoryPinned=true;unsigned mode=3;uint64_t gpuRoot=0x870000000ull;
    IOBufferMemoryDescriptor *firmwareMemory=nullptr,*allocations[8]={};IOMemoryMap *firmwareMap=nullptr;
    unsigned hfiIndex=0;uint32_t devPacket[255]={};bool devWritten[255]={};
    static int readRegister(void *,unsigned window,uint32_t offset,uint32_t *v) {
        *v=0;
        if(window==GGP_GCC)*v=1;
        if(window==GGP_GPUCC && offset==0x9168)*v=0x80000000;
        if(window==GGP_GPUCC && (offset==0x7000 || offset==0x913c))*v=1;
        if(window==GGP_SMMU && offset==0x20)*v=0x4c017e18;
        return 0;
    }
};
'''+(ROOT/'ios16/giulia_gpu_platform_impl.h').read_text()+r'''
int GiuliaGPU::configureDevMappings(){return 8;} // Separate actual mapping-method suite.
static void idle(GiuliaGPU &s) {
    s.platformWrite(0x3d7e000,1);
    for(unsigned i=0;i<24;i++)s.platformWrite(0x3da0c00+4*i,0x20000);
}
static void pagewalk(GiuliaGPU &s) {
    auto *tables=(uint64_t *)s.secureTables->getBytesNoCopy();
    for(unsigned segment=0;segment<GGS_SEC_SEGMENTS;segment++)for(unsigned offset=0;offset<GGS_SEC_GRANULE;offset+=4096) {
        uint64_t va=GGS_SEC_BASE+(uint64_t)segment*GGS_SEC_GRANULE+offset;size_t at=0;
        for(unsigned level=1;level<3;level++) {uint64_t e=tables[at*512+((va>>(39-9*level))&511)];assert((e&3)==3);at=((e&0x0000fffffffff000ull)-s.secureRoot)/4096;assert(at<GGS_SEC_TABLE_BYTES/4096);}
        uint64_t leaf=tables[at*512+((va>>12)&511)];
        uint64_t relative=va-GGS_SEC_BASE;
        assert((leaf&0x0000fffffffff000ull)==s.securePA[relative/GGS_SEC_GRANULE]+relative%GGS_SEC_GRANULE);
        assert((leaf&(3ull<<53))==(3ull<<53) && (leaf&0xfff)==0xe03); // secure data RW/priv/NX
    }
}
int main() {
    GiuliaGPU good={};assert(good.platformPrepare());idle(good);pagewalk(good);
    assert(good.platformCommand(1)==0 && good.platformCommand(2)==0);
    assert(good.platformCommand(4)==0 && good.secureOwnershipReady && assignments==17);
    for(unsigned i=0;i<GGS_SEC_SEGMENTS;i++)assert(good.secureData[i]->cp_only);
    assert(!good.secureTables->cp_only && !good.scmMemory->cp_only);
    assert(good.secureAllocate(4096,4096,0x1007,5)==8); /* No mapping success yet. */
    good.secureContextActive=true;good.platformWrite(0x3da0c08,3);
    for(unsigned i=0;i<GGS_SEC_CHUNKS;i++)assert(good.secureAllocate(0x37d000,4096,0x1007,5)==0);
    assert(good.secureAllocate(4096,4096,0x1007,5)==3);
    assert(good.secureAllocate(4096,4096,0x1107,5)==4);
    GiuliaGPU bad={};assert(bad.platformPrepare());idle(bad);
    assert(bad.platformCommand(2)==0);failAssignment=assignments+3;
    assert(bad.platformCommand(4)!=0 && bad.platformPoisoned && bad.platformOwned && !bad.secureOwnershipReady);
    unsigned old=assignments;assert(bad.platformCommand(4)!=0 && assignments==old);
    assert(bad.secureAllocate(4096,4096,0x1007,5)!=0);
    puts("Actual GPU platform methods: independent walks, 17 ownership calls, denied CPU aliases, exhaustion and ambiguous-failure retention passed");
}
'''
with tempfile.TemporaryDirectory(prefix='gpu-platform-runtime-') as temporary:
    p=Path(temporary);(p/'test.cpp').write_text(fixture)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-c',str(ROOT/'ios16/vendor/ArmSmmu4K/ArmSmmu4K.c'),'-o',str(p/'tables.o')],check=True)
    subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-g','-I',str(ROOT/'ios16'),str(p/'test.cpp'),str(p/'tables.o'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
