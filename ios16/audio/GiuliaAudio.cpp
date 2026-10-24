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
#include <libkern/kernel_mach_header.h>
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
#include "giulia_audio_fixture.h"
#include "../common/giulia_sha256.h"

#ifndef GIULIA_PLATFORM_BROKER_ENABLED
#define GIULIA_PLATFORM_BROKER_ENABLED 1
#endif
#if GIULIA_PLATFORM_BROKER_ENABLED
#include "../platform/giulia_platform_resources.h"
#endif

extern "C" { extern unsigned long gPhysBase,gPhysSize,gVirtBase; }
static const struct ga_platform *audioPlatform;
static unsigned audioRequest,audioOnline,audioClosing;
static unsigned audioOwner,audioRegistering;
static unsigned audioServiceState,audioMode,audioFirmware,audioActive,audioFixtureActive;
static unsigned audioSnapshotSequence;
static uint64_t audioSnapshot[32];
static uint64_t audioProtocolEpoch;
enum { GA_SERVICE_ABSENT,GA_SERVICE_DISABLED,GA_SERVICE_VALIDATING,GA_SERVICE_BAD_FIRMWARE,
    GA_SERVICE_STARTING,GA_SERVICE_THREAD_FAILED,GA_SERVICE_ONLINE,GA_SERVICE_CLOSING,GA_SERVICE_STOPPED };

#if defined(KERNEL) && GIULIA_HOTLOAD_ENABLED
extern "C" __attribute__((noinline)) int giulia_audio_hotload_decide(uint64_t epoch,uint32_t operation,
    const uint64_t *request,uint32_t request_words,uint64_t *response,uint32_t response_words)
{
    return giulia_hotload_select(GH_DOMAIN_AUDIO,epoch,operation,request,request_words,response,response_words);
}
#endif

static void gaudio_cached_status()
{
    uint64_t w[32]={};unsigned valid=0,sequence=0;
    for(unsigned attempt=0;attempt<4;attempt++) {
        sequence=__atomic_load_n(&audioSnapshotSequence,__ATOMIC_ACQUIRE);
        if(sequence&1u)continue;
        for(unsigned i=0;i<32;i++)w[i]=__atomic_load_n(&audioSnapshot[i],__ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if(sequence==__atomic_load_n(&audioSnapshotSequence,__ATOMIC_ACQUIRE)) {valid=sequence!=0;break;}
    }
    IOLog("GIULIA AUDIO: runtime=1 service_state=%u mode=%u worker_online=%u active_request=%u queued_request=%u closing=%u firmware_verified=%u platform_registered=%u snapshot=%u sequence=%u stage=%u boot_phase=%u result=%u backend=%d adsp_ready=%u spf_ready=%u playback_active=%u quarantined=%u playback_verified=0 hardware_verified=0\n",
        __atomic_load_n(&audioServiceState,__ATOMIC_ACQUIRE),__atomic_load_n(&audioMode,__ATOMIC_ACQUIRE),
        __atomic_load_n(&audioOnline,__ATOMIC_ACQUIRE),__atomic_load_n(&audioActive,__ATOMIC_ACQUIRE),
        __atomic_load_n(&audioRequest,__ATOMIC_ACQUIRE),__atomic_load_n(&audioClosing,__ATOMIC_ACQUIRE),
        __atomic_load_n(&audioFirmware,__ATOMIC_ACQUIRE)==1,
        __atomic_load_n(&audioPlatform,__ATOMIC_ACQUIRE)!=NULL,valid,sequence,
        (unsigned)w[0],(unsigned)w[1],(unsigned)w[2],(int)(uint32_t)w[3],
        (unsigned)w[11],(unsigned)w[12],(unsigned)w[13],(unsigned)w[14]);
    if(valid)IOLog("GIULIA AUDIO: cause_result=%u cleanup_backend=%d failure_stage=%u boot_backend=%d boot_cleanup=%d cleanup_phase=%u cleanup_image=%u graph_attempted=%u graph_running=%u speaker_cleanup=%d local_quiesced=%u protocol_epoch=%llu spf_queries=%u spf_basic_replies=%u spf_timeouts=%u spf_last_state=%u spf_last_basic=%u metadata_mask=%u init_mask=%u authenticated_mask=%u prepared=%u\n",
        (unsigned)w[4],(int)(uint32_t)w[5],(unsigned)w[6],(int)(uint32_t)w[7],(int)(uint32_t)w[8],
        (unsigned)w[9],(unsigned)w[10],(unsigned)w[15],(unsigned)w[16],(int)(uint32_t)w[17],
        (unsigned)w[18],(unsigned long long)w[19],(unsigned)w[20],(unsigned)w[21],(unsigned)w[22],
        (unsigned)w[23],(unsigned)w[24],(unsigned)w[25],(unsigned)w[26],(unsigned)w[27],(unsigned)w[28]);
    if(!__atomic_load_n(&audioPlatform,__ATOMIC_ACQUIRE))
        IOLog("GIULIA AUDIO: blocker=native-platform-owner-missing power_icc_lease=0 pcm_graph_provider=0 speaker_i2c_provider=0\n");
    uint64_t counts[6];gapt_snapshot(counts);
    IOLog("GIULIA AUDIO: proto_domain=%u bridge_abi=%u local_attempted=%llu local_adopted=%llu local_fallback=%llu local_rejected=%llu local_changed=%llu\n",
        GH_DOMAIN_AUDIO,GH_ABI,(unsigned long long)counts[0],(unsigned long long)counts[1],
        (unsigned long long)counts[2],(unsigned long long)counts[3],(unsigned long long)counts[4]);
}

static void gaudio_protocol_fixture()
{
    unsigned idle=0;
    if(!__atomic_compare_exchange_n(&audioFixtureActive,&idle,1u,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        IOLog("GIULIA AUDIO: proto_fixture rejected=busy synthetic=1 hardware_verified=0\n");return;
    }
    struct gaf_fixture *f=(struct gaf_fixture *)IOMalloc(sizeof(*f));
    if(!f)IOLog("GIULIA AUDIO: proto_fixture rejected=no-memory synthetic=1 hardware_verified=0\n");
    else {
        /* Minimum logical fixture epoch avoids advancing the domain past a
         * real future cold epoch. Use the last published native epoch if any.
         * These isolated objects never become a hardware ownership proof. */
        uint64_t epoch=__atomic_load_n(&audioProtocolEpoch,__ATOMIC_ACQUIRE);
        if(!epoch)epoch=1;
        bool noProvider=__atomic_load_n(&audioPlatform,__ATOMIC_ACQUIRE)==NULL;
        bool disabled=__atomic_load_n(&audioMode,__ATOMIC_ACQUIRE)!=1&&
            !__atomic_load_n(&audioOnline,__ATOMIC_ACQUIRE);
        uint64_t before[6],after[6];gapt_snapshot(before);
        uint32_t passed=gaf_run(f,epoch);gapt_snapshot(after);
        unsigned exact=(noProvider&&!__atomic_load_n(&audioPlatform,__ATOMIC_ACQUIRE))||
            (disabled&&__atomic_load_n(&audioMode,__ATOMIC_ACQUIRE)!=1&&
             !__atomic_load_n(&audioOnline,__ATOMIC_ACQUIRE));
        IOLog("GIULIA AUDIO: proto_fixture synthetic=1 hardware_verified=0 counter_exact=%u cases=20 passed_mask=%#x expected_mask=%#x failed_mask=%#x epoch=%llu attempted=%llu adopted=%llu fallback=%llu rejected=%llu changed=%llu\n",
            exact,passed,0xfffffu,passed^0xfffffu,(unsigned long long)epoch,
            (unsigned long long)(after[0]-before[0]),(unsigned long long)(after[1]-before[1]),
            (unsigned long long)(after[2]-before[2]),(unsigned long long)(after[3]-before[3]),
            (unsigned long long)(after[4]-before[4]));
        IOFree(f,sizeof(*f));
    }
    __atomic_store_n(&audioFixtureActive,0u,__ATOMIC_RELEASE);
}

extern "C" int giulia_audio_platform_register(const struct ga_platform *p)
{
#if GIULIA_PLATFORM_BROKER_ENABLED
    if(gpr_global_initialize()!=GPR_OK)return 0;
#endif
    if(!p || p->abi!=GA_PLATFORM_ABI || p->bytes!=sizeof(*p) || !gar_ops_valid(&p->ops) ||
       !p->sipa || !p->sipa_bytes || p->sipa_bytes>GSIPA_MAX_BLOB)return 0;
    /* Serialize registration so duplicate callers cannot acquire or release
     * another registration's broker membership for the same pointer. */
    unsigned idle=0;
    if(!__atomic_compare_exchange_n(&audioRegistering,&idle,1u,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return 0;
    if(__atomic_load_n(&audioPlatform,__ATOMIC_ACQUIRE)) {
        __atomic_store_n(&audioRegistering,0u,__ATOMIC_RELEASE);return 0;
    }
#if GIULIA_PLATFORM_BROKER_ENABLED
    if(!gpr_global_legacy_ipcc_claim(2u,p)) {
        __atomic_store_n(&audioRegistering,0u,__ATOMIC_RELEASE);return 0;
    }
#endif
    const struct ga_platform *expected=NULL;
    int published=__atomic_compare_exchange_n(&audioPlatform,&expected,p,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED);
#if GIULIA_PLATFORM_BROKER_ENABLED
    if(!published && expected!=p)(void)gpr_global_legacy_ipcc_release(2u,p);
#endif
    /* The provider and this host membership remain pinned until reboot. */
    __atomic_store_n(&audioRegistering,0u,__ATOMIC_RELEASE);return published;
}
extern "C" int giulia_audio_runtime_command(const char *command)
{
    if(!command)return 0;
    if(!strcmp(command,"audio.proto")) {
        int debug=0;PE_parse_boot_argn("giulia_debug",&debug,sizeof(debug));
        if(debug!=1)IOLog("GIULIA AUDIO: proto_fixture rejected=debug-disabled synthetic=1 hardware_verified=0\n");
        else gaudio_protocol_fixture();
        return 1;
    }
    unsigned request=!strcmp(command,"audio.status")?1:!strcmp(command,"audio.preflight")?2:
        !strcmp(command,"audio.start")?3:!strcmp(command,"audio.stop")?4:!strcmp(command,"audio.test")?5:0;
    if(!request)return 0;
    if(request==1) {gaudio_cached_status();return 1;}
    int debug=0;PE_parse_boot_argn("giulia_debug",&debug,sizeof(debug));
    unsigned expected=0;
    const char *reason=NULL;
    if(__atomic_load_n(&audioClosing,__ATOMIC_ACQUIRE))reason="closing";
    else if(request>=3 && debug!=1)reason="debug-disabled";
    else if(!__atomic_load_n(&audioOnline,__ATOMIC_ACQUIRE)) {
        unsigned state=__atomic_load_n(&audioServiceState,__ATOMIC_ACQUIRE);
        reason=state==GA_SERVICE_DISABLED?"mode-disabled":state==GA_SERVICE_BAD_FIRMWARE?
            "firmware-invalid":state==GA_SERVICE_THREAD_FAILED?"worker-failed":"worker-offline";
    } else if(!__atomic_compare_exchange_n(&audioRequest,&expected,request,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))reason="busy";
    if(reason) {
        IOLog("GIULIA AUDIO: command=%s rejected=%s checks_performed=0 playback=0\n",command,reason);
        gaudio_cached_status();
    }
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
    const kernel_mach_header_t *h=&_mh_execute_header;
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
    uint64_t w[32]={runtime->stage,runtime->boot.phase,runtime->result,(uint32_t)runtime->backend,
        runtime->cause_result,(uint32_t)runtime->cleanup_backend,runtime->failure_stage,
        (uint32_t)runtime->boot.backend_result,(uint32_t)runtime->boot.cleanup_result,
        runtime->boot.cleanup_phase,runtime->boot.cleanup_image,
        runtime->boot.running&&!runtime->quarantined,runtime->transport.spf_ready,runtime->playback,
        runtime->quarantined,runtime->graph_attempted,runtime->graph_running,
        (uint32_t)runtime->speaker.cleanup_error,(unsigned)gar_quiesced(runtime),runtime->transport.epoch,
        runtime->transport.spf_queries,runtime->transport.spf_basic_replies,runtime->transport.spf_timeouts,
        runtime->transport.spf_status,runtime->transport.spf_error,runtime->boot.metadata_mask,
        runtime->boot.init_attempted_mask,runtime->boot.authenticated_mask,runtime->boot.prepared};
    uint64_t epoch=__atomic_load_n(&audioProtocolEpoch,__ATOMIC_RELAXED);
    while(w[19]>epoch&&!__atomic_compare_exchange_n(&audioProtocolEpoch,&epoch,w[19],0,
        __ATOMIC_RELEASE,__ATOMIC_RELAXED)) {}
    __atomic_add_fetch(&audioSnapshotSequence,1u,__ATOMIC_ACQ_REL);
    for(unsigned i=0;i<32;i++)__atomic_store_n(&audioSnapshot[i],w[i],__ATOMIC_RELAXED);
    __atomic_add_fetch(&audioSnapshotSequence,1u,__ATOMIC_RELEASE);gaudio_cached_status();
}
void GiuliaAudio::run(unsigned command)
{
    const struct ga_platform *p=__atomic_load_n(&audioPlatform,__ATOMIC_ACQUIRE);
    if(command<=2) {
        if(command==2 && p)IOLog("GIULIA AUDIO: boot_prerequisites=%d pcm_provider_ready=%d checks_only=1\n",
            p->ops.boot.prerequisites(p->ops.boot.cookie),p->ops.pcm_available(p->ops.cookie));
        else if(command==2)IOLog("GIULIA AUDIO: preflight=blocked checks_only=1 checks_performed=0 provider=0\n");
        report();return;
    }
    if(!p || !firmwareVerified) {
        IOLog("GIULIA AUDIO: request=%u completed_result=%u rejected=%s hardware_verified=0\n",command,
            GAR_PLATFORM,p?"firmware-invalid":"native-platform-owner-missing");report();return;
    }
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
        if(command) {
            __atomic_store_n(&audioActive,command,__ATOMIC_RELEASE);s->run(command);
            __atomic_store_n(&audioActive,0u,__ATOMIC_RELEASE);
        }
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
    __atomic_store_n(&audioMode,(unsigned)mode,__ATOMIC_RELEASE);
    if(mode!=1) {
        __atomic_store_n(&audioServiceState,GA_SERVICE_DISABLED,__ATOMIC_RELEASE);
        IOLog("GIULIA AUDIO: compiled runtime disabled; use giulia_audio=1 for preparation\n");report();return true;
    }
    __atomic_store_n(&audioServiceState,GA_SERVICE_VALIDATING,__ATOMIC_RELEASE);
    if(!loadFirmware()) {
        __atomic_store_n(&audioFirmware,2u,__ATOMIC_RELEASE);
        __atomic_store_n(&audioServiceState,GA_SERVICE_BAD_FIRMWARE,__ATOMIC_RELEASE);
        IOLog("GIULIA AUDIO: pinned firmware validation failed; device_writes=0\n");report();return true;
    }
    __atomic_store_n(&audioFirmware,1u,__ATOMIC_RELEASE);
    __atomic_store_n(&audioServiceState,GA_SERVICE_STARTING,__ATOMIC_RELEASE);
    thread_t thread;workerRunning=true;retain();
    if(kernel_thread_start(worker,this,&thread)!=KERN_SUCCESS) {
        workerRunning=false;release();
        __atomic_store_n(&audioServiceState,GA_SERVICE_THREAD_FAILED,__ATOMIC_RELEASE);return false;
    }
    thread_deallocate(thread);__atomic_store_n(&audioOnline,1u,__ATOMIC_RELEASE);
    __atomic_store_n(&audioServiceState,GA_SERVICE_ONLINE,__ATOMIC_RELEASE);
    registerService();IOLockLock(lock);report();IOLockUnlock(lock);return true;
}
void GiuliaAudio::stop(IOService *provider)
{
    if(terminalOwner) {
        __atomic_store_n(&audioServiceState,GA_SERVICE_CLOSING,__ATOMIC_RELEASE);
        __atomic_store_n(&audioClosing,1u,__ATOMIC_RELEASE);__atomic_store_n(&audioOnline,0u,__ATOMIC_RELEASE);
    }
    IOLockLock(lock);stopping=true;
    while(workerRunning)IOLockSleep(lock,&workerRunning,THREAD_UNINT);
    IOLockUnlock(lock);
    if(terminalOwner)__atomic_store_n(&audioServiceState,GA_SERVICE_STOPPED,__ATOMIC_RELEASE);
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
