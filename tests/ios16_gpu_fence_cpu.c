/* Actual envelope/import code, synthetic mapper/ownership fixtures only.
 * No physical memory mapping, carveout access, PTE change or device I/O. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "../ios16/gpu/giulia_gpu_fence_cpu.h"

struct fixture {
    struct ggfc_state state;
    struct ggfc_owner owner;
    struct ggf_layout layout;
    struct ggfc_envelope envelope;
    unsigned char *memory;
    unsigned observe_calls,map_calls,audit_calls,reenter,loss_on;
    unsigned source_bad,source_bad_at,map_bad,audit_bad,handle_token;
};
static void reentry(struct fixture *f)
{
    if(!f->reenter)return;
    struct ggfc_mapping out;
    assert(ggfc_import(&f->state,&f->owner,&f->layout,16384)==GGFC_BUSY);
    assert(ggfc_get(&f->state,&out)==GGFC_BUSY);
}
static int observe(void *cookie,const struct ggfc_envelope *e,struct ggfc_source *out)
{
    struct fixture *f=cookie;++f->observe_calls;reentry(f);
    assert(ggfc_envelope_valid(e));
    if(f->loss_on==1)assert(ggfc_invalidate(&f->state)==GGFC_LOST);
    memset(out,0,sizeof(*out));out->identity=f;out->epoch=37;
    out->carveout_physical=GGF_CARVEOUT_PHYSICAL;out->carveout_bytes=GGF_CARVEOUT_BYTES;
    out->envelope_physical=e->physical;out->envelope_bytes=e->bytes;
    out->self_vmid=3;out->peer_vmid=7;out->parcel_handle=0;
    out->layout=f->layout;out->proofs=GGFC_SOURCE_REQUIRED;
    if(f->source_bad_at && f->source_bad_at!=f->observe_calls)return 1;
    switch(f->source_bad) {
    case 1:return 0;
    case 2:out->identity=&f->handle_token;break;
    case 3:out->epoch++;break;
    case 4:out->carveout_physical+=4096;break;
    case 5:out->carveout_bytes-=4096;break;
    case 6:out->envelope_physical+=4096;break;
    case 7:out->envelope_bytes-=4096;break;
    case 8:out->layout.physical+=4096;break;
    case 9:out->layout.page_bytes=16384;break;
    case 10:out->self_vmid=0;break;
    case 11:out->peer_vmid=out->self_vmid;break;
    case 12:out->proofs^=GGFC_SHARED;break;
    case 13:out->proofs^=GGFC_CPU_RANGE;break;
    case 14:out->proofs^=GGFC_CACHE_OWNER;break;
    case 15:out->peer_vmid++;break;
    case 16:out->parcel_handle++;break;
    case 17:out->proofs^=GGFC_READY;break;
    }
    return 1;
}
static int map(void *cookie,const struct ggfc_envelope *e,struct ggfc_mapping *out)
{
    struct fixture *f=cookie;++f->map_calls;reentry(f);
    assert(f->state.retained && ggfc_envelope_valid(e));
    out->handle=&f->handle_token;out->base=f->memory;out->queue=f->memory+e->offset;
    out->physical=e->physical;out->bytes=e->bytes;out->local_page_bytes=e->local_page_bytes;
    if(f->loss_on==2)assert(ggfc_invalidate(&f->state)==GGFC_LOST);
    switch(f->map_bad) {
    case 1:return 0;
    case 2:out->handle=NULL;break;
    case 3:out->base=f->memory+1;break;
    case 4:out->queue=(void *)f->memory;break;
    case 5:out->physical+=4096;break;
    case 6:out->bytes+=16384;break;
    case 7:out->local_page_bytes=4096;break;
    case 8:out->handle=f->memory;break;
    case 9:out->base=(void *)(UINTPTR_MAX-4095);break;
    case 10:out->base=(void *)&f->state;break;
    }
    return 1;
}
static int audit(void *cookie,const struct ggfc_envelope *e,const struct ggfc_mapping *m,
    struct ggfc_cpu_audit *out)
{
    struct fixture *f=cookie;++f->audit_calls;reentry(f);assert(ggfc_envelope_valid(e));
    out->mapping=*m;out->proofs=GGFC_AUDIT_REQUIRED;
    if(f->loss_on==3)assert(ggfc_invalidate(&f->state)==GGFC_LOST);
    switch(f->audit_bad) {
    case 1:return 0;
    case 2:out->proofs^=GGFC_NORMAL_NC;break;
    case 3:out->proofs^=GGFC_NO_WB_ALIAS;break;
    case 4:out->proofs^=GGFC_EXACT_PA;break;
    case 5:out->proofs^=GGFC_PINNED;break;
    case 6:out->mapping.physical+=4096;break;
    case 7:out->mapping.bytes-=4096;break;
    case 8:out->mapping.queue=(void *)f->memory;break;
    case 9:out->mapping.handle=f;break;
    }
    return 1;
}
static void setup(struct fixture *f)
{
    memset(f,0,sizeof(*f));assert(ggf_layout(4096,&f->layout));
    assert(ggfc_envelope(&f->layout,16384,&f->envelope));
    f->memory=aligned_alloc(16384,f->envelope.bytes);assert(f->memory);
    memset(f->memory,0xa7,f->envelope.bytes);
    f->owner=(struct ggfc_owner){f,37,f,observe,map,audit};
}
static void cleanup(struct fixture *f)
{
    for(unsigned i=0;i<f->envelope.bytes;i++)assert(f->memory[i]==0xa7);
    free(f->memory);
}
static void failed(struct fixture *f,int retained)
{
    assert(f->state.state==GGFC_BROKEN && f->state.loss && f->state.retained==(unsigned)retained);
    unsigned maps=f->map_calls,observes=f->observe_calls,audits=f->audit_calls;
    struct ggfc_mapping out,saved;memset(&out,0x6b,sizeof(out));saved=out;
    assert(ggfc_get(&f->state,&out)==GGFC_QUARANTINED && !memcmp(&out,&saved,sizeof(out)));
    assert(ggfc_import(&f->state,&f->owner,&f->layout,16384)==GGFC_QUARANTINED);
    assert(f->map_calls==maps && f->observe_calls==observes && f->audit_calls==audits);
    if(retained)assert(f->state.mapping.handle==&f->handle_token || f->map_bad);
}
static void geometry(void)
{
    struct ggf_layout layout;struct ggfc_envelope e,saved;
    for(unsigned provider=4096;provider<=65536;provider*=4)for(unsigned local=4096;local<=16384;local*=4) {
        assert(ggf_layout(provider,&layout) && ggfc_envelope(&layout,local,&e));
        assert(ggfc_envelope_valid(&e) && !(e.physical&(local-1)) && !(e.bytes&(local-1)));
        assert(e.physical+e.offset==layout.physical && e.offset==e.prefix_bytes);
        assert(e.offset+layout.bytes+e.suffix_bytes==e.bytes);
        assert(e.physical>=GGF_CARVEOUT_PHYSICAL &&
            e.physical+e.bytes<=GGF_CARVEOUT_PHYSICAL+GGF_CARVEOUT_BYTES);
    }
    assert(ggf_layout(4096,&layout) && ggfc_envelope(&layout,16384,&e));
    assert(layout.physical==UINT64_C(0xd4f6d000) && layout.bytes==0x1a000);
    assert(e.physical==UINT64_C(0xd4f6c000) && e.bytes==0x1c000 && e.offset==4096 && e.suffix_bytes==4096);
    saved=e;layout.physical-=4096;
    assert(!ggfc_envelope(&layout,16384,&e) && !memcmp(&e,&saved,sizeof(e)));
    assert(ggf_layout(4096,&layout));
    const unsigned bad[]={0,1,4095,8192,65536,UINT32_MAX};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++)
        assert(!ggfc_envelope(&layout,bad[i],&e) && !memcmp(&e,&saved,sizeof(e)));
    for(unsigned field=0;field<5;field++) {
        e=saved;
        if(field==0)e.bytes+=16384;
        if(field==1)e.offset+=4096;
        if(field==2)e.prefix_bytes=0;
        if(field==3)e.suffix_bytes=0;
        if(field==4)e.physical-=16384;
        assert(!ggfc_envelope_valid(&e));
    }
}
static void normal(void)
{
    struct fixture f;setup(&f);f.reenter=1;
    assert(ggfc_import(&f.state,&f.owner,&f.layout,16384)==GGFC_OK);
    assert(f.map_calls==1 && f.state.retained && f.state.state==GGFC_MAPPED);
    memset(&f.owner,0,sizeof(f.owner)); /* Frozen callbacks, never the source descriptor. */
    struct ggfc_mapping out;
    assert(ggfc_get(&f.state,&out)==GGFC_OK && out.handle==&f.handle_token &&
        out.base==f.memory && out.queue==f.memory+4096 && out.physical==f.envelope.physical && out.bytes==0x1c000);
    assert(ggfc_get(&f.state,&f.state.mapping)==GGFC_ARGUMENT);
    assert(ggfc_get(&f.state,(struct ggfc_mapping *)f.memory)==GGFC_ARGUMENT);
    assert(ggfc_get(&f.state,(struct ggfc_mapping *)(f.memory+f.envelope.bytes-sizeof(out)))==GGFC_ARGUMENT);
    assert(ggfc_invalidate(&f.state)==GGFC_LOST);f.owner=f.state.owner;failed(&f,1);cleanup(&f);
}
static void failures(void)
{
    for(unsigned fault=1;fault<=17;fault++) {
        if(fault==15 || fault==16)continue;
        struct fixture f;setup(&f);f.source_bad=fault;
        assert(ggfc_import(&f.state,&f.owner,&f.layout,16384)==GGFC_LOST && !f.map_calls);
        failed(&f,0);cleanup(&f);
    }
    for(unsigned fault=1;fault<=10;fault++) {
        struct fixture f;setup(&f);f.map_bad=fault;
        assert(ggfc_import(&f.state,&f.owner,&f.layout,16384)!=GGFC_OK && f.map_calls==1);
        failed(&f,1);cleanup(&f);
    }
    for(unsigned fault=1;fault<=9;fault++) {
        struct fixture f;setup(&f);f.audit_bad=fault;
        assert(ggfc_import(&f.state,&f.owner,&f.layout,16384)==GGFC_AUDIT && f.map_calls==1);
        failed(&f,1);cleanup(&f);
    }
    for(unsigned fault=1;fault<=17;fault++) {
        struct fixture f;setup(&f);assert(ggfc_import(&f.state,&f.owner,&f.layout,16384)==GGFC_OK);
        f.source_bad=fault;
        struct ggfc_mapping out,saved;memset(&out,0x45,sizeof(out));saved=out;
        assert(ggfc_get(&f.state,&out)==GGFC_AUDIT && !memcmp(&out,&saved,sizeof(out)));
        failed(&f,1);cleanup(&f);
    }
    for(unsigned call=2;call<=4;call++) {
        struct fixture f;setup(&f);f.source_bad=16;f.source_bad_at=call;
        assert(ggfc_import(&f.state,&f.owner,&f.layout,16384)!=GGFC_OK);failed(&f,1);cleanup(&f);
    }
    for(unsigned stage=1;stage<=3;stage++) {
        struct fixture f;setup(&f);f.loss_on=stage;
        assert(ggfc_import(&f.state,&f.owner,&f.layout,16384)!=GGFC_OK);
        failed(&f,stage!=1);cleanup(&f);
    }
}
static void arguments(void)
{
    struct fixture f;setup(&f);struct ggfc_mapping out;
    assert(ggfc_get(&f.state,&out)==GGFC_ARGUMENT && !f.observe_calls);
    assert(ggfc_invalidate(&f.state)==GGFC_ARGUMENT);
    for(unsigned mode=0;mode<8;mode++) {
        struct ggfc_owner owner=f.owner;struct ggf_layout layout=f.layout;
        unsigned page=16384;
        if(mode==0)owner.identity=NULL;
        if(mode==1)owner.epoch=0;
        if(mode==2)owner.observe=NULL;
        if(mode==3)owner.map=NULL;
        if(mode==4)owner.audit=NULL;
        if(mode==5)layout.bytes-=4096;
        if(mode==6)page=8192;
        if(mode==7)layout.physical=GGF_CARVEOUT_PHYSICAL;
        assert(ggfc_import(&f.state,&owner,&layout,page)==GGFC_ARGUMENT);
        assert(f.state.state==GGFC_EMPTY && !f.map_calls && !f.observe_calls && !f.audit_calls);
    }
    assert(ggfc_import(&f.state,&f.owner,&f.layout,16384)==GGFC_OK);
    unsigned observations=f.observe_calls;
    assert(ggfc_import(&f.state,&f.owner,&f.layout,16384)==GGFC_ALREADY && f.map_calls==1 && observations==f.observe_calls);
    cleanup(&f);
}
struct contender {struct fixture *f;unsigned result;};
static void *import_thread(void *cookie)
{
    struct contender *c=cookie;
    c->result=(unsigned)ggfc_import(&c->f->state,&c->f->owner,&c->f->layout,16384);return NULL;
}
static void races(void)
{
    for(unsigned round=0;round<100;round++) {
        struct fixture f;setup(&f);pthread_t threads[4];struct contender candidates[4];
        for(unsigned i=0;i<4;i++) {
            candidates[i]=(struct contender){&f,99};
            assert(!pthread_create(&threads[i],NULL,import_thread,&candidates[i]));
        }
        unsigned success=0;
        for(unsigned i=0;i<4;i++) {
            assert(!pthread_join(threads[i],NULL));
            assert(candidates[i].result==GGFC_OK || candidates[i].result==GGFC_BUSY || candidates[i].result==GGFC_ALREADY);
            success+=candidates[i].result==GGFC_OK;
        }
        assert(success==1 && f.map_calls==1 && f.state.state==GGFC_MAPPED);
        struct ggfc_mapping out;assert(ggfc_get(&f.state,&out)==GGFC_OK);cleanup(&f);
    }
}
int main(void)
{
    geometry();normal();failures();arguments();races();
    puts("GPU fence CPU: exact 4K/16K envelopes, neighboring-page authority, real-mapper audit contract, live identity, retained failures, 400 competing imports and quarantine passed (synthetic owner only)");
    return 0;
}
