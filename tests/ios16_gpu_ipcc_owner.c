/* Shared host membership / exclusive GMU aperture concurrency model. */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include "../ios16/giulia_gpu_ipcc_owner.h"
static struct ggi_owner state;
static unsigned tokens[4],using_host[2],using_gpu,ready,start;
static void *client(void *cookie)
{
    unsigned id=*(unsigned *)cookie;
    __atomic_add_fetch(&ready,1u,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&start,__ATOMIC_ACQUIRE))sched_yield();
    for(unsigned i=0;i<12000;i++) {
        if(!ggi_claim(&state,id,&tokens[id])){sched_yield();continue;}
        assert(ggi_owns(&state,id,&tokens[id]));
        if(id==GGI_GPU) {
            __atomic_store_n(&using_gpu,1u,__ATOMIC_RELEASE);
            assert(!__atomic_load_n(&using_host[0],__ATOMIC_ACQUIRE));
            assert(!__atomic_load_n(&using_host[1],__ATOMIC_ACQUIRE));
            __atomic_store_n(&using_gpu,0u,__ATOMIC_RELEASE);
        } else {
            __atomic_store_n(&using_host[id-1],1u,__ATOMIC_RELEASE);
            assert(!__atomic_load_n(&using_gpu,__ATOMIC_ACQUIRE));
            __atomic_store_n(&using_host[id-1],0u,__ATOMIC_RELEASE);
        }
        while(!ggi_release(&state,id,&tokens[id])) {
            assert(id==GGI_GPU && ggi_owns(&state,id,&tokens[id]));sched_yield();
        }
    }
    return NULL;
}
int main(void)
{
    for(unsigned id=0;id<5;id++) {
        if(id>=1 && id<=3)continue;
        assert(!ggi_claim(&state,id,&tokens[0]) && !ggi_owns(&state,id,&tokens[0]));
        assert(!ggi_release(&state,id,&tokens[0]));
    }
    assert(!ggi_claim(NULL,1,&tokens[1]) && !ggi_claim(&state,1,NULL));
    assert(ggi_claim(&state,1,&tokens[1]) && ggi_claim(&state,2,&tokens[2]));
    assert(!ggi_claim(&state,1,&tokens[1]) && !ggi_claim(&state,3,&tokens[3]));
    assert(!ggi_release(&state,1,&tokens[0]) && ggi_owns(&state,1,&tokens[1]));
    state.gate=1;assert(ggi_release(&state,1,&tokens[1])); // Rollback cannot fail as busy.
    assert(!ggi_claim(&state,3,&tokens[3]));state.gate=0;
    assert(!ggi_claim(&state,3,&tokens[3]) && ggi_release(&state,2,&tokens[2]));
    assert(ggi_claim(&state,3,&tokens[3]));
    assert(!ggi_claim(&state,1,&tokens[1]) && !ggi_claim(&state,2,&tokens[2]));
    assert(ggi_release(&state,3,&tokens[3]));
    pthread_t threads[3];unsigned ids[3]={1,2,3};
    for(unsigned i=0;i<3;i++)assert(!pthread_create(&threads[i],NULL,client,&ids[i]));
    while(__atomic_load_n(&ready,__ATOMIC_ACQUIRE)!=3)sched_yield();
    __atomic_store_n(&start,1u,__ATOMIC_RELEASE);
    for(unsigned i=0;i<3;i++)assert(!pthread_join(threads[i],NULL));
    assert(!state.gate && !state.owners[0] && !state.owners[1] && !state.owners[2]);
    assert(ggi_claim(&state,GGI_GPU,&tokens[3]));
    assert(!ggi_pair_fence(&state,&tokens[2],&tokens[0],1));
    assert(ggi_pair_fence(&state,&tokens[3],&tokens[0],1));
    assert(ggi_pair_fence(&state,&tokens[3],&tokens[0],1));
    assert(!ggi_pair_fence(&state,&tokens[3],&tokens[0],2));
    assert(!ggi_pair_fence(&state,&tokens[3],&tokens[1],1));
    assert(ggi_fence_owns(&state,&tokens[3],&tokens[0],1));
    assert(!ggi_release(&state,GGI_GPU,&tokens[3]));
    assert(!ggi_claim(&state,GGI_HOST_QMP,&tokens[1]));
    puts("IPCC ownership: host coexistence, exclusive GPU, exact tokens, reliable rollback and 36000 racing claims passed");
}
