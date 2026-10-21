#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/giulia_wifi_apple.h"
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
    puts("iOS16 IO80211 ABI: distinct GET/SET envelopes, ignored user pointers, scan request and 1164-byte result boundaries passed");return 0;
}
