/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Ace 5 WCN7850 scan transport and iOS16 IO80211 compatibility service.
 * Ownership starts after a checked enumerated or prepared UEFI handoff.
 * The worker serializes all device access. User clients only queue scans and
 * read snapshots; they cannot name MMIO/DMA addresses or upload firmware.
 */
#include <pexpert/arm64/board_config.h>
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSData.h>
#include <mach-o/loader.h>
#include <arm/machine_routines.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <mach/thread_act.h>
#include <pexpert/pexpert.h>
extern "C" {
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kern_event.h>
#include <sys/sockio.h>
#include <sys/kpi_mbuf.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/kpi_interface.h>
extern unsigned long gPhysBase,gPhysSize,gVirtBase;
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
}
#include "giulia_wifi_pci.h"
#include "giulia_wifi_link.h"
#include "giulia_wifi_smmu.h"
#include "giulia_wifi_mhi.h"
#include "giulia_wifi_ce.h"
#include "giulia_wifi_apple.h"
#include "giulia_wifi_firmware.h"
#include "giulia_wifi_firmware_pin.h"
#include "giulia_wifi_qmp.h"
#include "giulia_wifi_pmu.h"

#ifndef GIULIA_GPU_QMP_GATE
#define GIULIA_GPU_QMP_GATE 0
#endif
#if GIULIA_GPU_QMP_GATE
extern "C" int giulia_gpu_qmp_available(void);
#endif

#define GW_ARENA_BYTES 0x4000000u
#define GW_IOVA 0x20000000ull
#define GW_CE_OFFSET 0x1200000u
#define GW_ALLOC_START 0x1400000u
class IO80211Controller;
struct giulia_wifi_runtime {
    IO80211Controller *owner;IOLock *lock;
    vm_offset_t root,config,parf,smmu,bar;
    vm_offset_t link_maps[GWL_REGIONS];
    struct gwl_state link_state;unsigned handoff_mode;uint64_t link_started;
    IOBufferMemoryDescriptor *arena,*tables;
    IOMemoryDescriptor *firmware_memory;IOMemoryMap *firmware_map;
    uint8_t *memory;uint64_t arena_pa,table_pa;size_t allocated;
    struct gwp_state pci;struct gwp_sid_state sid;struct gwp_msi_state msi;
    struct gws_state translation;struct gwm_state mhi;struct gwq_state qmi;
    struct gwc_state ce;struct gww_state wmi;struct gwf_view firmware;
    struct gwp_io pio;struct gws_io sio;struct gwm_io mio;struct gwq_io qio;struct gwc_io cio;struct gww_io wio;
    ifnet_t interface;uint8_t mac[6];unsigned ready,powered,scan_pending,stage,error,quarantine,qmp_retained;
    const char *error_domain,*operation;unsigned pci_trace,smmu_last_offset,smmu_last_value,smmu_seen,debug_wait;
    unsigned mhi_last_offset,mhi_last_value,mhi_seen;
};
/* Sole APSS QMP provider in this kernel profile. Future AOP clients must share
 * this provider/lease, never open a second channel over its pending mailbox. */
static struct {
    vm_offset_t ram,ipcc;
    const void *lease;
    giulia_wifi_runtime *owner;
    struct qpt_state state;
} gw_qmp;
static unsigned gw_terminal_go,gw_terminal_waiting;
extern "C" int giulia_wifi_terminal(const char *command) {
    if(strcmp(command,"wifi.start") && strcmp(command,"wifi.status"))return 0;
    int enabled=0;PE_parse_boot_argn("giulia_debug",&enabled,sizeof(enabled));
    if(enabled!=1){IOLog("GIULIA WIFI: terminal control requires giulia_debug=1\n");return 1;}
    if(!strcmp(command,"wifi.start"))__atomic_store_n(&gw_terminal_go,1u,__ATOMIC_RELEASE);
    IOLog("GIULIA WIFI: terminal command=%s start_requested=%u waiting=%u\n",command,
        __atomic_load_n(&gw_terminal_go,__ATOMIC_ACQUIRE),__atomic_load_n(&gw_terminal_waiting,__ATOMIC_ACQUIRE));
    return 1;
}
class IONetworkController : public IOService { OSDeclareDefaultStructors(IONetworkController); };
OSDefineMetaClassAndStructors(IONetworkController,IOService);
class IO80211Interface : public IOService { OSDeclareDefaultStructors(IO80211Interface); };
OSDefineMetaClassAndStructors(IO80211Interface,IOService);
class IO80211Controller : public IONetworkController {
    OSDeclareDefaultStructors(IO80211Controller);
public:
    struct giulia_wifi_runtime *r;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void free(void) APPLE_KEXT_OVERRIDE;
    IOReturn query(const struct gwi_request &,void *,size_t *,unsigned *);
    bool publishInterface();
};
OSDefineMetaClassAndStructors(IO80211Controller,IONetworkController);
static uint32_t gr32(vm_offset_t base,unsigned offset) {
    uint32_t n=*(volatile uint32_t *)(base+offset);__asm__ volatile("dmb oshld" ::: "memory");return n;
}
static void gw32(vm_offset_t base,unsigned offset,uint32_t n) {
    *(volatile uint32_t *)(base+offset)=n;__asm__ volatile("dsb oshst" ::: "memory");
}
static void gw_delay(void *,unsigned us) {if(us>=1000)IOSleep((us+999)/1000);else if(us)IODelay(us);}
static uint64_t gw_now(void *) {uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns/1000000;}
static uint32_t gl_read(void *v,unsigned region,unsigned offset) {
    return gr32(((giulia_wifi_runtime *)v)->link_maps[region],offset);
}
static void gl_write(void *v,unsigned region,unsigned offset,uint32_t value) {
    gw32(((giulia_wifi_runtime *)v)->link_maps[region],offset,value);
}
static void gl_log(void *v,const char *name,uint64_t address,uint32_t value,uint32_t mask,uint32_t expected) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    const char *result=!mask?"sample":value==0xffffffffu?"invalid":(value&mask)==expected?"pass":"FAIL";
    IOLog("GIULIA XNU PCIE: +%llu ms step=%u %s %s addr=0x%llx value=0x%x mask=0x%x expected=0x%x\n",
        (unsigned long long)(gw_now(NULL)-r->link_started),r->link_state.stage,name,result,
        (unsigned long long)address,value,mask,expected);
}
static int gw_xnu_link(giulia_wifi_runtime *r) {
    IOLog("GIULIA WIFI: XNU PCIe link worker starting after ExitBootServices; handoff=prepared\n");
    r->link_started=gw_now(NULL);
    for(unsigned i=0;i<GWL_REGIONS;i++) {
        r->link_maps[i]=ml_io_map((vm_offset_t)gwl_bases[i],gwl_sizes[i]);
        if(!r->link_maps[i]){IOLog("GIULIA XNU PCIE: mapping failed region=%u pa=0x%llx\n",i,(unsigned long long)gwl_bases[i]);return GWL_ARGUMENT;}
    }
    r->root=r->link_maps[GWL_ROOT];r->config=r->link_maps[GWL_CFG];r->parf=r->link_maps[GWL_PARF];
    struct gwl_io io={r,gl_read,gl_write,gw_delay,gl_log};
    int error=gwl_train(&io,&r->link_state);
    IOLog("GIULIA XNU PCIE: result=%d stage=%u polls=%u elapsed_ms=%llu elbi=0x%x link=0x%x endpoint=0x%x resources=retained error_name=%s\n",
        error,r->link_state.stage,r->link_state.polls,(unsigned long long)(gw_now(NULL)-r->link_started),
        r->link_state.elbi,r->link_state.link,r->link_state.device,gwl_error_name((unsigned)error));
    return error;
}
static uint32_t gp_read(void *v,unsigned space,unsigned offset) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    vm_offset_t base=space==GWP_ENDPOINT?r->config:space==GWP_ROOT?r->root:space==GWP_ATU?r->root+0x1000:r->parf;
    uint32_t value=gr32(base,offset);
    if((r->stage==2 || r->stage==3) && r->pci_trace++<256)
        IOLog("GIULIA PCI: read space=%u offset=0x%x value=0x%x operation=%s\n",space,offset,value,r->operation);
    return value;
}
static void gp_write(void *v,unsigned space,unsigned offset,uint32_t n) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    vm_offset_t base=space==GWP_ENDPOINT?r->config:space==GWP_ROOT?r->root:space==GWP_ATU?r->root+0x1000:r->parf;
    gw32(base,offset,n);
    if((r->stage==2 || r->stage==3) && r->pci_trace++<256)
        IOLog("GIULIA PCI: write space=%u offset=0x%x value=0x%x operation=%s\n",space,offset,n,r->operation);
}
static uint32_t gs_read(void *v,unsigned offset){
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;uint32_t value=gr32(r->smmu,offset);
    if(!r->smmu_seen || offset!=r->smmu_last_offset || value!=r->smmu_last_value)
        IOLog("GIULIA SMMU: read pa=0x%x value=0x%x operation=%s error=%u cb=%u\n",
            0x15000000+offset,value,r->operation,r->translation.error,r->translation.cb);
    r->smmu_seen=1;r->smmu_last_offset=offset;r->smmu_last_value=value;return value;
}
static void gs_write(void *v,unsigned offset,uint32_t n){
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    IOLog("GIULIA SMMU: write-request pa=0x%x value=0x%x operation=%s error=%u cb=%u failed_reg=0x%x actual=0x%x expected=0x%x\n",
        0x15000000+offset,n,r->operation,r->translation.error,r->translation.cb,
        r->translation.check_offset,r->translation.check_value,r->translation.check_expected);
    if(r->debug_wait)IOSleep(10); /* Let the attached USB worker drain the pre-store record. */
    gw32(r->smmu,offset,n);
}
static int gs_idle(void *v){giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;uint32_t n=gr32(r->config,4);return n!=0xffffffffu&&!(n&4);}
static int gs_owned(void *v,uint64_t pa,size_t n){giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    return (pa==r->arena_pa&&n==GW_ARENA_BYTES)||(pa==r->table_pa&&n==0x4000);}
/* CPU mappings for the arena/tables inhibit caching. The SMMU page walker
 * reads the same physical allocation. Barriers also constrain compiler loads. */
static void gs_clean(void *,void *,size_t){__asm__ volatile("dsb sy" ::: "memory");}
static int gm_ready(void *v,uint64_t pa,size_t n){giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    return pa>=GW_IOVA && n<=GW_ARENA_BYTES && pa-GW_IOVA<=GW_ARENA_BYTES-n &&
        r->sid.changed && r->msi.changed && gws_live(&r->sio,&r->translation);}
static void gw_dma_snapshot(giulia_wifi_runtime *r,const char *checkpoint) {
    if(!r->translation.active)return;
    unsigned cb=gws_cb(r->translation.cb);
    IOLog("GIULIA DMA: checkpoint=%s cb=%u fsr=0x%x far_hi=0x%x far_lo=0x%x fsynr0=0x%x fsynr1=0x%x cb_sid=0x%x\n",
        checkpoint,r->translation.cb,gr32(r->smmu,cb+0x58),gr32(r->smmu,cb+0x64),
        gr32(r->smmu,cb+0x60),gr32(r->smmu,cb+0x68),gr32(r->smmu,cb+0x6c),
        gr32(r->smmu,0x1400+4*r->translation.cb));
    IOLog("GIULIA DMA: checkpoint=%s root_command=0x%x aer_uncorrectable=0x%x aer_correctable=0x%x tlp=0x%x,0x%x,0x%x,0x%x\n",
        checkpoint,gr32(r->root,4),gr32(r->root,0x104),gr32(r->root,0x110),
        gr32(r->root,0x11c),gr32(r->root,0x120),gr32(r->root,0x124),gr32(r->root,0x128));
}
static int gm_master(void *v,int on){giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    int ready=!on||gm_ready(v,GW_IOVA,GW_ARENA_BYTES);
    IOLog("GIULIA MHI: bus-master request=%d dma_ready=%d iova=0x%llx arena_pa=0x%llx\n",on,ready,
        (unsigned long long)GW_IOVA,(unsigned long long)r->arena_pa);
    if(on)gw_dma_snapshot(r,"before-master");
    int ok=gwp_master(&r->pio,&r->pci,on,ready);
    IOLog("GIULIA MHI: bus-master request=%d result=%d command=0x%x root_command=0x%x\n",
        on,ok,gr32(r->config,4),gr32(r->root,4));return ok;}
static uint32_t gm_read(void *v,uint32_t o){
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;uint32_t value=gr32(r->bar,o);
    if(r->stage==4 && (!r->mhi_seen || o!=r->mhi_last_offset || value!=r->mhi_last_value))
        IOLog("GIULIA MHI: read offset=0x%x value=0x%x stage=%u operation=%s\n",o,value,r->mhi.stage,r->operation);
    r->mhi_seen=1;r->mhi_last_offset=o;r->mhi_last_value=value;return value;
}
static void gm_write(void *v,uint32_t o,uint32_t n){
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    if(r->stage==4)IOLog("GIULIA MHI: write-request offset=0x%x value=0x%x stage=%u operation=%s\n",o,n,r->mhi.stage,r->operation);
    if(r->stage==4 && r->debug_wait)IOSleep(10);
    gw32(r->bar,o,n);
}
static void gm_sync(void *v,size_t offset,size_t bytes,int to_device){
    __asm__ volatile("dsb sy" ::: "memory");
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    if(r->stage==4 && bytes>=GWM_FIRMWARE)
        IOLog("GIULIA MHI: DMA byte-access preparation complete offset=0x%lx bytes=0x%lx to_device=%d\n",
            (unsigned long)offset,(unsigned long)bytes,to_device);
}
static int gm_verify(void *v,const void *data,size_t n,unsigned major){giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;struct gwf_asset a;
    return gwf_main(&r->firmware,major,&a)&&a.bytes==n&&a.data==data;}
static void gm_receive(void *v,const void *p,size_t n){giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;(void)gwq_receive(&r->qmi,&r->qio,p,n);}
static int gq_send(void *v,const void *p,size_t n){giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    /* May run from the RX callback to acknowledge QRTR. Do not reenter MHI
     * polling before its current event has been retired. */
    const uint8_t *wire=(const uint8_t *)p;
    if(n>=39 && gwq_u32(wire)==1 && gwq_u32(wire+4)==1 && !wire[32]) {
        unsigned message=gwq_u16(wire+35);size_t bytes=gwq_u16(wire+37),z=0;
        if(bytes<=n-39 && (message==0x36 || message==0x3c)) {
            const uint8_t *field=gwq_field(wire+39,bytes,1,&z);
            if(message==0x36 && field && z && field[0]<=GWQ_SEGMENTS && z==1u+17u*field[0]) {
                IOLog("GIULIA QMI: memory-response count=%u\n",field[0]);
                for(unsigned i=0;i<field[0];i++) {
                    const uint8_t *entry=field+1+17*i;
                    IOLog("GIULIA QMI: memory-response index=%u type=%u bytes=%u iova=0x%llx\n",
                        i,gwq_u32(entry+12),gwq_u32(entry+8),(unsigned long long)gwq_u64(entry));
                }
            } else if(message==0x3c && field && z==8) {
                uint64_t address=gwq_u64(field);
                const uint8_t *length=gwq_field(wire+39,bytes,2,&z);
                if(length && z==4)IOLog("GIULIA QMI: M3-info iova=0x%llx bytes=%u\n",
                    (unsigned long long)address,gwq_u32(length));
            }
        }
    }
    return gwm_send(&r->mio,&r->mhi,p,n)!=GWM_OK;}
static int gq_pump(void *v,unsigned us){giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    int e=gwm_poll(&r->mio,&r->mhi,128);gw_delay(v,us);return e || r->qmi.error;}
static int gq_allocate(void *v,size_t n,uint64_t *pa,void **va){giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    size_t off=(r->allocated+0x3fff)&~(size_t)0x3fff;
    if(!n || off>GW_ARENA_BYTES || n>GW_ARENA_BYTES-off) {
        IOLog("GIULIA QMI: allocation failed bytes=0x%lx offset=0x%lx arena=0x%x\n",
            (unsigned long)n,(unsigned long)off,GW_ARENA_BYTES);return 1;
    }
    *pa=GW_IOVA+off;*va=r->memory+off;r->allocated=off+n;return 0;}
static void gq_clean(void *,const void *,size_t){__asm__ volatile("dsb sy" ::: "memory");}
static int gq_asset(void *v,unsigned kind,unsigned major,uint32_t chip,uint32_t board,const void **data,size_t *n){
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;struct gwf_asset a;
    int ok=kind==GWQ_REGDB?gwf_get(&r->firmware,"kiwi/regdb.bin",&a):kind==GWQ_BOARD?gwf_board(&r->firmware,chip,board,&a):gwf_phy(&r->firmware,major,&a);
    if(!ok)return 1;*data=a.data;*n=a.bytes;return 0;}
static uint32_t gqp_read(void *,uint32_t offset) {
    if((offset&3) || offset>QPT_MSGRAM_BYTES-4)return 0xffffffffu;
    return gr32(gw_qmp.ram,offset);
}
static void gqp_write(void *,uint32_t offset,uint32_t value) {
    if(!(offset&3) && offset<=QPT_MSGRAM_BYTES-4)gw32(gw_qmp.ram,offset,value);
}
static void gqp_barrier(void *){__asm__ volatile("dsb sy" ::: "memory");}
static void gqp_kick(void *v) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    IOLog("GIULIA QMP: doorbell pa=0x%x value=0 phase=%u pending=%u\n",
        QPT_DOORBELL_PA,gw_qmp.state.phase,gw_qmp.state.pending);
    if(r->debug_wait)IOSleep(10);
    gw32(gw_qmp.ipcc,0x0c,0); /* APSS IPCC_SEND_ID; never read RECV_ID. */
}
static uint64_t gqp_now(void *){return gw_now(NULL)*1000u;}
static int gqp_claim(void *,const void *owner) {
    const void *expected=NULL;
    return __atomic_compare_exchange_n(&gw_qmp.lease,&expected,owner,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
static int gqp_owns(void *,const void *owner)
{return __atomic_load_n(&gw_qmp.lease,__ATOMIC_ACQUIRE)==owner;}
static void gqp_release(void *,const void *owner) {
    const void *expected=owner;
    (void)__atomic_compare_exchange_n(&gw_qmp.lease,&expected,(const void *)NULL,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
static struct qpt_io gqp_io(giulia_wifi_runtime *r)
{return (struct qpt_io){r,QPT_MSGRAM_BYTES,gqp_read,gqp_write,gqp_barrier,gqp_kick,gqp_now,gw_delay,gqp_claim,gqp_owns,gqp_release};}
static void gqp_drop_owner(giulia_wifi_runtime *r) {
    giulia_wifi_runtime *expected=r;
    (void)__atomic_compare_exchange_n(&gw_qmp.owner,&expected,(giulia_wifi_runtime *)NULL,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
static int gq_power(void *v,const struct gwq_pmu_param *params,unsigned count) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
#if GIULIA_GPU_QMP_GATE
    if(!giulia_gpu_qmp_available()) {
        IOLog("GIULIA QMP: GPU firmware handoff retains uncertain channel; refusing WLAN QMP reuse\n");
        return QPT_OWNER_LOST;
    }
#endif
    if(!count)return 0;
    if(!params || count>16)return -2;
    struct qpp_entry entries[16];struct qpp_plan plan={};
    for(unsigned i=0;i<count;i++) {
        for(unsigned j=0;j<32;j++)entries[i].name[j]=params[i].pin_name[j];
        entries[i].wake_valid=params[i].wake_valid;entries[i].wake_mv=params[i].wake_mv;
        entries[i].sleep_valid=params[i].sleep_valid;entries[i].sleep_mv=params[i].sleep_mv;
    }
    int error=qpp_make(entries,count,&plan);
    IOLog("GIULIA QMP: PMU plan result=%d count=%u commands=%u bad_entry=%u bad_rail=%u\n",
        error,count,plan.command_count,plan.bad_entry,plan.bad_rail);
    if(error)return 100+error;
    if(!plan.command_count)return 0;
    giulia_wifi_runtime *expected=NULL;
    if(!__atomic_compare_exchange_n(&gw_qmp.owner,&expected,r,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return QPT_BUSY;
    if(!gw_qmp.ram)gw_qmp.ram=ml_io_map(QPT_MSGRAM_PA,QPT_MSGRAM_BYTES);
    if(!gw_qmp.ipcc)gw_qmp.ipcc=ml_io_map(0x00406000,0x1000);
    if(!gw_qmp.ram || !gw_qmp.ipcc) {gqp_drop_owner(r);return -3;}
    struct qpt_io io=gqp_io(r);
    error=qpt_open(&io,&gw_qmp.state);
    IOLog("GIULIA QMP: open result=%d phase=%u offset=0x%x size=0x%x last_reg=0x%x value=0x%x\n",
        error,gw_qmp.state.phase,gw_qmp.state.offset,gw_qmp.state.size,gw_qmp.state.last_offset,gw_qmp.state.last_value);
    if(error) {if(!gw_qmp.state.leased && !gw_qmp.state.owned)gqp_drop_owner(r);return error;}
    for(unsigned i=0;i<plan.command_count;i++) {
        const struct qpp_command *command=qpp_get_command(&plan,i);
        if(!command)return -4;
        IOLog("GIULIA QMP: send index=%u pdc=%s resource=%s parameter=%s mv=%u bytes=%u\n",
            i,qpp_pdc(command->rail),qpp_resource(command->rail),command->sleep?"dwnval":"upval",command->mv,QPP_COMMAND_BYTES);
        error=qpt_send(&io,&gw_qmp.state,command->words,QPP_COMMAND_BYTES);
        IOLog("GIULIA QMP: send index=%u result=%d pending=%u phase=%u elapsed_us=%llu last_reg=0x%x value=0x%x\n",
            i,error,gw_qmp.state.pending,gw_qmp.state.phase,(unsigned long long)gw_qmp.state.wait_elapsed_us,
            gw_qmp.state.last_offset,gw_qmp.state.last_value);
        if(error)return error;
    }
    IOLog("GIULIA QMP: PMU commands consumed=%u; voltage readback not provided by QMP\n",plan.command_count);
    return 0;
}
static void gw_qmp_stop(giulia_wifi_runtime *r) {
    if(__atomic_load_n(&gw_qmp.owner,__ATOMIC_ACQUIRE)!=r)return;
    if(!gw_qmp.state.owned && !gw_qmp.state.leased) {gqp_drop_owner(r);return;}
    if(gw_qmp.state.pending || gw_qmp.state.phase!=QPT_READY) {
        r->qmp_retained=1;
        IOLog("GIULIA QMP: retaining uncertain channel and owner phase=%u pending=%u\n",gw_qmp.state.phase,gw_qmp.state.pending);return;
    }
    struct qpt_io io=gqp_io(r);int error=qpt_close(&io,&gw_qmp.state);
    IOLog("GIULIA QMP: close result=%d phase=%u pending=%u\n",error,gw_qmp.state.phase,gw_qmp.state.pending);
    if(!error)gqp_drop_owner(r);
    else r->qmp_retained=1;
}
/* Serialized dynamic BAR window. Preserve the independent static windows.
 * The endpoint's MHI registers stay in BAR's low fixed aperture. */
static unsigned gc_offset(giulia_wifi_runtime *r,uint32_t offset) {
    if(offset<0x80000)return offset;
    uint32_t window=gr32(r->bar,0x310c);if(window==0xffffffffu)return 0xffffffffu;
    uint32_t desired=(window&~0x3fu)|0x40000000u|((offset>>19)&0x3f);
    gw32(r->bar,0x310c,desired);
    if(gr32(r->bar,0x310c)!=desired)return 0xffffffffu;
    return 0x80000|(offset&0x7ffff);
}
static uint32_t gc_read(void *v,uint32_t offset){giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;unsigned o=gc_offset(r,offset);
    return o==0xffffffffu?o:gr32(r->bar,o);}
static void gc_write(void *v,uint32_t offset,uint32_t n){giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;unsigned o=gc_offset(r,offset);if(o!=0xffffffffu)gw32(r->bar,o,n);}
static int gc_receive(void *v,unsigned pipe,const void *p,size_t n){giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;return gww_receive(&r->wmi,&r->wio,pipe,p,n);}
static int gw_send(void *v,unsigned pipe,const void *p,size_t n){giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;int e=gwc_send(&r->ce,&r->cio,pipe,p,n);return e==GWC_FULL?2:e?1:0;}
static int gw_pump(void *v,unsigned us){giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    int e=gwm_poll(&r->mio,&r->mhi,128);if(!e)e=gwc_poll(&r->ce,&r->cio);gw_delay(v,us);return e||r->qmi.error||r->wmi.error;}
static void gw_notice(void *v,unsigned status,uint32_t id){giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    /* IO80211.framework's observed PF_SYSTEM event filter is vendor=1,
     * class=6, subclass=1; event 10 carries a 32-bit scan completion status. */
    struct {char name[16];uint32_t status;} data; bzero(&data,sizeof(data));strlcpy(data.name,"en0",sizeof(data.name));data.status=status;
    struct kev_msg event;bzero(&event,sizeof(event));event.vendor_code=1;event.kev_class=6;event.kev_subclass=1;event.event_code=10;
    event.dv[0].data_ptr=&data;event.dv[0].data_length=sizeof(data);(void)kev_post_msg(&event);
    IOLog("GIULIA WIFI: scan id=%u status=%u BSS=%u\n",id,status,r->wmi.bss_count);
}
static int gw_handoff(void) {
    char text[225]={0},pending[225]={0};
    bool normal=PE_parse_boot_argn("giulia_wifi5",text,sizeof(text));
    bool prepared=PE_parse_boot_argn("giulia_wifip",pending,sizeof(pending));
    if(normal && prepared)return 0;
    return prepared?(int)gwl_handoff(pending,1):normal?(int)gwl_handoff(text,0):0;
}
static bool gw_firmware(giulia_wifi_runtime *r) {
    const struct mach_header_64 *h=&_mh_execute_header;
    if(h->magic!=MH_MAGIC_64 || h->ncmds>128 || h->sizeofcmds>0x10000)return false;
    const uint8_t *p=(const uint8_t *)(h+1);size_t left=h->sizeofcmds;struct segment_command_64 firmware;bzero(&firmware,sizeof(firmware));bool found=false;
    for(unsigned i=0;i<h->ncmds;i++) {
        if(left<sizeof(struct load_command))return false;struct load_command command;memcpy(&command,p,sizeof(command));
        if(command.cmdsize<8 || command.cmdsize>left || (command.cmdsize&7))return false;
        if(command.cmd==LC_SEGMENT_64 && command.cmdsize>=sizeof(struct segment_command_64)) {
            struct segment_command_64 seg;memcpy(&seg,p,sizeof(seg));
            if(!strncmp(seg.segname,"__WIFI_FW",16)){
                if(found || command.cmdsize!=sizeof(seg) || seg.nsects || seg.flags || seg.initprot!=VM_PROT_READ || seg.maxprot!=VM_PROT_READ)return false;
                firmware=seg;found=true;
            }
        }
        p+=command.cmdsize;left-=command.cmdsize;
    }
    if(left || !found || firmware.filesize!=GW_FIRMWARE_PACK_BYTES || firmware.vmaddr<gVirtBase ||
        firmware.vmsize<firmware.filesize || firmware.vmaddr-gVirtBase>gPhysSize ||
        firmware.vmsize>gPhysSize-(firmware.vmaddr-gVirtBase))return false;
    uint64_t physical=gPhysBase+firmware.vmaddr-gVirtBase;
    /* arm_vm_prot_init only maps its known segment names. The loader reserves
     * __WIFI_FW through TopOfKernelData, but it must not be dereferenced via
     * an assumed static virtual mapping. Map that bounded physical RAM here. */
    r->firmware_memory=IOMemoryDescriptor::withPhysicalAddress(physical,firmware.filesize,kIODirectionIn);
    if(!r->firmware_memory)return false;
    r->firmware_map=r->firmware_memory->map(kIOMapReadOnly|kIOMapCopybackCache);
    if(!r->firmware_map)return false;
    const void *data=(const void *)r->firmware_map->getVirtualAddress();
    if(!gwf_open(&r->firmware,data,firmware.filesize,gw_firmware_pack_sha256))return false;
    IOLog("GIULIA WIFI: firmware pack mapped read-only and digest verified\n");return true;
}
static bool gw_allocate(giulia_wifi_runtime *r) {
    r->arena=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,kIODirectionInOut|kIOMemoryPhysicallyContiguous|kIOMapInhibitCache,GW_ARENA_BYTES,0x200000);
    r->tables=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,kIODirectionInOut|kIOMemoryPhysicallyContiguous|kIOMapInhibitCache,0x4000,0x4000);
    if(!r->arena || !r->tables)return false;
    IOByteCount bytes=0;r->arena_pa=r->arena->getPhysicalSegment(0,&bytes,kIOMemoryMapperNone);
    if(bytes<GW_ARENA_BYTES || (r->arena_pa&0x1fffff))return false;
    r->table_pa=r->tables->getPhysicalSegment(0,&bytes,kIOMemoryMapperNone);
    if(bytes<0x4000 || (r->table_pa&0x3fff))return false;
    if(r->arena_pa<gPhysBase || gPhysSize<GW_ARENA_BYTES || r->arena_pa-gPhysBase>gPhysSize-GW_ARENA_BYTES ||
        r->table_pa<gPhysBase || r->table_pa-gPhysBase>gPhysSize-0x4000)return false;
    r->memory=(uint8_t *)r->arena->getBytesNoCopy();r->allocated=GW_ALLOC_START;return r->memory!=NULL;
}
static int gw_bringup(giulia_wifi_runtime *r) {
    r->error_domain="XNU_PCIE";r->operation="link-training";
    if(r->handoff_mode==2) {r->stage=9;int e=gw_xnu_link(r);if(e)return e;}
    r->error_domain="WIFI_SETUP";r->operation="firmware-map";
    r->stage=1;if(!gw_firmware(r))return 1;
    r->operation="controller-mmio-map";
    if(!r->root)r->root=ml_io_map(0x60000000,0x2000);
    if(!r->config)r->config=ml_io_map(0x60100000,0x1000);
    if(!r->parf)r->parf=ml_io_map(0x1c00000,0x3000);
    r->smmu=ml_io_map(0x15000000,0x100000);
    if(!r->root || !r->config || !r->parf || !r->smmu)return 2;
    r->pio=(struct gwp_io){r,gp_read,gp_write,gw_delay};r->sio=(struct gws_io){r,gs_read,gs_write,gw_delay,gs_idle,gs_owned,gs_clean};
    r->stage=2;
    r->operation="link-status-recheck";
    if(!(gr32(r->root,0x80)&(1u<<29)) || !(gr32(r->root,0xf28)&(1u<<10)))return 3;
    r->error_domain="PCI_ENUM";r->operation="pci-enumerate";
    int e=gwp_enumerate(&r->pio,&r->pci,0x60300000,0x3d00000);
    IOLog("GIULIA PCI: enumerate result=%d name=%s cap_reason=%s cap_offset=0x%x cap_value=0x%x cap_count=%u pcie=0x%x msi=0x%x msi_control=0x%x bar=0x%llx bytes=0x%llx ready=%u restore_failed=%u\n",
        e,gwp_error_name((unsigned)e),r->pci.cap_reason?r->pci.cap_reason:"not-reached",
        r->pci.cap_offset,r->pci.cap_value,r->pci.cap_count,r->pci.pcie_cap,r->pci.msi_cap,r->pci.msi_control,
        (unsigned long long)r->pci.bar_base,(unsigned long long)r->pci.bar_bytes,r->pci.ready,r->pci.restore_failed);
    if(e)return e;
    r->operation="bar-minimum-size";
    if(r->pci.bar_bytes<0x100000)return GWP_BAD_BAR;
    r->error_domain="WIFI_SETUP";r->operation="bar-mmio-map";
    r->bar=ml_io_map((vm_offset_t)r->pci.bar_base,(vm_size_t)r->pci.bar_bytes);if(!r->bar)return 4;
    IOLog("GIULIA PCI: BAR mapped pa=0x%llx bytes=0x%llx; endpoint DMA remains disabled\n",
        (unsigned long long)r->pci.bar_base,(unsigned long long)r->pci.bar_bytes);
    /* Reserve contiguous RAM before userspace fragments it while a debugger
     * connects. SID/SMMU publication and device DMA still wait for wifi.start. */
    r->operation="dma-arena-allocation";
    r->stage=3;if(!gw_allocate(r))return 5;
    IOLog("GIULIA WIFI: DMA allocations arena_pa=0x%llx tables_pa=0x%llx bytes=0x%x\n",
        (unsigned long long)r->arena_pa,(unsigned long long)r->table_pa,GW_ARENA_BYTES);
    int wait=0;PE_parse_boot_argn("giulia_wifi_wait",&wait,sizeof(wait));
    if(wait==1) {
        r->operation="await-terminal-start";r->debug_wait=1;
        __atomic_store_n(&gw_terminal_waiting,1u,__ATOMIC_RELEASE);
        IOLog("GIULIA WIFI: awaiting wifi.start before DMA setup\n");
        while(!__atomic_load_n(&gw_terminal_go,__ATOMIC_ACQUIRE))IOSleep(25);
        __atomic_store_n(&gw_terminal_waiting,0u,__ATOMIC_RELEASE);
        IOLog("GIULIA WIFI: terminal released DMA setup\n");
    }
    r->operation="requester-sid-claim";
    if(!gwp_sid_claim(&r->pio,&r->sid))return 6;
    r->error_domain="SMMU";r->operation="smmu-take";
    e=gws_take(&r->sio,&r->translation,(uint64_t *)r->tables->getBytesNoCopy(),r->table_pa,r->arena_pa,GW_IOVA,GW_ARENA_BYTES);
    IOLog("GIULIA SMMU: result=%d cb=%u slot=%u active=%u quarantined=%u failed_reg=0x%x actual=0x%x expected=0x%x\n",
        e,r->translation.cb,r->translation.slot,r->translation.active,r->translation.quarantined,
        r->translation.check_offset,r->translation.check_value,r->translation.check_expected);
    if(e)return e;
    r->error_domain="WIFI_SETUP";r->operation="msi-sink";
    if(!gwp_msi_sink(&r->pio,&r->pci,&r->msi,GW_IOVA,GW_ARENA_BYTES,GW_IOVA+GWM_MSI_SINK))return 7;
    r->mio=(struct gwm_io){r,gm_read,gm_write,gw_delay,gm_ready,gm_master,gm_verify,gm_sync,gm_receive,r->memory,GW_IOVA,GW_ARENA_BYTES,(uint32_t)r->pci.bar_bytes};
    r->qio=(struct gwq_io){r,gq_send,gq_pump,gq_allocate,gq_clean,gq_asset,GW_IOVA+GW_ALLOC_START,GW_ARENA_BYTES-GW_ALLOC_START,gw_now,gq_power};
    r->cio=(struct gwc_io){r,gc_read,gc_write,gm_ready,gm_sync,gc_receive,r->memory+GW_CE_OFFSET,GW_IOVA+GW_CE_OFFSET};
    r->wio=(struct gww_io){r,gw_send,gw_pump,gw_now,gw_notice};
    r->stage=4;r->error_domain="MHI";r->operation="mhi-probe";
    e=gwm_probe(&r->mio,&r->mhi);
    IOLog("GIULIA MHI: probe result=%d soc=0x%x major=%u bhi=0x%x bhie=0x%x ee=%u\n",
        e,r->mhi.soc,r->mhi.major,r->mhi.bhi,r->mhi.bhie,r->mhi.ee);
    if(e)return e;
    r->error_domain="WIFI_SETUP";r->operation="firmware-select";
    struct gwf_asset firmware;if(!gwf_main(&r->firmware,r->mhi.major,&firmware))return 8;
    r->error_domain="MHI";r->operation="mhi-boot";
    e=gwm_boot(&r->mio,&r->mhi,firmware.data,firmware.bytes,(uint32_t)mach_absolute_time());
    IOLog("GIULIA MHI: boot result=%d stage=%u bhi_status=0x%x bhie_status=0x%x ee=%u bus_master=%u quarantined=%u\n",
        e,r->mhi.stage,r->mhi.bhi_status,r->mhi.bhie_status,r->mhi.ee,r->mhi.bus_master,r->mhi.quarantined);
    if(e)return e;
    r->operation="mhi-start-channels";
    IOLog("GIULIA MHI: firmware mission entered_ms=%llu\n",(unsigned long long)gw_now(NULL));
    e=gwm_start_channels(&r->mio,&r->mhi);if(e)return e;
    r->error_domain="QMI";r->operation="qmi-initialize";
    uint64_t qmi_started=gw_now(NULL);
    IOLog("GIULIA QMI: host features=0x%llx qdss_configuration_supplied=0\n",
        (unsigned long long)GWQ_HOST_FEATURE_QDSS_CFG_MISS);
    r->stage=5;e=gwq_initialize(&r->qmi,&r->qio,r->mhi.major);
    IOLog("GIULIA QMI: initialize result=%d last_message=0x%x remote_error=%u node=0x%x port=0x%x memory_ready=%u provisioned=%u fw_ready=%u\n",
        e,r->qmi.last_message,r->qmi.remote_error,r->qmi.node,r->qmi.port,
        r->qmi.mem_ready,r->qmi.provisioned,r->qmi.fw_ready);
    IOLog("GIULIA QMI: identity cap_bytes=%u chip_present=%u chip_bytes=%u chip=0x%x family=0x%x board_present=%u board_bytes=%u board=0x%x board_default=%u soc=0x%x firmware=0x%x\n",
        r->qmi.cap_response_bytes,r->qmi.chip_info_present,r->qmi.chip_info_bytes,r->qmi.chip,r->qmi.chip_family,
        r->qmi.board_info_present,r->qmi.board_info_bytes,r->qmi.board,r->qmi.board_default,
        r->qmi.soc,r->qmi.firmware);
    IOLog("GIULIA QMI: PMU present=%u bytes=%u count=%u valid=%u bad_entry=%u\n",
        r->qmi.pmu_present,r->qmi.pmu_bytes,r->qmi.pmu_count,r->qmi.pmu_valid,r->qmi.pmu_bad_entry);
    if(r->qmi.pmu_valid)for(unsigned i=0;i<r->qmi.pmu_count && i<16;i++)
        IOLog("GIULIA QMI: PMU index=%u pin=%s wake_valid=%u wake_mv=%u sleep_valid=%u sleep_mv=%u\n",
            i,r->qmi.pmu[i].pin_name,r->qmi.pmu[i].wake_valid,r->qmi.pmu[i].wake_mv,
            r->qmi.pmu[i].sleep_valid,r->qmi.pmu[i].sleep_mv);
    IOLog("GIULIA QMI: indications count=%u last=0x%x unknown=%u last_unknown=0x%x mask=0x%llx elapsed_ms=%llu mhi_status=0x%x ee=0x%x\n",
        r->qmi.indication_count,r->qmi.last_indication,r->qmi.unknown_indications,r->qmi.last_unknown_indication,
        (unsigned long long)r->qmi.indication_mask,(unsigned long long)(gw_now(NULL)-qmi_started),
        gr32(r->bar,0x48),gr32(r->bar,r->mhi.bhi+0x28));
    IOLog("GIULIA QMI: timing timeout_phase=%u start_ms=%llu elapsed_ms=%llu host_cap_ms=%llu memory_ready_ms=%llu cap_ms=%llu m3_ms=%llu fw_ready_ms=%llu rpc_elapsed_ms=%llu\n",
        r->qmi.timeout_phase,(unsigned long long)r->qmi.mission_started_ms,(unsigned long long)r->qmi.elapsed_ms,
        (unsigned long long)r->qmi.host_cap_ms,(unsigned long long)r->qmi.memory_ready_ms,(unsigned long long)r->qmi.cap_ms,
        (unsigned long long)r->qmi.m3_ms,(unsigned long long)r->qmi.fw_ready_ms,(unsigned long long)r->qmi.rpc_elapsed_ms);
    IOLog("GIULIA QMI: platform configured=%u result=%d completed_ms=%llu filtered=%u last_src=0x%x:0x%x last_dst=0x%x:0x%x\n",
        r->qmi.pmu_configured,r->qmi.platform_error,(unsigned long long)r->qmi.pmu_config_ms,
        r->qmi.filtered_data_packets,r->qmi.filtered_src_node,r->qmi.filtered_src_port,r->qmi.filtered_dst_node,r->qmi.filtered_dst_port);
    gw_dma_snapshot(r,"after-qmi-initialize");
    if(e)return e;
    r->error_domain="CE";r->operation="ce-initialize";
    r->stage=6;e=gwc_init(&r->ce,&r->cio);if(e)return e;
    uint8_t pipes[180],services[168];gwc_qmi_tables(pipes,services);
    r->error_domain="QMI";r->operation="qmi-configure";
    e=gwq_configure(&r->qmi,&r->qio,pipes,9,services,14);if(e)return e;
    r->error_domain="WMI";r->operation="wmi-initialize";
    r->stage=7;e=gww_initialize(&r->wmi,&r->wio,r->mac);if(e)return e;
    r->stage=8;return 0;
}
static void gw_shutdown(giulia_wifi_runtime *r) {
    r->ready=0;r->powered=0;
    /* DMA uses copies in the owned arena, never this original pack mapping. */
    r->firmware.valid=0;OSSafeReleaseNULL(r->firmware_map);OSSafeReleaseNULL(r->firmware_memory);
    if(r->ce.active && !gwc_stop(&r->ce,&r->cio))r->quarantine=1;
    if(r->mhi.bus_master && !gwm_stop(&r->mio,&r->mhi))r->quarantine=1;
    if(r->pci.ready && !gwp_master(&r->pio,&r->pci,0,0))r->quarantine=1;
    if(r->quarantine || r->mhi.quarantined)return; /* Never free/retarget outstanding DMA. */
    gw_qmp_stop(r);
    if(r->msi.changed&&!gwp_msi_restore(&r->pio,&r->msi))r->quarantine=1;
    if(r->translation.changed&&gws_restore(&r->sio,&r->translation))r->quarantine=1;
    if(r->sid.changed&&!gwp_sid_restore(&r->pio,&r->sid))r->quarantine=1;
    if(r->pci.changed&&!gwp_restore(&r->pio,&r->pci))r->quarantine=1;
    if(!r->quarantine){OSSafeReleaseNULL(r->arena);OSSafeReleaseNULL(r->tables);r->memory=NULL;}
}
static void gw_worker(void *v,wait_result_t) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;r->error=(unsigned)gw_bringup(r);
    if(!r->error){r->error_domain="WIFI_SETUP";r->operation="publish-interface";
        r->ready=1;r->powered=1;if(!r->owner->publishInterface())r->error=9;}
    if(!r->error)IOLog("GIULIA WIFI: PCIe/MHI/QMI/WMI ready; en0 passive scan service published\n");
    while(!r->error) {
        r->error_domain="TRANSPORT";r->operation="transport-poll";
        IOLockLock(r->lock);
        if(r->scan_pending){r->scan_pending=0;int e=gww_scan(&r->wmi,&r->wio);if(e)gw_notice(r,(unsigned)e,r->wmi.scan_id);}
        r->error=(unsigned)gw_pump(r,0);gww_tick(&r->wmi,&r->wio);IOLockUnlock(r->lock);IOSleep(5);
    }
    IOLog("GIULIA WIFI: failure stage=%u error_domain=%s error_code=%u operation=%s pci_ready=%u bar=0x%llx bytes=0x%llx\n",
        r->stage,r->error_domain,r->error,r->operation,r->pci.ready,
        (unsigned long long)r->pci.bar_base,(unsigned long long)r->pci.bar_bytes);
    IOLockLock(r->lock);gw_shutdown(r);IOLockUnlock(r->lock);
    if(r->stage==9)
        IOLog("GIULIA WIFI: stopped stage=9 error_domain=XNU_PCIE error_code=%u error_name=%s quarantine=%u\n",
            r->error,gwl_error_name(r->error),r->quarantine);
    else IOLog("GIULIA WIFI: stopped stage=%u error=%u MHI=%u QMI=%u CE=%u WMI=%u quarantine=%u\n",
        r->stage,r->error,r->mhi.error,r->qmi.error,r->ce.error,r->wmi.error,r->quarantine);
    r->owner->setProperty("GiuliaWiFiError",r->error,32);r->owner->setProperty("GiuliaWiFiStage",r->stage,32);
    r->owner->release();thread_terminate(current_thread());
}
bool IO80211Controller::start(IOService *provider) {
    r=NULL;if(!IONetworkController::start(provider))return false;
    unsigned handoff=(unsigned)gw_handoff();
    if(!handoff){IOLog("GIULIA WIFI: no valid enumerated/prepared handoff; PCIe resource ownership unavailable\n");return false;}
    r=(giulia_wifi_runtime *)IOMallocZero(sizeof(*r));if(!r)return false;r->owner=this;r->lock=IOLockAlloc();if(!r->lock)return false;
    r->handoff_mode=handoff;
    IORegistryEntry *chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);if(!chosen)return false;
    OSData *mac=OSDynamicCast(OSData,chosen->getProperty("mac-address-wifi0"));
    bool valid=mac&&mac->getLength()==6;if(valid)memcpy(r->mac,mac->getBytesNoCopy(),6);chosen->release();if(!valid)return false;
    retain();thread_t t; kern_return_t kr=kernel_thread_start(gw_worker,r,&t);
    if(kr!=KERN_SUCCESS){release();return false;}thread_deallocate(t);return true;
}
void IO80211Controller::free(void) {
    /* An uncertain QMP channel keeps its callback context/owner token alive,
     * independently of the PCIe arena already reclaimed by gw_shutdown. */
    if(r && !r->quarantine && !r->mhi.quarantined && !r->interface && !r->qmp_retained) {
        OSSafeReleaseNULL(r->arena);OSSafeReleaseNULL(r->tables);
        OSSafeReleaseNULL(r->firmware_map);OSSafeReleaseNULL(r->firmware_memory);
        if(r->lock)IOLockFree(r->lock);IOFree(r,sizeof(*r));r=NULL;
    }
    IONetworkController::free();
}
static void gn_detached(ifnet_t interface) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)ifnet_softc(interface);
    r->interface=NULL;ifnet_release(interface);r->owner->release();
}
static errno_t gn_output(ifnet_t,mbuf_t packet){mbuf_freem(packet);return ENETDOWN;}
static errno_t gn_demux(ifnet_t,mbuf_t,char *,protocol_family_t *){return EOPNOTSUPP;}
static errno_t gn_add(ifnet_t,protocol_family_t,const struct ifnet_demux_desc *,u_int32_t){return EOPNOTSUPP;}
static errno_t gn_del(ifnet_t,protocol_family_t){return 0;}
static errno_t gn_multi(ifnet_t,const struct sockaddr *){return EOPNOTSUPP;}
static errno_t gn_ioctl(ifnet_t,unsigned long command,void *data) {
    if(command==SIOCGIFMEDIA){struct ifmediareq *m=(struct ifmediareq *)data;
        m->ifm_current=IFM_IEEE80211|IFM_AUTO;m->ifm_active=m->ifm_current;m->ifm_status=IFM_AVALID;m->ifm_count=0;return 0;}
    if(command==SIOCSIFFLAGS)return 0;return EOPNOTSUPP;
}
bool IO80211Controller::publishInterface() {
    static const uint8_t broadcast[6]={255,255,255,255,255,255};struct ifnet_init_eparams p;bzero(&p,sizeof(p));
    p.ver=IFNET_INIT_CURRENT_VERSION;p.len=sizeof(p);p.flags=IFNET_INIT_LEGACY;p.name="en";p.unit=0;p.family=IFNET_FAMILY_ETHERNET;
    p.type=IFT_ETHER;p.subfamily=IFNET_SUBFAMILY_WIFI;p.output=gn_output;p.demux=gn_demux;p.add_proto=gn_add;p.del_proto=gn_del;
    p.check_multi=gn_multi;p.ioctl=gn_ioctl;p.detach=gn_detached;p.softc=r;p.broadcast_addr=broadcast;p.broadcast_len=6;
    if(ifnet_allocate_extended(&p,&r->interface))return false;
    struct sockaddr_dl address;bzero(&address,sizeof(address));address.sdl_len=sizeof(address);address.sdl_family=AF_LINK;address.sdl_type=IFT_ETHER;
    address.sdl_alen=6;memcpy(address.sdl_data,r->mac,6);
    if(ifnet_attach(r->interface,&address)){ifnet_release(r->interface);r->interface=NULL;return false;}
    retain(); /* ifnet callbacks keep this provider alive until gn_detached. */
    (void)ifnet_set_lladdr(r->interface,r->mac,6);(void)ifnet_set_mtu(r->interface,1500);
    (void)ifnet_set_flags(r->interface,IFF_BROADCAST|IFF_SIMPLEX|IFF_MULTICAST,0xffff);
    setProperty(kIOUserClientClassKey,"GiuliaWiFiUserClient");setProperty("IOInterfaceName","en0");setProperty("IOBSDName","en0");
    setProperty("IOMACAddress",r->mac,6);setProperty("IOInterfaceUnit",0ULL,32);setProperty("IOInterfaceType",IFT_ETHER,32);
    setProperty("IO80211InterfaceRole","Infrastructure");setProperty("IO80211CountryCode","CN");setProperty("GiuliaPassiveScan",true);
    IO80211Interface *interface=new IO80211Interface;
    if(!interface || !interface->init() || !interface->attach(this) || !interface->start(this)){OSSafeReleaseNULL(interface);return false;}
    interface->setProperty("IOBSDName","en0");interface->setProperty("IOInterfaceName","en0");interface->setProperty("IOInterfaceUnit",0ULL,32);
    interface->setProperty("IOMACAddress",r->mac,6);interface->setProperty("IO80211InterfaceRole","Infrastructure");
    interface->setProperty(kIOUserClientClassKey,"GiuliaWiFiUserClient");interface->registerService();interface->release();registerService();return true;
}
IOReturn IO80211Controller::query(const struct gwi_request &q,void *output,size_t *bytes,unsigned *cursor) {
    if(!r || !bytes || !cursor)return kIOReturnBadArgument;
    IOReturn result=kIOReturnSuccess;IOLockLock(r->lock);
    if(!r->ready){result=kIOReturnNotReady;goto done;}
    if(q.set) {
        if(q.type==10) {
            if(!gwi_scan_request(&q)){result=kIOReturnBadArgument;goto done;}
            if(!r->powered){result=kIOReturnNotReady;goto done;}
            if(r->scan_pending||r->wmi.scanning){result=kIOReturnBusy;goto done;}
            r->scan_pending=1;*cursor=0;
        } else if(q.type==19) {
            if(q.bytes!=24 || gwq_u32(q.data)!=1 || gwq_u32(q.data+4)!=1 || gwq_u32(q.data+8)>1){result=kIOReturnBadArgument;goto done;}
            if(!gwq_u32(q.data+8) && (r->scan_pending||r->wmi.scanning)){result=kIOReturnBusy;goto done;}
            r->powered=gwq_u32(q.data+8);
        } else {result=kIOReturnUnsupported;goto done;}
        /* The framework reuses the SET buffer afterwards. Preserve the
         * validated input and zero any extra output, as an in-place ioctl. */
        if(output&&*bytes){bzero(output,*bytes);size_t copy=q.bytes<*bytes?q.bytes:*bytes;if(copy)memcpy(output,q.data,copy);}goto done;
    }
    if(!output || !*bytes){result=kIOReturnBadArgument;goto done;}
    bzero(output,*bytes);
    switch(q.type) {
    case 11:
        if(r->scan_pending||r->wmi.scanning){result=kIOReturnBusy;break;}
        if(*cursor>=r->wmi.bss_count){result=(IOReturn)0xe0820446u;break;}
        if(!gwi_result(output,*bytes,&r->wmi.bss[*cursor],gw_now(NULL))){result=kIOReturnBadArgument;break;}
        *bytes=GWI_SCAN_RESULT_BYTES;(*cursor)++;break;
    case 12: if(*bytes!=13)result=kIOReturnBadArgument;break; /* no unimplemented feature bits */
    case 13: if(*bytes!=4){result=kIOReturnBadArgument;break;}gwq_w32(output,r->wmi.scanning?1:0);break;
    case 14: if(*bytes!=12){result=kIOReturnBadArgument;break;}gwq_w32(output,1);gwq_w32((uint8_t *)output+4,1);gwq_w32((uint8_t *)output+8,0x1e);break;
    case 15: if(*bytes!=4){result=kIOReturnBadArgument;break;}gwq_w32(output,1);break;
    case 19: if(*bytes!=24){result=kIOReturnBadArgument;break;}gwq_w32(output,1);gwq_w32((uint8_t *)output+4,1);gwq_w32((uint8_t *)output+8,r->powered);break;
    case 23: strlcpy((char *)output,"OnePlus Ace 5 WCN7850",*bytes);break;
    case 27: if(!gwi_channels(output,*bytes))result=kIOReturnBadArgument;break;
    case 51: if(*bytes!=3){result=kIOReturnBadArgument;break;}memcpy(output,"CN",3);break;
    case 43: case 44: strlcpy((char *)output,"Giulia WCN7850 iOS16 passive-scan 1",*bytes);break;
    default:result=kIOReturnUnsupported;break;
    }
done:IOLockUnlock(r->lock);return result;
}
class GiuliaWiFiUserClient : public IOUserClient {
    OSDeclareDefaultStructors(GiuliaWiFiUserClient);
    unsigned cursor;
public:
    bool initWithTask(task_t,void *,UInt32) APPLE_KEXT_OVERRIDE;
    IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(uint32_t,IOExternalMethodArguments *,IOExternalMethodDispatch *,OSObject *,void *) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(GiuliaWiFiUserClient,IOUserClient);
bool GiuliaWiFiUserClient::initWithTask(task_t t,void *security,UInt32 type){cursor=0;return type==0&&IOUserClient::initWithTask(t,security,type);}
IOReturn GiuliaWiFiUserClient::clientClose(void){terminate();return kIOReturnSuccess;}
IOReturn GiuliaWiFiUserClient::externalMethod(uint32_t selector,IOExternalMethodArguments *a,IOExternalMethodDispatch *,OSObject *,void *) {
    if(!a || a->scalarInputCount || a->scalarOutputCount)return kIOReturnBadArgument;
    if(selector==1 || selector==5) {
        if(a->structureInputSize || a->structureInputDescriptor || a->structureOutputSize!=1 || !a->structureOutput || a->structureOutputDescriptor)return kIOReturnBadArgument;
        *(uint8_t *)a->structureOutput=selector==1?1:0;return kIOReturnSuccess; /* verified PF_SYSTEM notification fallback */
    }
    if(selector!=0)return kIOReturnUnsupported;
    size_t n=a->structureInputDescriptor?a->structureInputDescriptor->getLength():a->structureInputSize;
    if((n!=GWI_GET_BYTES&&n!=GWI_SET_BYTES) || a->structureOutputSize>0x3c000)return kIOReturnBadArgument;
    uint8_t *input=(uint8_t *)IOMallocData(n);if(!input)return kIOReturnNoMemory;
    if(a->structureInputDescriptor) {
        if(a->structureInputDescriptor->readBytes(0,input,n)!=n){IOFreeData(input,n);return kIOReturnBadArgument;}
    }else if(a->structureInput)memcpy(input,a->structureInput,n);else{IOFreeData(input,n);return kIOReturnBadArgument;}
    struct gwi_request q;if(!gwi_decode(input,n,&q)){IOFreeData(input,n);return kIOReturnBadArgument;}
    IO80211Controller *provider=OSDynamicCast(IO80211Controller,getProvider());
    if(!provider && getProvider())provider=OSDynamicCast(IO80211Controller,getProvider()->getProvider());
    size_t out=a->structureOutputDescriptor?a->structureOutputDescriptor->getLength():a->structureOutputSize;
    if(out>0x3c000){IOFreeData(input,n);return kIOReturnBadArgument;}
    uint8_t *buffer=out?(uint8_t *)IOMallocZeroData(out):NULL;
    if(out&&!buffer){IOFreeData(input,n);return kIOReturnNoMemory;}
    size_t used=out;IOReturn result=provider?provider->query(q,buffer,&used,&cursor):kIOReturnNotReady;
    if(result==kIOReturnSuccess && used) {
        if(a->structureOutputDescriptor){if(a->structureOutputDescriptor->writeBytes(0,buffer,used)!=used)result=kIOReturnBadArgument;}
        else if(a->structureOutput)memcpy(a->structureOutput,buffer,used);else result=kIOReturnBadArgument;
    }
    a->structureOutputSize=(uint32_t)used;if(buffer)IOFreeData(buffer,out);IOFreeData(input,n);return result;
}
#endif
