"""Production audio orchestration and whole-service strict C++ host checks."""
from pathlib import Path as _ModulePath
import sys as _module_sys
_module_sys.path.insert(0, str(_ModulePath(__file__).resolve().parents[1] / "scripts"))
from ios16_sources import include_flags
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

import test_audio_pack as pack_fixtures
from test_audio_sipa import fixture as sipa_fixture

ROOT = Path(__file__).resolve().parents[1]


SERVICE_SHIM = r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <vector>
#include <string>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <thread>
#include <atomic>
#define ARM64_BOARD_CONFIG_SWEET 1
#define GIULIA_PLATFORM_BROKER_ENABLED 0
#define OSDeclareDefaultStructors(x)
#define OSDefineMetaClassAndStructors(x,y)
#define OSSafeReleaseNULL(p) do {if(p){p->release();p=nullptr;}}while(0)
#define THREAD_UNINT 0
#define KERN_SUCCESS 0
#define MH_MAGIC_64 0xfeedfacf
#define LC_SEGMENT_64 0x19
enum {kIODirectionIn=1,kIOMapReadOnly=2,kIOMapCopybackCache=4};
struct mach_header_64 {uint32_t magic,cpu,subtype,type,ncmds,sizeofcmds,flags,reserved;};
struct load_command {uint32_t cmd,cmdsize;};
struct segment_command_64 {uint32_t cmd,cmdsize;char segname[16];uint64_t vmaddr,vmsize,fileoff,filesize;int32_t maxprot,initprot;uint32_t nsects,flags;};
alignas(16) static unsigned char hostMach[1024];
static const mach_header_64 &_mh_execute_header=*(const mach_header_64 *)hostMach;
extern "C" {unsigned long gPhysBase=0x90000000,gPhysSize=0x1000000,gVirtBase=0xfffffe0007004000;}
static std::vector<unsigned char> blob;
static unsigned maps,releases,hardware,pollCalls;
static int audioArg=1,debugArg=1;
static std::mutex logMutex;static std::vector<std::string> logs;
static void IOLog(const char *f,...) {char b[2048];va_list a;va_start(a,f);vsnprintf(b,sizeof(b),f,a);va_end(a);std::lock_guard<std::mutex> l(logMutex);logs.emplace_back(b);}
static void PE_parse_boot_argn(const char *n,int *v,size_t) {*v=!strcmp(n,"giulia_audio")?audioArg:debugArg;}
struct OSDictionary {};
struct IOService {std::atomic<unsigned> references{1};virtual ~IOService()=default;virtual bool init(OSDictionary * =nullptr){return true;}virtual bool start(IOService *){return true;}virtual void stop(IOService *){}virtual void free(){}void registerService(){}void retain(){++references;}void release(){assert(references>1);--references;}};
struct IOLock {std::mutex mutex;std::condition_variable cv;};
static IOLock *IOLockAlloc(){return new IOLock;}
static void IOLockFree(IOLock *p){delete p;}
static void IOLockLock(IOLock *p){p->mutex.lock();}
static void IOLockUnlock(IOLock *p){p->mutex.unlock();}
static void IOLockSleep(IOLock *p,void *,int){std::unique_lock<std::mutex> l(p->mutex,std::adopt_lock);p->cv.wait(l);l.release();}
static void IOLockWakeup(IOLock *p,void *,bool){p->cv.notify_all();}
static void *IOMalloc(size_t n){return malloc(n);}
static void IOFree(void *p,size_t){free(p);}
static void IOSleep(unsigned n){std::this_thread::sleep_for(std::chrono::milliseconds(n));}
using wait_result_t=int;using thread_t=std::thread *;
static int kernel_thread_start(void(*f)(void *,wait_result_t),void *v,thread_t *t){*t=new std::thread([=]{f(v,0);});return KERN_SUCCESS;}
static void thread_deallocate(thread_t t){t->detach();delete t;}
static void *current_thread(){return nullptr;}static void thread_terminate(void *){}
struct IOMemoryMap {uintptr_t getVirtualAddress(){return (uintptr_t)blob.data();}void release(){++releases;delete this;}};
struct IOMemoryDescriptor {
 static IOMemoryDescriptor *withPhysicalAddress(uint64_t p,uint64_t n,unsigned direction){assert(p==gPhysBase+0x4000 && n==blob.size() && direction==kIODirectionIn);++maps;return new IOMemoryDescriptor;}
 IOMemoryMap *map(unsigned flags){assert(flags==(kIOMapReadOnly|kIOMapCopybackCache));return new IOMemoryMap;}
 void release(){++releases;delete this;}
};
'''

SERVICE_TEST = r'''
static void header() {
 memset(hostMach,0,sizeof(hostMach));mach_header_64 *h=(mach_header_64 *)hostMach;
 h->magic=MH_MAGIC_64;h->ncmds=1;h->sizeofcmds=72;
 segment_command_64 *s=(segment_command_64 *)(h+1);s->cmd=LC_SEGMENT_64;s->cmdsize=72;
 memcpy(s->segname,"__AUDIO_FW",10);s->vmaddr=gVirtBase+0x4000;s->vmsize=(blob.size()+0x3fff)&~0x3fffULL;
 s->filesize=blob.size();s->maxprot=s->initprot=1;
}
static void boot_epoch(){audioOwner=audioOnline=audioClosing=audioRequest=0;audioPlatform=nullptr;header();}
static ga_platform unavailable_provider() {
 ga_platform p={};p.abi=GA_PLATFORM_ABI;p.bytes=sizeof(p);p.sipa=blob.data();p.sipa_bytes=188;
 p.ops.boot.verify_pack=[](void *,const void *,size_t){return 1;};
 p.ops.boot.prerequisites=[](void *){return 0;};
 p.ops.boot.metadata_alloc=[](void *,unsigned,const void *,size_t,uint64_t *){++hardware;return -1;};
 p.ops.boot.metadata_release=[](void *,unsigned){++hardware;};p.ops.boot.prepare=[](void *){++hardware;return -1;};
 p.ops.boot.memory_write=[](void *,uint64_t,const void *,size_t){++hardware;return -1;};
 p.ops.boot.memory_sync=[](void *,uint64_t,size_t){++hardware;return -1;};
 p.ops.boot.pas=[](void *,unsigned,unsigned,uint64_t,uint64_t){++hardware;return -1;};
 p.ops.boot.wait_ready=[](void *,unsigned){++hardware;return -1;};p.ops.boot.unprepare=[](void *){++hardware;};
 p.ops.speaker.verify_sha256=[](void *,const uint8_t *,size_t,const uint8_t *){return -1;};
 p.ops.speaker.read=[](void *,unsigned,uint8_t,uint16_t *){++hardware;return -1;};
 p.ops.speaker.write=[](void *,unsigned,uint8_t,uint16_t){++hardware;return -1;};
 p.ops.speaker.reset=[](void *,unsigned,unsigned){++hardware;return -1;};
 p.ops.speaker.delay_us=[](void *,uint32_t){return 0;};
 p.ops.ipc_open=[](void *,gat_transport *){++hardware;return -1;};p.ops.ipc_poll=[](void *,gat_transport *){++pollCalls;return -1;};
 p.ops.delay_ms=[](void *,unsigned){return 0;};p.ops.pcm_available=[](void *){return 0;};
 p.ops.pcm_start=[](void *,gat_transport *){++hardware;return -1;};
 p.ops.pcm_write=[](void *,gat_transport *,const void *,size_t){++hardware;return -1;};
 p.ops.pcm_stop=[](void *,gat_transport *){++hardware;return -1;};p.ops.ipc_quiesce=[](void *,gat_transport *){++hardware;return -1;};return p;
}
int main(int argc,char **argv) {
 assert(argc==2);std::ifstream f(argv[1],std::ios::binary);blob.assign(std::istreambuf_iterator<char>(f),{});assert(blob.size()==GA_FIRMWARE_PACK_BYTES);
 for(unsigned bad=0;bad<7;bad++) {
  boot_epoch();GiuliaAudio service;assert(service.init());
  auto *h=(mach_header_64 *)hostMach;auto *s=(segment_command_64 *)(h+1);unsigned before=maps;
  if(bad==0)h->magic=0;if(bad==1)s->cmdsize=8;if(bad==2)s->filesize--;
  if(bad==3)s->initprot=3;if(bad==4)s->vmaddr=gVirtBase-1;if(bad==5)s->vmsize=gPhysSize+1;
  if(bad==6){h->ncmds=2;h->sizeofcmds=144;memcpy(s+1,s,sizeof(*s));}
  assert(!service.loadFirmware() && !service.firmwareVerified && maps==before);service.free();
 }
 boot_epoch();GiuliaAudio badDigest;assert(badDigest.init());blob.back()^=1;
 assert(!badDigest.loadFirmware() && !badDigest.firmwareVerified);blob.back()^=1;badDigest.free();
 boot_epoch();GiuliaAudio noProvider;assert(noProvider.init() && noProvider.start(nullptr));
 assert(noProvider.firmwareVerified && !hardware);
 debugArg=0;assert(giulia_audio_runtime_command("audio.start")==1);
 assert(__atomic_load_n(&audioRequest,__ATOMIC_ACQUIRE)==0);debugArg=1;
 assert(giulia_audio_runtime_command("audio.start")==1);assert(giulia_audio_runtime_command("audio.unknown")==0);
 std::this_thread::sleep_for(std::chrono::milliseconds(30));
 noProvider.stop(nullptr);
 for(unsigned n=0;n<1000 && noProvider.references!=1;n++)std::this_thread::sleep_for(std::chrono::milliseconds(1));
 assert(!noProvider.workerRunning && !hardware && noProvider.references==1);
 assert(!noProvider.pinned && noProvider.runtime->stage==GAR_OFF);noProvider.free();
 boot_epoch();GiuliaAudio nativeMissing;assert(nativeMissing.init() && nativeMissing.loadFirmware());
 static ga_platform p=unavailable_provider();assert(giulia_audio_platform_register(&p)==1);
 assert(giulia_audio_platform_register(&p)==0);nativeMissing.run(2);nativeMissing.run(3);
 assert(!hardware && nativeMissing.runtime->stage==GAR_OFF && !nativeMissing.runtime->boot.running);
 assert(nativeMissing.pinned && nativeMissing.references==2);
 // Independently model an established idle session, then inject a fatal
 // observation through the real worker. It must revoke readiness and retain
 // its pin without attempting another secure call or amplifier operation.
 nativeMissing.runtime->stage=GAR_SPF_READY;nativeMissing.runtime->boot.running=1;
 nativeMissing.runtime->transport.spf_ready=1;nativeMissing.workerRunning=true;nativeMissing.retain();
 std::thread failedWorker(GiuliaAudio::worker,&nativeMissing,0);bool quarantined=false;
 for(unsigned n=0;n<1000 && !quarantined;n++) {
  IOLockLock(nativeMissing.lock);quarantined=nativeMissing.runtime->quarantined;IOLockUnlock(nativeMissing.lock);
  if(!quarantined)std::this_thread::sleep_for(std::chrono::milliseconds(1));
 }
 assert(quarantined);nativeMissing.stop(nullptr);failedWorker.join();
 assert(nativeMissing.references==2 && pollCalls==1 && !hardware && !nativeMissing.runtime->transport.spf_ready);
 nativeMissing.free();
 boot_epoch();GiuliaAudio disabled;assert(disabled.init());audioArg=0;unsigned before=maps;
 assert(disabled.start(nullptr) && maps==before && !audioOnline && !hardware);disabled.stop(nullptr);disabled.free();
 assert(releases==maps*2);puts("Production AUDIO service: strict compile, mapping validation, provider gating, worker teardown passed");
}
'''


class AudioRuntime(unittest.TestCase):
    def test_c_orchestration_and_fault_cleanup(self):
        cc = os.environ.get('CC') or shutil.which('clang') or shutil.which('cc')
        if not cc:
            self.skipTest('A C compiler is required')
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            sipa = folder/'sipa.bin'
            sipa.write_bytes(sipa_fixture())
            binary = folder/'runtime'
            result = subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                                     '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                                     str(ROOT/'tests/ios16_audio_runtime.c'), '-o', str(binary)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            result = subprocess.run([str(binary), str(sipa)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_full_service_compiles_and_guards_hardware(self):
        cxx = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++')
        if not cxx:
            self.skipTest('A C++ compiler is required')
        case = pack_fixtures.AudioPack('test_deterministic_wire_format_and_bss')
        case.setUp()
        self.addCleanup(case.doCleanups)
        pack = case.build()
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            (folder/'giulia_audio_firmware_pin.h').write_text(pack_fixtures.packer.pin_header(pack))
            (folder/'giulia_audio_sha256.h').write_text((ROOT/'ios16/common/giulia_sha256.h').read_text())
            source = (ROOT/'ios16/audio/GiuliaAudio.cpp').read_text()
            source = re.sub(r'^#include <[^>]+>\n', '', source, flags=re.MULTILINE)
            source = source.replace('extern const struct mach_header_64 _mh_execute_header;', '')
            source = source.replace('class GiuliaAudio : public IOService {', 'class GiuliaAudio : public IOService {\npublic:')
            unit = folder/'service.cpp'
            unit.write_text(SERVICE_SHIM+'\n'+source+'\n'+SERVICE_TEST)
            (folder/'pack.bin').write_bytes(pack)
            binary = folder/'service'
            result = subprocess.run([cxx, '-std=c++14', '-Wall', '-Wextra', '-Werror',
                                     '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-pthread',
                                     '-I', str(folder), *include_flags(ROOT), str(unit), '-o', str(binary)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            result = subprocess.run([str(binary), str(folder/'pack.bin')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)


if __name__ == '__main__':
    unittest.main()
