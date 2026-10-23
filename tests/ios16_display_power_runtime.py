"""Compile the actual AppleCLCD callback with lock/IOKit stubs."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
source=(root/'ios16/giulia_display_power.inc').read_text()
callback=source.replace('__asm__ volatile("dsb sy" ::: "memory");','')
stubs=r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include "giulia_display_power.h"
using vm_offset_t=uintptr_t;
using vm_size_t=size_t;
using IOReturn=int;
enum {kIOReturnSuccess=0,kIOReturnNotReady=1,kIOReturnBusy=2,kIOReturnIOError=3};
struct IOLock { bool held=false;unsigned entries=0; };
static void IOLockLock(IOLock *l){assert(l && !l->held);l->held=true;l->entries++;}
static void IOLockUnlock(IOLock *l){assert(l && l->held);l->held=false;}
static unsigned map_count,commands,last_on;
static int map_available=1,native_error,native_known=1;
extern "C" vm_offset_t ml_io_map(vm_offset_t address,vm_size_t bytes) {
 assert(address==0xae94000 && bytes==0x4000);map_count++;return map_available?0x1000:0;
}
static void IODelay(unsigned) {}
static IOLock power_lock,capture_lock,frame_lock;
static int native_power(const struct gdp_io *,struct gdp_state *s,unsigned on) {
 assert(power_lock.held && capture_lock.held && !frame_lock.held);
 commands++;last_on=on;s->known=native_known;s->on=on;s->error=native_error;
 return native_error;
}
#define gdp_power native_power
#define IOLog std::printf
struct AppleCLCD {
 IOLock *fDisplayPowerLock=&power_lock,*fCaptureLock=&capture_lock,*fLock=&frame_lock;
 vm_offset_t fDisplayRegisters=0;gdp_state fDisplayPower={};
 bool fDisplayPowerOff=false,fDirty=false;
 void setProperty(const char *,uint64_t,unsigned=0) {}
 IOReturn setDisplayPower(uint64_t requested);
};
'''
checks=r'''
int main() {
 AppleCLCD fb;
 assert(fb.setDisplayPower(0)==kIOReturnSuccess && fb.fDisplayPowerOff && !fb.fDirty);
 assert(commands==1 && !last_on && map_count==1);
 assert(fb.setDisplayPower(2)==kIOReturnSuccess && !fb.fDisplayPowerOff && fb.fDirty);
 assert(commands==2 && last_on==1 && map_count==1);
 native_error=GDP_BUSY;native_known=0;
 assert(fb.setDisplayPower(0)==kIOReturnBusy && !fb.fDisplayPowerOff);
 native_error=GDP_ACK;
 assert(fb.setDisplayPower(1)==kIOReturnIOError);
 assert(!power_lock.held && !capture_lock.held && !frame_lock.held);
 fb.fDisplayRegisters=0;map_available=0;unsigned count=commands;
 assert(fb.setDisplayPower(0)==kIOReturnIOError && commands==count);
 fb.fDisplayPowerLock=nullptr;
 assert(fb.setDisplayPower(0)==kIOReturnNotReady && commands==count);
 assert(!power_lock.held && !capture_lock.held && !frame_lock.held);
 puts("AppleCLCD power callback: actual C++ passes 0-off/nonzero-on, serialization, redraw, map failure and truthful error tests");
}
'''
with tempfile.TemporaryDirectory() as folder:
    test=Path(folder)/'callback.cpp'
    binary=Path(folder)/'callback'
    test.write_text(stubs+callback+checks)
    subprocess.run(['clang++','-std=c++14','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                    '-I'+str(root/'ios16'),str(test),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
