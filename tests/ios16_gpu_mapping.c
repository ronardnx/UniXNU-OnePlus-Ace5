/* Host-only independent PTE oracle: no device access. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/giulia_gpu_mapping.h"
#include "../ios16/vendor/ArmSmmu4K/ArmSmmu4K.h"
static uint64_t gt[128*512],mt[128*512],before[128*512],before_mt[128*512];
static struct ggm_tables gpu={gt,sizeof(gt),UINT64_C(0x880000000)};
static struct ggm_tables gmu={mt,sizeof(mt),UINT64_C(0x880100000)};
static struct ggm_owned owned;
static void build(void)
{
    ArmSmmu4KRange ranges[GGD_CHUNKS+2];
    for(unsigned i=0;i<GGD_CHUNKS;i++) {
        owned.physical[i]=UINT64_C(0x900000000)+(uint64_t)i*0x800000;
        ranges[i]=(ArmSmmu4KRange){GGD_BASE+(uint64_t)i*GGD_CHUNK,owned.physical[i],GGD_CHUNK,15,0,2};
    }
    ArmSmmu4KConfig c={32,36,gpu.physical,32768};ArmSmmu4KResult result;
    memset(gt,0,sizeof(gt));memset(mt,0,sizeof(mt));
    assert(ArmSmmu4KBuild(&c,ranges,GGD_CHUNKS,gt,128,&result)==ArmSmmu4KSuccess);
    memmove(ranges+2,ranges,sizeof(*ranges)*GGD_CHUNKS);
    ranges[0]=(ArmSmmu4KRange){GGD_CACHE,owned.physical[0],GGD_CACHE_BYTES,15,0,2};
    ranges[1]=(ArmSmmu4KRange){0x60010000,UINT64_C(0x990000000),4096,15,0,2};
    c.table_physical=gmu.physical;
    assert(ArmSmmu4KBuild(&c,ranges,GGD_CHUNKS+2,mt,128,&result)==ArmSmmu4KSuccess);
}
static uint64_t *slot(const struct ggm_tables *t,uint64_t va)
{
    /* Independent walk, no production leaf lookup/attribute encoder. */
    uint64_t *p=t->words;
    for(unsigned shift=30;shift>=21;shift-=9) {
        uint64_t e=p[(va>>shift)&511u];assert((e&3)==3);
        uint64_t pa=e&UINT64_C(0x0000fffffffff000);
        assert(pa>=t->physical && pa-t->physical<t->bytes);
        p=t->words+(pa-t->physical)/8;
    }
    return &p[(va>>12)&511u];
}
static void apply(struct ggd_allocation *a,unsigned code)
{
    assert(ggm_allocation(&gpu,&gmu,&owned,a,code,GGM_CLEAR));
    assert(ggm_allocation(&gpu,&gmu,&owned,a,code,GGM_APPLY));
    assert(ggm_allocation(&gpu,&gmu,&owned,a,code,GGM_VERIFY));
}
static uint64_t *parent(const struct ggm_tables *t,uint64_t va)
{
    uint64_t pa=t->words[va>>30]&UINT64_C(0x0000fffffffff000);
    assert(pa>=t->physical && pa-t->physical<t->bytes);
    return &t->words[(pa-t->physical)/8+((va>>21)&511u)];
}
static void reject_corrupt_tree(const struct ggd_allocation *a)
{
    struct ggd_pool pool={0};unsigned char ucode[GGD_MAX]={0};
    pool.next=GGD_CACHE_BYTES;
    assert(ggm_allocation_valid(a));
    memcpy(before,gt,sizeof(gt));memcpy(before_mt,mt,sizeof(mt));
    for(unsigned action=GGM_CHECK;action<=GGM_VERIFY;action++)
        assert(!ggm_allocation(&gpu,&gmu,&owned,a,0,action));
    assert(!ggm_reset_pool(&gpu,&gmu,&owned,0));
    assert(!ggm_reset_pool(&gpu,&gmu,&owned,1));
    assert(!ggm_audit_pool(&gpu,&gmu,&owned,&pool,ucode));
    assert(!memcmp(before,gt,sizeof(gt)) && !memcmp(before_mt,mt,sizeof(mt)));
}
static void topology(void)
{
    struct ggd_allocation a={GGD_CACHE_BYTES,4096,0x33f,2};
    build();assert(ggm_structure_valid(&gpu) && ggm_structure_valid(&gmu));
    /* A second root entry would mirror all pool pages outside the pool. */
    gt[2]=gt[1];reject_corrupt_tree(&a);
    build();
    /* Empty leaves hide the PA mismatch from the per-page ownership check.
     * Reusing the first pool leaf table for HFI must fail before any writes. */
    memset(slot(&gmu,GGD_BASE),0,4096);
    *parent(&gmu,0x60010000)=*parent(&gmu,GGD_BASE);
    a.offset=0x10000;a.flags=0x3bf;assert(!*slot(&gmu,0x60010000));reject_corrupt_tree(&a);
    build();
    memset(slot(&gmu,GGD_BASE),0,4096);
    *parent(&gmu,GGD_CACHE)=*parent(&gmu,GGD_BASE);
    a.offset=0;a.flags=0x3bf;reject_corrupt_tree(&a);
    build();
    /* A leaf-table page must not be reused at another table level either. */
    memset(slot(&gpu,GGD_BASE),0,4096);
    gt[2]=*parent(&gpu,GGD_BASE);reject_corrupt_tree(&a);
    build();
    /* Even an unvisited branch must stay inside the owned table allocation. */
    gt[3]=(gpu.physical+gpu.bytes)|3;reject_corrupt_tree(&a);
}
static void page_bounds(void)
{
    build();memcpy(before,gt,sizeof(gt));memcpy(before_mt,mt,sizeof(mt));
    assert(!ggm_page(&gpu,&gmu,NULL,0,0,0,GGM_APPLY));
    assert(!ggm_page(&gpu,&gmu,&owned,GGD_BYTES,0,0,GGM_APPLY));
    assert(!ggm_page(&gpu,&gmu,&owned,UINT32_MAX,0,0,GGM_APPLY));
    assert(!ggm_page(&gpu,&gmu,&owned,1,0,0,GGM_APPLY));
    assert(!ggm_page(&gpu,&gmu,&owned,0,0x1000,0,GGM_APPLY));
    assert(!ggm_page(&gpu,&gmu,&owned,GGD_CACHE_BYTES,0x80,0,GGM_APPLY));
    assert(!ggm_page(&gpu,&gmu,&owned,0,0,2,GGM_APPLY));
    assert(!ggm_page(&gpu,&gmu,&owned,0,0,0,GGM_VERIFY+1));
    assert(!memcmp(before,gt,sizeof(gt)) && !memcmp(before_mt,mt,sizeof(mt)));
}
int main(void)
{
    build();assert(ggm_owned_valid(&gpu,&gmu,&owned));
    struct ggm_tables overlapping=gmu;overlapping.words=gpu.words+512;
    assert(!ggm_owned_valid(&gpu,&overlapping,&owned));
    uint64_t hfi=*slot(&gmu,0x60010000);
    assert(ggm_reset_pool(&gpu,&gmu,&owned,1));
    assert(*slot(&gmu,0x60010000)==hfi);
    for(uint32_t at=0;at<GGD_BYTES;at+=4096) {
        assert(!(*slot(&gpu,GGD_BASE+(uint64_t)at)&3));
        assert(!(*slot(&gmu,GGD_BASE+(uint64_t)at)&3));
        if(at<GGD_CACHE_BYTES)assert(!(*slot(&gmu,GGD_CACHE+(uint64_t)at)&3));
    }
    const uint64_t normal=GGD_BASE+GGD_CACHE_BYTES;
    struct ggd_allocation a={GGD_CACHE_BYTES,4096,0x339,15};apply(&a,0);
    uint64_t g=*slot(&gpu,normal),m=*slot(&gmu,normal);
    assert((g&0xc0)==0xc0 && (g&(UINT64_C(3)<<53))==(UINT64_C(3)<<53));
    assert((m&0xc0)==0 && !(m&(UINT64_C(3)<<53)) && (m&3)==3);
    assert(!(*slot(&gmu,GGD_CACHE)&3));
    a.flags=0x37b;a.kind=1;apply(&a,0);assert((*slot(&gpu,normal)&0xc0)==0x80);
    /* GMU cache selection changes VA only, not PTE cache attributes. */
    a.offset=0;a.flags=0x3bf;apply(&a,0);
    assert(!(*slot(&gmu,GGD_BASE)&3) && (*slot(&gmu,GGD_CACHE)&3)==3);
    assert(!(*slot(&gmu,GGD_CACHE)&0x1c));
    assert(ggm_allocation(&gpu,&gmu,&owned,&a,0,GGM_CLEAR));
    a.offset=GGD_CACHE_BYTES;a.flags=0x178;a.kind=16;apply(&a,0);assert(!(*slot(&gpu,normal)&3));
    a.flags=0x147;a.kind=7;apply(&a,0);assert(!(*slot(&gmu,normal)&3));
    a.flags=0x33f;a.kind=0;apply(&a,1);
    g=*slot(&gpu,normal);assert((g&0xc0)==0xc0 && !(g&(UINT64_C(3)<<53)));
    assert(!(*slot(&gmu,normal)&3) && !(*slot(&gmu,GGD_CACHE)&3));
    /* Independently compare the complete descriptors for every permission,
     * alias selection and firmware policy combination, including absent access. */
    for(unsigned cache=0;cache<2;cache++)for(unsigned code=0;code<2;code++)for(unsigned f=0;f<64;f++) {
        a.offset=cache?0:GGD_CACHE_BYTES;a.flags=f|(cache?0x80:0);apply(&a,code);
        uint64_t pa=owned.physical[cache?0:1],va=GGD_BASE+a.offset;
        uint64_t ga=code?UINT64_C(0xec3):(f&1)?UINT64_C(0x60000000000e03)|
            (f&2?0u:64u)|(f&4?0u:128u):0;
        uint64_t ma=!code && (f&8)?UINT64_C(0xe03)|(f&16?0u:64u)|(f&32?0u:128u):0;
        assert(*slot(&gpu,va)==(pa|ga));
        assert(*slot(&gmu,va)==(pa|(cache?0:ma)));
        if(cache)assert(*slot(&gmu,GGD_CACHE)==(pa|ma));
    }
    assert(ggm_reset_pool(&gpu,&gmu,&owned,1));
    struct ggd_pool pool={0};unsigned char ucode[GGD_MAX]={0};
    pool.count=1;pool.next=GGD_CACHE_BYTES+4096;
    pool.allocations[0]=(struct ggd_allocation){GGD_CACHE_BYTES,4096,0x33f,2};
    apply(&pool.allocations[0],0);assert(ggm_audit_pool(&gpu,&gmu,&owned,&pool,ucode));
    *slot(&gpu,GGD_BASE+4096)|=3;assert(!ggm_audit_pool(&gpu,&gmu,&owned,&pool,ucode));
    *slot(&gpu,GGD_BASE+4096)&=~UINT64_C(3);
    pool.count=2;pool.allocations[1]=pool.allocations[0];assert(!ggm_audit_pool(&gpu,&gmu,&owned,&pool,ucode));
    /* Preflight finds the bad last page before touching an earlier page. */
    a=(struct ggd_allocation){GGD_CACHE_BYTES,8192,0x33f,2};
    *slot(&gpu,normal+4096)=UINT64_C(0x980000000)|0xe03;
    memcpy(before,gt,sizeof(gt));memcpy(before_mt,mt,sizeof(mt));
    assert(!ggm_allocation(&gpu,&gmu,&owned,&a,0,GGM_APPLY));
    assert(!memcmp(before,gt,sizeof(gt)) && !memcmp(before_mt,mt,sizeof(mt)));
    build();uint64_t *out=NULL,saved=gt[1];
    gt[1]=UINT64_C(0x980000000)|3;assert(!ggm_leaf(&gpu,GGD_BASE,owned.physical[0],&out));
    gt[1]=gpu.physical|3;assert(!ggm_leaf(&gpu,GGD_BASE,owned.physical[0],&out));
    gt[1]=saved;*slot(&gpu,GGD_BASE)=0;assert(ggm_leaf(&gpu,GGD_BASE,owned.physical[0],&out));
    *slot(&gpu,GGD_BASE)=owned.physical[0]|1;assert(!ggm_leaf(&gpu,GGD_BASE,owned.physical[0],&out));
    *slot(&gpu,GGD_BASE)=owned.physical[0]|0x400;assert(!ggm_leaf(&gpu,GGD_BASE,owned.physical[0],&out));
    *slot(&gpu,GGD_BASE)=owned.physical[0];assert(ggm_leaf(&gpu,GGD_BASE,owned.physical[0],&out));
    assert(!ggm_leaf(&gpu,GGD_BASE+1,owned.physical[0],&out));
    assert(!ggm_leaf(&gpu,UINT64_C(0x100000000),owned.physical[0],&out));
    assert(!ggm_leaf(&gpu,GGD_BASE,gpu.physical,&out));
    a=(struct ggd_allocation){GGD_BYTES-4096,8192,0x33f,2};assert(!ggm_allocation_valid(&a));
    a=(struct ggd_allocation){GGD_CACHE_BYTES,4096,0x3bf,2};assert(!ggm_allocation_valid(&a));
    a=(struct ggd_allocation){0,4096,0x1007,5};assert(!ggm_allocation_valid(&a));
    owned.physical[1]=owned.physical[0];assert(!ggm_owned_valid(&gpu,&gmu,&owned));
    topology();page_bounds();
    puts("Owned mapping permissions, absent aliases, firmware executable policy, full-pool audit, unique table topology and corruption bounds passed");
    return 0;
}
