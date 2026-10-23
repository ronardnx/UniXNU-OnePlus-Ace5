/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_GPU_POWER_CONSUMER_H
#define GIULIA_GPU_POWER_CONSUMER_H
#include "../uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe/GpuPowerReport.h"

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
       (r.valid_reads&~0xffffu) || (r.flags&~0x3ffu) || r.retained>1)return GGPH_INVALID;
    /* The GPU service does not receive a runtime lease. These checks only
     * establish the recorded firmware preparation result, never a DMA grant.
     */
    if(r.flags&GPP_READY) {
        if(r.code!=GPP_OK || r.phase!=GPP_PHASE_DONE || r.protocol_status ||
           r.firmware_status || r.last_firmware_status || r.failure_phase ||
           (r.flags&(GPP_PROTOCOL_UNCERTAIN|GPP_QMP_UNCERTAIN)) ||
           !(r.flags&GPP_ACD_CONSUMED) || !r.retained ||
           (r.confirmed&((r.flags&GPP_DIRECT_CLOCKS)?0x1a9fu:0x1ffeu))!=((r.flags&GPP_DIRECT_CLOCKS)?0x1a9fu:0x1ffeu) ||
           (!(r.confirmed&1u) && !(r.flags&GPP_CFG_INHERITED)) ||
           (r.valid_reads&0x3fffu)!=0x3fffu ||
           !(r.values[0]&1) || (r.values[1]&1) || !(r.values[2]&0x80000000u) ||
           !(r.values[11]&1) || r.values[12]!=((r.flags&GPP_DIRECT_CLOCKS)?260000000u:625000000u) || r.values[13]!=200000000u)
            return GGPH_INVALID;
        for(i=0;i<=11;i++)if(r.values[i]==0xffffffffu)return GGPH_INVALID;
        for(i=3;i<=10;i++) {
            unsigned direct_on=(r.flags&GPP_DIRECT_CLOCKS) && (i==8 || i==9);
            unsigned mask=direct_on?0x80000000u:i==5?1u:0x80000001u; /* DDRSS AXI HALT_SKIP. */
            if((r.values[i]&mask)!=(direct_on?0u:1u))return GGPH_INVALID;
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
