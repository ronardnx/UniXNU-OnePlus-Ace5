/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <pexpert/arm64/board_config.h>
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <mach/thread_act.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include "giulia_keys.h"
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
extern int giulia_physical_button(unsigned,unsigned);
extern unsigned giulia_remote_ready(void);
#include "giulia_keys_terminal.inc"
void giulia_keys_start(void);
static struct {
    vm_offset_t core,observer;
    struct gk_hw hw;
    struct gk_filter filter;
    unsigned started;
} gkeys;
static const unsigned gkeys_usage[3]={0x30,0xe9,0xea};
static void gkeys_publish(unsigned state,int error)
{
    GK_STATUS_SET(version,gkeys.hw.version);
    GK_STATUS_SET(pon_apid,gkeys.hw.pon.mapping?gkeys.hw.pon.apid:1024);
    GK_STATUS_SET(gpio_apid,gkeys.hw.gpio.mapping?gkeys.hw.gpio.apid:1024);
    GK_STATUS_SET(status,gkeys.hw.status);
    GK_STATUS_SET(stable,gkeys.filter.stable);
    GK_STATUS_SET(delivered,gkeys.filter.delivered);
    GK_STATUS_SET(error,error);
    GK_STATUS_SET(state,state);
}
static uint32_t gkeys_read(void *ctx,unsigned space,unsigned offset)
{
    (void)ctx;
    vm_offset_t base=space==GK_CORE?gkeys.core:gkeys.observer;
    uint32_t value=*(volatile uint32_t *)(base+offset);
    __asm__ volatile("dsb sy" ::: "memory");return value;
}
static void gkeys_write(void *ctx,unsigned space,unsigned offset,uint32_t value)
{
    (void)ctx;(void)space;
    *(volatile uint32_t *)(gkeys.observer+offset)=value;
    __asm__ volatile("dsb sy" ::: "memory");
}
static void gkeys_delay(void *ctx,unsigned us){(void)ctx;delay(us);}
static const struct gk_io gkeys_io={NULL,gkeys_read,gkeys_write,gkeys_delay};
static void gkeys_wait(void)
{
    uint64_t deadline;clock_interval_to_deadline(10,NSEC_PER_MSEC,&deadline);
    assert_wait_deadline((event_t)&gkeys,THREAD_UNINT,deadline);
    thread_block(THREAD_CONTINUE_NULL);
}
static void gkeys_worker(void *arg,wait_result_t result)
{
    (void)arg;(void)result;
    int error=GK_IO;
    gkeys.core=ml_io_map(0x0c400000,0x4000);
    if(!gkeys.core)goto done;
    error=gk_resolve(&gkeys_io,&gkeys.hw);if(error)goto done;
    /* EE0 v7 observer stride is 0x20, with at most 1024 channels. */
    gkeys.observer=ml_io_map(0x0c440000,0x8000);
    if(!gkeys.observer){error=GK_IO;goto done;}
    error=gk_identify(&gkeys_io,&gkeys.hw);if(error)goto done;
    printf("GIULIA KEYS: observer polling active version=%#x pon_apid=%u gpio_apid=%u debounce_ms=40\n",
        gkeys.hw.version,gkeys.hw.pon.apid,gkeys.hw.gpio.apid);
    gkeys_publish(2,GK_OK);
    for(;;) {
        unsigned pon,gpio;
        error=gk_read(&gkeys_io,&gkeys.hw,&gkeys.hw.pon,0x10,&pon);if(error)break;
        error=gk_read(&gkeys_io,&gkeys.hw,&gkeys.hw.gpio,0x10,&gpio);if(error)break;
        uint64_t ns;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
        unsigned changed=gk_filter_sample(&gkeys.filter,gk_decode(pon,gpio),ns/1000000);
        GK_STATUS_SET(pon,pon);GK_STATUS_SET(gpio,gpio);
        GK_STATUS_SET(raw,gk_decode(pon,gpio));GK_STATUS_INC(samples);
        for(unsigned k=0;k<3;k++)if(changed&(1u<<k)) {
            unsigned down=!!(gkeys.filter.stable&(1u<<k));
            if(giulia_physical_button(gkeys_usage[k],down)) {
                gkeys.filter.delivered^=1u<<k;
                GK_STATUS_INC(events);
                printf("GIULIA KEYS: usage=%#x down=%u pon=%#x gpio=%#x\n",gkeys_usage[k],down,pon,gpio);
            } else GK_STATUS_INC(retries);
        }
        gkeys_publish(2,GK_OK);
        gkeys_wait();
    }
done:
    if(error)GK_STATUS_INC(errors);
    gkeys_publish(4,error);
    /* Stop after failed/unknown completion; never reuse a timed-out observer.
     * Retry event allocation if needed so a delivered down cannot stick. */
    while(gkeys.filter.delivered) {
        for(unsigned k=0;k<3;k++)if(gkeys.filter.delivered&(1u<<k)) {
            if(giulia_physical_button(gkeys_usage[k],0)) {
                gkeys.filter.delivered&=~(1u<<k);GK_STATUS_INC(events);
            } else GK_STATUS_INC(retries);
        }
        gkeys_publish(4,error);
        if(gkeys.filter.delivered)gkeys_wait();
    }
    printf("GIULIA KEYS: stopped error=%d status=%#x; GUI continues\n",error,gkeys.hw.status);
    gkeys_publish(5,error);
    thread_terminate(current_thread());
}
void giulia_keys_start(void)
{
    int enabled=1;thread_t thread;
    if(gkeys.started)return;gkeys.started=1;
    PE_parse_boot_argn("giulia_keys",&enabled,sizeof(enabled));
    GK_STATUS_SET(enabled,enabled);
    if(enabled!=1){GK_STATUS_SET(state,3);printf("GIULIA KEYS: disabled\n");return;}
    GK_STATUS_SET(state,1);
    kern_return_t kr=kernel_thread_start(gkeys_worker,NULL,&thread);
    if(kr==KERN_SUCCESS)thread_deallocate(thread);
    else {
        GK_STATUS_SET(thread_error,(int)kr);GK_STATUS_INC(errors);GK_STATUS_SET(state,6);
        printf("GIULIA KEYS: thread creation failed %d\n",kr);
    }
}
#endif
