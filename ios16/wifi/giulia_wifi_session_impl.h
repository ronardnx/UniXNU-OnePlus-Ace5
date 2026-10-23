/* Worker-only connection scheduling. A live profile is a volatile session
 * credential, never a disk store. Explicit leave/power-off erases it. */
static void gn_forget_profile(giulia_wifi_runtime *r) {
    gwn_clear(&r->profile,sizeof(r->profile));r->profile_valid=r->profile_connected=0;
    r->retry_count=0;r->retry_at=r->next_roam_scan=0;
}
static const struct gww_bss *gn_choose(giulia_wifi_runtime *r,const struct gwn_join *join) {
    if(r->wmi.bss_count>GWW_BSS_MAX)return NULL;
    const struct gww_bss *peer=NULL;uint64_t now=gw_now(r);
    for(unsigned i=0;i<r->wmi.bss_count;i++) {
        const struct gww_bss *b=&r->wmi.bss[i];struct gwt_channel channel;
        uint8_t rates[16],count=0;
        if(!gwn_matches(b,join) || now<b->timestamp || now-b->timestamp>30000 ||
           gwt_channel_allowed(&r->wmi,b->frequency,&channel))continue;
        struct gwt_request candidate;
        if(!gwn_station_request(&candidate,join,b) || !gwt_request_valid(&candidate,rates,&count))continue;
        if(!peer || b->rssi>peer->rssi)peer=b;
    }
    return peer;
}
static void gn_join_failed(giulia_wifi_runtime *r,unsigned result,unsigned status) {
    r->join_pending=0;r->join_deadline=0;r->net_result=result;r->net_status=status;r->assoc_notice=1;
    gwn_clear(&r->join,sizeof(r->join));
    if(r->profile_valid && r->profile_connected && r->retry_count<3) {
        r->retry_at=gw_now(r)+(UINT64_C(1000)<<(r->retry_count+1));
    } else gn_forget_profile(r);
}
static void gn_directed_scan(const struct gwn_join *join,struct gww_scan_request *q) {
    gwn_clear(q,sizeof(*q));q->active=1;q->ssid_count=1;
    q->ssids[0].length=(uint8_t)join->ssid_bytes;gwq_copy(q->ssids[0].bytes,join->ssid,join->ssid_bytes);
    if(join->has_bssid){q->bssid_count=1;gwq_copy(q->bssids[0],join->bssid,6);}
}
static int gn_begin_scan(giulia_wifi_runtime *r,unsigned kind,struct gww_scan_request *q) {
    q->connected=r->station.phase==GWT_RUN;q->home_frequency=q->connected?r->station.request.bss.frequency:0;
    int error=gww_scan_begin(&r->wmi,&r->wio,q);
    if(!error){r->scan_kind=kind;r->scan_wait=1;r->join_scan_id=r->wmi.scan_id;}
    return error;
}
static void gn_consider_roam(giulia_wifi_runtime *r) {
    if(!r->profile_valid || !r->profile_connected || r->profile.has_bssid || !r->dp.authorized || r->station.phase!=GWT_RUN || r->join_pending)return;
    for(unsigned i=0;i<r->wmi.bss_count && i<GWW_BSS_MAX;i++) {
        const struct gww_bss *b=&r->wmi.bss[i];
        if(b->frequency==r->station.request.bss.frequency && gwd_same(b->bssid,r->station.request.bss.bssid) &&
           b->timestamp<=gw_now(r) && gw_now(r)-b->timestamp<=30000) {
            r->station.request.bss.rssi=b->rssi;r->station.request.bss.timestamp=b->timestamp;
        }
    }
    struct gwn_join desired=r->profile;desired.has_bssid=0;
    const struct gww_bss *peer=gn_choose(r,&desired);
    if(peer && !gwd_same(peer->bssid,r->station.request.bss.bssid) &&
       r->station.request.bss.rssi<=-70 && peer->rssi>=r->station.request.bss.rssi+12) {
        desired.has_bssid=1;gwq_copy(desired.bssid,peer->bssid,6);
        if(!gwn_station_request(&r->join_request,&desired,peer)){gwn_clear(&desired,sizeof(desired));return;}
        gwn_clear(&r->join,sizeof(r->join));r->join=desired;r->join_pending=GWN_JOIN_LEAVE;
        r->join_automatic=1;
        r->join_deadline=gw_now(r)+45000;r->net_result=GWN_RESULT_PENDING;
    }
    gwn_clear(&desired,sizeof(desired));
}
static void gn_session_tick(giulia_wifi_runtime *r) {
    uint64_t now=gw_now(r);
    if(r->disconnect_pending) {
        /* Cancelling a queued/scanning join can leave the station IDLE, so
         * no station STOPPING notification will complete that request. End
         * its result now while still waiting for scan ownership to return. */
        if(r->join_pending && r->net_result==GWN_RESULT_PENDING) {
            r->net_result=GWN_RESULT_FAILURE;r->net_status=0;r->assoc_notice=1;
        }
        gn_forget_profile(r);r->join_pending=0;r->join_automatic=0;r->scan_pending=0;
        gwn_clear(&r->join,sizeof(r->join));gwr_clear(&r->rsn);r->rsn_started=0;
        gwn_clear(&r->tx_queue,sizeof(r->tx_queue));gwn_clear(&r->rx_queue,sizeof(r->rx_queue));
        gwn_clear(&r->eapol_queue,sizeof(r->eapol_queue));
        if(r->wmi.scanning){(void)gww_scan_cancel(&r->wmi,&r->wio);return;}
        unsigned reason=r->disconnect_pending;r->disconnect_pending=0;r->scan_wait=r->scan_kind=0;
        r->reclaim_pending=r->dp.queues!=0;(void)gwt_disconnect(&r->station,reason);
    }
    if(!r->powered)return;
    if(r->scan_wait && !r->wmi.scanning) {
        unsigned kind=r->scan_kind;r->scan_wait=r->scan_kind=0;
        if(r->wmi.scan_id!=r->join_scan_id || r->wmi.scan_status) {
            if(kind==GWN_SCAN_JOIN && r->join_pending==GWN_JOIN_SCAN)gn_join_failed(r,GWN_RESULT_TIMEOUT,r->wmi.scan_status);
        } else if(kind==GWN_SCAN_JOIN && r->join_pending==GWN_JOIN_SCAN) {
            const struct gww_bss *peer=gn_choose(r,&r->join);
            if(!peer)gn_join_failed(r,GWN_RESULT_TIMEOUT,0);
            else if(gwn_station_request(&r->join_request,&r->join,peer))r->join_pending=GWN_JOIN_LEAVE;
            else gn_join_failed(r,GWN_RESULT_FAILURE,GWT_UNSUPPORTED);
        } else if(kind==GWN_SCAN_ROAM || kind==GWN_SCAN_USER)gn_consider_roam(r);
    }
    /* Automatic reconnect retains the user's active profile, with bounded
     * retries. A new explicit request resets this budget. */
    if(!r->join_pending && r->profile_valid && r->profile_connected &&
       r->station.phase==GWT_IDLE && r->retry_count<3 && !r->wmi.scanning && !r->scan_pending && now>=r->retry_at) {
        r->join=r->profile;r->join_pending=GWN_JOIN_SELECT;r->join_automatic=1;
        r->join_deadline=now+45000;r->retry_count++;r->net_result=GWN_RESULT_PENDING;
    }
    if(r->join_pending && r->join_deadline && now>=r->join_deadline) {
        if(r->wmi.scanning)(void)gww_scan_cancel(&r->wmi,&r->wio);
        gn_join_failed(r,GWN_RESULT_TIMEOUT,0);
    }
    if(r->join_pending==GWN_JOIN_SELECT && !r->wmi.scanning && !r->scan_pending) {
        const struct gww_bss *peer=gn_choose(r,&r->join);
        if(peer){
            if(gwn_station_request(&r->join_request,&r->join,peer))r->join_pending=GWN_JOIN_LEAVE;
            else gn_join_failed(r,GWN_RESULT_FAILURE,GWT_UNSUPPORTED);
        }
        else {
            struct gww_scan_request scan;gn_directed_scan(&r->join,&scan);
            if(gn_begin_scan(r,GWN_SCAN_JOIN,&scan))gn_join_failed(r,GWN_RESULT_TIMEOUT,0);
            else r->join_pending=GWN_JOIN_SCAN;
        }
    }
    if(r->join_pending==GWN_JOIN_LEAVE && r->station.phase!=GWT_IDLE) {
        if(r->wmi.scanning){(void)gww_scan_cancel(&r->wmi,&r->wio);return;}
        if(r->station.phase==GWT_FAILED){gn_join_failed(r,GWN_RESULT_FAILURE,r->station.error);return;}
        gwr_clear(&r->rsn);r->rsn_started=0;r->reclaim_pending=r->dp.queues!=0;
        gwn_clear(&r->tx_queue,sizeof(r->tx_queue));gwn_clear(&r->eapol_queue,sizeof(r->eapol_queue));
        (void)gwt_disconnect(&r->station,3);
    }
    if(r->station.phase==GWT_IDLE && !r->station.peer_created && !r->station.peer_mapped &&
       !r->station.tx_pending && !r->wmi.scanning && (r->reclaim_pending || r->dp.queues || r->join_pending==GWN_JOIN_LEAVE)) {
        int error=gwd_reo_reclaim(&r->dp,&r->dio,1);
        if(error==GWD_NOT_READY){r->reclaim_pending=1;return;}
        if(error){gn_join_failed(r,GWN_RESULT_FAILURE,(unsigned)error);return;}
        r->reclaim_pending=0;
    }
    if(r->join_pending==GWN_JOIN_LEAVE && r->station.phase==GWT_IDLE && !r->wmi.scanning && !r->reclaim_pending) {
        r->net_result=GWN_RESULT_PENDING;r->net_status=0;
        int error=gwd_select_peer(&r->dp,0,r->wmi.pmac,r->wmi.pdev,r->join_request.bss.bssid,r->join_request.security!=GWT_OPEN);
        if(error==GWD_NOT_READY || (error==GWD_ARGUMENT && r->dp.tx_pending))return;
        r->join_pending=0;
        if(!error){
            if(!r->join_automatic){
                gwn_clear(&r->profile,sizeof(r->profile));r->profile=r->join;r->profile_valid=1;r->profile_connected=0;
            }
            error=gns_prepare(r);if(!error)error=gwt_connect(&r->station,&r->join_request);
        }
        if(!r->join_request.security)gwn_clear(&r->join,sizeof(r->join));
        if(error){gwr_clear(&r->rsn);r->rsn_started=0;gn_join_failed(r,GWN_RESULT_FAILURE,(unsigned)error);}
    }
    if(!r->join_pending && !r->wmi.scanning && (r->station.phase==GWT_IDLE || r->station.phase==GWT_RUN)) {
        if(r->scan_pending) {
            r->scan_pending=0;int error=gn_begin_scan(r,GWN_SCAN_USER,&r->scan_request);
            if(error && r->wio.scan_done)r->wio.scan_done(r->wio.context,(unsigned)error,r->wmi.scan_id);
        } else if(r->profile_valid && r->profile_connected && !r->profile.has_bssid && r->station.phase==GWT_RUN && now>=r->next_roam_scan) {
            struct gww_scan_request scan;struct gwn_join desired=r->profile;desired.has_bssid=0;
            gn_directed_scan(&desired,&scan);gwn_clear(&desired,sizeof(desired));r->next_roam_scan=now+30000;
            (void)gn_begin_scan(r,GWN_SCAN_ROAM,&scan);
        }
    }
}
