#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_dev.h"
static void rejected(struct ggd_pool *p,uint32_t bytes,uint32_t align,uint32_t flags,uint32_t kind) {
 struct ggd_pool before=*p;
 assert(ggd_alloc(p,bytes,align,flags,kind)<0);
 assert(!memcmp(p,&before,sizeof(before)));
}
static void corrupt_pool(void) {
 struct ggd_pool p={0};
 assert(!ggd_pool_valid(NULL));assert(ggd_alloc(NULL,1,4096,0,0)<0);
 rejected(&p,1,4096,0,0); /* ordinary cursor cannot enter the cached backing */
 p.next=GGD_CACHE_BYTES;assert(ggd_pool_valid(&p));
 p.count=GGD_MAX+1;rejected(&p,1,4096,0,0);p.count=UINT32_MAX;rejected(&p,1,4096,0,0);p.count=0;
 p.next=UINT32_MAX;rejected(&p,1,4096,0,0);
 p.next=GGD_BYTES+4096;rejected(&p,1,4096,0,0);
 p.next=GGD_CACHE_BYTES+1;rejected(&p,1,4096,0,0);p.next=GGD_CACHE_BYTES;
 p.cached=UINT32_MAX;rejected(&p,1,4096,0x80,0);
 p.cached=GGD_CACHE_BYTES+4096;rejected(&p,1,4096,0x80,0);
 p.cached=1;rejected(&p,1,4096,0x80,0);p.cached=0;
 assert(ggd_alloc(&p,4096,4096,0,0)==0);
 p.next-=4096;rejected(&p,1,4096,0,0);p.next+=4096;
 p.allocations[0].offset=0;rejected(&p,1,4096,0,0);p.allocations[0].offset=GGD_CACHE_BYTES;
 p.allocations[0].flags=0x80;rejected(&p,1,4096,0,0);p.allocations[0].flags=0;
 p.allocations[1]=p.allocations[0];p.count=2;rejected(&p,1,4096,0,0);p.count=1;
 p.allocations[0].bytes=UINT32_MAX;rejected(&p,1,4096,0,0);
}
static void capacity_and_kinds(void) {
 struct ggd_pool p={0};p.next=GGD_CACHE_BYTES;
 for(unsigned kind=0;kind<26;kind++) {
  int supported=kind!=5&&kind!=19&&kind!=22&&kind!=23;
  if(supported)assert(ggd_alloc(&p,1,4096,0,kind)>=0);else rejected(&p,1,4096,0,kind);
 }
 rejected(&p,1,4096,0,26);
 while(p.count<GGD_MAX)assert(ggd_alloc(&p,1,4096,0,0)>=0);
 rejected(&p,1,4096,0,0);
 memset(&p,0,sizeof(p));p.next=GGD_CACHE_BYTES;
 assert(ggd_alloc(&p,GGD_CACHE_BYTES,4096,0x80,0)==0);
 assert(ggd_alloc(&p,GGD_BYTES-GGD_CACHE_BYTES,4096,0,0)==1);
 assert(ggd_pool_valid(&p));rejected(&p,1,4096,0x80,0);rejected(&p,1,4096,0,0);
 assert(ggd_owner(&p,0,GGD_CACHE_BYTES)==0);
 assert(ggd_owner(&p,GGD_CACHE_BYTES,GGD_BYTES-GGD_CACHE_BYTES)==1);
 assert(ggd_owner(&p,GGD_CACHE_BYTES-4,8)<0);
 assert(ggd_owner(&p,UINT32_MAX,4)<0 && ggd_owner(&p,0,0)<0);
}
static void gen7_named_mmio_sysram(void) {
 const unsigned kinds[]={11,12,13,20};
 struct ggd_pool p={0};p.next=GGD_CACHE_BYTES;
 for(unsigned i=0;i<sizeof(kinds)/sizeof(*kinds);i++) {
  unsigned kind=kinds[i];
  /* These are effective flags supplied by the HFI host. Kernel debug calls
   * still store exactly the caller's flags instead of inventing access. */
  int gmu=ggd_alloc(&p,24,8192,0x38,kind);
  assert(gmu>=0 && p.allocations[gmu].kind==kind && p.allocations[gmu].flags==0x38);
  assert(p.allocations[gmu].bytes==4096 && !(p.allocations[gmu].offset&8191));
  assert(ggd_owner(&p,p.allocations[gmu].offset,24)==gmu);
  int cache=ggd_alloc(&p,4097,4096,0xb8,kind);
  assert(cache>=0 && p.allocations[cache].offset<GGD_CACHE_BYTES);
  assert(p.allocations[cache].bytes==8192 && p.allocations[cache].flags==0xb8);
  int gfx=ggd_alloc(&p,4096,4096,0x3f,kind);
  assert(gfx>=0 && p.allocations[gfx].offset>=GGD_CACHE_BYTES && p.allocations[gfx].flags==0x3f);
  int raw=ggd_alloc(&p,4096,4096,0,kind);
  assert(raw>=0 && p.allocations[raw].flags==0);
 }
 assert(ggd_pool_valid(&p));
}
int main(void) {
 struct ggd_pool p={0};p.next=GGD_CACHE_BYTES;
 uint32_t v[6];assert(ggd_parse("1 1 0x1000 4096 0x33f 2",v));
 assert(!ggd_parse("1 1 0x100000000 0 0 0",v));
 assert(!ggd_parse("1 2 3 4 5",v));assert(!ggd_parse("1 2 3 4 5 6 7",v));
 int n=ggd_alloc(&p,4096,4096,0x33f,2);assert(n==0 && p.allocations[n].offset==0x400000);
 assert(ggd_alloc(&p,1,4096,0x3bf,0)==1 && p.allocations[1].offset==0);
 assert(ggd_alloc(&p,4096,8192,0x33f,0)==2 && p.allocations[2].offset==0x402000);
 assert(ggd_alloc(&p,0xffffffff,4096,0,0)<0);assert(ggd_alloc(&p,1,8193,0,0)<0);
 assert(ggd_alloc(&p,1,4096,0x1000,0)<0);assert(!ggd_bounds(0xffffffff,4));
 assert(ggd_alloc(&p,GGD_BYTES,4096,0,0)<0);
 corrupt_pool();capacity_and_kinds();gen7_named_mmio_sysram();
 puts("GPU development allocation bounds, corruption, pool isolation, unsupported kinds and protocol passed");
}
