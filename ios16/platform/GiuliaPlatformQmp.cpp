/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Firmware handoff policy shared by every AOP mailbox client.
 * No dependency on the GPU service or its lifecycle.
 */
#include <pexpert/arm64/board_config.h>
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include "giulia_platform_power.h"
#include "giulia_platform_resources.h"

extern "C" int giulia_platform_qmp_available(void)
{
    IORegistryEntry *chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);
    if(!chosen)return 0;
    OSObject *property=chosen->getProperty("giulia-gpu-power");
    OSData *data=OSDynamicCast(OSData,property);
    int available=!property || (data && data->getBytesNoCopy() &&
        data->getLength()==sizeof(struct gpp_report) &&
        ggph_qmp_available(data->getBytesNoCopy(),data->getLength()));
    chosen->release();return available;
}
#endif
