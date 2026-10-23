/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Read-only GPU hardware-fence queue ABI. Fixed source evidence is recorded
 * in hardware/gpu-fence-source.json. No function here maps, initializes,
 * clears, consumes or signals shared storage, or proves fence completion.
 */
#ifndef GIULIA_GPU_FENCE_H
#define GIULIA_GPU_FENCE_H
#include <stdint.h>
#include <stddef.h>

#define GGF_CARVEOUT_PHYSICAL UINT64_C(0xd4e23000)
enum {
    GGF_CARVEOUT_BYTES=0x2dd000,
    GGF_QUEUE_ENTRIES=800, GGF_FENCE_ENTRIES=8192,
    GGF_TABLE_HEADER_BYTES=24, GGF_QUEUE_HEADER_BYTES=56,
    GGF_GPU_PAYLOAD_BYTES=64, GGF_CONTROL_ALLOCATION_BYTES=24,
    GGF_FENCE_ENTRY_BYTES=160, GGF_LOCK_BYTES=16*8
};

/* All offsets are bytes relative to the GPU client's own region. page_bytes
 * is the provider's verified host ABI, never inferred from XNU PAGE_SIZE.
 * These calculations alone do not establish ownership or controller ABI.
 */
struct ggf_layout {
    uint64_t physical;
    uint32_t bytes,tx_header,rx_header,tx_payload,rx_payload,queue_bytes,page_bytes;
};

static inline int ggf_layout(uint32_t page_bytes,struct ggf_layout *out)
{
    if(!out || (page_bytes!=4096 && page_bytes!=16384 && page_bytes!=65536))return 0;
    const uint32_t headers=GGF_TABLE_HEADER_BYTES+2*GGF_QUEUE_HEADER_BYTES;
    const uint32_t control=headers+2*GGF_QUEUE_ENTRIES*GGF_CONTROL_ALLOCATION_BYTES;
    const uint32_t table_end=control+GGF_LOCK_BYTES+GGF_FENCE_ENTRIES*GGF_FENCE_ENTRY_BYTES;
    const uint32_t offset=(table_end+page_bytes-1u)&~(page_bytes-1u);
    struct ggf_layout result;
    result.physical=GGF_CARVEOUT_PHYSICAL+offset;
    result.queue_bytes=GGF_QUEUE_ENTRIES*GGF_GPU_PAYLOAD_BYTES;
    result.bytes=(headers+2*result.queue_bytes+page_bytes-1u)&~(page_bytes-1u);
    result.tx_header=GGF_TABLE_HEADER_BYTES;
    result.rx_header=result.tx_header+GGF_QUEUE_HEADER_BYTES;
    result.tx_payload=headers;
    result.rx_payload=headers+result.queue_bytes;
    result.page_bytes=page_bytes;
    if(offset>GGF_CARVEOUT_BYTES || result.bytes>GGF_CARVEOUT_BYTES-offset)return 0;
    *out=result;
    return 1;
}

/* Re-derive every field before trusting offsets, sizes or physical addresses.
 * The fixed carveout is below 4 GiB: its vendor queue start_addr is only u32.
 */
static inline int ggf_layout_valid(const struct ggf_layout *layout)
{
    struct ggf_layout expected;
    return layout && ggf_layout(layout->page_bytes,&expected) &&
        layout->physical==expected.physical && layout->bytes==expected.bytes &&
        layout->tx_header==expected.tx_header && layout->rx_header==expected.rx_header &&
        layout->tx_payload==expected.tx_payload && layout->rx_payload==expected.rx_payload &&
        layout->queue_bytes==expected.queue_bytes;
}

struct ggf_observation { uint32_t tx_read,tx_write,rx_read,rx_write,stable; };

static inline int ggf_index_valid(uint32_t index)
{
    /* GPU indices count dwords, at boundaries of 64-byte client payloads. */
    return index<GGF_QUEUE_ENTRIES*(GGF_GPU_PAYLOAD_BYTES/4) &&
        !(index% (GGF_GPU_PAYLOAD_BYTES/4));
}

/* Internal bounded sample; only call after validating the mapping and layout.
 * Despite public header comments saying dwords, the pinned initializer writes
 * all size/offset fields in bytes. status, drop counts, watermarks, requests
 * and IRQ fields are not initialized there and are not validity predicates.
 */
static inline int ggf_read_sample(const volatile uint32_t *words,
    const struct ggf_layout *layout,struct ggf_observation *out)
{
    if(words[0]!=0 || words[1]!=layout->bytes || words[2]!=24 ||
       words[3]!=56 || words[4]!=2 || words[5]!=2)return 0;
    const volatile uint32_t *tx=words+layout->tx_header/4;
    const volatile uint32_t *rx=words+layout->rx_header/4;
    if(tx[1]!=(uint32_t)(layout->physical+layout->tx_payload) || tx[2]!=1 ||
       tx[3]!=layout->queue_bytes || tx[4]!=64 ||
       rx[1]!=(uint32_t)(layout->physical+layout->rx_payload) || rx[2]!=2 ||
       rx[3]!=layout->queue_bytes || rx[4]!=64)return 0;
    struct ggf_observation result;
    result.tx_read=tx[12];result.tx_write=tx[13];
    result.rx_read=rx[12];result.rx_write=rx[13];result.stable=0;
    if(!ggf_index_valid(result.tx_read) || !ggf_index_valid(result.tx_write) ||
       !ggf_index_valid(result.rx_read) || !ggf_index_valid(result.rx_write))return 0;
    *out=result;
    return 1;
}

/* Read two ordered descriptor/index samples. An invalid static header or
 * invalid index in either sample rejects the observation. stable only means
 * the four indices matched; it is not an atomic GPU/controller snapshot,
 * evidence that the peer is READY, or a completed fence. Payloads are not read.
 *
 * The caller must establish live ownership and CPU read coherency before
 * calling. The actual CPU backend supplies barriers. Structural input errors
 * do not read backing or invoke barriers. Every failure preserves out. out
 * must not alias the shared mapping, including its page padding.
 */
static inline int ggf_snapshot(const volatile void *memory,size_t bytes,
    uint64_t physical,const struct ggf_layout *layout,struct ggf_observation *out,
    void (*barrier)(void *),void *cookie)
{
    uintptr_t start=(uintptr_t)memory,destination=(uintptr_t)out;
    if(!memory || !out || !barrier || !ggf_layout_valid(layout) ||
       bytes!=layout->bytes || physical!=layout->physical ||
       (start&3u) || bytes>UINTPTR_MAX-start || (destination&3u) ||
       sizeof(*out)>UINTPTR_MAX-destination ||
       (destination<start+bytes && start<destination+sizeof(*out)))return 0;
    const volatile uint32_t *words=(const volatile uint32_t *)memory;
    struct ggf_observation first,second;
    barrier(cookie);
    if(!ggf_read_sample(words,layout,&first))return 0;
    barrier(cookie);
    if(!ggf_read_sample(words,layout,&second))return 0;
    second.stable=first.tx_read==second.tx_read && first.tx_write==second.tx_write &&
        first.rx_read==second.rx_read && first.rx_write==second.rx_write;
    *out=second;
    return 1;
}
#endif
