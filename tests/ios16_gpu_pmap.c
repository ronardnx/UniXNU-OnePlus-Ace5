/* Host-side policy oracle only; no page tables, mapper or device access. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_pmap.h"

static uint64_t leaf(uint64_t pa, unsigned writable)
{
    return pa | UINT64_C(0x60000000000407) | (writable ? 0 : UINT64_C(0x80));
}
static struct ggpm_policy policy(unsigned page, uint64_t mair, uint64_t tcr)
{
    struct ggpm_policy p;
    memset(&p, 0, sizeof(p));
    p.abi = GGPM_ABI;
    p.boot_valid = 1;
    p.page_bytes = page;
    p.mair = mair;
    p.tcr = tcr;
    p.ttbr1 = UINT64_C(0x0000000080120000);
    p.physmap_base = UINT64_C(0x8c0000000);
    p.physmap_bytes = UINT64_C(0x80000000);
    p.managed_base = UINT64_C(0x8c0000000);
    p.managed_bytes = UINT64_C(0x78000000);
    return p;
}
static uint64_t tcr(unsigned page)
{
    const uint64_t tg1 = page == 16384 ? UINT64_C(1) :
        page == 4096 ? UINT64_C(2) : UINT64_C(3);
    return (tg1 << 30) | (UINT64_C(16) << 16);   /* T1SZ=16 -> 48-bit VA */
}
static void windows(void)
{
    struct ggpm_policy p = policy(16384, UINT64_C(0x4400), tcr(16384));
    assert(ggpm_policy_valid(&p));
    assert(ggpm_window_valid(0, 0) && !ggpm_window_valid(0, 1));
    assert(!ggpm_window_valid(UINT64_MAX - 3, 8));
    assert(!ggpm_hits(0, 0, GGPM_CARVEOUT_PHYSICAL, 16));
    assert(ggpm_hits(GGPM_CARVEOUT_PHYSICAL, 16, GGPM_CARVEOUT_PHYSICAL, 16));
    assert(ggpm_hits(GGPM_CARVEOUT_END - 1, 2, GGPM_CARVEOUT_END - 16, 16));
    assert(!ggpm_hits(GGPM_CARVEOUT_END, 16, GGPM_CARVEOUT_PHYSICAL, 16));
    assert(ggpm_carveout_hit(GGPM_CARVEOUT_PHYSICAL - 1, 2));
    assert(!ggpm_carveout_hit(GGPM_CARVEOUT_END, 1));
    assert(!ggpm_carveout_hit(GGPM_CARVEOUT_PHYSICAL, 0));
    assert(ggpm_carveout_hit(UINT64_MAX - 1, 4));   /* wrapping range */
}
static void policy_matrix(void)
{
    const uint64_t mair = UINT64_C(0x4400);
    struct ggpm_policy p = policy(16384, mair, tcr(16384));
    assert(GGPM_CARVEOUT_PHYSICAL == GGFPM_PHYSICAL);
    assert(GGPM_CARVEOUT_BYTES == GGFPM_BYTES);
    assert(ggpm_policy_valid(&p));
    /* MAIR index 1 must be Normal NC; a writeback index 1 is not accepted. */
    struct ggpm_policy q = p;
    q.mair = UINT64_C(0xff44);
    assert(!ggpm_policy_valid(&q));
    q = p;
    q.mair = UINT64_C(0x0044);
    assert(!ggpm_policy_valid(&q));
    /* TG1 must describe the same granule as the reported page size. */
    q = p;
    q.page_bytes = 4096;
    assert(!ggpm_policy_valid(&q));
    /* Boot latch and ABI are required. */
    q = p;
    q.boot_valid = 0;
    assert(!ggpm_policy_valid(&q));
    q = p;
    q.abi = 2;
    assert(!ggpm_policy_valid(&q));
    q = p;
    q.page_bytes = 8192;
    assert(!ggpm_policy_valid(&q));
    /* TTBR1 is a 48-bit page-aligned BADDR; ASID bits must already be gone. */
    q = p;
    q.ttbr1 = 0;
    assert(!ggpm_policy_valid(&q));
    q = p;
    q.ttbr1 |= UINT64_C(1) << 63;
    assert(!ggpm_policy_valid(&q));
    q = p;
    q.ttbr1 = UINT64_C(0x80120001);
    assert(!ggpm_policy_valid(&q));
    /* T1SZ outside 16..32 cannot describe this kernel window. */
    q = p;
    q.tcr = tcr(16384) & ~(UINT64_C(0x3f) << 16);
    assert(!ggpm_policy_valid(&q));
    q = p;
    q.tcr = tcr(16384) | (UINT64_C(33) << 16);
    assert(!ggpm_policy_valid(&q));
    /* The carveout must be device memory: no direct map and no managed RAM. */
    q = p;
    q.physmap_base = GGPM_CARVEOUT_PHYSICAL;
    q.physmap_bytes = 0x1000;
    assert(!ggpm_policy_valid(&q));
    q = p;
    q.managed_base = GGPM_CARVEOUT_PHYSICAL + 0x1000;
    q.managed_bytes = 0x1000;
    assert(!ggpm_policy_valid(&q));
    q = p;
    q.physmap_bytes = UINT64_MAX;
    assert(!ggpm_policy_valid(&q));
    q = p;
    q = p;
    q.managed_bytes = 0;
    assert(ggpm_policy_valid(&q));
}
static void alias(void)
{
    const struct ggpm_policy p = policy(16384, UINT64_C(0x4400), tcr(16384));
    assert(ggpm_alias_free(&p, UINT64_C(0xd4f6c000), 0x1c000));
    assert(ggpm_alias_free(&p, GGPM_CARVEOUT_PHYSICAL, GGPM_CARVEOUT_BYTES));
    assert(!ggpm_alias_free(&p, GGPM_CARVEOUT_PHYSICAL - 0x4000, 0x4000));
    assert(!ggpm_alias_free(&p, GGPM_CARVEOUT_END - 0x1000, 0x2000));
    assert(!ggpm_alias_free(&p, GGPM_CARVEOUT_PHYSICAL, 0));
    assert(!ggpm_alias_free(&p, UINT64_MAX - 1, 4));
    struct ggpm_policy q = p;
    q.managed_base = UINT64_C(0xd4e23000);
    q.managed_bytes = 0x1000;
    assert(!ggpm_alias_free(&q, UINT64_C(0xd4e23000), 0x1000));
    q = p;
    q.physmap_base = UINT64_C(0xd5000000) - 0x1000;
    q.physmap_bytes = 0x2000;
    /* A window over the carveout invalidates the whole policy, so such a
     * window can never be used to bless an audited span. */
    assert(!ggpm_policy_valid(&q));
    assert(!ggpm_alias_free(&q, UINT64_C(0xd4fff000), 0x1000));
}
static void seal(void)
{
    const struct ggpm_policy p4 = policy(4096, UINT64_C(0x4400), tcr(4096));
    const struct ggpm_policy p16 = policy(16384, UINT64_C(0x4400), tcr(16384));
    const struct ggpm_policy p64 = policy(65536, UINT64_C(0x4400), tcr(65536));
    assert(ggpm_policy_valid(&p4) && ggpm_policy_valid(&p16) && ggpm_policy_valid(&p64));
    struct ggfpm_audit audit;
    memset(&audit, 0xa5, sizeof(audit));
    const uint64_t va = UINT64_C(0xfffffff010000000);
    /* 4 KiB queue geometry: 0x1a000 bytes starting at 0xd4f6d000. */
    uint64_t first = leaf(UINT64_C(0xd4f6d000), 1);
    uint64_t last = leaf(UINT64_C(0xd4f6d000) + 0x19000, 1);
    assert(ggpm_audit_seal(&p4, (uintptr_t)va, UINT64_C(0xd4f6d000), 0x1a000, 1,
        first, last, &audit));
    assert(audit.abi == GGFPM_ABI && audit.bytes == 0x1a000);
    assert(audit.cpu_page_bytes == 4096 && audit.writable == 1);
    assert(audit.virtual_address == (uintptr_t)va);
    assert(audit.physical == UINT64_C(0xd4f6d000) && audit.span == 0x1a000);
    assert(audit.mair == p4.mair && audit.tcr == p4.tcr && audit.ttbr1 == p4.ttbr1);
    assert(audit.first_descriptor == first && audit.last_descriptor == last);
    /* 16 KiB CPU envelope around the same 4 KiB queue geometry. */
    first = leaf(UINT64_C(0xd4f6c000), 1);
    last = leaf(UINT64_C(0xd4f6c000) + 6 * 0x4000, 1);
    struct ggfpm_audit envelope;
    assert(ggpm_audit_seal(&p16, (uintptr_t)va, UINT64_C(0xd4f6c000), 0x1c000, 1,
        first, last, &envelope));
    assert(envelope.cpu_page_bytes == 16384 && envelope.bytes == 0x1c000);
    /* 64 KiB remains a decoder check over an aligned interior span. */
    assert(ggpm_audit_seal(&p64, (uintptr_t)va, UINT64_C(0xd4e30000), 0x10000, 0,
        leaf(UINT64_C(0xd4e30000), 0), leaf(UINT64_C(0xd4e30000), 0), &audit));
    /* A wrong endpoint PA, an attribute drift or a wrapping span is refused. */
    struct ggfpm_audit unchanged;
    memset(&unchanged, 0x5a, sizeof(unchanged));
    struct ggfpm_audit saved = unchanged;
    assert(!ggpm_audit_seal(&p4, (uintptr_t)va, UINT64_C(0xd4f6d000), 0x1a000, 1,
        first, last, &unchanged));
    assert(!memcmp(&unchanged, &saved, sizeof(unchanged)));
    assert(!ggpm_audit_seal(&p16, (uintptr_t)va, UINT64_C(0xd4f6c000), 0x1c000, 1,
        leaf(UINT64_C(0xd4f6c000), 1), leaf(UINT64_C(0xd4f6c000) + 6 * 0x4000, 0), &unchanged));
    assert(!ggpm_audit_seal(&p16, (uintptr_t)va, UINT64_C(0xd4f6c000), 0x1c000, 2,
        leaf(UINT64_C(0xd4f6c000), 1), leaf(UINT64_C(0xd4f6c000) + 6 * 0x4000, 1), &unchanged));
    assert(!ggpm_audit_seal(&p16, UINT64_C(0x10000000), UINT64_C(0xd4f6c000), 0x1c000, 1,
        leaf(UINT64_C(0xd4f6c000), 1), leaf(UINT64_C(0xd4f6c000) + 6 * 0x4000, 1), &unchanged));
    assert(!ggpm_audit_seal(&p16, (uintptr_t)va + 1, UINT64_C(0xd4f6c000), 0x1c000, 1,
        leaf(UINT64_C(0xd4f6c000), 1), leaf(UINT64_C(0xd4f6c000) + 6 * 0x4000, 1), &unchanged));
    assert(!ggpm_audit_seal(&p16, (uintptr_t)va, UINT64_C(0xd4f6c000) + 1, 0x1c000, 1,
        leaf(UINT64_C(0xd4f6c000), 1), leaf(UINT64_C(0xd4f6c000) + 6 * 0x4000, 1), &unchanged));
    assert(!ggpm_audit_seal(&p16, (uintptr_t)va, UINT64_C(0xd4f6c000), 0x1c001, 1,
        leaf(UINT64_C(0xd4f6c000), 1), leaf(UINT64_C(0xd4f6c000) + 6 * 0x4000, 1), &unchanged));
    assert(!ggpm_audit_seal(&p16, (uintptr_t)va, UINT64_C(0xd4e20000), 0x4000, 1,
        leaf(UINT64_C(0xd4e20000), 1), leaf(UINT64_C(0xd4e20000), 1), &unchanged));
    assert(!ggpm_audit_seal(&p16, (uintptr_t)va, UINT64_C(0xd4f6c000), 0x1c000, 1,
        leaf(UINT64_C(0xd4f6c000), 1), first /* 4 KiB leaf, wrong granule */, &unchanged));
    assert(!ggpm_audit_seal(NULL, (uintptr_t)va, UINT64_C(0xd4f6c000), 0x1c000, 1,
        first, last, &unchanged));
    assert(!ggpm_audit_seal(&p16, (uintptr_t)va, UINT64_C(0xd4f6c000), 0x1c000, 1,
        first, last, NULL));
}
int main(void)
{
    windows();
    policy_matrix();
    alias();
    seal();
    puts("GPU pmap policy: TG1/MAIR/TTBR1/T1SZ, direct-map and managed-RAM "
         "alias windows, 4/16/64 KiB seals and immutable failure outputs passed");
    return 0;
}
