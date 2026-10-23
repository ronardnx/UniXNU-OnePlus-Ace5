/* Included by the network bridge before station callbacks are registered. */
static int gns_random(void *,uint8_t *out,size_t bytes) {
    if(!out || !bytes || bytes>64)return 1;read_random(out,(unsigned)bytes);return 0;
}
static void gns_clear(giulia_wifi_runtime *r) {gwn_clear(&r->security,sizeof(r->security));}
static int gns_enqueue(giulia_wifi_runtime *r,unsigned kind,const uint8_t *p,size_t bytes,unsigned status,unsigned frequency) {
    struct gns_state *s=&r->security;
    if(!p || !bytes || bytes>GWT_MGMT_MAX || s->count>=GNS_QUEUE_COUNT ||
       !r->powered || r->disconnect_pending || __atomic_load_n(&r->stopping,__ATOMIC_ACQUIRE))return 1;
    struct gns_packet *q=&s->packets[s->head];q->kind=kind;q->status=status;q->frequency=frequency;
    q->bytes=bytes;gwq_copy(q->data,p,bytes);s->head=(s->head+1)%GNS_QUEUE_COUNT;s->count++;return 0;
}
static int gns_auth_send(void *v,const uint8_t *p,size_t bytes) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    if(r->station.tx_pending || r->mgmt_cookie)return 1;
    int error=gwt_auth_send(&r->station,p,bytes);return error==GWT_BUSY?1:error?-1:0;
}
static int gns_auth_start(void *v) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    struct gsa_ops ops={r,gns_random,gns_auth_send,gw_now};
    int error=gsa_init(&r->security.sae,&ops,r->mac,r->station.request.bss.bssid,
        r->join.password,r->join.password_bytes,r->join.ssid,r->join.ssid_bytes,r->station.request.h2e);
    gwn_clear(r->join.password,sizeof(r->join.password));r->join.password_bytes=0;
    return error?error:gsa_start(&r->security.sae);
}
static void gns_auth_rx(void *v,const uint8_t *p,size_t bytes) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;
    if(bytes>=6 && bytes<=GSA_AUTH_MAX)(void)gns_enqueue(r,1,p,bytes,0,r->station.request.bss.frequency);
}
static int gns_mgmt_rx(void *v,const uint8_t *p,size_t bytes,unsigned status,unsigned frequency,uint8_t *,size_t *used) {
    giulia_wifi_runtime *r=(giulia_wifi_runtime *)v;if(used)*used=0;
    if(frequency==r->station.request.bss.frequency && bytes>=24)(void)gns_enqueue(r,0,p,bytes,status,frequency);
    return 1; /* Deferred frames cannot also reach the unprotected parser. */
}
static int gns_mgmt_tx(void *v,const uint8_t *p,size_t bytes,uint8_t *out,size_t *used) {
    return gpm_protect(&((giulia_wifi_runtime *)v)->security.pmf,p,bytes,out,used);
}
static int gns_protected_ready(void *v) {return !gpm_ready(&((giulia_wifi_runtime *)v)->security.pmf);}
static int gns_igtk(void *v,unsigned index,uint64_t ipn,const uint8_t key[16]) {
    return gpm_install_igtk(&((giulia_wifi_runtime *)v)->security.pmf,index,ipn,key);
}
static int gns_prepare(giulia_wifi_runtime *r) {
    gns_clear(r);if(!r->join_request.pmf)return 0;
    struct gpm_ops ops={r,gpm_crypto_cmac,gns_random,gw_now};
    return gpm_init(&r->security.pmf,&ops,r->mac,r->join_request.bss.bssid);
}
static void gns_tick(giulia_wifi_runtime *r) {
    struct gns_state *s=&r->security;
    if(r->station.phase==GWT_IDLE || r->station.phase==GWT_FAILED){gns_clear(r);return;}
    if(r->station.phase==GWT_STOPPING || r->disconnect_pending || !r->powered) {
        gsa_clear(&s->sae);gwn_clear(s->packets,sizeof(s->packets));s->head=s->tail=s->count=0;
        gwn_clear(s->ptk,sizeof(s->ptk));s->pending_ptk=0;return;
    }
    for(unsigned budget=0;budget<GNS_QUEUE_COUNT*2 && s->count;budget++) {
        struct gns_packet packet=s->packets[s->tail];gwn_clear(&s->packets[s->tail],sizeof(packet));
        s->tail=(s->tail+1)%GNS_QUEUE_COUNT;s->count--;
        if(packet.kind==1) {
            if(r->station.phase==GWT_AUTH_WAIT)(void)gsa_receive(&s->sae,r->station.request.bss.bssid,packet.data,packet.bytes);
        } else if(packet.frequency==r->station.request.bss.frequency) {
            uint8_t frame[GWT_MGMT_MAX];size_t bytes=sizeof(frame);
            if(!gpm_filter(&s->pmf,packet.data,packet.bytes,packet.status,frame,&bytes))gwt_mgmt(&r->station,frame,bytes,packet.frequency);
            gwn_clear(frame,sizeof(frame));
        }
        gwn_clear(&packet,sizeof(packet));
        if(r->station.phase==GWT_STOPPING)break;
    }
    if(r->station.phase==GWT_AUTH_WAIT && r->station.request.security==GWT_WPA3_SAE) {
        (void)gsa_tick(&s->sae);
        if(s->sae.phase==GSA_FAILED)gwt_abort(&r->station,s->sae.error==GSA_TIMEOUT?GWT_TIMEOUT:GWT_REJECTED,0);
        else if(s->sae.phase==GSA_ACCEPTED && !s->accepted) {
            gwq_copy(r->join.pmk,s->sae.pmk,32);r->join.has_pmk=1;s->accepted=1;
            gwq_copy(r->station.request.pmkid,s->sae.pmkid,16);r->station.request.has_pmkid=1;
            r->join_request=r->station.request;(void)gwt_auth_complete(&r->station,0);
        }
    } else if(s->sae.phase)gsa_clear(&s->sae);
    if(s->pmf.initialized && (r->station.phase==GWT_KEY_WAIT || r->station.phase==GWT_RUN)) {
        int error=gpm_tick(&s->pmf);if(error){gwt_abort(&r->station,GWT_TIMEOUT,0);return;}
        if(s->pmf.pending && !r->station.tx_pending && !r->mgmt_cookie) {
            uint8_t frame[28];size_t bytes=sizeof(frame);
            if(!gpm_next_tx(&s->pmf,frame,&bytes) && gwt_mgmt_send(&r->station,frame,bytes))gwt_abort(&r->station,GWT_TRANSPORT,0);
            gwn_clear(frame,sizeof(frame));
        }
    }
}
