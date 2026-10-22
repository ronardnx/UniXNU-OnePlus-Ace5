#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe/WifiPcieBandwidth.h"

int main(void) {
    unsigned char aux[8]={0xe8,3,0,0,16,0,0,0};unsigned vote=0,cases=0;
    const unsigned expected[6]={GCB_VALID|(500u<<14)|3200,GCB_VALID|(125u<<14)|800,
        GCB_VALID|1,GCB_VALID|(500u<<14)|800,0,GCB_VALID|1};
    for(unsigned n=0;n<6;n++) {
        if(n==4){assert(!gcb_name(n)&&!gcb_vote(n,aux,8,&vote));}
        else{assert(gcb_name(n)&&gcb_vote(n,aux,8,&vote)&&vote==expected[n]);}
        cases++;
    }
    assert(!gcb_name(6)&&!gcb_vote(6,aux,8,&vote));cases++;
    for(unsigned len=0;len<18;len++)if(len!=8){assert(!gcb_vote(0,aux,len,&vote));cases++;}
    assert(!gcb_vote(0,0,8,&vote)&&!gcb_vote(0,aux,8,0));cases++;
    for(unsigned variant=0;variant<4;variant++) {
        unsigned char changed[8];memcpy(changed,aux,8);
        if(variant==0)memset(changed,0,4);
        if(variant==1)memset(changed+4,0,2);
        if(variant==2)changed[6]=255;
        if(variant==3)changed[7]=255;
        unsigned want=variant==0?0x2fffffffu:(variant==1?(GCB_VALID|(31u<<14)|200):expected[0]);
        assert(gcb_vote(0,changed,8,&vote)&&vote==want);cases++;
    }
    unsigned char flag_aux[8]={0};
    assert(gcb_vote(2,flag_aux,8,&vote)&&vote==(GCB_VALID|1));
    assert(gcb_vote(5,flag_aux,8,&vote)&&vote==(GCB_VALID|1));cases+=2;
    unsigned char huge[8]={1,0,0,0,255,255,0,0};
    assert(gcb_vote(0,huge,8,&vote)&&vote==0x2fffffffu);cases++;
    unsigned char tiny[8]={255,255,255,255,1,0,0,0};
    assert(gcb_vote(0,tiny,8,&vote)&&vote==(GCB_VALID|(1u<<14)|1));cases++;
    /* Raising peak bandwidth must never lower an inherited average vote. */
    assert(gcb_merge(GCB_VALID|(900u<<14)|400,expected[0],0)==(GCB_VALID|(900u<<14)|3200));cases++;
    assert(gcb_merge(GCB_VALID|(200u<<14)|4000,expected[0],0)==(GCB_VALID|(500u<<14)|4000));cases++;
    assert(gcb_merge(GCB_VALID|4,expected[2],2)==(GCB_VALID|5));cases++;
    assert(gcb_merge(GCB_VALID|8,expected[5],5)==(GCB_VALID|9));cases++;
    assert(gcb_merge(0x20012345u,0,4)==0x20012345u);cases++;
    printf("PCIe bandwidth: %u topology, units, bounds, saturation and preserved-floor cases passed\n",cases);
    return 0;
}
