/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Darwin 22 A750 bring-up service. giulia_gpu=1 prepares pinned firmware and
 * host memory; =2 additionally observes gated power/SMMU registers.
 * No GPU transactions, SMMU writes, secure calls or accelerator publication.
 */
#include <pexpert/arm64/board_config.h>
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <mach-o/loader.h>
#include <pexpert/pexpert.h>
#include <libkern/c++/OSData.h>
extern "C" { extern unsigned long gPhysBase, gPhysSize, gVirtBase; }
#include "giulia_gpu_pack.h"
#include "giulia_gpu_memory.h"
#include "giulia_gpu_power.h"
#include "vendor/ArmSmmu4K/ArmSmmu4K.h"

class GiuliaGPU : public IOService {
    OSDeclareDefaultStructors(GiuliaGPU)
    IOMemoryDescriptor *firmwareMemory;
    IOMemoryMap *firmwareMap;
    IOBufferMemoryDescriptor *tableMemory;
    IOBufferMemoryDescriptor *allocations[GG_FW_MAX_BLOCKS];
    IOMemoryDescriptor *registerMemory[GGP_WINDOWS];
    IOMemoryMap *registerMaps[GGP_WINDOWS];
    bool loadFirmware(struct gg_pack *pack);
    bool prepareMemory(const struct gg_pack *pack);
    void releaseResources();
    static int readRegister(void *, unsigned, uint32_t, uint32_t *);
public:
    bool init(OSDictionary *dictionary=0) override;
    bool start(IOService *) override;
    void stop(IOService *) override;
    void free() override;
};
OSDefineMetaClassAndStructors(GiuliaGPU, IOService)

bool GiuliaGPU::init(OSDictionary *dictionary)
{
    firmwareMemory=NULL; firmwareMap=NULL; tableMemory=NULL;
    bzero(allocations,sizeof(allocations));
    bzero(registerMemory,sizeof(registerMemory)); bzero(registerMaps,sizeof(registerMaps));
    return IOService::init(dictionary);
}

static bool gpuFirmwareError(unsigned code,uint64_t at,const char *reason,uint64_t expected,uint64_t observed)
{
    IOLog("GIULIA GPU: subsystem=firmware phase=validate result=fail code=%u location=%#llx check=%s expected=%#llx observed=%#llx device_writes=0 cause=unconfirmed\n",
        code,(unsigned long long)at,reason,(unsigned long long)expected,(unsigned long long)observed);
    return false;
}

bool GiuliaGPU::loadFirmware(struct gg_pack *pack)
{
    const struct mach_header_64 *h=&_mh_execute_header;
    struct segment_command_64 segment={}; bool found=false;
    if (h->magic!=MH_MAGIC_64) return gpuFirmwareError(1,0,"mach-magic",MH_MAGIC_64,h->magic);
    if (!h->ncmds || h->ncmds>128) return gpuFirmwareError(2,16,"command-count-range-1-to-128",128,h->ncmds);
    if (h->sizeofcmds>0x10000) return gpuFirmwareError(3,20,"command-table-size-limit",0x10000,h->sizeofcmds);
    const uint8_t *p=(const uint8_t *)(h+1); size_t left=h->sizeofcmds;
    for (unsigned i=0;i<h->ncmds;++i) {
        struct load_command c;
        if (left<sizeof(c)) return gpuFirmwareError(4,p-(const uint8_t *)h,"truncated-command",sizeof(c),left);
        memcpy(&c,p,sizeof(c));
        if (c.cmdsize<8 || c.cmdsize>left) return gpuFirmwareError(5,p-(const uint8_t *)h,"command-size-out-of-range",left,c.cmdsize);
        if (c.cmdsize&7) return gpuFirmwareError(6,p-(const uint8_t *)h,"command-size-alignment-remainder",0,c.cmdsize&7);
        if (c.cmd==LC_SEGMENT_64) {
            struct segment_command_64 s;
            if (c.cmdsize<sizeof(s)) return gpuFirmwareError(7,p-(const uint8_t *)h,"truncated-segment",sizeof(s),c.cmdsize);
            memcpy(&s,p,sizeof(s));
            if (!strncmp(s.segname,"__GPU_FW",16)) {
                if (found) return gpuFirmwareError(8,p-(const uint8_t *)h,"duplicate-GPU-segment",1,2);
                if (c.cmdsize!=sizeof(s) || s.nsects || s.flags) {
                    IOLog("GIULIA GPU: subsystem=firmware phase=segment-shape cmdsize=%u expected_cmdsize=%lu sections=%u flags=%#x\n",c.cmdsize,(unsigned long)sizeof(s),s.nsects,s.flags);
                    return gpuFirmwareError(9,p-(const uint8_t *)h,"GPU-segment-shape",sizeof(s),c.cmdsize);
                }
                if(s.initprot!=VM_PROT_READ || s.maxprot!=VM_PROT_READ) {
                    IOLog("GIULIA GPU: subsystem=firmware phase=segment-permissions initial=%#x maximum=%#x expected=1\n",s.initprot,s.maxprot);
                    return gpuFirmwareError(10,p-(const uint8_t *)h,"GPU-segment-not-read-only",1,s.initprot);
                }
                segment=s; found=true;
            }
        }
        p+=c.cmdsize; left-=c.cmdsize;
    }
    if (left) return gpuFirmwareError(11,0,"unconsumed-load-command-bytes",0,left);
    if (!found) return gpuFirmwareError(12,0,"GPU-firmware-segment-missing",1,0);
    if (segment.filesize!=GG_FIRMWARE_PACK_BYTES) return gpuFirmwareError(13,segment.vmaddr,"pack-size",GG_FIRMWARE_PACK_BYTES,segment.filesize);
    if (segment.vmsize<segment.filesize) return gpuFirmwareError(14,segment.vmaddr,"virtual-size-smaller-than-file",segment.filesize,segment.vmsize);
    if (segment.vmaddr<gVirtBase) return gpuFirmwareError(15,segment.vmaddr,"segment-below-kernel-window",gVirtBase,segment.vmaddr);
    if (segment.vmaddr-gVirtBase>gPhysSize) return gpuFirmwareError(16,segment.vmaddr,"segment-start-outside-window",gPhysSize,segment.vmaddr-gVirtBase);
    if (segment.vmsize>gPhysSize-(segment.vmaddr-gVirtBase)) return gpuFirmwareError(17,segment.vmaddr,"segment-end-outside-window",gPhysSize-(segment.vmaddr-gVirtBase),segment.vmsize);
    if (gPhysBase>UINT64_MAX-(segment.vmaddr-gVirtBase)) return gpuFirmwareError(18,segment.vmaddr,"physical-address-overflow",UINT64_MAX-(segment.vmaddr-gVirtBase),gPhysBase);
    uint64_t pa=gPhysBase+segment.vmaddr-gVirtBase;
    firmwareMemory=IOMemoryDescriptor::withPhysicalAddress(pa,segment.filesize,kIODirectionIn);
    if (!firmwareMemory) return gpuFirmwareError(19,pa,"physical-memory-descriptor-null",segment.filesize,0);
    firmwareMap=firmwareMemory->map(kIOMapReadOnly|kIOMapCopybackCache);
    if (!firmwareMap) return gpuFirmwareError(20,pa,"read-only-RAM-map-null",segment.filesize,0);
    if (!gg_pack_open((const void *)firmwareMap->getVirtualAddress(),segment.filesize,pack)) {
        unsigned char digest[32];char actual[65],expected[65];
        gwh_sha256((const void *)firmwareMap->getVirtualAddress(),segment.filesize,digest);
        for(unsigned i=0;i<32;++i) {snprintf(actual+2*i,3,"%02x",digest[i]);snprintf(expected+2*i,3,"%02x",gg_firmware_pack_sha256[i]);}
        IOLog("GIULIA GPU: subsystem=firmware phase=pack-validation result=fail expected_sha256=%s observed_sha256=%s digest_match=%u\n",expected,actual,gwh_equal(digest,gg_firmware_pack_sha256));
        return gpuFirmwareError(21,pa,"container-validation-failed",GG_FIRMWARE_PACK_BYTES,segment.filesize);
    }
    IOLog("GIULIA GPU: firmware pack mapped read-only and digest verified\n");
    return true;
}

bool GiuliaGPU::prepareMemory(const struct gg_pack *pack)
{
    /* Keep large parsing scratch off the kernel stack. */
    struct gg_fw_block *blocks=(struct gg_fw_block *)IOMalloc(sizeof(*blocks)*GG_FW_MAX_BLOCKS);
    struct gg_reservation *plan=(struct gg_reservation *)IOMalloc(sizeof(*plan)*GG_FW_MAX_BLOCKS);
    ArmSmmu4KRange *ranges=(ArmSmmu4KRange *)IOMalloc(sizeof(*ranges)*GG_FW_MAX_BLOCKS);
    size_t count=0, reservations=0; bool ok=false;
    int prepared=0;
    if (!blocks || !plan || !ranges) {
        IOLog("GIULIA GPU: subsystem=memory phase=allocate-plan-scratch result=fail blocks_allocated=%u reservations_allocated=%u ranges_allocated=%u capacity=%u cause=unconfirmed\n",
            blocks!=NULL,plan!=NULL,ranges!=NULL,GG_FW_MAX_BLOCKS);
    } else {
        enum gg_fw_status status=gg_fw_plan(pack->asset[0].data,pack->asset[0].size,blocks,GG_FW_MAX_BLOCKS,&count);
        IOLog("GIULIA GPU: subsystem=gmu phase=parse-firmware code=%u firmware_bytes=%lu blocks=%lu capacity=%u\n",
            (unsigned)status,(unsigned long)pack->asset[0].size,(unsigned long)count,GG_FW_MAX_BLOCKS);
        if(status==GG_FW_OK) {
            status=gg_memory_plan(blocks,count,plan,GG_FW_MAX_BLOCKS,&reservations);
            IOLog("GIULIA GPU: subsystem=memory phase=plan-reservations code=%u blocks=%lu reservations=%lu capacity=%u\n",
                (unsigned)status,(unsigned long)count,(unsigned long)reservations,GG_FW_MAX_BLOCKS);
            prepared=status==GG_FW_OK;
        }
    }
    if (prepared) {
        ok=true;
        for (size_t i=0;i<reservations;++i) {
            size_t bytes=(plan[i].bytes+0x3fff)&~(size_t)0x3fff;
            allocations[i]=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
                kIODirectionInOut|kIOMemoryPhysicallyContiguous|kIOMapInhibitCache,bytes,0x4000);
            if (!allocations[i]) { IOLog("GIULIA GPU: subsystem=memory phase=allocate result=fail reservation=%lu iova=%#x allocation_bytes=%lu reason=descriptor-null\n",(unsigned long)i,plan[i].iova,(unsigned long)bytes); ok=false; break; }
            IOByteCount contiguous=0;
            uint64_t pa=allocations[i]->getPhysicalSegment(0,&contiguous,kIOMemoryMapperNone);
            void *cpu=allocations[i]->getBytesNoCopy();
            if (!cpu || contiguous<bytes || (pa&0x3fff) || pa<gPhysBase ||
                bytes>gPhysSize || pa-gPhysBase>gPhysSize-bytes) {
                IOLog("GIULIA GPU: subsystem=memory phase=validate result=fail reservation=%lu pa=%#llx requested=%lu contiguous=%llu cpu_mapped=%u reason=allocation-outside-owned-window-or-misaligned\n",(unsigned long)i,(unsigned long long)pa,(unsigned long)bytes,(unsigned long long)contiguous,cpu!=NULL);
                ok=false; break; }
            ranges[i].iova=plan[i].iova; ranges[i].physical=pa; ranges[i].bytes=plan[i].bytes;
            ranges[i].permissions=ArmSmmu4KRead|ArmSmmu4KWrite|ArmSmmu4KPrivileged|
                (plan[i].region==GG_ICACHE?ArmSmmu4KExecute:0);
            ranges[i].writeback=0; ranges[i].shareability=2;
            bzero(cpu,bytes);
            /* Prepare only ordinary host RAM. TCM stays untouched. */
            for (size_t j=0;j<count;++j) {
                const struct gg_fw_block *b=blocks+j;
                if (b->size && b->address>=plan[i].iova && b->size<=plan[i].bytes &&
                    b->address-plan[i].iova<=plan[i].bytes-b->size)
                    memcpy((uint8_t *)cpu+b->address-plan[i].iova,
                        pack->asset[0].data+b->payload_offset,b->size);
            }
            IOLog("GIULIA GPU: GMU RAM iova=%#x bytes=%#x persistent=%u; not mapped to GPU\n",
                plan[i].iova,plan[i].bytes,plan[i].persistent);
        }
        if (ok) {
            const size_t capacity=16, bytes=capacity*4096;
            tableMemory=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
                kIODirectionInOut|kIOMemoryPhysicallyContiguous|kIOMapInhibitCache,bytes,0x4000);
            IOByteCount contiguous=0;
            uint64_t pa=tableMemory?tableMemory->getPhysicalSegment(0,&contiguous,kIOMemoryMapperNone):0;
            if (!tableMemory || !tableMemory->getBytesNoCopy() || contiguous<bytes || (pa&0x3fff) ||
                pa<gPhysBase || bytes>gPhysSize || pa-gPhysBase>gPhysSize-bytes) {
                IOLog("GIULIA GPU: subsystem=smmu phase=table-allocation result=fail pa=%#llx requested=%lu contiguous=%llu reason=invalid-owned-allocation\n",(unsigned long long)pa,(unsigned long)bytes,(unsigned long long)contiguous);
                ok=false;
            } else {
                ArmSmmu4KConfig config={32,36,pa,1024}; ArmSmmu4KResult result;
                ArmSmmu4KStatus status=ArmSmmu4KBuild(&config,ranges,reservations,
                    (uint64_t *)tableMemory->getBytesNoCopy(),capacity,&result);
                IOLog("GIULIA GPU: subsystem=smmu phase=build-inactive-tables result=%s code=%u root_pa=%#llx capacity_pages=%lu table_pages=%lu leaf_pages=%lu input_bits=32 output_bits=36 active=0\n",
                    status==ArmSmmu4KSuccess?"ok":"fail",(unsigned)status,(unsigned long long)pa,
                    (unsigned long)capacity,(unsigned long)result.table_pages,(unsigned long)result.leaf_pages);
                ok=status==ArmSmmu4KSuccess;
                if (ok) setProperty("InactiveSmmuTablePages",(unsigned long long)result.table_pages,32);
            }
        }
        if (ok) {
            setProperty("GMUReservationCount",(unsigned long long)reservations,32);
            IOLog("GIULIA GPU: host preparation complete; blocks=%lu reservations=%lu; hardware inactive\n",
                (unsigned long)count,(unsigned long)reservations);
        }
    }
    if (blocks) IOFree(blocks,sizeof(*blocks)*GG_FW_MAX_BLOCKS);
    if (plan) IOFree(plan,sizeof(*plan)*GG_FW_MAX_BLOCKS);
    if (ranges) IOFree(ranges,sizeof(*ranges)*GG_FW_MAX_BLOCKS);
    return ok;
}

int GiuliaGPU::readRegister(void *cookie,unsigned window,uint32_t offset,uint32_t *value)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    const uint64_t bases[GGP_WINDOWS]={0x171000,0x3d90000,0x3da0000};
    const size_t sizes[GGP_WINDOWS]={0x1000,0x9800,0x1000};
    if (!value || window>=GGP_WINDOWS || (offset&3) || offset>sizes[window]-4) {
        IOLog("GIULIA GPU: subsystem=mmio phase=validate-read result=fail native_code=1 window=%u offset=%#x output_present=%u check=range-or-alignment\n",window,offset,value!=NULL);
        return 1;
    }
    /* Map lazily: a failed prerequisite cannot even map the later window. */
    if (!self->registerMaps[window]) {
        self->registerMemory[window]=IOMemoryDescriptor::withPhysicalAddress(bases[window],sizes[window],kIODirectionIn);
        if (!self->registerMemory[window]) {
            IOLog("GIULIA GPU: subsystem=mmio phase=create-descriptor result=fail native_code=2 base=%#llx bytes=%lu window=%u cause=unconfirmed\n",
                (unsigned long long)bases[window],(unsigned long)sizes[window],window);
            return 2;
        }
        self->registerMaps[window]=self->registerMemory[window]->map(kIOMapReadOnly|kIOMapInhibitCache);
        if (!self->registerMaps[window]) {
            IOLog("GIULIA GPU: subsystem=mmio phase=map-read-only result=fail native_code=3 base=%#llx bytes=%lu window=%u cause=unconfirmed\n",
                (unsigned long long)bases[window],(unsigned long)sizes[window],window);
            return 3;
        }
    }
    *value=*(volatile uint32_t *)(self->registerMaps[window]->getVirtualAddress()+offset);
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

bool GiuliaGPU::start(IOService *provider)
{
    int mode=0;
    if (!PE_parse_boot_argn("giulia_gpu",&mode,sizeof(mode)) || mode<1 || mode>2) return false;
    IORegistryEntry *root=IORegistryEntry::fromPath("/",gIODTPlane);
    if (!root) return false;
    OSData *model=OSDynamicCast(OSData,root->getProperty("physical-model"));
    const char expected[]="OnePlus Ace 5";
    bool board=model && model->getLength()==sizeof(expected) && !memcmp(model->getBytesNoCopy(),expected,sizeof(expected));
    root->release();
    if (!board || !IOService::start(provider)) return false;
    struct gg_pack pack={};
    if (!loadFirmware(&pack) || !prepareMemory(&pack)) {
        IOLog("GIULIA GPU: preparation failed; no device writes performed\n");
        releaseResources(); IOService::stop(provider); return false;
    }
    setProperty("GPUChipID",(unsigned long long)0x43051401,32);
    setProperty("GPUModel","Adreno750v2"); setProperty("HardwareReady",false);
    setProperty("BringupStage","host-memory-prepared");
    if (mode==2) {
        struct ggp_io io={this,readRegister}; struct ggp_report report;
        ggp_observe(&io,&report);
        setProperty("PowerObservation",&report,sizeof(report));
        setProperty("PowerObservationVersion",3ULL,32);
        IOLog("GIULIA GPU: subsystem=power-observation stage=%u valid_fields=%#x hardware_ready=0\n",report.stage,report.valid_fields);
        const char *const names[13]={"gcc_cfg_ahb","gcc_memnoc","gcc_dvm","cx_gdscr","cx_hw_ctrl","gpu_ahb",
            "smmu_vote_clock","gmu_clock","smmu_scr0","smmu_id0","smmu_id1","smmu_id2","smmu_fault"};
        const uint64_t addresses[13]={0x171004,0x171010,0x171018,0x3d99108,0x3d99168,0x3d9911c,
            0x3d97000,0x3d9913c,0x3da0000,0x3da0020,0x3da0024,0x3da0028,0x3da0048};
        const uint32_t values[13]={report.cfg,report.memnoc,report.dvm,report.cx,report.cx_hw,report.ahb,
            report.smmu_clock,report.gmu_clock,report.scr0,report.id0,report.id1,report.id2,report.fault};
        for(unsigned i=0;i<13;++i) {
            if(report.valid_fields&(1u<<i)) IOLog("GIULIA GPU: subsystem=power-observation register=%s address=%#llx observed=%#x valid=1\n",names[i],(unsigned long long)addresses[i],values[i]);
            else IOLog("GIULIA GPU: subsystem=power-observation register=%s address=%#llx observed=unavailable valid=0\n",names[i],(unsigned long long)addresses[i]);
        }
        if (report.compared) {
            const uint64_t bases[GGP_WINDOWS]={0x171000,0x3d90000,0x3da0000};
            IOLog("GIULIA GPU: subsystem=power-observation result=blocked stage=%u address=%#llx expected=%#x observed=%#x mask=%#x later_windows_skipped=1 cause=unconfirmed\n",
                report.stage,(unsigned long long)(bases[report.window]+report.offset),report.expected,report.observed,report.mask);
        } else if (report.stage==GGP_IO_ERROR) {
            IOLog("GIULIA GPU: subsystem=power-observation result=fail stage=%u window=%u offset=%#x native_code=%d observed=%#x observed_valid=%u cause=unconfirmed\n",
                report.stage,report.window,report.offset,report.io_error,report.observed,report.io_error==0);
        }
    }
    registerService();
    return true;
}

void GiuliaGPU::releaseResources()
{
    for (unsigned i=0;i<GGP_WINDOWS;++i) {
        OSSafeReleaseNULL(registerMaps[i]); OSSafeReleaseNULL(registerMemory[i]);
    }
    for (unsigned i=0;i<GG_FW_MAX_BLOCKS;++i) OSSafeReleaseNULL(allocations[i]);
    OSSafeReleaseNULL(tableMemory);
    OSSafeReleaseNULL(firmwareMap); OSSafeReleaseNULL(firmwareMemory);
}
void GiuliaGPU::stop(IOService *provider) { releaseResources(); IOService::stop(provider); }
void GiuliaGPU::free() { releaseResources(); IOService::free(); }
#endif
