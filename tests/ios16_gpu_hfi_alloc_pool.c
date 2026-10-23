/* Host fixture only: no phone, no device memory, no HFI queue.
 *
 * Runs the real responder (giulia_gpu_hfi_alloc.h) against the real pool
 * adapter (giulia_gpu_hfi_alloc_pool.h) and a fake service that owns the pool,
 * its two mappings and the publish path. This covers the join between the two
 * units; it does not prove the A750 can read the result.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_hfi_alloc_pool.h"

static unsigned cases;

struct fake {
    struct ggd_pool *pool;
    uint64_t gpu_base,gmu_base,cache_base;
    unsigned map_calls[2],unmap_calls[2],publish_calls,barrier_calls,reserved_calls;
    int fail_map[2],fail_unmap[2],fail_publish,fail_reserved;
    unsigned last_publish_offset,last_publish_bytes;
};

static uint64_t fake_address(void *cookie,unsigned index,unsigned space)
{
    struct fake *f=cookie;
    const struct ggd_allocation *allocation=&f->pool->allocations[index];
    if (space==0) return f->gpu_base+allocation->offset;
    return ((allocation->flags&0x80u)?f->cache_base:f->gmu_base)+allocation->offset;
}
static int fake_map(void *cookie,unsigned index,unsigned space)
{
    struct fake *f=cookie;
    unsigned at=space&1u;
    (void)index;
    ++f->map_calls[at];
    return f->fail_map[at]?1:0;
}
static int fake_unmap(void *cookie,unsigned index,unsigned space)
{
    struct fake *f=cookie;
    unsigned at=space&1u;
    (void)index;
    ++f->unmap_calls[at];
    return f->fail_unmap[at]?1:0;
}
static int fake_publish(void *cookie,unsigned index)
{
    struct fake *f=cookie;
    const struct ggd_allocation *allocation=&f->pool->allocations[index];
    ++f->publish_calls;
    if (f->fail_publish) return 1;
    f->last_publish_offset=allocation->offset;
    f->last_publish_bytes=allocation->bytes;
    return 0;
}
static int fake_barrier(void *cookie)
{
    struct fake *f=cookie;
    ++f->barrier_calls;
    return 0;
}
static int fake_reserved(void *cookie,const struct gga_request *request,struct gga_block *out)
{
    struct fake *f=cookie;
    ++f->reserved_calls;
    if (f->fail_reserved) return 1;
    if (request->kind!=GGA_KIND_MEMSTORE) return 1;
    out->backing=0;
    out->bytes=request->size;
    out->effective_flags=request->flags;
    out->gpu_addr=0;
    out->gmu_addr=f->cache_base+0x3000;
    return 0;
}

static void setup(struct gga_pool *pool,struct ggd_pool *devpool,struct fake *f,int allow_reserved)
{
    struct gga_pool_io io;
    memset(f,0,sizeof(*f));
    f->gpu_base=0x61000000ull;
    f->gmu_base=0x60000000ull;
    f->cache_base=0x800000ull;
    io.cookie=f;
    io.address=fake_address;
    io.map=fake_map;
    io.unmap=fake_unmap;
    io.publish=fake_publish;
    io.barrier=fake_barrier;
    io.reserved=allow_reserved?fake_reserved:0;
    assert(gga_pool_init(pool,devpool,f->gpu_base,f->cache_base,&io)==GGA_OK);
    f->pool=devpool;
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

/* The exact packet START sends after CORE_FW_START. */
static void test_scratch_end_to_end(void)
{
    struct gga_pool pool;
    struct ggd_pool devpool;
    struct fake f;
    struct gga_state state={0};
    struct gga_ops ops;
    const uint32_t observed[11]={0x00700b14u,1u,0,0,0x33fu,2u,0,2u,0,4096u,0};
    uint32_t reply[16];
    unsigned reply_words=0;
    setup(&pool,&devpool,&f,0);
    ops=gga_pool_ops(&pool);
    assert(gga_handle(&ops,&state,observed,11,reply,16,&reply_words)==GGA_OK);
    assert(reply_words==GGA_HDR_WORDS);
    assert(devpool.count==1);
    assert(devpool.allocations[0].offset==GGD_CACHE_BYTES);
    assert(devpool.allocations[0].bytes==4096);
    assert(reply[2]==0x61400000u && reply[3]==0);   /* GPU window, pool offset 4 MiB */
    assert(reply[8]==0x60400000u);                  /* GMU window, same offset */
    assert(reply[9]==4096);
    assert(f.map_calls[0]==1 && f.map_calls[1]==1);
    assert(f.publish_calls==1 && f.last_publish_offset==GGD_CACHE_BYTES);
    assert(f.last_publish_bytes==4096 && f.barrier_calls==1);
    assert(ggd_pool_valid(&devpool));
    assert(state.count==1 && state.published==1 && state.quarantined==0);
    ++cases;
}

static void test_cacheable_and_single_master(void)
{
    struct gga_pool pool;
    struct ggd_pool devpool;
    struct fake f;
    struct gga_state state={0};
    struct gga_ops ops;
    uint32_t words[12],reply[16];
    unsigned count,reply_words=0;
    /* GMU_CACHEABLE selects the pool's cache window and its GMU alias. */
    setup(&pool,&devpool,&f,0);
    ops=gga_pool_ops(&pool);
    count=packet(words,0x33fu|GGA_FLAG_GMU_CACHEABLE,GGA_KIND_SCRATCH,2,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_OK);
    assert(devpool.allocations[0].offset==0);
    assert(reply[2]==0x61000000u && reply[8]==0x800000u);
    assert(ggd_pool_valid(&devpool));
    ++cases;
    /* GMU-only: no GPU alias, so the GPU mapping is never requested. */
    setup(&pool,&devpool,&f,0);
    state.count=0;
    count=packet(words,GGA_FLAG_HOST_ACC|GGA_FLAG_HOST_INIT,GGA_KIND_SCRATCH,9,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_OK);
    assert(f.map_calls[0]==0 && f.map_calls[1]==1);
    assert(reply[2]==0 && reply[8]==0x60400000u);
    ++cases;
    /* GFX without GMU_ACC: no GMU alias and no GMU mapping. */
    setup(&pool,&devpool,&f,0);
    state.count=0;
    count=packet(words,GGA_FLAG_GFX_ACC|GGA_FLAG_GFX_WRITE,GGA_KIND_CMD_BUFFER,10,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_OK);
    assert(f.map_calls[0]==1 && f.map_calls[1]==0);
    assert(reply[2]==0x61400000u && reply[8]==0);
    ++cases;
}

static void test_pool_sequences(void)
{
    struct gga_pool pool;
    struct ggd_pool devpool;
    struct fake f;
    struct gga_state state={0};
    struct gga_ops ops;
    uint32_t words[12],reply[16];
    unsigned count,reply_words=0;
    setup(&pool,&devpool,&f,0);
    ops=gga_pool_ops(&pool);
    /* Distinct keys get distinct, page-aligned ranges. */
    count=packet(words,0x33fu,GGA_KIND_SCRATCH,2,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_OK);
    count=packet(words,0x33fu,GGA_KIND_CSW_SMMU_INFO,3,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_OK);
    assert(devpool.count==2);
    assert(devpool.allocations[1].offset==GGD_CACHE_BYTES+4096);
    assert(devpool.allocations[0].offset+devpool.allocations[0].bytes<=
        devpool.allocations[1].offset);
    /* The same key reuses the first range and does not touch the pool again. */
    count=packet(words,0x33fu,GGA_KIND_SCRATCH,2,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_OK);
    assert(devpool.count==2 && f.publish_calls==2 && state.reused==1);
    assert(reply[2]==0x61400000u);
    assert(ggd_pool_valid(&devpool));
    ++cases;
}

static void test_mapping_failure_is_atomic(void)
{
    struct gga_pool pool;
    struct ggd_pool devpool;
    struct fake f;
    struct gga_state state={0};
    struct gga_ops ops;
    uint32_t words[12],reply[16];
    unsigned count,reply_words=0;
    /* GMU mapping fails after the GPU mapping was made: the GPU mapping is
     * dropped and no reply is produced. The bump pool keeps the space. */
    setup(&pool,&devpool,&f,0);
    ops=gga_pool_ops(&pool);
    f.fail_map[1]=1;
    count=packet(words,0x33fu,GGA_KIND_SCRATCH,2,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_PROVIDER);
    assert(reply_words==0 && state.count==0 && state.quarantined==0);
    assert(f.map_calls[0]==1 && f.map_calls[1]==1);
    assert(f.unmap_calls[0]==1 && f.unmap_calls[1]==0);
    assert(f.publish_calls==0);
    assert(devpool.count==1 && pool.failures==1);
    assert(ggd_pool_valid(&devpool));
    ++cases;
    /* GPU mapping fails first: nothing to unwind. */
    setup(&pool,&devpool,&f,0);
    state.count=0;
    f.fail_map[0]=1;
    count=packet(words,0x33fu,GGA_KIND_SCRATCH,2,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_PROVIDER);
    assert(f.unmap_calls[0]==0 && f.unmap_calls[1]==0 && f.publish_calls==0);
    ++cases;
}

static void test_publish_and_release(void)
{
    struct gga_pool pool;
    struct ggd_pool devpool;
    struct fake f;
    struct gga_state state={0};
    struct gga_ops ops;
    uint32_t words[12],reply[16];
    unsigned count,reply_words=0;
    /* Publish failure: confirmed unmaps, no reply, no quarantine. */
    setup(&pool,&devpool,&f,0);
    ops=gga_pool_ops(&pool);
    f.fail_publish=1;
    count=packet(words,0x33fu,GGA_KIND_SCRATCH,2,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_PUBLISH);
    assert(state.count==0 && state.quarantined==0);
    assert(pool.releases==1 && f.unmap_calls[0]==1 && f.unmap_calls[1]==1);
    ++cases;
    /* An unconfirmed unmap keeps the responder from guessing. */
    setup(&pool,&devpool,&f,0);
    state.count=0;
    f.fail_publish=1;
    f.fail_unmap[0]=1;
    count=packet(words,0x33fu,GGA_KIND_SCRATCH,3,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_RELEASE);
    assert(state.quarantined==1 && state.count==0);
    ++cases;
}

static void test_limits_and_reserved(void)
{
    struct gga_pool pool;
    struct ggd_pool devpool;
    struct fake f;
    struct gga_state state={0};
    struct gga_ops ops;
    uint32_t words[12],reply[16];
    unsigned count,reply_words=0;
    /* Alignment beyond the pool's 4 MiB chunk cannot be honoured. */
    setup(&pool,&devpool,&f,0);
    ops=gga_pool_ops(&pool);
    count=packet(words,0x33fu,GGA_KIND_SCRATCH,2,4096,23);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_PROVIDER);
    assert(devpool.count==0);
    ++cases;
    /* More than the whole pool is refused before anything is mapped. */
    setup(&pool,&devpool,&f,0);
    state.count=0;
    count=packet(words,0x33fu,GGA_KIND_SCRATCH,2,GGD_BYTES,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_PROVIDER);
    assert(devpool.count==0 && f.map_calls[0]==0);
    ++cases;
    /* Memstore has its own owner; without one the kind stays refused. */
    setup(&pool,&devpool,&f,0);
    state.count=0;
    count=packet(words,0x33eu,GGA_KIND_MEMSTORE,4,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_RESERVED);
    ++cases;
    setup(&pool,&devpool,&f,1);
    ops=gga_pool_ops(&pool); /* the reserved backend changes the ops table */
    state.count=0;
    count=packet(words,0x33eu,GGA_KIND_MEMSTORE,4,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_OK);
    assert(f.reserved_calls==1 && devpool.count==0);
    assert(reply[2]==0 && reply[8]==0x803000u);
    ++cases;
}

int main(void)
{
    test_scratch_end_to_end();
    test_cacheable_and_single_master();
    test_pool_sequences();
    test_mapping_failure_is_atomic();
    test_publish_and_release();
    test_limits_and_reserved();
    printf("HFI POOL FIXTURE: %u cases passed (pool/adapter join only; no hardware evidence)\n",cases);
    return 0;
}
