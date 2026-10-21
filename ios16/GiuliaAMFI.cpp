/* iOS16's read-only AMFI user-client trust-cache membership query.
 * Source caches are extracted from the hash-pinned Apple IPSW by the host.
 * This does not implement Apple's Image4 runtime or claim CMS-chain validation.
 * Existing XNU code-directory/page-hash checks remain unchanged.
 */
#include <pexpert/arm64/board_config.h>
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSData.h>
#include <arm/machine_routines.h>
#include <sys/proc.h>
#include "giulia_trust_catalog.h"

extern "C" { extern unsigned long gPhysBase, gPhysSize; }

class AppleMobileFileIntegrity : public IOService {
    OSDeclareDefaultStructors(AppleMobileFileIntegrity);
    void *catalog;
    size_t catalogSize;
    struct gtc_view view;
public:
    bool init(OSDictionary *dictionary = NULL) APPLE_KEXT_OVERRIDE;
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void free(void) APPLE_KEXT_OVERRIDE;
    bool contains(const void *hash, size_t length) const;
};
OSDefineMetaClassAndStructors(AppleMobileFileIntegrity, IOService);

bool AppleMobileFileIntegrity::init(OSDictionary *dictionary) {
    catalog = NULL; catalogSize = 0; bzero(&view, sizeof(view));
    return IOService::init(dictionary);
}
bool AppleMobileFileIntegrity::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    IORegistryEntry *map = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
    if (!map) return false;
    OSData *range = OSDynamicCast(OSData, map->getProperty("TrustCache"));
    uint64_t pair[2] = {0, 0};
    if (range && range->getLength() == sizeof(pair))
        memcpy(pair, range->getBytesNoCopy(), sizeof(pair));
    map->release();
    uint64_t address = pair[0], bytes = pair[1];
    if (bytes < 8 || bytes > GTC_MAX_BYTES || address < gPhysBase ||
        gPhysSize < bytes || address - gPhysBase > gPhysSize - bytes) {
        IOLog("GIULIA AMFI: no bounded trust catalogue; query service not published\n");
        return false;
    }
    catalog = IOMallocData((size_t)bytes);
    if (!catalog) return false;
    catalogSize = (size_t)bytes;
    memcpy(catalog, (const void *)ml_static_ptovirt((vm_offset_t)address), catalogSize);
    if (!gtc_validate(catalog, catalogSize, &view) || !view.entries) {
        IOLog("GIULIA AMFI: malformed or empty trust catalogue; refused\n");
        return false;
    }
    setProperty(kIOUserClientClassKey, "GiuliaAMFIUserClient");
    setProperty("GiuliaReadOnlyTrustEntries", view.entries, 32);
    registerService();
    IOLog("GIULIA AMFI: trust catalogue ready; modules=%u entries=%u; read-only selector 6\n",
          view.modules, view.entries);
    return true;
}
void AppleMobileFileIntegrity::free(void) {
    if (catalog) IOFreeData(catalog, catalogSize);
    catalog = NULL; bzero(&view, sizeof(view));
    IOService::free();
}
bool AppleMobileFileIntegrity::contains(const void *hash, size_t length) const {
    return gtc_contains(&view, hash, length) != 0;
}

class GiuliaAMFIUserClient : public IOUserClient {
    OSDeclareDefaultStructors(GiuliaAMFIUserClient);
public:
    bool initWithTask(task_t task, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
    IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
        IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(GiuliaAMFIUserClient, IOUserClient);
bool GiuliaAMFIUserClient::initWithTask(task_t task, void *securityID, UInt32 type) {
    return type == 0 && IOUserClient::initWithTask(task, securityID, type);
}
IOReturn GiuliaAMFIUserClient::clientClose(void) {
    terminate(); return kIOReturnSuccess;
}
IOReturn GiuliaAMFIUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) {
    (void)dispatch; (void)target; (void)reference;
    if (selector != 6) return kIOReturnUnsupported;
    /* Verified in both libmis TrustCacheOnly and launchd_cache_loader:
     * scalar[0]=1; one 20-byte cdhash; no output; success means exact membership.
     */
    if (!args || args->scalarInputCount != 1 || !args->scalarInput || args->scalarInput[0] != 1 ||
        args->scalarOutputCount || args->structureOutputSize || args->structureOutputDescriptor)
        return kIOReturnBadArgument;
    uint8_t hash[20];
    if (args->structureInputDescriptor) {
        if (args->structureInputDescriptor->getLength() != sizeof(hash) ||
            args->structureInputDescriptor->readBytes(0, hash, sizeof(hash)) != sizeof(hash))
            return kIOReturnBadArgument;
    } else {
        if (!args->structureInput || args->structureInputSize != sizeof(hash)) return kIOReturnBadArgument;
        memcpy(hash, args->structureInput, sizeof(hash));
    }
    AppleMobileFileIntegrity *provider = OSDynamicCast(AppleMobileFileIntegrity, getProvider());
    if (!provider) return kIOReturnNotReady;
    bool found = provider->contains(hash, sizeof(hash));
    static uint32_t logged;
    if (__atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < 96) {
        char name[32]; proc_selfname(name, sizeof(name));
        IOLog("GIULIA AMFI: %s selector 6 catalogue match=%u\n", name, found ? 1 : 0);
    }
    return found ? kIOReturnSuccess : kIOReturnNotFound;
}
#endif
