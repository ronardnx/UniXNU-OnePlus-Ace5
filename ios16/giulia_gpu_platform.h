/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_GPU_PLATFORM_H
#define GIULIA_GPU_PLATFORM_H
#include <stdint.h>
#include <stddef.h>
#include "giulia_gpu_scm.h"

enum { GGS_SEC_CHUNKS=8, GGS_SEC_CHUNK=0x400000, GGS_SEC_BASE=0xf0000000u,
       GGS_SEC_TABLE_BYTES=0x20000, GGS_CONTROL_BYTES=0x4000,
       GGS_CP_PIXEL=10, GGS_HLOS=3 };
struct ggs_mem_region { uint64_t address,bytes; };
struct ggs_vmperm { uint32_t vmid,perm; uint64_t context; uint32_t context_bytes,unused; };
struct ggs_secure_slot { uint32_t bytes,flags,kind,used; };

/* Fixed internal descriptor, never accepted from the terminal. Shared control
 * page is Normal NC. x5 points at args[3..9] when more than four arguments. */
static inline int ggs_build_frame(uint32_t function,uint32_t info,const uint64_t args[10],
    uint64_t control_pa,uint64_t *extended,struct ggs_regs *out)
{
    unsigned n=info&15u;
    if(!args || !extended || !out || !n || n>10 || !control_pa || (control_pa&16383) || control_pa>=(UINT64_C(1)<<36))return 0;
    for(unsigned i=0;i<8;i++)out->x[i]=0;
    for(unsigned i=0;i<7;i++)extended[i]=args[i+3];
    out->x[0]=function;out->x[1]=info;out->x[2]=args[0];out->x[3]=args[1];out->x[4]=args[2];
    out->x[5]=n>4?control_pa:args[3];return 1;
}

static inline int ggs_assign_descriptor(void *control,uint64_t control_pa,
    uint64_t physical,uint64_t bytes,int table,uint64_t args[10])
{
    if(!control || !args || !bytes || ((physical|bytes)&16383) ||
       physical>=(UINT64_C(1)<<36) || bytes>(UINT64_C(1)<<36)-physical ||
       !control_pa || (control_pa&16383))return 0;
    uint8_t *p=(uint8_t *)control;
    for(unsigned i=128;i<320;i++)p[i]=0;
    struct ggs_mem_region *region=(struct ggs_mem_region *)(p+128);
    uint32_t *source=(uint32_t *)(p+192);
    struct ggs_vmperm *dest=(struct ggs_vmperm *)(p+256);
    region->address=physical;region->bytes=bytes;*source=GGS_HLOS;
    dest[0].vmid=table?GGS_HLOS:GGS_CP_PIXEL;dest[0].perm=6;
    if(table){dest[1].vmid=GGS_CP_PIXEL;dest[1].perm=4;}
    for(unsigned i=0;i<10;i++)args[i]=0;
    args[0]=control_pa+128;args[1]=sizeof(*region);args[2]=control_pa+192;args[3]=4;
    args[4]=control_pa+256;args[5]=(table?2u:1u)*sizeof(*dest);return 1;
}

static inline int ggs_secure_request(uint32_t bytes,uint32_t alignment,uint32_t flags,uint32_t kind)
{
    return bytes && bytes<=GGS_SEC_CHUNK && alignment<=16384 &&
        (!alignment || !(alignment&(alignment-1))) && flags==0x1007 && kind==5;
}

/* Validate the signed, embedded ELF32 ZAP before any PAS call or carveout write. */
struct ggs_zap_plan { uint32_t metadata_header,hash_offset,hash_bytes,load_offset,load_bytes,memory_bytes; };
static inline uint32_t ggs_u32(const uint8_t *p)
{return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static inline int ggs_zap_plan(const void *data,size_t bytes,struct ggs_zap_plan *out)
{
    const uint8_t *p=(const uint8_t *)data;
    if(!p || !out || bytes<148 || ggs_u32(p)!=0x464c457f || p[4]!=1 || p[5]!=1 ||
       ggs_u32(p+28)!=52 || p[40]!=52 || p[42]!=32 || p[44]!=3 || p[45])return 0;
    const uint8_t *head=p+52,*load=head+32,*hash=load+32;
    if(ggs_u32(head)!=0 || ggs_u32(head+16)!=148 || ggs_u32(load)!=1 ||
       !(ggs_u32(load+24)&(1u<<27)) || ggs_u32(hash)!=0 ||
       (ggs_u32(hash+24)&0x7000000)!=0x2000000)return 0;
    out->metadata_header=148;out->hash_offset=ggs_u32(hash+4);out->hash_bytes=ggs_u32(hash+16);
    out->load_offset=ggs_u32(load+4);out->load_bytes=ggs_u32(load+16);out->memory_bytes=ggs_u32(load+20);
    return out->hash_offset<=bytes && out->hash_bytes<=bytes-out->hash_offset && out->hash_bytes<=16384-148 &&
        out->load_offset<=bytes && out->load_bytes<=bytes-out->load_offset && out->load_bytes<=out->memory_bytes &&
        out->memory_bytes && out->memory_bytes<=8192;
}
#endif
