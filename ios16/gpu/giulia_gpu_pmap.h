/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Fixed GPU carveout CPU cache policy for the hardware-fence queue.
 *
 * This header is the decision layer of the native pmap gate: it never walks
 * page tables and never maps memory. The generated pmap hook collects the
 * actual TTBR1/TCR/MAIR state, the direct-map and managed-RAM windows, and one
 * descriptor per audited CPU page, then calls ggpm_audit_seal(). A successful
 * seal therefore only repeats checks over data the caller actually observed.
 *
 * The carveout is device memory below 4 GiB. A policy that claims it as
 * managed RAM or as part of the physical aperture can never prove the absence
 * of a cacheable alias, so such a policy is rejected instead of audited.
 * Nothing here authorizes touching the shared queue: mapping authority stays
 * with the owner callbacks in giulia_gpu_fence_cpu.h.
 */
#ifndef GIULIA_GPU_PMAP_H
#define GIULIA_GPU_PMAP_H
#include <stdint.h>
#include <stddef.h>
#include "giulia_gpu_fence_pmap.h"

#define GGPM_ABI 1u
#define GGPM_CARVEOUT_PHYSICAL UINT64_C(0xd4e23000)
#define GGPM_CARVEOUT_BYTES UINT64_C(0x2dd000)
#define GGPM_CARVEOUT_END (GGPM_CARVEOUT_PHYSICAL + GGPM_CARVEOUT_BYTES)
/* AttrIndx/AP/XN/AF/DBM/address bits of a 48-bit stage-1 leaf. */
#define GGPM_LEAF_ADDRESS UINT64_C(0x0000fffffffff000)

/* physmap_* and managed_* are physical windows, not virtual ones: the direct
 * map covers only the physical aperture, and managed RAM is the only pool from
 * which XNU creates cacheable leaves. The two overlap by construction. */
struct ggpm_policy {
    uint32_t abi, boot_valid;
    uint64_t page_bytes, mair, tcr, ttbr1;
    uint64_t physmap_base, physmap_bytes;
    uint64_t managed_base, managed_bytes;
};

/* Zero-length windows are the normal "absent" encoding; a wrapping window is
 * never valid, and a wrapped length comparison must fail closed. */
static inline int ggpm_window_valid(uint64_t base, uint64_t bytes)
{
    return !bytes || (base && bytes <= UINT64_MAX - base);
}
static inline int ggpm_hits(uint64_t base, uint64_t bytes,
    uint64_t other, size_t other_bytes)
{
    if (!bytes || !other_bytes) return 0;
    if (!ggpm_window_valid(base, bytes) || !ggpm_window_valid(other, other_bytes)) return 1;
    return base < other + other_bytes && other < base + bytes;
}
static inline int ggpm_carveout_hit(uint64_t pa, size_t bytes)
{
    return ggpm_hits(GGPM_CARVEOUT_PHYSICAL, GGPM_CARVEOUT_BYTES, pa, bytes);
}

/* boot_valid is the latch produced by giulia_gpu_fence_cpu_boot_validate();
 * without it a policy is only a set of plausible numbers. T1SZ must describe a
 * 32..48-bit kernel window, TTBR1 must be a page-aligned 48-bit BADDR (the
 * caller strips the ASID field before filling this struct). */
static inline int ggpm_policy_valid(const struct ggpm_policy *p)
{
    if (!p || p->abi != GGPM_ABI || p->boot_valid != 1) return 0;
    if (!ggfpm_page_valid(p->page_bytes) || !ggfpm_nc_mair(p->mair)) return 0;
    if (ggfpm_tg1_page(p->tcr) != p->page_bytes) return 0;
    const uint64_t t1sz = (p->tcr >> 16) & 0x3fu;
    if (t1sz < 16 || t1sz > 32) return 0;
    if (!p->ttbr1 || (p->ttbr1 >> 48) || (p->ttbr1 & (p->page_bytes - 1u))) return 0;
    if (!ggpm_window_valid(p->physmap_base, p->physmap_bytes) ||
        !ggpm_window_valid(p->managed_base, p->managed_bytes)) return 0;
    if (ggpm_hits(p->physmap_base, p->physmap_bytes,
            GGPM_CARVEOUT_PHYSICAL, GGPM_CARVEOUT_BYTES)) return 0;
    if (ggpm_hits(p->managed_base, p->managed_bytes,
            GGPM_CARVEOUT_PHYSICAL, GGPM_CARVEOUT_BYTES)) return 0;
    return 1;
}

/* The audited CPU range is an owned interior of the fixed carveout which is
 * neither direct-mapped nor managed RAM. This is the whole global-alias claim
 * of this layer: device memory outside those windows has no kernel-created
 * cacheable alias, and the entry point refuses to create one. */
static inline int ggpm_alias_free(const struct ggpm_policy *p,
    uint64_t pa, size_t bytes)
{
    if (!ggpm_policy_valid(p) || !bytes || bytes > UINT64_MAX - pa) return 0;
    if (pa < GGPM_CARVEOUT_PHYSICAL || bytes > GGPM_CARVEOUT_END - pa) return 0;
    if (ggpm_hits(p->physmap_base, p->physmap_bytes, pa, bytes)) return 0;
    if (ggpm_hits(p->managed_base, p->managed_bytes, pa, bytes)) return 0;
    return 1;
}

/* Seal one audited span. first/last are the real leaf descriptors of the
 * first and last page; every intermediate page is checked by the native loop
 * with the same predicate, and count comes from the requested span so a
 * first/last-only result cannot hide a bad middle page. out is unchanged on
 * failure and must not alias the mapping. */
static inline int ggpm_audit_seal(const struct ggpm_policy *p, uintptr_t va,
    uint64_t pa, size_t bytes, unsigned writable, uint64_t first, uint64_t last,
    struct ggfpm_audit *out)
{
    if (!out || (uintptr_t)out % __alignof__(*out)) return 0;
    if (!ggpm_alias_free(p, pa, bytes) || writable > 1) return 0;
    if (bytes > UINT32_MAX || !ggfpm_audit_range(va, pa, bytes,
            (unsigned)p->page_bytes, writable)) return 0;
    const size_t count = bytes / (size_t)p->page_bytes;
    if (!count) return 0;
    if (!ggfpm_leaf_valid(first, pa, (unsigned)p->page_bytes, writable, p->mair)) return 0;
    const uint64_t last_pa = pa + (uint64_t)(count - 1) * p->page_bytes;
    if (!ggfpm_range_valid(last_pa, p->page_bytes)) return 0;
    if (!ggfpm_leaf_valid(last, last_pa, (unsigned)p->page_bytes, writable, p->mair)) return 0;
    /* Same permissions/attributes on every endpoint; only BADDR may differ. */
    if ((first & ~GGPM_LEAF_ADDRESS) != (last & ~GGPM_LEAF_ADDRESS)) return 0;
    struct ggfpm_audit audit;
    audit.abi = GGFPM_ABI;
    audit.bytes = (uint32_t)bytes;
    audit.cpu_page_bytes = (uint32_t)p->page_bytes;
    audit.writable = writable;
    audit.virtual_address = va;
    audit.physical = pa;
    audit.span = bytes;
    audit.mair = p->mair;
    audit.tcr = p->tcr;
    audit.ttbr1 = p->ttbr1;
    audit.first_descriptor = first;
    audit.last_descriptor = last;
    *out = audit;
    return 1;
}
#endif
