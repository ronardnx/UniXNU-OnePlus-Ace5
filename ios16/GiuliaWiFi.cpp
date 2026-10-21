/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Ace 5 WCN7850 scan transport and iOS16 IO80211 compatibility service.
 * Ownership starts only after the checked UEFI v5 PCIe handoff succeeded.
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
#include "giulia_wifi_smmu.h"
#include "giulia_wifi_mhi.h"
#include "giulia_wifi_ce.h"
#include "giulia_wifi_apple.h"
#include "giulia_wifi_firmware.h"
#include "giulia_wifi_firmware_pin.h"

#define GW_ARENA_BYTES 0x4000000u
#define GW_IOVA 0x20000000ull
#define GW_CE_OFFSET 0x1200000u
#define GW_ALLOC_START 0x1400000u
class IO80211Controller;
struct giulia_wifi_runtime {
    IO80211Controller *owner;IOLock *lock;
    vm_offset_t root,config,parf,smmu,bar;
    IOBufferMemoryDescriptor *arena,*tables;
    uint8_t *memory;uint64_t arena_pa,table_pa;size_t allocated;
    struct gwp_state pci;struct gwp_sid_state sid;struct gwp_msi_state msi;
    struct gws_state translation;struct gwm_state mhi;struct gwq_state qmi;
    struct gwc_state ce;struct gww_state wmi;struct gwf_view firmware;
    struct gwp_io pio;struct gws_io sio;struct gwm_io mio;struct gwq_io qio;struct gwc_io cio;struct gww_io wio;
    ifnet_t interface;uint8_t mac[6];unsigned ready,powered,scan_pending,stage,error,quarantine;
};
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
static uint32_t gp_read(void *v,unsigned space,unsigned offset) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    vm_offset_t base=space==GWP_ENDPOINT?r->config:space==GWP_ROOT?r->root:space==GWP_ATU?r->root+0x1000:r->parf;
    return gr32(base,offset);
}
static void gp_write(void *v,unsigned space,unsigned offset,uint32_t n) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    vm_offset_t base=space==GWP_ENDPOINT?r->config:space==GWP_ROOT?r->root:space==GWP_ATU?r->root+0x1000:r->parf;
    gw32(base,offset,n);
}
static uint32_t gs_read(void *v,unsigned offset){return gr32(((giulia_wifi_runtime *)v)->smmu,offset);}
static void gs_write(void *v,unsigned offset,uint32_t n){gw32(((giulia_wifi_runtime *)v)->smmu,offset,n);}
static int gs_idle(void *v){giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;uint32_t n=gr32(r->config,4);return n!=0xffffffffu&&!(n&4);}
static int gs_owned(void *v,uint64_t pa,size_t n){giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    return (pa==r->arena_pa&&n==GW_ARENA_BYTES)||(pa==r->table_pa&&n==0x4000);}
/* CPU mappings for the arena/tables inhibit caching. The SMMU page walker
 * reads the same physical allocation. Barriers also constrain compiler loads. */
static void gs_clean(void *,void *,size_t){__asm__ volatile("dsb sy" ::: "memory");}
static int gm_ready(void *v,uint64_t pa,size_t n){giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    return pa>=GW_IOVA && n<=GW_ARENA_BYTES && pa-GW_IOVA<=GW_ARENA_BYTES-n &&
        r->sid.changed && r->msi.changed && gws_live(&r->sio,&r->translation);}
static int gm_master(void *v,int on){giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    return gwp_master(&r->pio,&r->pci,on,!on||gm_ready(v,GW_IOVA,GW_ARENA_BYTES));}
static uint32_t gm_read(void *v,uint32_t o){return gr32(((giulia_wifi_runtime *)v)->bar,o);}
static void gm_write(void *v,uint32_t o,uint32_t n){gw32(((giulia_wifi_runtime *)v)->bar,o,n);}
static void gm_sync(void *,size_t,size_t,int){__asm__ volatile("dsb sy" ::: "memory");}
static int gm_verify(void *v,const void *data,size_t n,unsigned major){giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;struct gwf_asset a;
    return gwf_main(&r->firmware,major,&a)&&a.bytes==n&&a.data==data;}
static void gm_receive(void *v,const void *p,size_t n){giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;(void)gwq_receive(&r->qmi,&r->qio,p,n);}
static int gq_send(void *v,const void *p,size_t n){giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    /* May run from the RX callback to acknowledge QRTR. Do not reenter MHI
     * polling before its current event has been retired. */
    return gwm_send(&r->mio,&r->mhi,p,n)!=GWM_OK;}
static int gq_pump(void *v,unsigned us){giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    int e=gwm_poll(&r->mio,&r->mhi,128);gw_delay(v,us);return e || r->qmi.error;}
static int gq_allocate(void *v,size_t n,uint64_t *pa,void **va){giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    size_t off=(r->allocated+0x3fff)&~(size_t)0x3fff;
    if(!n || off>GW_ARENA_BYTES || n>GW_ARENA_BYTES-off)return 1;
    *pa=GW_IOVA+off;*va=r->memory+off;r->allocated=off+n;return 0;}
static void gq_clean(void *,const void *,size_t){__asm__ volatile("dsb sy" ::: "memory");}
static int gq_asset(void *v,unsigned kind,unsigned major,uint32_t chip,uint32_t board,const void **data,size_t *n){
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;struct gwf_asset a;
    int ok=kind==GWQ_REGDB?gwf_get(&r->firmware,"kiwi/regdb.bin",&a):kind==GWQ_BOARD?gwf_board(&r->firmware,chip,board,&a):gwf_phy(&r->firmware,major,&a);
    if(!ok)return 1;*data=a.data;*n=a.bytes;return 0;}
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
    char text[225]={0};uint8_t bytes[168];
    if(!PE_parse_boot_argn("giulia_wifi5",text,sizeof(text)) || strlen(text)!=224)return 0;
    const char *alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for(unsigned i=0;i<56;i++){uint32_t n=0;for(unsigned j=0;j<4;j++){const char *c=strchr(alphabet,text[i*4+j]);if(!c)return 0;n=n<<6|(unsigned)(c-alphabet);}
        bytes[i*3]=(uint8_t)(n>>16);bytes[i*3+1]=(uint8_t)(n>>8);bytes[i*3+2]=(uint8_t)n;}
    return gwq_u32(bytes)==5 && gwq_u32(bytes+4)==16 && !gwq_u32(bytes+12) &&
        gwq_u32(bytes+32)==0x1ff && gwq_u32(bytes+36)==0xfff &&
        (gwq_u32(bytes+44)&(1u<<29)) && (gwq_u32(bytes+48)&(1u<<10)) &&
        gwq_u32(bytes+52)==0x110717cb && !(gwq_u32(bytes+56)&4);
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
    return gwf_open(&r->firmware,(void *)ml_static_ptovirt((vm_offset_t)physical),firmware.filesize,gw_firmware_pack_sha256)!=0;
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
    r->stage=1;if(!gw_firmware(r))return 1;
    r->root=ml_io_map(0x60000000,0x2000);r->config=ml_io_map(0x60100000,0x1000);
    r->parf=ml_io_map(0x1c00000,0x3000);r->smmu=ml_io_map(0x15000000,0x100000);
    if(!r->root || !r->config || !r->parf || !r->smmu)return 2;
    r->pio=(struct gwp_io){r,gp_read,gp_write,gw_delay};r->sio=(struct gws_io){r,gs_read,gs_write,gw_delay,gs_idle,gs_owned,gs_clean};
    r->stage=2;
    if(!(gr32(r->root,0x80)&(1u<<29)) || !(gr32(r->root,0xf28)&(1u<<10)))return 3;
    int e=gwp_enumerate(&r->pio,&r->pci,0x60300000,0x3d00000);if(e)return e;
    if(r->pci.bar_bytes<0x100000)return GWP_BAD_BAR;
    r->bar=ml_io_map((vm_offset_t)r->pci.bar_base,(vm_size_t)r->pci.bar_bytes);if(!r->bar)return 4;
    r->stage=3;if(!gw_allocate(r))return 5;
    if(!gwp_sid_claim(&r->pio,&r->sid))return 6;
    e=gws_take(&r->sio,&r->translation,(uint64_t *)r->tables->getBytesNoCopy(),r->table_pa,r->arena_pa,GW_IOVA,GW_ARENA_BYTES);if(e)return e;
    if(!gwp_msi_sink(&r->pio,&r->pci,&r->msi,GW_IOVA,GW_ARENA_BYTES,GW_IOVA+GWM_MSI_SINK))return 7;
    r->mio=(struct gwm_io){r,gm_read,gm_write,gw_delay,gm_ready,gm_master,gm_verify,gm_sync,gm_receive,r->memory,GW_IOVA,GW_ARENA_BYTES,(uint32_t)r->pci.bar_bytes};
    r->qio=(struct gwq_io){r,gq_send,gq_pump,gq_allocate,gq_clean,gq_asset,GW_IOVA+GW_ALLOC_START,GW_ARENA_BYTES-GW_ALLOC_START};
    r->cio=(struct gwc_io){r,gc_read,gc_write,gm_ready,gm_sync,gc_receive,r->memory+GW_CE_OFFSET,GW_IOVA+GW_CE_OFFSET};
    r->wio=(struct gww_io){r,gw_send,gw_pump,gw_now,gw_notice};
    r->stage=4;e=gwm_probe(&r->mio,&r->mhi);if(e)return e;
    struct gwf_asset firmware;if(!gwf_main(&r->firmware,r->mhi.major,&firmware))return 8;
    e=gwm_boot(&r->mio,&r->mhi,firmware.data,firmware.bytes,(uint32_t)mach_absolute_time());if(e)return e;
    e=gwm_start_channels(&r->mio,&r->mhi);if(e)return e;
    r->stage=5;e=gwq_initialize(&r->qmi,&r->qio,r->mhi.major);if(e)return e;
    r->stage=6;e=gwc_init(&r->ce,&r->cio);if(e)return e;
    uint8_t pipes[180],services[168];gwc_qmi_tables(pipes,services);
    e=gwq_configure(&r->qmi,&r->qio,pipes,9,services,14);if(e)return e;
    r->stage=7;e=gww_initialize(&r->wmi,&r->wio,r->mac);if(e)return e;
    r->stage=8;return 0;
}
static void gw_shutdown(giulia_wifi_runtime *r) {
    r->ready=0;r->powered=0;
    if(r->ce.active && !gwc_stop(&r->ce,&r->cio))r->quarantine=1;
    if(r->mhi.bus_master && !gwm_stop(&r->mio,&r->mhi))r->quarantine=1;
    if(r->pci.ready && !gwp_master(&r->pio,&r->pci,0,0))r->quarantine=1;
    if(r->quarantine || r->mhi.quarantined)return; /* Never free/retarget outstanding DMA. */
    if(r->msi.changed&&!gwp_msi_restore(&r->pio,&r->msi))r->quarantine=1;
    if(r->translation.changed&&gws_restore(&r->sio,&r->translation))r->quarantine=1;
    if(r->sid.changed&&!gwp_sid_restore(&r->pio,&r->sid))r->quarantine=1;
    if(r->pci.changed&&!gwp_restore(&r->pio,&r->pci))r->quarantine=1;
    if(!r->quarantine){OSSafeReleaseNULL(r->arena);OSSafeReleaseNULL(r->tables);r->memory=NULL;}
}
static void gw_worker(void *v,wait_result_t) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;r->error=(unsigned)gw_bringup(r);
    if(!r->error){r->ready=1;r->powered=1;if(!r->owner->publishInterface())r->error=9;}
    if(!r->error)IOLog("GIULIA WIFI: PCIe/MHI/QMI/WMI ready; en0 passive scan service published\n");
    while(!r->error) {
        IOLockLock(r->lock);
        if(r->scan_pending){r->scan_pending=0;int e=gww_scan(&r->wmi,&r->wio);if(e)gw_notice(r,(unsigned)e,r->wmi.scan_id);}
        r->error=(unsigned)gw_pump(r,0);gww_tick(&r->wmi,&r->wio);IOLockUnlock(r->lock);IOSleep(5);
    }
    IOLockLock(r->lock);gw_shutdown(r);IOLockUnlock(r->lock);
    IOLog("GIULIA WIFI: stopped stage=%u error=%u MHI=%u QMI=%u CE=%u WMI=%u quarantine=%u\n",
        r->stage,r->error,r->mhi.error,r->qmi.error,r->ce.error,r->wmi.error,r->quarantine);
    r->owner->setProperty("GiuliaWiFiError",r->error,32);r->owner->setProperty("GiuliaWiFiStage",r->stage,32);
    r->owner->release();thread_terminate(current_thread());
}
bool IO80211Controller::start(IOService *provider) {
    r=NULL;if(!IONetworkController::start(provider))return false;
    if(!gw_handoff()){IOLog("GIULIA WIFI: PCIe handoff not ready; runtime stays inactive\n");return false;}
    r=(giulia_wifi_runtime *)IOMallocZero(sizeof(*r));if(!r)return false;r->owner=this;r->lock=IOLockAlloc();if(!r->lock)return false;
    IORegistryEntry *chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);if(!chosen)return false;
    OSData *mac=OSDynamicCast(OSData,chosen->getProperty("mac-address-wifi0"));
    bool valid=mac&&mac->getLength()==6;if(valid)memcpy(r->mac,mac->getBytesNoCopy(),6);chosen->release();if(!valid)return false;
    retain();thread_t t; kern_return_t kr=kernel_thread_start(gw_worker,r,&t);
    if(kr!=KERN_SUCCESS){release();return false;}thread_deallocate(t);return true;
}
void IO80211Controller::free(void) {
    if(r && !r->quarantine && !r->mhi.quarantined && !r->interface) {
        OSSafeReleaseNULL(r->arena);OSSafeReleaseNULL(r->tables);
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
