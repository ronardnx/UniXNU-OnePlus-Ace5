#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/giulia_wifi_wmi.h"
struct fixture {struct gww_state s;struct gww_io io;unsigned stage,sent,done,status,drop,fail;uint64_t time;};
static int rx(struct fixture *f,unsigned ep,const void *p,size_t n){uint8_t b[2048]={0};b[0]=(uint8_t)ep;gwq_w16(b+2,(uint16_t)n);memcpy(b+8,p,n);return gww_receive(&f->s,&f->io,2,b,8+n);}
static uint64_t now(void *v){return ((struct fixture *)v)->time;}
static void done(void *v,unsigned status,uint32_t id){struct fixture *f=v;assert(id==f->s.scan_id);f->done++;f->status=status;}
static int pump(void *v,unsigned us){struct fixture *f=v;f->time+=us/1000;
    if(!f->stage && !f->drop){uint8_t b[8]={1,0,16,0,0,1,8,0};f->stage=1;return rx(f,0,b,8);}return 0;}
static int sendmsg(void *v,unsigned pipe,const void *data,size_t bytes){struct fixture *f=v;const uint8_t *p=data;assert(bytes>=8&&gwq_u16(p+2)==bytes-8);f->sent++;
    if(f->fail)return 1;if(f->drop)return 0;const uint8_t *cmd=p+8;unsigned ep=p[0],msg=gwq_u32(cmd);
    uint8_t reply[256]={0};
    if(!ep){assert(pipe==0);if((msg&0xffff)==2){assert(msg==0x1000002&&gwq_u32(cmd+4)==16*256);gwq_w32(reply,0x1000003);reply[5]=1;gwq_w16(reply+6,2048);return rx(f,0,reply,12);}
        assert(msg==5);gwq_w32(reply,1);gww_tlv(reply+4,32,76);gwq_w32(reply+8+36,2);return rx(f,1,reply,84);}
    assert(ep==1&&pipe==3);
    if(msg==1){assert(bytes==340&&gwq_u16(cmd+6)==74&&gwq_u16(cmd+38)==75);assert(gwq_u32(cmd+36+32)==3);
        gwq_w32(reply,2);gww_tlv(reply+4,35,52);reply[8+24]=2;return rx(f,1,reply,60);}
    if(msg==0x5001){assert(bytes==96&&gwq_u32(cmd+12)==2&&gwq_u32(cmd+32)==1);assert(gwq_u32(cmd+60)==0&&gwq_u32(cmd+76)==1);return 0;}
    assert(msg==0x3001);assert(bytes==276&&gwq_u32(cmd+4+60)==0x10001&&gwq_u32(cmd+4+68)==22);
    assert(gwq_u16(cmd+166)==16&&gwq_u32(cmd+168)==2412&&gwq_u32(cmd+252)==5825);return 0;
}
static struct fixture *make(void){struct fixture *f=calloc(1,sizeof(*f));assert(f);f->io=(struct gww_io){f,sendmsg,pump,now,done};return f;}
static void scan_event(struct fixture *f,unsigned event,unsigned reason,uint32_t id){uint8_t b[36]={0};gwq_w32(b,0x3001);gww_tlv(b+4,36,28);gwq_w32(b+8,event);gwq_w32(b+12,reason);gwq_w32(b+20,0xa001);gwq_w32(b+24,id);assert(!rx(f,1,b,sizeof(b)));}
static void beacon(struct fixture *f,int corrupt){
    uint8_t b[136]={0};gwq_w32(b,0x7001);gww_tlv(b+4,44,68);gwq_w32(b+8+16,44);gwq_w32(b+8+44,(uint32_t)-54);gwq_w32(b+8+64,2412);
    gww_tlv(b+76,17,44);uint8_t *frame=b+80;frame[0]=0x80;frame[16]=2;frame[21]=7;gwq_w16(frame+32,100);gwq_w16(frame+34,0x11);
    frame[36]=0;frame[37]=corrupt?33:3;memcpy(frame+38,"AP1",3);frame[41]=1;frame[42]=1;frame[43]=0x82;
    assert(!rx(f,1,b,124));
}
int main(void){uint8_t mac[6]={2,1,2,3,4,5};struct fixture *f=make();assert(!gww_initialize(&f->s,&f->io,mac));assert(f->s.vdev&&f->s.fw_ready);
    assert(!gww_scan(&f->s,&f->io));assert(gww_scan(&f->s,&f->io)==GWW_BUSY);beacon(f,1);assert(!f->s.bss_count);beacon(f,0);
    assert(f->s.bss_count==1&&f->s.bss[0].ssid_len==3&&f->s.bss[0].rssi==-54&&f->s.bss[0].capability==0x11);
    scan_event(f,2,0,f->s.scan_id+1);assert(f->s.scanning&&!f->done);scan_event(f,1,0,f->s.scan_id);assert(f->s.scan_started);
    scan_event(f,2,0,f->s.scan_id);assert(!f->s.scanning&&f->done==1&&!f->status);beacon(f,0);assert(f->s.bss_count==1);
    assert(!gww_scan(&f->s,&f->io));assert(!f->s.bss_count);f->time+=20000;gww_tick(&f->s,&f->io);assert(f->done==2&&f->status==GWW_TIMEOUT);free(f);
    f=make();f->drop=1;assert(gww_initialize(&f->s,&f->io,mac)==GWW_TIMEOUT&&!f->s.vdev);free(f);
    f=make();f->fail=1;assert(gww_initialize(&f->s,&f->io,mac)==GWW_TRANSPORT&&!f->s.vdev);free(f);
    f=make();assert(!gww_initialize(&f->s,&f->io,mac));uint8_t bad[16]={1,2,8,0,8,0,0,0,1,4,0,0,1,255,0,0};
    assert(gww_receive(&f->s,&f->io,2,bad,sizeof(bad))==GWW_CREDITS);free(f);
    f=make();uint8_t packet[12]={0,0,100,0};assert(gww_receive(&f->s,&f->io,2,packet,12)==GWW_FRAME);free(f);
    puts("HTC/WMI model: initialization wire layouts, passive scan, received BSS parsing, completion, timeout and malformed packets passed");return 0;}
