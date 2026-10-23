/* Included after SweetHID's service implementations. All published providers
 * have a permanent retain, matching the existing hardware touch worker.
 */
#include "../common/giulia_input_provider.h"
bool
SweetHIDButtons::remoteKey(UInt32 page, UInt32 usage, bool down)
{
    return page == 12 && sourceKey(usage, down, true);
}

bool
SweetHIDButtons::sourceKey(UInt32 usage, bool down, bool remote)
{
    unsigned bit = usage == 0x30 ? 1u : usage == 0xe9 ? 2u :
        usage == 0xea ? 4u : usage == 0x40 ? 8u : 0u;
    if (!bit || !fButtonLock) return false;
    IOLockLock(fButtonLock);
    UInt32 before = fPhysicalButtons | fRemoteButtons;
    UInt32 &source = remote ? fRemoteButtons : fPhysicalButtons;
    UInt32 next = down ? source | bit : source & ~bit;
    UInt32 after = next | (remote ? fPhysicalButtons : fRemoteButtons);
    if ((before ^ after) & bit) {
        IOHIDEvent *event = IOHIDEvent::keyboardEvent(mach_absolute_time(), 12, usage, !!(after & bit));
        if (!event) { IOLockUnlock(fButtonLock); return false; }
        source = next;
        dispatchEvent(event);
        event->release();
    } else source = next;
    IOLockUnlock(fButtonLock);
    return true;
}

extern "C" int giulia_physical_button(unsigned usage, unsigned down)
{
    if (down > 1 || (usage != 0x30 && usage != 0xe9 && usage != 0xea)) return 0;
    SweetHIDButtons *service = os_atomic_load(&gSweetButtons, acquire);
    return service && service->sourceKey(usage, down != 0, false);
}

bool SweetHIDButtons::sliderRing(bool enabled)
{
    if (!fButtonLock) return false;
    IOLockLock(fButtonLock);
    if (fSliderRing != (UInt32)enabled) {
        IOHIDEvent *event = IOHIDEvent::keyboardEvent(mach_absolute_time(), 0x0b, 0x2d, enabled);
        if (!event) { IOLockUnlock(fButtonLock); return false; }
        fSliderRing = enabled; dispatchEvent(event); event->release();
    }
    IOLockUnlock(fButtonLock); return true;
}
extern "C" int giulia_slider_ring(unsigned enabled)
{
    if (enabled > 1) return 0;
    SweetHIDButtons *service = os_atomic_load(&gSweetButtons, acquire);
    return service && service->sliderRing(enabled != 0);
}

extern "C" unsigned giulia_remote_ready(void)
{
    return (os_atomic_load(&gSweetTouch, acquire) ? 1u : 0u) |
        (os_atomic_load(&gGiuliaKeyboard, acquire) ? 2u : 0u) |
        (os_atomic_load(&gSweetButtons, acquire) ? 4u : 0u);
}

extern "C" int giulia_remote_touch(unsigned down, unsigned x, unsigned y)
{
    SweetHIDTouch *service = os_atomic_load(&gSweetTouch, acquire);
    if (!service || down > 1 || x > 65535 || y > 65535) return 0;
    /* Remote contact 10 never aliases hardware contacts 0..9. */
    service->touch(10, down != 0, (int)((uint64_t)x * (GD_WIDTH - 1) / 65535),
        (int)((uint64_t)y * (GD_HEIGHT - 1) / 65535));
    return 1;
}

extern "C" int giulia_remote_hid(unsigned page, unsigned usage, unsigned down)
{
    if (down > 1) return 0;
    if (page == 7 && usage >= 4 && usage <= 231) {
        GiuliaHIDKeyboard *service = os_atomic_load(&gGiuliaKeyboard, acquire);
        return service && service->key(usage, down != 0);
    }
    if (page == 12 && (usage == 0x30 || usage == 0x40 || usage == 0xe9 || usage == 0xea)) {
        SweetHIDButtons *service = os_atomic_load(&gSweetButtons, acquire);
        return service && service->remoteKey(page, usage, down != 0);
    }
    return 0;
}
