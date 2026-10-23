/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Fixed GPU-fence CPU cache policy and proposed native pmap audit ABI.
 * Protecting intersecting CPU pages only forbids incompatible aliases; it
 * grants no memory ownership, SCM/RM sharing, initialization or GPU access.
 */
#ifndef GIULIA_GPU_FENCE_PMAP_H
#define GIULIA_GPU_FENCE_PMAP_H
#include <stdint.h>
#include <stddef.h>

#define GGFPM_PHYSICAL UINT64_C(0xd4e23000)
#define GGFPM_BYTES UINT64_C(0x2dd000)
#define GGFPM_END (GGFPM_PHYSICAL+GGFPM_BYTES)
#define GGFPM_ABI 1u
enum { GGFPM_NC_INDEX=1,GGFPM_NC_MAIR=0x44 };

struct ggfpm_audit {
    uint32_t abi,bytes,cpu_page_bytes,writable;
    uintptr_t virtual_address;
    uint64_t physical,span,mair,tcr,ttbr1,first_descriptor,last_descriptor;
};
static inline int ggfpm_page_valid(uint64_t page)
{return page==4096 || page==16384 || page==65536;}
static inline int ggfpm_range_valid(uint64_t start,uint64_t bytes)
{return bytes && bytes<=UINT64_MAX-start;}
/* Invalid/overflowed ranges fail closed. A zero-length operation maps no
 * page; callers which authorize access separately require a positive span. */
static inline int ggfpm_intersects(uint64_t pa,uint64_t bytes,uint64_t page)
{
    if(!ggfpm_page_valid(page))return 1;
    if(!bytes)return 0;
    if(!ggfpm_range_valid(pa,bytes))return 1;
    uint64_t begin=GGFPM_PHYSICAL&~(page-1),end=(GGFPM_END+page-1)&~(page-1);
    return pa<end && begin<pa+bytes;
}
static inline int ggfpm_cache_allowed(uint64_t pa,uint64_t bytes,uint64_t page,unsigned index)
{
    if(!ggfpm_page_valid(page) || (bytes && !ggfpm_range_valid(pa,bytes)))return 0;
    return !ggfpm_intersects(pa,bytes,page) || index==GGFPM_NC_INDEX;
}
static inline unsigned ggfpm_tg1_page(uint64_t tcr)
{
    unsigned tg=(unsigned)((tcr>>30)&3u);
    return tg==1?16384u:tg==2?4096u:tg==3?65536u:0u;
}
static inline int ggfpm_nc_mair(uint64_t mair)
{return ((mair>>(8*GGFPM_NC_INDEX))&255u)==GGFPM_NC_MAIR;}
/* An audit span is a fully owned interior CPU envelope. The policy's outer
 * protected pages are deliberately NOT accepted as mapping authority here.
 * For the 4K-layout GPU queues, the 16K envelope is d4f6c000..d4f88000.
 */
static inline int ggfpm_audit_range(uintptr_t va,uint64_t pa,size_t bytes,
    unsigned page,unsigned writable)
{
    return va && bytes && ggfpm_page_valid(page) && writable<=1 &&
        ((uint64_t)va>>48)==UINT64_C(0xffff) &&
        !((va|pa|(uint64_t)bytes)&(page-1u)) && bytes<=UINTPTR_MAX-va &&
        pa>=GGFPM_PHYSICAL && pa<GGFPM_END && bytes<=GGFPM_END-pa;
}
/* Pure oracle for one 48-bit leaf descriptor. Native code additionally checks
 * effective table AP/XN restrictions and the current TTBR1/root/TCR/MAIR.
 * Kernel RO/RW only (AP=2/0), AF, PXN and UXN are required; no EL0 access.
 */
static inline int ggfpm_leaf_valid(uint64_t pte,uint64_t pa,unsigned page,
    unsigned writable,uint64_t mair)
{
    if(!ggfpm_page_valid(page) || writable>1 || (pa>>48) || (pa&(page-1u)) ||
       (pte&(UINT64_C(1)<<51)) || /* DBM: do not accept mutable permissions. */
       (pte&3u)!=3u || !(pte&UINT64_C(0x400)) ||
       ((pte>>2)&7u)!=GGFPM_NC_INDEX || !ggfpm_nc_mair(mair) ||
       ((pte>>6)&3u)!=(writable?0u:2u) ||
       (pte&UINT64_C(0x60000000000000))!=UINT64_C(0x60000000000000))return 0;
    uint64_t address=pte&UINT64_C(0x0000fffffffff000);
    return !(address&(page-1u)) && address==pa;
}

/* Validate every leaf in an interior CPU envelope, including padding pages.
 * The caller must collect these descriptors under the native pmap lock and
 * separately check table restrictions, architectural state and global aliases.
 * This pure decoder does not establish any of those runtime facts. Exact
 * count prevents a first/last-only audit from overlooking a bad middle page.
 * The supplied snapshot is private aligned memory, never the queue itself.
 */
static inline int ggfpm_leaf_span_valid(uintptr_t va,uint64_t pa,size_t bytes,
    unsigned page,unsigned writable,uint64_t mair,const uint64_t *leaves,size_t count)
{
    if(!ggfpm_audit_range(va,pa,bytes,page,writable) || !ggfpm_nc_mair(mair) ||
       !leaves || (uintptr_t)leaves%__alignof__(*leaves) || count!=bytes/page ||
       count>(UINTPTR_MAX-(uintptr_t)leaves)/sizeof(*leaves))return 0;
    for(size_t i=0;i<count;i++)
        if(!ggfpm_leaf_valid(leaves[i],pa+i*page,page,writable,mair))return 0;
    return 1;
}

#ifdef __cplusplus
extern "C" {
#endif
/* Reserved for a future native pmap patch; no implementation is installed yet.
 * Must be defined by that patch, never by a service callback. A successful
 * audit reads actual CPU descriptors and architectural registers, and is
 * conditional on the cold-boot alias policy and excluded managed/directmap
 * windows. Outputs remain unchanged on failure. The owner pins its mapping
 * and storage and runs this from a serialized, sleep-capable kernel worker.
 * This ABI does not authorize touching the backing memory by itself.
 */
int giulia_gpu_fence_cpu_policy_ready(void);
int giulia_gpu_fence_cpu_audit(uintptr_t,uint64_t,size_t,unsigned,struct ggfpm_audit *);
/* Bootstrap-only hooks. Ordinary services must never call them. */
void giulia_gpu_fence_cpu_boot_validate(uint64_t,uint64_t,uint64_t);
void giulia_gpu_fence_cpu_boot_map_check(uint64_t,uint64_t,unsigned);
void giulia_gpu_fence_cpu_boot_retired(void);
#ifdef __cplusplus
}
#endif
#endif
