#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/giulia_touch_gpio.h"
static unsigned pin[4]={0,0,0,1},edges,samples,pauses,cs_changes;
static uint8_t sent[519],receive[519];
static void set(unsigned p,unsigned v)
{
    assert(p>=1 && p<=3 && v<=1);
    if(p==3 && pin[3]!=v){cs_changes++;if(v)assert(pin[2]==0);}
    if(p==2 && !pin[2] && v) {
        assert(pin[3]==0 && edges<519*8);
        sent[edges/8]=(uint8_t)((sent[edges/8]<<1)|pin[1]);edges++;
    }
    pin[p]=v;
}
static unsigned sample(void)
{
    assert(pin[2]==1 && pin[3]==0 && samples+1==edges);
    unsigned n=samples++;return (receive[n/8]>>(7-n%8))&1;
}
static void pause_us(void){pauses++;}
static void reset(void){edges=samples=pauses=cs_changes=0;memset(sent,0,sizeof(sent));}
int main(void)
{
    const struct gg_bus b={set,sample,pause_us};uint8_t tx[519],rx[519];
    assert(gg_pin_owned(0) && gg_pin_owned(0x1000) && gg_pin_owned(0x1800));
    assert(!gg_pin_owned(0x800));
    for(unsigned i=0;i<519;i++){tx[i]=(uint8_t)i;receive[i]=(uint8_t)(255-i);}
    reset();assert(!gg_transfer(&b,tx,rx,519));
    assert(!memcmp(sent,tx,519) && !memcmp(rx,receive,519));
    assert(edges==519*8 && samples==edges && pauses==2*edges+3 && cs_changes==2 && pin[3]==1 && pin[2]==0);
    reset();assert(!gg_transfer(&b,NULL,rx,2));assert(sent[0]==255 && sent[1]==255);
    reset();assert(!gg_transfer(&b,tx,NULL,2));assert(sent[0]==0 && sent[1]==1);
    reset();assert(gg_transfer(&b,tx,rx,520));assert(gg_transfer(&b,tx,rx,0));
    assert(gg_transfer(&b,NULL,NULL,1));assert(gg_transfer(NULL,tx,rx,1));
    assert(!edges && !cs_changes);
    puts("GPIO SPI mode 0: all byte values, MSB order, rising-edge sampling, dummy FF, CS lifetime and transfer bounds passed");
    return 0;
}
