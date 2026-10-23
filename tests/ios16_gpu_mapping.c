/* Host-only independent PTE oracle: no device access. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/giulia_gpu_mapping.h"
#include "../ios16/vendor/ArmSmmu4K/ArmSmmu4K.h"
static uint64_t gt[128*512],mt[128*512],before[128*512];
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
    struct ggd_allocation a={0,4096,0x339,15};apply(&a,0);
    uint64_t g=*slot(&gpu,GGD_BASE),m=*slot(&gmu,GGD_BASE);
    assert((g&0xc0)==0xc0 && (g&(UINT64_C(3)<<53))==(UINT64_C(3)<<53));
    assert((m&0xc0)==0 && !(m&(UINT64_C(3)<<53)) && (m&3)==3);
    assert(!(*slot(&gmu,GGD_CACHE)&3));
    a.flags=0x37b;a.kind=1;apply(&a,0);assert((*slot(&gpu,GGD_BASE)&0xc0)==0x80);
    /* GMU cache selection changes VA only, not PTE cache attributes. */
    a.flags=0x3bf;apply(&a,0);
    assert(!(*slot(&gmu,GGD_BASE)&3) && (*slot(&gmu,GGD_CACHE)&3)==3);
    assert(!(*slot(&gmu,GGD_CACHE)&0x1c));
    a.flags=0x178;a.kind=16;apply(&a,0);assert(!(*slot(&gpu,GGD_BASE)&3));
    a.flags=0x147;a.kind=7;apply(&a,0);assert(!(*slot(&gmu,GGD_BASE)&3));
    a.flags=0x33f;a.kind=0;apply(&a,1);
    g=*slot(&gpu,GGD_BASE);assert((g&0xc0)==0xc0 && !(g&(UINT64_C(3)<<53)));
    assert(!(*slot(&gmu,GGD_BASE)&3) && !(*slot(&gmu,GGD_CACHE)&3));
    /* Every combination has independently derived GPU and GMU AP bits. */
    for(unsigned f=0;f<64;f++) {
        a.flags=f;apply(&a,0);g=*slot(&gpu,GGD_BASE);m=*slot(&gmu,GGD_BASE);
        assert(!!(g&1)==!!(f&1) && !!(m&1)==!!(f&8));
        if(f&1)assert((g&0xc0)==((f&2?0u:64u)|(f&4?0u:128u)));
        if(f&8)assert((m&0xc0)==((f&16?0u:64u)|(f&32?0u:128u)));
    }
    assert(ggm_reset_pool(&gpu,&gmu,&owned,1));
    struct ggd_pool pool={0};unsigned char ucode[GGD_MAX]={0};
    pool.count=1;pool.allocations[0]=(struct ggd_allocation){0,4096,0x33f,2};
    apply(&pool.allocations[0],0);assert(ggm_audit_pool(&gpu,&gmu,&owned,&pool,ucode));
    *slot(&gpu,GGD_BASE+4096)|=3;assert(!ggm_audit_pool(&gpu,&gmu,&owned,&pool,ucode));
    *slot(&gpu,GGD_BASE+4096)&=~UINT64_C(3);
    pool.count=2;pool.allocations[1]=pool.allocations[0];assert(!ggm_audit_pool(&gpu,&gmu,&owned,&pool,ucode));
    /* Preflight finds the bad last page before touching an earlier page. */
    a=(struct ggd_allocation){0,8192,0x33f,2};
    *slot(&gpu,GGD_BASE+4096)=UINT64_C(0x980000000)|0xe03;
    memcpy(before,gt,sizeof(gt));assert(!ggm_allocation(&gpu,&gmu,&owned,&a,0,GGM_APPLY));
    assert(!memcmp(before,gt,sizeof(gt)));
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
    puts("Owned mapping permissions, absent aliases, firmware executable policy, full-pool audit and corruption bounds passed");
    return 0;
}
