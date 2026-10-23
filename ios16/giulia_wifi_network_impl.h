/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Runtime bridge, included after the serialized MHI/CE/WMI and DMA helpers.
 * BSD never accesses device registers. RX and EAPOL are deferred until the
 * current ring entry has been retired, avoiding recursive polling of a ring.
 */
static void gd_sync(void *v,size_t offset,size_t bytes,int) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    if(!bytes || bytes>GWD_BYTES || offset>GWD_BYTES-bytes ||
       !gwmap_dma_at(&r->dma,r->dp_offset+offset,bytes,NULL,NULL,NULL)){r->mapping_error=1;return;}
    __asm__ volatile("dsb sy" ::: "memory");
}
static int gd_receive(void *v,const void *data,size_t bytes) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;const uint8_t *p=(const uint8_t *)data;
    if(!p || bytes<14 || bytes>GWN_FRAME_BYTES || !r->powered || r->disconnect_pending ||
       __atomic_load_n(&r->stopping,__ATOMIC_ACQUIRE))return 1;
    if(p[12]==0x88 && p[13]==0x8e) {
        if(!r->station.request.security || !r->station.peer_mapped || !gwd_same(p,r->mac) || !gwd_same(p+6,r->station.request.bss.bssid))return 1;
        if(gwn_enqueue(&r->eapol_queue,p,bytes))return 0;
    } else if(r->dp.authorized && r->interface && ((p[0]&1) || gwd_same(p,r->mac))) {
        if(gwn_enqueue(&r->rx_queue,p,bytes))return 0;
    }
    r->net_rx_drops++;return 1;
}
static void gd_complete(void *v,uint32_t,unsigned status) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;if(status)r->net_tx_errors++;
}
static void gd_peer(void *v,unsigned vdev,const uint8_t *mac,unsigned peer,int mapped) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    gwt_peer_map(&r->station,vdev,mac,peer,(unsigned)mapped);
}
static int gd_connect(void *v) {giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;return gwd_connect(&r->dp,&r->dio);}
static int gd_control(void *v,const uint8_t *p,size_t bytes) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;return gwd_htc_control(&r->dp,p,bytes);
}
static int gd_htt(void *v,unsigned endpoint,const uint8_t *p,size_t bytes) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;return gwd_receive_htt(&r->dp,&r->dio,endpoint,p,bytes);
}
static int gt_ready(void *v,unsigned encrypted) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    return encrypted>1 || !r->dp.active || r->dp.error || r->mapping_error ||
        __atomic_load_n(&r->stopping,__ATOMIC_ACQUIRE);
}
static int gt_channel(void *v,unsigned frequency,struct gwt_channel *out) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;return gwt_channel_allowed(&r->wmi,frequency,out);
}
static int gt_hold(void *v,const uint8_t *frame,size_t bytes,uint32_t cookie,uint64_t *iova) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    if(!iova || !frame || !bytes || bytes>GWD_MGMT_BYTES || !cookie || r->mgmt_cookie || !r->dp.active)return 1;
    gwq_copy(r->dio.memory+GWD_MGMT,frame,bytes);gd_sync(r,GWD_MGMT,bytes,1);
    if(r->mapping_error)return 1;
    *iova=r->dio.dma+GWD_MGMT;r->mgmt_cookie=cookie;return 0;
}
static void gt_done(void *v,uint32_t cookie,unsigned) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    if(r->mgmt_cookie==cookie)r->mgmt_cookie=0;
}
static int gt_queue(void *v,unsigned tid,uint64_t *iova) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;return gwd_reo_queue(&r->dp,&r->dio,tid,iova);
}
static int gt_key(void *v,unsigned peer,unsigned index,unsigned pairwise,uint64_t rsc) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    if(!r->dp.peer_mapped || peer!=r->dp.peer_id)return 1;
    return gwd_key_ready(&r->dp,index,(int)pairwise,rsc);
}
static int gt_port(void *v,unsigned peer,unsigned open) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    if(open && (!r->dp.peer_mapped || peer!=r->dp.peer_id))return 1;
    unsigned before=r->dp.authorized;int error=gwd_authorize(&r->dp,(int)open);
    if(!open)gwn_clear(&r->rx_queue,sizeof(r->rx_queue));
    if(!error && before!=r->dp.authorized)r->link_pending=1;
    return error;
}
static void gt_notify(void *v,unsigned phase,unsigned error) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    IOLog("GIULIA WIFI NETWORK: phase=%u error=%u status=%u peer_mapped=%u authorized=%u\n",
        phase,error,r->station.status,r->station.peer_mapped,r->station.authorized);
    /* RUN can recur for the current AP during GTK rekey while a replacement
     * request is still selecting/scanning/leaving. Only the connection which
     * has actually entered the station state machine can complete its join. */
    if(phase==GWT_RUN && !r->join_pending){r->net_result=GWN_RESULT_SUCCESS;r->net_status=0;r->assoc_notice=1;
        r->profile_connected=r->profile_valid;r->retry_count=0;r->retry_at=0;r->next_roam_scan=gw_now(r)+30000;}
    else if(phase==GWT_STOPPING || phase==GWT_FAILED) {
        if(!r->join_pending){
            if(r->net_result!=GWN_RESULT_RSN_TIMEOUT)r->net_result=error==GWT_TIMEOUT?GWN_RESULT_TIMEOUT:GWN_RESULT_FAILURE;
            r->net_status=r->station.status;r->assoc_notice=1;
        }
        r->reclaim_pending=r->dp.queues!=0;
        if(r->profile_connected)r->retry_at=gw_now(r)+2000;
        r->link_pending=1;
    }
}
#include "giulia_wifi_security_impl.h"
static int gr_random(void *,uint8_t out[32]) {read_random(out,32);return 0;}
static int gr_send(void *v,const uint8_t *eapol,size_t bytes) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    if(!r || !eapol || bytes<4 || bytes>1024)return 1;
    uint8_t frame[1038];gwq_copy(frame,r->station.request.bss.bssid,6);gwq_copy(frame+6,r->mac,6);
    frame[12]=0x88;frame[13]=0x8e;gwq_copy(frame+14,eapol,bytes);
    uint64_t started=gw_now(r);uint32_t token=++r->net_token;int error=GWD_NOT_READY;
    /* RSN runs after RX dispatch has retired its descriptor. Pump completions
     * here when ordinary traffic temporarily owns every TCL slot; successful
     * return still means the complete EAPOL frame was actually submitted. */
    for(unsigned waits=0;;waits++) {
        if(__atomic_load_n(&r->stopping,__ATOMIC_ACQUIRE) || r->disconnect_pending ||
           !r->ready || !r->powered || r->error || r->mapping_error || r->qmi.error ||
           r->wmi.error || r->station.error ||
           (r->station.phase!=GWT_KEY_WAIT && r->station.phase!=GWT_RUN)) {
            error=GWD_NOT_READY;break;
        }
        if(waits && (waits>=1000 || gw_now(r)-started>=1000)){error=GWD_TIMEOUT;break;}
        error=gwd_send(&r->dp,&r->dio,frame,bytes+14,token);if(error!=GWD_FULL)break;
        if(gw_pump(r,1000)){error=GWD_TRANSPORT;break;}
    }
    gwn_clear(frame,sizeof(frame));return error;
}
static int gr_install(void *v,unsigned index,unsigned pairwise,uint64_t rsc,const uint8_t key[16]) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;struct gwt_key k;
    k.index=index;k.pairwise=pairwise;k.rsc=rsc;gwq_copy(k.data,key,16);
    int error=gwt_install_key(&r->station,&k);
    if(!error && pairwise && r->station.request.pmf){gwq_copy(r->security.ptk,key,16);r->security.pending_ptk=1;}
    gwn_clear(&k,sizeof(k));return error;
}
static int gr_key_ready(void *v,unsigned index,unsigned pairwise) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    if(r->station.error || r->station.phase==GWT_FAILED || r->station.phase==GWT_STOPPING)return -1;
    if(r->station.key_pending)return 0;
    if(pairwise && (r->station.installed&1) && r->security.pending_ptk) {
        int error=gpm_install_pairwise(&r->security.pmf,r->security.ptk);
        gwn_clear(r->security.ptk,sizeof(r->security.ptk));r->security.pending_ptk=0;if(error)return -1;
    }
    return (r->station.installed&(1u<<(pairwise?0:index+1)))?1:0;
}
static int gr_authorize(void *v,unsigned open) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    return open?gwt_authorize(&r->station):gt_port(r,r->station.peer_id,0);
}
static int gn_prepare(giulia_wifi_runtime *r) {
    uintptr_t cpu=0;uint64_t physical=0,iova=0;
    if(!gwmap_dma_allocate(&r->dma,GWD_BYTES,0x4000,&cpu,&physical,&iova))return GWD_DMA;
    r->dp_offset=(size_t)(iova-GW_IOVA);
    r->dio=(struct gwd_io){r,gc_read,gc_write,gm_ready,gd_sync,gw_send,gw_pump,gw_now,gs_idle,
        gd_receive,gd_complete,gd_peer,(uint8_t *)cpu,iova};
    struct gwt_ops ops={r,gt_ready,gt_channel,gt_hold,gt_done,gt_queue,gt_key,gt_port,gt_notify};
    int error=gwt_init(&r->station,&r->wmi,&r->wio,&ops);if(error)return error;
    struct gwt_security_ops security={r,gns_auth_start,gns_auth_rx,gns_mgmt_rx,gns_mgmt_tx,gns_protected_ready};
    error=gwt_set_security_ops(&r->station,&security);if(error)return error;
    r->wmi.service_context=r;r->wmi.connect_services=gd_connect;r->wmi.htc_control=gd_control;r->wmi.htc_data=gd_htt;
    return gwd_prepare(&r->dp,&r->dio);
}
static int gn_start(giulia_wifi_runtime *r) {
    if(!r->wmi.hw_valid || !r->wmi.regulatory.valid)return GWD_NOT_READY;
    int error=gwd_start(&r->dp,&r->dio);if(error)return error;
    r->network_available=1;r->net_result=GWN_RESULT_PENDING;
    IOLog("GIULIA WIFI NETWORK: datapath prepared; association and keys await requests\n");return 0;
}
static void gn_disable(giulia_wifi_runtime *r) {
    r->network_available=0;r->join_pending=r->disconnect_pending=0;
    r->dp.authorized=0;r->rsn_started=0;r->link_pending=1;
    gwr_clear(&r->rsn);gwn_clear(&r->join,sizeof(r->join));
    gns_clear(r);
    gwn_clear(&r->profile,sizeof(r->profile));r->profile_valid=r->profile_connected=0;
    r->scan_pending=r->scan_wait=r->scan_kind=r->reclaim_pending=r->join_automatic=0;r->retry_count=0;
    r->retry_at=r->next_roam_scan=r->join_deadline=0;
    gwn_clear(&r->tx_queue,sizeof(r->tx_queue));gwn_clear(&r->rx_queue,sizeof(r->rx_queue));
    gwn_clear(&r->eapol_queue,sizeof(r->eapol_queue));
}
static int gn_begin_rsn(giulia_wifi_runtime *r) {
    uint8_t association[192];size_t association_bytes=gwt_assoc_frame(&r->station,association);
    const uint8_t *assoc_ie=NULL;size_t assoc_bytes=0;
    for(size_t at=28;at<association_bytes;) {
        size_t bytes=2+association[at+1];if(association[at]==48){assoc_ie=association+at;assoc_bytes=bytes;}at+=bytes;
    }
    const struct gww_bss *b=&r->join_request.bss;const uint8_t *ap_ie=NULL;size_t ap_bytes=0;
    for(size_t at=0;at<b->ie_bytes;) {
        if(b->ie_bytes-at<2 || b->ie[at+1]>b->ie_bytes-at-2)return 1;
        size_t n=2+b->ie[at+1];if(b->ie[at]==48){if(ap_ie)return 1;ap_ie=b->ie+at;ap_bytes=n;}at+=n;
    }
    if(!r->join.has_pmk || !ap_ie || !assoc_ie)return 1;
    struct gwr_ops ops={r,gr_random,gwr_crypto_hmac_sha1,gwr_crypto_unwrap,gr_send,gr_install,gr_key_ready,gr_authorize,gw_now};
    struct gwr_security security={};security.sae=r->station.request.security==GWT_WPA3_SAE;security.pmf=r->station.request.pmf;security.h2e=r->station.request.h2e;
    security.hmac_sha256=gsa_crypto_hmac;security.cmac=gpm_crypto_cmac;security.install_igtk=gns_igtk;
    int error=gwr_init_secure(&r->rsn,&ops,&security,r->join.pmk,r->mac,b->bssid,assoc_ie,assoc_bytes,ap_ie,ap_bytes);
    gwn_clear(association,sizeof(association));
    gwn_clear(&r->join,sizeof(r->join));if(!error)r->rsn_started=1;return error;
}
#include "giulia_wifi_session_impl.h"
static void gn_work(giulia_wifi_runtime *r) {
    if(!r->network_available)return;
    gn_session_tick(r);
    gns_tick(r);
    gwt_tick(&r->station);
    if(r->station.phase==GWT_KEY_WAIT && !r->rsn_started && r->join.has_pmk) {
        if(gn_begin_rsn(r)){r->net_result=GWN_RESULT_RSN_TIMEOUT;(void)gwt_disconnect(&r->station,15);}
    }
    /* An AP can send M1 before the WMI association confirmation. Preserve it
     * until the worker has initialized RSN, outside the device RX dispatch. */
    for(unsigned i=0;i<8 && r->eapol_queue.count && r->rsn_started &&
        (r->station.phase==GWT_KEY_WAIT || r->station.phase==GWT_RUN);i++) {
        const struct gwn_packet *p=gwn_front(&r->eapol_queue);
        (void)gwr_receive(&r->rsn,p->data+6,p->data+14,p->bytes-14);
        gwn_pop(&r->eapol_queue);
    }
    if(r->rsn_started && (r->station.phase==GWT_KEY_WAIT || r->station.phase==GWT_RUN)) {
        gwr_tick(&r->rsn);
        if(r->rsn.phase==GWR_FAILED){r->net_result=GWN_RESULT_RSN_TIMEOUT;(void)gwt_disconnect(&r->station,15);}
    }
    if(r->station.phase==GWT_STOPPING || r->station.phase==GWT_FAILED || r->station.phase==GWT_IDLE) {
        if(!r->join_pending){gwr_clear(&r->rsn);gwn_clear(&r->join,sizeof(r->join));r->rsn_started=0;}
        if(!r->join_pending && (!r->profile_connected || r->retry_count>=3 || r->station.phase==GWT_FAILED))gn_forget_profile(r);
        gwn_clear(&r->tx_queue,sizeof(r->tx_queue));
        gwn_clear(&r->rx_queue,sizeof(r->rx_queue));gwn_clear(&r->eapol_queue,sizeof(r->eapol_queue));
    }
    for(unsigned i=0;i<16 && r->tx_queue.count && r->dp.authorized;i++) {
        const struct gwn_packet *p=gwn_front(&r->tx_queue);
        int error=gwd_send(&r->dp,&r->dio,p->data,p->bytes,++r->net_token);
        if(error==GWD_FULL)break;if(error)r->net_tx_errors++;gwn_pop(&r->tx_queue);
        if(error)break;
    }
}
static void gn_event(unsigned code,const void *body,size_t bytes) {
    uint8_t data[16+152]={0};if(bytes>152 || (bytes && !body))return;
    gwq_copy(data,"en0",4);if(bytes)gwq_copy(data+16,body,bytes);
    struct kev_msg event;bzero(&event,sizeof(event));event.vendor_code=1;event.kev_class=6;event.kev_subclass=1;event.event_code=code;
    event.dv[0].data_ptr=data;event.dv[0].data_length=16+bytes;(void)kev_post_msg(&event);
}
/* Called without the runtime lock: the network stack may synchronously
 * call output/ioctl back into this driver while consuming an input packet. */
static void gn_deliver(giulia_wifi_runtime *r) {
    unsigned link=0,notice=0,up=0,result=0,security=0,reason=0,scan=0,scan_status=0;
    int rssi=0,noise=0;uint8_t bssid[6];
    IOLockLock(r->lock);link=r->link_pending;r->link_pending=0;notice=r->assoc_notice;r->assoc_notice=0;
    up=r->dp.authorized;result=r->net_result;security=r->station.request.security;reason=r->station.reason;
    rssi=r->station.request.bss.rssi;noise=r->station.request.bss.noise;gwq_copy(bssid,r->station.request.bss.bssid,6);
    scan=r->scan_notice_pending;scan_status=r->scan_notice_status;r->scan_notice_pending=0;IOLockUnlock(r->lock);
    if(scan)gn_event(10,&scan_status,sizeof(scan_status));
    if(link && r->interface) {
        (void)ifnet_set_flags(r->interface,up?IFF_RUNNING:0,IFF_RUNNING);
        struct net_event_data data;bzero(&data,sizeof(data));data.if_family=IFNET_FAMILY_ETHERNET;
        data.if_unit=0;strlcpy(data.if_name,"en",sizeof(data.if_name));
        struct kev_msg event;bzero(&event,sizeof(event));event.vendor_code=KEV_VENDOR_APPLE;
        event.kev_class=KEV_NETWORK_CLASS;event.kev_subclass=KEV_DL_SUBCLASS;event.event_code=up?KEV_DL_LINK_ON:KEV_DL_LINK_OFF;
        event.dv[0].data_ptr=&data;event.dv[0].data_length=sizeof(data);(void)kev_post_msg(&event);
        /* 20H392 __dispatchLinkChangedEvent requires 32 bytes; byte 0 is
         * isLinkDown, not link-up. A four-byte status is silently ignored. */
        uint8_t body[32]={0};body[0]=up?0:1;
        gwq_w32(body+4,(uint32_t)(int32_t)rssi);
        gwq_w16(body+10,(uint16_t)noise);gwq_w32(body+20,reason);
        gn_event(4,body,sizeof(body));
    }
    if(notice) {
        /* 20H392 __dispatchAsyncAssocEvent reads three 44-byte optional
         * attempt records and +0x94 before querying ASSOCIATE_RESULT. Supply
         * a complete zeroed history, with only the actual peer populated. */
        uint8_t body[152]={0};gwq_copy(body+16,bssid,6);
        gn_event(9,body,sizeof(body));
        if(result==GWN_RESULT_SUCCESS && security){uint32_t status=0;gn_event(20,&status,sizeof(status));}
    }
    for(unsigned budget=0;budget<32;budget++) {
        struct gwn_packet packet;
        IOLockLock(r->lock);const struct gwn_packet *p=gwn_front(&r->rx_queue);
        if(!p){IOLockUnlock(r->lock);break;}
        if(!r->powered || !r->dp.authorized || r->disconnect_pending){gwn_clear(&r->rx_queue,sizeof(r->rx_queue));IOLockUnlock(r->lock);break;}
        packet=*p;gwn_pop(&r->rx_queue);IOLockUnlock(r->lock);
        if(!r->interface)continue;
        mbuf_t m=NULL;unsigned chunks=1;
        if(mbuf_allocpacket(MBUF_DONTWAIT,packet.bytes,&chunks,&m))continue;
        if(mbuf_copyback(m,0,packet.bytes,packet.data,MBUF_DONTWAIT)){mbuf_freem(m);continue;}
        (void)mbuf_pkthdr_setrcvif(m,r->interface);mbuf_pkthdr_setheader(m,mbuf_data(m));
        if(packet.data[0]==255 && packet.data[1]==255 && packet.data[2]==255 &&
           packet.data[3]==255 && packet.data[4]==255 && packet.data[5]==255)(void)mbuf_setflags_mask(m,MBUF_BCAST,MBUF_BCAST);
        else if(packet.data[0]&1)(void)mbuf_setflags_mask(m,MBUF_MCAST,MBUF_MCAST);
        mbuf_adj(m,14);(void)ifnet_input(r->interface,m,NULL);
    }
}
static int gn_query(giulia_wifi_runtime *r,const struct gwi_request &q,void *output,size_t *bytes,int *handled) {
    *handled=0;
    if(!r->network_available)return GWI_UNSUPPORTED;
    if(q.set && (q.type==GWN_ASSOCIATE || q.type==GWN_DISASSOCIATE)) {
        *handled=1;size_t capacity=*bytes;*bytes=0;
        if((capacity && !output) || capacity>GWI_SET_CAPACITY || (capacity && capacity<q.bytes))return GWI_ARGUMENT;
        if(!r->ready || !r->powered)return GWI_NOT_READY;
        if(q.type==GWN_ASSOCIATE) {
            if(r->join_pending || r->disconnect_pending ||
               (r->station.phase!=GWT_IDLE && r->station.phase!=GWT_RUN) || r->scan_pending || r->wmi.scanning)return GWI_BUSY;
            struct gwn_join join;int error=gwn_join_decode(&q,&join);if(error)return error;
            if(join.rsn_bytes) {
                unsigned sae=join.security==GWT_WPA3_SAE;
                int valid=gwr_rsn_valid_secure(join.rsn,join.rsn_bytes,1,sae,2);
                if(!sae)valid|=gwr_rsn_valid_secure(join.rsn,join.rsn_bytes,1,0,0) || gwr_rsn_valid_secure(join.rsn,join.rsn_bytes,1,0,1);
                if(!valid){gwn_clear(&join,sizeof(join));return GWI_UNSUPPORTED;}
            }
            if(r->wmi.bss_count>GWW_BSS_MAX){gwn_clear(&join,sizeof(join));return GWI_ARGUMENT;}
            const struct gww_bss *peer=gn_choose(r,&join);
            r->join=join;r->join_automatic=0;r->retry_count=0;r->retry_at=0;
            r->join_pending=GWN_JOIN_SELECT;r->join_deadline=gw_now(r)+45000;r->net_result=GWN_RESULT_PENDING;r->net_status=0;
            if(peer)(void)gwn_station_request(&r->join_request,&join,peer);
            gwn_clear(&join,sizeof(join));
        } else {
            if(q.bytes || q.value>65535)return GWI_ARGUMENT;
            r->disconnect_pending=q.value?q.value:3;
            gn_forget_profile(r);
            (void)gt_port(r,r->dp.peer_id,0);gwn_clear(&r->tx_queue,sizeof(r->tx_queue));
        }
        if(capacity && q.bytes){gwq_copy(output,q.data,q.bytes);*bytes=q.bytes;}return GWI_OK;
    }
    if(q.set && q.type==GWI_SCAN_REQUEST) {
        *handled=1;size_t capacity=*bytes;*bytes=0;
        if((capacity && !output) || capacity>GWI_SET_CAPACITY || (capacity && capacity<q.bytes))return GWI_ARGUMENT;
        if(!r->ready || !r->powered)return GWI_NOT_READY;
        if(r->join_pending || r->disconnect_pending || r->scan_pending || r->wmi.scanning ||
           (r->station.phase!=GWT_IDLE && r->station.phase!=GWT_RUN))return GWI_BUSY;
        struct gww_scan_request scan;int error=gwn_scan_decode(&q,&scan);if(error)return error;
        r->scan_request=scan;r->scan_pending=1;
        if(capacity){gwq_copy(output,q.data,q.bytes);*bytes=q.bytes;}return GWI_OK;
    }
    unsigned phase=r->join_pending?2:r->station.phase==GWT_AUTH_WAIT?2:
        r->station.phase>=GWT_ASSOC_WAIT && r->station.phase<=GWT_KEY_WAIT?3:r->station.phase==GWT_RUN?4:0;
    if((r->scan_pending || r->wmi.scanning) && r->station.phase!=GWT_RUN)phase=1;
    if(!r->powered || r->disconnect_pending)phase=0;
    struct gwn_view view={r->network_available,phase,r->powered && !r->disconnect_pending && r->station.vdev_up,r->dp.authorized,r->station.request.security,
        r->net_result,r->net_status,&r->station.request.bss};
    return gwn_get(&view,&q,output,bytes,handled);
}
