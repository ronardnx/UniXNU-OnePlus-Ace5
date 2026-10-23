/* Independent numeric ABI oracle; no production initializer seeds fixtures. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_fence.h"

static uint32_t backing[0x20000/4],before_memory[0x20000/4];
struct fixture { uint32_t page,bytes;uint64_t physical; };
static const struct fixture fixtures[]={
    {4096,0x1a000,UINT64_C(0xd4f6d000)},
    {16384,0x1c000,UINT64_C(0xd4f6f000)},
    {65536,0x20000,UINT64_C(0xd4f73000)}
};
struct mutation { unsigned calls,word;uint32_t value; };
static void read_barrier(void *cookie)
{
    struct mutation *change=(struct mutation *)cookie;
    change->calls++;
    if(change->calls==2 && change->word<sizeof(backing)/sizeof(backing[0]))
        backing[change->word]=change->value;
}
static struct mutation no_change(void)
{
    struct mutation result={0,UINT32_MAX,0};return result;
}
static void populate(const struct fixture *fixture)
{
    /* The vendor does not initialize status, watermarks or IRQ fields.
     * Nonzero junk there and throughout payloads must not cause rejection.
     */
    memset(backing,0xa5,sizeof(backing));
    backing[0]=0;backing[1]=fixture->bytes;backing[2]=24;
    backing[3]=56;backing[4]=2;backing[5]=2;
    const unsigned headers[]={6,20};
    const uint32_t payloads[]={0x88,0xc888};
    for(unsigned i=0;i<2;i++) {
        unsigned at=headers[i];
        backing[at+1]=(uint32_t)(fixture->physical+payloads[i]);
        backing[at+2]=i+1;backing[at+3]=51200;backing[at+4]=64;
        backing[at+12]=0;backing[at+13]=0;
    }
}
static void layouts(void)
{
    assert(GGF_CARVEOUT_PHYSICAL==UINT64_C(0xd4e23000) && GGF_CARVEOUT_BYTES==0x2dd000);
    assert(GGF_CONTROL_ALLOCATION_BYTES==24 && GGF_GPU_PAYLOAD_BYTES==64);
    for(unsigned i=0;i<3;i++) {
        struct ggf_layout got;
        assert(ggf_layout(fixtures[i].page,&got));
        assert(got.physical==fixtures[i].physical && got.bytes==fixtures[i].bytes);
        assert(got.tx_header==24 && got.rx_header==80 && got.tx_payload==136);
        assert(got.rx_payload==51336 && got.queue_bytes==51200 && got.page_bytes==fixtures[i].page);
        assert(ggf_layout_valid(&got));
        /* Oracle arithmetic distinguishes 24-byte control allocation from
         * 64-byte GPU entries, includes 16 locks and 8192 160-byte fences.
         */
        assert(24+2*56+2*800*24==0x9688);
        assert(0x9688+16*8+8192*160==0x149708);
        assert(got.physical-0xd4e23000u>=0x149708u);
        assert(got.physical-0xd4e23000u+got.bytes<=0x2dd000u);
        struct ggf_layout bad=got;
#define BAD_LAYOUT(field,value) do {bad=got;bad.field=(value);assert(!ggf_layout_valid(&bad));} while(0)
        BAD_LAYOUT(physical,UINT64_MAX);BAD_LAYOUT(physical,got.physical+4096);
        BAD_LAYOUT(bytes,got.bytes-1);BAD_LAYOUT(bytes,UINT32_MAX);
        BAD_LAYOUT(tx_header,UINT32_MAX);BAD_LAYOUT(rx_header,0);
        BAD_LAYOUT(tx_payload,got.rx_payload);BAD_LAYOUT(rx_payload,UINT32_MAX);
        BAD_LAYOUT(queue_bytes,800*24);BAD_LAYOUT(page_bytes,8192);
        BAD_LAYOUT(page_bytes,fixtures[(i+1)%3].page);
#undef BAD_LAYOUT
    }
    const uint32_t invalid[]={0,1,2048,8192,32768,131072,UINT32_MAX};
    struct ggf_layout output,before;
    memset(&output,0x5a,sizeof(output));memcpy(&before,&output,sizeof(before));
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
        assert(!ggf_layout(invalid[i],&output));assert(!memcmp(&output,&before,sizeof(output)));
    }
    assert(!ggf_layout(4096,NULL));assert(!ggf_layout_valid(NULL));
}
static void samples(void)
{
    for(unsigned i=0;i<3;i++) {
        struct ggf_layout layout;assert(ggf_layout(fixtures[i].page,&layout));
        populate(&fixtures[i]);memcpy(before_memory,backing,sizeof(backing));
        struct mutation change=no_change();struct ggf_observation got;
        assert(ggf_snapshot(backing,layout.bytes,layout.physical,&layout,&got,read_barrier,&change));
        assert(got.stable && !got.tx_read && !got.tx_write && !got.rx_read && !got.rx_write);
        assert(change.calls==2 && !memcmp(before_memory,backing,sizeof(backing)));
        /* Check every legal slot index, including the final 64-byte slot. */
        for(unsigned slot=0;slot<800;slot++) {
            uint32_t index=slot*16;
            backing[18]=index;backing[19]=index;backing[32]=index;backing[33]=index;
            change=no_change();
            assert(ggf_snapshot(backing,layout.bytes,layout.physical,&layout,&got,read_barrier,&change));
            assert(got.stable && got.tx_read==index && got.tx_write==index &&
                got.rx_read==index && got.rx_write==index);
        }
        /* Each changing index independently invalidates stability; wrapping
         * from the last slot to zero remains a structurally valid sample.
         */
        const unsigned index_words[]={18,19,32,33};
        for(unsigned field=0;field<4;field++) {
            populate(&fixtures[i]);backing[index_words[field]]=12784;
            change.calls=0;change.word=index_words[field];change.value=0;
            assert(ggf_snapshot(backing,layout.bytes,layout.physical,&layout,&got,read_barrier,&change));
            assert(!got.stable && !got.tx_read && !got.tx_write && !got.rx_read && !got.rx_write);
        }
    }
}
static void invalid_headers(void)
{
    struct ggf_layout layout;assert(ggf_layout(4096,&layout));
    struct ggf_observation got={11,22,33,44,55},before=got;
    const unsigned static_words[]={0,1,2,3,4,5,7,8,9,10,21,22,23,24};
    for(unsigned i=0;i<sizeof(static_words)/sizeof(static_words[0]);i++) {
        for(unsigned pass=0;pass<2;pass++) {
            populate(&fixtures[0]);struct mutation change=no_change();
            unsigned word=static_words[i];uint32_t corrupt=backing[word]^1u;
            if(!pass)backing[word]=corrupt;
            else {change.word=word;change.value=corrupt;}
            assert(!ggf_snapshot(backing,layout.bytes,layout.physical,&layout,&got,read_barrier,&change));
            assert(!memcmp(&got,&before,sizeof(got)) && change.calls==pass+1);
        }
    }
    /* Explicit unit/payload confusion, an external qheader, wrong physical
     * payload addresses, a truncated u64 address, and the wrong queue type.
     */
    const uint32_t invalid_pairs[][2]={
        {1,0x1a000/4},{2,24/4},{2,0x1a000},{2,UINT32_MAX},{3,56/4},
        {9,51200/4},{10,24},{23,800*24},{24,0},{7,0xd4e23000},
        {21,0xd5100000},{7,0xd4f6d000},{8,2},{22,1}
    };
    for(unsigned i=0;i<sizeof(invalid_pairs)/sizeof(invalid_pairs[0]);i++) {
        populate(&fixtures[0]);struct mutation change=no_change();
        backing[invalid_pairs[i][0]]=invalid_pairs[i][1];
        assert(!ggf_snapshot(backing,layout.bytes,layout.physical,&layout,&got,read_barrier,&change));
        assert(!memcmp(&got,&before,sizeof(got)));
    }
    const unsigned index_words[]={18,19,32,33};
    for(unsigned field=0;field<4;field++) {
        /* Exercise each non-slot dword in the entire queue, not just size
         * boundaries; values in byte units can otherwise look plausible.
         */
        for(uint32_t index=0;index<=12816;index++) {
            if(index<12800 && !(index%16))continue;
            populate(&fixtures[0]);struct mutation change=no_change();
            backing[index_words[field]]=index;
            assert(!ggf_snapshot(backing,layout.bytes,layout.physical,&layout,&got,read_barrier,&change));
        }
        for(unsigned pass=0;pass<2;pass++) {
            populate(&fixtures[0]);struct mutation change=no_change();
            if(!pass)backing[index_words[field]]=UINT32_MAX;
            else {change.word=index_words[field];change.value=12800;}
            assert(!ggf_snapshot(backing,layout.bytes,layout.physical,&layout,&got,read_barrier,&change));
            assert(change.calls==pass+1 && !memcmp(&got,&before,sizeof(got)));
        }
    }
}
static void invalid_inputs(void)
{
    struct ggf_layout layout,bad;assert(ggf_layout(4096,&layout));
    populate(&fixtures[0]);memcpy(before_memory,backing,sizeof(backing));
    struct mutation change=no_change();
    struct ggf_observation got={11,22,33,44,55},before=got;
#define REJECT(memory,size,pa,descriptor,result,barrier) \
    assert(!ggf_snapshot(memory,size,pa,descriptor,result,barrier,&change))
    REJECT(NULL,layout.bytes,layout.physical,&layout,&got,read_barrier);
    REJECT(backing,layout.bytes-1,layout.physical,&layout,&got,read_barrier);
    REJECT(backing,layout.bytes+1,layout.physical,&layout,&got,read_barrier);
    REJECT(backing,SIZE_MAX,layout.physical,&layout,&got,read_barrier);
    REJECT(backing,layout.bytes,layout.physical+1,&layout,&got,read_barrier);
    REJECT(backing,layout.bytes,UINT64_MAX,&layout,&got,read_barrier);
    REJECT((unsigned char *)backing+1,layout.bytes,layout.physical,&layout,&got,read_barrier);
    REJECT((void *)(UINTPTR_MAX-3),layout.bytes,layout.physical,&layout,&got,read_barrier);
    REJECT(backing,layout.bytes,layout.physical,NULL,&got,read_barrier);
    REJECT(backing,layout.bytes,layout.physical,&layout,NULL,read_barrier);
    REJECT(backing,layout.bytes,layout.physical,&layout,&got,NULL);
    REJECT(backing,layout.bytes,layout.physical,&layout,(struct ggf_observation *)(UINTPTR_MAX-3),read_barrier);
    REJECT(backing,layout.bytes,layout.physical,&layout,(struct ggf_observation *)backing,read_barrier);
    REJECT(backing,layout.bytes,layout.physical,&layout,(struct ggf_observation *)&backing[layout.bytes/4-1],read_barrier);
    bad=layout;bad.rx_header=UINT32_MAX;
    REJECT(backing,layout.bytes,layout.physical,&bad,&got,read_barrier);
    bad=layout;bad.physical=UINT64_MAX;
    REJECT(backing,layout.bytes,UINT64_MAX,&bad,&got,read_barrier);
#undef REJECT
    assert(!change.calls && !memcmp(&got,&before,sizeof(got)));
    assert(!memcmp(before_memory,backing,sizeof(backing)));
}
int main(void)
{
    layouts();samples();invalid_headers();invalid_inputs();
    puts("GPU hardware-fence layout and read-only queue descriptor/index validation pass (host only)");
    return 0;
}
