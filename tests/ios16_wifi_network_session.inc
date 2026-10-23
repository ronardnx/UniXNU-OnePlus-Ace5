/* Exercise the production scheduler and WMI scan commands. Only the CE/DMA
 * device boundary is simulated; join/roam/retry decisions run unmodified.
 * These host fixtures do not prove radio bring-up or DMA cache coherence. */
namespace wifi_session_oracle {
#include "fixtures/ath12k_wifi_layout.h"
}
using wifi_session_oracle::hal_tlv_64_hdr;
using wifi_session_oracle::hal_reo_cmd_hdr;
using wifi_session_oracle::hal_reo_status_hdr;
using wifi_session_oracle::hal_reo_flush_cache_status;
struct NetworkSessionFixture;
static NetworkSessionFixture *networkSessionActive;
static int network_session_send(void *,unsigned,const void *,size_t);
struct NetworkSessionFixture {
    Fixture f;FakeIfnet net{};std::vector<uint8_t> memory;
    std::vector<unsigned> commands;unsigned reo_updates=0,reo_flushes=0;
    NetworkSessionFixture():memory(GWD_BYTES+0x3fff) {
        assert(!networkSessionActive);networkSessionActive=this;runtimeNow=1000;
        network_setup(f,net);auto &r=f.runtime;
        r.wio.send=network_session_send;r.wio.scan_done=gw_notice;
        r.wmi.endpoint=1;r.wmi.max_message=2048;r.wmi.credit_size=256;
        r.wmi.credits=r.wmi.credit_total=255;r.wmi.vdev=r.wmi.fw_ready=r.wmi.hw_valid=1;
        r.wmi.services_valid=1;r.wmi.services[2]=(1u<<(76-64))|(1u<<(79-64));
        memcpy(r.wmi.mac,r.mac,6);r.dp.active=1;gwd_layout(&r.dp);
        auto *aligned=(uint8_t *)(((uintptr_t)memory.data()+0x3fff)&~(uintptr_t)0x3fff);
        r.dio={&r,gc_read,gc_write,gm_ready,network_test_sync,network_session_send,gw_pump,gw_now,gs_idle,
            gd_receive,gd_complete,gd_peer,aligned,GW_IOVA};
        for(unsigned i=0;i<GWD_RX_COUNT;i++)r.dp.rx_cookie[i]=i;
        r.wmi.bss[0].timestamp=runtimeNow;
    }
    ~NetworkSessionFixture(){networkSessionActive=nullptr;}
    void tick(){IOLockLock(&f.lock);gn_session_tick(&f.runtime);IOLockUnlock(&f.lock);}
    void request(const char *ssid="lab") {
        auto p=envelope(true,GWN_ASSOCIATE,GWN_ASSOCIATE_BYTES);auto *b=p.data()+8;
        gwq_w32(b,1);gwq_w16(b+4,2);gwq_w16(b+6,1);gwq_w32(b+12,(unsigned)strlen(ssid));memcpy(b+16,ssid,strlen(ssid));
        size_t bytes=0;assert(f.call(p,nullptr,bytes)==kIOReturnSuccess);
    }
    void scan_event(unsigned event,unsigned reason=0) {
        auto &r=f.runtime;uint8_t p[36]={0};gwq_w32(p,0x3001);gww_tlv(p+4,36,28);
        gwq_w32(p+8,event);gwq_w32(p+12,reason);gwq_w32(p+20,0xa001);gwq_w32(p+24,r.wmi.scan_id);
        IOLockLock(&f.lock);assert(!gww_wmi_receive(&r.wmi,&r.wio,p,sizeof(p)));IOLockUnlock(&f.lock);
    }
    void response(unsigned suffix=2,int rssi=-40) {
        auto &r=f.runtime;uint8_t b[44]={0};b[0]=0x50;memcpy(b+4,r.mac,6);b[10]=b[16]=2;b[15]=b[21]=(uint8_t)suffix;
        gwq_w16(b+32,100);gwq_w16(b+34,1);b[36]=0;b[37]=3;memcpy(b+38,"lab",3);b[41]=1;b[42]=1;b[43]=0x82;
        IOLockLock(&f.lock);gww_beacon(&r.wmi,&r.wio,b,sizeof(b),2412,rssi);IOLockUnlock(&f.lock);
    }
    void connected() {
        auto &r=f.runtime;r.station.request.bss=r.wmi.bss[0];r.station.phase=GWT_RUN;
        r.station.peer_created=r.station.peer_mapped=r.dp.peer_selected=r.dp.peer_mapped=1;
        r.station.peer_id=r.dp.peer_id=7;r.dp.endpoint=7;memcpy(r.dp.bssid,r.wmi.bss[0].bssid,6);
        r.station.vdev_up=r.station.authorized=r.dp.authorized=1;
        r.profile.ssid_bytes=3;memcpy(r.profile.ssid,"lab",3);r.profile_valid=r.profile_connected=1;
        r.next_roam_scan=runtimeNow+30000;
    }
    bool reo_completion() {
        // Decode actual command bytes with the pinned upstream wire oracle;
        // do not use the driver's expected tag, pending ID or reclaim phase.
        auto &r=f.runtime;auto &cmd=r.dp.ring[GWD_REO_COMMAND];auto &status=r.dp.ring[GWD_REO_STATUS];
        auto *ram=r.dio.memory;unsigned tail=gwq_u32(ram+cmd.remote)/cmd.entry;
        if(tail==cmd.head)return false;
        const uint8_t *p=ram+cmd.desc+tail*cmd.entry*4;unsigned tag=(gwq_u32(p)&HAL_TLV_HDR_TAG)>>1;
        unsigned id=gwq_u32(p+sizeof(hal_tlv_64_hdr))&HAL_REO_CMD_HDR_INFO0_CMD_NUMBER;
        assert(id && (gwq_u32(p+sizeof(hal_tlv_64_hdr))&HAL_REO_CMD_HDR_INFO0_STATUS_REQUIRED));
        assert(tag==HAL_REO_UPDATE_RX_REO_QUEUE || tag==HAL_REO_FLUSH_CACHE);
        uint64_t address=gwq_u32(p+sizeof(hal_tlv_64_hdr)+sizeof(hal_reo_cmd_hdr));
        assert(address>=r.dio.dma+GWD_QUEUES && address<r.dio.dma+GWD_QUEUES+GWD_QUEUE_BANK_BYTES);
        unsigned reply=HAL_REO_UPDATE_RX_REO_QUEUE_STATUS,bytes=sizeof(hal_reo_status_hdr);
        if(tag==HAL_REO_FLUSH_CACHE){reply=HAL_REO_FLUSH_CACHE_STATUS;bytes=sizeof(hal_reo_flush_cache_status);reo_flushes++;}
        else reo_updates++;
        unsigned head=gwq_u32(ram+status.remote)/status.entry;assert((head+1)%status.count!=status.tail);
        uint8_t *out=ram+status.desc+head*status.entry*4;memset(out,0,status.entry*4);
        gwq_w32(out,(reply<<1)|(bytes<<10));gwq_w32(out+sizeof(hal_tlv_64_hdr),id);
        gwq_w32(ram+status.remote,((head+1)%status.count)*status.entry);
        gwq_w32(ram+cmd.remote,((tail+1)%cmd.count)*cmd.entry);return true;
    }
};
static int network_session_send(void *v,unsigned pipe,const void *data,size_t n) {
    assert(networkSessionActive && v==&networkSessionActive->f.runtime && pipe==3 && n>=12);
    auto &r=networkSessionActive->f.runtime;assert(r.lock->locked);
    networkSessionActive->commands.push_back(gwq_u32((const uint8_t *)data+8));r.wmi.credits=r.wmi.credit_total;return 0;
}
static void network_session_paths() {
    {
        NetworkSessionFixture f;auto &r=f.f.runtime;r.wmi.bss_count=0;f.request();f.tick();
        assert(r.join_pending==GWN_JOIN_SCAN && r.wmi.scanning && r.wmi.scan_request.active);
        assert(r.wmi.scan_request.ssid_count==1 && !memcmp(r.wmi.scan_request.ssids[0].bytes,"lab",3));
        assert(!r.station.peer_created && !r.profile_valid);f.scan_event(1);f.response();f.scan_event(2);
        assert(r.scan_notice_pending && !r.wmi.scanning);f.tick();
        assert(!r.join_pending && r.station.phase==GWT_PEER_WAIT && r.station.peer_created && r.profile_valid);
        assert(f.commands.back()==0x6001 && r.station.request.bss.bssid[5]==2);
        networkEventData.clear();gn_deliver(&r);assert(!r.scan_notice_pending);
        bool done=false;for(const auto &e:networkEventData)if(e.kind==6 && e.code==10){assert(e.data.size()==20 && !gwq_u32(e.data.data()+16));done=true;}assert(done);
    }
    {
        NetworkSessionFixture f;auto &r=f.f.runtime;r.wmi.bss_count=0;f.request();f.tick();f.scan_event(1);f.scan_event(2);f.tick();
        assert(!r.join_pending && !r.station.peer_created && r.net_result==GWN_RESULT_TIMEOUT && !r.profile_valid);
        assert(!gwn_nonzero((const uint8_t *)&r.join,sizeof(r.join)));
    }
    {
        NetworkSessionFixture f;auto &r=f.f.runtime;f.connected();r.wmi.bss[0].rssi=-80;
        r.scan_request.active=1;r.scan_request.ssid_count=1;r.scan_request.ssids[0].length=3;
        memcpy(r.scan_request.ssids[0].bytes,"lab",3);r.scan_pending=1;f.tick();
        assert(r.wmi.scanning && r.wmi.scan_request.connected && r.wmi.scan_request.home_frequency==2412 && r.dp.authorized);
        f.scan_event(1);f.response(2,-80);f.response(3,-54);f.scan_event(2);f.tick();
        assert(r.join_pending==GWN_JOIN_LEAVE && r.join_automatic && r.station.phase==GWT_STOPPING);
        assert(r.join_request.bss.bssid[5]==3 && !r.dp.authorized && r.profile_connected);
        assert(r.net_result==GWN_RESULT_PENDING);
        auto leave=envelope(true,GWN_DISASSOCIATE,0);size_t bytes=0;assert(!f.f.call(leave,nullptr,bytes));f.tick();
        assert(!r.join_pending && !r.profile_valid && !gwn_nonzero((uint8_t *)&r.profile,sizeof(r.profile)));
    }
    {
        NetworkSessionFixture f;auto &r=f.f.runtime;f.connected();r.profile.has_bssid=1;
        memcpy(r.profile.bssid,r.station.request.bss.bssid,6);r.wmi.bss_count=2;r.wmi.bss[1]=r.wmi.bss[0];
        r.wmi.bss[0].rssi=-80;r.wmi.bss[1].rssi=-40;r.wmi.bss[1].bssid[5]=3;gn_consider_roam(&r);
        assert(!r.join_pending && r.dp.authorized);runtimeNow+=31000;f.tick();assert(!r.wmi.scanning);
    }
    {
        NetworkSessionFixture f;auto &r=f.f.runtime;f.connected();f.request("missing");f.tick();
        assert(r.wmi.scanning && r.dp.authorized && !memcmp(r.profile.ssid,"lab",3));
        // GTK rekey can re-enter RUN for the current AP while the requested
        // replacement network is still being scanned. It is not that join's
        // association success and must not emit its completion notification.
        r.assoc_notice=0;IOLockLock(&f.f.lock);gt_notify(&r,GWT_RUN,0);IOLockUnlock(&f.f.lock);
        assert(r.join_pending==GWN_JOIN_SCAN && r.net_result==GWN_RESULT_PENDING && !r.assoc_notice);
        f.scan_event(1);f.scan_event(2);f.tick();assert(!r.join_pending && r.dp.authorized && r.profile_connected);
        assert(!memcmp(r.profile.ssid,"lab",3));
    }
    {
        NetworkSessionFixture f;auto &r=f.f.runtime;f.connected();r.station.phase=GWT_IDLE;r.dp.authorized=0;
        r.retry_at=runtimeNow+2000;r.wmi.bss_count=0;f.tick();assert(!r.join_pending && !r.retry_count);
        for(unsigned attempt=1;attempt<=3;attempt++) {
            runtimeNow=r.retry_at;f.tick();assert(r.join_pending==GWN_JOIN_SCAN && r.retry_count==attempt);
            f.scan_event(1);f.scan_event(2);f.tick();assert(!r.join_pending);
        }
        assert(!r.profile_valid && !r.profile_connected);runtimeNow+=60000;f.tick();assert(!r.wmi.scanning && !r.join_pending);
    }
    for(unsigned cancel=0;cancel<2;cancel++) {
        NetworkSessionFixture f;auto &r=f.f.runtime;f.connected();r.dp.prepared=1;
        for(unsigned tid=0;tid<17;tid++){uint64_t dma=0;assert(!gwd_reo_queue(&r.dp,&r.dio,tid,&dma));}
        r.wmi.bss_count=2;r.wmi.bss[1]=r.wmi.bss[0];r.wmi.bss[0].rssi=-80;
        r.wmi.bss[1].rssi=-40;r.wmi.bss[1].bssid[5]=3;
        IOLockLock(&f.f.lock);gn_consider_roam(&r);IOLockUnlock(&f.f.lock);f.tick();
        assert(r.join_pending==GWN_JOIN_LEAVE && r.station.phase==GWT_STOPPING);
        // Enter the confirmed VDEV_STOP / PEER_DELETE stage. HTT unmap is
        // delivered through the actual datapath callback into the station.
        r.station.stop_stage=3;r.station.delete_confirm=1;r.station.vdev_up=0;
        uint8_t unmap[4];gwq_w32(unmap,4|(7u<<16));
        IOLockLock(&f.f.lock);assert(!gwd_receive_htt(&r.dp,&r.dio,7,unmap,sizeof(unmap)));gwt_tick(&r.station);IOLockUnlock(&f.f.lock);
        assert(r.station.phase==GWT_IDLE && !r.station.peer_created && !r.dp.peer_mapped);
        f.tick();assert(r.reclaim_pending && r.dp.queues==0x1ffff && f.commands.empty());
        if(cancel){auto leave=envelope(true,GWN_DISASSOCIATE,0);size_t bytes=0;assert(!f.f.call(leave,nullptr,bytes));f.tick();}
        for(unsigned step=0;r.reclaim_pending;step++) {
            assert(step<100 && !r.station.peer_created && f.commands.empty());
            IOLockLock(&f.f.lock);assert(!gwd_poll(&r.dp,&r.dio,128));IOLockUnlock(&f.f.lock);
            (void)f.reo_completion();runtimeNow+=r.dp.reclaim_phase==GWD_RECLAIM_AGE?1000:1;f.tick();
        }
        assert(f.reo_updates==17 && f.reo_flushes==65 && !r.dp.queues && !r.dp.quarantine);
        if(cancel)assert(!r.profile_valid && !r.join_pending && r.station.phase==GWT_IDLE && f.commands.empty());
        else {
            assert(r.station.phase==GWT_PEER_WAIT && r.station.request.bss.bssid[5]==3);
            assert(r.profile_valid && r.profile_connected && !r.profile.has_bssid && f.commands.back()==0x6001);
        }
    }
    puts("Host Wi-Fi sessions: directed join, connected scan, RSSI roam, pinned BSSID, failed-switch retention, bounded reconnect, deferred notifications and confirmed REO reclaim passed");
}
