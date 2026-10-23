/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ios16/giulia_audio_boot.h"

enum { PACK_BYTES=5*4096 };
static unsigned char fixture[PACK_BYTES];
static void w16(unsigned char *p,uint16_t v) {p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);}
static void w32(unsigned char *p,uint32_t v)
{unsigned i;for(i=0;i<4;++i)p[i]=(unsigned char)(v>>(8*i));}
static void ph(unsigned char *p,unsigned kind,unsigned pa,unsigned file,unsigned mem,unsigned flags)
{w32(p,kind);w32(p+12,pa);w32(p+16,file);w32(p+20,mem);w32(p+24,flags);}
static void make_fixture(void)
{
    unsigned i,j,at=0;
    memset(fixture,0,sizeof(fixture));memcpy(fixture,"GAFWPK1",7);
    w32(fixture+8,PACK_BYTES);w32(fixture+12,2);w32(fixture+16,3);
    w32(fixture+20,1);w32(fixture+24,64);w32(fixture+28,192);
    for(i=0;i<2;++i) {
        unsigned char *im=fixture+64+i*64,*m=fixture+(1+i*2)*4096;
        unsigned base=i?(unsigned)GAB_ADSP_PHYSICAL:(unsigned)GAB_DTB_PHYSICAL;
        unsigned count=i?4:3,meta=52+count*32+16;
        w32(im,i?1:36);w32(im+4,base);w32(im+8,i?GAB_ADSP_BYTES:GAB_DTB_BYTES);
        w32(im+12,(1+i*2)*4096);w32(im+16,meta);w32(im+20,at);
        w32(im+24,i?2:1);w32(im+28,i?8192:4096);w32(im+32,base);w32(im+36,1);
        memcpy(m,"\177ELF\1\1\1",7);w16(m+16,2);w16(m+18,(uint16_t)(i?164:1));
        w32(m+20,1);w32(m+24,base);w32(m+28,52);w16(m+40,52);w16(m+42,32);w16(m+44,(uint16_t)count);
        ph(m+52,0,0,52+count*32,0,0x7000000);
        ph(m+84,1,base,7+i,16,0x8000005);
        if(i)ph(m+116,1,base+4096,0,16,0x8000006);
        ph(m+52+(count-1)*32,0,0,16,16,0x2000000);
        for(j=0;j<(i?2u:1u);++j) {
            unsigned char *sr=fixture+192+at*32;
            w32(sr,i);w32(sr+4,1+j);w32(sr+8,j*4096);w32(sr+12,16);
            w32(sr+16,j?0:(2+i*2)*4096);w32(sr+20,j?0:7+i);w32(sr+24,j?0x8000006:0x8000005);
            ++at;
        }
        memset(fixture+(2+i*2)*4096,0x31+i,7+i);
    }
}
struct fake {
    unsigned effects,fail_at,allocations,releases,prepared,unprepared,checks;
    unsigned pin,prereq,lose_prereq,metadata_bad,pas_count,ready_count;
    unsigned pas_command[16],pas_id[16];
    unsigned char memory[2][8192];
};
static int effect(struct fake *f) {return ++f->effects==f->fail_at?-99:0;}
static int verify(void *v,const void *p,size_t n)
{struct fake *f=v;(void)p;(void)n;return (int)f->pin;}
static int prerequisites(void *v)
{struct fake *f=v;++f->checks;return f->lose_prereq && f->checks==2?0:(int)f->prereq;}
static int alloc_meta(void *v,unsigned i,const void *p,size_t n,uint64_t *pa)
{
    struct fake *f=v;int r=effect(f);assert(p && n>52);if(r)return r;
    ++f->allocations;*pa=UINT64_C(0x88000000)+i*0x10000;
    if(f->metadata_bad==1)*pa+=1;
    if(f->metadata_bad==2)*pa=GAB_DTB_PHYSICAL;
    if(f->metadata_bad==3)*pa=UINT64_C(0x88000000);
    if(f->metadata_bad==4)*pa=UINT64_C(0xfffffffffffff000);
    return 0;
}
static void free_meta(void *v,unsigned i) {struct fake *f=v;(void)i;++f->releases;}
static int prepare(void *v) {struct fake *f=v;++f->prepared;return effect(f);}
static int write_memory(void *v,uint64_t pa,const void *p,size_t n)
{
    struct fake *f=v;unsigned i=pa>=GAB_ADSP_PHYSICAL;size_t off=(size_t)(pa-(i?GAB_ADSP_PHYSICAL:GAB_DTB_PHYSICAL));
    int r=effect(f);assert(off+n<=8192);if(r)return r;
    if(p)memcpy(f->memory[i]+off,p,n);else memset(f->memory[i]+off,0,n);return 0;
}
static int sync_memory(void *v,uint64_t pa,size_t n)
{struct fake *f=v;assert(pa>=GAB_DTB_PHYSICAL && n);return effect(f);}
static int pas(void *v,unsigned cmd,unsigned id,uint64_t a,uint64_t b)
{
    struct fake *f=v;assert(f->prepared);
    assert(cmd==1 || cmd==2 || cmd==5 || cmd==6);assert(id==1 || id==36);
    if(cmd==1)assert(a>=0x88000000 && a<0x88100000 && !b);
    if(cmd==2)assert(a==(id==1?GAB_ADSP_PHYSICAL:GAB_DTB_PHYSICAL) && b==(id==1?8192:4096));
    if(cmd==5 || cmd==6)assert(!a && !b);
    f->pas_command[f->pas_count]=cmd;f->pas_id[f->pas_count++]=id;
    return effect(f);
}
static int ready(void *v,unsigned timeout)
{struct fake *f=v;assert(timeout==5000);++f->ready_count;return effect(f);}
static void unprepare(void *v) {struct fake *f=v;++f->unprepared;}
static struct gab_ops operations(struct fake *f)
{
    struct gab_ops o={f,verify,prerequisites,alloc_meta,free_meta,prepare,write_memory,sync_memory,pas,ready,unprepare};
    return o;
}
static void reset(struct fake *f,struct gab_state *s)
{memset(f,0,sizeof(*f));memset(s,0,sizeof(*s));f->pin=f->prereq=1;memset(f->memory,0xcc,sizeof(f->memory));make_fixture();}
static void rejected_before_effects(void)
{
    static const unsigned offsets[]={0,8,12,16,20,24,28,32,64,68,72,76,80,84,88,92,96,100,104,
        128,132,136,140,144,148,152,156,160,164,168,192,196,200,204,208,212,216,220,
        224,228,232,236,240,244,248,252,256,260,264,268,272,276,280,284,4096+28,4096+44,4096+68,
        4096+84+12,4096+84+16,4096+84+20,4096+116+16,4096+200};
    struct fake f;struct gab_state s;unsigned i;
    for(i=0;i<sizeof(offsets)/sizeof(offsets[0]);++i) {
        struct gab_ops o=operations(&f);reset(&f,&s);fixture[offsets[i]]^=0x80;
        if(gab_start(&o,&s,fixture,sizeof(fixture))!=GAB_PACK) {
            fprintf(stderr,"unexpected accepted mutation at %u\n",offsets[i]);abort();
        }
        assert(!f.effects);
    }
    reset(&f,&s);{struct gab_ops o=operations(&f);fixture[4096+116+27]^=1;
        assert(gab_start(&o,&s,fixture,sizeof(fixture))==GAB_PACK);assert(!f.effects);}
    reset(&f,&s);{struct gab_ops o=operations(&f);assert(gab_start(&o,&s,NULL,0)==GAB_PACK);assert(!f.effects);}
}
static void lifecycle(void)
{
    struct fake f;struct gab_state s;struct gab_ops o=operations(&f);unsigned i,total;
    reset(&f,&s);assert(gab_start(&o,&s,fixture,sizeof(fixture))==GAB_OK);
    assert(s.running && !s.quarantined && s.metadata_mask==3 && s.authenticated_mask==3);
    assert(s.copied_bytes==15 && s.zeroed_bytes==33);assert(!f.releases && !f.unprepared);
    assert(!memcmp(f.memory[0],"1111111",7) && !memcmp(f.memory[1],"22222222",8));
    for(i=7;i<16;++i)assert(!f.memory[0][i]);
    for(i=8;i<16;++i)assert(!f.memory[1][i]);
    for(i=4096;i<4112;++i)assert(!f.memory[1][i]);
    assert(f.memory[1][16]==0xcc); /* Gaps are not spuriously zeroed. */
    assert(f.pas_count==6);
    for(i=0;i<6;++i) {assert(f.pas_command[i]==(i%3==0?1u:i%3==1?2u:5u));assert(f.pas_id[i]==(i<3?36u:1u));}
    total=f.effects;assert(gab_start(&o,&s,fixture,sizeof(fixture))==GAB_ALREADY_RUNNING);assert(f.effects==total);
    assert(gab_stop(&o,&s)==GAB_OK);assert(!s.running && !s.metadata_mask && !s.prepared);
    assert(f.pas_command[6]==6 && f.pas_id[6]==1 && f.pas_id[7]==36);
    assert(f.releases==2 && f.unprepared==1);assert(gab_stop(&o,&s)==GAB_NOT_RUNNING);
    /* Fail at each stage, including final ready wait. Once prepare may have
     * changed hardware, all resource references remain pinned. */
    for(i=1;i<=total;++i) {
        reset(&f,&s);f.fail_at=i;
        assert(gab_start(&o,&s,fixture,sizeof(fixture))!=GAB_OK);
        if(i<=2) {assert(!s.quarantined && !s.metadata_mask);assert(f.releases==i-1);}
        else {
            assert(s.quarantined && s.metadata_mask==3 && !f.releases && !f.unprepared);
            assert(gab_stop(&o,&s)==GAB_QUARANTINED);assert(gab_start(&o,&s,fixture,sizeof(fixture))==GAB_QUARANTINED);
            assert(f.effects==i);
        }
    }
    for(i=1;i<=2;++i) {
        reset(&f,&s);assert(gab_start(&o,&s,fixture,sizeof(fixture))==GAB_OK);f.fail_at=f.effects+i;
        assert(gab_stop(&o,&s)==GAB_PAS);assert(s.quarantined && s.metadata_mask==3 && !f.releases && !f.unprepared);
    }
}
static void prerequisites_and_metadata(void)
{
    struct fake f;struct gab_state s;struct gab_ops o=operations(&f);unsigned i;
    reset(&f,&s);f.pin=0;assert(gab_start(&o,&s,fixture,sizeof(fixture))==GAB_PIN);assert(!f.effects);
    reset(&f,&s);f.prereq=0;assert(gab_start(&o,&s,fixture,sizeof(fixture))==GAB_PREREQUISITE);assert(!f.effects);
    reset(&f,&s);f.lose_prereq=1;assert(gab_start(&o,&s,fixture,sizeof(fixture))==GAB_PREREQUISITE);
    assert(f.releases==2 && !f.prepared && !s.quarantined);
    for(i=1;i<=4;++i) {
        reset(&f,&s);f.metadata_bad=i;
        assert(gab_start(&o,&s,fixture,sizeof(fixture))==GAB_METADATA);
        assert(f.allocations==f.releases && !f.prepared && !s.metadata_mask);
    }
    reset(&f,&s);o.wait_ready=NULL;assert(gab_start(&o,&s,fixture,sizeof(fixture))==GAB_ARGUMENT);assert(!f.effects);
}
int main(int argc,char **argv)
{
    if(argc==2) {
        struct gab_plan plan;FILE *file=fopen(argv[1],"rb");long length;void *data;int result;
        assert(file && !fseek(file,0,SEEK_END));length=ftell(file);assert(length>0 && !fseek(file,0,SEEK_SET));
        data=malloc((size_t)length);assert(data && fread(data,1,(size_t)length,file)==(size_t)length);fclose(file);
        result=gab_validate(data,(size_t)length,&plan);assert(result==GAB_OK);
        printf("validated audio pack: %u segments, DTB span %u, ADSP span %u\n",plan.segment_count,plan.images[0].load_bytes,plan.images[1].load_bytes);
        free(data);return 0;
    }
    rejected_before_effects();lifecycle();prerequisites_and_metadata();puts("ADSP boot lifecycle tests passed");return 0;
}
