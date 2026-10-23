/* Host fixture only: no phone, no device, no firmware.
 *
 * Drives the pump over real queue-table memory (`ggh_init`) with the real
 * responder and the real pool adapter, so the join between queue, policy and
 * backing is exercised together. It proves the pump's decisions; it does not
 * prove the GMU ever writes these messages.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_hfi_pump.h"
#include "../ios16/gpu/giulia_gpu_hfi_alloc_pool.h"

static unsigned cases;
static uint32_t hfi[GGH_BYTES/4];

struct service {
    struct ggd_pool *pool;
    uint64_t gpu_base,gmu_base,cache_base;
    unsigned map_calls[2],unmap_calls[2],publish_calls,barrier_calls;
};
static uint64_t service_address(void *cookie,unsigned index,unsigned space)
{
    struct service *s=cookie;
    const struct ggd_allocation *allocation=&s->pool->allocations[index];
    if (space==0) return s->gpu_base+allocation->offset;
    return ((allocation->flags&0x80u)?s->cache_base:s->gmu_base)+allocation->offset;
}
static int service_map(void *cookie,unsigned index,unsigned space)
{
    struct service *s=cookie;
    unsigned at=space&1u;
    (void)index;
    ++s->map_calls[at];
    return 0;
}
static int service_unmap(void *cookie,unsigned index,unsigned space)
{
    struct service *s=cookie;
    (void)index;
    ++s->unmap_calls[space&1u];
    return 0;
}
static int service_publish(void *cookie,unsigned index)
{
    struct service *s=cookie;
    (void)index;
    ++s->publish_calls;
    return 0;
}
static int service_barrier(void *cookie)
{
    struct service *s=cookie;
    ++s->barrier_calls;
    return 0;
}
static void io_barrier(void *cookie)
{
    struct service *s=cookie;
    ++s->barrier_calls;
}

struct harness {
    struct ggh_io io;
    struct gga_pool pool;
    struct ggd_pool devpool;
    struct gga_ops ops;
    struct ggpp_state state;
    struct service service;
    uint32_t scratch[64];
};

static void harness_init(struct harness *h)
{
    struct gga_pool_io pool_io;
    memset(h,0,sizeof(*h));
    h->service.gpu_base=0x61000000ull;
    h->service.gmu_base=0x60000000ull;
    h->service.cache_base=0x800000ull;
    assert(ggh_init(hfi,GGH_BYTES,GGH_IOVA)==GGH_OK);
    h->io.words=hfi;
    h->io.bytes=GGH_BYTES;
    h->io.iova=GGH_IOVA;
    h->io.cookie=&h->service;
    h->io.barrier=io_barrier;
    pool_io.cookie=&h->service;
    pool_io.address=service_address;
    pool_io.map=service_map;
    pool_io.unmap=service_unmap;
    pool_io.publish=service_publish;
    pool_io.barrier=service_barrier;
    pool_io.reserved=0;
    assert(gga_pool_init(&h->pool,&h->devpool,h->service.gpu_base,h->service.cache_base,&pool_io)==GGA_OK);
    h->service.pool=&h->devpool;
    h->ops=gga_pool_ops(&h->pool);
}

/* Write a packet into a firmware-owned queue, as the GMU would. */
static void inject(struct harness *h,unsigned q,const uint32_t *message,unsigned words)
{
    volatile uint32_t *header,*ring;
    unsigned read=0,write=0,padded=(words+3)&~3u;
    assert(ggh_queue(&h->io,q,&header,&ring,&read,&write)==GGH_OK);
    for (unsigned i=0;i<padded;++i) ring[(write+i)&1023]=i<words?message[i]:0xfafafafau;
    h->io.barrier(h->io.cookie);
    header[11]=(write+padded)&1023;
    h->io.barrier(h->io.cookie);
}
/* Read one packet back from the host command queue. */
static unsigned drain(struct harness *h,uint32_t *out,unsigned capacity)
{
    volatile uint32_t *header,*ring;
    unsigned read=0,write=0,words=0,padded;
    assert(ggh_queue(&h->io,0,&header,&ring,&read,&write)==GGH_OK);
    if (read==write) return 0;
    words=(ring[read]>>8)&255u;
    padded=(words+3)&~3u;
    assert(words && words<=capacity && padded<=((write-read)&1023));
    for (unsigned i=0;i<words;++i) out[i]=ring[(read+i)&1023];
    h->io.barrier(h->io.cookie);
    header[10]=(read+padded)&1023;
    h->io.barrier(h->io.cookie);
    return words;
}

static unsigned packet(uint32_t *words,uint32_t flags,uint32_t kind,uint32_t gmu_handle,
    uint32_t size,uint32_t align)
{
    memset(words,0,sizeof(uint32_t)*12);
    words[0]=GGA_MSG_ID|(GGA_HDR_WORDS<<8)|(7u<<20);
    words[1]=1;
    words[4]=flags;
    words[5]=kind;
    words[6]=0;
    words[7]=gmu_handle;
    words[8]=0;
    words[9]=size;
    words[10]=align;
    return GGA_HDR_WORDS;
}

static void test_empty(void)
{
    struct harness h;
    harness_init(&h);
    assert(ggpp_pump(&h.io,&h.ops,&h.state,4,h.scratch,64)==GGPP_OK);
    assert(h.state.packets==0 && h.state.replied==0);
    assert(h.devpool.count==0);
    ++cases;
}

static void test_reply(void)
{
    struct harness h;
    const uint32_t observed[11]={0x00700b14u,1u,0,0,0x33fu,2u,0,2u,0,4096u,0};
    uint32_t reply[16];
    unsigned words;
    harness_init(&h);
    inject(&h,1,observed,11);
    assert(ggpp_pump(&h.io,&h.ops,&h.state,4,h.scratch,64)==GGPP_OK);
    assert(h.state.received==1 && h.state.replied==1 && h.state.dropped==0);
    words=drain(&h,reply,16);
    assert(words==GGA_HDR_WORDS);
    assert((reply[0]&0xffu)==GGA_MSG_ID && ((reply[0]>>16)&0xfu)==GGA_ACK_TYPE);
    assert(reply[1]==observed[0]);
    assert(reply[2]==0x61400000u && reply[8]==0x60400000u && reply[9]==4096);
    assert(h.devpool.count==1 && h.service.publish_calls==1);
    assert(h.state.alloc.count==1 && h.state.alloc.quarantined==0);
    ++cases;
}

static void test_other_messages(void)
{
    struct harness h;
    uint32_t ack[2]={126u|(2u<<8),0x1234u};
    uint32_t error[4]={100u|(4u<<8),0x11u,0,0};
    uint32_t debug[3]={101u|(3u<<8),0,0};
    uint32_t reply[4];
    harness_init(&h);
    inject(&h,1,ack,2);
    inject(&h,1,error,4);
    inject(&h,1,debug,3);
    assert(ggpp_pump(&h.io,&h.ops,&h.state,8,h.scratch,64)==GGPP_OK);
    assert(h.state.dropped==3 && h.state.replied==0 && h.state.alloc.count==0);
    assert(drain(&h,reply,4)==0);
    ++cases;
}

static void test_refused_stops(void)
{
    struct harness h;
    uint32_t words[12],reply[4];
    unsigned count;
    harness_init(&h);
    count=packet(words,0x33fu,GGA_KIND_SCRATCH,2,4096,0);
    words[0]=(words[0]&~(0xffu<<8))|(10u<<8); /* size field disagrees with frame */
    inject(&h,1,words,count);
    assert(ggpp_pump(&h.io,&h.ops,&h.state,4,h.scratch,64)==GGPP_REFUSED);
    assert(h.state.refused==1 && h.state.stalled==1 && h.state.replied==0);
    assert(h.devpool.count==0 && drain(&h,reply,4)==0);
    ++cases;
}

static void test_budget(void)
{
    struct harness h;
    uint32_t words[12],reply[16];
    unsigned count;
    harness_init(&h);
    count=packet(words,0x33fu,GGA_KIND_SCRATCH,2,4096,0);
    inject(&h,1,words,count);
    count=packet(words,0x33fu,GGA_KIND_CMD_BUFFER,3,4096,0);
    inject(&h,1,words,count);
    assert(ggpp_pump(&h.io,&h.ops,&h.state,1,h.scratch,64)==GGPP_OK);
    assert(h.state.replied==1 && h.devpool.count==1);
    assert(ggpp_pump(&h.io,&h.ops,&h.state,1,h.scratch,64)==GGPP_OK);
    assert(h.state.replied==2 && h.devpool.count==2);
    assert(drain(&h,reply,16)==GGA_HDR_WORDS);
    assert(drain(&h,reply,16)==GGA_HDR_WORDS);
    assert(drain(&h,reply,16)==0);
    ++cases;
}

static void test_send_failure_quarantines(void)
{
    struct harness h;
    uint32_t words[12];
    unsigned count;
    harness_init(&h);
    /* Fill the host command queue so the reply cannot be queued. */
    {
        uint32_t filler[12]={0};
        filler[0]=1u|(12u<<8); /* id 1, HFI_MSG_CMD, 12 words */
        while (ggh_send(&h.io,filler,12)==GGH_OK) { }
    }
    count=packet(words,0x33fu,GGA_KIND_SCRATCH,2,4096,0);
    inject(&h,1,words,count);
    assert(ggpp_pump(&h.io,&h.ops,&h.state,4,h.scratch,64)==GGPP_SEND);
    assert(h.state.quarantined==1 && h.state.replied==0);
    assert(h.state.alloc.count==1);
    assert(ggpp_pump(&h.io,&h.ops,&h.state,4,h.scratch,64)==GGPP_QUARANTINED);
    ++cases;
}

static void test_corrupt_queue_quarantines(void)
{
    struct harness h;
    uint32_t words[12];
    volatile uint32_t *header,*ring;
    unsigned read=0,write=0,count;
    harness_init(&h);
    count=packet(words,0x33fu,GGA_KIND_SCRATCH,2,4096,0);
    inject(&h,1,words,count);
    assert(ggh_queue(&h.io,1,&header,&ring,&read,&write)==GGH_OK);
    ring[read]=0x7u|(200u<<8); /* claims 200 words in a 1024-word ring */
    assert(ggpp_pump(&h.io,&h.ops,&h.state,4,h.scratch,64)==GGPP_RECEIVE);
    assert(h.state.quarantined==1 && h.state.alloc.count==0);
    ++cases;
}

static void test_duplicate_reuse(void)
{
    struct harness h;
    const uint32_t observed[11]={0x00700b14u,1u,0,0,0x33fu,2u,0,2u,0,4096u,0};
    uint32_t reply[16];
    harness_init(&h);
    inject(&h,1,observed,11);
    inject(&h,1,observed,11);
    assert(ggpp_pump(&h.io,&h.ops,&h.state,4,h.scratch,64)==GGPP_OK);
    assert(h.state.replied==2 && h.devpool.count==1 && h.state.alloc.reused==1);
    assert(drain(&h,reply,16)==GGA_HDR_WORDS && drain(&h,reply,16)==GGA_HDR_WORDS);
    assert(reply[2]==0x61400000u);
    ++cases;
}

int main(void)
{
    test_empty();
    test_reply();
    test_other_messages();
    test_refused_stops();
    test_budget();
    test_send_failure_quarantines();
    test_corrupt_queue_quarantines();
    test_duplicate_reuse();
    printf("HFI PUMP FIXTURE: %u cases passed (queue policy only; no hardware evidence)\n",cases);
    return 0;
}
