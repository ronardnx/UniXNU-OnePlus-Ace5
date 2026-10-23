#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/giulia_wifi_apple.h"

static void scan_rate_mapping(void) {
    /* 20H392 __addScanResultToList compares the raw words at +0x24 with
     * integer Mbps sets {1,2,5,11} and {6,9,12,18,24,36,48,54}. They are
     * neither the on-air 500 kbps codes nor basic-rate flag bitmaps. */
    static const uint8_t air[]={0x82,0x84,0x8b,0x96,0x8c,18,24,36,48,72,96,108};
    static const unsigned mbps[]={1,2,5,11,6,9,12,18,24,36,48,54};
    struct gww_bss b={0};b.frequency=2412;b.rate_count=sizeof(air);b.ssid_len=3;
    memcpy(b.ssid,"lab",3);memcpy(b.rates,air,sizeof(air));
    b.ie[0]=1;b.ie[1]=sizeof(air);memcpy(b.ie+2,air,sizeof(air));b.ie_bytes=sizeof(air)+2;
    struct gww_bss before=b;uint8_t out[GWI_SCAN_RESULT_BYTES+1];memset(out,0xa5,sizeof(out));
    assert(gwi_result(out,GWI_SCAN_RESULT_BYTES,&b,100));
    assert(out[0x22]==sizeof(air) && out[0x60]==3 && out[GWI_SCAN_RESULT_BYTES]==0xa5);
    for(unsigned i=0;i<sizeof(air);i++)assert(gwq_u32(out+0x24+4*i)==mbps[i]);
    assert(!memcmp(out+0x8c,b.ie,b.ie_bytes) && !memcmp(&b,&before,sizeof(b)));
}

static void stock_int_mit_unsupported(void) {
    struct gww_state wmi={0};struct gwi_state state={1,1,0,&wmi};struct gwi_cursor cursor={3,7};
    struct gwi_request q={0,GWI_INT_MIT,0,NULL,0};uint8_t out[5];
    assert(gwi_scalar(GWI_INT_MIT) && gwi_get_bytes(GWI_INT_MIT)==4);
    for(unsigned set=0;set<2;set++)for(unsigned value=0;value<3;value++) {
        q.set=set;q.value=value;size_t bytes=sizeof(out);memset(out,0xa5,sizeof(out));
        assert(gwi_query(&state,&q,out,&bytes,&cursor,0)==GWI_UNSUPPORTED && !bytes && out[0]==0xa5);
        assert(state.powered && !state.scan_pending && cursor.index==3 && cursor.generation==7);
    }
}
static void bind_driver_version(void) {
    /* Apple80211Bind 20H392 clears the GET envelope, writes only command,
     * en0 and type 43, and asks IOConnectCallStructMethod for 256 output
     * bytes. It leaves the payload length at zero (0x1d3ed4ef8..4f7c). */
    uint8_t *wire=calloc(1,GWI_GET_BYTES);assert(wire);
    gwq_w32(wire,GWI_GET);memcpy(wire+0x3c08,"en0",4);gwq_w32(wire+0x3c18,GWI_DRIVER_VERSION);
    gwq_w64(wire+0x3c28,~(uint64_t)0);
    struct gwi_request q;assert(gwi_decode(wire,GWI_GET_BYTES,&q) && !q.set && !q.bytes);
    struct gww_state wmi={0};struct gwi_state state={1,1,0,&wmi};
    struct gwi_cursor cursor={3,7};uint8_t output[GWI_VERSION_BYTES+1],version[GWI_VERSION_BYTES];
    memset(output,0xa5,sizeof(output));size_t used=GWI_VERSION_BYTES-1;
    assert(gwi_query(&state,&q,output,&used,&cursor,0)==GWI_ARGUMENT && !used && output[0]==0xa5);
    used=sizeof(output);
    assert(gwi_query(&state,&q,output,&used,&cursor,0)==GWI_OK && used==GWI_VERSION_BYTES);
    assert(!strcmp((const char *)output,"Giulia WCN7850 iOS16 passive-scan 1") && output[used]==0xa5);
    assert(cursor.index==3 && cursor.generation==7 && state.powered && !state.scan_pending);
    memcpy(version,output,sizeof(version));q.bytes=GWI_VERSION_BYTES;used=sizeof(output);
    assert(gwi_query(&state,&q,output,&used,&cursor,0)==GWI_OK && !memcmp(output,version,sizeof(version)));
    q.bytes=1;used=sizeof(output);memset(output,0xa5,sizeof(output));
    assert(gwi_query(&state,&q,output,&used,&cursor,0)==GWI_ARGUMENT && !used && output[0]==0xa5);
    q.type=GWI_HARDWARE_VERSION;q.bytes=0;used=sizeof(output);
    assert(gwi_query(&state,&q,output,&used,&cursor,0)==GWI_ARGUMENT && !used && output[0]==0xa5);
    free(wire);
}

static void query_mapping(void) {
    struct gww_state wmi={0};wmi.chains=2;wmi.scan_id=7;
    struct gwi_state state={1,1,0,&wmi};struct gwi_cursor cursor={0,7};
    uint8_t input[GWI_SCAN_REQUEST_BYTES]={0},output[GWI_SCAN_REQUEST_BYTES+8];
    struct gwi_request q={0,GWI_STATE,0,NULL,0};size_t used=sizeof(output);
    memset(output,0xaa,sizeof(output));
    assert(gwi_query(&state,&q,output,&used,&cursor,100)==GWI_OK);
    assert(used==4 && gwq_u32(output)==0 && output[4]==0xaa);
    state.scan_pending=1;used=4;
    assert(gwi_query(&state,&q,output,&used,&cursor,100)==GWI_OK && gwq_u32(output)==1);
    state.scan_pending=0;wmi.scanning=1;used=4;
    assert(gwi_query(&state,&q,output,&used,&cursor,100)==GWI_OK && gwq_u32(output)==1);
    wmi.scanning=0;
    /* Scalar GET has zero request bytes and a four-byte reply. */
    q.bytes=4;q.data=input;used=4;memset(output,0xaa,sizeof(output));
    assert(gwi_query(&state,&q,output,&used,&cursor,100)==GWI_ARGUMENT && !used && output[0]==0xaa);

    static const unsigned types[]={GWI_CAPABILITIES,GWI_STATE,GWI_PHY_MODE,GWI_OP_MODE,
        GWI_POWER,GWI_CHANNELS,GWI_DRIVER_VERSION,GWI_HARDWARE_VERSION,GWI_COUNTRY,GWI_RADIO_INFO};
    for(unsigned i=0;i<sizeof(types)/sizeof(types[0]);i++) {
        q.type=types[i];size_t size=gwi_get_bytes(q.type);
        q.bytes=gwi_scalar(q.type)?0:size;q.data=q.bytes?input:NULL;
        used=size-1;memset(output,0xaa,sizeof(output));
        assert(gwi_query(&state,&q,output,&used,&cursor,100)==GWI_ARGUMENT && !used && output[0]==0xaa);
        q.bytes++;used=sizeof(output);
        assert(gwi_query(&state,&q,output,&used,&cursor,100)==GWI_ARGUMENT && !used);
        q.bytes--;used=sizeof(output);
        assert(gwi_query(&state,&q,output,&used,&cursor,100)==GWI_OK && used==size && output[size]==0xaa);
        switch(q.type) {
        case GWI_CAPABILITIES:for(size_t j=0;j<used;j++)assert(!output[j]);break;
        case GWI_PHY_MODE:assert(gwq_u32(output)==1 && gwq_u32(output+4)==0x0e && !gwq_u32(output+8));break;
        case GWI_POWER:assert(gwq_u32(output)==1 && gwq_u32(output+4)==1 && gwq_u32(output+8)==1);break;
        case GWI_CHANNELS:assert(gwq_u32(output+4)==GWW_SCAN_CHANNELS);break;
        case GWI_COUNTRY:assert(!memcmp(output,"CN",3));break;
        case GWI_RADIO_INFO:assert(gwq_u32(output)==1 && gwq_u32(output+4)==2);break;
        default:break;
        }
    }
    static const unsigned unassociated[]={GWI_SSID,GWI_CHANNEL,GWI_BSSID,GWI_RSSI,GWI_NOISE,
        GWI_ASSOCIATE_RESULT,GWI_ASSOCIATION_STATUS,GWI_CURRENT_NETWORK,GWI_ASSOCIATE_RESULT_EXTENDED,GWI_POWERSAVE,GWI_RATE,GWI_MCS};
    for(unsigned i=0;i<sizeof(unassociated)/sizeof(unassociated[0]);i++) {
        q.type=unassociated[i];q.bytes=gwi_scalar(q.type)?0:gwi_get_bytes(q.type);q.data=input;
        used=sizeof(output);memset(output,0xaa,sizeof(output));
        assert(gwi_query(&state,&q,output,&used,&cursor,100)==GWI_NOT_READY && !used && output[0]==0xaa);
    }
    q.type=0xffff;q.bytes=0;used=sizeof(output);
    assert(gwi_query(&state,&q,output,&used,&cursor,100)==GWI_UNSUPPORTED && !used);
    q.type=GWI_STATE;state.ready=0;used=sizeof(output);
    assert(gwi_query(&state,&q,output,&used,&cursor,100)==GWI_NOT_READY && !used);
    state.ready=1;

    /* Reject truncated SET replies before changing power or queueing a scan. */
    q.set=1;q.type=GWI_POWER;q.data=input;q.bytes=GWI_POWER_BYTES;
    gwq_w32(input,1);gwq_w32(input+4,1);gwq_w32(input+8,0);
    used=GWI_POWER_BYTES-1;memset(output,0xaa,sizeof(output));
    assert(gwi_query(&state,&q,output,&used,&cursor,100)==GWI_ARGUMENT && !used && state.powered==1 && output[0]==0xaa);
    used=GWI_POWER_BYTES;
    assert(gwi_query(&state,&q,NULL,&used,&cursor,100)==GWI_ARGUMENT && !used && state.powered==1);
    used=0;assert(gwi_query(&state,&q,NULL,&used,&cursor,100)==GWI_OK && !used && !state.powered);
    q.type=GWI_SCAN_REQUEST;q.bytes=GWI_SCAN_REQUEST_BYTES;used=0;
    assert(gwi_query(&state,&q,NULL,&used,&cursor,100)==GWI_NOT_READY && !state.scan_pending);
    q.type=GWI_POWER;q.bytes=GWI_POWER_BYTES;gwq_w32(input+8,1);used=sizeof(output);
    assert(gwi_query(&state,&q,output,&used,&cursor,100)==GWI_OK && used==GWI_POWER_BYTES && state.powered==1);
    assert(!memcmp(input,output,GWI_POWER_BYTES) && output[GWI_POWER_BYTES]==0);
    gwq_w32(input+8,2);used=0;
    assert(gwi_query(&state,&q,NULL,&used,&cursor,100)==GWI_ARGUMENT && state.powered==1);
    gwq_w32(input+8,0);wmi.scanning=1;used=0;
    assert(gwi_query(&state,&q,NULL,&used,&cursor,100)==GWI_BUSY && state.powered==1);
    wmi.scanning=0;
    q.type=GWI_SCAN_REQUEST;q.bytes=GWI_SCAN_REQUEST_BYTES;used=GWI_SCAN_REQUEST_BYTES-1;
    assert(gwi_query(&state,&q,output,&used,&cursor,100)==GWI_ARGUMENT && !state.scan_pending);
    cursor.index=5;used=0;
    assert(gwi_query(&state,&q,NULL,&used,&cursor,100)==GWI_OK && state.scan_pending && cursor.index==0);
    used=0;assert(gwi_query(&state,&q,NULL,&used,&cursor,100)==GWI_BUSY);
    q.type=20;used=0;assert(gwi_query(&state,&q,NULL,&used,&cursor,100)==GWI_UNSUPPORTED);

    /* Every client restarts its result cursor when another client starts a scan. */
    q.set=0;q.type=GWI_SCAN_RESULT;q.bytes=GWI_SCAN_RESULT_BYTES;used=sizeof(output);
    assert(gwi_query(&state,&q,output,&used,&cursor,100)==GWI_BUSY && !used);
    state.scan_pending=0;wmi.scan_id=8;wmi.bss_count=1;wmi.bss[0].frequency=2412;
    wmi.bss[0].timestamp=75;wmi.bss[0].ssid_len=3;memcpy(wmi.bss[0].ssid,"AP1",3);
    cursor.index=63;cursor.generation=7;used=GWI_SCAN_RESULT_BYTES-1;
    assert(gwi_query(&state,&q,output,&used,&cursor,100)==GWI_ARGUMENT && cursor.index==63 && cursor.generation==7);
    used=sizeof(output);
    assert(gwi_query(&state,&q,output,&used,&cursor,100)==GWI_OK && used==GWI_SCAN_RESULT_BYTES);
    assert(cursor.index==1 && cursor.generation==8 && gwq_u32(output+0x84)==25 && !memcmp(output+0x61,"AP1",3));
    used=sizeof(output);memset(output,0xaa,sizeof(output));
    assert(gwi_query(&state,&q,output,&used,&cursor,100)==GWI_END && !used && output[0]==0xaa);
    wmi.scan_id=9;wmi.bss[0].ie_bytes=GWW_IE_MAX+1;used=sizeof(output);
    assert(gwi_query(&state,&q,output,&used,&cursor,100)==GWI_ARGUMENT && !used && output[0]==0xaa);
    assert(cursor.index==1 && cursor.generation==8);
    wmi.bss_count=GWW_BSS_MAX+1;used=sizeof(output);
    assert(gwi_query(&state,&q,output,&used,&cursor,100)==GWI_ARGUMENT && !used);
    wmi.bss_count=0;q.data=NULL;used=sizeof(output);
    assert(gwi_query(&state,&q,output,&used,&cursor,100)==GWI_ARGUMENT && !used);
    q.bytes=0;q.type=GWI_STATE;used=GWI_SET_CAPACITY+1;
    assert(gwi_query(&state,&q,output,&used,&cursor,100)==GWI_ARGUMENT && !used);
}
int main(void){
    scan_rate_mapping();
    stock_int_mit_unsupported();
    uint8_t *p=calloc(1,GWI_SET_BYTES);assert(p);struct gwi_request r;
    gwq_w32(p,GWI_GET);memcpy(p+0x3c08,"en0",4);gwq_w32(p+0x3c18,11);gwq_w32(p+4,0x48c);
    /* Poison the unused embedded pointer: decoding must not follow it. */
    gwq_w64(p+0x3c28,~(uint64_t)0);assert(gwi_decode(p,GWI_GET_BYTES,&r)&&r.type==11&&r.bytes==0x48c&&!r.set);
    assert(!gwi_decode(p,GWI_SET_BYTES,&r));gwq_w32(p+4,0x3c01);assert(!gwi_decode(p,GWI_GET_BYTES,&r));
    memset(p,0,GWI_SET_BYTES);gwq_w32(p,GWI_SET);memcpy(p+0x3c008,"en0",4);gwq_w32(p+0x3c018,10);
    gwq_w32(p+4,GWI_SCAN_REQUEST_BYTES);gwq_w32(p+8,1);assert(gwi_decode(p,GWI_SET_BYTES,&r)&&gwi_scan_request(&r));
    p[8]=2;assert(!gwi_scan_request(&r));assert(!gwi_decode(p,GWI_GET_BYTES,&r));
    struct gww_bss b={0};b.frequency=2412;b.ssid_len=3;memcpy(b.ssid,"AP1",3);b.bssid[0]=2;b.rssi=-55;b.noise=-127;
    b.capability=0x11;b.beacon=100;b.ie_bytes=5;memcpy(b.ie,"\0\3AP1",5);b.timestamp=100;
    uint8_t result[GWI_SCAN_RESULT_BYTES+4];memset(result,0xaa,sizeof(result));
    assert(gwi_result(result,GWI_SCAN_RESULT_BYTES,&b,125));assert(gwq_u32(result)==1&&gwq_u32(result+8)==1);
    assert(result[0x60]==3&&!memcmp(result+0x61,"AP1",3)&&gwq_u16(result+0x8a)==5&&gwq_u32(result+0x84)==25);
    assert(result[GWI_SCAN_RESULT_BYTES]==0xaa);assert(!gwi_result(result,GWI_SCAN_RESULT_BYTES-1,&b,125));
    b.ie_bytes=1025;assert(!gwi_result(result,sizeof(result),&b,125));
    assert(gwi_channels(p,0x12d8));assert(gwq_u32(p+4)==22&&gwq_u32(p+12)==1&&gwq_u32(p+8+21*12+4)==165);
    assert(!(gwq_u32(p+16)&0x80));assert(!gwi_channels(p,0x12d7));free(p);
    assert(!gwi_scan_request(NULL));r.data=NULL;assert(!gwi_scan_request(&r));
    bind_driver_version();query_mapping();
    puts("iOS16 IO80211 ABI: envelopes, scalar GET, payload bounds, power/scan states, per-client scan generations and disconnected queries passed");return 0;
}
