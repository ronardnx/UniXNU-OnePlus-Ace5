#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include "../ios16/giulia_display.h"
using UInt32 = uint32_t;
static uint64_t mach_absolute_time() { return 0; }
static bool failEvent;
struct IOLock { bool locked = false; };
static void IOLockLock(IOLock *l) { assert(!l->locked); l->locked=true; }
static void IOLockUnlock(IOLock *l) { assert(l->locked); l->locked=false; }
struct IOHIDEvent {
    unsigned page, usage, down;
    static IOHIDEvent *keyboardEvent(uint64_t, unsigned page, unsigned usage, unsigned down) {
        if (failEvent) return nullptr;
        static IOHIDEvent event; event = {page, usage, down}; return &event;
    }
    void release() {}
};
struct IOService {};
struct IOHIDEventService {
    IOHIDEvent last = {}; unsigned count = 0;
    bool start(IOService *) { return true; }
    void setProperty(const char *, bool) {}
    void retain() {}
    void registerService() {}
    void dispatchEvent(IOHIDEvent *event) { last = *event; count++; }
};
#define OSDeclareDefaultStructors(type)
#define OSDefineMetaClassAndStructors(type, parent)
#define APPLE_KEXT_OVERRIDE
#define _Atomic
#define os_atomic_load(pointer, order) (*(pointer))
#define os_atomic_store(pointer, value, order) (*(pointer) = (value))
enum { kHIDPage_GenericDesktop=1, kHIDUsage_GD_Keyboard=6, kHIDPage_KeyboardOrKeypad=7 };
static const char *kIOHIDBuiltInKey="BuiltIn";
static const bool kOSBooleanFalse=false;
#include "../ios16/giulia_input_keyboard.inc"
struct SweetHIDButtons {
    IOLock lock; IOLock *fButtonLock = &lock;
    UInt32 fPhysicalButtons = 0, fRemoteButtons = 0;
    IOHIDEvent last = {}; unsigned count = 0;
    bool remoteKey(UInt32, UInt32, bool);
    bool sourceKey(UInt32, bool, bool);
    void dispatchEvent(IOHIDEvent *event) { last = *event; count++; }
};
struct SweetHIDTouch {
    int finger = -1, down = -1, x = -1, y = -1; unsigned count = 0;
    void touch(int f, bool d, int px, int py) { finger=f; down=d; x=px; y=py; count++; }
};
static SweetHIDButtons *gSweetButtons;
static SweetHIDTouch *gSweetTouch;
#include "../ios16/giulia_input_hid.inc"
int main()
{
    assert(giulia_remote_ready()==0 && !giulia_remote_touch(1,0,0) && !giulia_remote_hid(7,4,1));
    SweetHIDButtons buttons; SweetHIDTouch touch; GiuliaHIDKeyboard keyboard;
    gSweetButtons=&buttons; assert(giulia_remote_ready()==4);
    assert(keyboard.start(nullptr)); assert(giulia_remote_ready()==6);
    gSweetTouch=&touch; assert(giulia_remote_ready()==7);
    assert(giulia_remote_touch(1,65535,65535));
    assert(touch.finger==10 && touch.down==1 && touch.x==1263 && touch.y==2779);
    assert(giulia_remote_touch(0,0,0) && touch.down==0 && touch.x==0 && touch.y==0);
    assert(!giulia_remote_touch(2,1,1) && !giulia_remote_touch(1,65536,1));
    assert(touch.count==2);
    assert(giulia_remote_hid(7,4,1) && keyboard.last.page==7 && keyboard.last.usage==4 && keyboard.last.down);
    assert(giulia_remote_hid(7,231,0) && keyboard.last.usage==231 && !keyboard.last.down);
    assert(giulia_remote_hid(12,0x40,1) && buttons.last.page==12 && buttons.last.usage==0x40);
    assert(!giulia_remote_hid(7,3,1) && !giulia_remote_hid(7,232,1) && !giulia_remote_hid(8,4,1));
    assert(!giulia_remote_hid(12,0xff,1) && !giulia_remote_hid(7,4,2));
    assert(buttons.count==1 && keyboard.count==2);
    assert(giulia_physical_button(0xe9,1) && buttons.count==2 && buttons.last.down);
    assert(giulia_remote_hid(12,0xe9,1) && buttons.count==2);
    assert(giulia_remote_hid(12,0xe9,0) && buttons.count==2); // physical hold survives USB release
    assert(giulia_physical_button(0xe9,0) && buttons.count==3 && !buttons.last.down);
    assert(giulia_remote_hid(12,0x30,1) && buttons.count==4);
    assert(giulia_physical_button(0x30,1) && buttons.count==4);
    assert(giulia_physical_button(0x30,0) && buttons.count==4);
    failEvent=true;
    assert(!giulia_remote_hid(12,0x30,0) && buttons.fRemoteButtons&1);
    failEvent=false;
    assert(giulia_remote_hid(12,0x30,0) && buttons.count==5 && !buttons.last.down);
    assert(!giulia_physical_button(0x40,1) && !giulia_physical_button(0xea,2));
    puts("Remote HID provider: service readiness, native endpoint transform, isolated contact slot and page/usage validation passed");
}
