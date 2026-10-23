/* Host-only ABI/ownership descriptors and signed-ZAP bounds. No SMC or device. */
#define GG_HOST_TEST 1
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_platform.h"
#include "../ios16/gpu/giulia_gpu_secure_mapping.h"

static void secure_mapping(void)
{
    uint64_t *words=calloc(1,GGS_SEC_TABLE_BYTES),*snapshot=malloc(GGS_SEC_TABLE_BYTES);
    assert(words && snapshot);
    struct ggm_tables table={words,GGS_SEC_TABLE_BYTES,UINT64_C(0x8a0000000)};
    uint64_t physical[GGS_SEC_SEGMENTS];
    struct ggs_secure_slot slots[GGS_SEC_CHUNKS]={{0}};
    words[3]=(table.physical+4096)|3;
    for(unsigned i=0;i<16;i++)words[512+384+i]=(table.physical+(uint64_t)(i+2)*4096)|3;
    for(unsigned i=0;i<GGS_SEC_SEGMENTS;i++) {
        physical[i]=UINT64_C(0x8b0000000)+(uint64_t)i*0x20000;
        for(unsigned page=0;page<16;page++)words[1024+i*16+page]=physical[i]+page*4096;
    }
    assert(ggs_mapping_owned(&table,physical) && ggs_mapping_audit(&table,physical,slots));
    // Independent expected leaves: byte sizes round up once, every remaining
    // page in the fixed slot and every other slot remains an invalid PA.
    const uint32_t sizes[]={1,4095,4096,4097,65537,0x37d000,0x3fffff,0x400000};
    for(unsigned slot=0;slot<GGS_SEC_CHUNKS;slot++) {
        assert(ggs_mapping_slot(&table,physical,slot,sizes[slot],GGM_CHECK));
        assert(ggs_mapping_slot(&table,physical,slot,sizes[slot],GGM_APPLY));
        slots[slot]=(struct ggs_secure_slot){sizes[slot],0x1007,5,1};
        assert(ggs_mapping_slot(&table,physical,slot,sizes[slot],GGM_VERIFY));
        assert(ggs_mapping_audit(&table,physical,slots));
        for(unsigned page=0;page<GGS_SEC_CHUNK/4096;page++) {
            unsigned index=slot*(GGS_SEC_CHUNK/4096)+page;
            uint64_t expected=physical[index/16]+(index%16)*4096;
            if(page<(sizes[slot]+4095)/4096)expected|=(UINT64_C(3)<<53)|UINT64_C(0xe03);
            assert(words[1024+index]==expected);
        }
    }
    memcpy(snapshot,words,GGS_SEC_TABLE_BYTES);
    assert(!ggs_mapping_slot(&table,physical,GGS_SEC_CHUNKS,4096,GGM_APPLY));
    assert(!ggs_mapping_slot(&table,physical,0,0,GGM_APPLY));
    assert(!ggs_mapping_slot(&table,physical,0,GGS_SEC_CHUNK+1,GGM_APPLY));
    assert(!ggs_mapping_slot(&table,physical,0,4096,GGM_VERIFY+1));
    assert(!memcmp(snapshot,words,GGS_SEC_TABLE_BYTES));
    // Reject duplicated child tables and out-of-aperture branches even when
    // those branches could otherwise produce valid-looking address walks.
    uint64_t saved=words[512+385];words[512+385]=words[512+384];
    assert(!ggs_mapping_reset(&table,physical));words[512+385]=saved;
    words[512+400]=words[512+384];assert(!ggs_mapping_audit(&table,physical,slots));words[512+400]=0;
    words[511]=3;assert(!ggs_mapping_owned(&table,physical));words[511]=0;
    saved=physical[511];physical[511]=physical[0]+16384;
    assert(!ggs_mapping_owned(&table,physical));physical[511]=saved;
    // A failure at the last page must leave every earlier page untouched.
    words[1024+8191]^=4096;memcpy(snapshot,words,GGS_SEC_TABLE_BYTES);
    assert(!ggs_mapping_reset(&table,physical));
    assert(!memcmp(snapshot,words,GGS_SEC_TABLE_BYTES));words[1024+8191]^=4096;
    assert(ggs_mapping_reset(&table,physical));memset(slots,0,sizeof(slots));
    assert(ggs_mapping_audit(&table,physical,slots));
    // Equal-size requests in distinct slots must have distinct backing/I/O VAs.
    for(unsigned i=0;i<2;i++) {
        slots[i]=(struct ggs_secure_slot){4096,0x1007,5,1};
        assert(ggs_mapping_slot(&table,physical,i,4096,GGM_APPLY));
    }
    assert(words[1024]!=words[1024+1024] && ggs_mapping_audit(&table,physical,slots));
    slots[7].used=2;assert(!ggs_mapping_audit(&table,physical,slots));slots[7].used=0;
    slots[7].bytes=1;assert(!ggs_mapping_audit(&table,physical,slots));slots[7].bytes=0;
    words[1025]|=GGS_SEC_ATTRIBUTES;assert(!ggs_mapping_audit(&table,physical,slots));
    free(snapshot);free(words);
}
int main(int argc,char **argv)
{
    secure_mapping();
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
    uint64_t physical[32];
    for(unsigned i=0;i<32;i++)physical[i]=0x8b0000000ull+(uint64_t)i*0x20000;
    assert(ggs_assign_batch(control,0x8a0000000ull,physical,32,args));
    assert(args[0]==0x8a0000200ull && args[1]==512 && args[5]==24);
    struct ggs_mem_region *regions=(struct ggs_mem_region *)(control+512);
    assert(regions[31].address==physical[31] && regions[31].bytes==65536);
    assert(!ggs_assign_batch(control,0x8a0000000ull,physical,33,args));
    physical[31]++;assert(!ggs_assign_batch(control,0x8a0000000ull,physical,32,args));
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
    puts("GPU platform descriptors, exact secure mapping bounds/topology, isolated permissions and ZAP bounds passed (host only)");return 0;
}
