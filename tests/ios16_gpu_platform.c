/* Host-only ABI/ownership descriptors and signed-ZAP bounds. No SMC or device. */
#define GG_HOST_TEST 1
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ios16/giulia_gpu_platform.h"
int main(int argc,char **argv)
{
    _Alignas(16384) unsigned char control[16384];
    uint64_t args[10];struct ggs_regs frame;
    memset(control,0xee,sizeof(control));
    assert(sizeof(struct ggs_vmperm)==24 && sizeof(struct ggs_mem_region)==16);
    assert(ggs_assign_descriptor(control,0x8a0000000ull,0x8b0000000ull,0x200000,0,args));
    assert(((struct ggs_mem_region *)(control+128))->address==0x8b0000000ull);
    assert(*(uint32_t *)(control+192)==3);
    struct ggs_vmperm *v=(struct ggs_vmperm *)(control+256);
    assert(v[0].vmid==10 && v[0].perm==6 && !v[0].context && args[5]==24);
    assert(ggs_build_frame(0x42000c16,0x1117,args,0x8a0000000ull,(uint64_t *)control,&frame));
    assert(frame.x[2]==0x8a0000080ull && frame.x[3]==16 && frame.x[4]==0x8a00000c0ull && frame.x[5]==0x8a0000000ull);
    assert(((uint64_t *)control)[0]==4 && ((uint64_t *)control)[1]==0x8a0000100ull && ((uint64_t *)control)[2]==24);
    assert(ggs_assign_descriptor(control,0x8a0000000ull,0x8b0000000ull,0x20000,1,args));
    assert(v[0].vmid==3 && v[0].perm==6 && v[1].vmid==10 && v[1].perm==4 && args[5]==48);
    assert(!ggs_assign_descriptor(control,0x8a0000000ull,0x8b0000001ull,0x20000,1,args));
    assert(!ggs_assign_descriptor(control,0x8a0000000ull,0xfffffc000ull,0x20000,1,args));
    assert(!ggs_build_frame(1,11,args,0x8a0000000ull,(uint64_t *)control,&frame));
    args[0]=0xffff0000;args[1]=args[2]=args[3]=0xffffffff;
    assert(ggs_build_frame(0x42000c1b,4,args,0x8a0000000ull,(uint64_t *)control,&frame) && frame.x[5]==0xffffffff);
    assert(ggs_secure_request(0x37d000,4096,0x1007,5));
    assert(!ggs_secure_request(0x37d000,4096,0x1107,5)); /* host access prohibited */
    assert(!ggs_secure_request(0x37d000,4096,0x100f,5)); /* GMU alias prohibited */
    assert(!ggs_secure_request(0x400001,4096,0x1007,5));
    assert(!ggs_secure_request(4096,3,0x1007,5));
    if(argc==2) {
        FILE *file=fopen(argv[1],"rb");assert(file);assert(!fseek(file,0,SEEK_END));long n=ftell(file);assert(n>0);
        rewind(file);unsigned char *bytes=malloc((size_t)n);assert(bytes && fread(bytes,1,(size_t)n,file)==(size_t)n);fclose(file);
        struct ggs_zap_plan plan;assert(ggs_zap_plan(bytes,(size_t)n,&plan));
        assert(plan.metadata_header==148 && plan.hash_bytes==3896 && plan.load_bytes==1072 && plan.memory_bytes==1072);
        assert(!ggs_zap_plan(bytes,147,&plan));
        memset(bytes+52+32+20,0xff,4);assert(!ggs_zap_plan(bytes,(size_t)n,&plan));free(bytes);
    }
    puts("GPU platform descriptors, isolated secure permissions and ZAP bounds passed (host only)");return 0;
}
