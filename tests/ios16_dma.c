#include <stdint.h>
#include <assert.h>
#define VERMEER_HOST_TEST 1
uint64_t gPhysBase=0x880000000ULL, gPhysSize=0x80000000ULL;
#include "vermeer_ufs_dma.h"
#include "vermeer_usb_protocol.h"
#include "vermeer_media_policy.h"
int main(void) {
 assert(vu_dma_allowed(gPhysBase,1,0x01000000,0x5111));
 assert(!vu_dma_allowed(gPhysBase,0,0x01000000,0x5111));
 assert(!vu_dma_allowed(gPhysBase,1,0,0x5111));
 assert(!vu_dma_allowed(gPhysBase-0x4000,1,0x01000000,0x5111));
 assert(!vu_dma_allowed(gPhysBase+gPhysSize-0x4000,1,0x01000000,0x5111));
 assert(vusb_dma_allowed(gPhysBase,0x4000,36));
 assert(!vusb_dma_allowed(gPhysBase,0x4000,32));
 assert(!vusb_dma_allowed(gPhysBase+gPhysSize,0x4000,36));
 assert(!vusb_dma_allowed(gPhysBase+1,0x4000,36));
 assert(!vusb_dma_allowed(UINT64_MAX-0x3fff,0x4000,64));
 assert(vi_write_allowed(1,48738048,12582912,48738048,2));
 assert(!vi_write_allowed(1,2570304,59272123,2570304,2));
 assert(!vi_write_allowed(1,48738048,12582912,48738048+4194304-1,2));
 gPhysSize=1;
 assert(!vusb_dma_allowed(gPhysBase,0x4000,36));
 assert(!vu_dma_allowed(gPhysBase,1,0x01000000,0x5111));
 return 0;
}
