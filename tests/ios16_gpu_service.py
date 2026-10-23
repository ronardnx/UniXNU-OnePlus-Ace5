"""Run the production terminal worker/stop methods against host lock fixtures.

This verifies CPU lifetime/order only. The observation stub cannot validate
register accessibility, cache behavior, bus errors or device power timing.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'ios16/GiuliaGPU.cpp').read_text()


def method(signature):
    start=source.index(signature)
    return source[start:source.index('\n}',start)+2]


fixture=r'''
#include <atomic>
#include <cstring>
#include "giulia_gpu_dev.h"
static unsigned devState;static uint32_t devArgs[6];
static void PE_parse_boot_argn(const char *,int *v,size_t){*v=1;}
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "giulia_gpu_terminal.h"
#include "giulia_gpu_power.h"
#include "giulia_gpu_power_handoff.h"
#include "giulia_gpu_hfi.h"
static void gpuHfiBarrier(void *){__sync_synchronize();}
static int giulia_gpu_qmp_available(){return 1;}
static std::mutex logsLock;
static std::vector<std::string> logs;
static std::atomic<unsigned> statuses{0};
static void IOLog(const char *format,...) {
    char line[2048];va_list args;va_start(args,format);
    vsnprintf(line,sizeof(line),format,args);va_end(args);
    std::lock_guard<std::mutex> guard(logsLock);logs.emplace_back(line);
    if(logs.back().find("command=gpu.status stage=")!=std::string::npos)++statuses;
}
struct IOLock { std::mutex lock;std::condition_variable wake; };
static void IOLockLock(IOLock *p){p->lock.lock();}
static void IOLockUnlock(IOLock *p){p->lock.unlock();}
static void IOLockWakeup(IOLock *p,void *,bool){p->wake.notify_all();}
static void IOLockSleep(IOLock *p,void *,int) {
    std::unique_lock<std::mutex> lock(p->lock,std::adopt_lock);
    p->wake.wait(lock);lock.release();
}
static void IOSleep(unsigned){std::this_thread::sleep_for(std::chrono::milliseconds(1));}
#define THREAD_UNINT 0
using wait_result_t=int;
static void *current_thread(){return nullptr;}
static void thread_terminate(void *){}
struct IOService {void stop(IOService *){}};
static struct gg_terminal_mailbox gpuTerminal;
class GiuliaGPU:public IOService {
public:
    IOLock lock;
    IOLock *runtimeLock=&lock;
    bool workerRunning=true,stopping=false,terminalOwner=true;
    unsigned mode=1,reservationCount=5,tablePages=6,leafPages=104,observations=0;
    bool firmwareVerified=true,hostPrepared=true;
    bool manualMemoryPinned=false,platformPoisoned=false,fenceMapped=false;
    int auditFenceMappings(){platformPoisoned=true;return 8;}
    unsigned hfiIndex=0,hfiSequence=0,hfiLastHeader=0;
    struct Memory {uint32_t words[GGH_BYTES/4]={};void *getBytesNoCopy(){return words;}} memory,logMemory;
    Memory *allocations[3]={&memory,&logMemory,&logMemory};
    ggp_report powerObservation={};
    gpp_report firmwarePower={};unsigned firmwarePowerState=GGPH_INVALID;
    std::atomic<unsigned> references{2},inObservation{0},released{0};
    std::atomic<bool> entered{false},allowObservation{false};
    void retain(){++references;}
    void release(){assert(references.fetch_sub(1)>=2);}
    void observePower() {
        assert(!released);++inObservation;entered=true;
        while(!allowObservation.load())std::this_thread::yield();
        powerObservation={};powerObservation.stage=GGP_CFG_OFF;
        powerObservation.valid_fields=7;++observations;--inObservation;
    }
    void releaseResources(){assert(!inObservation);if(!manualMemoryPinned)++released;}
    void devCommand(const uint32_t *){}
    void hfiCommand(unsigned);
    void reportStatus();void closeTerminal(unsigned);void stop(IOService *);
    static void worker(void *,wait_result_t);
};
template<class F> static void until(F condition) {
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!condition()) {
        assert(std::chrono::steady_clock::now()<deadline);
        std::this_thread::yield();
    }
}
'''
for signature in ('extern "C" int giulia_gpu_terminal(',
                  'void GiuliaGPU::reportStatus(',
                  'void GiuliaGPU::hfiCommand(',
                  'void GiuliaGPU::worker(',
                  'void GiuliaGPU::closeTerminal(',
                  'void GiuliaGPU::stop('):
    fixture+='\n'+method(signature)+'\n'
fixture+=r'''
int main() {
    assert(giulia_gpu_terminal("gpu.start")==0);
    assert(giulia_gpu_terminal("gpu.status")==1);
    GiuliaGPU first;
    assert(gg_terminal_open(&gpuTerminal));
    std::thread worker(GiuliaGPU::worker,&first,0);
    until([]{return __atomic_load_n(&gpuTerminal.stage,__ATOMIC_ACQUIRE)==GGT_READY;});
    assert(giulia_gpu_terminal("gpu.status")==1);
    until([]{return statuses.load()>=1;});
    assert(!first.entered); /* mode 1/status have no observation side effect. */
    assert(giulia_gpu_terminal("gpu.observe")==1);
    until([&]{return first.entered.load();});
    std::atomic<bool> stopped{false};
    std::thread stopper([&]{first.stop(nullptr);stopped=true;});
    for(unsigned i=0;i<100;++i)assert(giulia_gpu_terminal("gpu.status")==1);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(!stopped && !first.released && first.references==2);
    first.allowObservation=true;
    stopper.join();worker.join();
    assert(first.released==1 && first.references==1 && !first.workerRunning);
    assert(first.observations==1 && first.powerObservation.stage==GGP_CFG_OFF);
    assert(!gg_terminal_take(&gpuTerminal));
    assert(__atomic_load_n(&gpuTerminal.stage,__ATOMIC_ACQUIRE)==GGT_STOPPED);
    assert(giulia_gpu_terminal("gpu.observe")==1);
    assert(!gg_terminal_take(&gpuTerminal));
    GiuliaGPU second;second.mode=2;second.allowObservation=true;
    assert(gg_terminal_open(&gpuTerminal));
    std::thread next(GiuliaGPU::worker,&second,0);
    until([&]{return second.entered.load();});
    assert(giulia_gpu_terminal("gpu.status")==1);
    second.stop(nullptr);next.join();
    assert(second.observations==1 && second.released==1 && second.references==1);
    GiuliaGPU third;third.mode=3;third.workerRunning=false;
    assert(!ggh_init(third.memory.words,GGH_BYTES,GGH_IOVA));
    third.hfiCommand(GGT_HFI_QUERY);assert(!third.hfiLastHeader && !third.memory.words[17]);
    third.hfiCommand(GGT_HFI_PIN);third.hfiCommand(GGT_HFI_PIN);
    assert(third.references==3 && third.manualMemoryPinned);
    third.hfiCommand(GGT_HFI_QUERY|GGT_HFI_STATUS|GGT_HFI_LOG);
    assert(third.hfiLastHeader==0x10030c && third.memory.words[17]==4);
    uint32_t *rx=third.memory.words+GGH_RING_OFFSET/4+1024;
    rx[0]=0x1040c;rx[1]=third.hfiLastHeader;rx[2]=1;rx[3]=0;
    third.memory.words[6+12+11]=4;
    third.hfiCommand(GGT_HFI_POLL);assert(third.memory.words[6+12+10]==4);
    assert(!third.hfiLastHeader);
    // A matching value with error status, or a legacy reply with no status,
    // proves receipt only. Neither may be reported as an accepted command.
    for(unsigned words=3;words<=4;++words) {
        third.hfiCommand(GGT_HFI_QUERY);
        unsigned offset=third.memory.words[6+12+11];
        rx[offset]=(1u<<16)|(words<<8)|12;
        rx[offset+1]=third.hfiLastHeader;rx[offset+2]=1;rx[offset+3]=8;
        third.memory.words[6+12+11]=offset+4;
        third.hfiCommand(GGT_HFI_POLL);assert(!third.hfiLastHeader);
    }
    std::vector<uint32_t> saved(third.memory.words,third.memory.words+GGH_BYTES/4);
    unsigned sequence=third.hfiSequence;third.fenceMapped=true;
    third.hfiCommand(GGT_HFI_PIN|GGT_HFI_QUERY|GGT_HFI_POLL);
    assert(third.platformPoisoned && third.hfiSequence==sequence && third.references==3 &&
        !memcmp(saved.data(),third.memory.words,GGH_BYTES));
    assert(gg_terminal_open(&gpuTerminal));third.stop(nullptr);third.release();
    assert(!third.released && third.references==2); // Manual DMA ownership retains RAM until reset.
    bool sawStatus=false,sawUnavailable=false,sawInactive=false,sawAck=false;
    unsigned rejectedAcks=0;
    // Exercise the recorded failure and worst-width values under the target's
    // actual IOLog bound, including readiness records after the status line.
    second.firmwarePowerState=GGPH_RECORDED;
    second.firmwarePower.flags=GPP_EXPECTED_VALID|GPP_OBSERVED_VALID|GPP_MASK_VALID;
    second.firmwarePower.expected=second.firmwarePower.observed=second.firmwarePower.mask=~0ULL;
    second.firmwarePower.firmware_status=second.firmwarePower.protocol_status=~0ULL;
    second.firmwarePower.valid_reads=0xffff;
    for(unsigned i=0;i<16;++i)second.firmwarePower.values[i]=~0u;
    second.reportStatus();
    for(const auto &line:logs) {
        assert(line.size()<256 && line.back()=='\n');
        if(line.find("command=gpu.status stage=")!=std::string::npos) {
            sawStatus=true;
        }
        if(line.find("power_acquired=0 smmu_active=0 gmu_started=0")!=std::string::npos) {
            assert(line.find("hardware_ready=0 acceleration=0 device_writes=0")!=std::string::npos);
            sawInactive=true;
        }
        if(line.find("service=unavailable")!=std::string::npos)sawUnavailable=true;
        if(line.find("query_ack=1 header=0x10030c value=0x1 status_present=1 status=0 accepted=1")!=std::string::npos)sawAck=true;
        if(line.find("query_ack=1")!=std::string::npos && line.find("accepted=0")!=std::string::npos)++rejectedAcks;
    }
    assert(sawStatus && sawUnavailable && sawInactive && sawAck && rejectedAcks==2);
    puts("Production GPU worker/status/stop: mode gating, queued requests, observation-stop ordering and retained lifetime passed");
}
'''
with tempfile.TemporaryDirectory() as folder:
    directory=Path(folder)
    test=directory/'service.cpp';test.write_text(fixture)
    binary=directory/'service'
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-pthread','-I',str(ROOT/'ios16'),
                    str(test),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
