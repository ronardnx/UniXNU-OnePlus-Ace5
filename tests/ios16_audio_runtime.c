/* Real coordinator with existing independent boot, GLINK and amplifier fakes. */
#define main boot_fixture_main
#include "ios16_audio_boot.c"
#undef main
#define main transport_fixture_main
#include "ios16_audio_transport.c"
#undef main
#define main speaker_fixture_main
#include "ios16_audio_speaker.c"
#undef main
#include "../ios16/audio/giulia_audio_runtime.h"

struct app {
    struct fake boot;
    struct fixture wire;
    struct mock amp;
    struct gar_state state;
    unsigned polls,delays,starts,writes,stops,quiesces,available;
    unsigned fail_open,fail_poll,fail_delay,fail_start,fail_write,fail_stop,fail_quiesce;
    unsigned no_reply,graph_live;
};
static int ipc_open(void *v,struct gat_transport *t)
{
    struct app *a=v;
    assert(a->state.boot.running && !a->amp.reads && !a->amp.writes);
    if(a->fail_open)return GAT_OWNERSHIP;
    memset(&a->wire,0,sizeof(a->wire));
    return gat_init(t,a->wire.desc,32,a->wire.tx,sizeof(a->wire.tx),
        a->wire.rx,sizeof(a->wire.rx),&ops,&a->wire,1);
}
static int ipc_poll(void *v,struct gat_transport *t)
{
    struct app *a=v;unsigned step=a->polls++;
    if(a->fail_poll)return GAT_CLOSED;
    a->wire.t=*t;
    if(!a->no_reply) {
        if(step==0) {peer(&a->wire,0,1,5,0,0);peer(&a->wire,1,1,0,0,0);}
        if(step==1) {peer(&a->wire,2,9,10,(const uint8_t *)"adsp_apps",10);peer(&a->wire,4,1,0,0,0);}
        if(step==2) {uint8_t b[8];gat_put32(b,4096);gat_put32(b+4,7);peer(&a->wire,5,9,1,b,8);}
        if(step>=3 && t->spf_pending) {
            uint8_t b[28];response(b,t->spf_token,1);peer_data(&a->wire,9,b,28,0);
        }
    }
    int e=gat_poll(t,64);a->wire.desc[0]=a->wire.desc[1];return e;
}
static int delay_ms(void *v,unsigned n)
{struct app *a=v;assert(n==1);a->delays++;return a->fail_delay?-4:0;}
static int pcm_available(void *v) {return (int)((struct app *)v)->available;}
static int pcm_start(void *v,struct gat_transport *t)
{
    struct app *a=v;assert(t->spf_ready && !a->amp.reads && !a->amp.writes);
    a->starts++;if(a->fail_start)return -7;a->graph_live=1;return 0;
}
static int pcm_write(void *v,struct gat_transport *t,const void *p,size_t n)
{
    struct app *a=v;assert(t->spf_ready && a->graph_live && p && n==96);
    assert(a->state.speaker.started && a->state.playback && a->state.stage==GAR_SPEAKER);
    assert(!a->amp.reset[0] && !a->amp.reset[1]);a->writes++;return a->fail_write?-8:0;
}
static int pcm_stop(void *v,struct gat_transport *t)
{
    struct app *a=v;(void)t;
    if(a->graph_live)assert(a->amp.reset[0] && a->amp.reset[1] && !a->state.playback);
    a->stops++;if(a->fail_stop)return -9;a->graph_live=0;return 0;
}
static int ipc_quiesce(void *v,struct gat_transport *t)
{
    struct app *a=v;(void)t;assert(!a->graph_live && !a->state.playback);
    a->quiesces++;return a->fail_quiesce?-10:0;
}
static struct gar_ops app_ops(struct app *a)
{
    struct gar_ops o={0};o.boot=operations(&a->boot);
    o.speaker=(struct gsipa_ops){&a->amp,auth,rd,wr,rst,delay};o.cookie=a;
    o.ipc_open=ipc_open;o.ipc_poll=ipc_poll;o.delay_ms=delay_ms;
    o.pcm_available=pcm_available;o.pcm_start=pcm_start;o.pcm_write=pcm_write;
    o.pcm_stop=pcm_stop;o.ipc_quiesce=ipc_quiesce;return o;
}
static void app_reset(struct app *a,const uint8_t *sipa,size_t bytes)
{
    memset(a,0,sizeof(*a));reset(&a->boot,&a->state.boot);a->available=1;
    setup(&a->state.speaker,&a->amp,sipa,bytes);
}
static void app_start(struct app *a)
{
    struct gar_ops o=app_ops(a);
    assert(gar_start(&o,&a->state,fixture,sizeof(fixture))==GAR_OK);
    assert(a->state.stage==GAR_SPF_READY && a->state.boot.running && a->state.transport.spf_ready);
    assert(!a->state.playback && !a->amp.reads && !a->amp.writes && a->polls>=4);
}
int main(int argc,char **argv)
{
    uint8_t sipa[8192];int32_t pcm[24]={0};size_t bytes;FILE *file;unsigned effects;
    struct app a;struct gar_ops o=app_ops(&a);assert(argc==2);
    file=fopen(argv[1],"rb");assert(file);bytes=fread(sipa,1,sizeof(sipa),file);fclose(file);assert(bytes>188);
    app_reset(&a,sipa,bytes);
    assert(gar_start(NULL,&a.state,fixture,sizeof(fixture))==GAR_ARGUMENT && !a.boot.effects);
    a.boot.prereq=0;assert(gar_start(&o,&a.state,fixture,sizeof(fixture))==GAR_BOOT);
    assert(a.state.stage==GAR_OFF && !a.boot.effects && !a.polls && !a.amp.writes);
    app_reset(&a,sipa,bytes);a.boot.pin=0;
    assert(gar_start(&o,&a.state,fixture,sizeof(fixture))==GAR_BOOT && !a.boot.effects);
    app_reset(&a,sipa,bytes);app_start(&a);effects=a.boot.effects;
    assert(gar_start(&o,&a.state,fixture,sizeof(fixture))==GAR_BUSY && a.boot.effects==effects);
    a.available=0;assert(gar_play(&o,&a.state,sipa,bytes,pcm,sizeof(pcm))==GAR_PLATFORM);
    assert(!a.starts && !a.amp.reads && !a.amp.writes);a.available=1;
    assert(gar_play(&o,&a.state,sipa,bytes,pcm,sizeof(pcm))==GAR_OK);
    assert(a.starts==1 && a.writes==1 && a.stops==1 && !a.state.graph_running && !a.state.playback);
    assert(a.state.stage==GAR_SPF_READY && a.amp.reset[0] && a.amp.reset[1]);
    assert(gar_stop(&o,&a.state)==GAR_OK && a.state.stage==GAR_OFF && !a.state.boot.running);
    assert(a.quiesces==1 && a.boot.releases==2 && a.boot.unprepared==1);
    app_reset(&a,sipa,bytes);a.boot.fail_at=3;
    assert(gar_start(&o,&a.state,fixture,sizeof(fixture))==GAR_BOOT && a.state.quarantined);
    effects=a.boot.effects;assert(gar_stop(&o,&a.state)==GAR_UNCERTAIN);
    assert(gar_start(&o,&a.state,fixture,sizeof(fixture))==GAR_UNCERTAIN);
    assert(a.boot.effects==effects && !a.boot.releases && !a.boot.unprepared && !a.polls);
    for(unsigned which=0;which<4;which++) {
        app_reset(&a,sipa,bytes);
        if(which==0)a.fail_open=1;if(which==1)a.fail_poll=1;
        if(which==2)a.fail_delay=1;if(which==3)a.no_reply=1;
        assert(gar_start(&o,&a.state,fixture,sizeof(fixture))==(which==3?GAR_TIMEOUT:GAR_TRANSPORT));
        assert(a.state.quarantined && a.state.boot.running && !a.boot.releases && !a.amp.writes);
        if(which==3)assert(a.delays==5000);
    }
    app_reset(&a,sipa,bytes);app_start(&a);a.fail_start=1;
    assert(gar_play(&o,&a.state,sipa,bytes,pcm,sizeof(pcm))==GAR_GRAPH);
    assert(a.stops==1 && !a.amp.reads && !a.amp.writes && !a.state.quarantined && !a.state.graph_attempted);
    app_reset(&a,sipa,bytes);app_start(&a);a.fail_start=a.fail_stop=1;
    assert(gar_play(&o,&a.state,sipa,bytes,pcm,sizeof(pcm))==GAR_UNCERTAIN);
    assert(a.state.quarantined && a.state.graph_attempted && !a.boot.releases);
    app_reset(&a,sipa,bytes);app_start(&a);a.fail_write=1;
    assert(gar_play(&o,&a.state,sipa,bytes,pcm,sizeof(pcm))==GAR_PCM_WRITE);
    assert(a.writes==1 && a.stops==1 && !a.state.quarantined && !a.state.playback && a.amp.reset[0] && a.amp.reset[1]);
    app_reset(&a,sipa,bytes);app_start(&a);a.amp.regs[1][6]=0xffff;
    assert(gar_play(&o,&a.state,sipa,bytes,pcm,sizeof(pcm))==GAR_AMP);
    assert(!a.writes && a.stops==1 && a.amp.reset[0] && a.amp.reset[1]);
    app_reset(&a,sipa,bytes);app_start(&a);a.amp.fail_reset=1;
    assert(gar_play(&o,&a.state,sipa,bytes,pcm,sizeof(pcm))==GAR_UNCERTAIN);
    assert(a.state.quarantined && a.state.graph_running && !a.stops && !a.boot.releases);
    app_reset(&a,sipa,bytes);app_start(&a);a.fail_stop=1;
    assert(gar_play(&o,&a.state,sipa,bytes,pcm,sizeof(pcm))==GAR_UNCERTAIN);
    assert(a.state.quarantined && a.state.graph_running && a.amp.reset[0] && a.amp.reset[1]);
    app_reset(&a,sipa,bytes);app_start(&a);a.fail_quiesce=1;
    assert(gar_stop(&o,&a.state)==GAR_TRANSPORT && a.state.quarantined && !a.boot.releases);
    app_reset(&a,sipa,bytes);app_start(&a);a.fail_poll=1;
    assert(gar_play(&o,&a.state,sipa,bytes,pcm,sizeof(pcm))==GAR_TRANSPORT);
    assert(a.state.quarantined && !a.state.transport.spf_ready && !a.starts && !a.amp.writes && !a.boot.releases);
    puts("Audio runtime orchestration and quarantine tests passed");return 0;
}
