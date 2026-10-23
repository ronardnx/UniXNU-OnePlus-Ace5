/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Fixed qcom,ipc-core resource: GMU-only, no CPU or GPU alias. The caller
 * builds tables while inactive and owns publication, BBM, TLBI and locking.
 * This header never accesses the resource or creates intermediate tables.
 */
#ifndef GIULIA_GPU_IPC_H
#define GIULIA_GPU_IPC_H
#include "giulia_gpu_mapping.h"

enum { GGIPC_PHYSICAL=0x00400000, GGIPC_IOVA=0x67000000,
    GGIPC_BYTES=0x00140000, GGIPC_KIND=19 };

/* Only the captured GMU IPC_CORE form is supported. HOST_INIT is refused:
 * the IPC resource must never be zeroed as if it were newly allocated RAM.
 * BUFFERABLE is accepted as in vendor get_attrs; it does not change the
 * Normal NC, outer-shareable descriptor. Cacheable aliases are unsupported.
 * Wire alignment encodes VA exponent in byte 0 and size exponent in byte 1.
 * 256 KiB is the largest power-of-two divisor of the complete resource, so
 * larger size alignments must not advertise backing beyond its real extent.
 */
static inline int ggipc_request(uint32_t bytes,uint32_t wire_align,
    uint32_t flags,uint32_t kind)
{
    uint32_t va_shift=wire_align&255u,size_shift=(wire_align>>8)&255u;
    if(kind!=GGIPC_KIND || !bytes || bytes>GGIPC_BYTES ||
       !(flags&8u) || (flags&~0x78u) || wire_align>>16 ||
       va_shift>22 || size_shift>18)return 0;
    uint32_t va_align=1u<<va_shift,size_align=1u<<size_shift;
    return !(GGIPC_IOVA&(va_align-1u)) &&
        ((bytes+size_align-1u)&~(size_align-1u))<=GGIPC_BYTES;
}

/* Every operation first checks the complete table topology and all 320
 * owned leaves. Zero leaves can be populated once; clears retain exact PA
 * tombstones. A mismatched final leaf therefore cannot cause partial edits.
 * flags=0 describes a disabled mapping for CHECK/CLEAR/VERIFY only.
 * Vendor GMU get_attrs does not add NOEXEC to these Normal NC mappings.
 */
static inline int ggipc_mapping(const struct ggm_tables *gmu,
    uint32_t flags,unsigned action)
{
    if(action>GGM_VERIFY || (!flags && action==GGM_APPLY) ||
       (flags && !ggipc_request(GGIPC_BYTES,0,flags,GGIPC_KIND)) ||
       !ggm_structure_valid(gmu) ||
       (gmu->physical<(uint64_t)GGIPC_PHYSICAL+GGIPC_BYTES &&
        GGIPC_PHYSICAL<gmu->physical+gmu->bytes))return 0;
    for(unsigned at=0;at<GGIPC_BYTES;at+=4096) {
        uint64_t *leaf;
        if(!ggm_leaf(gmu,(uint64_t)GGIPC_IOVA+at,
            (uint64_t)GGIPC_PHYSICAL+at,&leaf))return 0;
    }
    if(action==GGM_CHECK)return 1;
    uint64_t attrs=action==GGM_CLEAR?0:ggm_attributes(flags,1,0);
    for(unsigned at=0;at<GGIPC_BYTES;at+=4096) {
        uint64_t *leaf,pa=(uint64_t)GGIPC_PHYSICAL+at;
        if(!ggm_leaf(gmu,(uint64_t)GGIPC_IOVA+at,pa,&leaf))return 0;
        if(action==GGM_VERIFY) {
            if(*leaf!=(pa|attrs))return 0;
        } else *leaf=pa|attrs;
    }
    return 1;
}
#endif
