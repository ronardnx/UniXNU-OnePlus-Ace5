#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/giulia_wifi_mapping.h"
#include "../ios16/giulia_wifi_link.h"
#include "../ios16/giulia_wifi_mhi.h"
#include "../ios16/giulia_wifi_ce.h"
#include "../ios16/giulia_wifi_qmp.h"

static struct gwmap_dma arena(void)
{
    struct gwmap_dma m;
    assert(gwmap_dma_init(&m,(uintptr_t)0x10000000,UINT64_C(0x8d0000000),GWMAP_IOVA,GWMAP_ARENA_BYTES));
    return m;
}
int main(void)
{
    unsigned cases=0;uint64_t pa,iova;size_t bytes,offset;uintptr_t cpu;
    for(unsigned i=0;i<GWL_REGIONS;i++) {
        assert(gwmap_resource(i,0,0,&pa,&bytes));
        assert(pa==gwl_bases[i] && bytes==gwl_sizes[i]);cases++;
    }
    assert(gwmap_resource(GWMAP_QMP,0,0,&pa,&bytes) && pa==QPT_MSGRAM_PA && bytes==QPT_MSGRAM_BYTES);
    assert(gwmap_resource(GWMAP_SMMU,0,0,&pa,&bytes) && pa==0x15000000 && bytes==0x100000);
    assert(gwmap_resource(GWMAP_IPCC,0,0,&pa,&bytes) && pa==0x406000 && bytes==0x1000);cases+=3;
    assert(!gwmap_resource(GWMAP_REGIONS,0,0,&pa,&bytes));
    assert(!gwmap_resource(GWMAP_ROOT,0,0,NULL,&bytes));
    assert(gwmap_resource(GWMAP_BAR,0x60400000,0x200000,&pa,&bytes) && pa==0x60400000 && bytes==0x200000);
    assert(gwmap_resource(GWMAP_BAR,0x63f00000,0x100000,&pa,&bytes));
    assert(!gwmap_resource(GWMAP_BAR,0x60000000,0x100000,&pa,&bytes));
    assert(!gwmap_resource(GWMAP_BAR,0x64000000,0x100000,&pa,&bytes));
    assert(!gwmap_resource(GWMAP_BAR,0x60300000,0x200000,&pa,&bytes));
    assert(!gwmap_resource(GWMAP_BAR,0x60400000,0x180000,&pa,&bytes));
    assert(!gwmap_resource(GWMAP_BAR,0x60400000,0x80000,&pa,&bytes));
    assert(!gwmap_resource(GWMAP_BAR,UINT64_MAX-0xfffff,0x100000,&pa,&bytes));cases+=10;
    for(unsigned width=1;width<=8;width*=2) {
        assert(gwmap_access(0x10000000,0x400,0x400-width,width,&cpu));
        assert(cpu==0x10000400-width);
        assert(!gwmap_access(0x10000000,0x400,0x400,width,&cpu));
        if(width>1)assert(!gwmap_access(0x10000000,0x400,1,width,&cpu));cases++;
    }
    assert(!gwmap_access(0x10000000,0x400,0,3,&cpu));
    assert(!gwmap_access(0x10000000,0x400,SIZE_MAX,4,&cpu));
    assert(!gwmap_access(UINTPTR_MAX-3,8,0,4,&cpu));
    assert(!gwmap_access(0,0x1000,0,4,&cpu));
    assert(!gwmap_access(0x10000001,0x400,0,4,&cpu));cases+=5;
    uint32_t mapped,desired;
    assert(gwmap_window(0x7fffc,4,0x100000,UINT32_MAX,&mapped,&desired) && mapped==0x7fffc);
    assert(gwmap_window(0x1b80000,4,0x100000,0x12345680,&mapped,&desired));
    assert(mapped==0x80000 && desired==0x523456b7);
    assert(gwmap_window(0x1fffffc,4,0x100000,0,&mapped,&desired));
    assert(mapped==0xffffc && desired==0x4000003f);
    assert(!gwmap_window(0x2000000,4,0x100000,0,&mapped,&desired));
    assert(!gwmap_window(0x1b80002,4,0x100000,0,&mapped,&desired));
    assert(!gwmap_window(0x80000,4,0x80000,0,&mapped,&desired));
    assert(!gwmap_window(0x80000,4,0x100000,UINT32_MAX,&mapped,&desired));
    assert(!gwmap_window(UINT32_MAX,4,0x100000,0,&mapped,&desired));cases+=8;
    assert(GWM_FIRMWARE+16u*1024*1024==GWMAP_CE_OFFSET && GWC_BYTES==GWMAP_CE_BYTES);
    struct gwmap_dma m=arena(),saved=m;
    assert(gwmap_dma_at(&m,0,1,&cpu,&pa,&iova));
    assert(cpu==m.cpu && pa==m.physical && iova==m.iova);
    assert(gwmap_dma_at(&m,m.bytes-1,1,&cpu,&pa,&iova));
    assert(gwmap_dma_from_cpu(&m,cpu,1,&offset) && offset==m.bytes-1);
    assert(gwmap_dma_from_physical(&m,pa,1,&offset) && offset==m.bytes-1);
    assert(gwmap_dma_from_iova(&m,iova,1,&offset) && offset==m.bytes-1);
    assert(!gwmap_dma_at(&m,m.bytes,1,NULL,NULL,NULL));
    assert(!gwmap_dma_at(&m,0,0,NULL,NULL,NULL));
    assert(!gwmap_dma_from_iova(&m,m.iova-1,1,&offset));
    assert(!gwmap_dma_from_iova(&m,m.iova+m.bytes,1,&offset));
    assert(!gwmap_dma_from_physical(&m,m.physical+m.bytes-1,2,&offset));
    assert(!gwmap_dma_from_cpu(&m,m.cpu+m.bytes-1,SIZE_MAX,&offset));cases+=12;
    assert(!gwmap_dma_init(&m,UINTPTR_MAX&~(uintptr_t)0x3fff,saved.physical,saved.iova,saved.bytes));
    assert(!gwmap_dma_init(&m,saved.cpu,GWMAP_PA_LIMIT,saved.iova,saved.bytes));
    assert(!gwmap_dma_init(&m,saved.cpu,saved.physical+4096,saved.iova,saved.bytes));
    assert(!gwmap_dma_init(&m,saved.cpu,saved.physical,0x3e000000,saved.bytes));
    assert(!gwmap_dma_init(&m,saved.cpu,saved.physical,0xfe000000,saved.bytes));
    assert(!gwmap_dma_init(&m,saved.cpu,saved.physical,saved.iova,saved.bytes-1));
    assert(!memcmp(&m,&saved,sizeof(m)));cases+=7;
    assert(gwmap_dma_tables_valid(&m,0x20000000,UINT64_C(0x8c0000000),GWMAP_TABLE_BYTES));
    assert(!gwmap_dma_tables_valid(&m,m.cpu,UINT64_C(0x8c0000000),GWMAP_TABLE_BYTES));
    assert(!gwmap_dma_tables_valid(&m,m.cpu-GWMAP_TABLE_BYTES/2,UINT64_C(0x8c0000000),GWMAP_TABLE_BYTES));
    assert(!gwmap_dma_tables_valid(&m,0x20000000,m.physical,GWMAP_TABLE_BYTES));
    assert(!gwmap_dma_tables_valid(&m,0x20000000,GWMAP_PA_LIMIT,GWMAP_TABLE_BYTES));
    assert(!gwmap_dma_tables_valid(&m,0x20001000,UINT64_C(0x8c0000000),GWMAP_TABLE_BYTES));
    assert(!gwmap_dma_tables_valid(&m,0x20000000,UINT64_C(0x8c0001000),GWMAP_TABLE_BYTES));
    assert(!gwmap_dma_tables_valid(&m,0x20000000,UINT64_C(0x8c0000000),GWMAP_TABLE_BYTES-1));cases+=8;
    assert(gwmap_dma_slice_offset(GWMAP_DMA_CE,0,GWC_BYTES,&offset) && offset==GWMAP_CE_OFFSET);
    assert(gwmap_dma_slice_offset(GWMAP_DMA_MHI,GWM_FIRMWARE,16u*1024*1024,&offset) && offset==GWM_FIRMWARE);
    assert(!gwmap_dma_slice_offset(GWMAP_DMA_MHI,GWMAP_CE_OFFSET,1,&offset));
    assert(!gwmap_dma_slice_offset(GWMAP_DMA_CE,GWC_BYTES-1,2,&offset));
    assert(gwmap_dma_slice_offset(GWMAP_DMA_QMI,0,m.bytes-GWMAP_ALLOC_START,&offset) && offset==GWMAP_ALLOC_START);
    assert(!gwmap_dma_slice_offset(GWMAP_DMA_QMI,SIZE_MAX,1,&offset));
    assert(!gwmap_dma_slice_offset(4,0,1,&offset));cases+=7;
    assert(gwmap_dma_allocate(&m,17,0x4000,&cpu,&pa,&iova));
    assert(cpu==m.cpu+GWMAP_ALLOC_START && pa==m.physical+GWMAP_ALLOC_START && iova==m.iova+GWMAP_ALLOC_START);
    assert(gwmap_dma_allocate(&m,1,0x4000,&cpu,&pa,&iova));
    assert(iova==m.iova+GWMAP_ALLOC_START+0x4000);saved=m;
    assert(!gwmap_dma_allocate(&m,1,3,&cpu,&pa,&iova));
    assert(!gwmap_dma_allocate(&m,1,SIZE_MAX,&cpu,&pa,&iova));
    assert(!gwmap_dma_allocate(&m,SIZE_MAX,0x4000,&cpu,&pa,&iova));
    assert(!gwmap_dma_allocate(&m,1,0x4000,NULL,&pa,&iova));
    assert(!memcmp(&m,&saved,sizeof(m)));
    m=arena();assert(gwmap_dma_allocate(&m,m.bytes-GWMAP_ALLOC_START,0x4000,&cpu,&pa,&iova));
    assert(m.allocated==m.bytes && !gwmap_dma_allocate(&m,1,0x4000,&cpu,&pa,&iova));cases+=10;
    printf("Wi-Fi mappings: %u resource, BAR window, DMA translation, partition and allocation cases passed\n",cases);
    return 0;
}
