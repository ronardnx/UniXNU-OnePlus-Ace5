"""Exercise actual Wi-Fi mapping and audio registration against the IPC broker.

The production methods are extracted unchanged; only IOKit allocations are
host doubles. Both build gates are compiled. No physical device is accessed.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
wifi = (ROOT / 'ios16/GiuliaWiFi.cpp').read_text()
audio = (ROOT / 'ios16/GiuliaAudio.cpp').read_text()


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


methods = ''.join(function(wifi, signature) for signature in (
    'static vm_offset_t gw_map_region(', 'static void gw_unmap_regions(',
    'static int gqp_claim(', 'static int gqp_owns(', 'static void gqp_release(',
))
methods += function(audio, 'extern "C" int giulia_audio_platform_register(')

fixture = r'''
#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>
#include "giulia_wifi_mapping.h"
#include "giulia_audio_platform.h"
#include "giulia_gpu_ipcc_owner.h"
using vm_offset_t=uintptr_t;
enum { kIODirectionInOut=3,kIOMapInhibitCache=0x200,GWL_REGIONS=3 };
static struct ggi_owner broker={};
static const struct ga_platform *audioPlatform;
static unsigned audioRegistering;
static std::atomic<unsigned> audioClaims{0},audioReleases{0};
static const void *wifiToken;
#if GIULIA_GPU_QMP_GATE || GIULIA_GPU_IPCC_GATE
static std::atomic<bool> holdAudio{false},audioEntered{false},resumeAudio{false};
extern "C" int giulia_gpu_ipcc_claim(unsigned client,const void *owner) {
    int result=ggi_claim(&broker,client,owner);
    if(client==GGI_HOST_AUDIO) {
        ++audioClaims;
        if(result && holdAudio.load()) {
            audioEntered.store(true);
            while(!resumeAudio.load())std::this_thread::yield();
        }
    }
    return result;
}
extern "C" int giulia_gpu_ipcc_owns(unsigned client,const void *owner)
{return ggi_owns(&broker,client,owner);}
extern "C" int giulia_gpu_ipcc_release(unsigned client,const void *owner) {
    if(client==GGI_HOST_AUDIO)++audioReleases;
    return ggi_release(&broker,client,owner);
}
#endif
static unsigned liveMaps,liveMemory,descriptorCalls,badMapping;
static bool failDescriptor;
static uint64_t ipccPhysical() {
    uint64_t physical=0;size_t bytes=0;
    assert(gwmap_resource(GWMAP_IPCC,0,0,&physical,&bytes));return physical;
}
static void checkOwned(uint64_t physical) {
#if GIULIA_GPU_QMP_GATE
    if(physical==ipccPhysical())assert(ggi_owns(&broker,GGI_HOST_QMP,wifiToken));
#else
    (void)physical;
#endif
}
struct IOMemoryDescriptor;
struct IOMemoryMap {
    IOMemoryDescriptor *memory;
    uintptr_t address;size_t bytes;
    uintptr_t getVirtualAddress(){return address;}
    size_t getLength(){return bytes;}
    void release();
};
struct IOMemoryDescriptor {
    std::vector<uint8_t> data;uint64_t physical;unsigned maps=0;
    IOMemoryDescriptor(uint64_t pa,size_t bytes):data(bytes),physical(pa){}
    static IOMemoryDescriptor *withPhysicalAddress(uint64_t pa,size_t bytes,unsigned options) {
        assert(options==kIODirectionInOut);checkOwned(pa);++descriptorCalls;
        if(failDescriptor)return nullptr;
        ++liveMemory;return new IOMemoryDescriptor(pa,bytes);
    }
    IOMemoryMap *map(unsigned options) {
        assert(options==kIOMapInhibitCache);checkOwned(physical);
        if(badMapping==4)return nullptr;
        ++maps;++liveMaps;
        uintptr_t address=badMapping==2?0:badMapping==3?UINTPTR_MAX-3:(uintptr_t)data.data();
        return new IOMemoryMap{this,address,data.size()-(badMapping==1)};
    }
    void release(){assert(!maps);checkOwned(physical);--liveMemory;delete this;}
};
void IOMemoryMap::release() {
    checkOwned(memory->physical);--memory->maps;--liveMaps;delete this;
}
#define OSSafeReleaseNULL(p) do{if(p){(p)->release();(p)=nullptr;}}while(0)
struct giulia_wifi_runtime {
    struct {uint64_t bar_base=0,bar_bytes=0;} pci;
    struct {unsigned quarantined=0;} mhi,translation;
    IOMemoryDescriptor *mmio_memory[GWMAP_REGIONS]={};
    IOMemoryMap *mmio_maps[GWMAP_REGIONS]={};
    unsigned quarantine=0,qmp_retained=0;
    vm_offset_t root=0,config=0,parf=0,smmu=0,bar=0,link_maps[GWL_REGIONS]={};
};
static struct {const void *lease;} gw_qmp;
#include "ipcc_clients_under_test.inc"

static struct ga_platform platform() {
    static const uint8_t sipa[1]={0};
    struct ga_platform p={};p.abi=GA_PLATFORM_ABI;p.bytes=sizeof(p);p.sipa=sipa;p.sipa_bytes=sizeof(sipa);
    p.ops.boot.verify_pack=[](void *,const void *,size_t){return 1;};
    p.ops.boot.prerequisites=[](void *){return 1;};
    p.ops.boot.metadata_alloc=[](void *,unsigned,const void *,size_t,uint64_t *){return 0;};
    p.ops.boot.metadata_release=[](void *,unsigned){};
    p.ops.boot.prepare=[](void *){return 0;};
    p.ops.boot.memory_write=[](void *,uint64_t,const void *,size_t){return 0;};
    p.ops.boot.memory_sync=[](void *,uint64_t,size_t){return 0;};
    p.ops.boot.pas=[](void *,unsigned,unsigned,uint64_t,uint64_t){return 0;};
    p.ops.boot.wait_ready=[](void *,unsigned){return 0;};p.ops.boot.unprepare=[](void *){};
    p.ops.speaker.verify_sha256=[](void *,const uint8_t *,size_t,const uint8_t *){return 0;};
    p.ops.speaker.read=[](void *,unsigned,uint8_t,uint16_t *){return 0;};
    p.ops.speaker.write=[](void *,unsigned,uint8_t,uint16_t){return 0;};
    p.ops.speaker.reset=[](void *,unsigned,unsigned){return 0;};
    p.ops.speaker.delay_us=[](void *,uint32_t){return 0;};
    p.ops.ipc_open=[](void *,struct gat_transport *){return 0;};p.ops.ipc_poll=p.ops.ipc_open;
    p.ops.delay_ms=[](void *,unsigned){return 0;};p.ops.pcm_available=[](void *){return 1;};
    p.ops.pcm_start=p.ops.ipc_open;p.ops.pcm_stop=p.ops.ipc_open;p.ops.ipc_quiesce=p.ops.ipc_open;
    p.ops.pcm_write=[](void *,struct gat_transport *,const void *,size_t){return 0;};
    assert(gar_ops_valid(&p.ops));return p;
}
static void wifiLifecycle() {
    giulia_wifi_runtime r;int gpu=0,qmp=0;wifiToken=&r;
    assert(gw_map_region(&r,GWMAP_IPCC));
    unsigned calls=descriptorCalls;
    assert(gw_map_region(&r,GWMAP_IPCC) && calls==descriptorCalls);
    assert(gqp_claim(&r,&qmp) && gqp_owns(&r,&qmp));
#if GIULIA_GPU_QMP_GATE
    // An unexpectedly lost membership cannot reuse a cached APSS mapping or
    // continue claiming QMP ownership; reacquire only for fixture teardown.
    assert(ggi_release(&broker,GGI_HOST_QMP,&r));
    assert(!gw_map_region(&r,GWMAP_IPCC) && !gqp_owns(&r,&qmp));
    assert(calls==descriptorCalls && !gqp_claim(&r,&qmp));
    assert(ggi_claim(&broker,GGI_HOST_QMP,&r));
#endif
    gqp_release(&r,&qmp);assert(!gqp_owns(&r,&qmp));
#if GIULIA_GPU_QMP_GATE
    // Channel close does not end the underlying APSS mapping lifetime.
    assert(ggi_owns(&broker,GGI_HOST_QMP,&r));assert(!ggi_claim(&broker,GGI_GPU,&gpu));
#endif
    for(unsigned retained=0;retained<4;retained++) {
        r.qmp_retained=retained==0;r.quarantine=retained==1;
        r.mhi.quarantined=retained==2;r.translation.quarantined=retained==3;
        gw_unmap_regions(&r);assert(r.mmio_maps[GWMAP_IPCC] && liveMaps==1 && liveMemory==1);
#if GIULIA_GPU_QMP_GATE
        assert(!ggi_claim(&broker,GGI_GPU,&gpu));
#endif
    }
    r.qmp_retained=r.quarantine=r.mhi.quarantined=r.translation.quarantined=0;
    // Cleanup must succeed even while another broker claim owns its gate.
    __atomic_store_n(&broker.gate,1u,__ATOMIC_RELEASE);
    gw_unmap_regions(&r);assert(!liveMaps && !liveMemory);
    assert(!ggi_owns(&broker,GGI_HOST_QMP,&r));
    __atomic_store_n(&broker.gate,0u,__ATOMIC_RELEASE);
    assert(ggi_claim(&broker,GGI_GPU,&gpu));
#if GIULIA_GPU_QMP_GATE
    calls=descriptorCalls;assert(!gw_map_region(&r,GWMAP_IPCC));assert(calls==descriptorCalls);
    assert(!gqp_claim(&r,&qmp));
#else
    assert(gw_map_region(&r,GWMAP_IPCC));gw_unmap_regions(&r);
#endif
    assert(ggi_release(&broker,GGI_GPU,&gpu));
}
static void wifiFailures() {
    giulia_wifi_runtime r;int gpu=0;wifiToken=&r;
    for(unsigned failure=0;failure<=4;failure++) {
        failDescriptor=failure==0;badMapping=failure;
        assert(!gw_map_region(&r,GWMAP_IPCC));assert(!liveMaps && !liveMemory);
        assert(!ggi_owns(&broker,GGI_HOST_QMP,&r));
        assert(ggi_claim(&broker,GGI_GPU,&gpu));assert(ggi_release(&broker,GGI_GPU,&gpu));
    }
    failDescriptor=false;badMapping=0;
}
static void audioLifecycle() {
    auto p=platform(),other=platform(),invalid=p;int gpu=0;
    invalid.ops.boot.prerequisites=nullptr;
    assert(!giulia_audio_platform_register(nullptr));assert(!giulia_audio_platform_register(&invalid));
    assert(!audioPlatform && audioClaims==0);
    assert(ggi_claim(&broker,GGI_GPU,&gpu));
#if GIULIA_GPU_IPCC_GATE
    assert(!giulia_audio_platform_register(&p));assert(!audioPlatform);
    assert(!ggi_owns(&broker,GGI_HOST_AUDIO,&p));assert(!audioRegistering);
#endif
    assert(ggi_release(&broker,GGI_GPU,&gpu));
    giulia_wifi_runtime r;wifiToken=&r;assert(gw_map_region(&r,GWMAP_IPCC));
#if GIULIA_GPU_IPCC_GATE
    holdAudio=true;unsigned result=0;
    std::thread publisher([&]{result=giulia_audio_platform_register(&p);});
    while(!audioEntered.load())std::this_thread::yield();
    // Reject concurrent same/different registration without releasing p.
    assert(!giulia_audio_platform_register(&p));assert(!giulia_audio_platform_register(&other));
    assert(ggi_owns(&broker,GGI_HOST_AUDIO,&p));assert(!ggi_claim(&broker,GGI_GPU,&gpu));
    resumeAudio=true;publisher.join();assert(result==1);holdAudio=false;
#else
    assert(giulia_audio_platform_register(&p));assert(!audioClaims);
#endif
    assert(audioPlatform==&p && !audioRegistering);
    auto releases=audioReleases.load();
    assert(!giulia_audio_platform_register(&p));assert(!giulia_audio_platform_register(&other));
    assert(audioReleases==releases && audioPlatform==&p);
    gw_unmap_regions(&r);assert(!liveMaps && !liveMemory);
#if GIULIA_GPU_IPCC_GATE
    assert(ggi_owns(&broker,GGI_HOST_AUDIO,&p));assert(!ggi_claim(&broker,GGI_GPU,&gpu));
    // Test-process teardown only; production retains this membership to reboot.
    assert(ggi_release(&broker,GGI_HOST_AUDIO,&p));
#endif
    audioPlatform=nullptr;
}
int main() {
    wifiLifecycle();wifiFailures();audioLifecycle();
    puts("IPCC clients: production map/unmap and registration lifecycle passed");
}
'''

with tempfile.TemporaryDirectory(prefix='gpu-ipcc-clients-') as folder:
    path = Path(folder)
    (path / 'ipcc_clients_under_test.inc').write_text(methods)
    source = path / 'clients.cpp'
    source.write_text(fixture)
    for gate in (0, 1):
        binary = path / f'clients-{gate}'
        subprocess.run([
            os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-function', '-Wno-missing-field-initializers',
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
            '-pthread', f'-DGIULIA_GPU_QMP_GATE={gate}', f'-DGIULIA_GPU_IPCC_GATE={gate}',
            '-I', str(ROOT / 'ios16'), '-I', str(path), str(source), '-o', str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
