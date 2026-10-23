/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_GPU_POWER_CONSUMER_H
#define GIULIA_GPU_POWER_CONSUMER_H
#include "../../uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe/GpuPowerReport.h"

enum { GGPH_INVALID, GGPH_RECORDED, GGPH_PREPARED };
static inline unsigned ggph_decode(const void *bytes,unsigned length,struct gpp_report *out)
{
    struct gpp_report r;
    unsigned i;
    if(out)for(i=0;i<sizeof(*out);i++)((unsigned char *)out)[i]=0;
    if(!bytes || !out || length!=sizeof(r))return GGPH_INVALID;
    for(i=0;i<sizeof(r);i++)((unsigned char *)&r)[i]=((const unsigned char *)bytes)[i];
    if(!gpp_report_valid(&r) || r.code>GPP_ERR_PROTOCOL ||
       !((r.phase>=GPP_PHASE_RESOLVE && r.phase<=GPP_PHASE_DONE) || r.phase==GPP_PHASE_LOADER) ||
       r.operation>GPP_OP_VALIDATE || r.resource>13 || r.failure_resource>13 ||
       (r.attempted&~0x1fffu) || (r.confirmed&~r.attempted) ||
       (r.valid_reads&~0xffffu) || (r.flags&~0x1ffu) || r.retained>1)return GGPH_INVALID;
    /* The GPU service does not receive a runtime lease. These checks only
     * establish the recorded firmware preparation result, never a DMA grant.
     */
    if(r.flags&GPP_READY) {
        if(r.code!=GPP_OK || r.phase!=GPP_PHASE_DONE || r.protocol_status ||
           r.firmware_status || r.last_firmware_status || r.failure_phase ||
           (r.flags&(GPP_PROTOCOL_UNCERTAIN|GPP_QMP_UNCERTAIN)) ||
           !(r.flags&GPP_ACD_CONSUMED) || !r.retained ||
           (r.confirmed&0x1ffeu)!=0x1ffeu ||
           (!(r.confirmed&1u) && !(r.flags&GPP_CFG_INHERITED)) ||
           (r.valid_reads&0x3fffu)!=0x3fffu ||
           !(r.values[0]&1) || (r.values[1]&1) || !(r.values[2]&0x80000000u) ||
           !(r.values[11]&1) || r.values[12]!=625000000u || r.values[13]!=200000000u)
            return GGPH_INVALID;
        for(i=0;i<=11;i++)if(r.values[i]==0xffffffffu)return GGPH_INVALID;
        /* Register slots: 3 GMU, 4 CXO, 5 AXI, 6 MEMNOC, 7 DVM, 8 AHB,
         * 9 HUB, 10 SMMU. Only CXO is a plain BRANCH_HALT branch in the pinned
         * gpucc-pineapple tables. The GMU, hub, SMMU vote, memnoc and DVM
         * branches are BRANCH_HALT_VOTED, so the vote bit is the strongest
         * provider-observable proof. AXI (HALT_SKIP) and AHB (HALT_DELAY) are
         * confirmed by the provider's accepted enable; their raw CBCR is not
         * required and AHB reads 0 on this board after a successful enable.
         */
        for(i=3;i<=10;i++) {
            if(i==5 || i==8)continue;
            if((r.values[i]&(i==4?0x80000001u:1u))!=1)return GGPH_INVALID;
        }
    } else if(r.code==GPP_OK)return GGPH_INVALID;
    *out=r;
    return r.flags&GPP_READY?GGPH_PREPARED:GGPH_RECORDED;
}
static inline int ggph_qmp_available(const void *bytes,unsigned length)
{
    struct gpp_report r;
    /* No requested GPU preparation means this handoff has no QMP ownership. */
    if(!bytes && !length)return 1;
    if(!ggph_decode(bytes,length,&r))return 0;
    return !(r.flags&(GPP_QMP_UNCERTAIN|GPP_PROTOCOL_UNCERTAIN));
}
#endif
