/* Actual network bridge + station + RSN + TCL submission. Only CE/MMIO and
 * device-owned memory synchronization are replaced; AP MIC/wrap uses OpenSSL. */
#include <openssl/hmac.h>
static const uint8_t network_rsn_ie[22]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0x80,0};
struct NetworkRsnFixture;
static NetworkRsnFixture *networkRsnActive;
static int network_rsn_send(void *,unsigned,const void *,size_t);
static void network_rsn_sync(void *v,size_t offset,size_t bytes,int) {
    auto *r=(giulia_wifi_runtime *)v;
    assert(r->lock->locked && bytes && bytes<=GWD_BYTES && offset<=GWD_BYTES-bytes);
}
struct NetworkRsnFixture {
    Fixture f;FakeIfnet net{};std::vector<uint8_t> memory;
    unsigned installs=0,pumps=0,pumpMode=0;std::vector<uint8_t> lastCommand;
    NetworkRsnFixture():memory(GWD_BYTES+0x3fff) {
        assert(!networkRsnActive);networkRsnActive=this;network_setup(f,net);auto &r=f.runtime;
        auto &b=r.wmi.bss[0];b.capability|=0x10;b.ie_bytes=sizeof(network_rsn_ie);
        memcpy(b.ie,network_rsn_ie,sizeof(network_rsn_ie));memcpy(r.wmi.mac,r.mac,6);
        r.wmi.endpoint=1;r.wmi.credit_size=256;r.wmi.max_message=1024;
        r.wmi.credits=r.wmi.credit_total=255;r.wio.send=network_rsn_send;
        r.join_request={};r.join_request.bss=b;r.join_request.security=GWT_WPA2_CCMP;r.station.request=r.join_request;
        assert(!gwt_channel_allowed(&r.wmi,b.frequency,&r.station.channel));
        r.station.phase=GWT_ASSOC_CONFIRM;r.station.deadline=10000;
        r.station.peer_mapped=r.station.peer_created=1;r.station.peer_id=7;r.station.aid=1;
        r.join.has_pmk=1;for(unsigned i=0;i<32;i++)r.join.pmk[i]=(uint8_t)i;
        r.dp.active=r.dp.peer_mapped=r.dp.peer_selected=r.dp.encrypted=1;r.dp.peer_id=7;
        r.dp.queues=0x1ffff;memcpy(r.dp.bssid,b.bssid,6);gwd_layout(&r.dp);
        auto *aligned=(uint8_t *)(((uintptr_t)memory.data()+0x3fff)&~(uintptr_t)0x3fff);
        r.dio={&r,gc_read,gc_write,gm_ready,network_rsn_sync,network_rsn_send,gw_pump,gw_now,gs_idle,
            gd_receive,gd_complete,gd_peer,aligned,GW_IOVA};
        assert(gwd_valid(&r.dio));
    }
    ~NetworkRsnFixture(){networkPumpHook=nullptr;networkRsnActive=nullptr;}
    void work(){IOLockLock(&f.lock);gn_work(&f.runtime);IOLockUnlock(&f.lock);}
    void receive(const std::vector<uint8_t> &eapol) {
        auto &r=f.runtime;std::vector<uint8_t> frame(14+eapol.size());
        memcpy(frame.data(),r.mac,6);memcpy(frame.data()+6,r.station.request.bss.bssid,6);
        frame[12]=0x88;frame[13]=0x8e;memcpy(frame.data()+14,eapol.data(),eapol.size());
        IOLockLock(&f.lock);assert(!gd_receive(&r,frame.data(),frame.size()));IOLockUnlock(&f.lock);
    }
    void event(const uint8_t *p,size_t n) {
        auto &r=f.runtime;IOLockLock(&f.lock);assert(!gww_wmi_receive(&r.wmi,&r.wio,p,n));IOLockUnlock(&f.lock);
    }
    void association_ack() {
        uint8_t p[20]={0};gwq_w32(p,0x6006);gww_tlv(p+4,434,12);
        memcpy(p+12,f.runtime.station.request.bss.bssid,6);event(p,sizeof(p));
    }
    void key_ack(unsigned index,unsigned pairwise,unsigned status=0) {
        uint8_t p[32]={0};gwq_w32(p,0x5003);gww_tlv(p+4,42,24);
        memcpy(p+12,f.runtime.station.request.bss.bssid,6);gwq_w32(p+20,index);
        gwq_w32(p+24,pairwise?0:1);gwq_w32(p+28,status);event(p,sizeof(p));
    }
};
static int network_rsn_send(void *v,unsigned pipe,const void *data,size_t n) {
    assert(networkRsnActive && v==&networkRsnActive->f.runtime && pipe==3 && n>=12);
    auto &r=networkRsnActive->f.runtime;assert(r.lock->locked);
    const auto *p=(const uint8_t *)data;assert(p[0]==r.wmi.endpoint && gwq_u16(p+2)==n-8);
    networkRsnActive->lastCommand.assign(p+8,p+n);
    if(gwq_u32(p+8)==0x5009) {
        assert(n==136 && gwq_u32(p+96)==16);++networkRsnActive->installs;
    }
    return 0;
}
static void network_rsn_sign(const uint8_t kck[16],std::vector<uint8_t> &p) {
    uint8_t digest[20];unsigned bytes=sizeof(digest);memset(p.data()+81,0,16);
    assert(HMAC(EVP_sha1(),kck,16,p.data(),p.size(),digest,&bytes) && bytes==20);
    memcpy(p.data()+81,digest,16);
}
static std::vector<uint8_t> network_rsn_m1(uint64_t replay) {
    std::vector<uint8_t> p(99);p[0]=2;p[1]=3;p[4]=2;gwr_w16(p.data()+2,95);
    gwr_w16(p.data()+5,0x008a);gwr_w16(p.data()+7,16);gwr_w64(p.data()+9,replay);
    for(unsigned i=0;i<32;i++)p[17+i]=(uint8_t)(0x80+i);return p;
}
static std::vector<uint8_t> network_rsn_key(const gwr_state &s,uint64_t replay,unsigned pairwise,unsigned index,unsigned value) {
    uint8_t plain[64]={0};size_t bytes=0;
    if(pairwise){memcpy(plain,network_rsn_ie,sizeof(network_rsn_ie));bytes=sizeof(network_rsn_ie);}
    uint8_t *k=plain+bytes;k[0]=221;k[1]=22;k[3]=15;k[4]=172;k[5]=1;k[6]=(uint8_t)index;memset(k+8,value,16);bytes+=24;
    if(bytes&7){plain[bytes++]=221;while(bytes&7)plain[bytes++]=0;}
    auto p=network_rsn_m1(replay);p.resize(99+bytes+8);memcpy(p.data()+17,s.anonce,32);
    gwr_w16(p.data()+5,pairwise?0x13ca:0x1382);p[65]=9;
    AES_KEY aes;assert(!AES_set_encrypt_key(s.ptk+16,128,&aes));
    assert(AES_wrap_key(&aes,nullptr,p.data()+99,plain,(unsigned)bytes)==(int)bytes+8);
    gwr_w16(p.data()+97,(unsigned)bytes+8);gwr_w16(p.data()+2,(unsigned)p.size()-4);network_rsn_sign(s.ptk,p);return p;
}
static void network_rsn_reply(const gwr_state &s,unsigned info,uint64_t replay) {
    assert(s.last_tx_bytes>=99 && gwr_be16(s.last_tx+5)==info && gwr_be64(s.last_tx+9)==replay);
    std::vector<uint8_t> copy(s.last_tx,s.last_tx+s.last_tx_bytes);network_rsn_sign(s.ptk,copy);
    assert(!memcmp(copy.data()+81,s.last_tx+81,16));
}
static void network_rsn_start(NetworkRsnFixture &h) {
    auto &r=h.f.runtime;h.receive(network_rsn_m1(1));h.work();
    assert(r.station.phase==GWT_ASSOC_CONFIRM && !r.rsn_started && r.eapol_queue.count==1 && !r.dp.tx_pending);
    assert(r.join.has_pmk && r.join.pmk[31]==31);
    h.association_ack();h.work();
    assert(r.station.phase==GWT_KEY_WAIT && r.rsn_started && r.rsn.phase==GWR_WAIT_M3);
    assert(!r.eapol_queue.count && r.dp.tx_pending==1 && !r.dp.authorized);
    assert(!gwn_nonzero((const uint8_t *)&r.join,sizeof(r.join)) && r.rsn.pmk[31]==31);
    network_rsn_reply(r.rsn,0x010a,1);
    /* The M2 IE must equal the real association frame's selected IE. */
    uint8_t association[128];r.station.rate_count=1;r.station.rates[0]=0x82;
    size_t n=gwt_assoc_frame(&r.station,association);
    assert(r.rsn.assoc_bytes==22 && !memcmp(r.rsn.assoc_rsn,association+n-22,22));
    assert(r.rsn.ap_rsn[20]==0x80 && r.rsn.assoc_rsn[20]==0);
}
static void network_rsn_fill_tx(NetworkRsnFixture &h) {
    auto &r=h.f.runtime;uint8_t frame[64]={0};memcpy(frame,r.dp.bssid,6);memcpy(frame+6,r.mac,6);frame[12]=8;
    IOLockLock(&h.f.lock);unsigned sent=0;int error;
    while(!(error=gwd_send(&r.dp,&r.dio,frame,sizeof(frame),++r.net_token))){assert(++sent<GWD_TX_COUNT);}
    assert(error==GWD_FULL && r.dp.tx_pending==GWD_TX_COUNT-1);
    // RX has no pending events in this completion-only device fixture.
    memset(r.dp.rx_owned,1,sizeof(r.dp.rx_owned));IOLockUnlock(&h.f.lock);
}
static int network_rsn_pump(void *v,unsigned us) {
    auto *h=networkRsnActive;assert(h && v==&h->f.runtime && us==1000);
    auto &r=h->f.runtime;assert(r.lock->locked);++h->pumps;
    if(h->pumpMode==1)return 0; // A valid device with no completion yet.
    if(h->pumpMode==2)return 1;
    if(h->pumpMode==3){__atomic_store_n(&r.stopping,1u,__ATOMIC_RELEASE);return 0;}
    if(h->pumpMode==4) {
        uint8_t unmap[4];gwq_w32(unmap,4|(r.dp.peer_id<<16));
        assert(!gwd_receive_htt(&r.dp,&r.dio,r.dp.endpoint,unmap,sizeof(unmap)));return 0;
    }
    if(h->pumpMode==5){r.wmi.error=GWW_TRANSPORT;return 0;}
    if(h->pumps<3)return 0;
    assert(h->pumps==3);
    auto &tcl=r.dp.ring[GWD_TCL];auto &completion=r.dp.ring[GWD_TX_COMPLETE];
    const uint8_t *tx=r.dio.memory+tcl.desc+tcl.tail*32;uint32_t cookie=gwq_u32(tx+4)>>12;
    unsigned slot=cookie&(GWD_TX_COUNT-1);assert(r.dp.tx_owned[slot]);
    uint8_t *wbm=r.dio.memory+completion.desc+completion.tail*32;memset(wbm,0,32);
    gwd_address(wbm,r.dio.dma+GWD_TX_BUFFERS+slot*GWD_BUFFER,5,cookie);
    gwq_w32(r.dio.memory+tcl.remote,((tcl.tail+1)&(tcl.count-1))*tcl.entry);
    gwq_w32(r.dio.memory+completion.remote,((completion.tail+1)&(completion.count-1))*completion.entry);
    return gwd_poll(&r.dp,&r.dio,1); // Real ownership/cookie/completion processing.
}
static void network_rsn_backpressure() {
    for(unsigned mode=0;mode<6;mode++) {
        NetworkRsnFixture h;auto &r=h.f.runtime;network_rsn_start(h);
        h.receive(network_rsn_key(r.rsn,2,1,1,0x77));h.work();h.key_ack(0,1);h.work();h.work();
        h.key_ack(1,0);h.work();assert(r.dp.authorized && r.rsn.authorized);
        network_rsn_fill_tx(h);h.pumpMode=mode;networkPumpHook=network_rsn_pump;
        unsigned head=r.dp.ring[GWD_TCL].head,token=r.net_token;
        IOLockLock(&h.f.lock);int result=gr_send(&r,r.rsn.last_tx,r.rsn.last_tx_bytes);IOLockUnlock(&h.f.lock);
        assert(r.net_token==token+1); // Poll retries keep the same logical packet.
        if(!mode) {
            assert(!result && h.pumps==3 && r.dp.tx_packets==1 && r.dp.tx_pending==GWD_TX_COUNT-1);
            const uint8_t *d=r.dio.memory+r.dp.ring[GWD_TCL].desc+head*32;
            assert(((gwq_u32(d+8)>>2)&63)==1 && gwq_u32(d+16)==r.rsn.last_tx_bytes+32);
            size_t at=gwq_u32(d)-(uint32_t)r.dio.dma;assert(at<GWD_BYTES-r.rsn.last_tx_bytes-32);
            assert(gwq_u16(r.dio.memory+at)==0x0108 && !memcmp(r.dio.memory+at+32,r.rsn.last_tx,r.rsn.last_tx_bytes));
            assert(r.dp.authorized && r.rsn.authorized);
        } else {
            assert(result && !r.dp.tx_packets && r.dp.ring[GWD_TCL].head==head);
            if(mode==1)assert(result==GWD_TIMEOUT && h.pumps==1000);
            else assert(h.pumps==1);
            if(mode==2)assert(result==GWD_TRANSPORT);
            if(mode==4)assert(r.station.phase==GWT_STOPPING && !r.dp.authorized && !r.dp.peer_mapped);
        }
    }
    puts("Actual EAPOL backpressure: real WBM completion submits one plaintext M4; bounded timeout, pump failure, stop, peer-unmap and WMI error cannot report success");
}
static void network_rsn_bridge() {
    {
        NetworkRsnFixture h;auto &r=h.f.runtime;network_rsn_start(h);
        h.receive(network_rsn_key(r.rsn,2,1,1,0x77));h.work();
        assert(h.installs==1 && r.station.key_pending && r.rsn.phase==GWR_PTK_WAIT && !r.dp.authorized);
        assert(!gwn_nonzero(r.wmi.tx,sizeof(r.wmi.tx)) && r.dp.tx_pending==1);
        h.key_ack(1,1);h.work();assert(!r.station.key_confirm && h.installs==1 && r.rsn.phase==GWR_PTK_WAIT);
        h.key_ack(0,1);h.work();assert(r.dp.key_valid[4] && r.rsn.phase==GWR_GTK_WAIT && !r.dp.authorized);
        h.work();assert(h.installs==2 && r.station.key_pending && !r.dp.authorized && r.dp.tx_pending==1);
        h.key_ack(1,0);h.work();
        assert(r.rsn.phase==GWR_RUN && r.station.phase==GWT_RUN && r.dp.authorized && r.station.authorized);
        assert(r.dp.tx_pending==2 && r.dp.pn[1][16]==9 && r.net_result==GWN_RESULT_SUCCESS);network_rsn_reply(r.rsn,0x030a,2);
        h.receive(network_rsn_key(r.rsn,3,0,2,0x66));h.work();
        assert(h.installs==3 && !r.dp.authorized && !r.rsn.authorized && r.station.key_pending);
        h.receive(network_rsn_key(r.rsn,4,0,2,0x66));h.work();assert(h.installs==3 && r.rsn.replay==4);
        h.key_ack(2,0);h.work();assert(r.dp.authorized && r.station.authorized && r.rsn.authorized);
        network_rsn_reply(r.rsn,0x0302,4);assert(r.dp.tx_pending==3);
        /* Same PTK and GTK in a renewed exchange exercise station's no-op
         * install plus gr_authorize/gt_port reopening, not a fake RSN callback. */
        h.receive(network_rsn_m1(5));h.work();h.receive(network_rsn_key(r.rsn,6,1,1,0x77));h.work();
        assert(h.installs==3 && !r.dp.authorized && r.rsn.phase==GWR_PTK_WAIT);
        h.work();h.work();assert(h.installs==3 && r.dp.authorized && r.station.authorized && r.rsn.authorized);
        network_rsn_reply(r.rsn,0x030a,6);
        IOLockLock(&h.f.lock);gn_disable(&r);IOLockUnlock(&h.f.lock);
        assert(!r.dp.authorized && !r.rsn_started && !gwn_nonzero((uint8_t *)&r.rsn,sizeof(r.rsn)));
    }
    {
        NetworkRsnFixture h;auto &r=h.f.runtime;r.station.phase=GWT_KEY_WAIT;
        r.join_request.bss.ie[1]=255;h.work();
        assert(r.station.phase==GWT_STOPPING && !r.rsn_started && !r.dp.authorized);
        assert(!gwn_nonzero((uint8_t *)&r.join,sizeof(r.join)) && !r.eapol_queue.count);
    }
    network_rsn_backpressure();
    puts("Actual Wi-Fi RSN bridge: early M1, association IE binding, PMK clearing, PTK/GTK ACK gating, group rekey and no-op port recovery passed");
}
