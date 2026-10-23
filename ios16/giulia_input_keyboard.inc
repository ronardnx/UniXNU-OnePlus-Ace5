/* Advertise a real keyboard usage pair independently of consumer buttons so
 * the iOS HID event system can discover and route ordinary keyboard events.
 */
class GiuliaHIDKeyboard : public IOHIDEventService {
    OSDeclareDefaultStructors(GiuliaHIDKeyboard);
public:
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    bool key(UInt32 usage, bool down);
protected:
    virtual UInt32 getPrimaryUsagePage() APPLE_KEXT_OVERRIDE { return kHIDPage_GenericDesktop; }
    virtual UInt32 getPrimaryUsage() APPLE_KEXT_OVERRIDE { return kHIDUsage_GD_Keyboard; }
    virtual UInt32 getProductID() APPLE_KEXT_OVERRIDE { return 3; }
};
OSDefineMetaClassAndStructors(GiuliaHIDKeyboard, IOHIDEventService);
static GiuliaHIDKeyboard *_Atomic gGiuliaKeyboard;

bool GiuliaHIDKeyboard::start(IOService *provider)
{
    if (!IOHIDEventService::start(provider)) return false;
    setProperty(kIOHIDBuiltInKey, kOSBooleanFalse);
    this->retain();
    os_atomic_store(&gGiuliaKeyboard, this, release);
    registerService();
    return true;
}

bool GiuliaHIDKeyboard::key(UInt32 usage, bool down)
{
    IOHIDEvent *event = IOHIDEvent::keyboardEvent(mach_absolute_time(), kHIDPage_KeyboardOrKeypad, usage, down);
    if (!event) return false;
    dispatchEvent(event);
    event->release();
    return true;
}
