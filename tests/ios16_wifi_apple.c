#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/giulia_wifi_apple.h"

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
        case GWI_PHY_MODE:assert(gwq_u32(output)==1 && gwq_u32(output+4)==0x1e && !gwq_u32(output+8));break;
        case GWI_POWER:assert(gwq_u32(output)==1 && gwq_u32(output+4)==1 && gwq_u32(output+8)==1);break;
        case GWI_CHANNELS:assert(gwq_u32(output+4)==GWW_SCAN_CHANNELS);break;
        case GWI_COUNTRY:assert(!memcmp(output,"CN",3));break;
        case GWI_RADIO_INFO:assert(gwq_u32(output)==1 && gwq_u32(output+4)==2);break;
        default:break;
        }
    }
    static const unsigned unassociated[]={GWI_SSID,GWI_CHANNEL,GWI_BSSID,GWI_RSSI,GWI_NOISE,
        GWI_ASSOCIATE_RESULT,GWI_ASSOCIATION_STATUS};
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
    query_mapping();
    puts("iOS16 IO80211 ABI: envelopes, scalar GET, payload bounds, power/scan states, per-client scan generations and disconnected queries passed");return 0;
}
