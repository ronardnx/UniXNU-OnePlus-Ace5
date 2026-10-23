/* Synthetic provider, share transcript and mapper; no kernel or hardware. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_fence_native.h"

#define TEST_VA UINT64_C(0xfffffff010000000) /* host stand-in for a kernel VA */

struct fixture {
    struct ggf_provider provider;
    struct ggf_layout layout;
    unsigned char *queue;
    unsigned char *cpu;
    unsigned observe_calls, barrier_calls, map_calls, audit_calls;
    unsigned live, map_fail, audit_fail, audit_alias, audit_proofs;
    struct ggfn_share share;
};

static int fixture_observe(void *cookie, struct ggf_proof *proof)
{
    struct fixture *f = (struct fixture *)cookie;
    ++f->observe_calls;
    if (!f->live) return 0;
    memset(proof, 0, sizeof(*proof));
    proof->epoch = f->provider.epoch;
    proof->carveout_physical = GGF_CARVEOUT_PHYSICAL;
    proof->carveout_bytes = GGF_CARVEOUT_BYTES;
    proof->queue_physical = f->layout.physical;
    proof->queue_bytes = f->layout.bytes;
    proof->page_bytes = f->layout.page_bytes;
    proof->peer = 3;
    proof->mem_label = 5;
    proof->doorbell_label = 6;
    proof->client = 1;
    proof->proofs = GGFP_REQUIRED;
    return 1;
}
static void fixture_barrier(void *cookie)
{
    struct fixture *f = (struct fixture *)cookie;
    ++f->barrier_calls;
}
static int fixture_share(void *cookie, struct ggfn_share *out)
{
    struct fixture *f = (struct fixture *)cookie;
    *out = f->share;
    return 1;
}
static int fixture_map(void *cookie, const struct ggfc_envelope *envelope,
    struct ggfc_mapping *out)
{
    struct fixture *f = (struct fixture *)cookie;
    ++f->map_calls;
    if (f->map_fail || !envelope || !out) return 0;
    out->handle = f;
    out->base = f->queue;
    out->queue = f->queue + envelope->offset;
    out->physical = envelope->physical;
    out->bytes = envelope->bytes;
    out->local_page_bytes = envelope->local_page_bytes;
    return 1;
}
static int fixture_audit(void *cookie, const struct ggfc_envelope *envelope,
    const struct ggfc_mapping *mapping, struct ggfc_cpu_audit *out)
{
    struct fixture *f = (struct fixture *)cookie;
    struct ggpm_policy policy;
    struct ggfpm_audit audit;
    ++f->audit_calls;
    if (f->audit_fail || !envelope || !mapping || !out) return 0;
    memset(&policy, 0, sizeof(policy));
    policy.abi = GGPM_ABI;
    policy.boot_valid = 1;
    policy.page_bytes = envelope->local_page_bytes;
    policy.mair = UINT64_C(0x4400);
    policy.tcr = (UINT64_C(1) << 30) | (UINT64_C(16) << 16);
    policy.ttbr1 = UINT64_C(0x80120000);
    policy.physmap_base = UINT64_C(0x8c0000000);
    policy.physmap_bytes = UINT64_C(0x80000000);
    policy.managed_base = UINT64_C(0x8c0000000);
    policy.managed_bytes = UINT64_C(0x80000000);
    const uint64_t first = envelope->physical |
        UINT64_C(0x60000000000407);
    const uint64_t last = (envelope->physical + envelope->bytes -
        envelope->local_page_bytes) | UINT64_C(0x60000000000407);
    /* User space cannot own a canonical kernel VA, so the RAM model seals the
     * span with a fixed high-half VA while the mapping identity stays real. */
    if (!ggpm_audit_seal(&policy, (uintptr_t)TEST_VA, envelope->physical,
            envelope->bytes, 1, first, last, &audit)) return 0;
    out->mapping = *mapping;
    if (f->audit_alias) out->mapping.queue = (volatile void *)((uintptr_t)out->mapping.queue + 4);
    out->proofs = f->audit_proofs ? 0 : GGFC_AUDIT_REQUIRED;
    return 1;
}
static void setup(struct fixture *f, struct ggfn_state *s, struct ggfn_inputs *in,
    uint32_t host_page)
{
    memset(f, 0, sizeof(*f));
    memset(s, 0, sizeof(*s));
    memset(in, 0, sizeof(*in));
    assert(ggf_layout(4096, &f->layout));
    /* The CPU mapper only publishes page-aligned bases; model that here. */
    f->queue = aligned_alloc(0x4000, 0x20000);
    f->cpu = aligned_alloc(0x4000, 0x4000);
    assert(f->queue && f->cpu);
    f->provider.abi = GGFP_ABI;
    f->provider.bytes = sizeof(f->provider);
    f->provider.identity = f;
    f->provider.cookie = f;
    f->provider.epoch = 55;
    f->provider.layout = f->layout;
    f->provider.memory = f->queue;
    f->provider.observe = fixture_observe;
    f->provider.barrier = fixture_barrier;
    f->live = 1;
    f->share.epoch = f->provider.epoch;
    f->share.parcel = 0x1234;
    f->share.self_vmid = 7;
    f->share.peer_vmid = 9;
    f->share.complete = 1;
    in->provider = &f->provider;
    in->share = fixture_share;
    in->map = fixture_map;
    in->audit = fixture_audit;
    in->cookie = f;
    (void)host_page;
}
static void cleanup(struct fixture *f)
{
    free(f->queue);
    free(f->cpu);
}
static void success_path(void)
{
    struct fixture f;
    struct ggfn_state s;
    struct ggfn_inputs in;
    struct ggfc_mapping mapping;
    struct ggfn_status status;
    setup(&f, &s, &in, 16384);
    assert(ggfn_import(&s, &in, 16384) == GGFC_OK);
    assert(f.map_calls == 1 && f.audit_calls == 1 && f.observe_calls >= 3);
    assert(ggfn_status(&s, &status) == 1);
    assert(status.state == GGFN_IMPORTED && status.cpu_state == GGFC_MAPPED);
    assert(status.page_bytes == 16384 && status.envelope_bytes == 0x1c000);
    assert(status.envelope_physical == UINT64_C(0xd4f6c000));
    assert(status.offset == 0x1000);
    assert(status.epoch == 55 && status.parcel == 0x1234);
    assert(status.self_vmid == 7 && status.peer_vmid == 9);
    assert(status.proofs == GGFC_AUDIT_REQUIRED && !status.invalidated);
    assert(ggfn_get(&s, &mapping) == GGFC_OK && f.audit_calls == 2);
    assert(mapping.queue == (volatile void *)((uintptr_t)mapping.base + 0x1000));
    assert(mapping.physical == UINT64_C(0xd4f6c000) && mapping.bytes == 0x1c000);
    /* A second import is refused without touching the mapper again. */
    assert(ggfn_import(&s, &in, 16384) == GGFC_ALREADY && f.map_calls == 1);
    /* The provider proof is re-asked: a provider that stops proving kills get. */
    f.live = 0;
    assert(ggfn_get(&s, &mapping) == GGFC_AUDIT && f.audit_calls == 2);
    assert(ggfn_get(&s, &mapping) == GGFC_QUARANTINED);
    cleanup(&f);
}
static void share_refusals(void)
{
    struct fixture f;
    struct ggfn_state s;
    struct ggfn_inputs in;
    struct ggfn_status status;
    /* Incomplete transaction. */
    setup(&f, &s, &in, 16384);
    f.share.complete = 0;
    assert(ggfn_import(&s, &in, 16384) == GGFC_LOST && !f.map_calls);
    assert(ggfn_status(&s, &status) && status.state == GGFN_BROKEN);
    cleanup(&f);
    /* Epoch drift, invalid transaction, missing/duplicate VMIDs. */
    setup(&f, &s, &in, 16384);
    f.share.epoch = 54;
    assert(ggfn_import(&s, &in, 16384) == GGFC_LOST && !f.map_calls);
    cleanup(&f);
    setup(&f, &s, &in, 16384);
    f.share.invalid = 1;
    assert(ggfn_import(&s, &in, 16384) == GGFC_LOST);
    cleanup(&f);
    setup(&f, &s, &in, 16384);
    f.share.parcel = 0;
    assert(ggfn_import(&s, &in, 16384) == GGFC_LOST);
    cleanup(&f);
    setup(&f, &s, &in, 16384);
    f.share.peer_vmid = f.share.self_vmid;
    assert(ggfn_import(&s, &in, 16384) == GGFC_LOST);
    cleanup(&f);
    setup(&f, &s, &in, 16384);
    f.share.self_vmid = UINT16_MAX;
    assert(ggfn_import(&s, &in, 16384) == GGFC_LOST);
    cleanup(&f);
    /* A host page that is not 4/16 KiB or a non-provider input is refused. */
    setup(&f, &s, &in, 16384);
    assert(ggfn_import(&s, &in, 8192) == GGFC_ARGUMENT && !f.map_calls);
    cleanup(&f);
    setup(&f, &s, &in, 16384);
    in.provider = NULL;
    assert(ggfn_import(&s, &in, 16384) == GGFC_ARGUMENT);
    cleanup(&f);
    setup(&f, &s, &in, 16384);
    in.audit = NULL;
    assert(ggfn_import(&s, &in, 16384) == GGFC_ARGUMENT);
    cleanup(&f);
}
static void provider_refusals(void)
{
    struct fixture f;
    struct ggfn_state s;
    struct ggfn_inputs in;
    setup(&f, &s, &in, 16384);
    f.provider.abi = 2;
    assert(ggfn_import(&s, &in, 16384) == GGFC_ARGUMENT);
    cleanup(&f);
    setup(&f, &s, &in, 16384);
    f.live = 0;
    assert(ggfn_import(&s, &in, 16384) == GGFC_LOST && !f.map_calls);
    cleanup(&f);
    /* The provider copy is frozen: mutating the caller's struct afterwards has
     * no effect on the stored proofs. */
    setup(&f, &s, &in, 16384);
    struct ggf_provider caller = f.provider;
    in.provider = &caller;
    assert(ggfn_import(&s, &in, 16384) == GGFC_OK);
    caller.epoch = 99;
    struct ggfc_mapping mapping;
    assert(ggfn_get(&s, &mapping) == GGFC_OK);
    cleanup(&f);
}
static void mapper_refusals(void)
{
    struct fixture f;
    struct ggfn_state s;
    struct ggfn_inputs in;
    struct ggfn_status status;
    /* Mapping failure is retained and quarantined. */
    setup(&f, &s, &in, 16384);
    f.map_fail = 1;
    assert(ggfn_import(&s, &in, 16384) == GGFC_MAP && f.map_calls == 1);
    assert(ggfn_status(&s, &status) && status.state == GGFN_BROKEN &&
           status.cpu_state == GGFC_BROKEN);
    assert(ggfn_import(&s, &in, 16384) == GGFC_QUARANTINED && f.map_calls == 1);
    cleanup(&f);
    /* Audit failure, an alias in the returned mapping and lying proof bits. */
    setup(&f, &s, &in, 16384);
    f.audit_fail = 1;
    assert(ggfn_import(&s, &in, 16384) == GGFC_AUDIT && f.map_calls == 1);
    cleanup(&f);
    setup(&f, &s, &in, 16384);
    f.audit_alias = 1;
    assert(ggfn_import(&s, &in, 16384) == GGFC_AUDIT);
    cleanup(&f);
    setup(&f, &s, &in, 16384);
    f.audit_proofs = 1;
    assert(ggfn_import(&s, &in, 16384) == GGFC_AUDIT);
    cleanup(&f);
    /* Audit is re-run by get; a later failure quarantines the import. */
    setup(&f, &s, &in, 16384);
    assert(ggfn_import(&s, &in, 16384) == GGFC_OK);
    f.audit_fail = 1;
    struct ggfc_mapping mapping;
    assert(ggfn_get(&s, &mapping) == GGFC_AUDIT);
    assert(ggfn_get(&s, &mapping) == GGFC_QUARANTINED);
    cleanup(&f);
}
static void invalidate(void)
{
    struct fixture f;
    struct ggfn_state s;
    struct ggfn_inputs in;
    struct ggfn_status status;
    struct ggfc_mapping mapping;
    setup(&f, &s, &in, 16384);
    assert(ggfn_invalidate(&s) == GGFC_LOST);
    assert(ggfn_import(&s, &in, 16384) == GGFC_QUARANTINED);
    cleanup(&f);
    setup(&f, &s, &in, 16384);
    assert(ggfn_import(&s, &in, 16384) == GGFC_OK);
    assert(ggfn_invalidate(&s) == GGFC_LOST);
    assert(ggfn_get(&s, &mapping) == GGFC_QUARANTINED);
    assert(ggfn_status(&s, &status) && status.invalidated == 1 &&
           status.cpu_state == GGFC_BROKEN);
    /* Explicit loss keeps the mapping and backing: there is no release API. */
    assert(f.map_calls == 1);
    cleanup(&f);
}
int main(void)
{
    success_path();
    share_refusals();
    provider_refusals();
    mapper_refusals();
    invalidate();
    puts("GPU fence CPU native owner: frozen provider, live share identity, "
         "refusal matrix, retained mapping and quarantine passed");
    return 0;
}
