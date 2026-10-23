/* Host-only IOKit boundary fixture; production methods are generated beside it
 * by ios16_wifi_runtime.py. Run through that driver, not standalone. */
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include "giulia_wifi_pci.h"
#include "giulia_wifi_link.h"
#include "giulia_wifi_smmu.h"
#include "giulia_wifi_mhi.h"
#include "giulia_wifi_ce.h"
#include "giulia_wifi_apple.h"
#include "giulia_wifi_firmware.h"
#include "giulia_wifi_qmp.h"
#include "giulia_wifi_pmu.h"
#include "giulia_wifi_mapping.h"
#include "giulia_wifi_network.h"
#include "giulia_wifi_dp.h"
#include "giulia_wifi_station.h"
#include "giulia_wifi_rsn.h"
#include "giulia_wifi_security.h"
#include "ios16_wifi_crypto_host.inc"
#define GW_ARENA_BYTES GWMAP_ARENA_BYTES
#define GW_IOVA GWMAP_IOVA
#define GW_CE_OFFSET GWMAP_CE_OFFSET
#define GW_ALLOC_START GWMAP_ALLOC_START

using vm_offset_t = uintptr_t;
using IOByteCount = size_t;
using UInt32 = uint32_t;
using IOReturn = uint32_t;
using task_t = void *;
using ifnet_t = void *;
using IOOptionBits = unsigned;
using errno_t = int;
static unsigned long gPhysBase=0x880000000ULL,gPhysSize=0x100000000ULL;
static void *kernel_task=nullptr;
enum {kIODirectionInOut=3,kIOMemoryHostPhysicallyContiguous=0x80,
    kIOMemoryMapperNone=0x800,kIOMapInhibitCache=0x200};
enum : IOReturn {
    kIOReturnSuccess=0, kIOReturnBadArgument=0xe00002c2,
    kIOReturnNoMemory=0xe00002bd, kIOReturnNotReady=0xe00002d8,
    kIOReturnBusy=0xe00002d5, kIOReturnUnsupported=0xe00002c7,
    kIOReturnNoSpace=0xe00002db,kIOReturnError=0xe00002bc,
};
static std::map<void *,size_t> allocations;
static unsigned allocationCalls, failAllocation;
static void *IOMallocData(size_t n) {
    if (++allocationCalls==failAllocation) return nullptr;
    void *p=malloc(n); assert(p); allocations[p]=n; return p;
}
static void *IOMallocZeroData(size_t n) {
    void *p=IOMallocData(n); if(p) memset(p,0,n); return p;
}
static void IOFreeData(void *p,size_t n) {
    assert(allocations.count(p) && allocations[p]==n);
    allocations.erase(p);free(p);
}
static void IOFree(void *p,size_t n) {IOFreeData(p,n);}
struct IOLock { std::mutex mutex;bool locked=false;unsigned entries=0; };
static unsigned locks;
static bool failLock;
static std::atomic<unsigned> lockWaits{0};
static IOLock *IOLockAlloc() {if(failLock)return nullptr;++locks;return new IOLock;}
static void IOLockLock(IOLock *l) {
    assert(l);if(!l->mutex.try_lock()){++lockWaits;l->mutex.lock();}
    assert(!l->locked);l->locked=true;++l->entries;
}
static void IOLockUnlock(IOLock *l) { assert(l && l->locked);l->locked=false;l->mutex.unlock(); }
static void IOLockFree(IOLock *l) {assert(!l->locked && locks);--locks;delete l;}
static uint64_t runtimeNow=1000;
static uint64_t gw_now(void *) {return runtimeNow;}
static void IOLog(const char *,...) {}
struct OSObject { virtual ~OSObject()=default; };
struct IOService : OSObject {
    IOService *provider=nullptr;
    unsigned releaseCalls=0;
    IOService *getProvider() { return provider; }
    void release() {++releaseCalls;}
};
struct IONetworkController : IOService {
    unsigned stopCalls=0;
    void free() {}
    void stop(IOService *) {++stopCalls;}
};
static unsigned serviceTerminations,serviceDetaches,serviceReleases;
struct IO80211Interface : IOService {
    bool terminated=false,detached=false;
    void terminate() {assert(!terminated);terminated=true;++serviceTerminations;}
    void detach(IOService *) {assert(terminated && !detached);detached=true;++serviceDetaches;}
    void release() {assert(terminated && detached);++serviceReleases;delete this;}
};
struct FakeIfnet {void *softc;unsigned detachCalls=0,releaseCalls=0;};
static int ifnet_detach(ifnet_t p) {++((FakeIfnet *)p)->detachCalls;return 0;}
static void *ifnet_softc(ifnet_t p) {return ((FakeIfnet *)p)->softc;}
static void ifnet_release(ifnet_t p) {++((FakeIfnet *)p)->releaseCalls;}
static void IOSleep(unsigned);
struct IOMemoryDescriptor;
static unsigned descriptors,maps,descriptorCalls,mapCalls,failDescriptor,failMap;
static unsigned badMapping; // 1: short length, 2: NULL VA, 3: overflowing VA.
static std::vector<std::pair<char,uint64_t>> releases;
struct IOMemoryMap {
    IOMemoryDescriptor *owner;
    uintptr_t address;
    size_t length;
    uintptr_t getVirtualAddress() {return address;}
    size_t getLength() {return length;}
    void release();
};
struct IOMemoryDescriptor {
    std::vector<uint8_t> bytes;
    bool shortRead=false,shortWrite=false;
    uint64_t physical=0;
    unsigned activeMaps=0;
    std::atomic<bool> *writeEntered=nullptr,*allowWrite=nullptr;
    explicit IOMemoryDescriptor(size_t n=0):bytes(n) {}
    virtual ~IOMemoryDescriptor()=default;
    static IOMemoryDescriptor *withPhysicalAddress(uint64_t pa,size_t n,unsigned options) {
        assert(options==kIODirectionInOut && pa && n);
        if(++descriptorCalls==failDescriptor)return nullptr;
        auto *p=new IOMemoryDescriptor(n);p->physical=pa;++descriptors;return p;
    }
    IOMemoryMap *map(unsigned options) {
        assert(options==kIOMapInhibitCache);
        if(++mapCalls==failMap)return nullptr;
        uintptr_t address=badMapping==2?0:badMapping==3?UINTPTR_MAX-3:(uintptr_t)bytes.data();
        auto *p=new IOMemoryMap{this,address,bytes.size()-(badMapping==1)};
        ++maps;++activeMaps;return p;
    }
    virtual void release() {
        assert(!activeMaps && descriptors);--descriptors;releases.emplace_back('d',physical);delete this;
    }
    size_t getLength() { return bytes.size(); }
    size_t readBytes(size_t offset,void *p,size_t n) {
        assert(offset<=bytes.size() && n<=bytes.size()-offset);
        size_t used=shortRead&&n?n-1:n;memcpy(p,bytes.data()+offset,used);return used;
    }
    size_t writeBytes(size_t offset,const void *p,size_t n) {
        assert(offset<=bytes.size() && n<=bytes.size()-offset);
        if(writeEntered) {
            writeEntered->store(true);
            while(!allowWrite->load())std::this_thread::yield();
        }
        size_t used=shortWrite&&n?n-1:n;memcpy(bytes.data()+offset,p,used);return used;
    }
};
void IOMemoryMap::release() {
    assert(maps && owner->activeMaps);--maps;--owner->activeMaps;
    releases.emplace_back('m',owner->physical);delete this;
}
static unsigned bufferCalls,failBuffer,buffers;
static unsigned badBuffer; // 1: short segment, 2: unaligned, 3: outside RAM, 4: overlapping tables.
struct IOBufferMemoryDescriptor : IOMemoryDescriptor {
    void *storage=nullptr;
    size_t length=0;
    static IOBufferMemoryDescriptor *inTaskWithOptions(void *,unsigned options,size_t n,size_t alignment) {
        assert(options==(kIODirectionInOut|kIOMemoryHostPhysicallyContiguous|kIOMemoryMapperNone|kIOMapInhibitCache));
        assert((n==GW_ARENA_BYTES && alignment==0x200000) || (n==0x4000 && alignment==0x4000));
        if(++bufferCalls==failBuffer)return nullptr;
        auto *p=new IOBufferMemoryDescriptor;
        assert(!posix_memalign(&p->storage,alignment,n));p->length=n;
        p->physical=gPhysBase+(n==GW_ARENA_BYTES?0x200000:badBuffer==4?0x200000:0x10000000);
        if(badBuffer==2)p->physical+=1;if(badBuffer==3)p->physical+=gPhysSize;
        ++buffers;return p;
    }
    uint64_t getPhysicalSegment(size_t offset,IOByteCount *n,unsigned options) {
        assert(offset==0 && options==kIOMemoryMapperNone);*n=length-(badBuffer==1);return physical;
    }
    void *getBytesNoCopy() {return storage;}
    void release() override {assert(buffers);--buffers;free(storage);delete this;}
};
struct IOExternalMethodDispatch {};
struct IOExternalMethodArguments {
    uint32_t scalarInputCount=0,scalarOutputCount=0;
    size_t structureInputSize=0,structureOutputSize=0;
    const void *structureInput=nullptr;
    void *structureOutput=nullptr;
    IOMemoryDescriptor *structureInputDescriptor=nullptr,*structureOutputDescriptor=nullptr;
};
struct IOUserClient : IOService {
    bool terminated=false;
    bool initWithTask(task_t,void *,UInt32) { return true; }
    void terminate() { terminated=true; }
    void free() {}
};
#define OSDeclareDefaultStructors(type)
#define OSDefineMetaClassAndStructors(type,base)
#define OSDynamicCast(type,value) dynamic_cast<type *>(value)
#define APPLE_KEXT_OVERRIDE
#define OSSafeReleaseNULL(p) do {if(p){(p)->release();(p)=nullptr;}} while(0)
class IO80211Controller;
#include "wifi_runtime_types.inc"
class IO80211Controller : public IONetworkController {
public:
    giulia_wifi_runtime *r=nullptr;
    IOReturn query(const gwi_request &,void *,size_t *,gwi_cursor *);
    void free();
    void stop(IOService *);
};
// QMP handshake behavior is separately tested in ios16_wifi_qmp.c. Preserve
// its caller-provided uncertain/retained flag while testing teardown here.
static void gw_qmp_stop(giulia_wifi_runtime *) {}
#include "ios16_wifi_network_stubs.inc"
#include "wifi_network_under_test.inc"
#include "wifi_mapping_under_test.inc"
#include "wifi_runtime_under_test.inc"
static giulia_wifi_runtime *finishingWorker;
static unsigned sleepCalls;
static void IOSleep(unsigned milliseconds) {
    assert(milliseconds==5 && finishingWorker && finishingWorker->stopping);
    ++sleepCalls;finishingWorker->worker_running=0;
}

static std::vector<uint8_t> envelope(bool set,unsigned type,size_t bytes) {
    size_t capacity=set?0x3c000:0x3c00;
    std::vector<uint8_t> p(set?GWI_SET_BYTES:GWI_GET_BYTES);
    gwq_w32(p.data(),set?GWI_SET:GWI_GET);gwq_w32(p.data()+4,(uint32_t)bytes);
    memcpy(p.data()+8+capacity,"en0",4);gwq_w32(p.data()+24+capacity,type);
    // These are untrusted user pointers and must never be dereferenced.
    gwq_w64(p.data()+40+capacity,UINT64_MAX);
    return p;
}
struct Fixture {
    IOLock lock;
    giulia_wifi_runtime runtime={};
    IO80211Controller controller;
    IOService interface;
    GiuliaWiFiUserClient client;
    Fixture() {
        runtimeNow=1000;
        runtime.lock=&lock;runtime.ready=1;runtime.powered=1;
        controller.r=&runtime;interface.provider=&controller;client.provider=&controller;
        assert(client.initWithTask(nullptr,nullptr,0));
    }
    ~Fixture() {assert(!lock.locked && allocations.empty());client.free();assert(!locks);}
    IOReturn call(std::vector<uint8_t> &input,void *out,size_t &bytes) {
        IOExternalMethodArguments a;a.structureInput=input.data();a.structureInputSize=input.size();
        a.structureOutput=out;a.structureOutputSize=bytes;
        IOReturn result=client.externalMethod(0,&a,nullptr,nullptr,nullptr);
        bytes=a.structureOutputSize;assert(!lock.locked && allocations.empty());return result;
    }
};

static void selectors_and_validation() {
    Fixture f;
    GiuliaWiFiUserClient invalid;
    assert(!invalid.initWithTask(nullptr,nullptr,1));invalid.free();
    failLock=true;assert(!invalid.initWithTask(nullptr,nullptr,0));failLock=false;
    IOExternalMethodArguments uninitialized;uninitialized.structureOutputSize=24;
    assert(invalid.externalMethod(0,&uninitialized,nullptr,nullptr,nullptr)==kIOReturnNotReady && !uninitialized.structureOutputSize);
    invalid.free();
    assert(f.client.externalMethod(0,nullptr,nullptr,nullptr,nullptr)==kIOReturnBadArgument);
    IOExternalMethodArguments a;
    assert(f.client.externalMethod(99,&a,nullptr,nullptr,nullptr)==kIOReturnUnsupported);
    for(unsigned selector:{1u,5u}) {
        uint8_t out=0xaa;a={};a.structureOutput=&out;a.structureOutputSize=1;
        assert(f.client.externalMethod(selector,&a,nullptr,nullptr,nullptr)==0 && out==(selector==1));
        a.scalarInputCount=1;assert(f.client.externalMethod(selector,&a,nullptr,nullptr,nullptr)==kIOReturnBadArgument);
        a.scalarInputCount=0;a.scalarOutputCount=1;assert(f.client.externalMethod(selector,&a,nullptr,nullptr,nullptr)==kIOReturnBadArgument);
        a.scalarOutputCount=0;a.structureOutputSize=2;assert(f.client.externalMethod(selector,&a,nullptr,nullptr,nullptr)==kIOReturnBadArgument);
    }
    auto input=envelope(false,19,24);uint8_t out[32];memset(out,0xa5,sizeof(out));size_t n=sizeof(out);
    f.runtime.ready=0;assert(f.call(input,out,n)==kIOReturnNotReady && n==0);
    f.runtime.ready=1;f.client.provider=nullptr;n=sizeof(out);
    assert(f.call(input,out,n)==kIOReturnNotReady && n==0);
    f.client.provider=&f.interface;n=sizeof(out);
    assert(f.call(input,out,n)==0 && n==24 && gwq_u32(out+8)==1 && out[24]==0xa5);
    for(size_t index:{size_t(0),size_t(4),size_t(0x3c08)}) {
        auto bad=input;bad[index]=0xff;n=sizeof(out);
        assert(f.call(bad,out,n)==kIOReturnBadArgument);
    }
    a={};a.structureInput=input.data();a.structureInputSize=input.size()-1;a.structureOutput=out;a.structureOutputSize=sizeof(out);
    assert(f.client.externalMethod(0,&a,nullptr,nullptr,nullptr)==kIOReturnBadArgument);
    a.structureInputSize=input.size();a.structureOutputSize=0x3c001;
    assert(f.client.externalMethod(0,&a,nullptr,nullptr,nullptr)==kIOReturnBadArgument);
    assert(f.client.clientClose()==0 && f.client.terminated);
}

static void query_lengths_and_failures() {
    Fixture f;
    // Apple80211Bind's first probe has no input body; the framework still
    // requests the full 256-byte driver-version response through UserClient.
    auto bind=envelope(false,GWI_DRIVER_VERSION,0);uint8_t version[272];memset(version,0xa5,sizeof(version));
    size_t versionBytes=sizeof(version);
    assert(f.call(bind,version,versionBytes)==kIOReturnSuccess && versionBytes==256 && version[0] && version[256]==0xa5);
    memset(version,0xa5,sizeof(version));versionBytes=255;
    assert(f.call(bind,version,versionBytes)==kIOReturnBadArgument && !versionBytes);
    for(auto b:version)assert(b==0xa5);
    const struct {unsigned type;size_t bytes;} cases[]={{12,13},{13,4},{14,12},{15,4},{19,24},{27,0x12d8},{51,3}};
    for(auto c:cases) {
        auto input=envelope(false,c.type,gwi_scalar(c.type)?0:c.bytes);std::vector<uint8_t> out(c.bytes+16,0xa5);size_t n=out.size();
        assert(f.call(input,out.data(),n)==0 && n==c.bytes);
        for(size_t i=c.bytes;i<out.size();i++)assert(out[i]==0xa5);
        n=c.bytes-1;memset(out.data(),0xa5,out.size());
        assert(f.call(input,out.data(),n)!=0 && n==0);
        for(auto b:out)assert(b==0xa5);
        gwq_w32(input.data()+4,(uint32_t)c.bytes+1);n=out.size();
        assert(f.call(input,out.data(),n)==kIOReturnBadArgument && n==0);
    }
    auto input=envelope(false,999,24);uint8_t out[32];memset(out,0xa5,sizeof(out));size_t n=sizeof(out);
    assert(f.call(input,out,n)==kIOReturnUnsupported && n==0);
    for(auto b:out)assert(b==0xa5);
    input=envelope(false,19,24);n=24;
    failAllocation=allocationCalls+1;assert(f.call(input,out,n)==kIOReturnNoMemory);failAllocation=0;
    n=24;failAllocation=allocationCalls+2;assert(f.call(input,out,n)==kIOReturnNoMemory);failAllocation=0;
    gwi_request request={0,19,0,input.data()+8,24};gwi_cursor cursor={};
    assert(f.controller.query(request,out,nullptr,&cursor)==kIOReturnBadArgument);
    n=24;assert(f.controller.query(request,out,&n,nullptr)==kIOReturnBadArgument);
    n=24;assert(f.controller.query(request,nullptr,&n,&cursor)==kIOReturnBadArgument && n==0);
}

static void descriptor_io() {
    Fixture f;auto input=envelope(false,19,24);
    IOMemoryDescriptor in(input.size()),out(32);in.bytes=input;memset(out.bytes.data(),0xa5,out.bytes.size());
    IOExternalMethodArguments a;a.structureInputDescriptor=&in;a.structureOutputDescriptor=&out;
    assert(f.client.externalMethod(0,&a,nullptr,nullptr,nullptr)==0);
    assert(a.structureOutputSize==24 && gwq_u32(out.bytes.data()+8)==1 && out.bytes[24]==0xa5);
    in.shortRead=true;
    assert(f.client.externalMethod(0,&a,nullptr,nullptr,nullptr)==kIOReturnBadArgument && allocations.empty());
    in.shortRead=false;out.shortWrite=true;
    assert(f.client.externalMethod(0,&a,nullptr,nullptr,nullptr)==kIOReturnBadArgument && allocations.empty());
    out.shortWrite=false;out.bytes.resize(0x3c001);
    assert(f.client.externalMethod(0,&a,nullptr,nullptr,nullptr)==kIOReturnBadArgument && allocations.empty());
    f.runtime.wmi.bss_count=2;
    for(unsigned i=0;i<2;i++) {
        auto &b=f.runtime.wmi.bss[i];b.frequency=2412;b.ssid_len=1;b.ssid[0]='A'+i;
    }
    in.bytes=envelope(false,11,GWI_SCAN_RESULT_BYTES);out.bytes.resize(GWI_SCAN_RESULT_BYTES);
    out.shortWrite=true;
    assert(f.client.externalMethod(0,&a,nullptr,nullptr,nullptr)==kIOReturnBadArgument && a.structureOutputSize==0);
    out.shortWrite=false;
    assert(f.client.externalMethod(0,&a,nullptr,nullptr,nullptr)==0 && out.bytes[0x61]=='A');
}

static void scan_and_power() {
    Fixture f;auto scan=envelope(true,10,GWI_SCAN_REQUEST_BYTES);gwq_w32(scan.data()+8,1);
    std::vector<uint8_t> echo(GWI_SCAN_REQUEST_BYTES+16,0xa5);size_t n=GWI_SCAN_REQUEST_BYTES-1;
    assert(f.call(scan,echo.data(),n)!=0 && n==0 && !f.runtime.scan_pending);
    n=echo.size();assert(f.call(scan,echo.data(),n)==0 && n==GWI_SCAN_REQUEST_BYTES && f.runtime.scan_pending);
    assert(!memcmp(echo.data(),scan.data()+8,n) && echo[n]==0xa5);
    n=echo.size();assert(f.call(scan,echo.data(),n)==kIOReturnBusy && n==0);
    auto power=envelope(true,19,24);gwq_w32(power.data()+8,1);gwq_w32(power.data()+12,1);
    n=24;assert(f.call(power,echo.data(),n)==kIOReturnBusy && f.runtime.powered);
    f.runtime.scan_pending=0;n=0;assert(f.call(power,nullptr,n)==0 && !f.runtime.powered);
    n=0;assert(f.call(scan,nullptr,n)==kIOReturnNotReady && !f.runtime.scan_pending);
    gwq_w32(power.data()+16,1);n=24;assert(f.call(power,echo.data(),n)==0 && f.runtime.powered);
    gwq_w32(power.data()+16,2);n=24;assert(f.call(power,echo.data(),n)==kIOReturnBadArgument && f.runtime.powered);
    f.runtime.wmi.bss_count=2;
    for(unsigned i=0;i<2;i++) {
        auto &b=f.runtime.wmi.bss[i];b.frequency=i?5180:2412;b.ssid_len=1;b.ssid[0]='A'+i;b.timestamp=900;
    }
    auto get=envelope(false,11,GWI_SCAN_RESULT_BYTES);n=echo.size();
    assert(f.call(get,echo.data(),n)==0 && n==GWI_SCAN_RESULT_BYTES && echo[0x61]=='A');
    n=echo.size();assert(f.call(get,echo.data(),n)==0 && echo[0x61]=='B');
    n=echo.size();assert(f.call(get,echo.data(),n)==0xe0820446u && n==0);
    n=0;assert(f.call(scan,nullptr,n)==0);f.runtime.scan_pending=0;
    n=echo.size();assert(f.call(get,echo.data(),n)==0 && echo[0x61]=='A');
    f.runtime.wmi.scanning=1;n=echo.size();assert(f.call(get,echo.data(),n)==kIOReturnBusy && n==0);
}

static void mapping_ownership_and_boundaries() {
    giulia_wifi_runtime r={};r.pci.bar_base=0x60400000;r.pci.bar_bytes=0x200000;
    for(unsigned id=0;id<GWMAP_REGIONS;id++) {
        uint64_t physical=0;size_t bytes=0;assert(gwmap_resource(id,r.pci.bar_base,r.pci.bar_bytes,&physical,&bytes));
        auto address=gw_map_region(&r,id);assert(address && r.mmio_memory[id]->physical==physical);
        assert(r.mmio_maps[id]->getLength()==bytes);
        unsigned calls=descriptorCalls;assert(gw_map_region(&r,id)==address && descriptorCalls==calls);
        gw_write_region(&r,id,0,0xaabbccdd);gw_write_region(&r,id,bytes-4,0x12345678);
        assert(gw_read_region(&r,id,0)==0xaabbccdd && gw_read_region(&r,id,bytes-4)==0x12345678);
        for(size_t offset:{size_t(1),bytes-3,bytes,SIZE_MAX}) {
            r.mapping_error=0;assert(gw_read_region(&r,id,offset)==UINT32_MAX && r.mapping_error);
            gw_write_region(&r,id,offset,0);assert(gw_read_region(&r,id,0)==0xaabbccdd);
        }
    }
    assert(maps==GWMAP_REGIONS && descriptors==GWMAP_REGIONS);
    r.mapping_error=0;assert(!gw_map_region(&r,GWMAP_REGIONS));
    assert(gw_read_region(&r,GWMAP_REGIONS,0)==UINT32_MAX && r.mapping_error);
    assert(!gw_map_region(nullptr,0));
    r.root=r.config=r.parf=r.smmu=r.bar=1;for(auto &a:r.link_maps)a=1;
    releases.clear();gw_unmap_regions(&r);
    assert(!maps && !descriptors && !r.root && !r.config && !r.parf && !r.smmu && !r.bar);
    for(auto a:r.link_maps)assert(!a);
    assert(releases.size()==2*GWMAP_REGIONS);
    for(unsigned i=0;i<GWMAP_REGIONS;i++) {
        uint64_t physical=0;size_t bytes=0;assert(gwmap_resource(GWMAP_REGIONS-1-i,r.pci.bar_base,r.pci.bar_bytes,&physical,&bytes));
        assert(releases[2*i]==std::make_pair('m',physical));
        assert(releases[2*i+1]==std::make_pair('d',physical));
    }
    auto count=releases.size();gw_unmap_regions(&r);assert(releases.size()==count);
    for(unsigned fail=0;fail<GWMAP_REGIONS;fail++) {
        failDescriptor=descriptorCalls+fail+1;
        for(unsigned id=0;id<=fail;id++)assert(bool(gw_map_region(&r,id))==(id<fail));
        failDescriptor=0;gw_shutdown(&r);assert(!maps && !descriptors);
        failMap=mapCalls+fail+1;
        for(unsigned id=0;id<=fail;id++)assert(bool(gw_map_region(&r,id))==(id<fail));
        failMap=0;gw_shutdown(&r);assert(!maps && !descriptors);
    }
    for(badMapping=1;badMapping<=3;badMapping++) {
        assert(!gw_map_region(&r,GWMAP_ROOT));assert(!maps && !descriptors && !r.mmio_maps[GWMAP_ROOT] && !r.mmio_memory[GWMAP_ROOT]);
    }
    badMapping=0;
    for(unsigned id:{unsigned(GWMAP_ROOT),unsigned(GWMAP_QMP),unsigned(GWMAP_IPCC)})assert(gw_map_region(&r,id));
    r.qmp_retained=1;gw_shutdown(&r);
    assert(maps==2 && descriptors==2 && r.mmio_maps[GWMAP_QMP] && r.mmio_maps[GWMAP_IPCC] && !r.mmio_maps[GWMAP_ROOT]);
    r.qmp_retained=0;gw_shutdown(&r);assert(!maps && !descriptors);
}

static void allocation_and_shutdown() {
    for(unsigned failure=0;failure<=6;failure++) {
        giulia_wifi_runtime r={};
        if(failure==1 || failure==2)failBuffer=bufferCalls+failure;
        badBuffer=failure>=3?failure-2:0;
        bool ok=gw_allocate(&r);assert(ok==(failure==0));
        if(ok)assert(r.memory && r.arena && r.tables && r.dma.cpu==(uintptr_t)r.memory && r.dma.allocated==GW_ALLOC_START);
        failBuffer=badBuffer=0;gw_shutdown(&r);
        assert(!buffers && !r.arena && !r.tables && !r.memory && !r.dma.cpu);
    }
    // Uncertain DMA retains both physical backing and MMIO; safe shutdown
    // subsequently tears them down in the same production path.
    for(unsigned reason=0;reason<2;reason++) {
        giulia_wifi_runtime r={};assert(gw_allocate(&r));assert(gw_map_region(&r,GWMAP_ROOT));
        r.quarantine=reason==0;r.mhi.quarantined=reason==1;gw_shutdown(&r);
        assert(buffers==2 && maps==1 && descriptors==1 && r.memory && r.dma.cpu);
        r.quarantine=r.mhi.quarantined=0;gw_shutdown(&r);assert(!buffers && !maps && !descriptors);
    }
    // Provider destruction reclaims failed startup allocations; asynchronous
    // users/QMP/DMA keep the runtime alive until their ownership is resolved.
    for(unsigned reason=0;reason<6;reason++) {
        IO80211Controller c;c.r=(giulia_wifi_runtime *)IOMallocZeroData(sizeof(*c.r));
        c.r->lock=IOLockAlloc();assert(gw_map_region(c.r,GWMAP_ROOT));
        if(reason==1)c.r->quarantine=1;
        if(reason==2)c.r->mhi.quarantined=1;
        if(reason==3)c.r->interface=(void *)1;
        if(reason==4)c.r->qmp_retained=1;
        if(reason==5)c.r->translation.quarantined=1;
        c.free();assert(bool(c.r)==bool(reason));
        if(reason) {
            assert(maps==1 && descriptors==1);c.r->quarantine=c.r->mhi.quarantined=c.r->qmp_retained=c.r->translation.quarantined=0;c.r->interface=nullptr;c.free();
        }
        assert(!c.r && allocations.empty() && !maps && !descriptors);
    }
}

static void mhi_aperture() {
    giulia_wifi_runtime r={};r.pci.bar_base=0x60400000;r.pci.bar_bytes=0x200000;
    assert(gw_map_region(&r,GWMAP_BAR));
    for(size_t offset:{size_t(0),size_t(GWMAP_BAR_DIRECT_BYTES-4)}) {
        gm_write(&r,(uint32_t)offset,0x12345678);
        assert(!r.mapping_error && gm_read(&r,(uint32_t)offset)==0x12345678);
    }
    gw_write_region(&r,GWMAP_BAR,GWMAP_BAR_DIRECT_BYTES,0xabcdef01);
    for(uint32_t offset:{uint32_t(1),GWMAP_BAR_DIRECT_BYTES-3,GWMAP_BAR_DIRECT_BYTES,UINT32_MAX}) {
        r.mapping_error=0;assert(gm_read(&r,offset)==UINT32_MAX && r.mapping_error);
        r.mapping_error=0;gm_write(&r,offset,0);assert(r.mapping_error);
        assert(gw_read_region(&r,GWMAP_BAR,GWMAP_BAR_DIRECT_BYTES)==0xabcdef01);
    }
    gw_shutdown(&r);assert(!maps && !descriptors);
}

static void service_lifecycle() {
    Fixture f;f.runtime.owner=&f.controller;f.runtime.service=new IO80211Interface;
    FakeIfnet interface{&f.runtime};f.runtime.interface=&interface;
    gw_unpublish(&f.runtime);
    assert(!f.runtime.service && serviceTerminations==1 && serviceDetaches==1 && serviceReleases==1);
    assert(interface.detachCalls==1 && !interface.releaseCalls && f.runtime.interface==&interface);
    // Detach completion may arrive later and owns exactly one provider release.
    gn_detached(&interface);
    assert(!f.runtime.interface && interface.releaseCalls==1 && f.controller.releaseCalls==1);
    gw_unpublish(&f.runtime);assert(interface.detachCalls==1 && serviceReleases==1);
    f.runtime.worker_running=1;finishingWorker=&f.runtime;
    f.controller.stop(nullptr);
    assert(f.runtime.stopping && !f.runtime.worker_running && sleepCalls==1 && f.controller.stopCalls==1);
    finishingWorker=nullptr;
    f.controller.stop(nullptr);assert(sleepCalls==1 && f.controller.stopCalls==2);
}

static void concurrent_client_requests() {
    Fixture f;f.runtime.wmi.bss_count=2;
    for(unsigned i=0;i<2;i++) {
        auto &b=f.runtime.wmi.bss[i];b.frequency=2412;b.ssid_len=1;b.ssid[0]='A'+i;
    }
    auto input=envelope(false,11,GWI_SCAN_RESULT_BYTES);
    IOMemoryDescriptor first(GWI_SCAN_RESULT_BYTES),second(GWI_SCAN_RESULT_BYTES);
    std::atomic<bool> entered{false},allow{false},secondFinished{false};
    first.writeEntered=&entered;first.allowWrite=&allow;
    IOExternalMethodArguments a,b;a.structureInput=b.structureInput=input.data();
    a.structureInputSize=b.structureInputSize=input.size();
    a.structureOutputDescriptor=&first;b.structureOutputDescriptor=&second;
    IOReturn r1=kIOReturnError,r2=kIOReturnError;
    unsigned priorWaits=lockWaits.load();
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    std::thread one([&]{r1=f.client.externalMethod(0,&a,nullptr,nullptr,nullptr);});
    while(!entered.load() && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    assert(entered.load());
    std::thread two([&]{r2=f.client.externalMethod(0,&b,nullptr,nullptr,nullptr);secondFinished=true;});
    while(lockWaits.load()==priorWaits && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    assert(lockWaits.load()>priorWaits && !secondFinished.load());
    allow=true;one.join();two.join();
    assert(r1==0 && r2==0 && a.structureOutputSize==GWI_SCAN_RESULT_BYTES && b.structureOutputSize==GWI_SCAN_RESULT_BYTES);
    assert(first.bytes[0x61]=='A' && second.bytes[0x61]=='B');
}

#include "ios16_wifi_network_runtime.inc"
#include "ios16_wifi_network_rsn.inc"
#include "ios16_wifi_network_security.inc"
#include "ios16_wifi_network_session.inc"
#include "ios16_wifi_network_traffic.inc"

int main() {
    mapping_ownership_and_boundaries();allocation_and_shutdown();
    mhi_aperture();
    service_lifecycle();
    selectors_and_validation();query_lengths_and_failures();descriptor_io();scan_and_power();
    concurrent_client_requests();
    network_dma_mapping();network_queries();network_packets();network_shutdown();network_ioctls();network_extended_queries();network_native_abi();
    network_rsn_bridge();
    network_security_bridge();
    network_session_paths();
    network_traffic_bridge();
    puts("Actual Wi-Fi runtime passed: mapping/lifecycle/query/concurrency, station sessions, BSD TX/RX, EAPOL isolation, RSN/key-ACK/TCL, SAE/PMF and secret/queue teardown");
}
