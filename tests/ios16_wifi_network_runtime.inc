static void network_test_sync(void *,size_t offset,size_t bytes,int) {assert(bytes && offset<=GWD_BYTES-bytes);}
static void network_dma_mapping() {
    giulia_wifi_runtime r={};uintptr_t cpu=0;uint64_t physical=0,iova=0;
    assert(gwmap_dma_init(&r.dma,0x10000000,UINT64_C(0x8d0000000),GW_IOVA,GW_ARENA_BYTES));
    // Firmware owns the preceding dynamic allocation. The data path's sync
    // callback must confine even malformed relative ranges to its own block.
    assert(gwmap_dma_allocate(&r.dma,17,0x4000,&cpu,&physical,&iova));
    assert(gwmap_dma_allocate(&r.dma,GWD_BYTES,0x4000,&cpu,&physical,&iova));
    const size_t start=(size_t)(iova-r.dma.iova);r.dp_offset=start;
    gd_sync(&r,0,GWD_BYTES,1);assert(!r.mapping_error);
    gd_sync(&r,GWD_BYTES-1,1,0);assert(!r.mapping_error);
    const gwmap_dma saved=r.dma;
    for(auto range:{std::make_pair(size_t(0),size_t(0)),std::make_pair(size_t(GWD_BYTES),size_t(1)),
                    std::make_pair(size_t(GWD_BYTES-1),size_t(2)),std::make_pair(SIZE_MAX,size_t(1))}) {
        r.mapping_error=0;gd_sync(&r,range.first,range.second,1);assert(r.mapping_error);
    }
    for(size_t invalid:{GW_ALLOC_START-GWD_BYTES,start+0x4000,SIZE_MAX-size_t(0x3fff)}) {
        r.mapping_error=0;r.dp_offset=invalid;gd_sync(&r,0x4000,1,1);assert(r.mapping_error);
    }
    assert(!memcmp(&saved,&r.dma,sizeof(saved)));
}
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
        if(type==GWI_CAPABILITIES)assert(initial[0]==8 && initial[1]==64 && initial[9]==8);
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
    auto phy=envelope(false,GWI_PHY_MODE,12);bytes=sizeof(output);
    assert(!f.call(phy,output,bytes) && bytes==12 && gwq_u32(output+8)==4);
    r.station.request.bss.rate_count=2;r.station.request.bss.rates[1]=0x8c;bytes=sizeof(output);
    assert(!f.call(phy,output,bytes) && gwq_u32(output+8)==8);
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
static void network_ioctls() {
    Fixture f;FakeIfnet net{};network_setup(f,net);auto &r=f.runtime;
    networkFlags=0;networkMtu=1500;networkMtuCalls=0;
    struct ifreq request={};
    assert(!gn_ioctl(&net,SIOCSIFADDR,&request));
    assert((networkFlags&IFF_UP) && !(networkFlags&IFF_RUNNING) && !r.dp.authorized);
    for(int mtu:{int(IF_MINMTU),1492,1500}) {
        request.ifr_mtu=mtu;assert(!gn_ioctl(&net,SIOCSIFMTU,&request));
        assert(networkMtu==(unsigned)mtu);
    }
    unsigned calls=networkMtuCalls;
    for(int mtu:{-1,0,IF_MINMTU-1,1501,65535}) {
        request.ifr_mtu=mtu;assert(gn_ioctl(&net,SIOCSIFMTU,&request)==EINVAL && networkMtu==1500);
    }
    assert(networkMtuCalls==calls);
    networkMtuError=ENOMEM;request.ifr_mtu=1492;
    assert(gn_ioctl(&net,SIOCSIFMTU,&request)==ENOMEM && networkMtu==1500);networkMtuError=0;
    assert(!gn_ioctl(&net,SIOCGIFDEVMTU,&request));
    assert(request.ifr_devmtu.ifdm_min==IF_MINMTU && request.ifr_devmtu.ifdm_max==1500 && request.ifr_devmtu.ifdm_current==1500);
    for(unsigned command:{SIOCADDMULTI,SIOCDELMULTI})assert(!gn_ioctl(&net,command,&request));
    for(unsigned command:{SIOCGIFMEDIA32,SIOCGIFMEDIA64}) {
        struct ifmediareq media;memset(&media,0xa5,sizeof(media));
        assert(!gn_ioctl(&net,command,&media) && media.ifm_status==IFM_AVALID && !media.ifm_mask && !media.ifm_count);
        assert(media.ifm_current==(IFM_IEEE80211|IFM_AUTO) && media.ifm_active==media.ifm_current);
        r.dp.authorized=1;assert(!gn_ioctl(&net,command,&media) && media.ifm_status==(IFM_AVALID|IFM_ACTIVE));r.dp.authorized=0;
    }
    request.ifr_media=IFM_IEEE80211|IFM_AUTO;assert(!gn_ioctl(&net,SIOCSIFMEDIA,&request));
    request.ifr_media|=1;assert(gn_ioctl(&net,SIOCSIFMEDIA,&request)==EOPNOTSUPP);
    for(unsigned command:{SIOCSIFMTU,SIOCGIFDEVMTU,SIOCGIFMEDIA32,SIOCGIFMEDIA64,SIOCSIFMEDIA})assert(gn_ioctl(&net,command,nullptr)==EINVAL);
    assert(gn_ioctl(&net,0xffffffffu,&request)==EOPNOTSUPP);
    r.dp.active=r.dp.peer_mapped=r.dp.authorized=1;networkFlags=IFF_RUNNING;
    assert(!gn_ioctl(&net,SIOCSIFFLAGS,&request) && r.disconnect_pending==3 && !r.dp.authorized);
    assert(!(networkFlags&IFF_RUNNING));
}
static void network_extended_queries() {
    Fixture f;FakeIfnet net{};network_setup(f,net);auto &r=f.runtime;
    auto current=envelope(false,GWI_CURRENT_NETWORK,GWI_SCAN_RESULT_BYTES);
    std::vector<uint8_t> result(GWI_SCAN_RESULT_BYTES+1,0xa5);size_t bytes=result.size();
    assert(f.call(current,result.data(),bytes)==kIOReturnNotReady && !bytes && result[0]==0xa5);
    r.station.request.bss=r.wmi.bss[0];r.station.request.bss.timestamp=runtimeNow-100;
    r.station.phase=GWT_RUN;r.station.vdev_up=1;r.dp.active=r.dp.peer_mapped=r.dp.authorized=1;
    r.wmi.scanning=1;bytes=result.size();
    assert(!f.call(current,result.data(),bytes) && bytes==GWI_SCAN_RESULT_BYTES);
    assert(result[0x60]==3 && !memcmp(result.data()+0x61,"lab",3) && gwq_u32(result.data()+0x84)==100 && result[bytes]==0xa5);
    // Reading the current peer must not consume or depend on scan iteration.
    r.wmi.scanning=0;r.wmi.bss[0].ssid[0]='X';bytes=result.size();
    assert(!f.call(current,result.data(),bytes) && result[0x61]=='l');
    auto scan=envelope(false,GWI_SCAN_RESULT,GWI_SCAN_RESULT_BYTES);bytes=result.size();
    assert(!f.call(scan,result.data(),bytes) && result[0x61]=='X');
    auto leave=envelope(true,GWN_DISASSOCIATE,8);gwq_w32(leave.data()+8,0xaaaaaaaa);gwq_w32(leave.data()+12,70000);
    bytes=0;assert(f.call(leave,nullptr,bytes)==kIOReturnBadArgument && r.dp.authorized && !r.disconnect_pending);
    gwq_w32(leave.data()+12,7);bytes=0;
    assert(!f.call(leave,nullptr,bytes) && r.disconnect_pending==7 && !r.dp.authorized);
    bytes=result.size();assert(f.call(current,result.data(),bytes)==kIOReturnNotReady && !bytes);
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
