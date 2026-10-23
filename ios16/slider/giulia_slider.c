/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Explicit gh=3/giulia_slider=1 diagnostic worker; no firmware/PMIC guesses.
 * Calibration and HID are separate operator requests after sensor identity.
 */
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
#include "giulia_slider.h"
#include "giulia_slider_pins.h"
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
#include "../common/giulia_input_provider.h"
void giulia_slider_start(void);
int giulia_slider_terminal(const char *);
static struct {
    vm_offset_t gcc,se,pin[2];struct gsl_pins pins;struct gsl_sensor sensor;
    struct gsl_calibration calibration;struct gsl_filter filter;
    unsigned started,enabled,handoff,state,error,cleanup_error,samples;
    unsigned raw,stable,calibrated,hid,ring,events,retries,mailbox,argument;
    int x,y,z;
} gsl;
#define GSL_GET(field) __atomic_load_n(&gsl.field,__ATOMIC_ACQUIRE)
#define GSL_SET(field,value) __atomic_store_n(&gsl.field,(value),__ATOMIC_RELEASE)
#define GSL_INC(field) ((void)__atomic_fetch_add(&gsl.field,1u,__ATOMIC_RELAXED))
static uint32_t gsl_read32(vm_offset_t base,unsigned offset)
{uint32_t v=*(volatile uint32_t *)(base+offset);__asm__ volatile("dsb sy" ::: "memory");return v;}
static void gsl_write32(vm_offset_t base,unsigned offset,uint32_t value)
{*(volatile uint32_t *)(base+offset)=value;__asm__ volatile("dsb sy" ::: "memory");}
static uint32_t gsl_pin_read(void *ctx,unsigned pin,unsigned offset)
{(void)ctx;return gsl_read32(gsl.pin[pin],offset);}
static void gsl_pin_write(void *ctx,unsigned pin,unsigned offset,uint32_t value)
{(void)ctx;gsl_write32(gsl.pin[pin],offset,value);}
static const struct gsl_pins_io gsl_pin_io={0,gsl_pin_read,gsl_pin_write};
static void gsl_low(void *ctx,unsigned pin,unsigned low)
{(void)ctx;gsl_pins_low(&gsl_pin_io,&gsl.pins,pin,low);}
static int gsl_level(void *ctx,unsigned pin)
{(void)ctx;return gsl_pins_level(&gsl_pin_io,&gsl.pins,pin);}
static void gsl_delay(void *ctx,unsigned us){(void)ctx;delay(us);}
static const struct gsl_bus gsl_bus_io={0,gsl_low,gsl_level,gsl_delay};
static int gsl_register_read(void *ctx,unsigned reg,uint8_t *data,unsigned length)
{(void)ctx;return gsl_transfer(&gsl_bus_io,reg,data,length,1);}
static int gsl_register_write(void *ctx,unsigned reg,const uint8_t *data,unsigned length)
{
    (void)ctx;
    if(length>16)return GSL_ARGUMENT;
    uint8_t buffer[16];memcpy(buffer,data,length);
    return gsl_transfer(&gsl_bus_io,reg,buffer,length,0);
}
static const struct gsl_sensor_io gsl_sensor_io={0,gsl_register_read,gsl_register_write,gsl_delay};
static void gsl_wait(void)
{
    uint64_t deadline;clock_interval_to_deadline(20,NSEC_PER_MSEC,&deadline);
    assert_wait_deadline((event_t)&gsl,THREAD_UNINT,deadline);thread_block(THREAD_CONTINUE_NULL);
}
static int gsl_preflight(void)
{
    gsl.gcc=ml_io_map(0x00100000,0x54000);if(!gsl.gcc)return 20;
    const unsigned clocks[]={0x23000,0x23004,0x170e4};
    for(unsigned i=0;i<3;i++) {
        uint32_t value=gsl_read32(gsl.gcc,clocks[i]);
        if(value==0xffffffff || (value&0x80000000u))return 21;
    }
    gsl.se=ml_io_map(0x009a0000,0x4000);if(!gsl.se)return 22;
    uint32_t status=gsl_read32(gsl.se,0x40),dma=gsl_read32(gsl.se,0xe40);
    if(status==0xffffffff || dma==0xffffffff || (status&0x1001) || (dma&3) ||
       gsl_read32(gsl.se,0xc3c) || gsl_read32(gsl.se,0xd3c))return 23;
    /* No other native driver is registered for this controller. Ownership
     * handoff is explicit; still refuse an active controller/DMA above. */
    for(unsigned i=0;i<2;i++) {
        gsl.pin[i]=ml_io_map(0x0f100000+(206+i)*0x1000,0x1000);
        if(!gsl.pin[i])return 24;
    }
    if(gsl_pins_claim(&gsl_pin_io,&gsl.pins))return 25;
    return 0;
}
static void gsl_status(void)
{
    printf("GIULIA SLIDER: compiled=1 enabled=%u gh=%u state=%u error=%u cleanup_error=%u samples=%u cache=approximate\n",
        GSL_GET(enabled),GSL_GET(handoff),GSL_GET(state),GSL_GET(error),GSL_GET(cleanup_error),GSL_GET(samples));
    printf("GIULIA SLIDER: x=%d y=%d z=%d raw=%u stable=%u calibrated_mask=%#x hid_optin=%u ring=%u events=%u retries=%u haptics=0\n",
        GSL_GET(x),GSL_GET(y),GSL_GET(z),GSL_GET(raw),GSL_GET(stable),GSL_GET(calibrated),
        GSL_GET(hid),GSL_GET(ring),GSL_GET(events),GSL_GET(retries));
    printf("GIULIA SLIDER: mailbox=%u reserved=255 closed=254 states=0:disabled,1:starting,2:polling,3:stopped,4:handoff-missing,5:stopping positions=0:unknown,1:up,2:middle,3:down ring_policy=down experimental_iOS_polarity=unverified\n",GSL_GET(mailbox));
}
static int gsl_mailbox(unsigned command,unsigned argument)
{
    if(GSL_GET(state)!=2){printf("GIULIA SLIDER: request rejected=worker-unavailable\n");return 1;}
    unsigned expected=0;
    if(!__atomic_compare_exchange_n(&gsl.mailbox,&expected,255u,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        printf("GIULIA SLIDER: request rejected=busy\n");return 1;
    }
    GSL_SET(argument,argument);expected=255;
    if(!__atomic_compare_exchange_n(&gsl.mailbox,&expected,command,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED)) {
        printf("GIULIA SLIDER: request rejected=worker-closed\n");return 1;
    }
    printf("GIULIA SLIDER: request queued=%u argument=%u\n",command,argument);return 1;
}
int giulia_slider_terminal(const char *command)
{
    if(!strcmp(command,"slider.status")){gsl_status();return 1;}
    if(!strncmp(command,"slider.calibrate ",17)) {
        const char *p=command+17;unsigned position=0,radius=0;
        if(!strncmp(p,"up ",3)){position=1;p+=3;}
        else if(!strncmp(p,"middle ",7)){position=2;p+=7;}
        else if(!strncmp(p,"down ",5)){position=3;p+=5;}
        else goto invalid;
        if(*p<'0'||*p>'9')goto invalid;
        while(*p>='0'&&*p<='9') {
            if(radius>3276)goto invalid;radius=radius*10+(unsigned)(*p++-'0');
        }
        if(*p || !radius || radius>32767)goto invalid;
        return gsl_mailbox(position,radius);
    }
    if(!strcmp(command,"slider.hid 0"))return gsl_mailbox(4,0);
    if(!strcmp(command,"slider.hid 1"))return gsl_mailbox(4,1);
    if(!strcmp(command,"slider.stop"))return gsl_mailbox(5,0);
    if(strncmp(command,"slider.",7))return 0;
invalid:
    printf("GIULIA SLIDER: rejected=invalid-command\n");return 1;
}
static void gsl_worker(void *arg,wait_result_t result)
{
    (void)arg;(void)result;
    int error=gsl_preflight();
    if(!error)error=gsl_sensor_begin(&gsl_sensor_io,&gsl.sensor);
    if(error)goto done;
    GSL_SET(state,2);
    unsigned not_ready=0;
    for(;;) {
        if(GSL_GET(mailbox)==5)break;
        struct gsl_vector vector;
        error=gsl_sensor_sample(&gsl_sensor_io,&gsl.sensor,&vector);
        if(error==GSL_NOT_READY){if(++not_ready>=50)break;gsl_wait();continue;}
        if(error)break;
        not_ready=0;
        GSL_SET(x,vector.x);GSL_SET(y,vector.y);GSL_SET(z,vector.z);GSL_INC(samples);
        unsigned command=GSL_GET(mailbox);
        if(command && command!=255) {
            if(command==5)break;
            if(command==4) {
                unsigned enabled=GSL_GET(argument);
                if(enabled && !gsl_calibration_valid(&gsl.calibration))
                    printf("GIULIA SLIDER: hid rejected=calibration-required\n");
                else {GSL_SET(hid,enabled);printf("GIULIA SLIDER: hid=%u experimental=1\n",enabled);}
            } else {
                unsigned radius=GSL_GET(argument),index=command-1;
                /* Capture an operator-labelled point only when HID is off.
                 * No magnetic-sign or boot-position fallback is permitted. */
                if(GSL_GET(hid))printf("GIULIA SLIDER: calibration rejected=disable-hid-first\n");
                else {
                    gsl.calibration.centre[index]=vector;gsl.calibration.radius[index]=radius;
                    gsl.calibration.valid|=1u<<index;GSL_SET(calibrated,gsl.calibration.valid);
                    memset(&gsl.filter,0,sizeof(gsl.filter));
                    printf("GIULIA SLIDER: calibration position=%u x=%d y=%d z=%d radius=%u all_valid=%u\n",
                        command,vector.x,vector.y,vector.z,radius,gsl_calibration_valid(&gsl.calibration));
                }
            }
            GSL_SET(mailbox,0);
        }
        unsigned raw=gsl_position(&gsl.calibration,&vector);
        uint64_t ns;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
        unsigned stable=gsl_filter_sample(&gsl.filter,raw,ns/1000000);
        GSL_SET(raw,raw);GSL_SET(stable,stable);
        if(GSL_GET(hid) && stable && GSL_GET(ring)!=(stable==3)) {
            if(giulia_slider_ring(stable==3)){GSL_SET(ring,stable==3);GSL_INC(events);}
            else GSL_INC(retries);
        }
        gsl_wait();
    }
done:
    /* CLOSED wins against an in-flight publisher: RESERVED->command is CAS. */
    GSL_SET(state,5);(void)__atomic_exchange_n(&gsl.mailbox,254u,__ATOMIC_ACQ_REL);
    GSL_SET(hid,0);GSL_SET(error,(unsigned)error);
    if(gsl.sensor.changed) {
        int restore=gsl_sensor_restore(&gsl_sensor_io,&gsl.sensor);
        if(restore)GSL_SET(cleanup_error,(unsigned)restore);
    }
    if(gsl.pins.claimed && gsl_pins_restore(&gsl_pin_io,&gsl.pins))GSL_SET(cleanup_error,26);
    GSL_SET(state,3);gsl_status();thread_terminate(current_thread());
}
void giulia_slider_start(void)
{
    if(gsl.started)return;gsl.started=1;
    int enabled=0,handoff=0;thread_t thread;
    PE_parse_boot_argn("giulia_slider",&enabled,sizeof(enabled));PE_parse_boot_argn("gh",&handoff,sizeof(handoff));
    GSL_SET(enabled,enabled==1);GSL_SET(handoff,(unsigned)handoff);GSL_SET(ring,2);
    if(enabled!=1)return;
    if(handoff!=3){GSL_SET(state,4);printf("GIULIA SLIDER: handoff-missing gh=%d no-hardware-access\n",handoff);return;}
    GSL_SET(state,1);kern_return_t kr=kernel_thread_start(gsl_worker,0,&thread);
    if(kr==KERN_SUCCESS)thread_deallocate(thread);
    else {GSL_SET(error,(unsigned)kr);GSL_SET(state,3);}
}
#endif
