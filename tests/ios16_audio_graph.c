#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../ios16/audio/giulia_audio_graph.h"
#include "../ios16/common/giulia_sha256.h"
struct mock {
    uint8_t packet[4096];size_t bytes;
    uint64_t now;unsigned sends,auth_calls,real_auth,fail_auth;
    int send_result;
};
static uint64_t now_us(void *p) { return ((struct mock *)p)->now; }
static int send_packet(void *p,const uint8_t *data,size_t n)
{
    struct mock *m=p;assert(n<=sizeof(m->packet));m->sends++;
    memcpy(m->packet,data,n);m->bytes=n;return m->send_result;
}
static int verify(void *p,const uint8_t *data,size_t n,const char *expected)
{
    struct mock *m=p;m->auth_calls++;
    assert(strlen(expected)==64);
    if(m->fail_auth)return -1;
    if(m->real_auth) {
        uint8_t actual[32];char encoded[65];gwh_sha256(data,n,actual);
        for(unsigned i=0;i<32;i++)(void)snprintf(encoded+2*i,3,"%02x",actual[i]);
        return strcmp(encoded,expected);
    }
    return 0; /* Explicit synthetic fixture auth; never linked into a backend. */
}
static void response(struct gag_graph *g,uint8_t p[32],uint32_t status)
{
    memset(p,0,32);gag_put(p,(32u<<8)|(6u<<4));p[4]=3;p[5]=2;
    gag_put(p+8,1);gag_put(p+12,g->src_port);gag_put(p+16,g->pending_token);
    gag_put(p+20,GAG_BASIC_RSP);gag_put(p+24,g->pending_opcode);gag_put(p+28,status);
}
static void ack(struct gag_graph *g)
{ uint8_t p[32];response(g,p,0);assert(gag_receive(g,p,32)==GAG_OK); }
static void init(struct gag_graph *g,struct mock *m,const struct gag_bundle *b)
{
    memset(m,0,sizeof(*m));m->send_result=GAG_OK;
    struct gag_ops ops={m,send_packet,now_us,verify};
    assert(gag_init(g,b,&ops,0x2010,0x41000001)==GAG_OK);
    assert(m->auth_calls==3&&!m->sends);
}
static void drive(struct gag_graph *g,struct mock *m)
{
    assert(gag_control(g,GAG_START,GAG_REQUIRED)==GAG_ESTATE);
    assert(gag_open(g)==GAG_OK&&g->pending_token&&g->state==GAG_CLOSED);
    assert(m->bytes==2952&&gag_u32(m->packet+20)==GAG_OPEN);
    assert(gag_u32(m->packet+24)==0&&gag_u32(m->packet+28)==0&&gag_u32(m->packet+32)==0);
    assert(gag_u32(m->packet+36)==2912);
    assert(gag_open(g)==GAG_ESTATE);
    uint8_t rsp[32];response(g,rsp,0);
    for(unsigned i=0;i<32;i++) {
        uint8_t bad[32];memcpy(bad,rsp,32);bad[i]^=1;
        /* Status bytes are tested as explicit errors below. */
        if(i>=28)continue;
        assert(gag_receive(g,bad,32)==GAG_WAIT&&g->state==GAG_CLOSED);
    }
    assert(gag_receive(g,rsp,31)==GAG_WAIT);
    ack(g);assert(g->state==GAG_OPENED);
    size_t done=0;unsigned chunks=0;
    while(g->state!=GAG_CONFIGURED) {
        assert(gag_config_next(g)==GAG_OK);
        assert(g->cal_offset==done&&g->state!=GAG_CONFIGURED);
        size_t bytes=gag_u32(m->packet+36);
        assert(gag_parameters(m->packet+40,bytes,GAG_MAX_PAYLOAD)==GAG_OK);
        assert(memcmp(m->packet+40,g->bundle.baseline_cal.data+done,bytes)==0);
        done+=bytes;chunks++;ack(g);
    }
    assert(done==6400&&chunks>=2);
    for(unsigned flags=0;flags<GAG_REQUIRED;flags++)
        assert(gag_control(g,GAG_PREPARE,flags)==GAG_ENOTREADY);
    assert(gag_control(g,GAG_PREPARE,GAG_REQUIRED)==GAG_OK&&g->state==GAG_CONFIGURED);
    assert(gag_u32(m->packet+44)==0x08001005&&gag_u32(m->packet+56)==4);
    assert(gag_u32(m->packet+60)==0xb0000002&&gag_u32(m->packet+72)==0xb0000023);
    ack(g);assert(g->state==GAG_PREPARED);
    assert(gag_control(g,GAG_START,0)==GAG_ENOTREADY);
    assert(gag_control(g,GAG_START,GAG_REQUIRED)==GAG_OK&&g->state!=GAG_STARTED);
    ack(g);assert(g->state==GAG_STARTED);
    assert(gag_control(g,GAG_STOP,0)==GAG_OK&&g->state==GAG_STARTED);
    ack(g);assert(g->state==GAG_CONFIGURED);
    assert(gag_control(g,GAG_CLOSE,0)==GAG_OK&&g->open_possible);
    ack(g);assert(g->state==GAG_CLOSED&&!g->open_possible);
    assert(gag_receive(g,rsp,32)==GAG_WAIT); /* replay from previous OPEN */
}
static void load(const char *dir,const char *name,uint8_t *out,size_t n)
{
    char path[1024];assert(snprintf(path,sizeof(path),"%s/%s",dir,name)>0);
    FILE *f=fopen(path,"rb");assert(f);assert(fread(out,1,n,f)==n);assert(fgetc(f)==EOF);fclose(f);
}
int main(int argc,char **argv)
{
    uint8_t open[2792]={0},connections[120]={0},cal[6400]={0},rsp[32];
    struct gag_bundle b={{open,sizeof(open)},{connections,sizeof(connections)},{cal,sizeof(cal)}};
    struct gag_graph g;struct mock m;
    /* Synthetic records sized to force a complete-record boundary at 3600. */
    gag_put(open,1);gag_put(open+4,0x8001001);gag_put(open+8,sizeof(open)-16);
    gag_put(connections,1);gag_put(connections+4,0x8001004);gag_put(connections+8,sizeof(connections)-16);
    gag_put(cal,0x4761);gag_put(cal+4,0x1800ea04);gag_put(cal+8,3584);
    gag_put(cal+3600,0x4761);gag_put(cal+3604,0x1800ea02);gag_put(cal+3608,2784);
    init(&g,&m,&b);drive(&g,&m);
    init(&g,&m,&b);m.send_result=GAG_WAIT;
    assert(gag_open(&g)==GAG_WAIT&&!g.pending_token&&!g.open_possible);
    m.send_result=-1;assert(gag_open(&g)==GAG_EIO&&g.state==GAG_FAILED);
    init(&g,&m,&b);assert(gag_open(&g)==GAG_OK);response(&g,rsp,3);
    assert(gag_receive(&g,rsp,32)==GAG_EREMOTE&&g.remote_status==3&&g.state==GAG_FAILED);
    assert(gag_control(&g,GAG_CLOSE,0)==GAG_OK);ack(&g);
    init(&g,&m,&b);assert(gag_open(&g)==GAG_OK);response(&g,rsp,0);
    m.now=GAG_TIMEOUT_US;assert(gag_poll(&g)==GAG_ETIMEOUT);
    assert(gag_receive(&g,rsp,32)==GAG_WAIT&&g.state==GAG_FAILED);
    assert(gag_control(&g,GAG_CLOSE,0)==GAG_OK);ack(&g);
    init(&g,&m,&b);assert(gag_open(&g)==GAG_OK);ack(&g);
    while(g.state!=GAG_CONFIGURED){assert(gag_config_next(&g)==GAG_OK);ack(&g);}
    assert(gag_control(&g,GAG_PREPARE,GAG_REQUIRED)==GAG_OK);ack(&g);
    assert(gag_control(&g,GAG_START,GAG_REQUIRED)==GAG_OK);
    m.now=GAG_TIMEOUT_US;assert(gag_poll(&g)==GAG_ETIMEOUT);
    assert(gag_control(&g,GAG_START,GAG_REQUIRED)==GAG_ESTATE);
    assert(gag_control(&g,GAG_CLOSE,0)==GAG_OK);ack(&g);
    init(&g,&m,&b);assert(gag_open(&g)==GAG_OK);ack(&g);gag_invalidate(&g);
    assert(g.state==GAG_FAILED&&gag_config_next(&g)==GAG_ESTATE);
    assert(gag_control(&g,GAG_CLOSE,0)==GAG_OK);ack(&g);
    init(&g,&m,&b);m.fail_auth=1;
    struct gag_ops ops={&m,send_packet,now_us,verify};
    assert(gag_init(&g,&b,&ops,0x2010,1)==GAG_EAUTH&&!g.valid&&!m.sends);
    m.fail_auth=0;assert(gag_init(&g,&b,&ops,3,1)==GAG_EINVAL);
    gag_put(cal+8,0xfffffff8);
    assert(gag_init(&g,&b,&ops,0x2010,1)==GAG_EINVAL&&!m.sends);
    if(argc==2) {
        load(argv[1],"apm-graph-open.bin",open,sizeof(open));
        load(argv[1],"apm-connections.bin",connections,sizeof(connections));
        load(argv[1],"cal-0000-nonpersist.bin",cal,sizeof(cal));
        memset(&m,0,sizeof(m));m.real_auth=1;m.send_result=GAG_OK;
        assert(gag_init(&g,&b,&ops,0x2010,0x41000001)==GAG_OK);drive(&g,&m);
        cal[20]^=1;
        assert(gag_init(&g,&b,&ops,0x2010,1)==GAG_EAUTH&&!g.valid);
    }
    puts("APM graph transaction checks passed (mock transport, no DSP execution)");
    return 0;
}
