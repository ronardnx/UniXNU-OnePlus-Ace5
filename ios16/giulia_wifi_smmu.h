/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * SM8650 stage-1 DMA isolation for PCIe0 SID 0x1401. The only mappings are
 * allocator-owned 2 MiB blocks in one arena. Page tables are separately owned
 * and never DMA-mapped. No global SMMU configuration or other stream changes.
 * Register/PTE definitions follow Linux v6.12 arm-smmu.h/io-pgtable-arm.c.
 */
#ifndef GIULIA_WIFI_SMMU_H
#define GIULIA_WIFI_SMMU_H
#include <stdint.h>
#include <stddef.h>
enum gws_error {GWS_OK,GWS_ARGUMENT,GWS_NOT_QUIESCENT,GWS_GEOMETRY,GWS_SHARED_STREAM,
    GWS_NO_CONTEXT,GWS_NO_STREAM,GWS_BUSY,GWS_WRITE_FAILED,GWS_TLB_TIMEOUT,GWS_RESTORE_FAILED};
struct gws_io {
    void *context;
    uint32_t (*read)(void *,unsigned);
    void (*write)(void *,unsigned,uint32_t);
    void (*delay)(void *,unsigned);
    int (*quiescent)(void *);
    int (*owns)(void *,uint64_t,size_t);
    void (*clean)(void *,void *,size_t);
};
struct gws_state {
    unsigned cb,slot,changed,active,quarantined,error;
    uint32_t old_smr,old_s2cr,old_cbar,old_cba2r,old_registers[10],asid;
    uint64_t table_pa,arena_pa,iova,bytes;
};
static const unsigned gws_offsets[10]={0,0x10,0x20,0x24,0x28,0x2c,0x30,0x34,0x38,0x3c};
static inline uint32_t gws_r(const struct gws_io *b,unsigned o){return b->read(b->context,o);}
static inline void gws_w(const struct gws_io *b,unsigned o,uint32_t v){b->write(b->context,o,v);}
static inline unsigned gws_cb(unsigned cb){return 0x80000+cb*0x1000;}
static inline int gws_overlap(uint64_t a,uint64_t n,uint64_t b,uint64_t m)
{return a<=b ? b-a<n : a-b<m;}
static inline int gws_matches(uint32_t smr,unsigned sid)
{return (smr>>31) && !((smr^sid)&~((smr>>16)&0x7fff)&0x7fff);}
static inline int gws_sync(const struct gws_io *b,unsigned cb,unsigned asid)
{
    unsigned base=gws_cb(cb);gws_w(b,base+0x610,asid);gws_w(b,base+0x7f0,0);
    for(unsigned i=0;i<1000;i++) {
        uint32_t v=gws_r(b,base+0x7f4);
        if(v==0xffffffffu)return 0;
        if(!(v&1))return 1;
        b->delay(b->context,10);
    }
    return 0;
}
static inline int gws_restore(const struct gws_io *b,struct gws_state *s)
{
    if(!s->changed)return GWS_OK;
    if(!b->quiescent(b->context)) {s->quarantined=1;return GWS_NOT_QUIESCENT;}
    uint32_t current_m=gws_r(b,0x800+s->slot*4),current_r=gws_r(b,0xc00+s->slot*4);
    if((current_m!=s->old_smr && current_m!=0x80001401u) ||
       (current_r!=s->old_s2cr && current_r!=(0x02000000u|s->cb) && current_r!=0x20000u)) {
        s->quarantined=1;return GWS_BUSY;
    }
    /* Fault the owned stream before disabling its new translation context. */
    gws_w(b,0xc00+s->slot*4,0x20000);
    if(gws_r(b,0xc00+s->slot*4)!=0x20000) {s->quarantined=1;return GWS_RESTORE_FAILED;}
    gws_w(b,gws_cb(s->cb),0);
    if(!gws_sync(b,s->cb,s->asid)) {s->quarantined=1;return GWS_TLB_TIMEOUT;}
    for(unsigned i=1;i<10;i++) gws_w(b,gws_cb(s->cb)+gws_offsets[i],s->old_registers[i]);
    gws_w(b,0x1000+s->cb*4,s->old_cbar);gws_w(b,0x1800+s->cb*4,s->old_cba2r);
    gws_w(b,gws_cb(s->cb),s->old_registers[0]);
    /* An originally unused SMR must become invalid before restoring S2CR. */
    gws_w(b,0x800+s->slot*4,s->old_smr);
    gws_w(b,0xc00+s->slot*4,s->old_s2cr);
    if(gws_r(b,0x800+s->slot*4)!=s->old_smr || gws_r(b,0xc00+s->slot*4)!=s->old_s2cr ||
       gws_r(b,0x1000+s->cb*4)!=s->old_cbar || gws_r(b,0x1800+s->cb*4)!=s->old_cba2r) {
        s->quarantined=1;return GWS_RESTORE_FAILED;
    }
    for(unsigned i=0;i<10;i++) if(gws_r(b,gws_cb(s->cb)+gws_offsets[i])!=s->old_registers[i]) {
        s->quarantined=1;return GWS_RESTORE_FAILED;
    }
    s->active=0;s->changed=0;s->quarantined=0;return GWS_OK;
}
static inline int gws_live(const struct gws_io *b,const struct gws_state *s)
{
    if(!s->active || s->quarantined)return 0;
    unsigned base=gws_cb(s->cb);
    uint64_t ttbr=(uint64_t)gws_r(b,base+0x20)|((uint64_t)gws_r(b,base+0x24)<<32);
    uint32_t smr=gws_r(b,0x800+s->slot*4);
    return smr==((s->old_smr>>31)?s->old_smr:0x80001401u) &&
      gws_r(b,0xc00+s->slot*4)==(0x02000000u|s->cb) &&
      gws_r(b,0x1000+s->cb*4)==0x1f300 && gws_r(b,0x1800+s->cb*4)==1 &&
      gws_r(b,base)==0x1127 && gws_r(b,base+0x10)==0x38011 &&
      gws_r(b,base+0x30)==0x803520 && gws_r(b,base+0x38)==0xff &&
      gws_r(b,base+0x28)==0 && gws_r(b,base+0x2c)==(s->asid<<16) && gws_r(b,base+0x3c)==0 &&
      ttbr==(s->table_pa|((uint64_t)s->asid<<48));
}
static inline int gws_take(const struct gws_io *b,struct gws_state *s,uint64_t *tables,
                           uint64_t table_pa,uint64_t arena_pa,uint64_t iova,size_t bytes)
{
    unsigned slot=107,free_slot=107,cb=71,matches=0,used[71]={0};
    if(!b || !s || !tables || !b->read || !b->write || !b->delay || !b->quiescent || !b->owns || !b->clean ||
       s->changed || s->quarantined || (table_pa&0x3fff) || ((uintptr_t)tables&7) ||
       (arena_pa&0x1fffff) || (iova&0x1fffff) || !bytes || bytes>0x4000000 || (bytes&0x1fffff) ||
       table_pa>(1ull<<36)-0x4000 || arena_pa>(1ull<<36)-bytes || iova>0x100000000ull-bytes ||
       (iova>>30)!=((iova+bytes-1)>>30) || gws_overlap(table_pa,0x4000,arena_pa,bytes) ||
       !b->owns(b->context,table_pa,0x4000) || !b->owns(b->context,arena_pa,bytes)) return GWS_ARGUMENT;
    if(!b->quiescent(b->context))return GWS_NOT_QUIESCENT;
    if(gws_r(b,0x20)!=0x4c017e6b || gws_r(b,0x24)!=0x60000047 || gws_r(b,0x28)!=0x5111)
        return GWS_GEOMETRY;
    for(unsigned i=0;i<107;i++) {
        uint32_t m=gws_r(b,0x800+i*4),route=gws_r(b,0xc00+i*4);
        if(m==0xffffffffu || route==0xffffffffu)return GWS_GEOMETRY;
        if(!(m>>31)) {if(free_slot==107)free_slot=i;continue;}
        if(((route>>16)&3)==0 && (route&255)<71)used[route&255]=1;
        if(gws_matches(m,0x1401)) {
            unsigned mask=(m>>16)&0x7fff;
            if(mask>1 || (mask==1 && (m&0x7ffe)!=0x1400))return GWS_SHARED_STREAM;
            slot=i;matches++;
        }
    }
    if(matches>1)return GWS_SHARED_STREAM;
    if(slot==107)slot=free_slot;
    if(slot==107)return GWS_NO_STREAM;
    /* CB2 is the retained UFS firmware context; CB70 is the XNU UFS context.
       Avoid firmware's low-numbered banks even when currently unreferenced. */
    for(unsigned i=70;i>4;i--) {
        unsigned candidate=i-1;uint32_t ctl=gws_r(b,gws_cb(candidate));
        if(!used[candidate] && ctl!=0xffffffffu && !(ctl&1)) {cb=candidate;break;}
    }
    if(cb==71)return GWS_NO_CONTEXT;
    unsigned asid=0x4700+cb;
    for(unsigned i=0;i<71;i++) if(gws_r(b,gws_cb(i))&1) {
        if((gws_r(b,gws_cb(i)+0x24)>>16)==asid || (gws_r(b,gws_cb(i)+0x2c)>>16)==asid)
            return GWS_BUSY;
    }
    {unsigned char *p=(unsigned char *)s;for(unsigned i=0;i<sizeof(*s);i++)p[i]=0;}
    s->cb=cb;s->slot=slot;s->asid=asid;s->table_pa=table_pa;s->arena_pa=arena_pa;s->iova=iova;s->bytes=bytes;
    s->old_smr=gws_r(b,0x800+slot*4);s->old_s2cr=gws_r(b,0xc00+slot*4);
    s->old_cbar=gws_r(b,0x1000+cb*4);s->old_cba2r=gws_r(b,0x1800+cb*4);
    if(s->old_cbar==0xffffffffu || s->old_cba2r==0xffffffffu)return GWS_GEOMETRY;
    for(unsigned i=0;i<10;i++) {
        s->old_registers[i]=gws_r(b,gws_cb(cb)+gws_offsets[i]);
        if(s->old_registers[i]==0xffffffffu)return GWS_GEOMETRY;
    }
    for(unsigned i=0;i<0x4000/8;i++)tables[i]=0;
    tables[iova>>30]=(table_pa+0x1000)|3|(1ull<<63);
    for(unsigned i=0;i<bytes/0x200000;i++)
        tables[512+((iova>>21)&511)+i]=(arena_pa+(uint64_t)i*0x200000)|(3ull<<53)|0xf61;
    b->clean(b->context,tables,0x4000);
    if(!b->quiescent(b->context) || gws_r(b,0x800+slot*4)!=s->old_smr ||
       gws_r(b,0xc00+slot*4)!=s->old_s2cr || gws_r(b,gws_cb(cb))!=s->old_registers[0])return GWS_BUSY;
    s->changed=1;
    unsigned base=gws_cb(cb);
    gws_w(b,base,0);gws_w(b,0x1800+cb*4,1);gws_w(b,0x1000+cb*4,0x1f300);
    gws_w(b,base+0x10,0x38011);gws_w(b,base+0x30,0x803520);
    uint64_t ttbr=table_pa|((uint64_t)asid<<48);
    gws_w(b,base+0x24,(uint32_t)(ttbr>>32));gws_w(b,base+0x20,(uint32_t)ttbr);
    gws_w(b,base+0x2c,asid<<16);gws_w(b,base+0x28,0);
    gws_w(b,base+0x38,0xff);gws_w(b,base+0x3c,0);
    if(!gws_sync(b,cb,asid)) {s->error=GWS_TLB_TIMEOUT;gws_restore(b,s);return GWS_TLB_TIMEOUT;}
    gws_w(b,base,0x1127);
    /* Publish S2CR first for a new SMR, so it can never briefly bypass. */
    gws_w(b,0xc00+slot*4,0x02000000u|cb);
    if(!(s->old_smr>>31))gws_w(b,0x800+slot*4,0x80001401u);
    s->active=1;
    if(!gws_live(b,s)) {s->error=GWS_WRITE_FAILED;gws_restore(b,s);return GWS_WRITE_FAILED;}
    return GWS_OK;
}
#endif
