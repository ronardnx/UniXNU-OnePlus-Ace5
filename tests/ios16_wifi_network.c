#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/giulia_wifi_network.h"

static void disconnected_getters(void) {
    /* gn_query passes its zero-initialized station BSS even before a join.
     * Global getters must not require a channel or a cached scan result. */
    struct gww_bss peer={0};struct gwn_view view={1,0,0,0,0,0,0,&peer,0,0,0,0};
    const unsigned global[]={GWI_CAPABILITIES,GWI_STATE,GWI_PHY_MODE,
        GWI_ASSOCIATE_RESULT,GWI_ASSOCIATION_STATUS,GWI_ASSOCIATE_RESULT_EXTENDED};
    uint8_t input[52]={0},output[53];int handled;size_t bytes;
    struct gwi_request q={0,0,0,input,0};
    for(unsigned i=0;i<sizeof(global)/sizeof(global[0]);i++) {
        q.type=global[i];q.bytes=gwi_scalar(q.type)?0:gwi_get_bytes(q.type);
        memset(output,0xa5,sizeof(output));bytes=sizeof(output);
        assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_OK && handled);
        assert(bytes==gwi_get_bytes(q.type) && output[bytes]==0xa5);
        if(q.type==GWI_PHY_MODE)assert(gwq_u32(output)==1 && gwq_u32(output+4)==0x0e && !gwq_u32(output+8));
    }
    const unsigned connected[]={GWI_SSID,GWI_CHANNEL,GWI_BSSID,GWI_RSSI,GWI_NOISE,GWN_AUTH_TYPE};
    for(unsigned i=0;i<sizeof(connected)/sizeof(connected[0]);i++) {
        q.type=connected[i];q.bytes=q.type==GWN_AUTH_TYPE?16:gwi_get_bytes(q.type);
        memset(output,0xa5,sizeof(output));bytes=sizeof(output);
        assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_NOT_READY && handled && !bytes && output[0]==0xa5);
    }
    /* An associated but malformed peer is still rejected before output. */
    view.associated=1;q.type=GWI_PHY_MODE;q.bytes=12;bytes=sizeof(output);
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_ARGUMENT && !bytes && output[0]==0xa5);
    view.peer=NULL;bytes=sizeof(output);
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_NOT_READY && !bytes && output[0]==0xa5);
    view.peer=&peer;peer.frequency=2412;peer.ssid_len=33;bytes=sizeof(output);
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_ARGUMENT && !bytes && output[0]==0xa5);
    peer.ssid_len=3;bytes=sizeof(output);
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_ARGUMENT && !bytes);
    peer.rate_count=1;peer.rates[0]=0x82;bytes=sizeof(output);
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_OK && bytes==12 && gwq_u32(output+8)==4);
    peer.rate_count=2;peer.rates[1]=0x7f;bytes=sizeof(output);
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_OK && gwq_u32(output+8)==4);
    peer.rates[1]=0x8c;bytes=sizeof(output);
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_OK && gwq_u32(output+8)==8);
    peer.rate_count=17;bytes=sizeof(output);memset(output,0xa5,sizeof(output));
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_ARGUMENT && !bytes && output[0]==0xa5);
    peer.rate_count=1;peer.rates[0]=0x8c;
    peer.frequency=5180;bytes=sizeof(output);
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_OK && gwq_u32(output+8)==2);
    q.data=NULL;bytes=sizeof(output);memset(output,0xa5,sizeof(output));
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_ARGUMENT && !bytes && output[0]==0xa5);
}

static void scan_rejected(const struct gwi_request *q,int status) {
    struct gww_scan_request out;memset(&out,0xa5,sizeof(out));
    struct gww_scan_request before=out;
    assert(gwn_scan_decode(q,&out)==status && !memcmp(&before,&out,sizeof(out)));
}

static void scan_mapping(void) {
    /* Fixed offsets/strides come from 20H392 __getScanData, including the
     * last of 400 channels and the last of ten 56-byte SSID records. */
    uint8_t wire[GWI_SCAN_REQUEST_BYTES]={0};
    struct gwi_request q={1,GWI_SCAN_REQUEST,0,wire,sizeof(wire)};
    struct gww_scan_request out;scan_rejected(&q,GWI_ARGUMENT);
    gwq_w32(wire,1);gwq_w32(wire+0x34,1);
    gwq_w32(wire+0x10,3);memcpy(wire+0x14,"lab",3);wire[8]=2;wire[13]=7;
    gwq_w32(wire+0x44,3);
    for(unsigned i=0;i<3;i++) {
        uint8_t *c=wire+0x48+12*i;gwq_w32(c,1);
        gwq_w32(c+4,i?36:1);gwq_w32(c+8,i?16:8);
    }
    assert(gwn_scan_decode(&q,&out)==GWI_OK && out.active && out.ssid_count==1);
    assert(out.ssids[0].length==3 && !memcmp(out.ssids[0].bytes,"lab",3));
    assert(out.bssid_count==1 && !memcmp(out.bssids[0],wire+8,6));
    assert(out.frequency_count==2 && out.frequencies[0]==2412 && out.frequencies[1]==5180);
    struct gww_scan_request snapshot=out;memset(wire,0,sizeof(wire));
    assert(!memcmp(&snapshot,&out,sizeof(out))); /* no pointers into the request */
    gwq_w32(wire,1);gwq_w32(wire+0x34,3);gwq_w32(wire+0x1314,10);
    for(unsigned i=0;i<10;i++) {
        uint8_t *s=wire+0x1318+56*i;gwq_w32(s,1);gwq_w32(s+4,32);memset(s+8,(int)i+1,32);
    }
    assert(gwn_scan_decode(&q,&out)==GWI_OK && !out.active && out.ssid_count==10);
    assert(out.ssids[9].length==32 && out.ssids[9].bytes[31]==10);
    /* The framework preserves an empty network SSID as a wildcard. Duplicate
     * wildcards and directed names must not fail later in the WMI worker. */
    gwq_w32(wire+0x1314,4);
    for(unsigned i=0;i<4;i++) {
        uint8_t *s=wire+0x1318+56*i;gwq_w32(s+4,i<2?0:3);memcpy(s+8,"lab",3);
    }
    assert(gwn_scan_decode(&q,&out)==GWI_OK && out.ssid_count==2);
    assert(!out.ssids[0].length && out.ssids[1].length==3 && !memcmp(out.ssids[1].bytes,"lab",3));
    gwq_w32(wire+0x1314,0);gwq_w32(wire+0x44,400);
    for(unsigned i=0;i<400;i++) {
        uint8_t *c=wire+0x48+12*i;gwq_w32(c,1);gwq_w32(c+4,1);gwq_w32(c+8,8);
    }
    assert(gwn_scan_decode(&q,&out)==GWI_OK && out.frequency_count==1 && out.frequencies[0]==2412);
    gwq_w32(wire+0x48+399*12,0);scan_rejected(&q,GWI_ARGUMENT);
    gwq_w32(wire+0x48+399*12,1);gwq_w32(wire+0x44,401);scan_rejected(&q,GWI_ARGUMENT);
    gwq_w32(wire+0x44,0);gwq_w32(wire+0x1314,11);scan_rejected(&q,GWI_ARGUMENT);
    gwq_w32(wire+0x1314,1);gwq_w32(wire+0x1318+4,33);scan_rejected(&q,GWI_ARGUMENT);
    gwq_w32(wire+0x1318+4,3);gwq_w32(wire+0x10,3);scan_rejected(&q,GWI_ARGUMENT);
    gwq_w32(wire+0x1314,0);gwq_w32(wire+0x10,33);scan_rejected(&q,GWI_ARGUMENT);
    gwq_w32(wire+0x10,0);gwq_w32(wire+0x34,5);scan_rejected(&q,GWI_ARGUMENT);
    gwq_w32(wire+0x34,1);wire[8]=1;scan_rejected(&q,GWI_ARGUMENT);
    memset(wire+8,255,6);assert(gwn_scan_decode(&q,&out)==GWI_OK && !out.bssid_count);
    gwq_w32(wire+0x44,1);gwq_w32(wire+0x50,24);scan_rejected(&q,GWI_UNSUPPORTED);
    gwq_w32(wire+0x50,8);gwq_w32(wire+0x4c,14);scan_rejected(&q,GWI_UNSUPPORTED);
    gwq_w32(wire+0x44,GWW_SCAN_FREQ_MAX+1);
    for(unsigned i=0;i<GWW_SCAN_FREQ_MAX+1;i++) {
        uint8_t *c=wire+0x48+12*i;gwq_w32(c,1);gwq_w32(c+4,36+i);gwq_w32(c+8,16);
    }
    scan_rejected(&q,GWI_ARGUMENT);
    q.bytes--;scan_rejected(&q,GWI_ARGUMENT);q.bytes++;q.data=NULL;scan_rejected(&q,GWI_ARGUMENT);
}

static void disassociate_mapping(void) {
    uint8_t payload[GWN_DISASSOCIATE_BYTES];memset(payload,0xaa,sizeof(payload));
    struct gwi_request q={1,GWN_DISASSOCIATE,0,NULL,0};unsigned reason=99;
    assert(gwn_disassociate_decode(&q,&reason)==GWI_OK && !reason);
    q.value=65535;assert(gwn_disassociate_decode(&q,&reason)==GWI_OK && reason==65535);
    q.value=65536;assert(gwn_disassociate_decode(&q,&reason)==GWI_ARGUMENT && reason==65535);
    q.data=payload;q.bytes=sizeof(payload);gwq_w32(payload+4,8);
    assert(gwn_disassociate_decode(&q,&reason)==GWI_OK && reason==8);
    /* The framework's unused first word is not an ABI version. */
    gwq_w32(payload,0);gwq_w32(payload+4,0);
    assert(gwn_disassociate_decode(&q,&reason)==GWI_OK && !reason);
    gwq_w32(payload+4,65536);reason=99;
    assert(gwn_disassociate_decode(&q,&reason)==GWI_ARGUMENT && reason==99);
    q.bytes--;assert(gwn_disassociate_decode(&q,&reason)==GWI_ARGUMENT && reason==99);
    q.bytes+=2;assert(gwn_disassociate_decode(&q,&reason)==GWI_ARGUMENT && reason==99);
    q.bytes=sizeof(payload);q.data=NULL;
    assert(gwn_disassociate_decode(&q,&reason)==GWI_ARGUMENT && reason==99);
    q.bytes=0;q.value=3;q.set=0;
    assert(gwn_disassociate_decode(&q,&reason)==GWI_ARGUMENT && reason==99);
    q.set=1;q.type=GWN_ASSOCIATE;
    assert(gwn_disassociate_decode(&q,&reason)==GWI_ARGUMENT && reason==99);
}

static void extended_association_result(void) {
    struct gwn_view view={1,0,0,0,0,0,17,NULL,0,0,0,0};
    uint8_t output[5];struct gwi_request q={0,GWI_ASSOCIATE_RESULT_EXTENDED,0,NULL,0};
    const unsigned results[]={GWN_RESULT_PENDING,GWN_RESULT_SUCCESS,GWN_RESULT_FAILURE,
        GWN_RESULT_TIMEOUT,GWN_RESULT_RSN_TIMEOUT};
    for(unsigned i=0;i<sizeof(results)/sizeof(results[0]);i++) {
        view.result=results[i];memset(output,0xa5,sizeof(output));size_t bytes=sizeof(output);int handled;
        assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_OK && handled && bytes==4);
        assert(gwq_u32(output)==results[i] && output[4]==0xa5); /* status 17 is a different domain */
        memset(output,0xa5,sizeof(output));bytes=3;
        assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_ARGUMENT && !bytes && output[0]==0xa5);
        q.bytes=4;q.data=output;bytes=sizeof(output);
        assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_ARGUMENT && !bytes && output[0]==0xa5);
        q.bytes=0;q.data=NULL;
    }
}

static void current_network_mapping(void) {
    struct gww_bss peer={0};peer.frequency=2412;peer.ssid_len=3;memcpy(peer.ssid,"lab",3);
    peer.bssid[0]=2;peer.bssid[5]=7;peer.rssi=-54;peer.noise=-127;peer.beacon=100;
    peer.rate_count=1;peer.rates[0]=0x82;peer.ie_bytes=5;memcpy(peer.ie,"\0\3lab",5);peer.timestamp=75;
    struct gwn_view view={1,4,1,1,GWT_OPEN,GWN_RESULT_SUCCESS,0,&peer,100,0,0,0};
    uint8_t input[GWI_SCAN_RESULT_BYTES]={0},output[GWI_SCAN_RESULT_BYTES+1],before[GWI_SCAN_RESULT_BYTES+1];
    struct gwi_request q={0,GWI_CURRENT_NETWORK,0,input,sizeof(input)};size_t bytes=sizeof(output);int handled;
    memset(output,0xa5,sizeof(output));
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_OK && handled && bytes==GWI_SCAN_RESULT_BYTES);
    assert(gwq_u32(output)==1 && gwq_u32(output+8)==1 && !memcmp(output+0x1c,peer.bssid,6));
    assert(output[0x22]==1 && gwq_u32(output+0x24)==1); /* 0x82 on air -> 1 Mbps */
    assert(output[0x60]==3 && !memcmp(output+0x61,"lab",3) && gwq_u32(output+0x84)==25);
    assert(gwq_u16(output+0x8a)==5 && !memcmp(output+0x8c,peer.ie,5) && output[bytes]==0xa5);
    memset(output,0xa5,sizeof(output));memcpy(before,output,sizeof(output));bytes=GWI_SCAN_RESULT_BYTES-1;
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_ARGUMENT && !bytes && !memcmp(output,before,sizeof(output)));
    q.bytes--;bytes=sizeof(output);
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_ARGUMENT && !bytes && !memcmp(output,before,sizeof(output)));
    q.bytes++;view.associated=0;bytes=sizeof(output);
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_NOT_READY && !bytes && !memcmp(output,before,sizeof(output)));
    view.associated=1;view.peer=NULL;bytes=sizeof(output);
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_NOT_READY && !bytes && !memcmp(output,before,sizeof(output)));
    view.peer=&peer;peer.rate_count=17;bytes=sizeof(output);
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_ARGUMENT && !bytes && !memcmp(output,before,sizeof(output)));
    peer.rate_count=1;peer.ie_bytes=GWW_IE_MAX+1;bytes=sizeof(output);
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_ARGUMENT && !bytes && !memcmp(output,before,sizeof(output)));
    peer.ie_bytes=5;q.data=NULL;bytes=sizeof(output);
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_ARGUMENT && !bytes && !memcmp(output,before,sizeof(output)));
}

static void powersave_mapping(void) {
    struct gwn_view view={1,0,0,0,0,0,0,NULL,0,0,0,0};
    struct gwi_request q={0,GWI_POWERSAVE,0,NULL,0};uint8_t output[5];int handled;
    memset(output,0xa5,sizeof(output));size_t bytes=sizeof(output);
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_NOT_READY && handled && !bytes && output[0]==0xa5);
    view.active_policy=1;bytes=sizeof(output);
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_OK && bytes==4 && !gwq_u32(output) && output[4]==0xa5);
    memset(output,0xa5,sizeof(output));bytes=3;
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_ARGUMENT && !bytes && output[0]==0xa5);
    q.bytes=4;q.data=output;bytes=sizeof(output);
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_ARGUMENT && !bytes && output[0]==0xa5);
    q.bytes=0;q.data=NULL;view.active_policy=0;bytes=sizeof(output);
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_NOT_READY && !bytes && output[0]==0xa5);
}

static void rate_and_legacy_mcs(void) {
    struct gww_bss peer={0};peer.frequency=2412;peer.ssid_len=3;memcpy(peer.ssid,"lab",3);
    struct gwn_view view={1,4,0,0,GWT_OPEN,GWN_RESULT_SUCCESS,0,&peer,100,0,0,0};
    struct gwi_request q={0,GWI_RATE,0,NULL,0};uint8_t out[5];size_t bytes;int handled;
    for(unsigned i=0;i<2;i++) {
        q.type=i?GWI_MCS:GWI_RATE;bytes=sizeof(out);memset(out,0xa5,sizeof(out));
        assert(gwn_get(&view,&q,out,&bytes,&handled)==GWI_NOT_READY && handled && !bytes && out[0]==0xa5);
    }
    view.associated=1;q.type=GWI_RATE;bytes=sizeof(out);
    assert(gwn_get(&view,&q,out,&bytes,&handled)==GWI_NOT_READY && !bytes && out[0]==0xa5);
    static const unsigned kbps[]={1000,2000,5500,6000,9000,11000,12000,18000,24000,36000,48000,54000};
    static const unsigned mbps[]={1,2,5,6,9,11,12,18,24,36,48,54};
    view.tx_rate_valid=1;
    for(unsigned i=0;i<sizeof(kbps)/sizeof(kbps[0]);i++) {
        view.tx_kbps=kbps[i];bytes=sizeof(out);memset(out,0xa5,sizeof(out));
        assert(gwn_get(&view,&q,out,&bytes,&handled)==GWI_OK && bytes==4 && gwq_u32(out)==mbps[i] && out[4]==0xa5);
    }
    memset(out,0xa5,sizeof(out));bytes=3;
    assert(gwn_get(&view,&q,out,&bytes,&handled)==GWI_ARGUMENT && !bytes && out[0]==0xa5);
    bytes=sizeof(out);q.bytes=4;q.data=out;
    assert(gwn_get(&view,&q,out,&bytes,&handled)==GWI_ARGUMENT && !bytes && out[0]==0xa5);
    q.bytes=0;q.data=NULL;view.tx_kbps=0;bytes=sizeof(out);
    assert(gwn_get(&view,&q,out,&bytes,&handled)==GWI_NOT_READY && !bytes && out[0]==0xa5);
    view.tx_kbps=54000;view.tx_rate_valid=0;bytes=sizeof(out);
    assert(gwn_get(&view,&q,out,&bytes,&handled)==GWI_NOT_READY && !bytes && out[0]==0xa5);
    q.type=GWI_MCS;bytes=sizeof(out);
    assert(gwn_get(&view,&q,out,&bytes,&handled)==GWI_OK && bytes==4 && !gwq_u32(out) && out[4]==0xa5);
    /* The compatibility zero is not conditional on having an HT rate sample:
     * the actual negotiated mode is legacy, with no valid HT MCS index. */
    view.associated=0;bytes=sizeof(out);memset(out,0xa5,sizeof(out));
    assert(gwn_get(&view,&q,out,&bytes,&handled)==GWI_NOT_READY && !bytes && out[0]==0xa5);
}

int main(void) {
    disconnected_getters();scan_mapping();disassociate_mapping();extended_association_result();current_network_mapping();powersave_mapping();rate_and_legacy_mcs();
    uint8_t wire[GWN_ASSOCIATE_BYTES]={0};struct gwi_request q={1,GWN_ASSOCIATE,0,wire,sizeof(wire)};
    struct gwn_join join;memset(&join,0xa5,sizeof(join));struct gwn_join before=join;
    assert(gwn_join_decode(&q,&join)!=GWI_OK && !memcmp(&join,&before,sizeof(join)));
    gwq_w32(wire,1);gwq_w16(wire+4,2);gwq_w16(wire+6,1);gwq_w32(wire+12,3);memcpy(wire+16,"lab",3);
    assert(gwn_join_decode(&q,&join)==GWI_OK && !join.security && !join.has_pmk && !join.has_bssid);
    wire[48]=1;assert(gwn_join_decode(&q,&join)==GWI_ARGUMENT);wire[48]=2;
    assert(gwn_join_decode(&q,&join)==GWI_OK && join.has_bssid);
    gwq_w16(wire+8,8);gwq_w32(wire+56,1);gwq_w32(wire+60,32);gwq_w32(wire+64,6);
    for(unsigned i=0;i<32;i++)wire[72+i]=(uint8_t)(i+1);
    assert(gwn_join_decode(&q,&join)==GWI_OK && join.security && join.has_pmk && !memcmp(join.pmk,wire+72,32));
    gwq_w32(wire+60,8);gwq_w32(wire+64,10);gwq_w32(wire+0x1d4,32);memset(wire+0x1d8,0x31,32);
    assert(gwn_join_decode(&q,&join)==GWI_OK && join.pmk[0]==0x31 && join.pmk[31]==0x31);
    gwq_w32(wire+0x1d4,0);assert(gwn_join_decode(&q,&join)==GWI_UNSUPPORTED);
    gwq_w32(wire+60,32);gwq_w32(wire+64,6);gwq_w16(wire+8,4096);
    assert(gwn_join_decode(&q,&join)==GWI_UNSUPPORTED);
    gwq_w32(wire+60,64);gwq_w32(wire+64,10);memset(wire+72,0x61,64);
    assert(gwn_join_decode(&q,&join)==GWI_OK && join.security==GWT_WPA3_SAE && !join.has_pmk && join.password_bytes==64 && !memcmp(join.password,wire+72,64));
    gwq_w32(wire+60,65);assert(gwn_join_decode(&q,&join)==GWI_UNSUPPORTED);gwq_w32(wire+60,0);assert(gwn_join_decode(&q,&join)==GWI_UNSUPPORTED);
    gwq_w16(wire+8,8);gwq_w32(wire+60,32);gwq_w32(wire+64,6);
    gwq_w16(wire+204,257);wire[206]=48;wire[207]=255;
    assert(gwn_join_decode(&q,&join)==GWI_OK && join.rsn_bytes==257);
    wire[207]=254;assert(gwn_join_decode(&q,&join)==GWI_ARGUMENT);gwq_w16(wire+204,258);
    assert(gwn_join_decode(&q,&join)==GWI_ARGUMENT);
    q.bytes--;assert(gwn_join_decode(&q,&join)==GWI_ARGUMENT);q.bytes++;
    gwq_w16(wire+204,0);assert(gwn_join_decode(&q,&join)==GWI_OK);
    static struct gww_state w;w.bss_count=3;
    for(unsigned i=0;i<3;i++) {struct gww_bss *b=&w.bss[i];b->frequency=2412;b->bssid[0]=2;
        b->bssid[5]=(uint8_t)i;b->ssid_len=3;memcpy(b->ssid,"lab",3);b->rssi=(int16_t)(-70+10*i);b->capability=0x11;
        b->rate_count=1;b->rates[0]=0x82;}
    assert(gwn_select(&w,&join)==&w.bss[0]);join.has_bssid=0;assert(gwn_select(&w,&join)==&w.bss[2]);
    join.security=0;assert(!gwn_select(&w,&join));join.security=1;
    w.bss[2].ssid[0]='x';assert(gwn_select(&w,&join)==&w.bss[1]);w.bss_count=65;assert(!gwn_select(&w,&join));w.bss_count=3;
    struct gwt_request station;struct gww_bss candidate=w.bss[0];candidate.beacon=100;candidate.rate_count=1;candidate.rates[0]=0x82;
    const uint8_t secure_ie[31]={48,26,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,8,0xc0,0,0,0,0,15,172,6,244,1,0x20};
    memcpy(candidate.ie,secure_ie,sizeof(secure_ie));candidate.ie_bytes=sizeof(secure_ie);join.security=GWT_WPA3_SAE;join.password_bytes=1;join.password[0]='a';
    assert(gwn_station_request(&station,&join,&candidate)&&station.security==GWT_WPA3_SAE&&station.pmf==2&&station.h2e==1);
    candidate.ie[20]=0;assert(!gwn_station_request(&station,&join,&candidate));candidate.ie[20]=0xc0;
    candidate.ie[30]=0;assert(gwn_station_request(&station,&join,&candidate)&&!station.h2e);
    candidate.ie[30]=0x21;assert(!gwn_station_request(&station,&join,&candidate));candidate.ie[30]=0x20;
    join.password_bytes=0;assert(!gwn_station_request(&station,&join,&candidate));join.security=GWT_WPA2_CCMP;
    candidate.ie[19]=2;assert(gwn_station_request(&station,&join,&candidate)&&station.pmf==1&&!station.h2e);
    candidate.ie_bytes=GWW_IE_MAX+1;assert(!gwn_station_request(&station,&join,&candidate));
    struct gwn_view view={1,4,1,1,1,0,0,&w.bss[0],0,0,0,0};uint8_t output[100];size_t bytes;int handled;
    const unsigned types[]={1,4,9,12,13,14,16,17,21,50,2};
    for(unsigned i=0;i<sizeof(types)/sizeof(types[0]);i++) {
        q.set=0;q.type=types[i];q.bytes=gwi_scalar(q.type)?0:q.type==2?16:gwi_get_bytes(q.type);
        memset(output,0xa5,sizeof(output));bytes=sizeof(output);
        assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_OK && handled && bytes);
        assert(output[bytes]==0xa5);size_t expected=bytes;bytes=expected-1;
        assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_ARGUMENT && bytes==0);
    }
    q.type=GWI_CAPABILITIES;q.bytes=13;bytes=sizeof(output);
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_OK && bytes==13);
    for(unsigned i=0;i<13;i++)assert(output[i]==(i==0 || i==9?8:i==1?64:0));
    view.associated=0;q.type=GWI_BSSID;q.bytes=6;bytes=sizeof(output);
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_NOT_READY && bytes==0);
    view.available=0;bytes=sizeof(output);
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_UNSUPPORTED && !handled);
    gwn_clear(&join,sizeof(join));assert(!gwn_nonzero((uint8_t *)&join,sizeof(join)));
    puts("iOS station ABI: real 592-byte association/PMK, security rejection, peer selection, connected getters and secret clearing passed");
    return 0;
}
