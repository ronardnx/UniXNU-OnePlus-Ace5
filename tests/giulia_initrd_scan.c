/* Host regression for the exact policy used before any physical scan reads. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef size_t UINTN;
typedef int BOOLEAN;
#define STATIC static
#define CONST const
#define TRUE 1
#define FALSE 0
#define MAX_UINT64 UINT64_MAX
#define EFI_PAGE_SIZE 4096ULL
#define EFI_MEMORY_RUNTIME (1ULL << 63)
#define EFI_MEMORY_RP 0x2000ULL
#define EfiConventionalMemory 7
#define EfiLoaderData 2
#define EfiBootServicesData 4
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define ALIGN_VALUE(x,a) (((x)+(a)-1)&~((UINT64)(a)-1))
#include "../uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe/ScanPolicy.h"

int main(void) {
  UINT64 s=0,e=0;
  const UINT64 payload=64ULL<<20;
  for(UINTN bank=0;bank<ARRAY_SIZE(mScanBanks);bank++) {
    // Regression: 64 MiB fits this misclassified low DDR descriptor, but the
    // first FAT signature access at 0x827001fe is in secure firmware memory.
    assert(!GiuliaScanIntersection(0x82700000,0x9500,7,0,bank,payload,&s,&e));
    assert(!GiuliaScanIntersection(0,UINT64_MAX,7,0,bank,payload,&s,&e));
    assert(!GiuliaScanIntersection(UINT64_MAX-4095,2,7,0,bank,payload,&s,&e));
    assert(!GiuliaScanIntersection(0x880000000,0,7,0,bank,payload,&s,&e));
    // A descriptor spanning bank holes must be clipped, never scan the holes.
    assert(GiuliaScanIntersection(0x80000000,0xA80000,7,0,bank,payload,&s,&e));
    assert(s==mScanBanks[bank][0] && e==mScanBanks[bank][1]);
  }
  for(UINT32 type=0;type<16;type++) {
    int ok=GiuliaScanIntersection(0xAA0213000,49152,type,0,2,payload,&s,&e);
    assert(ok==(type==2 || type==4 || type==7));
  }
  assert(GiuliaScanIntersection(0xAA0213000,49152,7,0,2,192ULL<<20,&s,&e));
  assert(s==0xAA0213000 && e==0xAAC213000); // Previous successful payload.
  assert(!GiuliaScanIntersection(0xAA0213000,49152,7,EFI_MEMORY_RP,2,payload,&s,&e));
  assert(!GiuliaScanIntersection(0xAA0213000,49152,7,EFI_MEMORY_RUNTIME,2,payload,&s,&e));
  assert(!GiuliaScanIntersection(0x8AF800000,0x2000,7,0,0,payload,&s,&e));
  assert(!GiuliaScanIntersection(0x8AF900000,0x10000,7,0,0,payload,&s,&e));
  assert(GiuliaScanIntersection(0x880000000,16384,7,0,0,payload,&s,&e));
  assert(!GiuliaScanIntersection(0x880000001,16384,7,0,0,payload,&s,&e));
  assert(!GiuliaScanIntersection(0x880000000,16383,7,0,0,payload,&s,&e));
  assert(!GiuliaScanIntersection(0x880000000,16384,7,0,3,payload,&s,&e));
  assert(!GiuliaScanIntersection(0x880000000,16384,7,0,0,0,&s,&e));
  puts("Initrd scan policy: secure DDR, holes, descriptor types, attributes, overflow and payload bounds passed");
  return 0;
}
