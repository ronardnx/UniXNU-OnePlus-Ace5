/* The actual kernel consumer is C++; portable protocol headers must compile
 * in the same language without C-only constructs or narrowing conversions. */
#include "../ios16/wifi/giulia_wifi_pci.h"
#include "../ios16/wifi/giulia_wifi_link.h"
#include "../ios16/wifi/giulia_wifi_smmu.h"
#include "../ios16/wifi/giulia_wifi_mhi.h"
#include "../ios16/wifi/giulia_wifi_ce.h"
#include "../ios16/wifi/giulia_wifi_apple.h"
#include "../ios16/wifi/giulia_wifi_firmware.h"
#include "../ios16/platform/giulia_platform_qmp.h"
#include "../ios16/wifi/giulia_wifi_pmu.h"
#include "../ios16/wifi/giulia_wifi_mapping.h"
#include "../ios16/wifi/giulia_wifi_network.h"
#include "../ios16/wifi/giulia_wifi_dp.h"
#include "../ios16/wifi/giulia_wifi_station.h"
#include "../ios16/wifi/giulia_wifi_rsn.h"
#include "../ios16/wifi/giulia_wifi_security.h"
#include "../ios16/display/giulia_display.h"
static_assert(sizeof(gwi_request) >= 20,"request layout");
static_assert((int)GWMAP_ROOT==(int)GWL_ROOT && (int)GWMAP_PARF==(int)GWL_PARF &&
    (int)GWMAP_PHY==(int)GWL_PHY && (int)GWMAP_GCC==(int)GWL_GCC &&
    (int)GWMAP_GPIO==(int)GWL_GPIO && (int)GWMAP_CFG==(int)GWL_CFG &&
    (int)GWMAP_REFCLK==(int)GWL_REFCLK && (int)GWMAP_CRM==(int)GWL_CRM,"link resource IDs");
static_assert(GWM_FIRMWARE+16u*1024*1024==GWMAP_CE_OFFSET,"firmware/CE boundary");
static_assert(GWC_BYTES<=GWMAP_CE_BYTES,"CE DMA partition");
static_assert(GWMAP_CE_OFFSET+GWMAP_CE_BYTES==GWMAP_ALLOC_START,"CE/QMI boundary");
