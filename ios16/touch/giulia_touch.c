/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Ace 5 / SM8650 Synaptics S3910 bring-up. Register and protocol provenance
 * is recorded in hardware/sm8650-touch.json. Uses GENI FIFO, never GPI DMA.
 * Firmware, regulator and GPIO reset ownership are inherited and diagnosed;
 * unsupported handoff states stop this worker without stopping GUI boot.
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
#include "giulia_touch_report.h"
#include "giulia_touch_wire.h"
#include "giulia_touch_gpio.h"

extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
#include "../common/giulia_input_provider.h"
void giulia_touch_start(void);

#define GT_SE 0x00a90000u
#define GT_GCC 0x00100000u
#define GT_TLMM 0x0f100000u
#define GT_SPI_ERRORS 0x33003e0eu
static struct {
    vm_offset_t se,gcc,pin[7];
    unsigned depth,rxdepth,last_irq;
    bool started,dead,gpio_mode,gpio_changed;
    uint32_t old_cfg[7],old_io[7];
    struct gt_state state;
} gt;
static int gt_wait_chan;
static const unsigned gt_pins[7]={48,49,50,51,108,161,162};

static uint32_t gr(vm_offset_t base,unsigned off)
{
    uint32_t v=*(volatile uint32_t *)(base+off);
    __asm__ volatile("dmb oshld" ::: "memory");return v;
}
static void gw(vm_offset_t base,unsigned off,uint32_t v)
{
    *(volatile uint32_t *)(base+off)=v;
    __asm__ volatile("dsb osh" ::: "memory");
}
static void gt_wait(unsigned ms)
{
    uint64_t deadline;
    clock_interval_to_deadline(ms,NSEC_PER_MSEC,&deadline);
    (void)assert_wait_deadline(&gt_wait_chan,THREAD_UNINT,deadline);
    thread_block(THREAD_CONTINUE_NULL);
}
static void gt_abort(void)
{
    gw(gt.se,0x604,2);
    for(unsigned i=0;i<1000;i++) {
        if(!(gr(gt.se,0x40)&1)){gw(gt.se,0x618,0xffffffffu);return;}
        delay(1);
    }
    gt.dead=true;
}
static void gt_gpio_set(unsigned pin,unsigned value)
{
    uint32_t io=gr(gt.pin[pin],4);
    gw(gt.pin[pin],4,(io&~2u)|(value?2u:0));
}
static unsigned gt_gpio_sample(void){return gr(gt.pin[0],4)&1;}
static void gt_gpio_pause(void){delay(1);}
static const struct gg_bus gt_gpio_bus={gt_gpio_set,gt_gpio_sample,gt_gpio_pause};

static void gt_gpio_restore(void)
{
    if(!gt.gpio_changed)return;
    gt_gpio_set(3,1);gt_gpio_set(2,0);
    for(unsigned i=0;i<7;i++) {
        gw(gt.pin[i],4,gt.old_io[i]);gw(gt.pin[i],0,gt.old_cfg[i]);
    }
    gt.gpio_changed=false;
}
static int gt_gpio_prepare(void)
{
    /* DT allocates dynamic GPI instances 2/3 to this AP. Inspect only those
     * instances and never issue a GPI command or modify its SMMU stream. */
    vm_offset_t gpi=ml_io_map(0xa00000,0x60000);
    if(!gpi)return 20;
    for(unsigned n=2;n<=3;n++)for(unsigned ch=0;ch<2;ch++) {
        unsigned off=0x10000+0x4000*n+0x80*ch;
        uint32_t cfg=gr(gpi,off),scratch=gr(gpi,off+0x60);
        unsigned state=(cfg>>20)&15;
        printf("GIULIA TOUCH: GPI%u ch%u cfg=%#x scratch=%#x\n",n,ch,cfg,scratch);
        if(cfg==0xffffffffu || scratch==0xffffffffu)return 24;
        if((scratch&15)==4 && state!=0 && state!=1 && state!=3)return 21;
    }
    for(unsigned i=0;i<7;i++) {
        gt.old_cfg[i]=gr(gt.pin[i],0);gt.old_io[i]=gr(gt.pin[i],4);
        if(!gg_pin_owned(gt.old_cfg[i]))return 22;
    }
    gt.gpio_changed=true;
    /* Preload deasserted CS, low clock and released reset before GPIO OE.
     * AVDD enable and reset delays follow this board's Synaptics DT. */
    for(unsigned i=0;i<7;i++) {
        bool output=i!=0 && i!=6;
        gt_gpio_set(i,i==2?0:1);
        uint32_t cfg=(gt.old_cfg[i]&~0x3ffu)|(2u<<6)|(output?1u<<9:0);
        gw(gt.pin[i],0,cfg);
        if((gr(gt.pin[i],0)&0x3ff)!=(cfg&0x3ff)){gt_gpio_restore();return 23;}
    }
    gt_wait(200);
    gt_gpio_set(5,0);gt_wait(10);gt_gpio_set(5,1);gt_wait(80);
    gt.gpio_mode=true;
    printf("GIULIA TOUCH: GPIO SPI mode 0 active; S3910 volatile reset complete\n");
    return 0;
}
static int gt_xfer(const uint8_t *tx,uint8_t *rx,unsigned n)
{
    if(gt.dead || !n || n>GW_MAX+7 || (!tx && !rx))return 1;
    if(gt.gpio_mode)return gg_transfer(&gt_gpio_bus,tx,rx,n);
    if((gr(gt.se,0x40)&0x1001) || (gr(gt.se,0x800)&0xfffffff) ||
       (gr(gt.se,0x804)&0x1ffffff))return 2;
    unsigned sent=0,got=0;
    gw(gt.se,0x618,0xffffffffu);
    gw(gt.se,0x26c,n);gw(gt.se,0x270,rx?n:0);
    gw(gt.se,0x600,(rx?3u:1u)<<27);
    for(unsigned tick=0;tick<20000;tick++) {
        uint32_t irq=gr(gt.se,0x610);gt.last_irq=irq;
        if(irq&GT_SPI_ERRORS){gt_abort();return 3;}
        unsigned occupied=gr(gt.se,0x800)&0xfffffff;
        if(occupied>gt.depth){gt_abort();return 4;}
        for(unsigned space=gt.depth-occupied;space && sent<n;space--) {
            uint32_t word=0;
            for(unsigned j=0;j<4 && sent<n;j++,sent++)word|=(uint32_t)(tx?tx[sent]:0xff)<<(j*8);
            gw(gt.se,0x700,word);
        }
        uint32_t status=gr(gt.se,0x804);
        unsigned words=status&0x1ffffff;
        if(words>gt.rxdepth){gt_abort();return 5;}
        for(unsigned w=0;w<words;w++) {
            unsigned valid=4;
            if(w+1==words && (status&0x80000000u)) {
                unsigned last=(status>>28)&7;if(last)valid=last;
            }
            if(!rx || valid>4 || got+valid>n){gt_abort();return 6;}
            uint32_t word=gr(gt.se,0x780);
            for(unsigned j=0;j<valid;j++)rx[got++]=(uint8_t)(word>>(j*8));
        }
        if(irq&1) {
            gw(gt.se,0x618,irq);
            return sent==n && (!rx || got==n)?0:7;
        }
        gw(gt.se,0x618,irq&0x4c000000u);
        delay(1);
    }
    gt_abort();return 8;
}
static int gt_read(uint8_t *data,unsigned size)
{
    static unsigned reads;
    int err=gt_xfer(NULL,data,size);
    if(reads++<4 && !err && size>=4)printf("GIULIA TOUCH: RX %u bytes: %02x %02x %02x %02x\n",
        size,data[0],data[1],data[2],data[3]);
    return err;
}
static int gt_write(const uint8_t *data,unsigned size){return gt_xfer(data,NULL,size);}

static int gt_controller(void)
{
    gt.gcc=ml_io_map(GT_GCC,0x54000);
    if(!gt.gcc)return 1;
    uint32_t branch=gr(gt.gcc,0x183bc),vote=gr(gt.gcc,0x52008);
    printf("GIULIA TOUCH: GCC branch=%#x vote=%#x rcgr=%#x cfg=%#x\n",
        branch,vote,gr(gt.gcc,0x183c8),gr(gt.gcc,0x183cc));
    /* Do not power a clock domain with an unverified parent/rail state. */
    if(branch&0x80000000u){printf("GIULIA TOUCH: SE clock halted; clock/power handoff required\n");return 2;}
    gt.se=ml_io_map(GT_SE,0x4000);
    if(!gt.se)return 3;
    uint32_t fw=gr(gt.se,0x68),iface=gr(gt.se,0x64),status=gr(gt.se,0x40);
    uint32_t tx=gr(gt.se,0xe24),rx=gr(gt.se,0xe28);
    gt.depth=(tx>>16)&0xff;gt.rxdepth=(rx>>16)&0xff;
    printf("GIULIA TOUCH: GENI fw=%#x iface=%#x status=%#x tx=%#x rx=%#x dma=%#x\n",
        fw,iface,status,tx,rx,gr(gt.se,0x258));
    if(((fw>>8)&0xff)!=1 || (status&0x1001) || (gr(gt.se,0xe40)&3)) {
        printf("GIULIA TOUCH: unsupported GENI firmware or active engine; no takeover\n");return 4;
    }
    for(unsigned i=0;i<7;i++) {
        gt.pin[i]=ml_io_map(GT_TLMM+gt_pins[i]*0x1000u,0x1000);
        if(!gt.pin[i])return 5;
        printf("GIULIA TOUCH: gpio%u cfg=%#x io=%#x\n",gt_pins[i],gr(gt.pin[i],0),gr(gt.pin[i],4));
    }
    if(iface&1) {
        int gpio_enabled=0;PE_parse_boot_argn("giulia_touch_gpio",&gpio_enabled,sizeof(gpio_enabled));
        if(gpio_enabled==1)return gt_gpio_prepare();
        printf("GIULIA TOUCH: GPI-only controller; GPIO backend not enabled\n");return 4;
    }
    if(((tx>>24)&0x3f)!=32 || ((rx>>24)&0x3f)!=32 || !gt.depth || gt.depth>64 ||
       !gt.rxdepth || gt.rxdepth>64 || gr(gt.se,0xc3c) || gr(gt.se,0xd3c))return 4;
    /* Require the board's reset and AVDD enable already deasserted/on. The
     * 1.8 V PMIC rail and pin ownership still belong to firmware. */
    for(unsigned i=4;i<6;i++)if((gr(gt.pin[i],0)&0x23c)!=0x200 || !(gr(gt.pin[i],4)&2))return 6;
    if((gr(gt.pin[6],0)&0x23c)!=0)return 7;
    for(unsigned i=0;i<4;i++)if(((gr(gt.pin[i],0)>>2)&15)!=1)return 8;
    if((gr(gt.se,0x800)&0xfffffff) || (gr(gt.se,0x804)&0x1ffffff))return 9;
    /* Use existing root frequency, conservative divider 64, mode 0 / CS0.
     * No GCC source, firmware, shared wrapper, SMMU or PMIC writes. */
    gw(gt.se,0x48,(64u<<4)|1);
    gw(gt.se,0x224,0);gw(gt.se,0x230,0);gw(gt.se,0x22c,0);
    gw(gt.se,0x24c,0);gw(gt.se,0x250,0);gw(gt.se,0x25c,0);
    gw(gt.se,0x268,4);gw(gt.se,0x274,0);gw(gt.se,0x278,0);
    gw(gt.se,0x254,0);gw(gt.se,0x260,0x7f8fe);gw(gt.se,0x264,0xffefe);
    gw(gt.se,0x284,0x7f8fe);gw(gt.se,0x288,0xffefe);
    gw(gt.se,0x258,0);gw(gt.se,0xe18,0);gw(gt.se,0xe1c,0);
    gw(gt.se,0x80c,0);gw(gt.se,0x810,0);gw(gt.se,0x814,gt.rxdepth-1);
    gw(gt.se,0x614,0x4c003e3f);gw(gt.se,0x618,0xffffffffu);
    return gr(gt.se,0x260)==0x7f8fe && gr(gt.se,0x284)==0x7f8fe?0:10;
}
static void gt_release(void)
{
    for(unsigned i=0;i<GT_CONTACTS;i++)if(gt.state.active&(1u<<i))
        sweet_hid_touch(i,0,gt.state.x[i],gt.state.y[i]);
    gt.state.active=0;
}
static void gt_thread(__unused void *arg,__unused wait_result_t result)
{
    struct gw_bus bus={gt_read,gt_write,gt_wait,64};
    struct gw_message msg;
    gt_wait(5000);
    int err=gt_controller();
    printf("GIULIA TOUCH: controller result=%d\n",err);
    if(err)goto done;
    err=gw_read(&bus,&msg);
    /* A cold touch controller can hold MISO low until its application boots. */
    for(unsigned retry=0;err==2 && retry<100 && !gt.dead;retry++) {
        gt_wait(10);err=gw_read(&bus,&msg);
    }
    printf("GIULIA TOUCH: first message result=%d code=%#x bytes=%u irq=%#x\n",
        err,err?0:msg.code,err?0:msg.size,gt.last_irq);
    if(err)goto done;
    if(msg.code!=0x10) {err=gw_command(&bus,2,NULL,0,&msg);if(err)goto done;}
    if(msg.size<24 || msg.data[1]!=1) {err=11;goto done;}
    /* Board identity comes from DT; this response confirms a live TouchCom
     * application, not a guessed report layout. Print bounded part number. */
    char part[17];memcpy(part,msg.data+2,16);part[16]=0;
    printf("GIULIA TOUCH: TouchCom v1 application part=%.16s build=%02x%02x%02x%02x\n",
        part,msg.data[21],msg.data[20],msg.data[19],msg.data[18]);
    bus.max_write=gw_le16(msg.data+22);
    if(bus.max_write<sizeof(gt_report_config)+3){err=12;goto done;}
    err=gw_command(&bus,0x20,NULL,0,&msg);if(err)goto done;
    if(msg.size<40 || gw_le16(msg.data+2) || gw_le16(msg.data+36)>GT_CONTACTS){err=13;goto done;}
    unsigned mx=gw_le16(msg.data+32),my=gw_le16(msg.data+34),config_size=gw_le16(msg.data+12);
    if(!mx || !my || config_size<sizeof(gt_report_config) || config_size>GW_MAX ||
       config_size+3>bus.max_write){err=14;goto done;}
    printf("GIULIA TOUCH: app max=%ux%u objects=%u\n",mx,my,gw_le16(msg.data+36));
    uint8_t raw=0x23,lbp[3]={0x11,2,0},touch=0x11;
    err=gw_command(&bus,6,&raw,1,&msg);if(err)goto done;
    err=gw_command(&bus,0x24,lbp,3,&msg);if(err)goto done;
    err=gw_command(&bus,0x23,lbp,1,&msg);if(err)goto done;
    if(msg.size!=2 || gw_le16(msg.data)!=2){err=15;goto done;}
    uint8_t config[GW_MAX]={0};memcpy(config,gt_report_config,sizeof(gt_report_config));
    err=gw_command(&bus,0x26,config,config_size,&msg);if(err)goto done;
    err=gw_command(&bus,0x25,NULL,0,&msg);if(err)goto done;
    if(!gt_config_matches(msg.data,msg.size)){err=16;goto done;}
    err=gw_command(&bus,5,&touch,1,&msg);if(err)goto done;
    printf("GIULIA TOUCH: report configuration verified; physical polling active\n");
    unsigned errors=0,frames=0;
    for(;;) {
        gt_wait(10);
        if(gr(gt.pin[6],4)&1)continue;
        err=gw_read(&bus,&msg);
        if(!err && msg.code==0x10){err=17;break;}
        if(!err && msg.code==0x11) {
            struct gt_frame frame;
            err=gt_decode(msg.code,msg.data,msg.size,mx,my,1,&gt.state,&frame);
            if(!err) {
                for(unsigned i=0;i<frame.count;i++) {
                    struct gt_event *e=&frame.events[i];sweet_hid_touch(e->id,e->down,e->x,e->y);
                }
                if(frames++<10 || frames%100==0)printf("GIULIA TOUCH: contacts=%#x frames=%u\n",frame.active,frames);
            }
        }
        if(err) {
            if(++errors>=3 || gt.dead)break;
        } else errors=0;
    }
done:
    gt_release();
    gt_gpio_restore();
    printf("GIULIA TOUCH: worker stopped result=%d; GUI continues\n",err);
    thread_terminate(current_thread());
}
void giulia_touch_start(void)
{
    int enabled=0;thread_t thread;
    if(gt.started)return;gt.started=true;
    PE_parse_boot_argn("giulia_touch",&enabled,sizeof(enabled));
    if(enabled!=1)return;
    kern_return_t kr=kernel_thread_start(gt_thread,NULL,&thread);
    if(kr==KERN_SUCCESS)thread_deallocate(thread);
    else printf("GIULIA TOUCH: thread creation failed %d\n",kr);
}
#endif
