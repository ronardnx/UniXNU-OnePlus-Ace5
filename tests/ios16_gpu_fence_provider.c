/* Synthetic provider metadata only. No Gunyah/READY producer is installed. */
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../ios16/gpu/giulia_gpu_fence_provider.h"
static unsigned observations;
static struct ggf_proof live_proof;
static void barrier(void *cookie){(void)cookie;assert(!"Registration must not inspect queues");}
static int observe(void *cookie,struct ggf_proof *out)
{(void)cookie;++observations;*out=live_proof;return 1;}
static struct ggf_registration registration;
static struct ggf_provider candidates[4];
static unsigned successes;
static void *register_one(void *cookie)
{
    if(ggfp_register(&registration,(const struct ggf_provider *)cookie))
        __atomic_add_fetch(&successes,1u,__ATOMIC_RELAXED);
    return NULL;
}
int main(void)
{
    struct ggf_layout layout;assert(ggf_layout(4096,&layout));
    void *memory=calloc(1,layout.bytes);assert(memory);
    struct ggf_provider good={GGFP_ABI,sizeof(good),&layout,NULL,1,layout,memory,observe,barrier};
    for(unsigned bad=0;bad<9;bad++) {
        struct ggf_provider copy=good;
        if(bad==0)copy.abi=0;if(bad==1)copy.bytes--;
        if(bad==2)copy.identity=NULL;if(bad==3)copy.epoch=0;
        if(bad==4)copy.memory=NULL;if(bad==5)copy.observe=NULL;
        if(bad==6)copy.barrier=NULL;if(bad==7)copy.layout.physical+=4096;
        if(bad==8)copy.memory=(void *)(UINTPTR_MAX-3);
        assert(!ggfp_register(&registration,&copy) && !ggfp_get(&registration));
    }
    pthread_t threads[4];
    for(unsigned i=0;i<4;i++) {
        candidates[i]=good;candidates[i].epoch=i+1;candidates[i].identity=&candidates[i];
        assert(!pthread_create(&threads[i],NULL,register_one,&candidates[i]));
    }
    for(unsigned i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
    const struct ggf_provider *saved=ggfp_get(&registration);
    assert(saved && successes==1 && !observations && !ggfp_register(&registration,&good));
    uint64_t epoch=saved->epoch;const void *identity=saved->identity;
    memset(candidates,0,sizeof(candidates));assert(saved->epoch==epoch && saved->identity==identity);
    live_proof=(struct ggf_proof){epoch,GGF_CARVEOUT_PHYSICAL,layout.physical,
        GGF_CARVEOUT_BYTES,layout.bytes,4096,3,5,6,1,GGFP_REQUIRED};
    assert(ggfp_live(saved));
    for(unsigned bit=0;bit<7;bit++) {
        live_proof.proofs=GGFP_REQUIRED^(1u<<bit);assert(!ggfp_live(saved));
    }
    live_proof.proofs=GGFP_REQUIRED;
    for(unsigned i=0;i<8;i++) {
        unsigned char *field=(unsigned char *)&live_proof+offsetof(struct ggf_proof,carveout_bytes)+i*4;
        uint32_t original,changed;memcpy(&original,field,4);changed=original^1u;memcpy(field,&changed,4);
        assert(!ggfp_live(saved));memcpy(field,&original,4);
    }
    live_proof.epoch++;assert(!ggfp_live(saved));live_proof.epoch--;
    live_proof.queue_physical+=4096;assert(!ggfp_live(saved));
    free(memory);
    puts("Fence registration: copied identity, one concurrent winner, no queue access, exact live-proof refusal passed (synthetic metadata only)");
}
