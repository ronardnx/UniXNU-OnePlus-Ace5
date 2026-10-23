#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/trust/giulia_trust_catalog.h"

static void u32(uint8_t *p, uint32_t v) {
    for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i));
}
int main(void) {
    uint8_t good[132]={0}, bad[sizeof(good)], hash[20]={0};
    struct gtc_view view;
    u32(good,2);u32(good+4,12);u32(good+8,84);
    u32(good+12,2);u32(good+32,2);
    good[36]=1;good[56]=2;good[60]=3;good[80]=2;
    u32(good+84,2);u32(good+104,1);good[108]=5;good[128]=2;
    assert(gtc_validate(good,sizeof(good),&view));assert(view.entries==3);
    for (unsigned i=0;i<7;i++) {
        hash[0]=(uint8_t)i;
        assert(gtc_contains(&view,hash,20)==(i==1||i==3||i==5));
    }
    assert(!gtc_contains(&view,hash,19));assert(!gtc_contains(&view,NULL,20));
    for (size_t length=0;length<sizeof(good);length++) {
        assert(!gtc_validate(good,length,&view));
        assert(!gtc_contains(&view,hash,20));
    }
    const unsigned positions[]={0,4,8,12,32,56,84,104,128};
    for (unsigned i=0;i<sizeof(positions)/sizeof(positions[0]);i++) {
        memcpy(bad,good,sizeof(bad));u32(bad+positions[i],0xffffffffu);
        assert(!gtc_validate(bad,sizeof(bad),&view));
    }
    memcpy(bad,good,sizeof(bad));memcpy(bad+60,bad+36,20);
    assert(!gtc_validate(bad,sizeof(bad),&view));
    /* Cross-check parser against a real catalogue when passed on stdin. */
    uint8_t real[100000];size_t length=fread(real,1,sizeof(real),stdin);
    if (length) {
        assert(gtc_validate(real,length,&view));
        for (uint32_t i=0;i<view.modules;i++) {
            const uint8_t *module=real+gtc_u32(real+4+4*i);
            for (uint32_t j=0;j<gtc_u32(module+20);j++)
                assert(gtc_contains(&view,module+24+24*j,20));
        }
    }
    puts("Trust catalogue membership, malformed bounds and truncation tests passed");
    return 0;
}
