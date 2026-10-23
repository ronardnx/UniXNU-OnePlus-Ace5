/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Dedicated timestamp storage ABI. This header never allocates, maps, clears,
 * or advances timestamps. The provider owns backing for its entire lifetime.
 * Fixed vendor evidence: hardware/gpu-memstore-source.json.
 */
#ifndef GIULIA_GPU_MEMSTORE_H
#define GIULIA_GPU_MEMSTORE_H
#include <stdint.h>
#include <stddef.h>

/* kgsl.h uses eight host pages. This provider explicitly uses eight 4 KiB
 * pages; it does not inherit XNU's PAGE_SIZE or assert the stock kernel's
 * configured host page size. The vendor's record layout is ten u32 words.
 */
enum {
    GGMS_BYTES=32768, GGMS_IOVA=0x66000000, GGMS_STRIDE=40, GGMS_RECORDS=GGMS_BYTES/GGMS_STRIDE,
    GGMS_CONTEXT_LIMIT=GGMS_RECORDS-2-4,
    GGMS_GLOBAL_CONTEXT=GGMS_CONTEXT_LIMIT-1,
    GGMS_USER_FIRST=1, GGMS_USER_MAX=GGMS_GLOBAL_CONTEXT-1,
    GGMS_SOP=0, GGMS_EOP=8
};

/* Preserve the original HFI alignment encoding: low byte is VA exponent,
 * next byte is size exponent. The caller must also check both fixed IOVAs
 * against the requested VA alignment. A larger size alignment cannot claim
 * backing beyond the complete, fixed-size provider.
 *
 * The gen7-9-1 device memstore is preallocated privileged and GPU writable.
 * Never relax those GPU attributes based on an incompatible request. GMU
 * ACC/PRIV/WRITEABLE retain their independent HFI meanings. A cacheable GMU
 * alias is deliberately unsupported. HOST_INIT never authorizes re-zeroing
 * existing live storage.
 */
static inline int ggms_request(uint32_t bytes,uint32_t alignment,
    uint32_t flags,uint32_t kind)
{
    uint32_t va_shift=alignment&255u,size_shift=(alignment>>8)&255u;
    if(kind!=22 || !bytes || bytes>GGMS_BYTES || (flags&~0x37fu) ||
       (flags&7u)!=7u || alignment>>16 || va_shift>22 || size_shift>22)return 0;
    uint32_t size_align=1u<<size_shift;
    return ((bytes+size_align-1u)&~(size_align-1u))<=GGMS_BYTES;
}

/* Context zero and the reserved global context are observable diagnostics.
 * Ringbuffer slots and the incomplete trailing record are not contexts.
 */
static inline int ggms_offset(uint32_t context,uint32_t field,uint32_t *out)
{
    if(!out || context>=GGMS_CONTEXT_LIMIT || (field!=GGMS_SOP && field!=GGMS_EOP))return 0;
    *out=context*GGMS_STRIDE+field;
    return 1;
}

struct ggms_observation { uint32_t sop,eop,stable; };

/* Two ordered volatile observations, with no retry loop that can stall on a
 * busy device. stable only means both sampled pairs matched. It does not
 * promise an atomic GPU snapshot, prove context registration, or indicate
 * fence completion. Read barriers are supplied by the actual CPU backend.
 * Invalid inputs leave the output unchanged and do not read backing.
 */
static inline int ggms_snapshot(const volatile void *memory,size_t bytes,
    uint32_t context,struct ggms_observation *out,
    void (*barrier)(void *),void *cookie)
{
    uint32_t sop_at,eop_at;
    if(!memory || !out || !barrier || bytes!=GGMS_BYTES ||
       ((uintptr_t)memory&3u) || bytes>UINTPTR_MAX-(uintptr_t)memory ||
       !ggms_offset(context,GGMS_SOP,&sop_at) ||
       !ggms_offset(context,GGMS_EOP,&eop_at))return 0;
    const volatile uint32_t *words=(const volatile uint32_t *)memory;
    barrier(cookie);
    uint32_t sop=words[sop_at/4],eop=words[eop_at/4];
    barrier(cookie);
    struct ggms_observation observed;
    observed.sop=words[sop_at/4];observed.eop=words[eop_at/4];
    observed.stable=(sop==observed.sop && eop==observed.eop);
    *out=observed;
    return 1;
}
#endif
