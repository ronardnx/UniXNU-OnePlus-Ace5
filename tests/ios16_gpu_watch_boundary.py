"""Run the real snapshot/watch methods against MMIO/HFI doubles; no device."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "ios16/giulia_gpu_platform_impl.h").read_text()
methods = source[source.index("int GiuliaGPU::platformSnapshot("):
                 source.index("int GiuliaGPU::platformZap(")]
fixture = r'''
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include "giulia_gpu_hfi.h"
static std::vector<std::string> logs;
static void IOLog(const char *format,...) {
    char text[512];va_list args;va_start(args,format);
    int n=vsnprintf(text,sizeof(text),format,args);va_end(args);
    assert(n>=0 && n<256);logs.emplace_back(text);
}
static bool logged(const char *part) {
    for(auto &line:logs)if(line.find(part)!=std::string::npos)return true;
    return false;
}
static void gpuHfiBarrier(void *){__sync_synchronize();}
static void IOSleep(unsigned);
struct Map {std::vector<uint64_t> words=std::vector<uint64_t>(0x40000/8);
    uintptr_t getVirtualAddress(){return reinterpret_cast<uintptr_t>(words.data());}};
struct Allocation {std::vector<uint32_t> words=std::vector<uint32_t>(GGH_BYTES/4);
    void *getBytesNoCopy(){return words.data();}};
class GiuliaGPU {
public:
    bool platformPrepared=true,platformPoisoned=false,secureContextActive=true,devMappingsManaged=true;
    struct {bool init_succeeded=true;} scmState;
    unsigned platformSnapshotSequence=0,hfiIndex=0;
    uint32_t devPacket[255]={};bool devWritten[255]={};
    Allocation owned;Allocation *allocations[1]={&owned};
    Map smmu;Map *platformMaps[4]={nullptr,nullptr,nullptr,&smmu};
    bool powered=true,doorbell=false,failOnPostDispatchMMIO=true;
    unsigned sleepCount=0,replyAt=1,mode=0;
    std::vector<uint64_t> reads,writes;
    std::map<uint64_t,uint32_t> registers;
    GiuliaGPU() {
        assert(!ggh_init(owned.words.data(),GGH_BYTES,GGH_IOVA));
        devPacket[0]=0x91600383;devPacket[1]=0x70970001;devPacket[2]=0x8000000;
        devWritten[0]=devWritten[1]=devWritten[2]=true;
        for(unsigned bank:{0u,2u,3u})registers[0x3dc0058+bank*4096]=0x400;
    }
    bool platformPower(){assert(!doorbell || !failOnPostDispatchMMIO);return powered;}
    uint32_t platformRead(uint64_t address) {
        assert(!doorbell || !failOnPostDispatchMMIO);
        reads.push_back(address);return registers[address];
    }
    void platformWrite(uint64_t address,uint32_t value) {
        assert(!doorbell && address==0x3d7e650 && value==1);
        assert(logged("stage=queue_committed") && logged("stage=doorbell_before"));
        writes.push_back(address);doorbell=true;
    }
    uint32_t *qh(unsigned queue){return owned.words.data()+GGH_HEADER_WORDS+queue*GGH_QUEUE_HEADER_WORDS;}
    uint32_t *ring(unsigned queue){return owned.words.data()+GGH_RING_OFFSET/4+queue*GGH_RING_WORDS;}
    void tick() {
        ++sleepCount;
        assert(doorbell);
        if(sleepCount!=replyAt)return;
        if(mode==1 || mode==2) {
            ring(1)[0]=mode==1?0x91610383:0x000b14;
            ring(1)[1]=devPacket[0];ring(1)[2]=0;
            qh(1)[11]=4;
        } else if(mode==3) qh(1)[11]=1; // Misaligned corrupted queue index.
    }
    int platformSnapshot(unsigned);
    int platformWatch(unsigned,unsigned);
};
static GiuliaGPU *current;
static void IOSleep(unsigned ms){assert(ms==1 && current);current->tick();}
'''+methods+r'''
static void reset(GiuliaGPU &gpu){logs.clear();current=&gpu;}
static void assert_no_snapshot(){assert(!logged("GPU SNAPSHOT:"));}
static void watch_arrival(unsigned mode) {
    GiuliaGPU gpu;reset(gpu);gpu.mode=mode;
    assert(gpu.platformWatch(3,5)==0 && !gpu.platformPoisoned);
    assert(gpu.reads==std::vector<uint64_t>({0x3d7e340,0x3d8ec40}));
    assert(gpu.writes==std::vector<uint64_t>({0x3d7e650}));
    assert(gpu.qh(0)[11]==4 && gpu.qh(1)[10]==0 && gpu.qh(1)[11]==4);
    assert(!gpu.devWritten[0] && !gpu.devWritten[1] && !gpu.devWritten[2]);
    assert(logged("outcome=packet_arrived queue_consumed=0 ack_verified=0 host_validation_required=1"));
    assert(!logged("ack_verified=1"));assert_no_snapshot();
}
int main() {
    watch_arrival(1); // A plausible matching ACK remains unconsumed/unvalidated.
    watch_arrival(2); // A request/other packet is not falsely claimed to be an ACK.
    {
        GiuliaGPU gpu;reset(gpu);
        assert(gpu.platformWatch(3,3)==8 && gpu.platformPoisoned && gpu.sleepCount==3);
        assert(logged("outcome=timeout quarantined=1 ack_verified=0 automatic_mmio=0"));
        assert(gpu.qh(0)[11]==4 && gpu.qh(1)[10]==0);assert_no_snapshot();
        auto reads=gpu.reads.size(),writes=gpu.writes.size();
        assert(gpu.platformWatch(3,3)==4);
        assert(gpu.reads.size()==reads && gpu.writes.size()==writes);
    }
    {
        GiuliaGPU gpu;reset(gpu);gpu.mode=3;
        assert(gpu.platformWatch(3,5)==8 && gpu.platformPoisoned && gpu.sleepCount==1);
        assert(logged("outcome=queue_invalid"));assert_no_snapshot();
    }
    for(unsigned invalid=0;invalid<6;invalid++) {
        GiuliaGPU gpu;reset(gpu);
        if(invalid==0)gpu.powered=false;
        if(invalid==1)gpu.registers[0x3d7e340]=3;
        if(invalid==2)gpu.registers[0x3d8ec40]=1;
        if(invalid==3)gpu.devWritten[2]=false;
        if(invalid==4)gpu.qh(1)[11]=4;
        if(invalid==5)gpu.qh(0)[11]=4;
        unsigned prior=gpu.qh(0)[11];
        assert(gpu.platformWatch(3,5)!=0 && !gpu.doorbell);
        assert(gpu.qh(0)[11]==prior && gpu.writes.empty());assert_no_snapshot();
    }
    for(unsigned reason:{0u,1u,12u}) {
        GiuliaGPU gpu;reset(gpu);
        assert(gpu.platformSnapshot(reason)==0);
        assert(gpu.reads.size()==11 && gpu.writes.empty());
        assert(!logged("width=64") && !logged("address=0x3da140"));
        assert(logged("stage=complete gx_reads=0 irq_handler=0"));
        for(auto address:gpu.reads)assert(address>=0x3d7e340);
        for(unsigned bank:{0u,2u,3u}) {
            uint64_t base=0x3dc0000+4096*bank;
            for(auto offset:{0x60,0x64,0x68,0x6c})
                assert(std::find(gpu.reads.begin(),gpu.reads.end(),base+offset)==gpu.reads.end());
        }
    }
    for(unsigned bank:{0u,2u,3u}) {
        GiuliaGPU gpu;reset(gpu);
        uint64_t base=0x3dc0000+4096*bank;
        gpu.registers[base+0x58]=0x402;
        gpu.smmu.words[(base+0x60-0x3da0000)/8]=0x123456789abcdef0ull;
        assert(gpu.platformSnapshot(0)==0);
        assert(gpu.reads.size()==14 && gpu.writes.empty());
        assert(logged("width=64 value=0x123456789abcdef0"));
        assert(std::find(gpu.reads.begin(),gpu.reads.end(),base+0x60)==gpu.reads.end());
        assert(std::find(gpu.reads.begin(),gpu.reads.end(),base+0x64)==gpu.reads.end());
        for(auto address:{base+0x68,base+0x6c,(uint64_t)(0x3da1400+4*bank)})
            assert(std::find(gpu.reads.begin(),gpu.reads.end(),address)!=gpu.reads.end());
    }
    {
        GiuliaGPU gpu;reset(gpu);gpu.registers[0x3dc3058]=0xffffffff;
        assert(gpu.platformSnapshot(0)==8 && !logged("width=64"));
    }
    puts("Real watch/snapshot methods passed: private preflight, visible publication boundaries, RAM-only wait, quarantine, unconsumed packets, fault-gated FAR64, no GX reads");
}
'''
with tempfile.TemporaryDirectory(prefix="gpu-watch-boundary-") as temporary:
    directory = Path(temporary)
    (directory / "test.cpp").write_text(fixture)
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g",
                    "-I", str(ROOT / "ios16"), str(directory / "test.cpp"),
                    "-o", str(directory / "test")], check=True)
    subprocess.run([str(directory / "test")], check=True)
