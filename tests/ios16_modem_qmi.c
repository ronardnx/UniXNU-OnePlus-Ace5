/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Synthetic independent byte oracles; no device IDs, captures or NV data.
 * Layout source is the pinned libqmi schema in giulia_modem_qmi.h.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/modem/giulia_modem_qmi.h"

static const uint8_t sim_reply[]={
    2,0x34,0x12,0x2f,0,0x2a,0,
    2,4,0,0,0,0,0,
    0x10,0x20,0,
    0,0,0xff,0xff,0xff,0xff,0xff,0xff,1,
    1,3,3,10,0,1,
    2,7,2,0,5,10,3,0xa0,0,1,0,2,3,10,3,3,10
};
static const uint8_t nas_reply[]={
    2,0x34,0x12,0x24,0,0x15,0,
    2,4,0,0,0,0,0,
    1,7,0,1,2,1,2,2,8,12,
    0x10,1,0,1
};
static const uint8_t start_reply[]={
    2,0x34,0x12,0x20,0,14,0,
    2,4,0,0,0,0,0,
    1,4,0,0x78,0x56,0x34,0x12
};
static const uint8_t stop_reply[]={
    2,0x34,0x12,0x21,0,7,0,2,4,0,0,0,0,0
};
static const uint8_t ipv4_reply[]={
    2,0x34,0x12,0x2d,0,0x35,0,
    2,4,0,0,0,0,0,
    0x1e,4,0,2,0,0,10,
    0x20,4,0,1,0,0,10,
    0x21,4,0,0,255,255,255,
    0x15,4,0,1,1,1,1,
    0x16,4,0,8,8,8,8,
    0x29,4,0,0xdc,5,0,0,
    0x2b,1,0,4
};
static void builders(void) {
    static const uint8_t uim[]={0,0x34,0x12,0x2f,0,0,0};
    static const uint8_t nas[]={0,0x34,0x12,0x24,0,0,0};
    static const uint8_t start[]={0,0x34,0x12,0x20,0,15,0,
        0x14,8,0,'i','n','t','e','r','n','e','t',0x19,1,0,4};
    static const uint8_t stop[]={0,0x34,0x12,0x21,0,7,0,
        1,4,0,0x78,0x56,0x34,0x12};
    static const uint8_t settings[]={0,0x34,0x12,0x2d,0,7,0,
        0x10,4,0,0x10,0xa3,0,0};
    static const char *bad[]={"",".","-apn","apn-","a..b","a.-b","a-.b","a.",
        "apn\n","apn user","apn/other","a_b","\xc3\xa9"};
    uint8_t b[256],old[256];char apn[103];size_t used,i,j;
    assert(gmq_build_uim_status(b,sizeof(b),0x1234,&used)==GMQ_OK);
    assert(used==sizeof(uim) && !memcmp(b,uim,used));
    assert(gmq_build_nas_serving(b,sizeof(b),0x1234,&used)==GMQ_OK);
    assert(used==sizeof(nas) && !memcmp(b,nas,used));
    assert(gmq_build_wds_start(b,sizeof(b),0x1234,"internet",&used)==GMQ_OK);
    assert(used==sizeof(start) && !memcmp(b,start,used));
    assert(gmq_build_wds_stop(b,sizeof(b),0x1234,0x12345678,&used)==GMQ_OK);
    assert(used==sizeof(stop) && !memcmp(b,stop,used));
    assert(gmq_build_wds_settings(b,sizeof(b),0x1234,&used)==GMQ_OK);
    assert(used==sizeof(settings) && !memcmp(b,settings,used));
    for(i=0;i<sizeof(start);i++) {
        memset(b,0xa5,sizeof(b));memcpy(old,b,sizeof(b));used=99;
        assert(gmq_build_wds_start(b,i,1,"internet",&used)==GMQ_LIMIT && used==0);
        assert(!memcmp(b,old,sizeof(b)));
    }
    for(i=0;i<7;i++)assert(gmq_build_uim_status(b,i,1,&used)==GMQ_LIMIT && !used);
    for(i=0;i<14;i++) {
        assert(gmq_build_wds_stop(b,i,1,1,&used)==GMQ_LIMIT && !used);
        assert(gmq_build_wds_settings(b,i,1,&used)==GMQ_LIMIT && !used);
    }
    for(i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        memset(b,0xa5,sizeof(b));memcpy(old,b,sizeof(b));
        assert(gmq_build_wds_start(b,sizeof(b),1,bad[i],&used)==GMQ_ARGUMENT && !used);
        assert(!memcmp(b,old,sizeof(b)));
    }
    memset(apn,'a',sizeof(apn));apn[63]='.';apn[100]=0;
    assert(gmq_build_wds_start(b,sizeof(b),1,apn,&used)==GMQ_OK && used==114);
    apn[100]='a';apn[101]=0;
    assert(gmq_build_wds_start(b,sizeof(b),1,apn,&used)==GMQ_ARGUMENT);
    apn[63]='a';apn[64]=0;
    assert(gmq_build_wds_start(b,sizeof(b),1,apn,&used)==GMQ_ARGUMENT);
    assert(gmq_build_wds_start(b,sizeof(b),1,NULL,&used)==GMQ_ARGUMENT);
    assert(gmq_build_wds_start(NULL,sizeof(b),1,"internet",&used)==GMQ_ARGUMENT);
    assert(gmq_build_wds_start(b,sizeof(b),1,"internet",NULL)==GMQ_ARGUMENT);
    assert(gmq_build_wds_stop(b,sizeof(b),1,0,&used)==GMQ_ARGUMENT);
    assert(gmq_build_wds_stop(b,sizeof(b),1,UINT32_MAX,&used)==GMQ_ARGUMENT);
    assert(gmq_build_uim_status(NULL,0,0,&used)==GMQ_ARGUMENT);
    assert(gmq_build_wds_settings(b,sizeof(b),0,NULL)==GMQ_ARGUMENT);
    for(j=0;j<sizeof(b);j++)b[j]=0;
    assert(gmq_build_uim_status(b,sizeof(b),0xffff,&used)==GMQ_OK && b[1]==255 && b[2]==255);
}
static void common_validation(void) {
    struct gmq_reply r;uint8_t b[80];size_t i;uint32_t handle;
    assert(gmq_response(stop_reply,sizeof(stop_reply),0x1234,0x21,&r)==GMQ_OK);
    assert(!r.remote_error && r.bytes==7);
    for(i=0;i<sizeof(stop_reply);i++)
        assert(gmq_response(stop_reply,i,0x1234,0x21,&r)!=GMQ_OK);
    assert(gmq_response(stop_reply,sizeof(stop_reply),0x1235,0x21,&r)==GMQ_TRANSACTION);
    assert(gmq_response(stop_reply,sizeof(stop_reply),0x1234,0x20,&r)==GMQ_TRANSACTION);
    memcpy(b,stop_reply,sizeof(stop_reply));b[0]=4;
    assert(gmq_response(b,sizeof(stop_reply),0x1234,0x21,&r)==GMQ_WIRE);
    b[0]=3;assert(gmq_response(b,sizeof(stop_reply),0x1234,0x21,&r)==GMQ_WIRE);
    b[0]=2;b[5]=6;assert(gmq_response(b,sizeof(stop_reply),0x1234,0x21,&r)==GMQ_WIRE);
    b[5]=7;b[10]=1;b[12]=14;
    assert(gmq_response(b,sizeof(stop_reply),0x1234,0x21,&r)==GMQ_REMOTE && r.remote_error==14);
    b[10]=2;assert(gmq_response(b,sizeof(stop_reply),0x1234,0x21,&r)==GMQ_WIRE);
    b[10]=0;assert(gmq_response(b,sizeof(stop_reply),0x1234,0x21,&r)==GMQ_WIRE);
    b[12]=0;b[8]=3;assert(gmq_response(b,sizeof(stop_reply),0x1234,0x21,&r)==GMQ_WIRE);
    b[8]=4;b[7]=3;assert(gmq_response(b,sizeof(stop_reply),0x1234,0x21,&r)==GMQ_MISSING);
    memcpy(b,stop_reply,sizeof(stop_reply));memcpy(b+14,stop_reply+7,7);b[5]=14;
    assert(gmq_response(b,21,0x1234,0x21,&r)==GMQ_WIRE); /* Duplicate result. */
    memcpy(b,stop_reply,sizeof(stop_reply));b[14]=0xef;b[15]=0;b[16]=0;b[5]=10;
    assert(gmq_response(b,17,0x1234,0x21,&r)==GMQ_OK); /* Unknown empty TLV. */
    b[17]=0xef;b[18]=0;b[19]=0;b[5]=13;
    assert(gmq_response(b,20,0x1234,0x21,&r)==GMQ_WIRE);
    b[17]=0xee;b[18]=255;b[19]=255;
    assert(gmq_response(b,20,0x1234,0x21,&r)==GMQ_WIRE);
    assert(gmq_response(NULL,0,1,1,&r)==GMQ_ARGUMENT);
    assert(gmq_response(stop_reply,sizeof(stop_reply),1,1,NULL)==GMQ_ARGUMENT);
    assert(gmq_response(stop_reply,(size_t)-1,1,1,&r)==GMQ_WIRE);
    memcpy(b,start_reply,sizeof(start_reply));b[10]=1;b[12]=14;handle=99;
    assert(gmq_parse_wds_start(b,sizeof(start_reply),0x1234,&handle)==GMQ_REMOTE && handle==99);
}
static void sim_status(void) {
    struct gmq_sim_status s,old;uint8_t b[128];size_t i;
    assert(gmq_parse_uim_status(sim_reply,sizeof(sim_reply),0x1234,&s)==GMQ_OK);
    assert(s.card_count==1 && s.cards[0].app_count==1 && s.cards[0].apps[0].type==2);
    assert(s.cards[0].apps[0].pin1_state==2 && s.cards[0].apps[0].puk1_retries==10);
    assert(gmq_sim_primary_ready(&s));
    for(i=0;i<sizeof(sim_reply);i++) {
        memset(&s,0xa5,sizeof(s));memcpy(&old,&s,sizeof(s));
        assert(gmq_parse_uim_status(sim_reply,i,0x1234,&s)!=GMQ_OK);
        assert(!memcmp(&s,&old,sizeof(s)));
    }
    /* Make outer framing valid while truncating each nested payload prefix. */
    for(i=0;i<32;i++) {
        memcpy(b,sim_reply,sizeof(sim_reply));b[5]=(uint8_t)(10+i);b[15]=(uint8_t)i;
        assert(gmq_parse_uim_status(b,17+i,0x1234,&s)!=GMQ_OK);
    }
    memcpy(b,sim_reply,sizeof(sim_reply));b[5]=43;b[15]=33;b[49]=0;
    assert(gmq_parse_uim_status(b,50,0x1234,&s)==GMQ_WIRE); /* Trailing nested byte. */
    memcpy(b,sim_reply,sizeof(sim_reply));b[33]=2;
    assert(gmq_parse_uim_status(b,sizeof(sim_reply),0x1234,&s)==GMQ_OK && !gmq_sim_primary_ready(&s));
    b[33]=7;b[42]=1;b[27]=1;
    assert(gmq_parse_uim_status(b,sizeof(sim_reply),0x1234,&s)==GMQ_OK && !gmq_sim_primary_ready(&s));
    b[27]=3;
    assert(gmq_parse_uim_status(b,sizeof(sim_reply),0x1234,&s)==GMQ_OK && gmq_sim_primary_ready(&s));
    b[42]=2;assert(gmq_parse_uim_status(b,sizeof(sim_reply),0x1234,&s)==GMQ_WIRE);
    memcpy(b,sim_reply,sizeof(sim_reply));b[38]=255;
    assert(gmq_parse_uim_status(b,sizeof(sim_reply),0x1234,&s)==GMQ_WIRE);
    memcpy(b,sim_reply,sizeof(sim_reply));b[25]=GMQ_MAX_CARDS+1;
    assert(gmq_parse_uim_status(b,sizeof(sim_reply),0x1234,&s)==GMQ_LIMIT);
    memcpy(b,sim_reply,sizeof(sim_reply));b[31]=GMQ_MAX_APPS+1;
    assert(gmq_parse_uim_status(b,sizeof(sim_reply),0x1234,&s)==GMQ_LIMIT);
    memcpy(b,sim_reply,sizeof(sim_reply));b[17]=1;
    assert(gmq_parse_uim_status(b,sizeof(sim_reply),0x1234,&s)==GMQ_WIRE); /* Bad app index. */
    b[17]=0;b[18]=1;
    assert(gmq_parse_uim_status(b,sizeof(sim_reply),0x1234,&s)==GMQ_WIRE); /* Bad slot index. */
    b[17]=255;b[18]=255;
    assert(gmq_parse_uim_status(b,sizeof(sim_reply),0x1234,&s)==GMQ_OK && !gmq_sim_primary_ready(&s));
    /* A second ready card does not make the first, locked provisioned app ready. */
    memcpy(b,sim_reply,sizeof(sim_reply));memcpy(b+49,sim_reply+26,23);
    b[5]=65;b[15]=55;b[25]=2;b[33]=2;
    assert(gmq_parse_uim_status(b,72,0x1234,&s)==GMQ_OK && !gmq_sim_primary_ready(&s));
    b[18]=1;
    assert(gmq_parse_uim_status(b,72,0x1234,&s)==GMQ_OK && gmq_sim_primary_ready(&s));
    /* An absent card and an empty card array are valid observations. */
    memcpy(b,sim_reply,sizeof(sim_reply));b[5]=25;b[15]=15;b[17]=255;b[18]=255;b[26]=0;b[31]=0;
    assert(gmq_parse_uim_status(b,32,0x1234,&s)==GMQ_OK && !gmq_sim_primary_ready(&s));
    b[5]=19;b[15]=9;b[25]=0;
    assert(gmq_parse_uim_status(b,26,0x1234,&s)==GMQ_OK && !gmq_sim_primary_ready(&s));
    assert(!gmq_sim_primary_ready(NULL));
    assert(gmq_parse_uim_status(sim_reply,sizeof(sim_reply),0x1234,NULL)==GMQ_ARGUMENT);
}
static void serving(void) {
    struct gmq_serving s,old;uint8_t b[80];size_t i;
    assert(gmq_parse_nas_serving(nas_reply,sizeof(nas_reply),0x1234,&s)==GMQ_OK);
    assert(s.registration==1 && s.ps_attach==1 && s.network==2);
    assert(s.radio_count==2 && s.radios[0]==8 && s.radios[1]==12 && s.roaming_present && s.roaming==1);
    for(i=0;i<sizeof(nas_reply);i++) {
        memset(&s,0xa5,sizeof(s));memcpy(&old,&s,sizeof(s));
        assert(gmq_parse_nas_serving(nas_reply,i,0x1234,&s)!=GMQ_OK);
        assert(!memcmp(&s,&old,sizeof(s)));
    }
    memcpy(b,nas_reply,sizeof(nas_reply));b[21]=3;
    assert(gmq_parse_nas_serving(b,sizeof(nas_reply),0x1234,&s)==GMQ_WIRE);
    memcpy(b,nas_reply,sizeof(nas_reply));b[5]=17;
    assert(gmq_parse_nas_serving(b,24,0x1234,&s)==GMQ_OK && !s.roaming_present);
    b[5]=15;b[15]=5;b[21]=0;
    assert(gmq_parse_nas_serving(b,22,0x1234,&s)==GMQ_OK && !s.radio_count);
    memcpy(b,nas_reply,sizeof(nas_reply));memcpy(b+28,nas_reply+14,10);b[5]=31;
    assert(gmq_parse_nas_serving(b,38,0x1234,&s)==GMQ_WIRE);
    assert(gmq_parse_nas_serving(nas_reply,sizeof(nas_reply),0x1234,NULL)==GMQ_ARGUMENT);
}
static void packet_data(void) {
    struct gmq_ipv4 s,old;uint8_t b[100];uint32_t handle=0;size_t i;
    assert(gmq_parse_wds_start(start_reply,sizeof(start_reply),0x1234,&handle)==GMQ_OK && handle==0x12345678);
    assert(gmq_parse_wds_stop(stop_reply,sizeof(stop_reply),0x1234)==GMQ_OK);
    assert(gmq_parse_wds_settings(ipv4_reply,sizeof(ipv4_reply),0x1234,&s)==GMQ_OK);
    assert(s.present==127 && s.address==0x0a000002 && s.gateway==0x0a000001);
    assert(s.netmask==0xffffff00 && s.dns1==0x01010101 && s.dns2==0x08080808 && s.mtu==1500 && s.family==4);
    for(i=0;i<sizeof(ipv4_reply);i++) {
        memset(&s,0xa5,sizeof(s));memcpy(&old,&s,sizeof(s));
        assert(gmq_parse_wds_settings(ipv4_reply,i,0x1234,&s)!=GMQ_OK);
        assert(!memcmp(&s,&old,sizeof(s)));
    }
    memcpy(b,ipv4_reply,sizeof(ipv4_reply));b[59]=6;
    assert(gmq_parse_wds_settings(b,sizeof(ipv4_reply),0x1234,&s)==GMQ_UNSUPPORTED);
    b[59]=4;b[15]=3;
    assert(gmq_parse_wds_settings(b,sizeof(ipv4_reply),0x1234,&s)==GMQ_WIRE);
    memcpy(b,ipv4_reply,sizeof(ipv4_reply));b[14]=0xef;
    assert(gmq_parse_wds_settings(b,sizeof(ipv4_reply),0x1234,&s)==GMQ_MISSING);
    memcpy(b,ipv4_reply,sizeof(ipv4_reply));b[5]=14;
    assert(gmq_parse_wds_settings(b,21,0x1234,&s)==GMQ_OK && s.present==GMQ_IPV4_ADDRESS && s.mtu==0);
    memcpy(b,ipv4_reply,sizeof(ipv4_reply));memcpy(b+60,ipv4_reply+14,7);b[5]=60;
    assert(gmq_parse_wds_settings(b,67,0x1234,&s)==GMQ_WIRE);
    memcpy(b,start_reply,sizeof(start_reply));memset(b+17,0,4);handle=55;
    assert(gmq_parse_wds_start(b,sizeof(start_reply),0x1234,&handle)==GMQ_WIRE && handle==55);
    memset(b+17,255,4);
    assert(gmq_parse_wds_start(b,sizeof(start_reply),0x1234,&handle)==GMQ_WIRE && handle==55);
    assert(gmq_parse_wds_start(start_reply,sizeof(start_reply),0x1234,NULL)==GMQ_ARGUMENT);
    assert(gmq_parse_wds_settings(ipv4_reply,sizeof(ipv4_reply),0x1234,NULL)==GMQ_ARGUMENT);
}
/* Deterministic malformed-input stress; also useful under ASan/UBSan. */
static void malformed_stress(void) {
    uint8_t b[128];uint32_t rng=0x8650;unsigned round,i;size_t n;
    struct gmq_sim_status sim;struct gmq_serving nas;struct gmq_ipv4 ip;uint32_t handle;
    for(round=0;round<20000;round++) {
        rng=rng*1664525u+1013904223u;n=rng%sizeof(b);
        for(i=0;i<n;i++){rng=rng*1664525u+1013904223u;b[i]=(uint8_t)(rng>>24);}
        if(n>=7){b[0]=2;gmq_w16(b+1,0x1234);gmq_w16(b+5,(uint16_t)(n-7));}
        if(n>=14){b[7]=2;b[8]=4;b[9]=0;memset(b+10,0,4);}
        if(n>=7)gmq_w16(b+3,0x2f);
        (void)gmq_parse_uim_status(b,n,0x1234,&sim);
        if(n>=7)gmq_w16(b+3,0x24);
        (void)gmq_parse_nas_serving(b,n,0x1234,&nas);
        if(n>=7)gmq_w16(b+3,0x2d);
        (void)gmq_parse_wds_settings(b,n,0x1234,&ip);
        if(n>=7)gmq_w16(b+3,0x20);
        (void)gmq_parse_wds_start(b,n,0x1234,&handle);
    }
}
int main(void) {
    builders();common_validation();sim_status();serving();packet_data();malformed_stress();
    puts("ios16 modem QMI: ok (synthetic payloads; no hardware)");return 0;
}
