#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fixtures/ath12k_wifi_station_layout.h"
#include "../ios16/wifi/giulia_wifi_wmi.h"
struct fixture {struct gww_state w;struct gww_io io;uint64_t now;unsigned sent,done,status,send_error;uint8_t command[1100];size_t bytes;};
static int sendmsg(void *p,unsigned pipe,const void *v,size_t n){struct fixture *f=p;assert(pipe==3 && n-8<=sizeof(f->command));memcpy(f->command,(const uint8_t *)v+8,n-8);f->bytes=n-8;f->sent++;f->w.credits=f->w.credit_total;return (int)f->send_error;}
static int pump(void *p,unsigned us){((struct fixture *)p)->now+=us/1000;return 0;}
static uint64_t now(void *p){return ((struct fixture *)p)->now;}
static void done(void *p,unsigned status,uint32_t id){struct fixture *f=p;assert(id==f->w.scan_id);f->done++;f->status=status;}
static struct fixture *make(void){struct fixture *f=calloc(1,sizeof(*f));assert(f);f->io=(struct gww_io){f,sendmsg,pump,now,done};
    f->w.vdev=f->w.fw_ready=f->w.hw_valid=1;f->w.endpoint=1;f->w.max_message=2048;f->w.credit_size=256;f->w.credit_total=f->w.credits=255;
    struct gwreg_state *r=&f->w.regulatory;r->valid=1;r->count=3;r->max_bw[0]=r->max_bw[1]=40;r->min_bw[0]=r->min_bw[1]=20;
    r->rules[0]=(struct gwreg_rule){2402,2482,40,20,6,0};r->rules[1]=(struct gwreg_rule){5170,5250,40,23,6,2};r->rules[2]=(struct gwreg_rule){5490,5710,40,23,6,8};return f;}
static void scan_event_vdev(struct fixture *f,unsigned reason,unsigned event_id,unsigned scan_id,unsigned requestor,unsigned vdev){
    struct wmi_scan_event e={0};e.event_type=event_id;e.reason=reason;e.scan_id=scan_id;e.scan_req_id=requestor;e.vdev_id=vdev;
    uint8_t p[8+sizeof(e)];gwq_w32(p,WMI_SCAN_EVENTID);gww_tlv(p+4,WMI_TAG_SCAN_EVENT,sizeof(e));memcpy(p+8,&e,sizeof(e));
    assert(gww_wmi_receive(&f->w,&f->io,p,sizeof(p))==GWW_OK);
}
static void scan_event(struct fixture *f,unsigned reason,unsigned event_id,unsigned scan_id,unsigned requestor){scan_event_vdev(f,reason,event_id,scan_id,requestor,0);}
static unsigned field(struct fixture *f,size_t offset){return gwq_u32(f->command+4+offset);}
#define FIELD(f,name) field(f,offsetof(struct wmi_start_scan_cmd,name))
static void end(struct fixture *f){scan_event(f,0,1,f->w.scan_id,0xa001);scan_event(f,0,2,f->w.scan_id,0xa001);assert(!f->w.scanning);}
static void frame(struct fixture *f,unsigned hidden,unsigned wrong){uint8_t b[64]={0};b[0]=hidden?0x80:0x50;b[16]=2;b[21]=(uint8_t)(wrong?8:7);gwq_w16(b+32,100);gwq_w16(b+34,1);
    b[36]=0;b[37]=(uint8_t)(hidden==1?0:3);if(!hidden)memcpy(b+38,"lab",3);unsigned at=hidden==1?38:41;b[at++]=1;b[at++]=1;b[at++]=0x82;gww_beacon(&f->w,&f->io,b,at,2412,-40);}
static void event_lifetime(void) {
    struct fixture *f=make();assert(!gww_scan(&f->w,&f->io));uint32_t id=f->w.scan_id;
    /* Linux v6.12 ath12k_scan_event: events are enum values, not masks.
     * PREEMPTED/RESTARTED keep firmware ownership; normal completion is
     * valid only after STARTED, or while explicitly aborting. */
    scan_event(f,0,2,id,0xa001);assert(f->w.scanning && !f->done);
    scan_event(f,0,1,id+1,0xa001);scan_event(f,0,1,id,0xa002);scan_event_vdev(f,0,1,id,0xa001,1);assert(!f->w.scan_started);
    scan_event(f,0,3,id,0xa001);assert(!f->w.scan_started && !f->done);
    scan_event(f,0,1,id,0xa001);assert(f->w.scan_started);
    static const unsigned continuing[]={4,8,32,128,256,512,1024,3,64};
    for(unsigned i=0;i<sizeof(continuing)/sizeof(continuing[0]);i++) {
        scan_event(f,0,continuing[i],id,0xa001);
        assert(f->w.scanning && !f->done && gww_scan(&f->w,&f->io)==GWW_BUSY);
    }
    scan_event(f,0xffffffffu,2,id,0xa001);assert(!f->w.scanning && f->done==1 && f->status);
    scan_event(f,0,2,id,0xa001);assert(f->done==1);
    assert(!gww_scan(&f->w,&f->io));scan_event(f,0,64,f->w.scan_id,0xa001);
    assert(!f->w.scanning && f->done==2 && f->status);
    assert(!gww_scan(&f->w,&f->io));scan_event(f,0,16,f->w.scan_id,0xa001);
    assert(!f->w.scanning && f->done==3 && f->status);
    assert(!gww_scan(&f->w,&f->io));assert(!gww_scan_cancel(&f->w,&f->io));unsigned sent=f->sent;
    assert(!gww_scan_cancel(&f->w,&f->io) && f->sent==sent);
    /* Even if completion races STOP, cancelled results cannot be selected. */
    scan_event(f,0,2,f->w.scan_id,0xa001);assert(!f->w.scanning && f->done==4 && f->status);
    free(f);
    f=make();assert(!gww_scan(&f->w,&f->io));f->io.send=NULL;
    assert(gww_scan_cancel(&f->w,&f->io)==GWW_ARGUMENT && f->w.error==GWW_ARGUMENT && f->done==1);
    f->io.send=sendmsg;assert(gww_scan(&f->w,&f->io)==GWW_STATE);free(f);
    f=make();f->send_error=1;assert(gww_scan(&f->w,&f->io)==GWW_TRANSPORT);
    assert(!f->w.scanning && f->done==1 && f->status==GWW_TRANSPORT);free(f);
    f=make();assert(!gww_scan(&f->w,&f->io));
    uint8_t truncated[32]={0};gwq_w32(truncated,WMI_SCAN_EVENTID);gww_tlv(truncated+4,WMI_TAG_SCAN_EVENT,24);
    gwq_w32(truncated+8,2);gwq_w32(truncated+20,0xa001);gwq_w32(truncated+24,f->w.scan_id);
    assert(gww_wmi_receive(&f->w,&f->io,truncated,sizeof(truncated))==GWW_FRAME);
    assert(f->w.scanning && !f->done);gww_tick(&f->w,&f->io);
    assert(!f->w.scanning && f->done==1 && f->status==GWW_FRAME);free(f);
}
static void scan_bounds(void) {
    struct fixture *f=make();struct gww_scan_request q={0};q.active=1;
    q.ssid_count=2;q.ssids[0].length=q.ssids[1].length=3;memcpy(q.ssids[0].bytes,"lab",3);memcpy(q.ssids[1].bytes,"lab",3);
    assert(gww_scan_begin(&f->w,&f->io,&q)==GWW_ARGUMENT && !f->sent);
    q.ssid_count=0;q.bssid_count=2;q.bssids[0][0]=q.bssids[1][0]=2;
    assert(gww_scan_begin(&f->w,&f->io,&q)==GWW_ARGUMENT && !f->sent);
    q.bssid_count=0;q.frequency_count=2;q.frequencies[0]=q.frequencies[1]=2412;
    assert(gww_scan_begin(&f->w,&f->io,&q)==GWW_ARGUMENT && !f->sent);
    q.frequency_count=1;q.frequencies[0]=2484;
    assert(gww_scan_begin(&f->w,&f->io,&q)==GWW_ARGUMENT && !f->sent);
    q.frequencies[0]=2412;q.connected=1;q.home_frequency=5180;
    assert(gww_scan_begin(&f->w,&f->io,&q)==GWW_STATE && !f->sent);
    q.connected=0;q.home_frequency=2412;
    assert(gww_scan_begin(&f->w,&f->io,&q)==GWW_ARGUMENT && !f->sent);
    memset(&q,0,sizeof(q));q.active=1;q.frequency_count=GWW_SCAN_FREQ_MAX;
    f->w.regulatory.rules[1]=(struct gwreg_rule){5170,5835,40,23,6,0};f->w.regulatory.count=2;
    for(unsigned i=0;i<q.frequency_count;i++)q.frequencies[i]=(uint16_t)(i<13?2412+5*i:5180+5*(i-13));
    q.ssid_count=GWW_SCAN_SSID_MAX;
    for(unsigned i=0;i<q.ssid_count;i++){q.ssids[i].length=32;memset(q.ssids[i].bytes,(int)i+1,32);}
    q.bssid_count=GWW_SCAN_BSSID_MAX;
    for(unsigned i=0;i<q.bssid_count;i++){q.bssids[i][0]=2;q.bssids[i][5]=(uint8_t)i;}
    assert(!gww_scan_begin(&f->w,&f->io,&q));
    size_t at=4+sizeof(struct wmi_start_scan_cmd);assert(FIELD(f,num_chan)==GWW_SCAN_FREQ_MAX);
    assert(gwq_u16(f->command+at)==4*GWW_SCAN_FREQ_MAX);at+=4+4*GWW_SCAN_FREQ_MAX;
    assert(gwq_u16(f->command+at)==sizeof(struct ath12k_wmi_ssid_params)*GWW_SCAN_SSID_MAX);at+=4;
    for(unsigned i=0;i<GWW_SCAN_SSID_MAX;i++,at+=sizeof(struct ath12k_wmi_ssid_params))
        assert(gwq_u32(f->command+at)==32 && !memcmp(f->command+at+4,q.ssids[i].bytes,32));
    assert(gwq_u16(f->command+at)==sizeof(struct ath12k_wmi_mac_addr_params)*GWW_SCAN_BSSID_MAX);at+=4;
    for(unsigned i=0;i<GWW_SCAN_BSSID_MAX;i++,at+=sizeof(struct ath12k_wmi_mac_addr_params))
        assert(!memcmp(f->command+at,q.bssids[i],6) && !gwq_u16(f->command+at+6));
    assert(gwq_u16(f->command+at)==0 && at+4==f->bytes);end(f);free(f);
}
static void regulatory_changes(void) {
    struct fixture *f=make();struct gww_scan_request q={0};q.connected=1;q.home_frequency=2412;
    q.frequency_count=1;q.frequencies[0]=5180;
    assert(!gww_scan_begin(&f->w,&f->io,&q));
    f->w.regulatory.rules[0].flags=2;f->w.regulatory.generation++;gww_tick(&f->w,&f->io);
    assert(f->w.scanning && f->w.scan_cancel_pending && f->w.scan_failure==GWW_STATE);
    end(f);assert(f->status==GWW_STATE);
    q.connected=0;q.home_frequency=0;assert(!gww_scan_begin(&f->w,&f->io,&q));
    f->w.regulatory.valid=0;f->w.regulatory.generation++;gww_tick(&f->w,&f->io);
    assert(f->w.scanning && !f->w.scan_cancel_pending);end(f);assert(!f->status);free(f);
}
int main(void){
    struct fixture *f=make();struct gww_scan_request q={0};q.active=1;q.connected=1;q.home_frequency=2412;
    q.ssid_count=1;q.ssids[0].length=3;memcpy(q.ssids[0].bytes,"lab",3);q.bssid_count=1;q.bssids[0][0]=2;q.bssids[0][5]=7;
    q.frequency_count=3;q.frequencies[0]=2412;q.frequencies[1]=5180;q.frequencies[2]=5500;
    assert(gww_scan_begin(&f->w,&f->io,&q)==GWW_OK && f->w.scan_request.frequency_count==1);
    assert(gwq_u32(f->command)==WMI_START_SCAN_CMDID && gwq_u16(f->command+4)==sizeof(struct wmi_start_scan_cmd)-4);
    assert(FIELD(f,scan_priority)==1 && FIELD(f,dwell_time_active)==40 && FIELD(f,min_rest_time)==100 && FIELD(f,max_rest_time)==500 && FIELD(f,burst_duration)==110);
    unsigned flags=FIELD(f,scan_ctrl_flags);assert(!(flags&(WMI_SCAN_FLAG_PASSIVE|WMI_SCAN_FLAG_FORCE_ACTIVE_ON_DFS|WMI_SCAN_ADD_BCAST_PROBE_REQ)) && (flags&WMI_SCAN_FLAG_STRICT_PASSIVE_ON_PCHN));
    size_t at=4+sizeof(struct wmi_start_scan_cmd);assert(gwq_u16(f->command+at)==4 && gwq_u32(f->command+at+4)==2412);at+=8;
    assert(gwq_u16(f->command+at)==sizeof(struct ath12k_wmi_ssid_params));assert(gwq_u32(f->command+at+4)==3 && !memcmp(f->command+at+8,"lab",3));at+=4+sizeof(struct ath12k_wmi_ssid_params);
    assert(gwq_u16(f->command+at)==sizeof(struct ath12k_wmi_mac_addr_params) && !memcmp(f->command+at+4,q.bssids[0],6));
    frame(f,1,0);frame(f,2,0);assert(!f->w.bss_count);frame(f,0,1);assert(!f->w.bss_count);frame(f,0,0);assert(f->w.bss_count==1 && f->w.bss[0].ssid_len==3);
    struct gww_bss snapshot=f->w.bss[0];frame(f,1,0);frame(f,2,0);assert(!memcmp(&snapshot,&f->w.bss[0],sizeof(snapshot)));
    assert(gww_scan_cancel(&f->w,&f->io)==GWW_OK && f->w.scanning && f->w.scan_cancel_pending);
    assert(gwq_u32(f->command)==WMI_STOP_SCAN_CMDID && gwq_u16(f->command+4)==sizeof(struct wmi_stop_scan_cmd)-4);
    assert(gwq_u32(f->command+4+offsetof(struct wmi_stop_scan_cmd,scan_id))==f->w.scan_id);
    scan_event(f,1,2,f->w.scan_id+1,0xa001);scan_event(f,1,2,f->w.scan_id,0xa002);assert(f->w.scanning && !f->done);
    scan_event(f,1,2,f->w.scan_id,0xa001);assert(!f->w.scanning && f->done==1 && f->status);
    q.active=0;assert(gww_scan_begin(&f->w,&f->io,&q)==GWW_OK && FIELD(f,num_chan)==3);assert(FIELD(f,scan_ctrl_flags)&WMI_SCAN_FLAG_PASSIVE);end(f);
    q.active=1;q.frequency_count=1;q.frequencies[0]=5180;unsigned sent=f->sent;assert(gww_scan_begin(&f->w,&f->io,&q)==GWW_STATE && f->sent==sent);
    q.frequency_count=GWW_SCAN_FREQ_MAX+1;assert(gww_scan_begin(&f->w,&f->io,&q)==GWW_ARGUMENT);q.frequency_count=1;q.frequencies[0]=2412;
    q.ssid_count=GWW_SCAN_SSID_MAX+1;assert(gww_scan_begin(&f->w,&f->io,&q)==GWW_ARGUMENT);q.ssid_count=1;q.ssids[0].length=33;assert(gww_scan_begin(&f->w,&f->io,&q)==GWW_ARGUMENT);q.ssids[0].length=3;
    q.bssid_count=GWW_SCAN_BSSID_MAX+1;assert(gww_scan_begin(&f->w,&f->io,&q)==GWW_ARGUMENT);q.bssid_count=1;q.bssids[0][0]=1;assert(gww_scan_begin(&f->w,&f->io,&q)==GWW_ARGUMENT);q.bssids[0][0]=2;
    q.connected=0;q.home_frequency=0;q.ssid_count=q.bssid_count=0;assert(gww_scan_begin(&f->w,&f->io,&q)==GWW_OK);
    assert(FIELD(f,num_ssids)==1 && FIELD(f,num_bssid)==1 && (FIELD(f,scan_ctrl_flags)&WMI_SCAN_ADD_BCAST_PROBE_REQ));
    frame(f,2,0);assert(f->w.bss_count==1 && !f->w.bss[0].ssid_len);
    frame(f,0,0);snapshot=f->w.bss[0];frame(f,1,0);frame(f,2,0);assert(!memcmp(&snapshot,&f->w.bss[0],sizeof(snapshot)));
    f->w.regulatory.rules[0].flags=2;f->w.regulatory.generation++;gww_tick(&f->w,&f->io);
    assert(f->w.scanning && f->w.scan_cancel_pending && f->w.scan_failure==GWW_STATE);end(f);assert(f->status==GWW_STATE);
    free(f);f=make();assert(!gww_scan(&f->w,&f->io));f->now=20000;gww_tick(&f->w,&f->io);assert(f->w.scanning && f->w.scan_cancel_pending && !f->done);
    f->now+=5000;gww_tick(&f->w,&f->io);assert(!f->w.scanning && f->w.error==GWW_TIMEOUT && f->status==GWW_TIMEOUT);free(f);
    event_lifetime();scan_bounds();regulatory_changes();
    puts("WMI directed/background scan: upstream wire oracle, maximum SSID/BSSID/channel bounds, passive/DFS filtering, connected home permission, hidden response integrity, firmware event ownership, real cancellation and timeout quiescence passed");return 0;
}
