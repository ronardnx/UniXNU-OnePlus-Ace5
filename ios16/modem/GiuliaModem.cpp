/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * One serialized modem owner shared by terminal and future proven Apple ABI
 * adapters. Native providers execute operations; this service owns all state.
 */
#include <pexpert/arm64/board_config.h>
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>
#include <mach/mach_time.h>
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
#include "giulia_modem_platform.h"
#include "giulia_modem_build.h"
#include "giulia_modem_runtime.h"

extern "C" const char *giulia_platform_resources_identity(void);
extern "C" int gpr_global_initialize(void);
static const struct gmp_platform *modemPlatform;
static unsigned modemOwner,modemOnline;

extern "C" int giulia_modem_platform_register(const struct gmp_platform *p)
{
    if(!p || p->abi!=GMP_ABI || p->bytes!=sizeof(*p) || p->source!=GMP_NATIVE ||
       !p->submit || !p->poll || !p->deliver_packet)return 0;
    const struct gmp_platform *expected=NULL;
    return __atomic_compare_exchange_n(&modemPlatform,&expected,p,0,
                                      __ATOMIC_RELEASE,__ATOMIC_RELAXED);
}

struct gm_packet {
    struct gmr_identity id;
    size_t bytes;
    uint8_t payload[GMR_PAYLOAD];
};
struct gm_service_buffers {
    struct gmr_runtime runtime;
    struct gmr_command commands[16];
    struct gmr_action actions[GMR_PENDING],action;
    struct gmr_event event;
    struct gmr_snapshot snapshot;
    struct gm_packet packets[16],packet;
    unsigned packet_head,packet_count;
    unsigned command_head,command_count,action_head,action_count,start_requested;
};

class GiuliaModem : public IOService {
    OSDeclareDefaultStructors(GiuliaModem)
    IOLock *lock;
    struct gm_service_buffers *b;
    bool workerRunning,closing,pinned,ownsTerminal,autoAttempted;
    int mode;
    static int enqueueAction(void *,const struct gmr_action *);
    static int enqueuePacket(void *,const struct gmr_identity *,const uint8_t *,size_t);
    static void worker(void *,wait_result_t);
    void reportLocked();
    void tickLocked();
    void executeLocked();
public:
    bool init(OSDictionary *dictionary=NULL) override;
    bool start(IOService *) override;
    void stop(IOService *) override;
    void free() override;
    int submit(const struct gmr_command *);
    int snapshot(struct gmr_snapshot *);
    int terminal(const char *);
};
OSDefineMetaClassAndStructors(GiuliaModem,IOService)
static GiuliaModem *modemService;

bool GiuliaModem::init(OSDictionary *dictionary)
{
    lock=NULL;b=NULL;workerRunning=closing=pinned=ownsTerminal=autoAttempted=false;mode=0;
    if(!IOService::init(dictionary))return false;
    if(gpr_global_initialize()!=0)return false;
    lock=IOLockAlloc();b=(struct gm_service_buffers *)IOMalloc(sizeof(*b));
    if(!lock || !b)return false;
    bzero(b,sizeof(*b));
    struct gmr_ops ops={};ops.ctx=this;ops.submit=enqueueAction;ops.packet=enqueuePacket;
    return gmr_init(&b->runtime,&ops)==GMR_OK;
}

int GiuliaModem::enqueueAction(void *cookie,const struct gmr_action *action)
{
    GiuliaModem *s=(GiuliaModem *)cookie;
    if(!__atomic_load_n(&modemPlatform,__ATOMIC_ACQUIRE))return GMR_UNSUPPORTED;
    /* Cancellation/removal retires core operations before the worker reaches
     * their local queue entries. Reclaim those slots now so a full TX queue
     * cannot prevent the DATA_REMOVE that must drain submitted DMA. */
    unsigned keep=0;
    for(unsigned i=0;i<s->b->action_count;i++) {
        const struct gmr_action queued=s->b->actions[(s->b->action_head+i)%GMR_PENDING];
        bool live=false;
        for(unsigned j=0;j<GMR_PENDING;j++)if(s->b->runtime.pending[j].used &&
            s->b->runtime.pending[j].action.kind==queued.kind &&
            gmr_identity_equal(&s->b->runtime.pending[j].action.id,&queued.id)) {live=true;break;}
        if(live)s->b->actions[(s->b->action_head+keep++)%GMR_PENDING]=queued;
    }
    s->b->action_count=keep;
    if(s->b->action_count>=GMR_PENDING)return GMR_FULL;
    unsigned at=(s->b->action_head+s->b->action_count)%GMR_PENDING;
    s->b->actions[at]=*action;s->b->action_count++;return GMR_OK;
}

int GiuliaModem::enqueuePacket(void *cookie,const struct gmr_identity *id,const uint8_t *data,size_t bytes)
{
    GiuliaModem *s=(GiuliaModem *)cookie;
    if(!id || !data || !bytes || bytes>GMR_PAYLOAD)return GMR_ARGUMENT;
    if(s->b->packet_count==16)return GMR_FULL;
    struct gm_packet *packet=&s->b->packets[(s->b->packet_head+s->b->packet_count)%16];
    packet->id=*id;packet->bytes=bytes;memcpy(packet->payload,data,bytes);
    s->b->packet_count++;return GMR_OK;
}

int GiuliaModem::submit(const struct gmr_command *command)
{
    if(!command || command->bytes>GMR_PAYLOAD)return GMR_ARGUMENT;
    IOLockLock(lock);
    int result=GMR_OK;
    if(closing || !mode || !workerRunning)result=GMR_STATE;
    else if(b->command_count==16)result=GMR_FULL;
    else {
        b->commands[(b->command_head+b->command_count)%16]=*command;
        b->command_count++;
    }
    IOLockUnlock(lock);return result;
}

int GiuliaModem::snapshot(struct gmr_snapshot *snapshot)
{
    if(!snapshot)return GMR_ARGUMENT;
    IOLockLock(lock);gmr_snapshot(&b->runtime,snapshot);IOLockUnlock(lock);return GMR_OK;
}

void GiuliaModem::reportLocked()
{
    gmr_snapshot(&b->runtime,&b->snapshot);
    const struct gmr_snapshot *s=&b->snapshot;
    unsigned up=0;
    for(unsigned i=0;i<GMR_BEARERS;i++)if(s->bearers[i].state==GMR_BEARER_UP)up++;
    IOLog("GIULIA MODEM: epoch=%llu lifecycle=%u last_error=%u pending=%u stale=%llu provider=%u mode=%d resources=%u storage_dirty=%u durable=%u bearers_up=%u\n",
          s->epoch,s->lifecycle,s->last_error,s->pending_count,s->stale_events,
          __atomic_load_n(&modemPlatform,__ATOMIC_ACQUIRE)!=NULL,mode,
          s->resources_owned,s->storage_dirty,s->storage_durable,up);
    IOLog("GIULIA MODEM: last_fault=%u fault_epoch=%llu fault_count=%llu\n",
          s->last_fault,s->fault_epoch,s->fault_count);
    for(unsigned i=0;i<GMR_SLOTS;i++)
        IOLog("GIULIA MODEM: epoch=%llu slot=%u generation=%u sim=%u registration=%u ims=%u call=%u sms=%u\n",
              s->epoch,i,s->slots[i].generation,s->slots[i].sim,s->slots[i].registration,
              s->slots[i].ims,s->slots[i].call,s->slots[i].sms);
}

int GiuliaModem::terminal(const char *command)
{
    if(!strcmp(command,"modem.status")) {
        IOLockLock(lock);reportLocked();IOLockUnlock(lock);return 1;
    }
    unsigned kind=!strcmp(command,"modem.stop")?GMR_CMD_STOP:
        !strcmp(command,"modem.suspend")?GMR_CMD_SUSPEND:
        !strcmp(command,"modem.resume")?GMR_CMD_RESUME:0;
    bool start=!strcmp(command,"modem.start");
    if(!kind && !start)return 0;
    int debug=0;PE_parse_boot_argn("giulia_debug",&debug,sizeof(debug));
    IOLockLock(lock);
    int result=GMR_STATE;
    if(debug==1 && mode && !closing && workerRunning) {
        if(start) {
            result=b->start_requested?GMR_BUSY:GMR_OK;
            if(!result)b->start_requested=1;
        } else if(b->command_count<16) {
            struct gmr_command *c=&b->commands[(b->command_head+b->command_count)%16];
            bzero(c,sizeof(*c));c->kind=kind;c->slot=GMR_NO_SLOT;c->bearer=GMR_NO_BEARER;
            b->command_count++;result=GMR_OK;
        } else result=GMR_FULL;
    }
    IOLog("GIULIA MODEM: command=%s queue_result=%d completion=asynchronous\n",command,result);
    IOLockUnlock(lock);return 1;
}

void GiuliaModem::tickLocked()
{
    uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
    (void)gmr_tick(&b->runtime,ns/1000000);
}

void GiuliaModem::executeLocked()
{
    tickLocked();
    if(!closing && mode==2 && !autoAttempted && __atomic_load_n(&modemPlatform,__ATOMIC_ACQUIRE)) {
        autoAttempted=true;b->start_requested=1;
    }
    if(b->start_requested) {
        b->start_requested=0;int result=gmr_start(&b->runtime);
        IOLog("GIULIA MODEM: start submitted result=%d\n",result);
    }
    if(b->command_count) {
        int result=gmr_submit(&b->runtime,&b->commands[b->command_head]);
        b->command_head=(b->command_head+1)%16;b->command_count--;
        IOLog("GIULIA MODEM: operation submitted result=%d\n",result);
    }
}

void GiuliaModem::worker(void *cookie,wait_result_t)
{
    GiuliaModem *s=(GiuliaModem *)cookie;
    for(;;) {
        IOLockLock(s->lock);s->executeLocked();
        bool finished=s->closing && (s->b->runtime.state.lifecycle==GMR_OFF ||
                                    s->b->runtime.state.lifecycle==GMR_ISOLATED);
        if(finished) {
            if(s->b->runtime.state.lifecycle==GMR_ISOLATED)s->pinned=true;
            s->workerRunning=false;IOLockWakeup(s->lock,&s->workerRunning,false);
            IOLockUnlock(s->lock);break;
        }
        bool have=false;
        while(s->b->action_count && !have) {
            s->b->action=s->b->actions[s->b->action_head];
            s->b->action_head=(s->b->action_head+1)%GMR_PENDING;s->b->action_count--;
            for(unsigned i=0;i<GMR_PENDING;i++) {
                const struct gmr_pending *pending=&s->b->runtime.pending[i];
                if(pending->used && pending->action.kind==s->b->action.kind &&
                   gmr_identity_equal(&pending->action.id,&s->b->action.id)) {have=true;break;}
            }
        }
        bool havePacket=false;
        while(s->b->packet_count && !havePacket) {
            s->b->packet=s->b->packets[s->b->packet_head];
            s->b->packet_head=(s->b->packet_head+1)%16;s->b->packet_count--;
            const struct gmr_identity *id=&s->b->packet.id;
            if(id->epoch==s->b->runtime.state.epoch && id->bearer<GMR_BEARERS && id->slot<GMR_SLOTS) {
                const struct gmr_bearer *bearer=&s->b->runtime.state.bearers[id->bearer];
                havePacket=bearer->state==GMR_BEARER_UP && bearer->slot==id->slot &&
                    bearer->slot_generation==id->slot_generation && bearer->endpoint_generation==id->endpoint_generation &&
                    bearer->generation==id->bearer_generation;
                if(havePacket) {
                    havePacket=false;
                    for(unsigned i=0;i<GMR_EVENTS;i++)if(s->b->runtime.rx_pending[i].used &&
                        gmr_identity_equal(&s->b->runtime.rx_pending[i].id,id)) {havePacket=true;break;}
                }
            }
        }
        IOLockUnlock(s->lock);
        const struct gmp_platform *p=__atomic_load_n(&modemPlatform,__ATOMIC_ACQUIRE);
        if(have) {
            int result=p?p->submit(p->ctx,&s->b->action):GMR_UNSUPPORTED;
            if(result) {
                bzero(&s->b->event,sizeof(s->b->event));
                s->b->event.kind=s->b->action.kind==GMR_ACT_QMI?GMR_EV_QMI_RESPONSE:GMR_EV_COMPLETE;
                s->b->event.action_kind=s->b->action.kind;
                s->b->event.id=s->b->action.id;s->b->event.status=result;
                IOLockLock(s->lock);s->tickLocked();(void)gmr_event(&s->b->runtime,&s->b->event);IOLockUnlock(s->lock);
            }
        }
        if(havePacket) {
            int result=p?p->deliver_packet(p->ctx,&s->b->packet.id,s->b->packet.payload,s->b->packet.bytes):GMR_UNSUPPORTED;
            bzero(&s->b->event,sizeof(s->b->event));
            s->b->event.kind=result?GMR_EV_DATA_ERROR:GMR_EV_DATA_DELIVERED;
            s->b->event.id=s->b->packet.id;s->b->event.status=result?GMR_IO:GMR_OK;
            s->b->event.value=(uint32_t)s->b->packet.bytes;
            IOLockLock(s->lock);s->tickLocked();(void)gmr_event(&s->b->runtime,&s->b->event);IOLockUnlock(s->lock);
        }
        for(unsigned i=0;p && i<16;i++) {
            bzero(&s->b->event,sizeof(s->b->event));
            int result=p->poll(p->ctx,&s->b->event);
            if(!result)break;
            IOLockLock(s->lock);
            s->tickLocked();
            if(result<0) {
                s->b->event.kind=GMR_EV_FATAL;s->b->event.id.epoch=s->b->runtime.state.epoch;
                s->b->event.status=GMR_IO;
            }
            (void)gmr_event(&s->b->runtime,&s->b->event);
            IOLockUnlock(s->lock);
            if(result<0)break;
        }
        IOSleep(10);
    }
    s->release();thread_terminate(current_thread());
}

bool GiuliaModem::start(IOService *provider)
{
    if(!IOService::start(provider))return false;
    unsigned expected=0;
    if(!__atomic_compare_exchange_n(&modemOwner,&expected,1u,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
    ownsTerminal=true;PE_parse_boot_argn("giulia_modem",&mode,sizeof(mode));
    if(mode<0 || mode>2)mode=0;
    IOLog("GIULIA MODEM: runtime ABI=1; single lifecycle/subscription/bearer/storage owner\n");
    IOLog("%s\n",GIULIA_MODEM_BUILD_MARKER);
    IOLog("%s\n",giulia_platform_resources_identity());
    thread_t thread;workerRunning=true;retain();
    if(kernel_thread_start(worker,this,&thread)!=KERN_SUCCESS) {
        workerRunning=false;release();return false;
    }
    thread_deallocate(thread);
    /* A published kernel entrypoint may race stop(). Keep its lock and core
     * address valid until reboot, even after the worker is quiescent. */
    retain();
    __atomic_store_n(&modemService,this,__ATOMIC_RELEASE);
    __atomic_store_n(&modemOnline,1u,__ATOMIC_RELEASE);
    registerService();return true;
}

void GiuliaModem::stop(IOService *provider)
{
    if(ownsTerminal)__atomic_store_n(&modemOnline,0u,__ATOMIC_RELEASE);
    IOLockLock(lock);closing=true;b->start_requested=0;b->command_count=0;
    if(b->runtime.state.lifecycle!=GMR_OFF && b->runtime.state.lifecycle!=GMR_ISOLATED) {
        struct gmr_command *c=&b->commands[0];bzero(c,sizeof(*c));
        c->kind=GMR_CMD_STOP;c->slot=GMR_NO_SLOT;c->bearer=GMR_NO_BEARER;
        b->command_head=0;b->command_count=1;
    }
    while(workerRunning)IOLockSleep(lock,&workerRunning,THREAD_UNINT);
    IOLockUnlock(lock);
    if(pinned)IOLog("GIULIA MODEM: unresolved ownership retained; service pinned\n");
    IOService::stop(provider);
}

void GiuliaModem::free()
{
    if(b)IOFree(b,sizeof(*b));
    if(lock)IOLockFree(lock);
    IOService::free();
}

extern "C" int giulia_modem_submit(const struct gmr_command *c)
{
    GiuliaModem *s=__atomic_load_n(&modemService,__ATOMIC_ACQUIRE);
    return s && __atomic_load_n(&modemOnline,__ATOMIC_ACQUIRE)?s->submit(c):GMR_STATE;
}
extern "C" int giulia_modem_get_snapshot(struct gmr_snapshot *snapshot)
{
    GiuliaModem *s=__atomic_load_n(&modemService,__ATOMIC_ACQUIRE);
    return s && __atomic_load_n(&modemOnline,__ATOMIC_ACQUIRE)?s->snapshot(snapshot):GMR_STATE;
}
extern "C" int giulia_modem_terminal(const char *command)
{
    if(!command || strncmp(command,"modem.",6))return 0;
    GiuliaModem *s=__atomic_load_n(&modemService,__ATOMIC_ACQUIRE);
    if(!s || !__atomic_load_n(&modemOnline,__ATOMIC_ACQUIRE)) {
        IOLog("GIULIA MODEM: offline command=%s\n",command);return 1;
    }
    return s->terminal(command);
}
#endif
