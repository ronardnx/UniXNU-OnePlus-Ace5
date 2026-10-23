/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Fixed secure-pool leaves only. The caller owns SCM state and publishes/TLBIs
 * each change; no secure data is accessed through the CPU by these helpers.
 */
#ifndef GIULIA_GPU_SECURE_MAPPING_H
#define GIULIA_GPU_SECURE_MAPPING_H
#include "giulia_gpu_platform.h"
#include "giulia_gpu_mapping.h"

#define GGS_SEC_ATTRIBUTES (GGM_LEAF_COMMON|GGM_XN)

static inline int ggs_mapping_owned(const struct ggm_tables *table,const uint64_t *physical)
{
    if(!physical || !ggm_structure_valid(table) || table->bytes!=GGS_SEC_TABLE_BYTES)return 0;
    for(unsigned i=0;i<GGS_SEC_SEGMENTS;i++) {
        uint64_t pa=physical[i];
        if(!pa || (pa&16383) || pa>=(UINT64_C(1)<<36) ||
           GGS_SEC_GRANULE>(UINT64_C(1)<<36)-pa ||
           (pa<table->physical+table->bytes && table->physical<pa+GGS_SEC_GRANULE))return 0;
        for(unsigned j=0;j<i;j++)
            if(pa<physical[j]+GGS_SEC_GRANULE && physical[j]<pa+GGS_SEC_GRANULE)return 0;
    }
    /* This domain has exactly one fixed aperture. Reject additional branches,
     * including otherwise valid leaves outside the secure allocation pool. */
    for(unsigned i=0;i<512;i++)if(i!=3 && table->words[i])return 0;
    uint64_t middle=table->words[3]&GGM_ADDRESS;
    if((table->words[3]&~GGM_ADDRESS)!=3 || middle<table->physical ||
       middle-table->physical>table->bytes-4096)return 0;
    size_t page=(size_t)((middle-table->physical)/4096);
    for(unsigned i=0;i<512;i++) {
        uint64_t entry=table->words[page*512+i];
        if(i<384 || i>=400) {if(entry)return 0;}
        else if(!entry)return 0;
    }
    return 1;
}

static inline int ggs_mapping_page(const struct ggm_tables *table,const uint64_t *physical,
    uint32_t offset,uint64_t attributes,unsigned action)
{
    if(!physical || offset>=GGS_SEC_CHUNKS*GGS_SEC_CHUNK || (offset&4095) ||
       (attributes && attributes!=GGS_SEC_ATTRIBUTES) || action>GGM_VERIFY)return 0;
    uint64_t pa=physical[offset/GGS_SEC_GRANULE]+offset%GGS_SEC_GRANULE,*leaf;
    if(!ggm_leaf(table,GGS_SEC_BASE+(uint64_t)offset,pa,&leaf))return 0;
    if(action==GGM_VERIFY)return *leaf==(pa|attributes);
    if(action==GGM_CLEAR)*leaf=pa;
    if(action==GGM_APPLY)*leaf=pa|attributes;
    return 1;
}

static inline int ggs_mapping_reset(const struct ggm_tables *table,const uint64_t *physical)
{
    if(!ggs_mapping_owned(table,physical))return 0;
    for(uint32_t at=0;at<GGS_SEC_CHUNKS*GGS_SEC_CHUNK;at+=4096)
        if(!ggs_mapping_page(table,physical,at,0,GGM_CHECK))return 0;
    for(uint32_t at=0;at<GGS_SEC_CHUNKS*GGS_SEC_CHUNK;at+=4096)
        if(!ggs_mapping_page(table,physical,at,0,GGM_CLEAR))return 0;
    return 1;
}

static inline int ggs_mapping_slot(const struct ggm_tables *table,const uint64_t *physical,
    unsigned slot,uint32_t bytes,unsigned action)
{
    if(!ggs_mapping_owned(table,physical) || slot>=GGS_SEC_CHUNKS ||
       !bytes || bytes>GGS_SEC_CHUNK || action>GGM_VERIFY)return 0;
    uint32_t mapped=(bytes+4095u)&~4095u;
    for(uint32_t at=0;at<GGS_SEC_CHUNK;at+=4096)
        if(!ggs_mapping_page(table,physical,slot*GGS_SEC_CHUNK+at,0,GGM_CHECK))return 0;
    if(action!=GGM_CHECK)for(uint32_t at=0;at<GGS_SEC_CHUNK;at+=4096)
        if(!ggs_mapping_page(table,physical,slot*GGS_SEC_CHUNK+at,
            at<mapped?GGS_SEC_ATTRIBUTES:0,action))return 0;
    return 1;
}

static inline int ggs_mapping_audit(const struct ggm_tables *table,const uint64_t *physical,
    const struct ggs_secure_slot *slots)
{
    if(!slots || !ggs_mapping_owned(table,physical))return 0;
    for(unsigned i=0;i<GGS_SEC_CHUNKS;i++) {
        const struct ggs_secure_slot *slot=slots+i;
        if(slot->used>1 || (slot->used && !ggs_secure_request(slot->bytes,4096,slot->flags,slot->kind)) ||
           (!slot->used && (slot->bytes || slot->flags || slot->kind)))return 0;
        uint32_t mapped=slot->used?(slot->bytes+4095u)&~4095u:0;
        for(uint32_t at=0;at<GGS_SEC_CHUNK;at+=4096)
            if(!ggs_mapping_page(table,physical,i*GGS_SEC_CHUNK+at,
                at<mapped?GGS_SEC_ATTRIBUTES:0,GGM_VERIFY))return 0;
    }
    return 1;
}
#endif
