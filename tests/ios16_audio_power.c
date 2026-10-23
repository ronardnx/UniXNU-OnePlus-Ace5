/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/giulia_audio_power.h"

struct fake { unsigned calls,fail_at,alias,slave; unsigned char aux[32]; size_t bytes; };
static void level(struct fake *f,unsigned i,unsigned n)
{f->aux[i*2]=(unsigned char)n;f->aux[i*2+1]=(unsigned char)(n>>8);}
static void reset(struct fake *f)
{
    static const unsigned values[]={0,16,64,128,256,384,448,512};unsigned i;
    memset(f,0,sizeof(*f));f->slave=3;f->bytes=32;
    for(i=0;i<sizeof(values)/sizeof(values[0]);++i)level(f,i,values[i]);
}
static int lookup(void *v,const char *name,struct gap_resource *r)
{
    struct fake *f=(struct fake *)v;static const char *const names[]={"lcx.lvl","lmx.lvl","xo.lvl"};
    unsigned index=f->calls++;assert(index<3);assert(!strcmp(names[index],name));
    if(f->calls==f->fail_at)return -19;
    r->address=0x30000+(f->alias?0:index*16);r->slave_id=f->slave;
    r->aux=f->aux;r->aux_bytes=f->bytes;return 0;
}
int main(void)
{
    struct fake f;struct gap_ops ops={&f,lookup};struct gap_plan p;
    uint32_t selected=99,voltage=99;unsigned i;
    reset(&f);assert(gap_build_plan(&ops,&p)==GAP_OK && p.valid);
    assert(p.votes[0].selector==5 && p.votes[0].consumer_level==384);
    assert(p.votes[1].selector==5 && p.votes[1].consumer_level==384);
    assert(p.votes[2].selector==3 && !p.votes[2].consumer_level);
    assert(p.icc_paths==2 && p.icc_average==UINT32_MAX && p.icc_peak==UINT32_MAX);
    for(i=0;i<3;++i)assert(p.votes[i].state_mask==7);
    reset(&f);level(&f,5,400);assert(gap_build_plan(&ops,&p)==GAP_OK);
    assert(p.votes[0].selector==5 && p.votes[0].consumer_level==400);
    reset(&f);level(&f,4,384);assert(gap_build_plan(&ops,&p)==GAP_OK);
    assert(p.votes[0].selector==4); /* First adequate native index wins. */
    for(i=1;i<=3;++i) {
        reset(&f);f.fail_at=i;assert(gap_build_plan(&ops,&p)==GAP_RESOURCE);
        assert(!p.valid && p.backend_result==-19 && f.calls==i);
    }
    reset(&f);f.slave=4;assert(gap_build_plan(&ops,&p)==GAP_RESOURCE && !p.valid);
    reset(&f);f.alias=1;assert(gap_build_plan(&ops,&p)==GAP_RESOURCE && !p.valid);
    reset(&f);level(&f,6,128);assert(gap_build_plan(&ops,&p)==GAP_AUX && !p.valid);
    reset(&f);level(&f,9,512);assert(gap_build_plan(&ops,&p)==GAP_AUX && !p.valid);
    reset(&f);f.bytes=10;assert(gap_build_plan(&ops,&p)==GAP_LEVEL && !p.valid);
    reset(&f);f.bytes=31;assert(gap_build_plan(&ops,&p)==GAP_AUX && !p.valid);
    reset(&f);f.bytes=34;assert(gap_build_plan(&ops,&p)==GAP_AUX && !p.valid);
    assert(gap_arc_selector(NULL,2,384,&selected,&voltage)==GAP_AUX);
    assert(selected==99 && voltage==99);
    assert(gap_build_plan(NULL,&p)==GAP_ARGUMENT && !p.valid);
    assert(gap_build_plan(&ops,NULL)==GAP_ARGUMENT);
    puts("ADSP power plan tests passed");return 0;
}
