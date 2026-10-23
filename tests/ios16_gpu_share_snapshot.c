/* Host fixture only: no phone, no HVC, no SMC, no carveout access.
 *
 * The snapshot logic only reads recorded fields, so the fixture builds the
 * `ggsh_state` / `ggrm_state` values it would have after a successful
 * transaction and checks every refusal. It does not prove that a real
 * transaction ever reaches those values.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_share_snapshot.h"

static unsigned cases;

struct fixture {
    struct ggsn_state snapshot;
    struct ggsh_state live;
    struct ggrm_state channel;
};

static void live_init(struct fixture *f)
{
    memset(f,0,sizeof(*f));
    f->live.initialized=1;
    f->live.attempted=1;
    f->live.phase=GGSH_COMPLETE;
    f->live.shared=1;
    f->live.assigned=1;
    f->live.epoch=0x1000u;
    f->live.parcel=0x99u;
    f->live.identity.epoch=f->live.epoch;
    f->live.identity.self_vmid=5;
    f->live.identity.peer_vmid=9;
    f->live.channel=&f->channel;
    f->channel.epoch=f->live.epoch;
    f->channel.next_sequence=42;
}

static void test_complete_snapshot(void)
{
    struct fixture f;
    struct ggfn_share share;
    live_init(&f);
    assert(ggsn_take(&f.snapshot,&f.live)==GGSN_OK);
    assert(f.snapshot.taken==1 && !f.snapshot.invalidated);
    assert(f.snapshot.frozen.epoch==0x1000u && f.snapshot.frozen.parcel==0x99u);
    assert(f.snapshot.frozen.self_vmid==5 && f.snapshot.frozen.peer_vmid==9);
    assert(f.snapshot.frozen.sequence==42);
    assert(f.snapshot.frozen.carveout_physical==GGF_CARVEOUT_PHYSICAL);
    assert(f.snapshot.frozen.carveout_bytes==GGF_CARVEOUT_BYTES);
    assert(f.snapshot.frozen.proofs==GGSN_PROOF_REQUIRED);
    memset(&share,0,sizeof(share));
    assert(ggsn_share(&f.snapshot,&f.live,&share)==GGSN_OK);
    assert(share.epoch==0x1000u && share.parcel==0x99u);
    assert(share.self_vmid==5 && share.peer_vmid==9);
    assert(share.complete==1 && share.invalid==0);
    ++cases;
    /* The frozen copy never follows later mutation of the live state. */
    f.live.parcel=0x77u;
    assert(f.snapshot.frozen.parcel==0x99u);
    assert(ggsn_share(&f.snapshot,&f.live,&share)==GGSN_STALE);
    assert(f.snapshot.frozen.parcel==0x99u && f.snapshot.invalidated==1);
    ++cases;
}

static void test_take_once(void)
{
    struct fixture f;
    live_init(&f);
    assert(ggsn_take(&f.snapshot,&f.live)==GGSN_OK);
    f.live.identity.self_vmid=6;
    assert(ggsn_take(&f.snapshot,&f.live)==GGSN_TAKEN);
    assert(f.snapshot.frozen.self_vmid==5);
    ++cases;
}

static void test_incomplete_refused(void)
{
    struct fixture f;
    struct ggsn_state *s=&f.snapshot;
    live_init(&f);
    f.live.phase=GGSH_SHARE;
    assert(ggsn_take(s,&f.live)==GGSN_NOT_COMPLETE);
    live_init(&f);
    f.live.shared=0;
    assert(ggsn_take(&f.snapshot,&f.live)==GGSN_NOT_COMPLETE);
    live_init(&f);
    f.live.assigned=0;
    assert(ggsn_take(&f.snapshot,&f.live)==GGSN_NOT_COMPLETE);
    live_init(&f);
    f.live.busy=1;
    assert(ggsn_take(&f.snapshot,&f.live)==GGSN_NOT_COMPLETE);
    live_init(&f);
    f.live.loss=1;
    assert(ggsn_take(&f.snapshot,&f.live)==GGSN_NOT_COMPLETE);
    live_init(&f);
    f.live.invalid=1;
    assert(ggsn_take(&f.snapshot,&f.live)==GGSN_NOT_COMPLETE);
    live_init(&f);
    f.live.channel=0;
    assert(ggsn_take(&f.snapshot,&f.live)==GGSN_NOT_COMPLETE);
    assert(!f.snapshot.taken);
    ++cases;
}

static void test_channel_refused(void)
{
    struct fixture f;
    live_init(&f);
    f.channel.epoch=0x2000u;
    assert(ggsn_take(&f.snapshot,&f.live)==GGSN_CHANNEL);
    live_init(&f);
    f.channel.quarantined=1;
    assert(ggsn_take(&f.snapshot,&f.live)==GGSN_CHANNEL);
    assert(!f.snapshot.taken);
    ++cases;
}

static void test_identity_refused(void)
{
    struct fixture f;
    live_init(&f);
    f.live.identity.epoch=0x3000u;
    assert(ggsn_take(&f.snapshot,&f.live)==GGSN_IDENTITY);
    live_init(&f);
    f.live.identity.invalid=1;
    assert(ggsn_take(&f.snapshot,&f.live)==GGSN_IDENTITY);
    live_init(&f);
    f.live.identity.self_vmid=0;
    assert(ggsn_take(&f.snapshot,&f.live)==GGSN_IDENTITY);
    live_init(&f);
    f.live.identity.self_vmid=64;
    assert(ggsn_take(&f.snapshot,&f.live)==GGSN_IDENTITY);
    live_init(&f);
    f.live.identity.peer_vmid=f.live.identity.self_vmid;
    assert(ggsn_take(&f.snapshot,&f.live)==GGSN_IDENTITY);
    assert(!f.snapshot.taken);
    ++cases;
}

static void test_parcel_refused(void)
{
    struct fixture f;
    live_init(&f);
    f.live.parcel=0;
    assert(ggsn_take(&f.snapshot,&f.live)==GGSN_PARCEL);
    assert(!f.snapshot.taken);
    ++cases;
}

static void test_stale_is_permanent(void)
{
    struct fixture f;
    struct ggfn_share share;
    live_init(&f);
    assert(ggsn_take(&f.snapshot,&f.live)==GGSN_OK);
    f.live.channel->quarantined=1;
    assert(ggsn_share(&f.snapshot,&f.live,&share)==GGSN_STALE);
    assert(f.snapshot.invalidated && f.snapshot.refused==1);
    f.live.channel->quarantined=0;
    assert(ggsn_share(&f.snapshot,&f.live,&share)==GGSN_STALE);
    assert(f.snapshot.refused==1);
    assert(ggsn_take(&f.snapshot,&f.live)==GGSN_TAKEN);
    ++cases;
}

static void test_binding_and_arguments(void)
{
    struct fixture f;
    struct ggsn_binding binding;
    struct ggfn_share share;
    live_init(&f);
    binding.snapshot=&f.snapshot;
    binding.live=&f.live;
    assert(ggsn_share_cb(&binding,&share)==GGSN_ARGUMENT); /* nothing taken yet */
    assert(ggsn_take(&f.snapshot,&f.live)==GGSN_OK);
    assert(ggsn_share_cb(&binding,&share)==GGSN_OK && share.complete==1);
    assert(ggsn_share_cb(0,&share)==GGSN_ARGUMENT);
    assert(ggsn_take(0,&f.live)==GGSN_ARGUMENT);
    assert(ggsn_take(&f.snapshot,0)==GGSN_ARGUMENT);
    assert(ggsn_share(0,&f.live,&share)==GGSN_ARGUMENT);
    assert(ggsn_share(&f.snapshot,0,&share)==GGSN_ARGUMENT);
    assert(ggsn_share(&f.snapshot,&f.live,0)==GGSN_ARGUMENT);
    assert(ggsn_invalidate(0)==GGSN_ARGUMENT);
    assert(!strcmp(ggsn_code_name(GGSN_STALE),"stale"));
    ++cases;
}

static void test_invalidate_before_take(void)
{
    struct fixture f;
    struct ggfn_share share;
    live_init(&f);
    assert(ggsn_invalidate(&f.snapshot)==GGSN_OK);
    assert(ggsn_take(&f.snapshot,&f.live)==GGSN_STALE);
    assert(!f.snapshot.taken);
    assert(ggsn_share(&f.snapshot,&f.live,&share)==GGSN_ARGUMENT);
    ++cases;
}

int main(void)
{
    test_complete_snapshot();
    test_take_once();
    test_incomplete_refused();
    test_channel_refused();
    test_identity_refused();
    test_parcel_refused();
    test_stale_is_permanent();
    test_binding_and_arguments();
    test_invalidate_before_take();
    printf("SHARE SNAPSHOT FIXTURE: %u cases passed (recorded-state logic only; no hardware evidence)\n",cases);
    return 0;
}
