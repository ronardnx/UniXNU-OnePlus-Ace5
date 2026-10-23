/* Actual network security hooks, RSN/SAE providers, station events and WMI
 * management ownership. Only the hardware DMA/firmware boundary is modeled;
 * successful protected RX models firmware-authenticated CCMP plaintext. */
#include <openssl/core_names.h>
struct NetworkSecurityFixture : NetworkRsnFixture {
    std::vector<uint8_t> arena;
    NetworkSecurityFixture():arena(GWMAP_ARENA_BYTES+0x3fff) {
        auto &r=f.runtime;unsigned phase=r.station.phase;r.station.phase=GWT_IDLE;
        gwt_security_ops ops={&r,gns_auth_start,gns_auth_rx,gns_mgmt_rx,gns_mgmt_tx,gns_protected_ready};
        assert(!gwt_set_security_ops(&r.station,&ops));r.station.phase=phase;
        r.join_request.pmf=r.station.request.pmf=1;assert(!gns_prepare(&r));
        auto *base=(uint8_t *)(((uintptr_t)arena.data()+0x3fff)&~(uintptr_t)0x3fff);
        assert(gwmap_dma_init(&r.dma,(uintptr_t)base,0x90000000,GW_IOVA,GWMAP_ARENA_BYTES));
        uintptr_t cpu=0;uint64_t physical=0,iova=0;
        assert(gwmap_dma_allocate(&r.dma,GWD_BYTES,0x4000,&cpu,&physical,&iova));
        r.dp_offset=(size_t)(iova-r.dma.iova);r.dio.memory=(uint8_t *)cpu;r.dio.dma=iova;
        r.station.rates[0]=0x82;r.station.rate_count=1;
    }
    std::vector<uint8_t> header(unsigned type,size_t bytes) {
        auto &r=f.runtime;std::vector<uint8_t> p(bytes);p[0]=(uint8_t)type;
        memcpy(p.data()+4,r.mac,6);memcpy(p.data()+10,r.station.request.bss.bssid,6);
        memcpy(p.data()+16,r.station.request.bss.bssid,6);return p;
    }
    void management(const std::vector<uint8_t> &frame,unsigned status=0,unsigned frequency=2412) {
        std::vector<uint8_t> p(80+frame.size());gwq_w32(p.data(),0x7001);gww_tlv(p.data()+4,44,68);
        gwq_w32(p.data()+24,(unsigned)frame.size());gwq_w32(p.data()+28,status);gwq_w32(p.data()+72,frequency);
        gww_tlv(p.data()+76,17,(unsigned)frame.size());memcpy(p.data()+80,frame.data(),frame.size());event(p.data(),p.size());
    }
    void tx_ack() {
        auto &r=f.runtime;assert(r.station.tx_pending && r.mgmt_cookie==r.station.tx_cookie);
        uint8_t p[20]={0};gwq_w32(p,0x7006);gww_tlv(p+4,423,12);gwq_w32(p+8,r.station.tx_cookie);
        event(p,sizeof(p));assert(!r.station.tx_pending && !r.mgmt_cookie);
    }
    std::vector<uint8_t> transmitted() {
        auto &r=f.runtime;assert(gwq_u32(lastCommand.data())==0x7008 && r.mgmt_cookie);
        unsigned bytes=gwq_u32(lastCommand.data()+28);assert(bytes>=24 && bytes<=GWT_MGMT_MAX);
        assert(gwq_u32(lastCommand.data()+20)==(uint32_t)(r.dio.dma+GWD_MGMT));
        assert(!memcmp(lastCommand.data()+44,r.dio.memory+GWD_MGMT,bytes<64?bytes:64));
        return std::vector<uint8_t>(r.dio.memory+GWD_MGMT,r.dio.memory+GWD_MGMT+bytes);
    }
    std::vector<uint8_t> protected_action(uint64_t pn,unsigned action,const uint8_t transaction[2]) {
        auto p=header(0xd0,44);p[1]=0x40;p[24]=(uint8_t)pn;p[25]=(uint8_t)(pn>>8);p[27]=0x20;
        for(unsigned i=0;i<4;i++)p[28+i]=(uint8_t)(pn>>(16+8*i));
        p[32]=8;p[33]=(uint8_t)action;memcpy(p.data()+34,transaction,2);return p;
    }
};
static void network_security_cmac(const uint8_t key[16],const uint8_t *p,size_t n,uint8_t out[16]) {
    EVP_MAC *mac=EVP_MAC_fetch(nullptr,"CMAC",nullptr);assert(mac);EVP_MAC_CTX *ctx=EVP_MAC_CTX_new(mac);assert(ctx);
    char cipher[]="AES-128-CBC";OSSL_PARAM params[]={OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_CIPHER,cipher,0),OSSL_PARAM_construct_end()};
    size_t bytes=0;assert(EVP_MAC_init(ctx,key,16,params) && EVP_MAC_update(ctx,p,n) && EVP_MAC_final(ctx,out,&bytes,16) && bytes==16);
    EVP_MAC_CTX_free(ctx);EVP_MAC_free(mac);
}
static std::vector<uint8_t> network_security_key(const gwr_state &s,uint64_t replay,unsigned pairwise,unsigned igtk,unsigned include_rsnxe=1) {
    uint8_t plain[128]={0};size_t bytes=0;
    if(pairwise){memcpy(plain,s.ap_rsn,s.ap_bytes);bytes=s.ap_bytes;}
    if(pairwise && s.security.h2e && include_rsnxe){plain[bytes++]=244;plain[bytes++]=1;plain[bytes++]=0x20;}
    uint8_t *k=plain+bytes;k[0]=221;k[1]=22;k[3]=15;k[4]=172;k[5]=1;k[6]=1;memset(k+8,0x77,16);bytes+=24;
    if(igtk){k=plain+bytes;k[0]=221;k[1]=28;k[3]=15;k[4]=172;k[5]=9;k[6]=4;k[8]=9;memset(k+14,0x88,16);bytes+=30;}
    if(bytes&7){plain[bytes++]=221;while(bytes&7)plain[bytes++]=0;}
    auto p=network_rsn_m1(replay);p.resize(99+bytes+8);memcpy(p.data()+17,s.anonce,32);
    gwr_w16(p.data()+5,(pairwise?0x13c8u:0x1380u)|(s.security.sae?0u:2u));p[65]=9;
    AES_KEY aes;assert(!AES_set_encrypt_key(s.ptk+16,128,&aes));
    assert(AES_wrap_key(&aes,nullptr,p.data()+99,plain,(unsigned)bytes)==(int)bytes+8);
    gwr_w16(p.data()+97,(unsigned)bytes+8);gwr_w16(p.data()+2,(unsigned)p.size()-4);
    if(s.security.sae){uint8_t mic[16];network_security_cmac(s.ptk,p.data(),p.size(),mic);memcpy(p.data()+81,mic,16);}
    else network_rsn_sign(s.ptk,p);return p;
}
static void network_security_rsn(NetworkSecurityFixture &h) {
    auto &r=h.f.runtime;h.association_ack();h.work();assert(r.rsn_started && r.rsn.security.pmf && !gpm_ready(&r.security.pmf));
    h.receive(network_rsn_m1(1));h.work();assert(r.rsn.phase==GWR_WAIT_M3);
    h.receive(network_security_key(r.rsn,2,1,0));h.work();
    assert(r.station.key_pending && r.security.pending_ptk && !r.security.pmf.ptk_valid && !r.dp.authorized);
    h.key_ack(1,1);h.work();assert(!r.security.pmf.ptk_valid && r.security.pending_ptk);
    h.key_ack(0,1);assert(!r.security.pmf.ptk_valid);h.work();
    assert(r.security.pmf.ptk_valid && !r.security.pending_ptk && !gwn_nonzero(r.security.ptk,16));
    h.work();h.key_ack(1,0);h.work();
    assert(r.rsn.phase==GWR_WAIT_GROUP && !r.dp.authorized && !gpm_ready(&r.security.pmf));
    IOLockLock(&h.f.lock);assert(gr_authorize(&r,1)==GWT_UNAVAILABLE);IOLockUnlock(&h.f.lock);
    auto igtk=network_security_key(r.rsn,3,0,1);igtk[81]^=1;h.receive(igtk);h.work();
    assert(!r.security.pmf.igtk_valid && !r.dp.authorized);igtk[81]^=1;h.receive(igtk);h.work();
    assert(r.rsn.phase==GWR_RUN && r.station.phase==GWT_RUN && r.dp.authorized && gpm_ready(&r.security.pmf));
    assert(r.security.pmf.ipn[0]==9 && r.security.pmf.igtk[0][15]==0x88 && h.installs==2);
}
static void network_security_queries() {
    NetworkSecurityFixture h;auto &r=h.f.runtime;network_security_rsn(h);
    auto unprotected=h.header(0xc0,26);unprotected[24]=7;h.management(unprotected);
    assert(r.security.count==1 && !r.security.pmf.query_active && r.station.phase==GWT_RUN);
    unprotected[24]=8;h.work();assert(!r.security.count && r.security.pmf.query_active && r.dp.authorized);
    auto tx=h.transmitted();assert(tx.size()==44 && tx[0]==0xd0 && (tx[1]&0x40) && tx[24]==1 && tx[27]==0x20 && tx[32]==8 && tx[33]==0);
    for(unsigned i=36;i<44;i++)assert(!tx[i]);
    uint8_t transaction[2];memcpy(transaction,r.security.pmf.transaction,2);
    auto response=h.protected_action(5,1,transaction);h.management(response,8);h.work();
    assert(r.security.pmf.query_active && !r.security.pmf.rx_pn);
    h.management(response,0,5180);assert(!r.security.count);h.management(response);
    assert(r.security.pmf.query_active && !r.security.pmf.rx_pn);h.work();
    assert(!r.security.pmf.query_active && r.security.pmf.rx_pn==5 && r.station.phase==GWT_RUN);
    h.management(response);h.work();assert(r.security.pmf.rx_pn==5 && r.dp.authorized);
    uint8_t peer_transaction[2]={0xab,0xcd};auto request=h.protected_action(6,0,peer_transaction);
    h.management(request);h.work();assert(r.security.pmf.pending && r.station.tx_pending && r.security.pmf.tx_pn==1);
    h.tx_ack();h.work();tx=h.transmitted();assert(tx.size()==44 && tx[24]==2 && tx[33]==1 && !memcmp(tx.data()+34,peer_transaction,2));h.tx_ack();
    unprotected[24]=6;h.management(unprotected);h.work();assert(r.security.pmf.query_active);h.tx_ack();
    runtimeNow+=1001;h.work();assert(r.station.phase==GWT_STOPPING && !r.dp.authorized && r.station.error==GWT_TIMEOUT);
    h.work();assert(!gwn_nonzero((const uint8_t *)&r.security.sae,sizeof(r.security.sae)) && !r.security.count && !gwn_nonzero(r.security.ptk,16));
    IOLockLock(&h.f.lock);gn_disable(&r);IOLockUnlock(&h.f.lock);
    assert(!gwn_nonzero((const uint8_t *)&r.security,sizeof(r.security)) && !gwn_nonzero((const uint8_t *)&r.rsn,sizeof(r.rsn)));
}
static void network_security_bip() {
    NetworkSecurityFixture h;auto &r=h.f.runtime;network_security_rsn(h);
    auto frame=h.header(0xc0,44);memset(frame.data()+4,255,6);frame[24]=8;frame[26]=76;frame[27]=16;frame[28]=4;frame[30]=10;
    uint8_t aad[40]={0},mic[16];aad[0]=frame[0];memcpy(aad+2,frame.data()+4,18);memcpy(aad+20,frame.data()+24,20);
    network_security_cmac(r.security.pmf.igtk[0],aad,sizeof(aad),mic);memcpy(frame.data()+36,mic,8);
    frame[36]^=1;h.management(frame);h.work();assert(r.station.phase==GWT_RUN && r.security.pmf.ipn[0]==9);
    frame[36]^=1;h.management(frame);assert(r.station.phase==GWT_RUN);h.work();
    assert(r.station.phase==GWT_STOPPING && r.station.reason==8 && !r.dp.authorized && r.security.pmf.ipn[0]==10);
    auto tx=h.transmitted();assert(tx.size()==42 && tx[0]==0xc0 && (tx[1]&0x40) && tx[32]==8);
}
struct NetworkSaePeer {
    gsa_state sae{};std::vector<uint8_t> body;unsigned random=0;
};
static int network_sae_random(void *v,uint8_t *p,size_t n) {
    auto *a=(NetworkSaePeer *)v;memset(p,(int)(0x21+ ++a->random),n);return 0;
}
static int network_sae_send(void *v,const uint8_t *p,size_t n) {
    auto *a=(NetworkSaePeer *)v;a->body.assign(p,p+n);return 0;
}
static void network_security_sae(unsigned h2e) {
    NetworkSecurityFixture h;auto &r=h.f.runtime;
    r.join_request.pmf=r.station.request.pmf=2;r.join_request.security=r.station.request.security=GWT_WPA3_SAE;
    r.join_request.h2e=r.station.request.h2e=h2e;r.station.request.bss.ie[19]=8;r.station.request.bss.ie[20]=0xc0;
    if(h2e){unsigned at=r.station.request.bss.ie_bytes;r.station.request.bss.ie[at++]=244;r.station.request.bss.ie[at++]=1;r.station.request.bss.ie[at++]=0x20;r.station.request.bss.ie_bytes=(uint16_t)at;}
    r.join_request.bss=r.station.request.bss;r.station.phase=GWT_AUTH_WAIT;r.station.deadline=runtimeNow+30000;
    r.join={};r.join.security=GWT_WPA3_SAE;r.join.password_bytes=8;memcpy(r.join.password,"password",8);
    r.join.ssid_bytes=3;memcpy(r.join.ssid,"lab",3);assert(!gns_prepare(&r));
    NetworkSaePeer ap;gsa_ops ops={&ap,network_sae_random,network_sae_send,gw_now};
    assert(!gsa_init(&ap.sae,&ops,r.station.request.bss.bssid,r.mac,(const uint8_t *)"password",8,(const uint8_t *)"lab",3,h2e));
    assert(!gsa_start(&ap.sae));auto peer_commit=ap.body;
    IOLockLock(&h.f.lock);assert(!gns_auth_start(&r));IOLockUnlock(&h.f.lock);
    assert(!r.join.password_bytes && !gwn_nonzero(r.join.password,sizeof(r.join.password)) && !r.join.has_pmk);
    auto local_commit=h.transmitted();assert(local_commit.size()==128 && local_commit[0]==0xb0);
    assert(!gsa_receive(&ap.sae,r.mac,local_commit.data()+24,local_commit.size()-24));auto peer_confirm=ap.body;
    auto commit=h.header(0xb0,24+peer_commit.size());memcpy(commit.data()+24,peer_commit.data(),peer_commit.size());
    h.management(commit);assert(r.security.count==1 && r.security.sae.phase==GSA_COMMITTED);
    h.work();assert(r.security.sae.phase==GSA_CONFIRMED && r.security.sae.pending && !r.join.has_pmk);
    h.tx_ack();h.work();auto local_confirm=h.transmitted();assert(local_confirm.size()==64 && !r.security.sae.pending);
    assert(!gsa_receive(&ap.sae,r.mac,local_confirm.data()+24,local_confirm.size()-24) && ap.sae.phase==GSA_ACCEPTED);
    auto confirm=h.header(0xb0,24+peer_confirm.size());memcpy(confirm.data()+24,peer_confirm.data(),peer_confirm.size());
    confirm.back()^=1;h.management(confirm);h.work();assert(!r.join.has_pmk && !r.station.auth_confirm);
    confirm.back()^=1;h.management(confirm);assert(!r.join.has_pmk);h.work();
    assert(r.security.sae.phase==GSA_ACCEPTED && r.join.has_pmk && r.station.auth_confirm && r.station.request.has_pmkid);
    assert(!memcmp(r.join.pmk,ap.sae.pmk,32) && !memcmp(r.station.request.pmkid,ap.sae.pmkid,16));
    h.tx_ack();h.work();assert(r.station.phase==GWT_ASSOC_WAIT);
    auto association=h.transmitted();if(h2e)assert(association[association.size()-3]==244 && association.back()==0x20);
    h.tx_ack();h.work();
    assert(!gwn_nonzero((const uint8_t *)&r.security.sae,sizeof(r.security.sae)) && r.join.has_pmk);
    auto association_reply=h.header(0x10,30);gwq_w16(association_reply.data()+24,0x11);gwq_w16(association_reply.data()+28,1);
    h.management(association_reply);h.work();assert(r.station.phase==GWT_ASSOC_CONFIRM);
    h.association_ack();h.work();assert(r.rsn_started && r.rsn.security.sae && r.rsn.security.h2e==h2e);
    assert(!r.join.has_pmk && !memcmp(r.rsn.pmk,ap.sae.pmk,32));
    auto m1=network_rsn_m1(1);h.receive(m1);h.work();assert(r.rsn.phase==GWR_WAIT_M1);
    gwr_w16(m1.data()+5,0x88);h.receive(m1);h.work();assert(r.rsn.phase==GWR_WAIT_M3);
    assert(!(gwr_be16(r.rsn.last_tx+5)&7));
    if(h2e){h.receive(network_security_key(r.rsn,2,1,1,0));h.work();assert(r.rsn.phase==GWR_WAIT_M3 && !h.installs);}
    h.receive(network_security_key(r.rsn,2,1,1));h.work();assert(r.rsn.phase==GWR_PTK_WAIT && !r.security.pmf.ptk_valid);
    h.key_ack(0,1);h.work();h.work();h.key_ack(1,0);h.work();
    assert(r.rsn.phase==GWR_RUN && r.station.phase==GWT_RUN && r.dp.authorized && gpm_ready(&r.security.pmf));
    auto m4=std::vector<uint8_t>(r.rsn.last_tx,r.rsn.last_tx+r.rsn.last_tx_bytes);uint8_t mic[16];memset(m4.data()+81,0,16);
    network_security_cmac(r.rsn.ptk,m4.data(),m4.size(),mic);assert(!memcmp(mic,r.rsn.last_tx+81,16));
    IOLockLock(&h.f.lock);gn_disable(&r);IOLockUnlock(&h.f.lock);gsa_clear(&ap.sae);
    assert(!gwn_nonzero((const uint8_t *)&r.security,sizeof(r.security)) && !gwn_nonzero((const uint8_t *)&r.join,sizeof(r.join)));
}
static void network_security_bridge() {
    network_security_queries();network_security_bip();network_security_sae(0);network_security_sae(1);
    puts("Actual security bridge: queued WMI management, SAE H&P/H2E through association and CCMP authorization, PMK/PMKID and RSNXE binding, firmware PTK ACK/IGTK gates, protected SA Query TX/RX, BIP MIC and secret cleanup passed");
}
