#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/giulia_touch_wire.h"
static uint8_t input[600],output[600];
static unsigned cursor,total,written,waits;
static int rd(uint8_t *p,unsigned n){if(cursor+n>total)return 1;memcpy(p,input+cursor,n);cursor+=n;return 0;}
static int wr(const uint8_t *p,unsigned n){memcpy(output,p,n);written=n;return 0;}
static void pause_ms(unsigned ms){assert(ms==10);waits++;}
static void prepare(uint8_t code,unsigned n){memset(input,0x5a,sizeof(input));cursor=0;total=4+(n?n+7:0);input[0]=0xa5;input[1]=code;input[2]=n;input[3]=n>>8;input[4]=0xa5;input[5]=3;}
int main(void)
{
    struct gw_bus bus={rd,wr,pause_ms,64};struct gw_message msg,old;
    memset(&msg,0x55,sizeof(msg));old=msg;
    prepare(0x10,24);input[6]=1;input[7]=1;
    assert(!gw_read(&bus,&msg) && msg.code==0x10 && msg.size==24 && msg.data[0]==1);
    for(unsigned k=0;k<5;k++) {
        msg=old;prepare(0x11,7);input[7+6+k]=0;
        assert(gw_read(&bus,&msg)==6 && !memcmp(&msg,&old,sizeof(msg)));
    }
    prepare(0x11,513);assert(gw_read(&bus,&msg)==3);
    prepare(0x11,7);input[0]=0x35;assert(gw_read(&bus,&msg)==2);
    prepare(0x11,7);input[5]=1;assert(gw_read(&bus,&msg)==5);
    prepare(0x11,7);total--;assert(gw_read(&bus,&msg)==4);
    prepare(1,0);uint8_t p[3]={0x11,2,0};
    assert(!gw_command(&bus,0x24,p,3,&msg));
    assert(written==6 && output[0]==0x24 && output[1]==3 && !memcmp(output+3,p,3) && waits==1);
    bus.max_write=5;written=0;assert(gw_command(&bus,0x24,p,3,&msg)==7 && !written);
    bus.max_write=64;prepare(0x0f,0);assert(gw_command(&bus,0x24,p,3,&msg)==9);
    puts("TouchCom wire: bounded reads, continuation, EOM/extension rejection, command framing and max-write checks passed");
    return 0;
}
