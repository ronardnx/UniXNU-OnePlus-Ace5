static void network_test_sync(void *,size_t offset,size_t bytes,int) {assert(bytes && offset<=GWD_BYTES-bytes);}
static void network_setup(Fixture &f,FakeIfnet &net) {
    auto &r=f.runtime;net.softc=&r;r.interface=&net;r.network_available=1;r.mac[0]=2;r.mac[5]=1;
    r.wio={&r,gw_send,gw_pump,gw_now,nullptr};
    gwt_ops ops={&r,gt_ready,gt_channel,gt_hold,gt_done,gt_queue,gt_key,gt_port,gt_notify};
    assert(!gwt_init(&r.station,&r.wmi,&r.wio,&ops));
    r.wmi.regulatory.valid=1;r.wmi.regulatory.count=1;
    r.wmi.regulatory.min_bw[0]=20;r.wmi.regulatory.max_bw[0]=40;
    r.wmi.regulatory.rules[0]={2402,2482,40,20,0,0};
    r.wmi.bss_count=1;auto &b=r.wmi.bss[0];b.bssid[0]=2;b.bssid[5]=2;
    b.ssid_len=3;memcpy(b.ssid,"lab",3);b.frequency=2412;b.beacon=100;
    b.capability=1;b.rate_count=1;b.rates[0]=0x82;
}
static void network_queries() {
    Fixture f;FakeIfnet net{};network_setup(f,net);auto &r=f.runtime;
    // The runtime always passes its zero-initialized station BSS before the
    // first association. Independent device/status getters must still work.
    assert(!r.station.request.bss.frequency && !r.station.vdev_up);
    for(unsigned type:{GWI_CAPABILITIES,GWI_STATE,GWI_PHY_MODE,GWI_ASSOCIATE_RESULT,GWI_ASSOCIATION_STATUS}) {
        const size_t expected=gwi_get_bytes(type);
        auto query=envelope(false,type,gwi_scalar(type)?0:expected);
        uint8_t initial[64];memset(initial,0xa5,sizeof(initial));size_t count=sizeof(initial);
        assert(f.call(query,initial,count)==kIOReturnSuccess && count==expected);
        if(type==GWI_STATE)assert(!gwq_u32(initial));
        if(type==GWI_PHY_MODE)assert(gwq_u32(initial)==1 && !gwq_u32(initial+8));
        assert(initial[expected]==0xa5);
    }
    auto input=envelope(true,GWN_ASSOCIATE,GWN_ASSOCIATE_BYTES);uint8_t *p=input.data()+8;
    gwq_w32(p,1);gwq_w16(p+4,2);gwq_w16(p+6,1);gwq_w32(p+12,3);memcpy(p+16,"lab",3);
    uint8_t output[GWN_ASSOCIATE_BYTES];size_t bytes=sizeof(output)-1;
    assert(f.call(input,output,bytes)==kIOReturnBadArgument && !bytes && !r.join_pending);
    bytes=sizeof(output);assert(f.call(input,output,bytes)==0 && bytes==sizeof(output) && r.join_pending);
    assert(!memcmp(output,p,sizeof(output)) && !memcmp(r.join_request.bss.bssid,r.wmi.bss[0].bssid,6));
    bytes=0;assert(f.call(input,nullptr,bytes)==kIOReturnBusy);
    auto leave=envelope(true,GWN_DISASSOCIATE,0);bytes=0;
    assert(f.call(leave,nullptr,bytes)==0 && r.disconnect_pending==3);
    // A cancelled queued join cannot turn into a peer-create on the next tick.
    gn_work(&r);assert(!r.join_pending && !r.disconnect_pending && r.station.phase==GWT_IDLE);
    bytes=0;assert(f.call(input,nullptr,bytes)==0);gn_work(&r);
    assert(!r.join_pending && r.assoc_notice && r.net_result && r.net_status && !r.join.has_pmk);
    std::vector<uint8_t> dma(GWD_BYTES+0x3fff);auto *memory=(uint8_t *)(((uintptr_t)dma.data()+0x3fff)&~(uintptr_t)0x3fff);
    r.dio={&r,gc_read,gc_write,gm_ready,network_test_sync,gw_send,gw_pump,gw_now,gs_idle,gd_receive,gd_complete,gd_peer,memory,GW_IOVA};
    r.dp.active=1;gwd_layout(&r.dp);r.wmi.pdev=0;r.wmi.fw_pdev=1;r.wmi.pmac=1;
    bytes=0;assert(f.call(input,nullptr,bytes)==0);gn_work(&r);
    assert(r.dp.peer_selected && r.dp.pdev==0 && r.dp.pmac==1 && !r.join_pending);
    // Runtime getters describe a confirmed peer, never the scan cache.
    auto bssid=envelope(false,GWI_BSSID,6);bytes=sizeof(output);
    assert(f.call(bssid,output,bytes)==kIOReturnNotReady && !bytes);
    r.station.request.bss=r.wmi.bss[0];r.station.vdev_up=1;r.station.phase=GWT_RUN;
    r.dp.active=r.dp.peer_mapped=r.dp.authorized=1;r.dp.queues=0x1ffff;
    bytes=sizeof(output);assert(f.call(bssid,output,bytes)==0 && bytes==6 && output[5]==2);
    auto power=envelope(true,GWI_POWER,GWI_POWER_BYTES);
    gwq_w32(power.data()+8,1);gwq_w32(power.data()+12,1);bytes=0;
    assert(f.call(power,nullptr,bytes)==0 && !r.powered && !r.dp.authorized && r.disconnect_pending==3);
    bytes=sizeof(output);assert(f.call(bssid,output,bytes)==kIOReturnNotReady && !bytes);
    bytes=0;assert(f.call(input,nullptr,bytes)==kIOReturnNotReady);
}
static mbuf_t network_mbuf(const uint8_t *p,size_t n) {
    mbuf_t m=nullptr;unsigned chunks=1;assert(!mbuf_allocpacket(0,n,&chunks,&m));
    memcpy(m->bytes.data(),p,n);return m;
}
static void network_packets() {
    Fixture f;FakeIfnet net{};network_setup(f,net);auto &r=f.runtime;
    uint8_t packet[64]={0};memcpy(packet,r.mac,6);memcpy(packet+6,r.wmi.bss[0].bssid,6);packet[12]=8;
    assert(gn_output(&net,network_mbuf(packet,sizeof(packet)))==ENETDOWN && !mbufs);
    r.dp.active=r.dp.peer_mapped=r.dp.authorized=1;r.dp.queues=0x1ffff;
    for(unsigned i=0;i<GWN_QUEUE_COUNT;i++)assert(!gn_output(&net,network_mbuf(packet,sizeof(packet))));
    assert(gn_output(&net,network_mbuf(packet,sizeof(packet)))==ENOBUFS && !mbufs);
    gwn_clear(&r.tx_queue,sizeof(r.tx_queue));
    packet[12]=0x88;packet[13]=0x8e;
    assert(gn_output(&net,network_mbuf(packet,sizeof(packet)))==EACCES && !mbufs);
    auto m=network_mbuf(packet,sizeof(packet));m->copyFailure=true;assert(gn_output(&net,m)==EINVAL && !mbufs);
    assert(gn_output(&net,network_mbuf(packet,13))==EMSGSIZE && !mbufs);
    r.station.request.bss=r.wmi.bss[0];r.station.request.security=1;r.station.peer_mapped=1;
    assert(!gd_receive(&r,packet,sizeof(packet)) && r.eapol_queue.count==1 && !r.rx_queue.count);
    packet[6]^=2;assert(gd_receive(&r,packet,sizeof(packet)) && r.eapol_queue.count==1);packet[6]^=2;
    packet[12]=8;packet[13]=0;unsigned before=networkInputs;
    assert(!gd_receive(&r,packet,sizeof(packet)));gn_deliver(&r);
    assert(networkInputs==before+1 && networkInput.size()==sizeof(packet) && !networkInputFlags && !mbufs);
    memset(packet,255,6);assert(!gd_receive(&r,packet,sizeof(packet)));gn_deliver(&r);
    assert(networkInputFlags==MBUF_BCAST && !mbufs);
    memset(packet,0,6);packet[0]=1;assert(!gd_receive(&r,packet,sizeof(packet)));gn_deliver(&r);
    assert(networkInputFlags==MBUF_MCAST && !mbufs);
    before=networkInputs;assert(!gd_receive(&r,packet,sizeof(packet)));failMbufAllocation=true;gn_deliver(&r);failMbufAllocation=false;
    assert(networkInputs==before && !mbufs);
    assert(!gd_receive(&r,packet,sizeof(packet)));failMbufCopyback=true;gn_deliver(&r);failMbufCopyback=false;
    assert(networkInputs==before && !mbufs);
    assert(!gd_receive(&r,packet,sizeof(packet)));assert(!gt_port(&r,0,0));gn_deliver(&r);
    assert(!r.rx_queue.count && networkInputs==before && !r.dp.authorized && !(networkFlags&IFF_RUNNING));
    assert(gd_receive(&r,packet,sizeof(packet)));
}
static void network_shutdown() {
    Fixture f;FakeIfnet net{};network_setup(f,net);auto &r=f.runtime;
    r.join.has_pmk=1;memset(r.join.pmk,0x77,32);memset(r.rsn.pmk,0x88,32);
    uint8_t packet[64]={0};assert(gwn_enqueue(&r.tx_queue,packet,sizeof(packet)));
    assert(gwn_enqueue(&r.rx_queue,packet,sizeof(packet)));assert(gwn_enqueue(&r.eapol_queue,packet,sizeof(packet)));
    r.dp.authorized=r.join_pending=r.rsn_started=1;
    gn_disable(&r);
    assert(!r.network_available && !r.dp.authorized && !r.join_pending && !r.rsn_started);
    assert(!gwn_nonzero((uint8_t *)&r.join,sizeof(r.join)) && !gwn_nonzero((uint8_t *)&r.rsn,sizeof(r.rsn)));
    assert(!r.tx_queue.count && !r.rx_queue.count && !r.eapol_queue.count && r.link_pending);
}
static void network_native_abi() {
    Fixture f;FakeIfnet net{};network_setup(f,net);auto &r=f.runtime;
    r.station.request.bss=r.wmi.bss[0];r.station.request.security=1;
    r.station.phase=GWT_RUN;r.station.vdev_up=1;r.dp.authorized=1;r.link_pending=1;
    gt_notify(&r,GWT_RUN,0);networkEventData.clear();gn_deliver(&r);
    bool link=false,assoc=false;
    for(const auto &event:networkEventData)if(event.kind==6) {
        assert(!memcmp(event.data.data(),"en0",4));
        if(event.code==4){assert(event.data.size()==48 && event.data[16]==0);link=true;}
        if(event.code==9){assert(event.data.size()==168 && !memcmp(event.data.data()+32,r.station.request.bss.bssid,6));assoc=true;}
    }
    assert(link && assoc);auto result=envelope(false,GWI_ASSOCIATE_RESULT,0);uint8_t out[4];size_t bytes=sizeof(out);
    assert(!f.call(result,out,bytes) && bytes==4 && gwq_u32(out)==1);
    auto scan=envelope(true,GWI_SCAN_REQUEST,GWI_SCAN_REQUEST_BYTES);uint8_t *p=scan.data()+8;
    gwq_w32(p,1);gwq_w32(p+0x34,1);gwq_w32(p+0x10,3);memcpy(p+0x14,"lab",3);
    gwq_w32(p+0x44,2);gwq_w32(p+0x48,1);gwq_w32(p+0x4c,1);gwq_w32(p+0x50,8);
    gwq_w32(p+0x54,1);gwq_w32(p+0x58,36);gwq_w32(p+0x5c,16);
    bytes=0;assert(!f.call(scan,nullptr,bytes) && r.scan_pending);
    assert(r.scan_request.ssid_count==1 && r.scan_request.frequency_count==2 && r.scan_request.active);
    assert(r.scan_request.frequencies[0]==2412 && r.scan_request.frequencies[1]==5180);
    auto state=envelope(false,GWI_STATE,0);bytes=sizeof(out);
    assert(!f.call(state,out,bytes) && gwq_u32(out)==4);
    r.scan_pending=0;gwq_w32(p+0x10,0);gwq_w32(p+0x1314,2);
    for(unsigned i=0;i<2;i++){uint8_t *s=p+0x1318+56*i;gwq_w32(s,1);gwq_w32(s+4,3);memcpy(s+8,i?"two":"one",3);}
    bytes=0;assert(!f.call(scan,nullptr,bytes) && r.scan_request.ssid_count==2);
    r.scan_pending=0;gwq_w32(p+0x1314,11);bytes=0;
    assert(f.call(scan,nullptr,bytes)==kIOReturnBadArgument && !r.scan_pending);
    gwq_w32(p+0x1314,0);gwq_w32(p+0x44,401);bytes=0;
    assert(f.call(scan,nullptr,bytes)==kIOReturnBadArgument && !r.scan_pending);
}
