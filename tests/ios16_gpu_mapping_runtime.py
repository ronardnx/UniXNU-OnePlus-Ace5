"""Exercise actual allocation/table/mapping methods with independent page walks.

No device access. The fixture checks descriptor semantics and publication/TLB
ordering; it cannot prove physical cache coherence or actual GPU transactions.
"""
from pathlib import Path as _ModulePath
import sys as _module_sys
_module_sys.path.insert(0, str(_ModulePath(__file__).resolve().parents[1] / "scripts"))
from ios16_sources import include_flags
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "ios16/gpu/GiuliaGPU.cpp").read_text()
begin = source.index("static IOBufferMemoryDescriptor *gpuAllocateHostMemory(")
end = source.index("\nint GiuliaGPU::readRegister(", begin)
dev = (ROOT / "ios16/gpu/giulia_gpu_dev_impl.h").read_text()
dev = dev[:dev.index("uint8_t *GiuliaGPU::devPointer(")]
fixture = r'''
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <utility>
#include <vector>
#include "giulia_gpu_pack.h"
#include "giulia_gpu_memory.h"
#include "giulia_gpu_cache.h"
#include "giulia_gpu_hfi.h"
#include "giulia_gpu_dev.h"
#include "giulia_gpu_mapping.h"
#include "giulia_gpu_memstore_mapping.h"
#include "giulia_gpu_ipc.h"
#include "giulia_gpu_ipcc_owner.h"
#include "giulia_gpu_fence_mapping.h"
#include "giulia_gpu_gmu_boot.h"
#include "vendor/ArmSmmu4K/ArmSmmu4K.h"
using IOByteCount=uint64_t;
static unsigned long gPhysBase=0x880000000ULL,gPhysSize=0x100000000ULL;
static void *kernel_task=nullptr;
static struct ggi_owner gpuIpcOwner;
static int giulia_platform_qmp_available(){return 1;}
enum { kIODirectionInOut=3,kIOMemoryHostPhysicallyContiguous=0x80,
    kIOMemoryMapperNone=0x800,kIOMapCopybackCache=0x400,kIOMapWriteCombineCache=0x500 };
static unsigned calls,allocated,freed,cleanCalls,cleanFail,pollCalls,pollFail;
static unsigned barriers;
static void IOLog(const char *,...) {}
static void gpuHfiBarrier(void *) { ++barriers;__sync_synchronize(); }
static void *IOMalloc(size_t n){return malloc(n);}
static void IOFree(void *p,size_t){free(p);}
struct IOBufferMemoryDescriptor {
    void *cpu;size_t bytes;uint64_t pa;
    static IOBufferMemoryDescriptor *inTaskWithOptions(void *,unsigned options,size_t n,size_t alignment) {
        assert(options==(kIODirectionInOut|kIOMemoryHostPhysicallyContiguous|kIOMemoryMapperNone|kIOMapCopybackCache) ||
               options==(kIODirectionInOut|kIOMemoryHostPhysicallyContiguous|kIOMemoryMapperNone|kIOMapWriteCombineCache));
        assert(n && !(n&0x3fff) && alignment==0x4000);
        auto *p=new IOBufferMemoryDescriptor;
        assert(!posix_memalign(&p->cpu,alignment,n));memset(p->cpu,0xa5,n);p->bytes=n;
        // Deliberate 4 MiB physical holes between every 4 MiB pool chunk.
        p->pa=gPhysBase+(++calls)*0x800000ULL;++allocated;return p;
    }
    uint64_t getPhysicalSegment(unsigned off,IOByteCount *length,unsigned options) {
        assert(off<bytes && options==kIOMemoryMapperNone);if(length)*length=bytes-off;return pa+off;
    }
    void *getBytesNoCopy(){return cpu;}
    size_t getLength(){return bytes;}
    void release(){free(cpu);++freed;delete this;}
};
extern "C" int giulia_gpu_cache_publish(const void *p,size_t n) {
    assert(p && !((uintptr_t)p&0x3fff) && n && !(n&0x3fff));
    return ++cleanCalls!=cleanFail;
}
class GiuliaGPU {
public:
    IOBufferMemoryDescriptor *allocations[GG_FW_MAX_BLOCKS]={},*tableMemory=nullptr;
    IOBufferMemoryDescriptor *devMemory[GGD_CHUNKS]={},*gpuTableMemory=nullptr;
    unsigned reservationCount=0,tablePages=0,leafPages=0,hfiIndex=0;
    bool hostPrepared=false,manualMemoryPinned=true;
    uint64_t gmuRoot=0,gpuRoot=0;
    struct ggb_config gmuConfig;
    struct gg_reservation gmuReservations[GG_FW_MAX_BLOCKS];
    ArmSmmu4KResult gmuSmmuBuild;
    IOBufferMemoryDescriptor *memstoreMemory=nullptr;uint32_t memstoreFlags=0;bool memstoreMapped=false;
    bool ipcMapped=false,fenceMapped=false;uint32_t ipcFlags=0;
    int ipcAllocate(uint32_t,uint32_t,uint32_t,uint32_t);int auditIpcMappings();
    bool prepareInitialMappings();int memstoreAllocate(uint32_t,uint32_t,uint32_t,uint32_t);
    int memstoreRead(uint32_t);int auditMemstoreMappings();int auditSecureMappings(){return 0;}
    int auditFenceMappings(){return 0;} // No provider is installed in this fixture.
    struct ggd_pool devPool={};bool devUcode[GGD_MAX]={},devMappingsManaged=false;
    bool platformPrepared=true,platformPoisoned=false,powerAvailable=true,idleAllowed=true;
    std::map<uint64_t,uint32_t> registers;
    std::vector<uint64_t> reads;
    std::vector<std::pair<uint64_t,uint32_t>> writes;
    GiuliaGPU(){devPool.next=GGD_CACHE_BYTES;registers[0x3d7e340]=3;}
    ~GiuliaGPU(){for(auto *p:allocations)if(p)p->release();if(tableMemory)tableMemory->release();
        for(auto *p:devMemory)if(p)p->release();if(gpuTableMemory)gpuTableMemory->release();if(memstoreMemory)memstoreMemory->release();}
    bool prepareDevMemory(ArmSmmu4KRange *,size_t *);
    bool prepareMemory(const gg_pack *);
    int configureDevMappings();int mapDevAllocation(unsigned,bool=false);int syncDevMappings();int auditDevMappings();
    void setProperty(const char *,unsigned long long,unsigned){}
    void setProperty(const char *,const char *){}
    bool platformPower(){return powerAvailable;}
    bool platformIdle(){return idleAllowed && !(registers[0x3dc0000]&1) && !(registers[0x3dc2000]&1);}
    uint32_t platformRead(uint64_t a){reads.push_back(a);return registers[a];}
    void platformWrite(uint64_t a,uint32_t v){writes.emplace_back(a,v);registers[a]=v;}
    bool platformPoll(uint64_t a,uint32_t mask,uint32_t value){
        assert(a==0x3dc07f4 || a==0x3dc27f4);assert(mask==1 && value==0);
        return ++pollCalls!=pollFail && (registers[a]&mask)==value;
    }
    void active(){
        for(unsigned i=0;i<2;i++) {uint64_t b=i?0x3dc2000:0x3dc0000,r=i?gmuRoot:gpuRoot;
            unsigned bank=i?2:0,slot=i?0:1,asid=i?0x750:0x752;
            registers[b]=0x27;registers[b+0x20]=(uint32_t)r;registers[b+0x24]=(uint32_t)(r>>32)|(asid<<16);
            registers[b+0x28]=0;registers[b+0x2c]=asid<<16;
            registers[b+0x10]=0x38071;registers[b+0x30]=0x80802020;registers[b+0x38]=0xff44;
            registers[0x3da1800+4*bank]=1;registers[0x3da1000+4*bank]=0x1f300;
            registers[0x3da0800+4*slot]=0x80000000u|(i?5:0);registers[0x3da0c00+4*slot]=bank;}
    }
};
'''
fixture += source[begin:end] + dev
fixture += (ROOT / "ios16/gpu/giulia_gpu_mapping_impl.h").read_text()
fixture += r'''
// Independent walker. No ggm_* or production permission helper is used below.
static constexpr uint64_t ADDRESS=0x0000fffffffff000ULL,NX=3ULL<<53;
struct Leaf {uint64_t value;size_t word;};
static Leaf walk(IOBufferMemoryDescriptor *table,uint64_t va) {
    assert(va<0x100000000ULL);auto *p=(uint64_t *)table->cpu;size_t page=0;
    for(unsigned level=1;level<3;level++) {
        unsigned shift=39-9*level,index=(unsigned)((va>>shift)&(level==1?3:511));
        uint64_t e=p[page*512+index],pa=e&ADDRESS;
        assert((e&~ADDRESS)==3 && pa>=table->pa && pa-table->pa<table->bytes);
        page=(size_t)((pa-table->pa)/4096);
    }
    size_t word=page*512+(size_t)((va>>12)&511);return {p[word],word};
}
static uint64_t backing(GiuliaGPU &s,uint32_t off) {return s.devMemory[off/GGD_CHUNK]->pa+off%GGD_CHUNK;}
static void check_leaf(GiuliaGPU &s,IOBufferMemoryDescriptor *table,uint64_t va,uint32_t off,uint64_t attrs) {
    uint64_t value=walk(table,va).value;
    assert((value&ADDRESS)==backing(s,off));assert((value&~ADDRESS)==attrs);
}
static std::vector<uint64_t> copytable(IOBufferMemoryDescriptor *p) {
    auto *w=(uint64_t *)p->cpu;return {w,w+p->bytes/8};
}
struct Snapshot {
    std::vector<uint64_t> gpu,gmu;std::vector<std::vector<unsigned char>> ram;
    Snapshot(GiuliaGPU &s):gpu(copytable(s.gpuTableMemory)),gmu(copytable(s.tableMemory)) {
        for(unsigned i=0;i<s.reservationCount;i++) {auto *m=s.allocations[i];auto *p=(unsigned char *)m->cpu;ram.emplace_back(p,p+m->bytes);}
    }
    void exact(GiuliaGPU &s) {assert(gpu==copytable(s.gpuTableMemory) && gmu==copytable(s.tableMemory));}
    void unrelated(GiuliaGPU &s) {
        std::set<size_t> gl,ml;
        for(uint32_t off=0;off<GGD_BYTES;off+=4096) {
            gl.insert(walk(s.gpuTableMemory,GGD_BASE+off).word);ml.insert(walk(s.tableMemory,GGD_BASE+off).word);
            if(off<GGD_CACHE_BYTES)ml.insert(walk(s.tableMemory,GGD_CACHE+off).word);
        }
        auto nowg=copytable(s.gpuTableMemory),nowm=copytable(s.tableMemory);
        for(size_t i=0;i<gpu.size();i++)if(!gl.count(i))assert(gpu[i]==nowg[i]);
        for(size_t i=0;i<gmu.size();i++)if(!ml.count(i))assert(gmu[i]==nowm[i]);
        for(unsigned i=0;i<s.reservationCount;i++)assert(!memcmp(ram[i].data(),s.allocations[i]->cpu,ram[i].size()));
    }
};
static void prepare(GiuliaGPU &s) {
    // Code/data firmware reservations plus a non-aligned payload exercise real setup.
    static const uint32_t fw[]={0x4000,0,7,4096,0x60000000,0,7,4096,
        0x4004,4,0,0,0x12345678,0x60000004,4,0,0,0x87654321};
    gg_pack pack={};pack.asset[0].data=(const uint8_t *)fw;pack.asset[0].size=sizeof(fw);
    assert(s.prepareMemory(&pack) && s.hostPrepared);
    // A provider has not supplied backing: all 512 GMU leaves must be literal
    // zero, while the GPU tree must have no L2 branch for the fence aperture.
    for(unsigned at=0;at<GGF_APERTURE;at+=4096)
        assert(walk(s.tableMemory,GGF_IOVA+at).value==0);
    auto *gpuWords=(uint64_t *)s.gpuTableMemory->cpu;
    uint64_t parent=gpuWords[GGF_IOVA>>30],pa=parent&ADDRESS;
    assert((parent&~ADDRESS)==3 && pa>=s.gpuTableMemory->pa &&
           pa-s.gpuTableMemory->pa<s.gpuTableMemory->bytes);
    size_t l2=(size_t)((pa-s.gpuTableMemory->pa)/4096);
    assert(gpuWords[l2*512+((GGF_IOVA>>21)&511)]==0);
    for(unsigned i=1;i<GGD_CHUNKS;i++)assert(s.devMemory[i]->pa!=s.devMemory[i-1]->pa+GGD_CHUNK);
}
struct Expected {unsigned index;uint64_t gpu,gmu;};
static void all_pages(GiuliaGPU &s,const std::vector<Expected> &expected) {
    for(uint32_t off=0;off<GGD_BYTES;off+=4096) {
        uint64_t g=0,m=0,cache=0;
        for(auto &e:expected) {auto &a=s.devPool.allocations[e.index];if(off>=a.offset && off-a.offset<a.bytes) {
            g=e.gpu;if(a.flags&0x80)cache=e.gmu;else m=e.gmu;}}
        check_leaf(s,s.gpuTableMemory,GGD_BASE+off,off,g);check_leaf(s,s.tableMemory,GGD_BASE+off,off,m);
        if(off<GGD_CACHE_BYTES)check_leaf(s,s.tableMemory,GGD_CACHE+off,off,cache);
    }
}
static unsigned add(GiuliaGPU &s,uint32_t flags,uint32_t bytes=4096,bool ucode=false) {
    int i=ggd_alloc(&s.devPool,bytes,4096,flags,0);assert(i>=0);s.devUcode[i]=ucode;return (unsigned)i;
}
static void check_tlb_writes(GiuliaGPU &s) {
    assert(!s.writes.empty() && !(s.writes.size()%4));
    for(size_t i=0;i<s.writes.size();i+=4) {
        assert(s.writes[i]==std::make_pair(uint64_t(0x3dc0618),uint32_t(0)));
        assert(s.writes[i+1]==std::make_pair(uint64_t(0x3dc07f0),uint32_t(0xffffffff)));
        assert(s.writes[i+2]==std::make_pair(uint64_t(0x3dc2618),uint32_t(0)));
        assert(s.writes[i+3]==std::make_pair(uint64_t(0x3dc27f0),uint32_t(0xffffffff)));
    }
}
static void normal() {
    GiuliaGPU s;prepare(s);Snapshot before(s);
    for(uint32_t off=0;off<GGD_BYTES;off+=4096) {
        check_leaf(s,s.gpuTableMemory,GGD_BASE+off,off,0);
        check_leaf(s,s.tableMemory,GGD_BASE+off,off,0);
        if(off<GGD_CACHE_BYTES)check_leaf(s,s.tableMemory,GGD_CACHE+off,off,0);
    }
    assert(s.configureDevMappings()==0 && s.devMappingsManaged);all_pages(s,{});before.unrelated(s);
    assert(s.auditDevMappings()==0);assert(s.configureDevMappings()==0);s.active();s.writes.clear();
    const struct {uint32_t flags;uint64_t gpu,gmu;} cases[]={
        {0x339,NX|0xec3,0xe03},{0x37b,NX|0xe83,0xe03},{0x178,0,0xe03},
        {0x147,NX|0xe03,0},{0x33f,NX|0xe03,0xe03},{0x3bf,NX|0xe03,0xe03},
        {0x109,NX|0xec3,0xec3},{0x101,NX|0xec3,0}};
    std::vector<Expected> expected;
    for(auto &c:cases) {unsigned i=add(s,c.flags,8192);assert(s.mapDevAllocation(i)==0);
        expected.push_back({i,c.gpu,c.gmu});all_pages(s,expected);before.unrelated(s);}
    unsigned ucode=add(s,0x301,12288,true);assert(s.mapDevAllocation(ucode)==0);
    expected.push_back({ucode,0xec3,0});all_pages(s,expected);
    // One allocation spans non-contiguous physical pool chunks.
    s.devPool.next=GGD_CHUNK*2-4096;unsigned split=add(s,0x33f,12288);
    assert(s.mapDevAllocation(split)==0);expected.push_back({split,NX|0xe03,0xe03});all_pages(s,expected);
    assert(s.auditDevMappings()==0);before.unrelated(s);check_tlb_writes(s);
    // Mutate one otherwise valid AP bit: production audit must detect drift.
    auto &a=s.devPool.allocations[ucode];auto leaf=walk(s.gpuTableMemory,GGD_BASE+a.offset);
    ((uint64_t *)s.gpuTableMemory->cpu)[leaf.word]^=0x40;
    assert(s.auditDevMappings()!=0);
}
static void existing() {
    GiuliaGPU s;prepare(s);unsigned i=add(s,0x33f),c=add(s,0x3bf);Snapshot before(s);
    assert(s.configureDevMappings()==0);all_pages(s,{{i,NX|0xe03,0xe03},{c,NX|0xe03,0xe03}});before.unrelated(s);
}
static void guards() {
    GiuliaGPU s;prepare(s);Snapshot original(s);
    s.idleAllowed=false;assert(s.configureDevMappings()!=0);original.exact(s);s.idleAllowed=true;
    // Real reset-state TTBRs may contain garbage while SCTLR.M is zero.
    // Initial configuration must neither read nor validate those inactive roots.
    s.registers[0x3dc0020]=0xdeadb000;s.registers[0x3dc0024]=0xabcdef12;
    s.registers[0x3dc2020]=0x12345000;s.registers[0x3dc2024]=0xfedcba98;
    s.reads.clear();assert(s.configureDevMappings()==0 && s.devMappingsManaged);
    for(uint64_t address:s.reads)
        assert(address!=0x3dc0020 && address!=0x3dc0024 && address!=0x3dc2020 && address!=0x3dc2024);
    all_pages(s,{});original.unrelated(s);s.active();unsigned i=add(s,0x33f);Snapshot empty(s);
    s.registers[0x3dc0020]^=4096;assert(s.mapDevAllocation(i)!=0);empty.exact(s);s.registers[0x3dc0020]^=4096;
    s.registers[0x3dc2020]^=4096;assert(s.mapDevAllocation(i)!=0);empty.exact(s);s.registers[0x3dc2020]^=4096;
    s.registers[0x3d7e340]=0;s.registers[0x3d8ec30]=1u<<23;
    assert(s.mapDevAllocation(i)!=0);empty.exact(s);s.registers[0x3d8ec30]=0;
    s.registers[0x3d8ec40]=1;assert(s.mapDevAllocation(i)!=0);empty.exact(s);s.registers[0x3d8ec40]=0;
    cleanFail=cleanCalls+1;assert(s.mapDevAllocation(i)!=0);empty.exact(s);cleanFail=0;
    s.powerAvailable=false;assert(s.mapDevAllocation(i)!=0);empty.exact(s);s.powerAvailable=true;
    assert(s.mapDevAllocation(i)==0);assert(s.auditDevMappings()==0);
}
static void context_guards() {
    GiuliaGPU s;prepare(s);assert(s.configureDevMappings()==0);s.active();unsigned i=add(s,0x33f);
    Snapshot before(s);
    const uint64_t addresses[]={0x3da0048,0x3dc0000,0x3dc2000,0x3dc0010,0x3dc2010,
        0x3dc0030,0x3dc2030,0x3dc0038,0x3dc2038,0x3dc003c,0x3dc203c,
        0x3da1800,0x3da1808,0x3da1000,0x3da1008,0x3da0800,0x3da0804,
        0x3da0c00,0x3da0c04,0x3dc0024,0x3dc2024,0x3dc0028,0x3dc2028,0x3dc002c,0x3dc202c,
        0x3dc0058,0x3dc2058};
    for(uint64_t address:addresses) {
        uint32_t saved=s.registers[address];s.registers[address]^=2;
        size_t writes=s.writes.size();assert(s.mapDevAllocation(i)!=0);
        assert(s.writes.size()==writes);before.exact(s);s.registers[address]=saved;
    }
    assert(s.mapDevAllocation(i)==0);
    assert(s.mapDevAllocation(i,true)==0);all_pages(s,{});
    s.devUcode[i]=true;assert(s.mapDevAllocation(i)==0);all_pages(s,{{i,0xec3,0}});
    assert(s.auditDevMappings()==0);
}
static void failure_after_edit(bool tlb,bool afterApply=false) {
    GiuliaGPU s;prepare(s);assert(s.configureDevMappings()==0);s.active();unsigned i=add(s,0x33f);
    assert(s.mapDevAllocation(i)==0);auto &a=s.devPool.allocations[i];a.flags=0x339;
    Snapshot before(s);
    if(tlb)pollFail=pollCalls+(afterApply?5:3);else cleanFail=cleanCalls+(afterApply?5:3);
    assert(s.mapDevAllocation(i)!=0 && s.platformPoisoned);
    // Failure while publishing the BBM invalidation must not install new access.
    check_leaf(s,s.gpuTableMemory,GGD_BASE+a.offset,a.offset,afterApply?NX|0xec3:0);
    check_leaf(s,s.tableMemory,GGD_BASE+a.offset,a.offset,afterApply?0xe03:0);before.unrelated(s);
    auto writes=s.writes.size();assert(s.mapDevAllocation(i)!=0 && s.writes.size()==writes);
    pollFail=cleanFail=0;
}
int main() {
    normal();existing();guards();context_guards();failure_after_edit(false);failure_after_edit(true);
    failure_after_edit(false,true);failure_after_edit(true,true);
    assert(allocated==freed && barriers);
    puts("Actual GPU mapping methods: all 33792 pool/alias pages, captured flags, UCODE, noncontiguous chunks, firmware/HFI preservation, stale roots, cache/TLB failure quarantine passed");
}
'''
fixture = fixture.replace('__asm__ volatile("dsb sy" ::: "memory")', '__sync_synchronize()')
with tempfile.TemporaryDirectory(prefix="gpu-mapping-runtime-") as folder:
    path = Path(folder)
    (path / "mapping.cpp").write_text(fixture)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-c", str(ROOT / "ios16/vendor/ArmSmmu4K/ArmSmmu4K.c"),
                    "-o", str(path / "tables.o")], check=True)
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-Wno-misleading-indentation", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    *include_flags(ROOT), str(path / "mapping.cpp"), str(path / "tables.o"),
                    "-o", str(path / "mapping")], check=True)
    subprocess.run([str(path / "mapping")], check=True)
