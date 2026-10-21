/* Ace 5 SM8650: retarget only SID60 after the UFS queues are quiescent.
 * CB2 is preserved. CB70 must be disabled and unreferenced before use.
 * Register geometry was observed on this handset; DMA is not yet verified.
 */
#ifndef GIULIA_SMMU_H
#define GIULIA_SMMU_H
#include <stdint.h>
typedef uint32_t (*gs_read)(unsigned);
typedef void (*gs_write)(unsigned, uint32_t);
static int gs_plan(gs_read r)
{
    if (r(0x20)!=0x4c017e6b || r(0x24)!=0x60000047 || r(0x28)!=0x5111) return 1;
    if (r(0x808)!=0x80000060 || r(0xc08)!=2 || (r(0xc6000)&1)) return 2;
    unsigned matches=0;
    for (unsigned i=0;i<107;i++) {
        uint32_t m=r(0x800+4*i), route=r(0xc00+4*i);
        if (!(m>>31)) continue;
        if (!((m^0x60)&~((m>>16)&0x7fff)&0xffff)) matches++;
        if (((route>>16)&3)==0 && (route&255)==70) return 3;
    }
    return matches==1 ? 0 : 4;
}
static int gs_take(gs_read r, gs_write w, int quiescent)
{
    if (!quiescent || gs_plan(r)) return 1;
    w(0xc6000,0); w(0x1918,1); w(0x1118,0x10000);
    if (r(0xc6000)!=0 || r(0x1918)!=1 || r(0x1118)!=0x10000) return 2;
    /* Recheck the stream just before publishing the new context. */
    if (r(0x808)!=0x80000060 || r(0xc08)!=2) return 3;
    w(0xc08,70);
    return r(0xc08)==70 ? 0 : 4;
}
static int gs_restore(gs_read r, gs_write w, int quiescent)
{
    if (!quiescent || r(0x808)!=0x80000060) return 1;
    if (r(0xc08)==2) return 0;
    if (r(0xc08)!=70) return 2;
    w(0xc08,2);
    return r(0xc08)==2 ? 0 : 3;
}
#endif
