/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Checked WCN7850 host/device address mappings. These helpers do not touch
 * hardware or grant DMA ownership. The caller owns each mapping, serializes
 * dynamic BAR window changes and retains backing while DMA is outstanding.
 */
#ifndef GIULIA_WIFI_MAPPING_H
#define GIULIA_WIFI_MAPPING_H
#include <stdint.h>
#include <stddef.h>

/* The first eight IDs intentionally match giulia_wifi_link.h's GWL regions. */
enum { GWMAP_ROOT,GWMAP_PARF,GWMAP_PHY,GWMAP_GCC,GWMAP_GPIO,GWMAP_CFG,
    GWMAP_REFCLK,GWMAP_CRM,GWMAP_SMMU,GWMAP_QMP,GWMAP_IPCC,GWMAP_BAR,GWMAP_REGIONS };
#define GWMAP_ARENA_BYTES ((size_t)0x4000000)
#define GWMAP_IOVA UINT64_C(0x20000000)
#define GWMAP_CE_OFFSET ((size_t)0x1200000)
#define GWMAP_CE_BYTES ((size_t)0x200000)
#define GWMAP_ALLOC_START ((size_t)0x1400000)
#define GWMAP_TABLE_BYTES ((size_t)0x4000)
#define GWMAP_PA_LIMIT (UINT64_C(1)<<36)
#define GWMAP_BAR_WINDOW_REG UINT32_C(0x310c)
#define GWMAP_BAR_DIRECT_BYTES UINT32_C(0x80000)
#define GWMAP_DEVICE_BYTES UINT32_C(0x2000000)

static inline int gwmap_range(uint64_t base,uint64_t bytes,uint64_t address,size_t n)
{
    return bytes && bytes<=UINT64_MAX-base && n && address>=base &&
        (uint64_t)n<=bytes && address-base<=bytes-(uint64_t)n;
}
static inline int gwmap_overlap(uint64_t a,uint64_t n,uint64_t b,uint64_t m)
{ return n && m && (a<=b ? b-a<n : a-b<m); }

/* BAR0 is admitted only after PCI enumeration supplies the actual resource.
 * The memory window and all other ranges come from the captured board map. */
static inline int gwmap_resource(unsigned id,uint64_t bar_pa,size_t bar_bytes,
                                 uint64_t *physical,size_t *bytes)
{
    static const uint64_t bases[GWMAP_BAR]={UINT64_C(0x60000000),UINT64_C(0x1c00000),
        UINT64_C(0x1c06000),UINT64_C(0x16b000),UINT64_C(0xf110000),UINT64_C(0x60100000),
        UINT64_C(0x1ff1000),UINT64_C(0x1d05000),UINT64_C(0x15000000),
        UINT64_C(0xc300000),UINT64_C(0x406000)};
    static const size_t sizes[GWMAP_BAR]={0x2000,0x3000,0x2000,0x2000,0x51000,
        0x1000,0x1000,0x2000,0x100000,0x400,0x1000};
    if(id>=GWMAP_REGIONS || !physical || !bytes)return 0;
    if(id==GWMAP_BAR) {
        if(bar_bytes<0x100000 || (bar_bytes&(bar_bytes-1)) ||
           (bar_pa&((uint64_t)bar_bytes-1)) ||
           !gwmap_range(UINT64_C(0x60300000),UINT64_C(0x3d00000),bar_pa,bar_bytes))return 0;
        *physical=bar_pa;*bytes=bar_bytes;
    } else {*physical=bases[id];*bytes=sizes[id];}
    return 1;
}

/* A register access must be naturally aligned in both offset and CPU space.
 * Reject zero lengths and arithmetic wrap before constructing a pointer. */
static inline int gwmap_access(uintptr_t base,size_t bytes,size_t offset,
                               unsigned width,uintptr_t *address)
{
    if(!address || !base || (width!=1 && width!=2 && width!=4 && width!=8) ||
       (offset&(width-1)) || (base&(width-1)) || bytes>UINTPTR_MAX-base ||
       width>bytes || offset>bytes-width)return 0;
    *address=base+offset;return 1;
}

/* MHI uses the fixed low aperture. CE uses the serialized 512 KiB dynamic
 * window. Reject addresses that would silently alias after masking to six
 * window bits, and accesses spanning two hardware windows. */
static inline int gwmap_window(uint32_t offset,unsigned width,size_t bar_bytes,
                               uint32_t current,uint32_t *mapped,uint32_t *desired)
{
    if(!mapped || !desired || (width!=1 && width!=2 && width!=4 && width!=8) ||
       (offset&(width-1)) || offset>GWMAP_DEVICE_BYTES-width)return 0;
    uint32_t in_window=offset&(GWMAP_BAR_DIRECT_BYTES-1);
    if(width>GWMAP_BAR_DIRECT_BYTES-in_window)return 0;
    uint32_t at=offset;
    uint32_t next=current;
    if(offset>=GWMAP_BAR_DIRECT_BYTES) {
        if(current==UINT32_MAX)return 0;
        at=GWMAP_BAR_DIRECT_BYTES|in_window;
        next=(current&~UINT32_C(0x3f))|UINT32_C(0x40000000)|(offset>>19);
    }
    if(width>bar_bytes || at>bar_bytes-width)return 0;
    *mapped=at;*desired=next;return 1;
}

enum { GWMAP_DMA_ALL,GWMAP_DMA_MHI,GWMAP_DMA_CE,GWMAP_DMA_QMI };
struct gwmap_dma {
    uintptr_t cpu;
    uint64_t physical,iova;
    size_t bytes,allocated;
};
static inline int gwmap_dma_valid(const struct gwmap_dma *m)
{
    return m && m->cpu && !(m->cpu&0x3fff) && m->bytes==GWMAP_ARENA_BYTES &&
        m->bytes<=UINTPTR_MAX-m->cpu && m->physical && !(m->physical&0x1fffff) &&
        m->physical<=GWMAP_PA_LIMIT-m->bytes && !(m->iova&0x1fffff) &&
        m->iova<=UINT64_C(0x100000000)-m->bytes &&
        (m->iova>>30)==((m->iova+m->bytes-1)>>30) &&
        m->allocated>=GWMAP_ALLOC_START && m->allocated<=m->bytes;
}
static inline int gwmap_dma_init(struct gwmap_dma *m,uintptr_t cpu,uint64_t physical,
                                 uint64_t iova,size_t bytes)
{
    struct gwmap_dma candidate={cpu,physical,iova,bytes,GWMAP_ALLOC_START};
    if(!m || !gwmap_dma_valid(&candidate))return 0;
    *m=candidate;return 1;
}
static inline int gwmap_dma_tables_valid(const struct gwmap_dma *m,uintptr_t cpu,
                                         uint64_t physical,size_t bytes)
{
    return gwmap_dma_valid(m) && cpu && !(cpu&0x3fff) && bytes==GWMAP_TABLE_BYTES &&
        bytes<=UINTPTR_MAX-cpu && physical && !(physical&0x3fff) &&
        physical<=GWMAP_PA_LIMIT-bytes &&
        !gwmap_overlap(m->physical,m->bytes,physical,bytes) &&
        !gwmap_overlap((uint64_t)m->cpu,m->bytes,(uint64_t)cpu,bytes);
}
static inline int gwmap_dma_slice_offset(unsigned slice,size_t relative,size_t bytes,
                                          size_t *absolute)
{
    size_t start=0,length=0;
    if(!absolute || !bytes)return 0;
    switch(slice) {
    case GWMAP_DMA_ALL:length=GWMAP_ARENA_BYTES;break;
    case GWMAP_DMA_MHI:length=GWMAP_CE_OFFSET;break;
    case GWMAP_DMA_CE:start=GWMAP_CE_OFFSET;length=GWMAP_CE_BYTES;break;
    case GWMAP_DMA_QMI:start=GWMAP_ALLOC_START;length=GWMAP_ARENA_BYTES-start;break;
    default:return 0;
    }
    if(bytes>length || relative>length-bytes)return 0;
    *absolute=start+relative;return 1;
}
static inline int gwmap_dma_at(const struct gwmap_dma *m,size_t offset,size_t bytes,
                               uintptr_t *cpu,uint64_t *physical,uint64_t *iova)
{
    if(!gwmap_dma_valid(m) || !bytes || bytes>m->bytes || offset>m->bytes-bytes)return 0;
    if(cpu)*cpu=m->cpu+offset;
    if(physical)*physical=m->physical+offset;
    if(iova)*iova=m->iova+offset;
    return 1;
}
static inline int gwmap_dma_from_iova(const struct gwmap_dma *m,uint64_t address,
                                      size_t bytes,size_t *offset)
{
    if(!offset || !gwmap_dma_valid(m) || !gwmap_range(m->iova,m->bytes,address,bytes))return 0;
    *offset=(size_t)(address-m->iova);return 1;
}
static inline int gwmap_dma_from_physical(const struct gwmap_dma *m,uint64_t address,
                                          size_t bytes,size_t *offset)
{
    if(!offset || !gwmap_dma_valid(m) || !gwmap_range(m->physical,m->bytes,address,bytes))return 0;
    *offset=(size_t)(address-m->physical);return 1;
}
static inline int gwmap_dma_from_cpu(const struct gwmap_dma *m,uintptr_t address,
                                     size_t bytes,size_t *offset)
{
    if(!offset || !gwmap_dma_valid(m) || !gwmap_range((uint64_t)m->cpu,m->bytes,(uint64_t)address,bytes))return 0;
    *offset=(size_t)(address-m->cpu);return 1;
}
/* Dynamic clients (including the DP arena) use subranges of the allocated
 * tail. Validate the complete client range before adding its relative offset,
 * so a wrapped offset cannot alias MHI/CE or an unallocated arena address. */
static inline int gwmap_dma_allocated_offset(const struct gwmap_dma *m,size_t start,
                                             size_t length,size_t relative,size_t bytes,
                                             size_t *absolute)
{
    if(!absolute || !gwmap_dma_valid(m) || !length || !bytes ||
       start<GWMAP_ALLOC_START || length>m->allocated || start>m->allocated-length ||
       bytes>length || relative>length-bytes)return 0;
    *absolute=start+relative;return 1;
}
/* QMI allocations cannot encroach on MHI/firmware or CE memory. Alignment is
 * enforced for all three address spaces, with no mutation on any failure. */
static inline int gwmap_dma_allocate(struct gwmap_dma *m,size_t bytes,size_t alignment,
                                     uintptr_t *cpu,uint64_t *physical,uint64_t *iova)
{
    if(!gwmap_dma_valid(m) || !cpu || !physical || !iova || !bytes || !alignment ||
       (alignment&(alignment-1)) || alignment>m->bytes ||
       (m->cpu&(alignment-1)) || (m->physical&(alignment-1)) || (m->iova&(alignment-1)) ||
       m->allocated>SIZE_MAX-(alignment-1))return 0;
    size_t offset=(m->allocated+alignment-1)&~(alignment-1);
    if(!gwmap_dma_at(m,offset,bytes,cpu,physical,iova))return 0;
    m->allocated=offset+bytes;return 1;
}
#endif
