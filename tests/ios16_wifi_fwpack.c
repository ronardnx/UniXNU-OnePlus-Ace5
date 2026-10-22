#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ios16/giulia_wifi_firmware.h"
#include "../ios16/giulia_wifi_firmware_pin.h"
static void digest_matches(const void *p,size_t n,const char *hex) {
    unsigned char digest[32];gwh_sha256(p,n,digest);
    for(unsigned i=0;i<32;i++) {unsigned v;assert(sscanf(hex+i*2,"%2x",&v)==1);assert(digest[i]==v);}
}
int main(int argc,char **argv) {
    assert(argc==2);
    digest_matches("",0,"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    digest_matches("abc",3,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    digest_matches("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",56,
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    FILE *f=fopen(argv[1],"rb");assert(f);unsigned char *pack=malloc(GW_FIRMWARE_PACK_BYTES);assert(pack);
    assert(fread(pack,1,GW_FIRMWARE_PACK_BYTES,f)==GW_FIRMWARE_PACK_BYTES);fclose(f);
    struct gwf_view view;struct gwf_asset main,phy,bdf,regdb;
    assert(gwf_open(&view,pack,GW_FIRMWARE_PACK_BYTES,gw_firmware_pack_sha256));
    assert(view.count==73 && gwf_main(&view,2,&main) && main.bytes==6352960);
    assert(gwf_phy(&view,2,&phy) && phy.bytes==299660);
    assert(gwf_board(&view,2,255,&bdf) && bdf.bytes==88768);
    assert(gwf_get(&view,"kiwi/regdb.bin",&regdb));
    assert(!gwf_main(&view,3,&main) && !gwf_board(&view,0x12,255,&bdf));
    assert(!gwf_board(&view,2,0x1234,&bdf) && !gwf_get(&view,"../amss.bin",&main));
    pack[GW_FIRMWARE_PACK_BYTES-1]^=1;
    assert(!gwf_open(&view,pack,GW_FIRMWARE_PACK_BYTES,gw_firmware_pack_sha256));
    pack[GW_FIRMWARE_PACK_BYTES-1]^=1;
    unsigned char trusted[32];pack[64]='.';pack[65]='.';pack[66]='/';
    gwh_sha256(pack,GW_FIRMWARE_PACK_BYTES,trusted);
    assert(!gwf_open(&view,pack,GW_FIRMWARE_PACK_BYTES,trusted));
    free(pack);puts("Wi-Fi firmware pack: NIST SHA-256, original ROM selection, corruption and name-boundary checks passed");
}
