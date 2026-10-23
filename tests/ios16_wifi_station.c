#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fixtures/ath12k_wifi_station_layout.h"
#include "../ios16/giulia_wifi_station.h"

/* Source-derived packed structures encode firmware messages independently of
 * station.h. Every event enters the actual HTC/WMI receive dispatcher. */
struct fixture {
    struct gww_state wmi;struct gww_io io;struct gwt_state sta;
    struct gwt_request request;
    uint64_t now;unsigned sent,mgmt_holds,mgmt_releases,reo_count,key_ready,port_open,port_error,ready_error;
    unsigned fail_send,fail_send_at,pmf_qos,auth_started,auth_rx_bytes,protected_ready,expected_phymode;
    unsigned ps_parameters,ps_modes;uint32_t lease_cookie;
    uint8_t command[512],frame[GWT_MGMT_MAX];size_t command_bytes,frame_bytes;
};
#define CMD32(f,type,member) gwq_u32((f)->command+4+offsetof(struct type,member))
static uint64_t now_ms(void *p){return ((struct fixture *)p)->now;}
static int pump(void *p,unsigned us){((struct fixture *)p)->now+=us/1000;return 0;}
static int sendmsg(void *p,unsigned pipe,const void *data,size_t n) {
    struct fixture *f=p;const uint8_t *b=data;
    assert(pipe==3 && b[0]==1 && n>=12 && n-8<=sizeof(f->command));
    assert(gwq_u16(b+2)==n-8);memcpy(f->command,b+8,n-8);f->command_bytes=n-8;f->sent++;
    if(gwq_u32(f->command)==WMI_STA_POWERSAVE_PARAM_CMDID) {
        static const unsigned parameters[]={WMI_STA_PS_PARAM_UAPSD,WMI_STA_PS_PARAM_RX_WAKE_POLICY,
            WMI_STA_PS_PARAM_TX_WAKE_THRESHOLD,WMI_STA_PS_PARAM_PSPOLL_COUNT};
        static const unsigned values[]={0,WMI_STA_PS_RX_WAKE_POLICY_WAKE,
            WMI_STA_PS_TX_WAKE_THRESHOLD_ALWAYS,WMI_STA_PS_PSPOLL_COUNT_NO_MAX};
        unsigned index=f->ps_parameters++%4;
        assert(f->command_bytes==4+sizeof(struct wmi_sta_powersave_param_cmd));
        assert(gwq_u16(f->command+4)==sizeof(struct wmi_sta_powersave_param_cmd)-4);
        assert(gwq_u16(f->command+6)==WMI_TAG_STA_POWERSAVE_PARAM_CMD);
        assert(!CMD32(f,wmi_sta_powersave_param_cmd,vdev_id));
        assert(CMD32(f,wmi_sta_powersave_param_cmd,param)==parameters[index]);
        assert(CMD32(f,wmi_sta_powersave_param_cmd,value)==values[index]);
    }
    if(gwq_u32(f->command)==WMI_STA_POWERSAVE_MODE_CMDID) {
        assert(f->ps_parameters && !(f->ps_parameters%4));f->ps_modes++;
        assert(f->command_bytes==4+sizeof(struct wmi_pdev_set_ps_mode_cmd));
        assert(gwq_u16(f->command+4)==sizeof(struct wmi_pdev_set_ps_mode_cmd)-4);
        assert(gwq_u16(f->command+6)==WMI_TAG_STA_POWERSAVE_MODE_CMD);
        assert(!CMD32(f,wmi_pdev_set_ps_mode_cmd,vdev_id));
        assert(CMD32(f,wmi_pdev_set_ps_mode_cmd,sta_ps_mode)==WMI_STA_PS_MODE_DISABLED);
    }
    if(gwq_u32(f->command)==WMI_PEER_REORDER_QUEUE_SETUP_CMDID) {
        assert(gwq_u16(f->command+4)==sizeof(struct wmi_peer_reorder_queue_setup_cmd)-4);
        assert(gwq_u16(f->command+6)==WMI_TAG_REORDER_QUEUE_SETUP_CMD);
        assert(CMD32(f,wmi_peer_reorder_queue_setup_cmd,tid)==f->reo_count-1);
        assert(CMD32(f,wmi_peer_reorder_queue_setup_cmd,queue_no)==f->reo_count-1);
        assert(CMD32(f,wmi_peer_reorder_queue_setup_cmd,queue_ptr_lo)==0x200000+(f->reo_count-1)*0x1000);
        assert(CMD32(f,wmi_peer_reorder_queue_setup_cmd,ba_window_size_valid)==1);
        assert(CMD32(f,wmi_peer_reorder_queue_setup_cmd,ba_window_size)==1);
    }
    if(gwq_u32(f->command)==WMI_PDEV_SET_PARAM_CMDID) {
        assert(gwq_u16(f->command+6)==WMI_TAG_PDEV_SET_PARAM_CMD);
        assert(CMD32(f,wmi_pdev_set_param_cmd,param_id)==WMI_PDEV_PARAM_PMF_QOS && CMD32(f,wmi_pdev_set_param_cmd,param_value)==1);
        assert(!CMD32(f,wmi_pdev_set_param_cmd,pdev_id));f->pmf_qos++;
    }
    /* Credit replenishment is irrelevant to station ownership; no event or
     * command completion is synthesized by this transport fixture. */
    unsigned failed=f->fail_send || (f->fail_send_at && f->sent==f->fail_send_at);
    if(!failed)f->wmi.credits=f->wmi.credit_total;
    return (int)failed;
}
static int ready(void *p,unsigned encrypted){(void)encrypted;return ((struct fixture *)p)->ready_error;}
static int channel(void *p,unsigned freq,struct gwt_channel *c){return gwt_channel_allowed(&((struct fixture *)p)->wmi,freq,c);}
static int mgmt_hold(void *p,const uint8_t *b,size_t n,uint32_t cookie,uint64_t *iova) {
    struct fixture *f=p;assert(!f->lease_cookie && n<=sizeof(f->frame));
    memcpy(f->frame,b,n);f->frame_bytes=n;f->lease_cookie=cookie;f->mgmt_holds++;*iova=0x123456000;return 0;
}
static void mgmt_done(void *p,uint32_t cookie,unsigned status) {
    struct fixture *f=p;(void)status;assert(cookie && cookie==f->lease_cookie);
    f->lease_cookie=0;f->mgmt_releases++;
}
static int reo(void *p,unsigned tid,uint64_t *iova){struct fixture *f=p;assert(tid==f->reo_count++);*iova=0x200000+tid*0x1000;return 0;}
static int key_ready(void *p,unsigned peer,unsigned index,unsigned pairwise,uint64_t rsc) {
    struct fixture *f=p;assert(peer==7 && index<=3 && pairwise<=1 && rsc<=UINT64_C(0xffffffffffff));f->key_ready++;return 0;
}
static int port(void *p,unsigned peer,unsigned open){struct fixture *f=p;assert(peer==7);if(f->port_error)return 1;f->port_open=open;return 0;}
static void receive(struct fixture *f,unsigned event,const uint8_t *payload,size_t bytes) {
    uint8_t packet[2048]={0};packet[0]=1;gwq_w16(packet+2,(uint16_t)(bytes+4));gwq_w32(packet+8,event);
    memcpy(packet+12,payload,bytes);assert(gww_receive(&f->wmi,&f->io,2,packet,12+bytes)==GWW_OK);
}
static void event(struct fixture *f,unsigned id,unsigned tag,const void *data,size_t bytes) {
    uint8_t payload[1024];assert(bytes+4<=sizeof(payload));gww_tlv(payload,tag,(unsigned)bytes);
    memcpy(payload+4,data,bytes);receive(f,id,payload,bytes+4);
}
static void tx_done(struct fixture *f,uint32_t cookie,unsigned status) {
    struct wmi_mgmt_tx_compl_event e={cookie,status,2};
    /* WCN7850 single_pdev_only routes all physical pdev IDs to logical 0. */
    event(f,WMI_MGMT_TX_COMPLETION_EVENTID,WMI_TAG_MGMT_TX_COMPL_EVENT,&e,sizeof(e));
}
static void mgmt_rx(struct fixture *f,const uint8_t *frame,size_t n,unsigned status) {
    uint8_t payload[512]={0};struct ath12k_wmi_mgmt_rx_params header={0};
    header.buf_len=(uint32_t)n;header.chan_freq=f->request.bss.frequency;header.pdev_id=2;header.status=status;
    gww_tlv(payload,WMI_TAG_MGMT_RX_HDR,sizeof(header));memcpy(payload+4,&header,sizeof(header));
    size_t at=4+sizeof(header);gww_tlv(payload+at,WMI_TAG_ARRAY_BYTE,(unsigned)n);memcpy(payload+at+4,frame,n);
    receive(f,WMI_MGMT_RX_EVENTID,payload,at+4+n);
}
static void response(struct fixture *f,unsigned subtype,uint8_t frame[64]) {
    memset(frame,0,64);frame[0]=(uint8_t)subtype;
    memcpy(frame+4,f->wmi.mac,6);memcpy(frame+10,f->request.bss.bssid,6);memcpy(frame+16,f->request.bss.bssid,6);
}
static void regulatory(struct fixture *f,unsigned flags,unsigned phy,unsigned country) {
    struct wmi_reg_chan_list_cc_ext_event header={0};struct ath12k_wmi_reg_rule_ext_params rule={0};
    header.phy_id=phy;header.alpha2=country;header.num_phy=2;header.min_bw_2g=20;header.max_bw_2g=40;
    header.num_2g_reg_rules=1;rule.tlv_header=(WMI_TAG_REGULATORY_RULE_EXT_STRUCT<<16)|(sizeof(rule)-4);
    rule.freq_info=2402|(2482u<<16);rule.bw_pwr_info=40|(20u<<16)|(6u<<24);rule.flag_info=flags;
    uint8_t payload[4+sizeof(header)+4+sizeof(rule)];
    gww_tlv(payload,WMI_TAG_REG_CHAN_LIST_CC_EXT_EVENT,sizeof(header));memcpy(payload+4,&header,sizeof(header));
    gww_tlv(payload+4+sizeof(header),WMI_TAG_ARRAY_STRUCT,sizeof(rule));memcpy(payload+8+sizeof(header),&rule,sizeof(rule));
    receive(f,WMI_REG_CHAN_LIST_CC_EXT_EVENTID,payload,sizeof(payload));
}
static struct fixture *make(unsigned security) {
    struct fixture *f=calloc(1,sizeof(*f));assert(f);f->now=100;f->expected_phymode=MODE_11B;
    f->io=(struct gww_io){f,sendmsg,pump,now_ms,NULL};
    f->wmi.endpoint=1;f->wmi.credit_size=256;f->wmi.credits=f->wmi.credit_total=255;f->wmi.max_message=2048;
    f->wmi.fw_ready=f->wmi.hw_valid=f->wmi.vdev=f->wmi.services_valid=1;
    f->wmi.services[2]=(1u<<(76-64))|(1u<<(79-64))|(1u<<(92-64));
    memcpy(f->wmi.mac,"\x02\x01\x02\x03\x04\x05",6);
    struct gwt_ops ops={f,ready,channel,mgmt_hold,mgmt_done,reo,key_ready,port,NULL};
    assert(gwt_init(&f->sta,&f->wmi,&f->io,&ops)==GWT_OK);regulatory(f,0,0,'C'|('N'<<8));
    assert(f->wmi.regulatory.valid);
    struct gww_bss *b=&f->request.bss;memcpy(b->bssid,"\x02\x0a\x0b\x0c\x0d\x0e",6);
    memcpy(b->ssid,"lab",3);b->ssid_len=3;b->frequency=2412;b->beacon=100;b->capability=1|(security?0x10:0);
    b->rate_count=4;memcpy(b->rates,"\x82\x84\x8b\x96",4);f->request.security=security;
    if(security){static const uint8_t rsn[]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};memcpy(b->ie,rsn,sizeof(rsn));b->ie_bytes=sizeof(rsn);}
    return f;
}
static void begin(struct fixture *f) {
    assert(gwt_connect(&f->sta,&f->request)==GWT_OK && f->sta.phase==GWT_PEER_WAIT);
    assert(gwq_u32(f->command)==WMI_PEER_CREATE_CMDID && f->command_bytes==4+sizeof(struct wmi_peer_create_cmd));
    assert(gwq_u16(f->command+6)==WMI_TAG_PEER_CREATE_CMD && !CMD32(f,wmi_peer_create_cmd,peer_type));
    assert(!memcmp(f->command+4+offsetof(struct wmi_peer_create_cmd,peer_macaddr),f->request.bss.bssid,6));
    assert(f->sta.active_policy && f->ps_parameters==4 && f->ps_modes==1);
    gwt_tick(&f->sta);assert(f->sent==6 && f->sta.phase==GWT_PEER_WAIT);
    gwt_peer_map(&f->sta,1,f->request.bss.bssid,7,1);assert(!f->sta.peer_mapped);
    gwt_peer_map(&f->sta,0,f->request.bss.bssid,7,1);gwt_tick(&f->sta);
    assert(f->sta.phase==GWT_START_WAIT && f->reo_count==17);
    assert(gwq_u32(f->command)==WMI_VDEV_START_REQUEST_CMDID);
    assert(gwq_u16(f->command+4)==sizeof(struct wmi_vdev_start_request_cmd)-4);
    assert(CMD32(f,wmi_vdev_start_request_cmd,beacon_interval)==100);
    assert(CMD32(f,wmi_vdev_start_request_cmd,preferred_tx_streams)==1);
    assert(CMD32(f,wmi_vdev_start_request_cmd,preferred_rx_streams)==1);
    assert(CMD32(f,wmi_vdev_start_request_cmd,punct_bitmap)==UINT32_MAX);
    struct wmi_vdev_start_resp_event start={0};start.vdev_id=1;
    event(f,WMI_VDEV_START_RESP_EVENTID,WMI_TAG_VDEV_START_RESPONSE_EVENT,&start,sizeof(start));
    gwt_tick(&f->sta);assert(f->sta.phase==GWT_START_WAIT && !f->mgmt_holds);
    start.vdev_id=0;event(f,WMI_VDEV_START_RESP_EVENTID,WMI_TAG_VDEV_START_RESPONSE_EVENT,&start,sizeof(start));
    gwt_tick(&f->sta);assert(f->sta.phase==GWT_AUTH_WAIT && f->lease_cookie);
    assert(gwq_u32(f->command)==WMI_MGMT_TX_SEND_CMDID);
    assert(gwq_u16(f->command+4)==sizeof(struct wmi_mgmt_send_cmd)-4);
    assert(CMD32(f,wmi_mgmt_send_cmd,desc_id)==f->lease_cookie);
    assert(CMD32(f,wmi_mgmt_send_cmd,paddr_lo)==0x23456000 && CMD32(f,wmi_mgmt_send_cmd,paddr_hi)==1);
    assert(!CMD32(f,wmi_mgmt_send_cmd,chanfreq) && CMD32(f,wmi_mgmt_send_cmd,frame_len)==30);
    assert(f->frame[0]==0xb0 && gwq_u16(f->frame+26)==1);
}
static void associated(struct fixture *f) {
    begin(f);uint8_t frame[64];response(f,0xb0,frame);gwq_w16(frame+26,2);
    mgmt_rx(f,frame,30,1);assert(!f->sta.auth_confirm);mgmt_rx(f,frame,30,0);
    gwt_tick(&f->sta);assert(f->sta.phase==GWT_AUTH_WAIT);
    tx_done(f,f->lease_cookie+1,0);assert(f->lease_cookie);
    tx_done(f,f->lease_cookie,0);gwt_tick(&f->sta);assert(f->sta.phase==GWT_ASSOC_WAIT && f->frame[0]==0);
    assert(!memcmp(f->frame+30,"lab",3) && f->frame[33]==1 && f->frame[35]==f->sta.rates[0]);
    response(f,0x10,frame);gwq_w16(frame+24,f->request.bss.capability);gwq_w16(frame+28,0xc025);
    frame[30]=1;frame[31]=2;frame[32]=f->sta.rates[0];frame[33]=f->sta.rates[1];
    mgmt_rx(f,frame,33,0);assert(!f->sta.assoc_confirm);mgmt_rx(f,frame,34,0);
    tx_done(f,f->lease_cookie,0);gwt_tick(&f->sta);assert(f->sta.phase==GWT_ASSOC_CONFIRM);
    assert(gwq_u32(f->command)==WMI_PEER_ASSOC_CMDID);
    assert(gwq_u16(f->command+4)==sizeof(struct wmi_peer_assoc_complete_cmd)-4);
    assert(CMD32(f,wmi_peer_assoc_complete_cmd,peer_associd)==37);
    assert(CMD32(f,wmi_peer_assoc_complete_cmd,peer_flags)==(f->request.security?4u:1u));
    assert(CMD32(f,wmi_peer_assoc_complete_cmd,peer_phymode)==f->expected_phymode);
    assert(CMD32(f,wmi_peer_assoc_complete_cmd,punct_bitmap)==UINT32_MAX);
    size_t at=4+sizeof(struct wmi_peer_assoc_complete_cmd);
    assert(gwq_u16(f->command+at+2)==WMI_TAG_ARRAY_BYTE && f->command[at+4]==(f->sta.rates[0]&0x7f));
    struct wmi_peer_assoc_conf_event peer={0};memcpy(peer.peer_macaddr.addr,f->request.bss.bssid,6);peer.vdev_id=1;
    event(f,WMI_PEER_ASSOC_CONF_EVENTID,WMI_TAG_PEER_ASSOC_CONF_EVENT,&peer,sizeof(peer));
    gwt_tick(&f->sta);assert(f->sta.phase==GWT_ASSOC_CONFIRM);
    peer.vdev_id=0;event(f,WMI_PEER_ASSOC_CONF_EVENTID,WMI_TAG_PEER_ASSOC_CONF_EVENT,&peer,sizeof(peer));gwt_tick(&f->sta);
    assert(gwq_u32(f->command)==WMI_VDEV_UP_CMDID && CMD32(f,wmi_vdev_up_cmd,vdev_assoc_id)==37);
    assert(f->sta.vdev_up && f->sta.phase==(f->request.security?GWT_KEY_WAIT:GWT_RUN));
    assert(f->port_open==!f->request.security);
}
static void key_complete(struct fixture *f,unsigned pairwise,unsigned index,unsigned status) {
    struct wmi_vdev_install_key_compl_event e={0};memcpy(e.peer_macaddr.addr,f->request.bss.bssid,6);
    e.key_idx=index;e.key_flags=pairwise?0:1;e.status=status;
    event(f,WMI_VDEV_INSTALL_KEY_COMPLETE_EVENTID,WMI_TAG_VDEV_INSTALL_KEY_COMPLETE_EVENT,&e,sizeof(e));
}
static void stop_complete(struct fixture *f) {
    struct wmi_vdev_stopped_event e={0};event(f,WMI_VDEV_STOPPED_EVENTID,WMI_TAG_VDEV_STOPPED_EVENT,&e,sizeof(e));
}
static void delete_complete(struct fixture *f) {
    struct wmi_peer_delete_resp_event e={0};memcpy(e.peer_macaddr.addr,f->request.bss.bssid,6);
    event(f,WMI_PEER_DELETE_RESP_EVENTID,WMI_TAG_PEER_DELETE_RESP_EVENT,&e,sizeof(e));
}
static void stop_and_reconnect(void) {
    struct fixture *f=make(0);associated(f);uint8_t frame[64];response(f,0xc0,frame);gwq_w16(frame+24,8);
    memset(frame+4,0xff,6);mgmt_rx(f,frame,26,0);assert(f->sta.phase==GWT_STOPPING && !f->port_open);
    /* Unsolicited old replies cannot skip commands in this teardown. */
    stop_complete(f);delete_complete(f);assert(!f->sta.stop_confirm && !f->sta.delete_confirm);
    gwt_tick(&f->sta);assert(f->lease_cookie);tx_done(f,f->lease_cookie,0);gwt_tick(&f->sta);
    assert(gwq_u32(f->command)==WMI_VDEV_STOP_CMDID && f->sta.stop_stage==2);
    delete_complete(f);assert(!f->sta.delete_confirm);stop_complete(f);gwt_tick(&f->sta);
    assert(gwq_u32(f->command)==WMI_PEER_DELETE_CMDID && f->sta.stop_stage==3);
    delete_complete(f);gwt_tick(&f->sta);assert(f->sta.phase==GWT_STOPPING);
    gwt_peer_map(&f->sta,0,f->request.bss.bssid,7,0);gwt_tick(&f->sta);assert(f->sta.phase==GWT_IDLE);
    unsigned cookie=f->sta.cookie;f->reo_count=0;assert(gwt_connect(&f->sta,&f->request)==GWT_OK);
    assert(!f->sta.stop_confirm && !f->sta.delete_confirm && f->sta.cookie==cookie);gwt_quiesced(&f->sta);free(f);
}
static void keys(void) {
    struct fixture *f=make(1);associated(f);struct gwt_key key={0,1,7,{0}};memset(key.data,0x31,16);
    assert(gwt_install_key(&f->sta,&key)==GWT_OK && f->sta.key_pending && !f->port_open);
    assert(gwq_u32(f->command)==WMI_VDEV_INSTALL_KEY_CMDID && f->command_bytes==4+sizeof(struct wmi_vdev_install_key_cmd)+4+16);
    assert(CMD32(f,wmi_vdev_install_key_cmd,key_cipher)==4 && CMD32(f,wmi_vdev_install_key_cmd,key_len)==16);
    assert(gwq_u64(f->command+4+offsetof(struct wmi_vdev_install_key_cmd,key_rsc_counter))==7);
    assert(!memcmp(f->command+8+sizeof(struct wmi_vdev_install_key_cmd),key.data,16));
    assert(gwt_authorize(&f->sta)==GWT_UNAVAILABLE);key_complete(f,0,0,0);gwt_tick(&f->sta);assert(f->sta.key_pending);
    key_complete(f,1,0,0);gwt_tick(&f->sta);assert(f->key_ready==1 && f->sta.installed==1);
    assert(gwt_authorize(&f->sta)==GWT_UNAVAILABLE);key.pairwise=0;key.index=1;memset(key.data,0x72,16);
    assert(gwt_install_key(&f->sta,&key)==GWT_OK);key_complete(f,0,1,0);gwt_tick(&f->sta);
    assert(gwt_authorize(&f->sta)==GWT_OK && f->port_open && f->sta.phase==GWT_RUN);
    unsigned sent=f->sent;assert(gwt_install_key(&f->sta,&key)==GWT_OK && f->sent==sent);
    f->port_open=0;assert(gwt_authorize(&f->sta)==GWT_OK && f->port_open && f->sent==sent);
    /* Same-index GTK replacement must close/reopen the real controlled port. */
    key.data[0]++;assert(gwt_install_key(&f->sta,&key)==GWT_OK && !f->port_open);
    assert(gwt_authorize(&f->sta)==GWT_UNAVAILABLE);key_complete(f,0,1,0);gwt_tick(&f->sta);
    assert(gwt_authorize(&f->sta)==GWT_OK && f->port_open);
    key.data[0]++;f->port_error=1;sent=f->sent;
    assert(gwt_install_key(&f->sta,&key)==GWT_DATAPATH && f->sta.phase==GWT_STOPPING && f->sent==sent);
    assert(!f->sta.key_pending);gwt_quiesced(&f->sta);free(f);
}
static void legacy_phy_modes(void) {
    for(unsigned kind=0;kind<3;kind++) {
        struct fixture *f=make(GWT_OPEN);f->expected_phymode=kind==2?MODE_11A:MODE_11G;
        if(kind==0)f->request.bss.rates[f->request.bss.rate_count++]=0x8c; /* mixed CCK/OFDM */
        else {f->request.bss.rate_count=2;f->request.bss.rates[0]=0x8c;f->request.bss.rates[1]=0x98;}
        if(kind==2) {
            f->request.bss.frequency=5180;f->wmi.regulatory.count=2;
            f->wmi.regulatory.min_bw[1]=f->wmi.regulatory.max_bw[1]=20;
            f->wmi.regulatory.rules[1]=(struct gwreg_rule){5170,5190,20,20,6,0};
        }
        associated(f);assert(f->port_open && f->sta.phase==GWT_RUN);gwt_quiesced(&f->sta);free(f);
    }
}
static void active_policy(void) {
    struct fixture *f=make(GWT_OPEN);
    assert(gwt_active_policy(&f->sta)==GWT_OK && f->sta.active_policy && f->sent==5);
    assert(f->ps_parameters==4 && f->ps_modes==1 && !f->sta.peer_created && f->sta.phase==GWT_IDLE);
    f->wmi.fw_ready=0;
    assert(gwt_active_policy(&f->sta)==GWT_UNAVAILABLE && !f->sta.active_policy && f->sent==5);
    gwt_quiesced(&f->sta);free(f);
    for(unsigned failed=1;failed<=5;failed++) {
        f=make(GWT_OPEN);f->fail_send_at=failed;f->sta.active_policy=1;
        assert(gwt_active_policy(&f->sta)==GWT_TRANSPORT && !f->sta.active_policy && f->sent==failed);
        assert(!f->sta.peer_created && !f->sta.tx_pending);gwt_quiesced(&f->sta);free(f);
        f=make(GWT_OPEN);f->fail_send_at=failed;
        assert(gwt_connect(&f->sta,&f->request)==GWT_TRANSPORT && !f->sta.active_policy);
        assert(!f->sta.peer_created && !f->sta.tx_pending && f->sent==failed);
        assert(f->sta.phase==GWT_STOPPING);gwt_quiesced(&f->sta);free(f);
    }
}
static void ownership_and_regulatory(void) {
    struct fixture *f=make(0);begin(f);unsigned cookie=f->lease_cookie;
    f->now+=2000;gwt_tick(&f->sta);assert(f->sta.phase==GWT_STOPPING && f->lease_cookie==cookie && !f->mgmt_releases);
    f->now+=5000;gwt_tick(&f->sta);assert(f->sta.phase==GWT_FAILED && f->lease_cookie==cookie);
    gwt_quiesced(&f->sta);assert(f->sta.phase==GWT_IDLE && !f->lease_cookie && f->mgmt_releases==1 && !f->sta.error);
    assert(!f->sta.start_confirm && !f->sta.auth_confirm && !f->sta.stop_confirm && f->sta.cookie==cookie);
    tx_done(f,cookie,0);assert(f->mgmt_releases==1);free(f);
    f=make(0);assert(gwt_connect(&f->sta,&f->request)==GWT_OK);assert(gwt_disconnect(&f->sta,3)==GWT_OK);
    gwt_peer_map(&f->sta,0,f->request.bss.bssid,7,1);assert(f->sta.peer_mapped);
    gwt_tick(&f->sta);delete_complete(f);gwt_tick(&f->sta);assert(f->sta.phase==GWT_STOPPING);
    gwt_peer_map(&f->sta,0,f->request.bss.bssid,7,0);gwt_tick(&f->sta);assert(f->sta.phase==GWT_IDLE);free(f);
    f=make(0);unsigned power=0,gain=0;assert(gwreg_channel(&f->wmi.regulatory,2412,&power,&gain) && power==40 && gain==12);
    regulatory(f,2,1,'C'|('N'<<8));assert(gwreg_channel(&f->wmi.regulatory,2412,&power,&gain));
    const unsigned forbidden[]={1,2,8,64,512,2048};
    for(unsigned i=0;i<sizeof(forbidden)/sizeof(forbidden[0]);i++) {
        regulatory(f,forbidden[i],0,'C'|('N'<<8));assert(!gwreg_channel(&f->wmi.regulatory,2412,&power,&gain));
        assert(gwt_connect(&f->sta,&f->request)==GWT_UNAVAILABLE);
    }
    regulatory(f,0,0,'U'|('S'<<8));assert(!f->wmi.regulatory.valid);
    regulatory(f,0,0,'C'|('N'<<8));associated(f);
    unsigned holds=f->mgmt_holds;
    regulatory(f,2,0,'C'|('N'<<8));gwt_tick(&f->sta);
    assert(f->sta.phase==GWT_STOPPING && !f->port_open && f->mgmt_holds==holds && !f->lease_cookie);
    gwt_quiesced(&f->sta);free(f);
}
static void rejected_and_malformed(void) {
    struct fixture *f=make(0);begin(f);uint8_t frame[64];response(f,0xb0,frame);gwq_w16(frame+26,2);
    frame[10]^=2;mgmt_rx(f,frame,30,0);assert(!f->sta.auth_confirm);frame[10]^=2;
    frame[1]=4;mgmt_rx(f,frame,30,0);assert(!f->sta.auth_confirm);frame[1]=0;
    gwq_w16(frame+28,13);mgmt_rx(f,frame,30,0);assert(f->sta.phase==GWT_STOPPING && f->sta.error==GWT_REJECTED);
    assert(f->lease_cookie);gwt_quiesced(&f->sta);free(f);
    f=make(1);associated(f);struct gwt_key key={0,1,0,{0}};
    assert(gwt_install_key(&f->sta,&key)==GWT_OK);key_complete(f,1,0,1);gwt_tick(&f->sta);
    assert(f->sta.phase==GWT_STOPPING && !f->key_ready && !f->port_open && f->sta.error==GWT_REJECTED);
    gwt_quiesced(&f->sta);free(f);
    f=make(0);begin(f);unsigned cookie=f->lease_cookie;uint8_t truncated[8]={0};gwq_w32(truncated,cookie);
    event(f,WMI_MGMT_TX_COMPLETION_EVENTID,WMI_TAG_MGMT_TX_COMPL_EVENT,truncated,sizeof(truncated));
    assert(f->sta.phase==GWT_STOPPING && f->sta.error==GWT_PROTOCOL && f->lease_cookie==cookie);
    gwt_quiesced(&f->sta);assert(f->mgmt_releases==1);free(f);
    f=make(0);begin(f);cookie=f->lease_cookie;tx_done(f,cookie,1);
    assert(f->sta.phase==GWT_STOPPING && f->sta.error==GWT_TRANSPORT && !f->lease_cookie);
    tx_done(f,cookie,1);assert(f->mgmt_releases==1);gwt_quiesced(&f->sta);free(f);
    f=make(0);begin(f);tx_done(f,f->lease_cookie,0);f->fail_send=1;response(f,0xb0,frame);gwq_w16(frame+26,2);
    mgmt_rx(f,frame,30,0);gwt_tick(&f->sta);
    assert(f->sta.phase==GWT_STOPPING && f->lease_cookie && f->mgmt_releases==1);
    gwt_quiesced(&f->sta);assert(f->mgmt_releases==2);free(f);
}
static int auth_start(void *p){((struct fixture *)p)->auth_started++;return 0;}
static void auth_rx(void *p,const uint8_t *body,size_t bytes){struct fixture *f=p;assert(gwq_u16(body)==3);f->auth_rx_bytes=(unsigned)bytes;}
static int protected_ready(void *p){return !((struct fixture *)p)->protected_ready;}
static int protect_copy(void *p,const uint8_t *frame,size_t n,uint8_t *out,size_t *bytes){(void)p;assert(*bytes>=n);memcpy(out,frame,n);*bytes=n;return 0;}
static int filter_copy(void *p,const uint8_t *frame,size_t n,unsigned status,unsigned frequency,uint8_t *out,size_t *bytes){assert(frequency==((struct fixture *)p)->request.bss.frequency);if(status)return 1;return protect_copy(p,frame,n,out,bytes);}
static void sae_and_loss_events(void){
    struct fixture *f=make(GWT_WPA3_SAE);f->request.pmf=2;f->request.bss.ie[19]=8;f->request.bss.ie[20]=0xc0;
    assert(gwt_connect(&f->sta,&f->request)==GWT_UNSUPPORTED);
    struct gwt_security_ops hooks={f,auth_start,auth_rx,filter_copy,protect_copy,protected_ready};
    assert(gwt_set_security_ops(&f->sta,&hooks)==GWT_OK);assert(gwt_connect(&f->sta,&f->request)==GWT_OK && f->pmf_qos==1);
    gwt_peer_map(&f->sta,0,f->request.bss.bssid,7,1);gwt_tick(&f->sta);
    assert(CMD32(f,wmi_vdev_start_request_cmd,flags)&2);
    struct wmi_vdev_start_resp_event start={0};event(f,WMI_VDEV_START_RESP_EVENTID,WMI_TAG_VDEV_START_RESPONSE_EVENT,&start,sizeof(start));gwt_tick(&f->sta);
    assert(f->sta.phase==GWT_AUTH_WAIT && f->auth_started==1 && !f->lease_cookie);
    uint8_t body[360]={0};gwq_w16(body,3);gwq_w16(body+2,1);assert(gwt_auth_send(&f->sta,body,sizeof(body))==GWT_OK);
    assert(f->frame_bytes==384 && CMD32(f,wmi_mgmt_send_cmd,buf_len)==64 && CMD32(f,wmi_mgmt_send_cmd,frame_len)==384);
    uint8_t frame[64];response(f,0xb0,frame);gwq_w16(frame+24,3);gwq_w16(frame+26,1);mgmt_rx(f,frame,30,0);
    assert(f->auth_rx_bytes==6 && !f->sta.auth_confirm);f->sta.request.h2e=1;f->sta.request.has_pmkid=1;memset(f->sta.request.pmkid,0x73,16);assert(gwt_auth_complete(&f->sta,0)==GWT_OK);
    gwt_tick(&f->sta);assert(f->sta.phase==GWT_AUTH_WAIT);tx_done(f,f->lease_cookie,0);gwt_tick(&f->sta);
    assert(f->sta.phase==GWT_ASSOC_WAIT);size_t at=28;while(f->frame[at]!=48)at+=2+f->frame[at+1];
    assert(f->frame[at+1]==42 && f->frame[at+19]==8 && f->frame[at+20]==0xc0 && f->frame[at+22]==1 && f->frame[at+43]==6);
    assert(!memcmp(f->frame+at+24,f->sta.request.pmkid,16)&&!memcmp(f->frame+at+44,"\xf4\x01\x20",3));
    /* Max SSID/rates plus SAE PMKID and RSNXE exceeds the old 128-byte
     * association buffer. The complete frame must fit without truncation. */
    uint8_t maximum[GWT_ASSOC_MAX];f->sta.request.bss.ssid_len=32;memset(f->sta.request.bss.ssid,'x',32);f->sta.rate_count=16;
    assert(gwt_assoc_frame(&f->sta,maximum)==129 && maximum[126]==244 && maximum[128]==0x20);
    gwt_quiesced(&f->sta);assert(f->sta.security_ops.auth_start==auth_start);free(f);
    f=make(0);associated(f);struct wmi_roam_event roam={0};roam.reason=3;
    event(f,WMI_ROAM_EVENTID,WMI_TAG_ROAM_EVENT,&roam,sizeof(roam));assert(f->sta.phase==GWT_RUN);
    roam.reason=2;roam.vdev_id=1;event(f,WMI_ROAM_EVENTID,WMI_TAG_ROAM_EVENT,&roam,sizeof(roam));assert(f->sta.phase==GWT_RUN);
    roam.vdev_id=0;event(f,WMI_ROAM_EVENTID,WMI_TAG_ROAM_EVENT,&roam,sizeof(roam));assert(f->sta.phase==GWT_STOPPING && f->sta.error==GWT_TIMEOUT);
    gwt_quiesced(&f->sta);free(f);
    f=make(0);associated(f);struct wmi_peer_sta_kickout_event kick={0};memcpy(kick.peer_macaddr.addr,f->request.bss.bssid,6);kick.peer_macaddr.addr[5]^=1;
    event(f,WMI_PEER_STA_KICKOUT_EVENTID,WMI_TAG_PEER_STA_KICKOUT_EVENT,&kick,sizeof(kick));assert(f->sta.phase==GWT_RUN);
    kick.peer_macaddr.addr[5]^=1;event(f,WMI_PEER_STA_KICKOUT_EVENTID,WMI_TAG_PEER_STA_KICKOUT_EVENT,&kick,sizeof(kick));assert(f->sta.phase==GWT_STOPPING);
    gwt_quiesced(&f->sta);free(f);
}
int main(void) {
    stop_and_reconnect();keys();legacy_phy_modes();active_policy();ownership_and_regulatory();rejected_and_malformed();sae_and_loss_events();
    puts("Station: Linux v6.12 WMI wire oracle, ACTIVE power policy, actual HTC event routing, association/key completion, GTK rekey, DMA leases, teardown ordering and regulatory revocation passed");
    return 0;
}
