/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * One dedicated memstore descriptor, with no development-pool offset.
 * All intermediate tables are built while inactive. The caller serializes,
 * publishes and synchronizes changes before returning a resource handle.
 */
#ifndef GIULIA_GPU_MEMSTORE_MAPPING_H
#define GIULIA_GPU_MEMSTORE_MAPPING_H
#include "giulia_gpu_memstore.h"
#include "giulia_gpu_mapping.h"

static inline int ggms_mapping(const struct ggm_tables *gpu,const struct ggm_tables *gmu,
    const struct ggm_owned *owned,uint64_t physical,uint32_t flags,unsigned action)
{
    if(!ggm_owned_valid(gpu,gmu,owned) || !physical || (physical&4095) ||
       physical>=(UINT64_C(1)<<36) || GGMS_BYTES>(UINT64_C(1)<<36)-physical ||
       (flags && !ggms_request(GGMS_BYTES,0,flags,22)) || action>GGM_VERIFY ||
       (physical<gpu->physical+gpu->bytes && gpu->physical<physical+GGMS_BYTES) ||
       (physical<gmu->physical+gmu->bytes && gmu->physical<physical+GGMS_BYTES))return 0;
    for(unsigned i=0;i<GGD_CHUNKS;i++)
        if(physical<owned->physical[i]+GGD_CHUNK && owned->physical[i]<physical+GGMS_BYTES)return 0;
    for(unsigned at=0;at<GGMS_BYTES;at+=4096) {
        uint64_t *g,*m;
        if(!ggm_leaf(gpu,GGMS_IOVA+at,physical+at,&g) ||
           !ggm_leaf(gmu,GGMS_IOVA+at,physical+at,&m))return 0;
    }
    if(action==GGM_CHECK)return 1;
    uint64_t ga=action==GGM_CLEAR?0:ggm_attributes(flags,0,0);
    uint64_t ma=action==GGM_CLEAR?0:ggm_attributes(flags,1,0);
    for(unsigned at=0;at<GGMS_BYTES;at+=4096) {
        uint64_t *g,*m,pa=physical+at;
        if(!ggm_leaf(gpu,GGMS_IOVA+at,pa,&g) || !ggm_leaf(gmu,GGMS_IOVA+at,pa,&m))return 0;
        if(action==GGM_VERIFY) {
            if(*g!=(pa|ga) || *m!=(pa|ma))return 0;
        } else {*g=pa|ga;*m=pa|ma;}
    }
    return 1;
}
#endif
