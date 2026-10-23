/* Host fixture only: no phone, no HVC, no SMC, no carveout access.
 *
 * The gate callbacks are the authority boundary: this fixture proves the unit
 * refuses before `ggsh_init()` when live/identity/authority is missing, and that
 * a refusal leaves the share state untouched so a later attempt starts clean.
 * The transaction itself (`ggsr_finish`) is covered by the share unit's own
 * scripted test; here it can only be checked for refusing an unprepared state.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_share_runtime.h"

static unsigned cases;
static uint8_t control_area[16384+16384];

struct fixture {
    struct ggsr_state runtime;
    struct ggsh_owner owner;
    struct ggrm_state channel;
    struct ggsr_gate gate;
    unsigned live,identity,authority,self_vmid,peer_vmid;
    unsigned live_calls,identity_calls,authority_calls;
    unsigned order[16],order_count;
    uint64_t authority_epoch,authority_physical;
    uint16_t authority_self,authority_peer;
    uint32_t authority_bytes;
};

static int f_live(void *cookie,uint64_t epoch)
{
    struct fixture *f=cookie;
    (void)epoch;
    ++f->live_calls;
    if(f->order_count<16)f->order[f->order_count++]=1;
    return (int)f->live;
}
static int f_identity(void *cookie,uint64_t epoch,uint16_t *self_vmid,uint16_t *peer_vmid)
{
    struct fixture *f=cookie;
    (void)epoch;
    ++f->identity_calls;
    if(f->order_count<16)f->order[f->order_count++]=2;
    if(!f->identity)return 0;
    *self_vmid=(uint16_t)f->self_vmid;
    *peer_vmid=(uint16_t)f->peer_vmid;
    return 1;
}
static int f_authority(void *cookie,uint64_t epoch,uint16_t self_vmid,uint16_t peer_vmid,
    uint64_t physical,uint32_t bytes)
{
    struct fixture *f=cookie;
    ++f->authority_calls;
    if(f->order_count<16)f->order[f->order_count++]=3;
    f->authority_epoch=epoch;
    f->authority_physical=physical;
    f->authority_bytes=bytes;
    f->authority_self=self_vmid;
    f->authority_peer=peer_vmid;
    return (int)f->authority;
}

static int o_valid(void *cookie,uint64_t epoch){(void)cookie;(void)epoch;return 1;}
static int o_authorize(void *cookie,uint64_t epoch,uint16_t self_vmid,uint16_t peer_vmid,
    uint64_t physical,uint32_t bytes)
{
    (void)cookie;(void)epoch;(void)self_vmid;(void)peer_vmid;(void)physical;(void)bytes;return 1;
}
static int o_publish(void *cookie,const void *bytes,size_t length)
{(void)cookie;(void)bytes;(void)length;return 1;}
static int rm_valid(void *cookie,uint64_t epoch){(void)cookie;(void)epoch;return 1;}
static int rm_send(void *cookie,const void *bytes,size_t length,unsigned flags)
{(void)cookie;(void)bytes;(void)length;(void)flags;return 0;}
static int rm_receive(void *cookie,void *bytes,size_t length,size_t *out)
{(void)cookie;(void)bytes;(void)length;(void)out;return 1;}
static uint64_t rm_now(void *cookie){(void)cookie;return 0;}
static int rm_delay(void *cookie,unsigned us){(void)cookie;(void)us;return 0;}
static int rm_notify(void *cookie,uint32_t id,const void *bytes,size_t length)
{(void)cookie;(void)id;(void)bytes;(void)length;return 1;}
static int scm_call(void *cookie,struct ggs_regs *regs){(void)cookie;(void)regs;return 1;}
static int scm_delay(void *cookie,unsigned ms){(void)cookie;(void)ms;return 0;}
static int scm_prereq(void *cookie,unsigned operation){(void)cookie;(void)operation;return 0;}

static void fixture_init(struct fixture *f)
{
    uintptr_t control=((uintptr_t)control_area+16383u)&~(uintptr_t)16383u;
    memset(f,0,sizeof(*f));
    memset(control_area,0,sizeof(control_area));
    f->live=1;
    f->identity=1;
    f->authority=1;
    f->self_vmid=5;
    f->peer_vmid=9;
    f->owner.valid=o_valid;
    f->owner.authorize=o_authorize;
    f->owner.publish=o_publish;
    f->owner.rm.cookie=f;
    f->owner.rm.valid=rm_valid;
    f->owner.rm.send=rm_send;
    f->owner.rm.receive=rm_receive;
    f->owner.rm.now_us=rm_now;
    f->owner.rm.delay_us=rm_delay;
    f->owner.rm.notification=rm_notify;
    f->owner.scm.cookie=f;
    f->owner.scm.call=scm_call;
    f->owner.scm.delay_ms=scm_delay;
    f->owner.scm.prerequisites=scm_prereq;
    f->owner.control=(void *)control;
    f->owner.control_pa=0x10000000u;
    f->owner.control_bytes=16384;
    f->channel.epoch=0x1000u;
    f->channel.next_sequence=7;
    f->gate.cookie=f;
    f->gate.live=f_live;
    f->gate.identity=f_identity;
    f->gate.authority=f_authority;
}

static void test_prepare_success(void)
{
    struct fixture f;
    fixture_init(&f);
    assert(ggsr_prepare(&f.runtime,&f.owner,&f.channel,0x1000u,&f.gate)==GGSR_OK);
    assert(f.runtime.prepared==1 && f.runtime.finished==0 && f.runtime.runs==0);
    assert(f.runtime.self_vmid==5 && f.runtime.peer_vmid==9);
    assert(f.runtime.share.initialized!=0 && f.runtime.share.epoch==0x1000u);
    assert(f.runtime.binding.snapshot==&f.runtime.snapshot);
    assert(f.runtime.binding.live==&f.runtime.share);
    assert(f.live_calls==1 && f.identity_calls==1 && f.authority_calls==1);
    assert(f.order_count==3 && f.order[0]==1 && f.order[1]==2 && f.order[2]==3);
    /* The authority proof is asked about exactly this transaction. */
    assert(f.authority_epoch==0x1000u && f.authority_self==5 && f.authority_peer==9);
    assert(f.authority_physical==GGF_CARVEOUT_PHYSICAL &&
        f.authority_bytes==GGF_CARVEOUT_BYTES);
    ++cases;
    assert(ggsr_prepare(&f.runtime,&f.owner,&f.channel,0x1000u,&f.gate)==GGSR_BUSY);
    ++cases;
}

static void test_live_refused(void)
{
    struct fixture f;
    fixture_init(&f);
    f.live=0;
    assert(ggsr_prepare(&f.runtime,&f.owner,&f.channel,0x1000u,&f.gate)==GGSR_NOT_LIVE);
    assert(f.runtime.prepared==0 && f.runtime.share.initialized==0);
    assert(f.identity_calls==0 && f.authority_calls==0);
    assert(f.runtime.refused==1 && f.runtime.last==GGSR_NOT_LIVE);
    /* Nothing was initialised, so a later attempt with a live owner is clean. */
    f.live=1;
    assert(ggsr_prepare(&f.runtime,&f.owner,&f.channel,0x1000u,&f.gate)==GGSR_OK);
    assert(f.runtime.prepared==1 && f.runtime.refused==1);
    ++cases;
}

static void test_identity_refused(void)
{
    struct fixture f;
    fixture_init(&f);
    f.identity=0;
    assert(ggsr_prepare(&f.runtime,&f.owner,&f.channel,0x1000u,&f.gate)==GGSR_IDENTITY);
    assert(f.authority_calls==0 && f.runtime.share.initialized==0);
    fixture_init(&f);
    f.self_vmid=0;
    assert(ggsr_prepare(&f.runtime,&f.owner,&f.channel,0x1000u,&f.gate)==GGSR_IDENTITY);
    fixture_init(&f);
    f.self_vmid=64;
    assert(ggsr_prepare(&f.runtime,&f.owner,&f.channel,0x1000u,&f.gate)==GGSR_IDENTITY);
    fixture_init(&f);
    f.peer_vmid=f.self_vmid;
    assert(ggsr_prepare(&f.runtime,&f.owner,&f.channel,0x1000u,&f.gate)==GGSR_IDENTITY);
    assert(f.authority_calls==0 && f.runtime.share.initialized==0);
    ++cases;
}

static void test_authority_refused(void)
{
    struct fixture f;
    fixture_init(&f);
    f.authority=0;
    assert(ggsr_prepare(&f.runtime,&f.owner,&f.channel,0x1000u,&f.gate)==GGSR_AUTHORITY);
    assert(f.runtime.share.initialized==0 && f.runtime.prepared==0);
    assert(f.runtime.snapshot.taken==0 && f.runtime.refused==1);
    /* No init happened, so the object is still usable with real authority. */
    f.authority=1;
    assert(ggsr_prepare(&f.runtime,&f.owner,&f.channel,0x1000u,&f.gate)==GGSR_OK);
    assert(f.runtime.prepared==1);
    ++cases;
}

static void test_channel_refused(void)
{
    struct fixture f;
    fixture_init(&f);
    f.channel.epoch=0x2000u;
    assert(ggsr_prepare(&f.runtime,&f.owner,&f.channel,0x1000u,&f.gate)==GGSR_CHANNEL);
    assert(f.live_calls==0);
    fixture_init(&f);
    f.channel.busy=1;
    assert(ggsr_prepare(&f.runtime,&f.owner,&f.channel,0x1000u,&f.gate)==GGSR_CHANNEL);
    fixture_init(&f);
    f.channel.quarantined=1;
    assert(ggsr_prepare(&f.runtime,&f.owner,&f.channel,0x1000u,&f.gate)==GGSR_CHANNEL);
    fixture_init(&f);
    f.channel.next_sequence=65530u;
    assert(ggsr_prepare(&f.runtime,&f.owner,&f.channel,0x1000u,&f.gate)==GGSR_CHANNEL);
    ++cases;
}

static void test_arguments(void)
{
    struct fixture f;
    fixture_init(&f);
    assert(ggsr_prepare(0,&f.owner,&f.channel,0x1000u,&f.gate)==GGSR_ARGUMENT);
    assert(ggsr_prepare(&f.runtime,0,&f.channel,0x1000u,&f.gate)==GGSR_ARGUMENT);
    assert(ggsr_prepare(&f.runtime,&f.owner,0,0x1000u,&f.gate)==GGSR_ARGUMENT);
    assert(ggsr_prepare(&f.runtime,&f.owner,&f.channel,0,&f.gate)==GGSR_ARGUMENT);
    assert(ggsr_prepare(&f.runtime,&f.owner,&f.channel,0x1000u,0)==GGSR_ARGUMENT);
    f.owner.publish=0;
    assert(ggsr_prepare(&f.runtime,&f.owner,&f.channel,0x1000u,&f.gate)==GGSR_ARGUMENT);
    fixture_init(&f);
    f.gate.authority=0;
    assert(ggsr_prepare(&f.runtime,&f.owner,&f.channel,0x1000u,&f.gate)==GGSR_ARGUMENT);
    assert(ggsr_finish(0,1000)==GGSR_ARGUMENT);
    assert(ggsr_finish(&f.runtime,1000)==GGSR_ARGUMENT); /* never prepared */
    assert(!strcmp(ggsr_code_name(GGSR_AUTHORITY),"no-authority"));
    ++cases;
}

int main(void)
{
    test_prepare_success();
    test_live_refused();
    test_identity_refused();
    test_authority_refused();
    test_channel_refused();
    test_arguments();
    printf("SHARE RUNTIME FIXTURE: %u cases passed (gate/order logic only; no hardware evidence)\n",cases);
    return 0;
}
