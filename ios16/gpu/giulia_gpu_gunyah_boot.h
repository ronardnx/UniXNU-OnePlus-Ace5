/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_GPU_GUNYAH_BOOT_H
#define GIULIA_GPU_GUNYAH_BOOT_H
#include "../../uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe/GpuGunyahReport.h"

/* Only a current loader-populated report is structurally accepted. This does
 * not validate the hypervisor, current capabilities, GIC masks or ownership.
 * Never use a successful decode as the live callback of the HVC adapter. */
static inline int ggr_boot_decode(const void *data,unsigned bytes,struct ggr_report *out)
{
    if(!out)return 0;
    for(unsigned i=0;i<sizeof(*out);i++)((unsigned char *)out)[i]=0;
    if(!data || bytes!=sizeof(*out))return 0;
    struct ggr_report copy;
    for(unsigned i=0;i<sizeof(copy);i++)((unsigned char *)&copy)[i]=((const unsigned char *)data)[i];
    if(!ggr_report_valid(&copy))return 0;
    *out=copy;return 1;
}
#endif
