#include <assert.h>
#include <stdio.h>
#include "../ios16/giulia_gpu_dev.h"
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
 puts("GPU development allocation bounds and protocol passed");
}