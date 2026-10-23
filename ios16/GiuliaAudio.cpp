/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Opt-in firmware validation and serialized audio runtime coordinator.
 * The platform lease/PCM provider is a separate unresolved dependency;
 * its absence is reported before any DSP/amp/MMIO operation.
 */
#include <pexpert/arm64/board_config.h>
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <mach-o/loader.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>
#ifndef UINT64_C
#define UINT64_C(x) x##ULL
#endif
#ifndef UINT32_C
#define UINT32_C(x) x##U
#endif
#ifndef UINT64_MAX
#define UINT64_MAX (~(uint64_t)0)
#endif
#ifndef UINT32_MAX
#define UINT32_MAX (~(uint32_t)0)
#endif
#include "giulia_audio_platform.h"
#include "giulia_audio_firmware_pin.h"
#include "giulia_audio_sha256.h"

#ifndef GIULIA_GPU_IPCC_GATE
#define GIULIA_GPU_IPCC_GATE 0
#endif
#if GIULIA_GPU_IPCC_GATE
extern "C" int giulia_gpu_ipcc_claim(unsigned,const void *);
extern "C" int giulia_gpu_ipcc_release(unsigned,const void *);
#endif

extern "C" { extern unsigned long gPhysBase,gPhysSize,gVirtBase; }
extern const struct mach_header_64 _mh_execute_header;
static const struct ga_platform *audioPlatform;
static unsigned audioRequest,audioOnline,audioClosing;
static unsigned audioOwner,audioRegistering;

extern "C" int giulia_audio_platform_register(const struct ga_platform *p)
{
    if(!p || p->abi!=GA_PLATFORM_ABI || p->bytes!=sizeof(*p) || !gar_ops_valid(&p->ops) ||
       !p->sipa || !p->sipa_bytes || p->sipa_bytes>GSIPA_MAX_BLOB)return 0;
    /* Serialize registration so duplicate callers cannot acquire or release
     * another registration's broker membership for the same pointer. */
    unsigned idle=0;
    if(!__atomic_compare_exchange_n(&audioRegistering,&idle,1u,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return 0;
    if(__atomic_load_n(&audioPlatform,__ATOMIC_ACQUIRE)) {
        __atomic_store_n(&audioRegistering,0u,__ATOMIC_RELEASE);return 0;
    }
#if GIULIA_GPU_IPCC_GATE
    if(!giulia_gpu_ipcc_claim(2u,p)) {
        __atomic_store_n(&audioRegistering,0u,__ATOMIC_RELEASE);return 0;
    }
#endif
    const struct ga_platform *expected=NULL;
    int published=__atomic_compare_exchange_n(&audioPlatform,&expected,p,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED);
#if GIULIA_GPU_IPCC_GATE
    if(!published && expected!=p)(void)giulia_gpu_ipcc_release(2u,p);
#endif
    /* The provider and this host membership remain pinned until reboot. */
    __atomic_store_n(&audioRegistering,0u,__ATOMIC_RELEASE);return published;
}
extern "C" int giulia_audio_runtime_command(const char *command)
{
    unsigned request=!strcmp(command,"audio.status")?1:!strcmp(command,"audio.preflight")?2:
        !strcmp(command,"audio.start")?3:!strcmp(command,"audio.stop")?4:!strcmp(command,"audio.test")?5:0;
    if(!request)return 0;
    int debug=0;PE_parse_boot_argn("giulia_debug",&debug,sizeof(debug));
    unsigned expected=0;
    if(!__atomic_load_n(&audioOnline,__ATOMIC_ACQUIRE) || __atomic_load_n(&audioClosing,__ATOMIC_ACQUIRE) ||
       (request>=3 && debug!=1) ||
       !__atomic_compare_exchange_n(&audioRequest,&expected,request,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
        IOLog("GIULIA AUDIO: command=%s rejected=offline-busy-or-debug-disabled playback=0\n",command);
    return 1;
}

class GiuliaAudio : public IOService {
    OSDeclareDefaultStructors(GiuliaAudio)
    IOLock *lock;
    IOMemoryDescriptor *firmwareMemory;
    IOMemoryMap *firmwareMap;
    struct gar_state *runtime;
    bool stopping,workerRunning,pinned,terminalOwner,firmwareVerified;
    size_t packBytes;
    const void *pack;
    static void worker(void *,wait_result_t);
    bool loadFirmware();
    void report();
    void run(unsigned);
public:
    bool init(OSDictionary *dictionary=NULL) override;
    bool start(IOService *) override;
    void stop(IOService *) override;
    void free() override;
};
OSDefineMetaClassAndStructors(GiuliaAudio,IOService)

bool GiuliaAudio::init(OSDictionary *d)
{
    lock=NULL;firmwareMemory=NULL;firmwareMap=NULL;runtime=NULL;pack=NULL;packBytes=0;
    stopping=workerRunning=pinned=terminalOwner=firmwareVerified=false;
    if(!IOService::init(d))return false;
    lock=IOLockAlloc();runtime=(struct gar_state *)IOMalloc(sizeof(*runtime));
    if(runtime)bzero(runtime,sizeof(*runtime));
    return lock && runtime;
}
bool GiuliaAudio::loadFirmware()
{
    const struct mach_header_64 *h=&_mh_execute_header;
    if(h->magic!=MH_MAGIC_64 || !h->ncmds || h->ncmds>128 || h->sizeofcmds>0x10000)return false;
    const uint8_t *p=(const uint8_t *)(h+1);size_t left=h->sizeofcmds;
    struct segment_command_64 found={};bool have=false;
    for(unsigned i=0;i<h->ncmds;i++) {
        struct load_command c;
        if(left<sizeof(c))return false;
        memcpy(&c,p,sizeof(c));
        if(c.cmdsize<8 || (c.cmdsize&7) || c.cmdsize>left)return false;
        if(c.cmd==LC_SEGMENT_64) {
            if(c.cmdsize<sizeof(found))return false;
            struct segment_command_64 s;memcpy(&s,p,sizeof(s));
            if(!strncmp(s.segname,"__AUDIO_FW",16)) {
                if(have || c.cmdsize!=sizeof(s) || s.nsects || s.flags || s.maxprot!=1 || s.initprot!=1)return false;
                found=s;have=true;
            }
        }
        p+=c.cmdsize;left-=c.cmdsize;
    }
    if(left || !have || found.filesize!=GA_FIRMWARE_PACK_BYTES || found.vmsize<found.filesize ||
       found.vmaddr<gVirtBase || found.vmaddr-gVirtBase>gPhysSize ||
       found.vmsize>gPhysSize-(found.vmaddr-gVirtBase) || gPhysBase>UINT64_MAX-(found.vmaddr-gVirtBase))return false;
    uint64_t physical=gPhysBase+found.vmaddr-gVirtBase;
    firmwareMemory=IOMemoryDescriptor::withPhysicalAddress(physical,found.filesize,kIODirectionIn);
    firmwareMap=firmwareMemory?firmwareMemory->map(kIOMapReadOnly|kIOMapCopybackCache):NULL;
    if(!firmwareMap)return false;
    pack=(const void *)firmwareMap->getVirtualAddress();packBytes=(size_t)found.filesize;
    unsigned char digest[32];gwh_sha256(pack,packBytes,digest);
    struct gab_plan *plan=(struct gab_plan *)IOMalloc(sizeof(*plan));
    if(!plan)return false;
    int error=gab_validate(pack,packBytes,plan);IOFree(plan,sizeof(*plan));
    if(error || !gwh_equal(digest,ga_firmware_pack_sha256))return false;
    firmwareVerified=true;
    IOLog("GIULIA AUDIO: firmware pack mapped read-only and digest verified\n");return true;
}
void GiuliaAudio::report()
{
    const struct ga_platform *p=__atomic_load_n(&audioPlatform,__ATOMIC_ACQUIRE);
    IOLog("GIULIA AUDIO: runtime=1 firmware_verified=%u platform_registered=%u stage=%u boot_phase=%u result=%u backend=%d adsp_ready=%u spf_ready=%u playback_active=%u quarantined=%u playback_verified=0 hardware_verified=0\n",
        firmwareVerified,p!=NULL,runtime->stage,runtime->boot.phase,runtime->result,runtime->backend,
        runtime->boot.running && !runtime->quarantined,runtime->transport.spf_ready,runtime->playback,runtime->quarantined);
    if(!p)IOLog("GIULIA AUDIO: blocker=native-platform-owner-missing power_icc_lease=0 pcm_graph_provider=0 speaker_i2c_provider=0\n");
}
void GiuliaAudio::run(unsigned command)
{
    const struct ga_platform *p=__atomic_load_n(&audioPlatform,__ATOMIC_ACQUIRE);
    if(command<=2) {
        if(command==2 && p)IOLog("GIULIA AUDIO: boot_prerequisites=%d pcm_provider_ready=%d checks_only=1\n",
            p->ops.boot.prerequisites(p->ops.boot.cookie),p->ops.pcm_available(p->ops.cookie));
        report();return;
    }
    if(!p || !firmwareVerified) {report();return;}
    /* Pin the coordinator, immutable pack and provider for uncertain secure
     * operations. Neither service stop nor a terminal retry revokes ownership. */
    if(!pinned){retain();pinned=true;}
    int result=GAR_ARGUMENT;
    if(command==3)result=gar_start(&p->ops,runtime,pack,packBytes);
    else if(command==4)result=gar_stop(&p->ops,runtime);
    else if(command==5) {
        /* Bounded low-level test; no sound until provider confirms calibrated
         * PCM/MI2S and the amp engine verifies both physical chips/trim. */
        const size_t bytes=12000u*8u;
        int32_t *pcm=(int32_t *)IOMalloc(bytes);
        if(pcm) {
            for(unsigned i=0;i<12000;i++) {
                /* 1kHz triangle, peak -48dBFS, 24 valid bits left-aligned. */
                int v=(int)(i%48);v=v<24?v:48-v;
                pcm[2*i]=pcm[2*i+1]=(v-12)*699136;
            }
            result=gar_play(&p->ops,runtime,p->sipa,p->sipa_bytes,pcm,bytes);
            /* pcm_write's contract consumes/copies this input synchronously;
             * its provider separately retains all in-flight DMA backing. */
            IOFree(pcm,bytes);
        }
    }
    IOLog("GIULIA AUDIO: request=%u completed_result=%d device_test_verified=0\n",command,result);report();
}
void GiuliaAudio::worker(void *cookie,wait_result_t)
{
    GiuliaAudio *s=(GiuliaAudio *)cookie;IOLockLock(s->lock);
    while(!s->stopping) {
        const struct ga_platform *p=__atomic_load_n(&audioPlatform,__ATOMIC_ACQUIRE);
        if(p && s->runtime->stage==GAR_SPF_READY && !s->runtime->quarantined) {
            int e=p->ops.ipc_poll(p->ops.cookie,&s->runtime->transport);
            if(e<0 || !s->runtime->transport.spf_ready) {
                gar_fail(s->runtime,GAR_TRANSPORT,e);s->report();
            }
        }
        unsigned command=__atomic_exchange_n(&audioRequest,0u,__ATOMIC_ACQ_REL);
        if(command)s->run(command);
        IOLockUnlock(s->lock);IOSleep(10);IOLockLock(s->lock);
    }
    s->workerRunning=false;IOLockWakeup(s->lock,&s->workerRunning,false);IOLockUnlock(s->lock);
    s->release();thread_terminate(current_thread());
}
bool GiuliaAudio::start(IOService *provider)
{
    if(!IOService::start(provider))return false;
    unsigned expected=0;
    if(!__atomic_compare_exchange_n(&audioOwner,&expected,1u,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
    terminalOwner=true;
    int mode=0;PE_parse_boot_argn("giulia_audio",&mode,sizeof(mode));
    if(mode!=1){IOLog("GIULIA AUDIO: compiled runtime disabled; use giulia_audio=1 for preparation\n");return true;}
    if(!loadFirmware()) {IOLog("GIULIA AUDIO: pinned firmware validation failed; device_writes=0\n");return true;}
    thread_t thread;workerRunning=true;retain();
    if(kernel_thread_start(worker,this,&thread)!=KERN_SUCCESS) {workerRunning=false;release();return false;}
    thread_deallocate(thread);__atomic_store_n(&audioOnline,1u,__ATOMIC_RELEASE);
    registerService();report();return true;
}
void GiuliaAudio::stop(IOService *provider)
{
    if(terminalOwner){__atomic_store_n(&audioClosing,1u,__ATOMIC_RELEASE);__atomic_store_n(&audioOnline,0u,__ATOMIC_RELEASE);}
    IOLockLock(lock);stopping=true;
    while(workerRunning)IOLockSleep(lock,&workerRunning,THREAD_UNINT);
    IOLockUnlock(lock);
    if(pinned)IOLog("GIULIA AUDIO: runtime resources retained until reboot; teardown not assumed\n");
    IOService::stop(provider);
}
void GiuliaAudio::free()
{
    OSSafeReleaseNULL(firmwareMap);OSSafeReleaseNULL(firmwareMemory);
    if(runtime)IOFree(runtime,sizeof(*runtime));
    if(lock)IOLockFree(lock);
    IOService::free();
}
#endif
