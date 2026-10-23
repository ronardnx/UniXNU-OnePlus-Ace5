/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Registered CTX0 provider memory, GMU-only. The caller owns serialization,
 * provider lifetime/READY proof, inactive preparation and BBM/TLBI publication.
 * No allocation, CPU writes to provider storage, IPC or ownership grants here.
 */
#ifndef GIULIA_GPU_FENCE_MAPPING_H
#define GIULIA_GPU_FENCE_MAPPING_H
#include "giulia_gpu_mapping.h"
#include "giulia_gpu_fence.h"

enum { GGF_IOVA=0x68000000, GGF_APERTURE=0x200000 };

/* A validated tree has at most one parent for each table page. Treat data
 * mappings into the table pool as references too: a zero data page is not an
 * available table page. The complete walk precedes the sole descriptor write.
 * Repeated preparation is rejected, including a previously prepared empty L3.
 */
static inline int ggf_mapping_prepare(struct ggm_tables *gmu)
{
    if(!ggm_structure_valid(gmu) || !gmu->words[GGF_IOVA>>30])return 0;
    unsigned char used[128]={1};
    for(unsigned root=0;root<4;root++) {
        uint64_t entry=gmu->words[root];
        if(!entry)continue;
        size_t l2=(size_t)(((entry&GGM_ADDRESS)-gmu->physical)/4096);
        used[l2]=1;
        for(unsigned slot=0;slot<512;slot++) {
            entry=gmu->words[l2*512+slot];
            if(!entry)continue;
            size_t l3=(size_t)(((entry&GGM_ADDRESS)-gmu->physical)/4096);
            used[l3]=1;
            for(unsigned leaf=0;leaf<512;leaf++) {
                uint64_t value=gmu->words[l3*512+leaf],pa=value&GGM_ADDRESS;
                if(value && pa>=gmu->physical && pa-gmu->physical<gmu->bytes)
                    used[(pa-gmu->physical)/4096]=1;
            }
        }
    }
    size_t l2=(size_t)(((gmu->words[GGF_IOVA>>30]&GGM_ADDRESS)-gmu->physical)/4096);
    uint64_t *parent=&gmu->words[l2*512+((GGF_IOVA>>21)&511u)];
    if(*parent)return 0;
    for(size_t page=1;page<gmu->bytes/4096;page++) {
        if(used[page])continue;
        unsigned leaf;
        for(leaf=0;leaf<512;leaf++)if(gmu->words[page*512+leaf])break;
        if(leaf!=512)continue;
        *parent=(gmu->physical+page*4096)|3u;
        return 1;
    }
    return 0;
}

/* The reserved aperture is exactly one L3 page. Structure validation proves
 * its parent cannot alias another table. No placeholder backing PA is needed
 * to inspect the unbound aperture.
 */
static inline uint64_t *ggf_mapping_leaves(const struct ggm_tables *gmu)
{
    if(!ggm_structure_valid(gmu))return NULL;
    uint64_t entry=gmu->words[GGF_IOVA>>30];
    if(!entry)return NULL;
    size_t l2=(size_t)(((entry&GGM_ADDRESS)-gmu->physical)/4096);
    entry=gmu->words[l2*512+((GGF_IOVA>>21)&511u)];
    if(!entry)return NULL;
    return gmu->words+((entry&GGM_ADDRESS)-gmu->physical)/8;
}

/* No existing IOVA outside this aperture may refer to the provider's pages,
 * even as an invalid PA tombstone. This prevents cacheable/permission aliases
 * through another prebuilt region. No leaf outside the aperture is modified.
 */
static inline int ggf_mapping_no_alias(const struct ggm_tables *gmu,
    const struct ggf_layout *layout,const uint64_t *leaves)
{
    for(unsigned root=0;root<4;root++) {
        uint64_t entry=gmu->words[root];
        if(!entry)continue;
        size_t l2=(size_t)(((entry&GGM_ADDRESS)-gmu->physical)/4096);
        for(unsigned slot=0;slot<512;slot++) {
            entry=gmu->words[l2*512+slot];
            if(!entry)continue;
            const uint64_t *other=gmu->words+((entry&GGM_ADDRESS)-gmu->physical)/8;
            if(other==leaves)continue;
            for(unsigned page=0;page<512;page++) {
                uint64_t value=other[page],pa=value&GGM_ADDRESS;
                if(value && pa>=layout->physical && pa-layout->physical<layout->bytes)return 0;
            }
        }
    }
    return 1;
}

/* CHECK/APPLY accept zero leaves or correctly formed leaves for the exact
 * provider PA. VERIFY additionally checks every permission bit. CLEAR is an
 * inactive-only rollback and restores zeros, not synthetic PA tombstones.
 * A NULL layout checks only an unbound, entirely empty aperture; no edits.
 */
static inline int ggf_mapping(const struct ggm_tables *gmu,
    const struct ggf_layout *layout,uint32_t flags,unsigned action)
{
    if(action>GGM_VERIFY)return 0;
    uint64_t *leaves=ggf_mapping_leaves(gmu);
    if(!leaves)return 0;
    if(!layout) {
        if(flags || (action!=GGM_CHECK && action!=GGM_VERIFY))return 0;
        for(unsigned page=0;page<512;page++)if(leaves[page])return 0;
        return 1;
    }
    if(!ggf_layout_valid(layout) || !(flags&8u) || (flags&~0x178u) ||
       !layout->bytes || layout->bytes>GGF_APERTURE ||
       ((layout->physical|layout->bytes)&4095u) ||
       (layout->physical<gmu->physical+gmu->bytes &&
        gmu->physical<layout->physical+layout->bytes) ||
       !ggf_mapping_no_alias(gmu,layout,leaves))return 0;
    unsigned pages=layout->bytes/4096;
    for(unsigned page=0;page<512;page++) {
        if(page>=pages) {
            if(leaves[page])return 0;
        } else {
            uint64_t *leaf;
            if(!ggm_leaf(gmu,(uint64_t)GGF_IOVA+page*4096,
                layout->physical+page*4096,&leaf) || leaf!=&leaves[page])return 0;
        }
    }
    if(action==GGM_CHECK)return 1;
    uint64_t attrs=ggm_attributes(flags,1,0);
    if(action==GGM_VERIFY) {
        for(unsigned page=0;page<pages;page++)
            if(leaves[page]!=((layout->physical+page*4096)|attrs))return 0;
        return 1;
    }
    for(unsigned page=0;page<pages;page++)
        leaves[page]=action==GGM_CLEAR?0:((layout->physical+page*4096)|attrs);
    return 1;
}
#endif
