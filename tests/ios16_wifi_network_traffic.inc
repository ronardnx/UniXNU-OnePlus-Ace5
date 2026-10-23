/* Host integration through the real RSN, BSD, TCL/WBM and RXDMA/REO bridge.
 * Firmware/DMA and ifnet_input are modeled; this does not run an IP stack or
 * establish that DHCP, DNS or physical radio traffic succeeds on the phone. */
using TrafficRx=wifi_session_oracle::hal_rx_desc_wcn7850;
using TrafficReo=wifi_session_oracle::hal_reo_dest_ring;
using TrafficTx=wifi_session_oracle::hal_tcl_data_cmd;
static uint32_t traffic_field(uint32_t mask,unsigned value) {
    unsigned shift=0;while(!(mask&1)){mask>>=1;shift++;}return (value&mask)<<shift;
}
static std::vector<uint8_t> traffic_frame(giulia_wifi_runtime &r,unsigned kind,bool incoming) {
    const unsigned sizes[]={42,342,96,86};std::vector<uint8_t> p(sizes[kind]);
    static const uint8_t wired[6]={2,9,8,7,6,5};
    memcpy(p.data(),incoming?r.mac:r.station.request.bss.bssid,6);
    memcpy(p.data()+6,incoming?wired:r.mac,6);
    if(kind==0 || kind==1)memset(p.data(),255,6);
    if(kind==0) {
        p[12]=8;p[13]=6;p[15]=1;p[16]=8;p[18]=6;p[19]=4;p[21]=incoming?2:1;
        memcpy(p.data()+22,p.data()+6,6);p[28]=192;p[29]=168;p[30]=1;p[31]=incoming?1:2;
        memcpy(p.data()+32,incoming?r.mac:wired,6);p[38]=192;p[39]=168;p[40]=1;p[41]=incoming?2:1;
    } else if(kind<3) {
        p[12]=8;p[14]=0x45;gwr_w16(p.data()+16,(unsigned)p.size()-14);p[22]=64;p[23]=17;
        p[26]=192;p[27]=168;p[28]=1;p[29]=incoming?1:2;
        p[30]=192;p[31]=168;p[32]=1;p[33]=incoming?2:1;
        gwr_w16(p.data()+34,kind==1?(incoming?67:68):(incoming?53:49152));
        gwr_w16(p.data()+36,kind==1?(incoming?68:67):(incoming?49152:53));
        gwr_w16(p.data()+38,(unsigned)p.size()-34);
        if(kind==1) {
            p[42]=incoming?2:1;p[43]=1;p[44]=6;memcpy(p.data()+70,r.mac,6);
            p[278]=99;p[279]=130;p[280]=83;p[281]=99;p[282]=53;p[283]=1;p[284]=incoming?2:1;p[285]=255;
        } else {p[42]=0x12;p[43]=0x34;p[44]=incoming?0x81:1;p[47]=1;}
    } else {
        p[12]=0x86;p[13]=0xdd;p[14]=0x60;gwr_w16(p.data()+18,(unsigned)p.size()-54);p[20]=58;p[21]=255;
        p[22]=p[38]=0xfe;p[23]=p[39]=0x80;p[37]=incoming?1:2;p[53]=incoming?2:1;p[54]=incoming?136:135;
    }
    return p;
}
static void traffic_tx_complete(NetworkRsnFixture &h,uint32_t rate=0) {
    auto &r=h.f.runtime;auto &tx=r.dp.ring[GWD_TCL];auto &done=r.dp.ring[GWD_TX_COMPLETE];
    auto *ram=r.dio.memory;
    while(gwq_u32(ram+tx.remote)/tx.entry!=tx.head) {
        unsigned tail=gwq_u32(ram+tx.remote)/tx.entry,head=gwq_u32(ram+done.remote)/done.entry;
        auto *out=ram+done.desc+head*sizeof(TrafficTx);memset(out,0,sizeof(TrafficTx));
        memcpy(out,ram+tx.desc+tail*sizeof(TrafficTx),8); // Real descriptor address/cookie returned by the device.
        gwq_w32(out+offsetof(wifi_session_oracle::hal_wbm_completion_ring_tx,rate_stats.info0),rate);
        gwq_w32(ram+tx.remote,((tail+1)%tx.count)*tx.entry);
        gwq_w32(ram+done.remote,((head+1)%done.count)*done.entry);
    }
    IOLockLock(&h.f.lock);assert(!gwd_poll(&r.dp,&r.dio,64));IOLockUnlock(&h.f.lock);
    assert(!r.dp.tx_pending);
}
static void traffic_rx(NetworkRsnFixture &h,const std::vector<uint8_t> &packet,uint64_t pn) {
    auto &r=h.f.runtime;auto *ram=r.dio.memory;auto &refill=r.dp.ring[GWD_REFILL];auto &reo=r.dp.ring[GWD_REO];
    unsigned tail=gwq_u32(ram+refill.remote)/refill.entry,head=gwq_u32(ram+reo.remote)/reo.entry;
    const uint8_t *buffer=ram+refill.desc+tail*8;unsigned slot=(gwq_u32(buffer+4)>>12)&(GWD_RX_COUNT-1);
    uint8_t *p=ram+GWD_RX_BUFFERS+slot*GWD_BUFFER;memset(p,0,GWD_BUFFER);
    gwq_w64(p+offsetof(TrafficRx,msdu_end_tag),traffic_field(HAL_TLV_64_HDR_TAG,197));
    gwq_w64(p+offsetof(TrafficRx,mpdu_start_tag),traffic_field(HAL_TLV_64_HDR_TAG,194));
    gwq_w32(p+offsetof(TrafficRx,msdu_end.info10),traffic_field(RX_MSDU_END_INFO10_MSDU_LENGTH,(unsigned)packet.size()));
    gwq_w32(p+offsetof(TrafficRx,msdu_end.info11),traffic_field(RX_MSDU_END_INFO11_DECAP_FORMAT,2));
    unsigned group=packet[0]&1;
    gwq_w32(p+offsetof(TrafficRx,msdu_end.info13),group?RX_MSDU_END_INFO13_MCAST_BCAST:0);
    gwq_w32(p+offsetof(TrafficRx,msdu_end.info14),RX_MSDU_END_INFO14_MSDU_DONE);
    gwq_w16(p+offsetof(TrafficRx,mpdu_start.sw_peer_id),(uint16_t)r.dp.peer_id);
    memcpy(p+offsetof(TrafficRx,mpdu_start.addr2),r.station.request.bss.bssid,6);
    gwq_w32(p+offsetof(TrafficRx,mpdu_start.info4),RX_MPDU_START_INFO4_MPDU_FCTRL_VALID|
        RX_MPDU_START_INFO4_MAC_ADDR2_VALID|RX_MPDU_START_INFO4_ENCRYPTED|RX_MPDU_START_INFO4_ENCRYPT_INFO_VALID);
    gwq_w32(p+offsetof(TrafficRx,mpdu_start.info2),traffic_field(RX_MPDU_START_INFO2_ENC_TYPE,6));
    gwq_w32(p+offsetof(TrafficRx,mpdu_start.info5),traffic_field(RX_MPDU_START_INFO5_KEY_ID,group?1:0));
    gwq_w64(p+offsetof(TrafficRx,mpdu_start.pn),pn);memcpy(p+offsetof(TrafficRx,msdu_payload),packet.data(),packet.size());
    auto *out=ram+reo.desc+head*sizeof(TrafficReo);memset(out,0,sizeof(TrafficReo));memcpy(out,buffer,8);
    gwq_w32(out+offsetof(TrafficReo,rx_msdu_info.info0),RX_MSDU_DESC_INFO0_FIRST_MSDU_IN_MPDU|
        RX_MSDU_DESC_INFO0_LAST_MSDU_IN_MPDU|traffic_field(RX_MSDU_DESC_INFO0_MSDU_LENGTH,(unsigned)packet.size()));
    gwq_w32(out+offsetof(TrafficReo,info0),traffic_field(HAL_REO_DEST_RING_INFO0_PUSH_REASON,1));
    gwq_w32(ram+refill.remote,((tail+1)%refill.count)*refill.entry);
    gwq_w32(ram+reo.remote,((head+1)%reo.count)*reo.entry);
    IOLockLock(&h.f.lock);assert(!gwd_poll(&r.dp,&r.dio,64));IOLockUnlock(&h.f.lock);
    gn_deliver(&r);
}
static void network_traffic_bridge() {
    NetworkRsnFixture h;auto &r=h.f.runtime;network_rsn_start(h);
    auto blocked=traffic_frame(r,1,false);assert(gn_output(&h.net,network_mbuf(blocked.data(),blocked.size()))==ENETDOWN);
    h.receive(network_rsn_key(r.rsn,2,1,1,0x77));h.work();h.key_ack(0,1);h.work();h.work();h.key_ack(1,0);h.work();
    assert(r.dp.authorized && r.station.phase==GWT_RUN);
    for(unsigned i=0;i<GWD_RX_COUNT;i++)r.dp.rx_cookie[i]=i;
    traffic_tx_complete(h);
    auto rate_query=envelope(false,GWI_RATE,0);uint8_t measured[4];size_t measured_bytes=sizeof(measured);
    assert(h.f.call(rate_query,measured,measured_bytes)==kIOReturnNotReady && !measured_bytes);
    assert(r.dp.rx_owned[0] && r.dp.ring[GWD_REFILL].head); // TX poll also refills real RX slots.
    struct ifreq request={};assert(!gn_ioctl(&h.net,SIOCSIFADDR,&request));
    for(unsigned kind=0;kind<4;kind++) {
        auto outgoing=traffic_frame(r,kind,false);auto &tx=r.dp.ring[GWD_TCL];unsigned head=tx.head;
        assert(!gn_output(&h.net,network_mbuf(outgoing.data(),outgoing.size())) && r.tx_queue.count==1);h.work();
        assert(!r.tx_queue.count && tx.head==(head+1)%tx.count);
        const uint8_t *d=r.dio.memory+tx.desc+head*sizeof(TrafficTx);
        size_t at=(size_t)(gwq_u32(d)-(uint32_t)r.dio.dma);
        assert(at<GWD_BYTES-outgoing.size() && !memcmp(r.dio.memory+at,outgoing.data(),outgoing.size()));
        uint32_t rate=HAL_TX_RATE_STATS_INFO0_VALID|
            traffic_field(HAL_TX_RATE_STATS_INFO0_PKT_TYPE,wifi_session_oracle::HAL_TX_RATE_STATS_PKT_TYPE_11A)|
            traffic_field(HAL_TX_RATE_STATS_INFO0_MCS,wifi_session_oracle::ATH12K_HW_RATE_OFDM_54M);
        traffic_tx_complete(h,rate);measured_bytes=sizeof(measured);
        assert(!h.f.call(rate_query,measured,measured_bytes) && measured_bytes==4 && gwq_u32(measured)==54);
        auto mcs_query=envelope(false,GWI_MCS,0);measured_bytes=sizeof(measured);
        assert(!h.f.call(mcs_query,measured,measured_bytes) && !gwq_u32(measured));
        auto incoming=traffic_frame(r,kind,true);unsigned before=networkInputs;
        traffic_rx(h,incoming,10+kind);assert(networkInputs==before+1 && networkInput==incoming && !mbufs);
        traffic_rx(h,incoming,10+kind);assert(networkInputs==before+1); // Real RX PN rejects the replay.
    }
    assert(!gn_ioctl(&h.net,SIOCSIFFLAGS,&request)); // IFF_UP is still set: no fabricated disconnect.
    networkFlags&=~IFF_UP;assert(!gn_ioctl(&h.net,SIOCSIFFLAGS,&request) && !r.dp.authorized);
    measured_bytes=sizeof(measured);assert(h.f.call(rate_query,measured,measured_bytes)==kIOReturnNotReady && !measured_bytes);
    assert(gn_output(&h.net,network_mbuf(blocked.data(),blocked.size()))==ENETDOWN && !mbufs);
    puts("Actual traffic bridge: firmware-gated WPA2 -> BSD address enable -> ARP/IPv4 UDP DHCP/DNS/IPv6 frames -> TCL/WBM and RXDMA/REO -> BSD; replay and interface-down gates passed (host model, no IP-stack or radio claim)");
}
