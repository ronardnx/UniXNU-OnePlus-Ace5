/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Owned development-pool leaves only. No allocation, MMIO, or table creation.
 * The caller serializes edits and performs break-before-make publication/TLBI.
 */
#ifndef GIULIA_GPU_MAPPING_H
#define GIULIA_GPU_MAPPING_H
#include "giulia_gpu_dev.h"

#define GGM_ADDRESS UINT64_C(0x0000fffffffff000)
#define GGM_XN (UINT64_C(3) << 53)
#define GGM_LEAF_COMMON UINT64_C(0xe03) /* AF, nG, outer-shareable, Normal NC */
enum { GGM_CHECK=0, GGM_CLEAR=1, GGM_APPLY=2, GGM_VERIFY=3 };
struct ggm_tables { uint64_t *words; size_t bytes; uint64_t physical; };
struct ggm_owned { uint64_t physical[GGD_CHUNKS]; };

static inline int ggm_table_valid(const struct ggm_tables *t)
{
    return t && t->words && !((uintptr_t)t->words&7) && t->bytes>=4096 &&
        t->bytes<=UINTPTR_MAX-(uintptr_t)t->words &&
        !(t->bytes&4095) && t->bytes<=128u*4096 && t->physical &&
        !(t->physical&4095) && t->physical<(UINT64_C(1)<<36) &&
        t->bytes<=(UINT64_C(1)<<36)-t->physical;
}

/* A table page has exactly one parent. Otherwise an empty leaf table could
 * pass every ownership check and acquire an unintended IOVA alias on apply.
 * Check the complete 32-bit tree before any edit, including non-pool branches.
 */
static inline int ggm_structure_valid(const struct ggm_tables *t)
{
    if(!ggm_table_valid(t))return 0;
    unsigned char seen[128]={1}; /* The root cannot also be a child table. */
    for(unsigned root=0;root<4;root++) {
        uint64_t entry=t->words[root],pa=entry&GGM_ADDRESS;
        if(!entry)continue;
        if((entry&~GGM_ADDRESS)!=3 || pa<t->physical ||
           pa-t->physical>t->bytes-4096)return 0;
        size_t page=(size_t)((pa-t->physical)/4096);
        if(seen[page])return 0;
        seen[page]=1;
        for(unsigned slot=0;slot<512;slot++) {
            entry=t->words[page*512+slot];pa=entry&GGM_ADDRESS;
            if(!entry)continue;
            if((entry&~GGM_ADDRESS)!=3 || pa<t->physical ||
               pa-t->physical>t->bytes-4096)return 0;
            size_t child=(size_t)((pa-t->physical)/4096);
            if(seen[child])return 0;
            seen[child]=1;
        }
    }
    return 1;
}

/* A zero leaf may be populated only after the caller supplies owned backing.
 * Invalid tombstones retain that backing PA, allowing later edits to verify it.
 * Table descriptors cannot redirect the walk outside the supplied table pool.
 */
static inline int ggm_leaf(const struct ggm_tables *t,uint64_t iova,
    uint64_t owned_pa,uint64_t **out)
{
    if(!out || !ggm_table_valid(t) || iova>=UINT64_C(0x100000000) ||
       (iova&4095) || !owned_pa || (owned_pa&4095) || owned_pa>=(UINT64_C(1)<<36) ||
       (owned_pa>=t->physical && owned_pa-t->physical<t->bytes))return 0;
    size_t page=0,previous=SIZE_MAX;
    for(unsigned level=1;level<3;level++) {
        unsigned shift=39-9*level;
        size_t index=page*512+(size_t)((iova>>shift)&(level==1?3u:511u));
        uint64_t entry=t->words[index],pa=entry&GGM_ADDRESS;
        if((entry&~GGM_ADDRESS)!=3 || pa<t->physical ||
           pa-t->physical>t->bytes-4096)return 0;
        size_t next=(size_t)((pa-t->physical)/4096);
        if(!next || next==page || next==previous)return 0;
        previous=page;page=next;
    }
    uint64_t *leaf=&t->words[page*512+(size_t)((iova>>12)&511u)];
    uint64_t entry=*leaf;
    if(entry && (entry&GGM_ADDRESS)!=owned_pa)return 0;
    if(entry && (entry&3)!=0 && (entry&3)!=3)return 0;
    if((entry&3)==0 && entry && entry!=owned_pa)return 0;
    if(entry&~(GGM_ADDRESS|GGM_XN|UINT64_C(0xfff)))return 0;
    *out=leaf;return 1;
}

static inline int ggm_owned_valid(const struct ggm_tables *gpu,
    const struct ggm_tables *gmu,const struct ggm_owned *owned)
{
    if(!ggm_table_valid(gpu) || !ggm_table_valid(gmu) || !owned ||
       ((uintptr_t)gpu->words<(uintptr_t)gmu->words+gmu->bytes &&
        (uintptr_t)gmu->words<(uintptr_t)gpu->words+gpu->bytes) ||
       (gpu->physical<gmu->physical+gmu->bytes && gmu->physical<gpu->physical+gpu->bytes))return 0;
    for(unsigned i=0;i<GGD_CHUNKS;i++) {
        uint64_t pa=owned->physical[i];
        if(!pa || (pa&4095) || pa>=(UINT64_C(1)<<36) ||
           GGD_CHUNK>(UINT64_C(1)<<36)-pa ||
           (pa<gpu->physical+gpu->bytes && gpu->physical<pa+GGD_CHUNK) ||
           (pa<gmu->physical+gmu->bytes && gmu->physical<pa+GGD_CHUNK))return 0;
        for(unsigned j=0;j<i;j++)if(pa<owned->physical[j]+GGD_CHUNK && owned->physical[j]<pa+GGD_CHUNK)return 0;
    }
    return ggm_structure_valid(gpu) && ggm_structure_valid(gmu);
}

/* Exact HFI ACC/PRIV/WRITE bits; UCODE follows adreno_get_firmware.
 * GPU data uses IOMMU_NOEXEC. Vendor GMU get_attrs does not add NOEXEC.
 */
static inline uint64_t ggm_attributes(uint32_t flags,unsigned gmu,unsigned ucode)
{
    if(gmu) {
        if(ucode || !(flags&8u))return 0;
        return GGM_LEAF_COMMON|((flags&16u)?0:UINT64_C(0x40))|
            ((flags&32u)?0:UINT64_C(0x80));
    }
    if(ucode)return GGM_LEAF_COMMON|UINT64_C(0xc0);
    if(!(flags&1u))return 0;
    return GGM_LEAF_COMMON|GGM_XN|((flags&2u)?0:UINT64_C(0x40))|
        ((flags&4u)?0:UINT64_C(0x80));
}

static inline int ggm_allocation_valid(const struct ggd_allocation *a)
{
    return ggd_allocation_valid(a);
}

static inline int ggm_page(const struct ggm_tables *gpu,const struct ggm_tables *gmu,
    const struct ggm_owned *owned,uint32_t offset,uint32_t flags,unsigned ucode,unsigned action)
{
    if(!owned || offset>=GGD_BYTES || (offset&4095u) || (flags&~0x3ffu) ||
       ((flags&0x80u) && offset>=GGD_CACHE_BYTES) || ucode>1 || action>GGM_VERIFY)return 0;
    uint64_t pa=owned->physical[offset/GGD_CHUNK]+offset%GGD_CHUNK;
    uint64_t *leaves[3];
    if(!ggm_leaf(gpu,GGD_BASE+(uint64_t)offset,pa,&leaves[0]) ||
       !ggm_leaf(gmu,GGD_BASE+(uint64_t)offset,pa,&leaves[1]))return 0;
    unsigned count=2;
    if(offset<GGD_CACHE_BYTES) {
        if(!ggm_leaf(gmu,GGD_CACHE+(uint64_t)offset,pa,&leaves[2]))return 0;
        count=3;
    }
    if(action==GGM_CHECK)return 1;
    uint64_t attrs[3]={0,0,0};
    if(action==GGM_APPLY || action==GGM_VERIFY) {
        attrs[0]=ggm_attributes(flags,0,ucode);
        attrs[(flags&0x80u)?2:1]=ggm_attributes(flags,1,ucode);
    }
    for(unsigned i=0;i<count;i++) {
        if(action==GGM_VERIFY) {
            if(*leaves[i]!=(pa|attrs[i]) && (attrs[i] || *leaves[i]))return 0;
        } else *leaves[i]=pa|attrs[i];
    }
    return 1;
}

/* Each operation preflights the entire affected range before writing any leaf.
 * GGM_CLEAR never touches table descriptors or firmware/HFI leaves.
 */
static inline int ggm_allocation(const struct ggm_tables *gpu,const struct ggm_tables *gmu,
    const struct ggm_owned *owned,const struct ggd_allocation *a,unsigned ucode,unsigned action)
{
    if(!ggm_owned_valid(gpu,gmu,owned) || !ggm_allocation_valid(a) || ucode>1 || action>GGM_VERIFY)return 0;
    for(uint32_t i=0;i<a->bytes;i+=4096)if(!ggm_page(gpu,gmu,owned,a->offset+i,a->flags,ucode,GGM_CHECK))return 0;
    if(action!=GGM_CHECK)for(uint32_t i=0;i<a->bytes;i+=4096)
        if(!ggm_page(gpu,gmu,owned,a->offset+i,a->flags,ucode,action))return 0;
    return 1;
}

static inline int ggm_reset_pool(const struct ggm_tables *gpu,const struct ggm_tables *gmu,
    const struct ggm_owned *owned,unsigned clear)
{
    if(!ggm_owned_valid(gpu,gmu,owned) || clear>1)return 0;
    for(uint32_t at=0;at<GGD_BYTES;at+=4096)
        if(!ggm_page(gpu,gmu,owned,at,0,0,GGM_CHECK))return 0;
    if(clear)for(uint32_t at=0;at<GGD_BYTES;at+=4096)
        if(!ggm_page(gpu,gmu,owned,at,0,0,GGM_CLEAR))return 0;
    return 1;
}

static inline int ggm_audit_pool(const struct ggm_tables *gpu,const struct ggm_tables *gmu,
    const struct ggm_owned *owned,const struct ggd_pool *pool,const unsigned char *ucode)
{
    if(!ggm_owned_valid(gpu,gmu,owned) || !ggd_pool_valid(pool) || !ucode)return 0;
    for(unsigned i=0;i<pool->count;i++)if(ucode[i]>1)return 0;
    for(uint32_t at=0;at<GGD_BYTES;at+=4096) {
        uint32_t flags=0;unsigned code=0;
        for(unsigned i=0;i<pool->count;i++) {
            const struct ggd_allocation *a=&pool->allocations[i];
            if(at>=a->offset && at-a->offset<a->bytes){flags=a->flags;code=ucode[i];break;}
        }
        if(!ggm_page(gpu,gmu,owned,at,flags,code,GGM_VERIFY))return 0;
    }
    return 1;
}
#endif
