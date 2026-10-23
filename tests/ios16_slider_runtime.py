"""Compile actual runtime with host kernel stubs; verify opt-in and mailbox gates."""
from pathlib import Path as _ModulePath
import sys as _module_sys
_module_sys.path.insert(0, str(_ModulePath(__file__).resolve().parents[1] / "scripts"))
from ios16_sources import include_flags
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'ios16/slider/giulia_slider.c').read_text()
source = re.sub(r'^#include <[^>]+>\n', '', source, flags=re.M)
source = source.replace('__asm__ volatile("dsb sy" ::: "memory");', '')
stubs = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define ARM64_BOARD_CONFIG_SWEET 1
typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
typedef unsigned thread_t;
typedef int wait_result_t;
typedef int kern_return_t;
typedef void *event_t;
enum {KERN_SUCCESS=0, NSEC_PER_MSEC=1000000, THREAD_UNINT=0};
#define THREAD_CONTINUE_NULL 0
static unsigned maps,starts;
static int optin,handoff;
vm_offset_t ml_io_map(vm_offset_t a,vm_size_t b) {(void)a;(void)b;maps++;return 0;}
int giulia_slider_ring(unsigned enabled) {(void)enabled;return 0;}
static void delay(unsigned us) {(void)us;}
static void clock_interval_to_deadline(unsigned a,unsigned b,uint64_t *v) {(void)a;(void)b;*v=0;}
static void assert_wait_deadline(event_t e,int flags,uint64_t deadline) {(void)e;(void)flags;(void)deadline;}
static void thread_block(int c) {(void)c;}
static unsigned current_thread(void) {return 0;}
static void thread_terminate(unsigned t) {(void)t;}
static uint64_t mach_absolute_time(void) {return 0;}
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *n) {*n=t;}
static int PE_parse_boot_argn(const char *n,void *p,size_t size) {
 assert(size==sizeof(int));*(int *)p=!strcmp(n,"gh")?handoff:optin;return 1;
}
static int kernel_thread_start(void (*f)(void *,wait_result_t),void *p,thread_t *t) {
 (void)f;(void)p;*t=1;starts++;return 0;
}
static void thread_deallocate(thread_t t) {(void)t;}
static unsigned close_on_publish;
static int host_cas(unsigned *p,unsigned *expected,unsigned value,int weak,int success,int failure) {
 if(close_on_publish && *expected==255) {
  close_on_publish=0;__atomic_exchange_n(p,254u,__ATOMIC_ACQ_REL);
 }
 return __atomic_compare_exchange_n(p,expected,value,weak,success,failure);
}
#define __atomic_compare_exchange_n host_cas
'''
checks = r'''
int main(void) {
 giulia_slider_start();assert(!maps && !starts && !GSL_GET(enabled));
 gsl.started=0;optin=1;handoff=1;giulia_slider_start();
 assert(!maps && !starts && GSL_GET(state)==4);
 gsl.started=0;handoff=3;giulia_slider_start();assert(!maps && starts==1);
 assert(giulia_slider_terminal("slider.status") && !maps);
 assert(giulia_slider_terminal("slider.calibrate up 100") && !GSL_GET(mailbox));
 GSL_SET(state,2);
 assert(giulia_slider_terminal("slider.calibrate up 100") && GSL_GET(mailbox)==1 && GSL_GET(argument)==100);
 assert(giulia_slider_terminal("slider.stop") && GSL_GET(mailbox)==1);
 GSL_SET(mailbox,0);
 assert(giulia_slider_terminal("slider.hid 1") && GSL_GET(mailbox)==4);
 GSL_SET(mailbox,0);
 assert(giulia_slider_terminal("slider.calibrate up 32768") && !GSL_GET(mailbox));
 assert(giulia_slider_terminal("slider.calibrate middle 0") && !GSL_GET(mailbox));
 assert(giulia_slider_terminal("slider.hid 2") && !GSL_GET(mailbox));
 assert(!giulia_slider_terminal("keys.status") && !maps);
 close_on_publish=1;
 assert(giulia_slider_terminal("slider.calibrate down 100") && GSL_GET(mailbox)==254);
 assert(giulia_slider_terminal("slider.stop") && GSL_GET(mailbox)==254);
 puts("Slider runtime: actual C compiled; default-off/handoff/mailbox/syntax/status gates pass without MMIO");
 return 0;
}
'''
with tempfile.TemporaryDirectory() as folder:
    test = Path(folder)/'runtime.c'
    binary = Path(folder)/'runtime'
    test.write_text(stubs+source+checks)
    subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                    *include_flags(root),str(test),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
