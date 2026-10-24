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
#include "giulia_modem_queue.h"
#include "giulia_modem_hotload_wire.h"
#include "../common/giulia_hotload.h"

static_assert(GMR_HOTLOAD_DOMAIN==GH_DOMAIN_MODEM &&
              GMR_HOTLOAD_INTERFACE_ABI==GH_INTERFACE_ABI &&
              GMR_HOTLOAD_STATE_ABI==GH_STATE_ABI &&
              GMR_HOTLOAD_MAX_WORDS==GH_MAX_WORDS,"modem GH contract mismatch");

/* Keep a real external call boundary for target-kernel attribution. The
 * production QMI callback and fixtures both enter here with resident-owned
 * request words and the captured hardware epoch. Only the bridge ticket
 * crosses NLH_CALL; these borrowed kernel pointers never do. The caller keeps
 * its lock and validates the returned response before adopting it. Without
 * hotload the public header supplies GH_UNAVAILABLE without a link dependency. */
extern "C" __attribute__((noinline)) int giulia_modem_hotload_decide(uint64_t epoch,
    const uint64_t *request,uint32_t requestWords,uint64_t *response)
{
    return giulia_hotload_select(GMR_HOTLOAD_DOMAIN,epoch,GMR_HOTLOAD_QMI_RESPONSE,
                                request,requestWords,response,1);
}

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
    struct gmr_queued_command commands[16];
    struct gmr_action actions[GMR_PENDING],action;
    struct gmr_event event;
    struct gmr_action proto_action;
    struct gmr_event proto_event;
    struct gmr_snapshot snapshot;
    struct gm_packet packets[16],packet;
    unsigned packet_head,packet_count;
    unsigned command_head,command_count,action_head,action_count,start_requested,stop_requested;
    uint64_t start_epoch;
    uint64_t proto_native_inputs,proto_fixture_inputs,proto_module_calls;
    uint64_t proto_fallbacks,proto_invalid_results,proto_oversized;
};

class GiuliaModem : public IOService {
    OSDeclareDefaultStructors(GiuliaModem)
    IOLock *lock;
    struct gm_service_buffers *b;
    bool workerRunning,closing,pinned,ownsTerminal,autoAttempted;
    int mode;
    static int enqueueAction(void *,const struct gmr_action *);
    static int enqueuePacket(void *,const struct gmr_identity *,const uint8_t *,size_t);
    static uint64_t checkResponse(void *,const struct gmr_action *,const struct gmr_event *);
    static void worker(void *,wait_result_t);
    void reportLocked();
    void tickLocked();
    void executeLocked();
    int controlLocked(uint32_t,uint64_t);
    void queueStopLocked();
    void applyStopLocked();
    bool packetCurrentLocked();
    uint64_t checkProtocolLocked(const struct gmr_action *,const struct gmr_event *,uint32_t);
    void protocolFixturesLocked();
public:
    bool init(OSDictionary *dictionary=NULL) override;
    bool start(IOService *) override;
    void stop(IOService *) override;
    void free() override;
    int submit(const struct gmr_command *);
    int snapshot(struct gmr_snapshot *);
    int terminal(const char *);
    int control(uint32_t,uint64_t);
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
    ops.qmi_check=checkResponse;
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

uint64_t GiuliaModem::checkResponse(void *cookie,const struct gmr_action *action,
    const struct gmr_event *event)
{
    return ((GiuliaModem *)cookie)->checkProtocolLocked(action,event,GMR_HOTLOAD_ORIGIN_NATIVE);
}

uint64_t GiuliaModem::checkProtocolLocked(const struct gmr_action *action,
    const struct gmr_event *event,uint32_t origin)
{
    if(origin==GMR_HOTLOAD_ORIGIN_NATIVE)b->proto_native_inputs++;
    else b->proto_fixture_inputs++;
    uint64_t baseline=gmr_proto_check(action->qmi_kind,action->id.txn,event->payload,event->bytes);
    uint64_t response=baseline,request[GMR_HOTLOAD_MAX_WORDS];
    uint32_t words=gmr_hotload_pack(request,action->qmi_kind,action->id.txn,
        action->id.opid,origin,event->payload,event->bytes);
    if(!words) {
        b->proto_oversized++;b->proto_fallbacks++;return baseline;
    }
    int result=giulia_modem_hotload_decide(action->id.epoch,request,words,&response);
    if(result) {b->proto_fallbacks++;return baseline;}
    b->proto_module_calls++;
    /* The resident result remains a safety floor. A module may conservatively
     * reject a valid frame, but cannot hide/relabel a native failure or invent
     * a remote modem failure that the actual result TLV did not contain. */
    if(action->id.epoch!=b->runtime.state.epoch || !gmr_proto_result_valid(response) ||
        ((baseline&255u)!=GMQ_OK && response!=baseline) ||
        ((baseline&255u)==GMQ_OK && (response&255u)==GMQ_REMOTE)) {
        b->proto_invalid_results++;b->proto_fallbacks++;return baseline;
    }
    return response;
}

void GiuliaModem::protocolFixturesLocked()
{
    /* Independent wire oracles, not builder/parser-generated expectations.
     * These never enter gmr_event or mutate any hardware/business state. */
    struct fixture {const char *name;unsigned bytes;uint64_t expected;uint8_t payload[24];};
    static const struct fixture fixtures[]={
        {"success",14,0,{2,0x34,0x12,0x21,0,7,0,2,4,0,0,0,0,0}},
        {"remote",14,0x2a04,{2,0x34,0x12,0x21,0,7,0,2,4,0,1,0,0x2a,0}},
        {"transaction",14,3,{2,0x35,0x12,0x21,0,7,0,2,4,0,0,0,0,0}},
        {"message",14,3,{2,0x34,0x12,0x22,0,7,0,2,4,0,0,0,0,0}},
        {"short-header",6,2,{2,0x34,0x12,0x21,0,7}},
        {"body-length",13,2,{2,0x34,0x12,0x21,0,7,0,2,4,0,0,0,0}},
        {"truncated-tlv",10,2,{2,0x34,0x12,0x21,0,3,0,2,4,0}},
        {"duplicate-result",21,2,{2,0x34,0x12,0x21,0,14,0,2,4,0,0,0,0,0,2,4,0,0,0,0,0}},
        {"missing-result",7,5,{2,0x34,0x12,0x21,0,0,0}},
        {"result-short",13,2,{2,0x34,0x12,0x21,0,6,0,2,3,0,0,0,0}},
        {"result-long",15,2,{2,0x34,0x12,0x21,0,8,0,2,5,0,0,0,0,0,0}},
        {"invalid-status",14,2,{2,0x34,0x12,0x21,0,7,0,2,4,0,2,0,0,0}},
        {"success-with-error",14,2,{2,0x34,0x12,0x21,0,7,0,2,4,0,0,0,1,0}},
        {"unknown-unique",18,0,{2,0x34,0x12,0x21,0,11,0,2,4,0,0,0,0,0,0xfe,1,0,0x5a}},
        {"unknown-duplicate",22,2,{2,0x34,0x12,0x21,0,15,0,2,4,0,0,0,0,0,0xfe,1,0,0x5a,0xfe,1,0,0x5a}},
        {"unknown-empty",17,0,{2,0x34,0x12,0x21,0,10,0,2,4,0,0,0,0,0,0xfe,0,0}},
        {"invalid-flags",14,2,{3,0x34,0x12,0x21,0,7,0,2,4,0,0,0,0,0}},
        {"only-unknown",11,5,{2,0x34,0x12,0x21,0,4,0,0xfe,1,0,0x5a}},
    };
    const unsigned fixedCases=(unsigned)(sizeof(fixtures)/sizeof(fixtures[0]));
    const unsigned cases=fixedCases+4;
    b->snapshot=b->runtime.state;
    uint64_t epoch=b->runtime.state.epoch,moduleBefore=b->proto_module_calls;
    uint64_t fallbackBefore=b->proto_fallbacks,nativeBefore=b->proto_native_inputs;
    unsigned passed=0;
    for(unsigned i=0;i<fixedCases;i++) {
        struct gmr_action *a=&b->proto_action;struct gmr_event *v=&b->proto_event;
        bzero(a,sizeof(*a));bzero(v,sizeof(*v));
        a->kind=GMR_ACT_QMI;a->qmi_kind=GMR_QMI_STOP;a->id.epoch=epoch;
        a->id.opid=i+1u;a->id.txn=0x1234;
        v->bytes=(uint16_t)fixtures[i].bytes;
        memcpy(v->payload,fixtures[i].payload,v->bytes);
        uint64_t moduleCase=b->proto_module_calls,fallbackCase=b->proto_fallbacks;
        uint64_t actual=checkProtocolLocked(a,v,GMR_HOTLOAD_ORIGIN_FIXTURE);
        bool pass=actual==fixtures[i].expected;if(pass)passed++;
        IOLog("GIULIA MODEM PROTO: source=fixture case=%s epoch=%llu expected=0x%llx actual=0x%llx pass=%u module_delta=%llu fallback_delta=%llu hardware_event=0\n",
              fixtures[i].name,epoch,fixtures[i].expected,actual,(unsigned)pass,
              b->proto_module_calls-moduleCase,b->proto_fallbacks-fallbackCase);
    }
    for(unsigned i=0;i<4;i++) {
        /* 223/224/225-byte complete frames and one malformed 225-byte frame.
         * The final TLV value itself reaches the bridge's exact size bound. */
        struct gmr_action *a=&b->proto_action;struct gmr_event *v=&b->proto_event;
        bzero(a,sizeof(*a));bzero(v,sizeof(*v));
        a->kind=GMR_ACT_QMI;a->qmi_kind=GMR_QMI_STOP;a->id.epoch=epoch;
        a->id.opid=fixedCases+i+1u;a->id.txn=0x1234;
        unsigned bytes=i<3?223u+i:225u;
        uint64_t expected=i==3?2u:0u;
        memcpy(v->payload,fixtures[0].payload,14);v->bytes=(uint16_t)bytes;
        v->payload[5]=(uint8_t)(bytes-7u);v->payload[6]=0;
        v->payload[14]=0xfe;v->payload[15]=(uint8_t)(bytes-17u+(i==3));v->payload[16]=0;
        for(unsigned j=17;j<bytes;j++)v->payload[j]=0x5a;
        uint64_t moduleCase=b->proto_module_calls,fallbackCase=b->proto_fallbacks;
        uint64_t actual=checkProtocolLocked(a,v,GMR_HOTLOAD_ORIGIN_FIXTURE);
        bool pass=actual==expected;if(pass)passed++;
        IOLog("GIULIA MODEM PROTO: source=fixture case=boundary-%u malformed=%u epoch=%llu expected=0x%llx actual=0x%llx pass=%u module_delta=%llu fallback_delta=%llu hardware_event=0\n",
              bytes,(unsigned)(i==3),epoch,expected,actual,(unsigned)pass,
              b->proto_module_calls-moduleCase,b->proto_fallbacks-fallbackCase);
    }
    unsigned unchanged=!memcmp(&b->snapshot,&b->runtime.state,sizeof(b->snapshot));
    IOLog("GIULIA MODEM PROTO: source=fixture cases=%u passed=%u failed=%u epoch=%llu module_delta=%llu fallback_delta=%llu native_delta=%llu state_unchanged=%u hardware_event=0\n",
          cases,passed,cases-passed,epoch,b->proto_module_calls-moduleBefore,
          b->proto_fallbacks-fallbackBefore,b->proto_native_inputs-nativeBefore,unchanged);
}

int GiuliaModem::submit(const struct gmr_command *command)
{
    if(!command || command->bytes>GMR_PAYLOAD)return GMR_ARGUMENT;
    IOLockLock(lock);
    int result=GMR_OK;
    if(closing || !mode || !workerRunning)result=GMR_STATE;
    else if(command->kind==GMR_CMD_STOP)queueStopLocked();
    else if(b->stop_requested)result=GMR_BUSY;
    else if(command->kind==GMR_CMD_SSR &&
        !__atomic_load_n(&modemPlatform,__ATOMIC_ACQUIRE))result=GMR_UNSUPPORTED;
    else if(b->command_count==16)result=GMR_FULL;
    else {
        gmr_queue_capture(&b->commands[(b->command_head+b->command_count)%16],
                          command,&b->runtime.state);
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
    IOLog("GIULIA MODEM: runtime_resident=1 runtime_reloadable=0 provider_required=1 hardware_validation=unverified queued_commands=%u start_pending=%u stop_pending=%u\n",
          b->command_count,b->start_requested,b->stop_requested);
    IOLog("GIULIA MODEM: storage_running=%u ipc_prepared=%u firmware_running=%u ipc_running=%u uncertain_actions=%u queued_events=%u restart_pending=%u\n",
          s->storage_running,s->ipc_prepared,s->firmware_running,s->ipc_running,
          s->uncertain_actions,s->queued_events,(unsigned)s->restart_requested);
    IOLog("GIULIA MODEM: proto_native_inputs=%llu proto_fixture_inputs=%llu proto_module_calls=%llu proto_fallbacks=%llu proto_invalid_results=%llu proto_oversized=%llu\n",
          b->proto_native_inputs,b->proto_fixture_inputs,b->proto_module_calls,
          b->proto_fallbacks,b->proto_invalid_results,b->proto_oversized);
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
    if(!strcmp(command,"modem.proto")) {
        int debug=0;PE_parse_boot_argn("giulia_debug",&debug,sizeof(debug));
        IOLockLock(lock);
        if(debug==1 && !closing && workerRunning)protocolFixturesLocked();
        else IOLog("GIULIA MODEM PROTO: rejected=offline-or-debug-disabled\n");
        IOLockUnlock(lock);return 1;
    }
    unsigned operation=!strcmp(command,"modem.start")?GMP_CONTROL_START:
        !strcmp(command,"modem.stop")?GMP_CONTROL_STOP:
        !strcmp(command,"modem.suspend")?GMP_CONTROL_SUSPEND:
        !strcmp(command,"modem.resume")?GMP_CONTROL_RESUME:0;
    if(!operation)return 0;
    IOLockLock(lock);
    int result=controlLocked(operation,b->runtime.state.epoch);
    IOLog("GIULIA MODEM: command=%s queue_result=%d completion=asynchronous\n",command,result);
    IOLockUnlock(lock);return 1;
}

int GiuliaModem::controlLocked(uint32_t operation,uint64_t expectedEpoch)
{
    if(operation<GMP_CONTROL_START || operation>GMP_CONTROL_RESUME)return GMR_ARGUMENT;
    int debug=0;PE_parse_boot_argn("giulia_debug",&debug,sizeof(debug));
    if(debug!=1 || !mode || closing || !workerRunning)return GMR_STATE;
    if(expectedEpoch!=b->runtime.state.epoch)return GMR_STALE;
    if(operation==GMP_CONTROL_STOP){queueStopLocked();return GMR_OK;}
    if(b->stop_requested)return GMR_BUSY;
    if(operation==GMP_CONTROL_START) {
        if(!__atomic_load_n(&modemPlatform,__ATOMIC_ACQUIRE))return GMR_UNSUPPORTED;
        if(b->start_requested)return GMR_BUSY;
        if(b->runtime.state.lifecycle!=GMR_OFF || b->runtime.state.pending_count ||
            b->runtime.state.resources_owned || b->runtime.state.uncertain_actions)return GMR_STATE;
        if(b->command_count)return GMR_BUSY;
        b->start_epoch=expectedEpoch;b->start_requested=1;return GMR_OK;
    }
    if(b->command_count==16)return GMR_FULL;
    struct gmr_queued_command *queued=&b->commands[(b->command_head+b->command_count)%16];
    struct gmr_command *c=&queued->command;
    bzero(c,sizeof(*c));c->kind=operation==GMP_CONTROL_SUSPEND?GMR_CMD_SUSPEND:GMR_CMD_RESUME;
    c->slot=GMR_NO_SLOT;c->bearer=GMR_NO_BEARER;
    gmr_queue_capture(queued,c,&b->runtime.state);
    b->command_count++;return GMR_OK;
}

void GiuliaModem::queueStopLocked()
{
    /* STOP has its own admission slot. Retire unexecuted commands, including
     * queued SSR, without touching the core from the terminal/client thread. */
    IOLog("GIULIA MODEM: stop barrier cancelled_commands=%u cancelled_start=%u\n",
          b->command_count,b->start_requested);
    b->command_count=0;b->command_head=0;b->start_requested=0;
    b->stop_requested=1;autoAttempted=true;
}

void GiuliaModem::applyStopLocked()
{
    if(!b->stop_requested)return;
    b->stop_requested=0;
    /* IOService teardown retains an already isolated owner until reboot, as
     * before. It must not turn service removal into an implicit recovery. */
    if(closing && (b->runtime.state.lifecycle==GMR_OFF ||
                   b->runtime.state.lifecycle==GMR_ISOLATED))return;
    struct gmr_command *c=&b->commands[0].command;bzero(c,sizeof(*c));
    c->kind=GMR_CMD_STOP;c->slot=GMR_NO_SLOT;c->bearer=GMR_NO_BEARER;
    int result=gmr_submit(&b->runtime,c);
    IOLog("GIULIA MODEM: stop submitted result=%d\n",result);
}

int GiuliaModem::control(uint32_t operation,uint64_t expectedEpoch)
{
    IOLockLock(lock);int result=controlLocked(operation,expectedEpoch);
    IOLockUnlock(lock);return result;
}

bool GiuliaModem::packetCurrentLocked()
{
    const struct gmr_identity *id=&b->packet.id;
    if(b->runtime.state.lifecycle!=GMR_ONLINE || id->epoch!=b->runtime.state.epoch ||
        id->bearer>=GMR_BEARERS || id->slot>=GMR_SLOTS)return false;
    const struct gmr_bearer *bearer=&b->runtime.state.bearers[id->bearer];
    if(bearer->state!=GMR_BEARER_UP || bearer->slot!=id->slot ||
        bearer->slot_generation!=id->slot_generation ||
        b->runtime.state.slots[id->slot].generation!=id->slot_generation ||
        bearer->endpoint_generation!=id->endpoint_generation ||
        bearer->generation!=id->bearer_generation)return false;
    for(unsigned i=0;i<GMR_EVENTS;i++)if(b->runtime.rx_pending[i].used &&
        gmr_identity_equal(&b->runtime.rx_pending[i].id,id))return true;
    return false;
}

void GiuliaModem::tickLocked()
{
    /* Apply an accepted STOP before completions/timers can finish SSR and
     * auto-restart. Only the worker calls this function or mutates gmr. */
    applyStopLocked();
    uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
    (void)gmr_tick(&b->runtime,ns/1000000);
}

void GiuliaModem::executeLocked()
{
    tickLocked();
    if(!closing && mode==2 && !autoAttempted && __atomic_load_n(&modemPlatform,__ATOMIC_ACQUIRE)) {
        autoAttempted=true;b->start_epoch=b->runtime.state.epoch;b->start_requested=1;
    }
    if(b->start_requested) {
        b->start_requested=0;
        /* No provider means no action was issued and no ownership became
         * uncertain. Do not enter ACQUIRING only to isolate on enqueue failure;
         * a provider registered later must still be able to start from OFF. */
        int result=b->start_epoch!=b->runtime.state.epoch?GMR_STALE:
            !__atomic_load_n(&modemPlatform,__ATOMIC_ACQUIRE)?GMR_UNSUPPORTED:
            gmr_start(&b->runtime);
        IOLog("GIULIA MODEM: start submitted result=%d\n",result);
    }
    if(b->command_count) {
        const struct gmr_queued_command *queued=&b->commands[b->command_head];
        int result=gmr_queue_current(queued,&b->runtime.state);
        if(!result && queued->command.kind==GMR_CMD_SSR &&
            !__atomic_load_n(&modemPlatform,__ATOMIC_ACQUIRE))result=GMR_UNSUPPORTED;
        if(!result)result=gmr_submit(&b->runtime,&queued->command);
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
            havePacket=s->packetCurrentLocked();
        }
        IOLockUnlock(s->lock);
        const struct gmp_platform *p=__atomic_load_n(&modemPlatform,__ATOMIC_ACQUIRE);
        if(have) {
            int result=p?p->submit(p->ctx,&s->b->action):GMR_UNSUPPORTED;
            if(result>GMR_OK && result<=GMR_UNCERTAIN) {
                IOLockLock(s->lock);
                (void)gmr_reject(&s->b->runtime,&s->b->action,result);
                s->tickLocked();IOLockUnlock(s->lock);
            } else if(result) {
                /* A contract-violating provider cannot prove non-acceptance.
                 * Preserve uncertainty instead of letting an invalid result
                 * be discarded by gmr_event() until a later timeout. */
                bzero(&s->b->event,sizeof(s->b->event));
                s->b->event.kind=s->b->action.kind==GMR_ACT_QMI?GMR_EV_QMI_RESPONSE:GMR_EV_COMPLETE;
                s->b->event.action_kind=s->b->action.kind;
                s->b->event.id=s->b->action.id;s->b->event.status=GMR_PROTOCOL;
                IOLockLock(s->lock);s->tickLocked();(void)gmr_event(&s->b->runtime,&s->b->event);IOLockUnlock(s->lock);
            }
        }
        if(havePacket) {
            /* The action above may have isolated the epoch and retired RX.
             * This worker alone mutates the core; revalidate after that event
             * before crossing into the native interface, outside the lock. */
            IOLockLock(s->lock);s->applyStopLocked();
            havePacket=s->packetCurrentLocked();IOLockUnlock(s->lock);
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
        /* Nothing has been published and no worker can own resources. A later
         * matching attempt may retry. Published/isolated owners stay pinned. */
        workerRunning=false;ownsTerminal=false;
        __atomic_store_n(&modemOwner,0u,__ATOMIC_RELEASE);
        release();return false;
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
    IOLockLock(lock);closing=true;queueStopLocked();
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
extern "C" int giulia_modem_control(uint32_t operation,uint64_t expectedEpoch)
{
    GiuliaModem *s=__atomic_load_n(&modemService,__ATOMIC_ACQUIRE);
    return s && __atomic_load_n(&modemOnline,__ATOMIC_ACQUIRE)?
        s->control(operation,expectedEpoch):GMR_STATE;
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
