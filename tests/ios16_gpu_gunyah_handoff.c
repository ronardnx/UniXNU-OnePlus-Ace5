/* Real libfdt trees and production FdtLib extraction. No firmware/device I/O. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libfdt.h>
#include "../uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe/GpuGunyahReport.h"

enum { NATIVE,SECOND_RM,DUPLICATE_REG,DUPLICATE_COMPAT,BAD_COMPAT,NO_RM,
    BAD_PARENT,NO_PARENT,PARENT_ZERO,PARENT_MISSING,DUPLICATE_PHANDLE,
    BAD_GIC_CELLS,NO_GIC_CONTROLLER,EXTENDED_IRQ,RM_DISABLED,BUS_DISABLED,
    GIC_DISABLED,BAD_IRQ_TYPE,BAD_IRQ_FLAGS,BAD_IRQ_NUMBER,SAME_IRQ,
    SHORT_REG,LONG_REG,SHORT_IRQ,ZERO_CAP,EQUAL_CAP,INVALID_CAP,
    DIRECT_PARENT,OTHER_CAPS,BAD_BUS_CELLS };
static void prop_u32(void *fdt,const char *name,uint32_t value)
{assert(!fdt_property_u32(fdt,name,value));}
static unsigned make_tree(void *fdt,unsigned mode)
{
    assert(!fdt_create(fdt,8192));assert(!fdt_finish_reservemap(fdt));
    assert(!fdt_begin_node(fdt,""));
    if(mode!=NO_PARENT) {
        if(mode==BAD_PARENT)assert(!fdt_property(fdt,"interrupt-parent","bad",3));
        else prop_u32(fdt,"interrupt-parent",mode==PARENT_ZERO?0:mode==PARENT_MISSING?99:1);
    }
    assert(!fdt_begin_node(fdt,"soc"));
    prop_u32(fdt,"#address-cells",1);prop_u32(fdt,"#size-cells",1);
    assert(!fdt_begin_node(fdt,"interrupt-controller@17100000"));
    assert(!fdt_property_string(fdt,"compatible","arm,gic-v3"));
    prop_u32(fdt,"phandle",1);prop_u32(fdt,"#interrupt-cells",mode==BAD_GIC_CELLS?2:3);
    if(mode!=NO_GIC_CONTROLLER)assert(!fdt_property(fdt,"interrupt-controller","",0));
    if(mode==GIC_DISABLED)assert(!fdt_property_string(fdt,"status","disabled"));
    assert(!fdt_end_node(fdt));
    if(mode==DUPLICATE_PHANDLE) {
        assert(!fdt_begin_node(fdt,"other"));prop_u32(fdt,"phandle",1);assert(!fdt_end_node(fdt));
    }
    assert(!fdt_end_node(fdt));
    assert(!fdt_begin_node(fdt,"hypervisor"));
    prop_u32(fdt,"#address-cells",mode==BAD_BUS_CELLS?1:2);prop_u32(fdt,"#size-cells",0);
    assert(!fdt_property_string(fdt,"compatible","qcom,gunyah-hypervisor"));
    if(mode==BUS_DISABLED)assert(!fdt_property_string(fdt,"status","disabled"));
    assert(!fdt_begin_node(fdt,"resource-manager@dynamic"));
    const char *compatible=mode==NO_RM?"qcom,resource-manager":"gunyah-resource-manager";
    if(mode==BAD_COMPAT)assert(!fdt_property(fdt,"compatible",compatible,(int)strlen(compatible)));
    else assert(!fdt_property_string(fdt,"compatible",compatible));
    if(mode==DUPLICATE_COMPAT)assert(!fdt_property_string(fdt,"compatible",compatible));
    if(mode==RM_DISABLED)assert(!fdt_property_string(fdt,"status","disabled"));
    if(mode==DIRECT_PARENT)prop_u32(fdt,"interrupt-parent",1);
    uint64_t caps[]={cpu_to_fdt64(mode==ZERO_CAP?0:mode==OTHER_CAPS?UINT64_C(0xabcdef0123456789):UINT64_C(0x123456789abcdef0)),
        cpu_to_fdt64(UINT64_C(0xfedcba9876543210)),0};
    if(mode==EQUAL_CAP)caps[1]=caps[0];
    if(mode==INVALID_CAP)caps[1]=UINT64_MAX;
    int cap_bytes=mode==SHORT_REG?8:mode==LONG_REG?24:16;
    assert(!fdt_property(fdt,"reg",caps,cap_bytes));
    if(mode==DUPLICATE_REG)assert(!fdt_property(fdt,"reg",caps,cap_bytes));
    uint32_t irqs[]={cpu_to_fdt32(mode==BAD_IRQ_TYPE?1:0),cpu_to_fdt32(928),
        cpu_to_fdt32(mode==BAD_IRQ_FLAGS?2:1),0,cpu_to_fdt32(mode==SAME_IRQ?928:mode==BAD_IRQ_NUMBER?988:929),cpu_to_fdt32(4)};
    assert(!fdt_property(fdt,"interrupts",irqs,mode==SHORT_IRQ?12:24));
    if(mode==EXTENDED_IRQ)assert(!fdt_property(fdt,"interrupts-extended",irqs,24));
    assert(!fdt_end_node(fdt));
    if(mode==SECOND_RM) {
        assert(!fdt_begin_node(fdt,"resource-manager@second"));
        assert(!fdt_property_string(fdt,"compatible","gunyah-resource-manager"));
        assert(!fdt_property_string(fdt,"status","disabled"));assert(!fdt_end_node(fdt));
    }
    assert(!fdt_end_node(fdt));assert(!fdt_end_node(fdt));assert(!fdt_finish(fdt));
    return fdt_totalsize(fdt);
}
static void rejected(const void *fdt,unsigned bytes,uint64_t epoch)
{
    struct ggr_report out,saved;memset(&out,0x5b,sizeof(out));saved=out;
    assert(ggr_fdt_capture(fdt,bytes,epoch,&out)!=GGR_OK && !memcmp(&out,&saved,sizeof(out)));
}
static void fixtures(void)
{
    _Alignas(8) uint8_t blob[8192];struct ggr_report r;
    unsigned bytes=make_tree(blob,NATIVE);
    ggr_report_init(&r);assert(ggr_report_requested(&r) && !ggr_report_valid(&r));
    assert(ggr_fdt_capture(blob,bytes,1234,&r)==GGR_OK);
    assert(ggr_report_valid(&r) && !ggr_report_requested(&r));
    assert(r.epoch==1234 && r.tx_cap==UINT64_C(0x123456789abcdef0) &&
        r.rx_cap==UINT64_C(0xfedcba9876543210) && r.tx_irq[1]==928 && r.rx_irq[1]==929);
    assert(r.source==GGR_SOURCE_BOOT_FDT && r.fdt_crc32==ggr_crc32(blob,bytes));
    assert((int)r.rm_node==fdt_path_offset(blob,"/hypervisor/resource-manager@dynamic"));
    assert((int)r.gic_node==fdt_path_offset(blob,"/soc/interrupt-controller@17100000"));
    for(unsigned i=0;i<sizeof(r);i++) {
        struct ggr_report corrupt=r;((uint8_t *)&corrupt)[i]^=1;assert(!ggr_report_valid(&corrupt));
    }
    struct ggr_report corrupt=r;corrupt.reserved[38]=1;ggr_report_seal(&corrupt);assert(!ggr_report_valid(&corrupt));
    corrupt=r;corrupt.source=99;ggr_report_seal(&corrupt);assert(!ggr_report_valid(&corrupt));
    rejected(blob,bytes,0);rejected(blob,bytes-1,1234);rejected(blob,39,1234);
    assert(ggr_fdt_capture(blob,bytes,1234,(struct ggr_report *)(blob+8))==GGR_ERR_ARGUMENT);
    _Alignas(struct ggr_report) uint8_t unaligned[257];
    memset(unaligned,0x3a,sizeof(unaligned));
    assert(ggr_fdt_capture(blob,bytes,1234,(struct ggr_report *)(unaligned+1))==GGR_ERR_ARGUMENT);
    for(unsigned i=0;i<sizeof(unaligned);i++)assert(unaligned[i]==0x3a);
    for(unsigned mode=SECOND_RM;mode<=BAD_BUS_CELLS;mode++) {
        bytes=make_tree(blob,mode);
        if(mode==ZERO_CAP || mode==DIRECT_PARENT || mode==OTHER_CAPS) {
            assert(ggr_fdt_capture(blob,bytes,1235,&r)==GGR_OK && ggr_report_valid(&r));
            if(mode==ZERO_CAP)assert(r.tx_cap==0);
            if(mode==OTHER_CAPS)assert(r.tx_cap==UINT64_C(0xabcdef0123456789));
        } else rejected(blob,bytes,1234);
    }
    /* Full FDT bounds, reservation map and structure validation precede all
     * property walks. Test malformed block layouts without dereferencing them. */
    const unsigned offsets[]={0,4,8,12,16,20,24,32,36};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
        bytes=make_tree(blob,NATIVE);memset(blob+offsets[i],0xff,4);rejected(blob,bytes,1234);
    }
    bytes=make_tree(blob,NATIVE);
    unsigned st=fdt_off_dt_struct(blob),ss=fdt_size_dt_struct(blob);
    memset(blob+st+ss-4,0,4);rejected(blob,bytes,1234);
    bytes=make_tree(blob,NATIVE);memset(blob+fdt_off_mem_rsvmap(blob),0xff,16);rejected(blob,bytes,1234);
}
static void le64(uint8_t *p,uint64_t x)
{for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(x>>(8*i));}
static void descriptor(uint8_t p[48],unsigned type,uint64_t address,uint64_t pages,uint64_t attr)
{memset(p,0,48);p[0]=(uint8_t)type;le64(p+8,address);le64(p+24,pages);le64(p+32,attr);}
static void coverage(void)
{
    uint8_t map[2][48];descriptor(map[0],0,0x1000,1,0);descriptor(map[1],4,0x2000,2,0x20000);
    assert(ggr_fdt_memory_covered(map,sizeof(map),48,0x1800,0x2000));
    assert(ggr_fdt_memory_covered(map,sizeof(map),48,0x1000,40));
    assert(!ggr_fdt_memory_covered(map,sizeof(map),48,0x800,40));
    assert(!ggr_fdt_memory_covered(map,sizeof(map),48,0x3ff0,40));
    descriptor(map[1],4,0x3000,2,0);assert(!ggr_fdt_memory_covered(map,sizeof(map),48,0x1800,0x2000));
    descriptor(map[1],4,0x1000,2,0);assert(!ggr_fdt_memory_covered(map,sizeof(map),48,0x1000,40));
    const unsigned bad[]={8,11,12,13,14,15,0xff};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        descriptor(map[1],bad[i],0x2000,2,0);
        assert(!ggr_fdt_memory_covered(map,sizeof(map),48,0x1800,0x2000));
    }
    descriptor(map[1],4,0x2000,2,0x2000);assert(!ggr_fdt_memory_covered(map,sizeof(map),48,0x1800,0x2000));
    descriptor(map[1],4,0x2001,2,0);assert(!ggr_fdt_memory_covered(map,sizeof(map),48,0x1800,0x2000));
    descriptor(map[1],4,0x2000,UINT64_MAX,0);assert(!ggr_fdt_memory_covered(map,sizeof(map),48,0x1000,40));
    assert(!ggr_fdt_memory_covered(map,sizeof(map)-1,48,0x1000,40));
    assert(!ggr_fdt_memory_covered(map,sizeof(map),39,0x1000,40));
    assert(!ggr_fdt_memory_covered(map,sizeof(map),48,UINT64_MAX-2,40));
}
struct epoch_fixture {uint64_t values[2];int status[2];unsigned calls;};
static int next_epoch(void *cookie,uint64_t *out)
{
    struct epoch_fixture *f=cookie;assert(f->calls<2);
    unsigned at=f->calls++;*out=f->values[at];return f->status[at];
}
static void epochs(void)
{
    uint64_t out=0x55;
    struct epoch_fixture f={{0,1},{1,1},0};
    assert(ggr_boot_epoch(next_epoch,&f,&out) && out==1 && f.calls==2);
    f=(struct epoch_fixture){{0,0},{1,1},0};out=0x55;
    assert(!ggr_boot_epoch(next_epoch,&f,&out) && out==0x55 && f.calls==2);
    f=(struct epoch_fixture){{UINT64_MAX,1},{1,1},0};
    assert(ggr_boot_epoch(next_epoch,&f,&out) && out==UINT64_MAX && f.calls==1);
    f=(struct epoch_fixture){{0,UINT64_MAX},{1,1},0};
    assert(ggr_boot_epoch(next_epoch,&f,&out) && out==UINT64_MAX && f.calls==2);
    f=(struct epoch_fixture){{27,1},{0,1},0};out=0x55;
    assert(!ggr_boot_epoch(next_epoch,&f,&out) && out==0x55 && f.calls==1);
    f=(struct epoch_fixture){{0,27},{1,0},0};
    assert(!ggr_boot_epoch(next_epoch,&f,&out) && out==0x55 && f.calls==2);
    f=(struct epoch_fixture){{0,1},{2,1},0};
    assert(!ggr_boot_epoch(next_epoch,&f,&out) && out==0x55 && f.calls==1);
    assert(!ggr_boot_epoch(NULL,&f,&out) && out==0x55 && f.calls==1);
    assert(!ggr_boot_epoch(next_epoch,&f,NULL) && f.calls==1);
}
int main(int argc,char **argv)
{
    fixtures();coverage();epochs();
    if(argc==2) {
        FILE *f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));
        long bytes=ftell(f);assert(bytes>0 && bytes<=GGR_MAX_FDT_BYTES);rewind(f);
        void *blob=malloc((size_t)bytes);assert(blob && fread(blob,1,(size_t)bytes,f)==(size_t)bytes);fclose(f);
        struct ggr_report r;int result=ggr_fdt_capture(blob,(unsigned)bytes,4567,&r);
        if(result!=GGR_OK)fprintf(stderr,"Actual FDT rejected: code=%d bytes=%ld\n",result,bytes);
        assert(result==GGR_OK && ggr_report_valid(&r));
        printf("Actual FDT capture: rm=%u gic=%u bytes=%u; no HVC or IRQ lease\n",r.rm_node,r.gic_node,r.fdt_bytes);free(blob);
    }
    puts("GPU Gunyah handoff: actual libfdt extraction, unique resources, GIC specs, bounded boot-local epoch, reserved bytes and CRC passed");
    return 0;
}
