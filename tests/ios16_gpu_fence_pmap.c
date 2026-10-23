/* Pure descriptor snapshots only; no native page tables or device access. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_fence_pmap.h"

static uint64_t leaf(uint64_t pa,unsigned writable)
{
    return pa|UINT64_C(0x60000000000407)|(writable?0:UINT64_C(0x80));
}
static void spans(unsigned page,unsigned writable)
{
    const uint64_t pa=UINT64_C(0xd4f70000),mair=UINT64_C(0x4400);
    const uintptr_t va=(uintptr_t)UINT64_C(0xfffffff010000000);
    uint64_t leaves[16];
    const size_t bytes=65536,count=bytes/page;
    for(size_t i=0;i<count;i++)leaves[i]=leaf(pa+i*page,writable);
    assert(ggfpm_leaf_span_valid(va,pa,bytes,page,writable,mair,leaves,count));
    /* Every position, including interior and padding, must be audited. */
    for(size_t i=0;i<count;i++) {
        const uint64_t saved=leaves[i];
        const unsigned bits[]={0,1,2,6,7,10,12,51,53,54};
        for(size_t j=0;j<sizeof(bits)/sizeof(bits[0]);j++) {
            leaves[i]=saved^(UINT64_C(1)<<bits[j]);
            assert(!ggfpm_leaf_span_valid(va,pa,bytes,page,writable,mair,leaves,count));
        }
        leaves[i]=saved;
    }
    assert(!ggfpm_leaf_span_valid(va,pa,bytes,page,writable,mair,leaves,count-1));
    assert(!ggfpm_leaf_span_valid(va,pa,bytes,page,writable,mair,leaves,count+1));
    assert(!ggfpm_leaf_span_valid(va,pa,bytes,page,writable,0,leaves,count));
    assert(!ggfpm_leaf_span_valid(va,pa,bytes,page,writable,mair,NULL,count));
    assert(!ggfpm_leaf_span_valid(va,pa,bytes,page,writable,mair,
        (const uint64_t *)((const unsigned char *)leaves+1),count));
    assert(!ggfpm_audit_range(0x10000,pa,bytes,page,writable));
    assert(!ggfpm_audit_range(va+1,pa,bytes,page,writable));
    assert(!ggfpm_audit_range(va,pa+1,bytes,page,writable));
    assert(!ggfpm_audit_range(va,pa,bytes-1,page,writable));
    assert(!ggfpm_audit_range(va,pa,bytes,page,2));
    assert(!ggfpm_audit_range(UINTPTR_MAX-page+1,pa,page,page,writable));
    assert(!ggfpm_leaf_valid(leaf(pa,writable),pa|(UINT64_C(1)<<48),page,writable,mair));
}
static void boundaries(unsigned page)
{
    const uint64_t first=GGFPM_PHYSICAL&~((uint64_t)page-1);
    const uint64_t end=(GGFPM_END+page-1)&~((uint64_t)page-1);
    assert(!ggfpm_intersects(first-page,page,page));
    assert(ggfpm_intersects(first-1,2,page));
    assert(ggfpm_intersects(end-1,1,page));
    assert(!ggfpm_intersects(end,page,page));
    assert(!ggfpm_intersects(first,0,page));
    assert(!ggfpm_cache_allowed(first,page,page,0));
    assert(ggfpm_cache_allowed(first,page,page,1));
    assert(!ggfpm_cache_allowed(UINT64_MAX-1,3,page,1));
    assert(!ggfpm_audit_range((uintptr_t)UINT64_C(0xfffffff010000000),
        first,page,page,0) || first==GGFPM_PHYSICAL);
}
int main(void)
{
    const unsigned pages[]={4096,16384,65536};
    for(unsigned i=0;i<3;i++) {
        boundaries(pages[i]);
        for(unsigned writable=0;writable<2;writable++)spans(pages[i],writable);
    }
    assert(!ggfpm_cache_allowed(0,0,8192,1));
    assert(ggfpm_tg1_page(UINT64_C(1)<<30)==16384);
    assert(ggfpm_tg1_page(UINT64_C(2)<<30)==4096);
    assert(ggfpm_tg1_page(UINT64_C(3)<<30)==65536);
    assert(!ggfpm_tg1_page(0));
    puts("GPU fence pmap: full leaf spans, permissions, NC attributes, padding and overflow passed; synthetic snapshots only");
    return 0;
}
