/* Host fixture only: no phone, no HFI queue, no device memory.
 *
 * The responder is pure decision logic, so the provider here is a RAM arena
 * that answers with addresses and a publish/release contract it can honour.
 * Passing proves the responder refuses to fabricate or alias memory; it proves
 * nothing about the A750, the GMU or the real HFI channel.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_hfi_alloc.h"

static unsigned cases;
static uint32_t arena_words[64*1024]; /* 256 KiB of host fixture RAM */

struct arena {
    uint32_t *words;
    unsigned cap_words,used;
    int fail_reserve,fail_publish,fail_release;
    int lie_flags,drop_gmu,force_gmu,add_bad_gpu,add_bad_gmu,short_bytes,reserved_fail;
    unsigned reserve_calls,publish_calls,release_calls,barrier_calls,reserved_calls;
    uint64_t gpu_base,gmu_base;
};

static int reserve_cb(void *cookie,const struct gga_request *request,uint32_t bytes,
    uint32_t gpu_alignment,uint32_t size_alignment,struct gga_block *out)
{
    struct arena *a=cookie;
    uint32_t alignment,start,capacity;
    int gfx,gmu;
    ++a->reserve_calls;
    if (a->fail_reserve) return 1;
    alignment=size_alignment>gpu_alignment?size_alignment:gpu_alignment;
    start=(a->used+alignment-1)&~(alignment-1);
    capacity=a->cap_words*4;
    if (bytes%4 || start>capacity || bytes>capacity-start) return 1;
    memset(a->words+start/4,0,bytes);
    a->used=start+bytes;
    out->backing=a->words+start/4;
    out->bytes=a->short_bytes?(bytes>GGA_MIN_ALIGN?bytes-GGA_MIN_ALIGN:0):bytes;
    out->effective_flags=a->lie_flags?request->flags:gga_effective_flags(request->flags,request->kind);
    gfx=(request->flags&GGA_FLAG_GFX_ACC)!=0;
    gmu=gga_kind_reserved(request->kind)?1:
        (gfx?((request->flags&GGA_FLAG_GMU_ACC)!=0):1);
    if (a->drop_gmu) gmu=0;
    if (a->force_gmu) gmu=1;
    out->gpu_addr=gfx?(a->gpu_base+start):0;
    out->gmu_addr=gmu?(a->gmu_base+start):0;
    if (a->add_bad_gpu) out->gpu_addr+=0x100;
    if (a->add_bad_gmu) out->gmu_addr+=0x100;
    return 0;
}
static int publish_cb(void *cookie,const struct gga_block *block)
{
    struct arena *a=cookie;
    const uint32_t *words=(const uint32_t *)block->backing;
    unsigned i;
    ++a->publish_calls;
    if (a->fail_publish) return 1;
    /* The provider owns zeroing; the responder only records that it happened. */
    for (i=0;i<block->bytes/4;++i) assert(words[i]==0);
    return 0;
}
static int release_cb(void *cookie,const struct gga_block *block)
{
    struct arena *a=cookie;
    (void)block;
    ++a->release_calls;
    return a->fail_release?1:0;
}
static int barrier_cb(void *cookie)
{
    struct arena *a=cookie;
    ++a->barrier_calls;
    return 0;
}
static int reserved_cb(void *cookie,const struct gga_request *request,struct gga_block *out)
{
    struct arena *a=cookie;
    ++a->reserved_calls;
    if (a->reserved_fail) return 1;
    out->backing=(void *)a->words;
    out->bytes=request->size;
    out->effective_flags=request->flags;
    switch (request->kind) {
    case GGA_KIND_MEMSTORE:
        out->gpu_addr=0;
        out->gmu_addr=a->gmu_base+0x8000;
        return 0;
    case GGA_KIND_HW_FENCE:
        out->gpu_addr=a->gpu_base+0x4000;
        out->gmu_addr=a->gmu_base+0x4000;
        return 0;
    default:
        return 1;
    }
}

static void arena_init(struct arena *a,unsigned cap_words)
{
    memset(a,0,sizeof(*a));
    a->words=arena_words;
    a->cap_words=cap_words?cap_words:sizeof(arena_words)/sizeof(arena_words[0]);
    a->gpu_base=0x61000000ull;
    a->gmu_base=0x60000000ull;
}
static struct gga_ops ops_of(struct arena *a,int reserved)
{
    struct gga_ops ops={a,reserve_cb,publish_cb,release_cb,barrier_cb,reserved?reserved_cb:0};
    return ops;
}

/* Build the F2H packet the firmware sends. Both versions keep the descriptor
 * at word 2; version 1 adds the align word at the end. */
static unsigned packet(uint32_t *words,uint32_t version,uint32_t flags,uint32_t kind,
    uint32_t gmu_handle,uint32_t size,uint32_t align)
{
    unsigned count=version?GGA_HDR_WORDS:GGA_LEGACY_WORDS;
    memset(words,0,sizeof(uint32_t)*12);
    words[0]=GGA_MSG_ID|(count<<8)|(7u<<20);
    words[1]=version;
    words[4]=flags;
    words[5]=kind;
    words[6]=0;
    words[7]=gmu_handle;
    words[8]=0;
    words[9]=size;
    if (version) words[10]=align;
    return count;
}

static void test_parse(void)
{
    /* The exact packet recorded from the device: START's first SCRATCH ask. */
    const uint32_t observed[11]={0x00700b14u,1u,0,0,0x33fu,2u,0,2u,0,4096u,0};
    uint32_t words[12];
    struct gga_request request;
    unsigned count;
    assert(gga_parse(observed,11,&request)==GGA_OK);
    assert(request.hdr==0x00700b14u && request.version==1);
    assert(((request.hdr>>20)&0xfffu)==7 && ((request.hdr>>8)&0xffu)==11);
    assert(request.flags==0x33fu && request.kind==GGA_KIND_SCRATCH);
    assert(request.gmu_handle==2 && request.size==4096 && request.align==0);
    assert(request.gpu_alignment==GGA_MIN_ALIGN && request.size_alignment==GGA_MIN_ALIGN);
    count=packet(words,0,0x33fu,GGA_KIND_RB,7,4096,0);
    assert(count==GGA_LEGACY_WORDS && gga_parse(words,count,&request)==GGA_OK);
    assert(request.version==0 && request.align==0 && request.kind==GGA_KIND_RB);
    assert(gga_parse(0,11,&request)==GGA_ARGUMENT);
    assert(gga_parse(observed,11,0)==GGA_ARGUMENT);
    ++cases;
}

static void test_scratch_allocation(void)
{
    struct arena a;
    struct gga_state state={0};
    struct gga_ops ops;
    uint32_t words[12],reply[16];
    unsigned count,reply_words=0;
    arena_init(&a,0);
    ops=ops_of(&a,0);
    count=packet(words,1,0x33fu,GGA_KIND_SCRATCH,2,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_OK);
    assert(reply_words==GGA_HDR_WORDS);
    assert((reply[0]&0xffu)==GGA_MSG_ID && ((reply[0]>>16)&0xfu)==GGA_ACK_TYPE);
    assert(((reply[0]>>8)&0xffu)==GGA_HDR_WORDS && ((reply[0]>>20)&0xfffu)==1);
    assert(reply[1]==words[0]);
    assert(reply[2]==0x61000000u && reply[3]==0);      /* GPU VA from the provider */
    assert(reply[4]==0x33fu && reply[5]==GGA_KIND_SCRATCH);
    assert(reply[6]==2 && reply[7]==2);                /* host handle echoes GMU handle */
    assert(reply[8]==0x60000000u && reply[9]==4096 && reply[10]==0);
    assert(a.reserve_calls==1 && a.publish_calls==1 && a.release_calls==0);
    assert(state.count==1 && state.allocated==1 && state.published==1);
    assert(state.entry[0].block.effective_flags==0x33fu);
    ++cases;
}

static void test_duplicate_and_conflict(void)
{
    struct arena a;
    struct gga_state state={0};
    struct gga_ops ops;
    uint32_t words[12],reply[16];
    unsigned count,reply_words=0;
    arena_init(&a,0);
    ops=ops_of(&a,0);
    count=packet(words,1,0x33fu,GGA_KIND_SCRATCH,2,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_OK);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_OK);
    assert(state.count==1 && state.reused==1 && a.reserve_calls==1 && a.publish_calls==1);
    assert(reply[2]==0x61000000u && reply[8]==0x60000000u);
    assert(((reply[0]>>20)&0xfffu)==2); /* a retry is answered with a fresh sequence */
    /* Same key, different size: refuse instead of sharing one buffer. */
    count=packet(words,1,0x33fu,GGA_KIND_SCRATCH,2,8192,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_CONFLICT);
    assert(state.count==1 && a.reserve_calls==1 && reply_words==0);
    /* Same key, different flags: also a conflict. */
    count=packet(words,1,0x33fu|GGA_FLAG_GMU_CACHEABLE,GGA_KIND_SCRATCH,2,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_CONFLICT);
    ++cases;
}

static void test_legacy(void)
{
    struct arena a;
    struct gga_state state={0};
    struct gga_ops ops;
    uint32_t words[12],reply[16];
    unsigned count,reply_words=0;
    arena_init(&a,0);
    ops=ops_of(&a,0);
    count=packet(words,0,0x33fu,GGA_KIND_RB,7,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_OK);
    assert(reply_words==GGA_LEGACY_WORDS);
    assert(((reply[0]>>8)&0xffu)==GGA_LEGACY_WORDS);
    assert(reply[9]==4096);
    assert(state.count==1);
    ++cases;
}

static void test_malformed(void)
{
    struct gga_request request;
    uint32_t words[12];
    unsigned count;
    count=packet(words,1,0x33fu,GGA_KIND_SCRATCH,2,4096,0);

    words[0]=(words[0]&~0xffu)|19u;
    assert(gga_parse(words,count,&request)==GGA_HEADER);
    count=packet(words,1,0x33fu,GGA_KIND_SCRATCH,2,4096,0);
    words[0]|=1u<<16; /* a request must be HFI_MSG_CMD */
    assert(gga_parse(words,count,&request)==GGA_HEADER);
    count=packet(words,1,0x33fu,GGA_KIND_SCRATCH,2,4096,0);
    words[0]=(words[0]&~(0xffu<<8))|(10u<<8); /* size field disagrees with count */
    assert(gga_parse(words,count,&request)==GGA_HEADER);
    count=packet(words,1,0x33fu,GGA_KIND_SCRATCH,2,4096,0);
    assert(gga_parse(words,count-1,&request)==GGA_HEADER);
    count=packet(words,0,0x33fu,GGA_KIND_SCRATCH,2,4096,0);
    assert(gga_parse(words,count+1,&request)==GGA_HEADER);
    count=packet(words,1,0x33fu,GGA_KIND_MAX,2,4096,0);
    assert(gga_parse(words,count,&request)==GGA_KIND);
    count=packet(words,1,0x33fu|0x2000u,GGA_KIND_SCRATCH,2,4096,0);
    assert(gga_parse(words,count,&request)==GGA_FLAGS);
    count=packet(words,1,0x33fu|GGA_FLAG_GFX_SECURE,GGA_KIND_SCRATCH,2,4096,0);
    assert(gga_parse(words,count,&request)==GGA_SECURE);
    count=packet(words,1,0x33fu,GGA_KIND_SCRATCH,2,0,0);
    assert(gga_parse(words,count,&request)==GGA_SIZE);
    count=packet(words,1,0x33fu,GGA_KIND_SCRATCH,2,4096,31);
    assert(gga_parse(words,count,&request)==GGA_ALIGN);
    ++cases;
}

static void test_reserved_kinds(void)
{
    struct arena a;
    struct gga_state state={0};
    struct gga_ops ops;
    uint32_t words[12],reply[16];
    unsigned count,reply_words=0;
    static const uint32_t reserved[]={GGA_KIND_CSW_PRIV_SECURE,GGA_KIND_MMIO_IPC_CORE,
        GGA_KIND_MEMSTORE,GGA_KIND_HW_FENCE};
    unsigned i;
    arena_init(&a,0);
    ops=ops_of(&a,0);
    for (i=0;i<sizeof(reserved)/sizeof(reserved[0]);++i) {
        count=packet(words,1,0x33fu,reserved[i],i+40,4096,0);
        assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_RESERVED);
        assert(reply_words==0 && state.count==0 && a.reserve_calls==0);
    }
    ++cases;
    /* With a dedicated backend the memstore and fence kinds are accepted. */
    arena_init(&a,0);
    ops=ops_of(&a,1);
    count=packet(words,1,0x33fu&~GGA_FLAG_GFX_ACC,GGA_KIND_MEMSTORE,3,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_OK);
    assert(reply[2]==0 && reply[8]==0x60008000u && reply[6]==3 && reply[7]==3);
    count=packet(words,1,0x33fu,GGA_KIND_HW_FENCE,4,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_OK);
    assert(reply[2]==0x61004000u && reply[8]==0x60004000u);
    assert(a.reserved_calls==2 && a.reserve_calls==0);
    ++cases;
    a.reserved_fail=1;state.count=0;reply_words=7;
    count=packet(words,1,0x33fu,GGA_KIND_HW_FENCE,5,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_PROVIDER);
    assert(reply_words==0 && state.count==0);
    ++cases;
}

static void test_gmu_only(void)
{
    struct arena a;
    struct gga_state state={0};
    struct gga_ops ops;
    uint32_t words[12],reply[16];
    unsigned count,reply_words=0;
    arena_init(&a,0);
    ops=ops_of(&a,0);
    count=packet(words,1,GGA_FLAG_HOST_ACC|GGA_FLAG_HOST_INIT,GGA_KIND_SCRATCH,9,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_OK);
    /* Vendor semantics: no GFX access means GMU-only, and the GMU bits are
     * folded in even though the request never asked for them. */
    assert(reply[2]==0 && reply[3]==0);
    assert(reply[8]==0x60000000u);
    assert(state.entry[0].block.effective_flags==(GGA_FLAG_HOST_ACC|GGA_FLAG_HOST_INIT|GGA_FLAG_GMU_ONLY_FOLD));
    assert(reply[4]==(GGA_FLAG_HOST_ACC|GGA_FLAG_HOST_INIT)); /* reply keeps the request flags */
    ++cases;
    /* A provider that disagrees about the effective flags is refused. */
    arena_init(&a,0);
    a.lie_flags=1;
    state.count=0;
    count=packet(words,1,GGA_FLAG_HOST_ACC|GGA_FLAG_HOST_INIT,GGA_KIND_SCRATCH,9,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_PROVIDER);
    assert(state.count==0 && a.release_calls==1 && state.quarantined==0);
    ++cases;
}

static void test_gfx_mapping_rules(void)
{
    struct arena a;
    struct gga_state state={0};
    struct gga_ops ops;
    uint32_t words[12],reply[16];
    unsigned count,reply_words=0;
    /* GFX access without GMU_ACC must not publish a GMU address. */
    arena_init(&a,0);
    ops=ops_of(&a,0);
    count=packet(words,1,GGA_FLAG_GFX_ACC|GGA_FLAG_GFX_WRITE,GGA_KIND_CMD_BUFFER,10,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_OK);
    assert(reply[2]!=0 && reply[8]==0);
    ++cases;
    /* Forcing one anyway is a provider bug and must be refused. */
    arena_init(&a,0);
    a.force_gmu=1;
    state.count=0;
    count=packet(words,1,GGA_FLAG_GFX_ACC|GGA_FLAG_GFX_WRITE,GGA_KIND_CMD_BUFFER,10,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_PROVIDER);
    assert(state.count==0 && a.release_calls==1);
    ++cases;
    /* GMU-only memory must not carry a GPU alias. */
    arena_init(&a,0);
    state.count=0;
    a.force_gmu=0;
    count=packet(words,1,GGA_FLAG_HOST_ACC|GGA_FLAG_HOST_INIT,GGA_KIND_SYS_LOG,11,4096,0);
    a.add_bad_gpu=1;
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_PROVIDER);
    assert(state.count==0 && a.release_calls==1);
    ++cases;
}

static void test_alignment(void)
{
    struct arena a;
    struct gga_state state={0};
    struct gga_ops ops;
    uint32_t words[12],reply[16];
    unsigned count,reply_words=0;
    arena_init(&a,0);
    ops=ops_of(&a,0);
    /* bits[0:7] VA alignment 2^12 (4 KiB), bits[8:15] size alignment 2^16. */
    count=packet(words,1,0x33fu,GGA_KIND_SCRATCH,12,4096,12u|(16u<<8));
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_OK);
    assert(reply[9]==65536u);          /* real size is rounded up */
    assert(reply[10]==(12u|(16u<<8))); /* request align is preserved */
    assert(a.used==65536u);
    assert(state.entry[0].block.bytes==65536u);
    ++cases;
}

static void test_exhaustion(void)
{
    struct arena a;
    struct gga_state state={0};
    struct gga_ops ops;
    uint32_t words[12],reply[16];
    unsigned count,reply_words=0;
    arena_init(&a,1024); /* 4 KiB: room for exactly one 4 KiB block */
    ops=ops_of(&a,0);
    count=packet(words,1,0x33fu,GGA_KIND_SCRATCH,20,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_OK);
    count=packet(words,1,0x33fu,GGA_KIND_CMD_BUFFER,21,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_PROVIDER);
    assert(state.count==1 && state.refused==1 && a.publish_calls==1 && reply_words==0);
    ++cases;
}

static void test_publish_and_release(void)
{
    struct arena a;
    struct gga_state state={0};
    struct gga_ops ops;
    uint32_t words[12],reply[16];
    unsigned count,reply_words=0;
    arena_init(&a,0);
    ops=ops_of(&a,0);
    a.fail_publish=1;
    count=packet(words,1,0x33fu,GGA_KIND_SCRATCH,30,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_PUBLISH);
    assert(a.release_calls==1 && state.count==0 && state.quarantined==0);
    ++cases;
    /* A block that was never published is released; a refused release means
     * the provider may still own it, so the responder quarantines. */
    arena_init(&a,0);
    a.fail_publish=1;
    a.fail_release=1;
    state.count=0;
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_RELEASE);
    assert(state.quarantined==1 && state.count==0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_QUARANTINED);
    ++cases;
}

static void test_invalid_blocks(void)
{
    struct arena a;
    struct gga_state state={0};
    struct gga_ops ops;
    uint32_t words[12],reply[16];
    unsigned count,reply_words=0;
    int scenario;
    for (scenario=0;scenario<3;++scenario) {
        arena_init(&a,0);
        ops=ops_of(&a,0);
        state.count=0;
        a.add_bad_gpu=scenario==0;
        a.add_bad_gmu=scenario==1;
        a.short_bytes=scenario==2;
        count=packet(words,1,0x33fu,GGA_KIND_SCRATCH,50+scenario,4096,0);
        assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_PROVIDER);
        assert(state.count==0 && a.release_calls==1 && state.quarantined==0);
    }
    ++cases;
}

static void test_capacity_and_table(void)
{
    struct arena a;
    struct gga_state state={0};
    struct gga_ops ops;
    uint32_t words[12],reply[16];
    unsigned count,reply_words=0,i;
    arena_init(&a,0);
    ops=ops_of(&a,0);
    count=packet(words,1,0x33fu,GGA_KIND_SCRATCH,60,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,10,&reply_words)==GGA_ARGUMENT);
    assert(state.count==0 && a.reserve_calls==0 && reply_words==0);
    ++cases;
    /* One entry per (kind, gmu_handle); the table refuses beyond GGA_MAX. */
    for (i=0;i<GGA_MAX;++i) {
        count=packet(words,1,0x33fu,GGA_KIND_SCRATCH,100+i,4096,0);
        assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_OK);
    }
    assert(state.count==GGA_MAX && a.reserve_calls==GGA_MAX);
    count=packet(words,1,0x33fu,GGA_KIND_SCRATCH,999,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_FULL);
    assert(state.count==GGA_MAX && a.reserve_calls==GGA_MAX && reply_words==0);
    ++cases;
}

static void test_sequence_wrap(void)
{
    struct arena a;
    struct gga_state state={0};
    struct gga_ops ops;
    uint32_t words[12],reply[16];
    unsigned count,reply_words=0;
    arena_init(&a,0);
    ops=ops_of(&a,0);
    state.seq=0xfffu;
    count=packet(words,1,0x33fu,GGA_KIND_SCRATCH,70,4096,0);
    assert(gga_handle(&ops,&state,words,count,reply,16,&reply_words)==GGA_OK);
    assert(((reply[0]>>20)&0xfffu)==1);
    assert(gga_alignment(12)==4096 && gga_alignment(13)==8192 && gga_alignment(31)==0);
    assert(!strcmp(gga_kind_name(GGA_KIND_SCRATCH),"scratch"));
    assert(!strcmp(gga_code_name(GGA_CONFLICT),"conflict"));
    ++cases;
}

int main(void)
{
    test_parse();
    test_scratch_allocation();
    test_duplicate_and_conflict();
    test_legacy();
    test_malformed();
    test_reserved_kinds();
    test_gmu_only();
    test_gfx_mapping_rules();
    test_alignment();
    test_exhaustion();
    test_publish_and_release();
    test_invalid_blocks();
    test_capacity_and_table();
    test_sequence_wrap();
    printf("HFI ALLOC FIXTURE: %u cases passed (allocation policy only; no hardware evidence)\n",cases);
    return 0;
}
