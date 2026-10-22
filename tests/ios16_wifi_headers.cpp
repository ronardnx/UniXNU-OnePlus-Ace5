/* The actual kernel consumer is C++; portable protocol headers must compile
 * in the same language without C-only constructs or narrowing conversions. */
#include "../ios16/giulia_wifi_pci.h"
#include "../ios16/giulia_wifi_link.h"
#include "../ios16/giulia_wifi_smmu.h"
#include "../ios16/giulia_wifi_mhi.h"
#include "../ios16/giulia_wifi_ce.h"
#include "../ios16/giulia_wifi_apple.h"
#include "../ios16/giulia_wifi_firmware.h"
#include "../ios16/giulia_wifi_qmp.h"
#include "../ios16/giulia_wifi_pmu.h"
#include "../ios16/giulia_display.h"
static_assert(sizeof(gwi_request) >= 20,"request layout");
