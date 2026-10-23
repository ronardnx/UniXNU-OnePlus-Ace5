#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/giulia_wifi_network.h"
int main(void) {
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
        b->bssid[5]=(uint8_t)i;b->ssid_len=3;memcpy(b->ssid,"lab",3);b->rssi=(int16_t)(-70+10*i);b->capability=0x11;}
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
    struct gwn_view view={1,4,1,1,1,0,0,&w.bss[0]};uint8_t output[100];size_t bytes;int handled;
    const unsigned types[]={1,4,9,12,13,14,16,17,21,50,2};
    for(unsigned i=0;i<sizeof(types)/sizeof(types[0]);i++) {
        q.set=0;q.type=types[i];q.bytes=gwi_scalar(q.type)?0:q.type==2?16:gwi_get_bytes(q.type);
        memset(output,0xa5,sizeof(output));bytes=sizeof(output);
        assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_OK && handled && bytes);
        assert(output[bytes]==0xa5);size_t expected=bytes;bytes=expected-1;
        assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_ARGUMENT && bytes==0);
    }
    q.type=GWI_CAPABILITIES;q.bytes=13;bytes=sizeof(output);
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_OK && output[0]==8 && output[1]==64);
    view.associated=0;q.type=GWI_BSSID;q.bytes=6;bytes=sizeof(output);
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_NOT_READY && bytes==0);
    view.available=0;bytes=sizeof(output);
    assert(gwn_get(&view,&q,output,&bytes,&handled)==GWI_UNSUPPORTED && !handled);
    gwn_clear(&join,sizeof(join));assert(!gwn_nonzero((uint8_t *)&join,sizeof(join)));
    puts("iOS station ABI: real 592-byte association/PMK, security rejection, peer selection, connected getters and secret clearing passed");
    return 0;
}
