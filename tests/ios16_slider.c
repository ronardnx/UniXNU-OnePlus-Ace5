#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/slider/giulia_slider.h"
#include "../ios16/slider/giulia_slider_pins.h"

static struct {
    unsigned low[2], queue[256], n, cursor, starts, stops, drive_count, waits;
    unsigned stretch, error, raised[256], edges,setup_waits,need_setup;
} wire;
static void low(void *ctx,unsigned pin,unsigned down)
{
    assert(ctx==&wire && pin<2 && down<2);
    if(down)wire.drive_count++;
    if(pin==GSL_SDA && !wire.low[GSL_SCL]) {
        if(!wire.low[pin] && down)wire.starts++;
        if(wire.low[pin] && !down)wire.stops++;
    }
    if(pin==GSL_SCL && wire.low[pin] && !down) {
        assert(!wire.need_setup || wire.setup_waits);
        assert(wire.edges<256);wire.raised[wire.edges++]=!wire.low[GSL_SDA];
    }
    if(pin==GSL_SDA && !down && wire.low[GSL_SCL]) {
        wire.need_setup=1;wire.setup_waits=0;
    }
    if(pin==GSL_SCL && !down)wire.need_setup=0;
    wire.low[pin]=down;
}
static int level(void *ctx,unsigned pin)
{
    assert(ctx==&wire && pin<2);
    if(wire.error)return -1;
    if(pin==GSL_SCL)return !wire.stretch;
    assert(!wire.low[GSL_SDA] && wire.cursor<wire.n);
    return (int)wire.queue[wire.cursor++];
}
static void pause_us(void *ctx,unsigned us)
{assert(ctx==&wire && us==5);wire.waits++;wire.setup_waits++;}
static const struct gsl_bus bus={&wire,low,level,pause_us};
static void push(unsigned value){assert(wire.n<256);wire.queue[wire.n++]=value;}
static void byte(unsigned value)
{for(unsigned i=0;i<8;i++)push((value>>(7-i))&1);}
static void released(void){assert(!wire.low[0] && !wire.low[1]);}
static unsigned transport(void)
{
    unsigned cases=0;uint8_t data[2]={0};
    memset(&wire,0,sizeof(wire));
    push(1);push(0);push(0);push(1);push(0);byte(0x48);byte(0xc1);
    assert(gsl_transfer(&bus,0,data,2,1)==GSL_OK);
    assert(data[0]==0x48 && data[1]==0xc1 && wire.cursor==wire.n);
    assert(wire.starts==2 && wire.stops==1);released();cases++;
    unsigned sent=0;
    for(unsigned i=0;i<8;i++)sent=(sent<<1)|wire.raised[i];
    assert(sent==0x20);cases++;
    /* Every sampled read bit releases SDA. The final read byte sends NACK. */
    assert(wire.raised[36]==0 && wire.raised[45]==1);cases++;

    memset(&wire,0,sizeof(wire));push(1);push(0);push(0);push(0);data[0]=0x2a;
    assert(gsl_transfer(&bus,0x21,data,1,0)==GSL_OK);
    assert(wire.starts==1 && wire.stops==1);released();cases++;
    for(unsigned fail=0;fail<3;fail++) {
        memset(&wire,0,sizeof(wire));push(1);
        for(unsigned i=0;i<fail;i++)push(0);
        push(1);
        assert(gsl_transfer(&bus,0x21,data,1,0)==GSL_NACK);
        assert(wire.starts==1 && wire.stops==1);released();cases++;
    }
    memset(&wire,0,sizeof(wire));push(0);
    assert(gsl_transfer(&bus,0,data,1,1)==GSL_BUS_BUSY);
    assert(!wire.drive_count && !wire.starts && !wire.stops);released();cases++;
    memset(&wire,0,sizeof(wire));wire.stretch=1;
    assert(gsl_transfer(&bus,0,data,1,1)==GSL_STRETCH);
    assert(wire.waits==201 && !wire.drive_count && !wire.stops);released();cases++;
    memset(&wire,0,sizeof(wire));wire.error=1;
    assert(gsl_transfer(&bus,0,data,1,1)==GSL_IO);
    assert(!wire.drive_count);released();cases++;
    memset(&wire,0,sizeof(wire));
    assert(gsl_transfer(&bus,0,data,0,1)==GSL_ARGUMENT);
    assert(gsl_transfer(&bus,0,data,17,1)==GSL_ARGUMENT);
    assert(gsl_transfer(&bus,256,data,1,1)==GSL_ARGUMENT);
    assert(gsl_transfer(&bus,0,data,1,2)==GSL_ARGUMENT);
    assert(gsl_transfer(0,0,data,1,1)==GSL_ARGUMENT && !wire.drive_count);cases++;
    return cases;
}

static struct {
    unsigned bad_id,writes,readback_bad,fail_write,fail_read,waits;
    uint8_t control,packet[7];
} chip;
static int read_sensor(void *ctx,unsigned reg,uint8_t *p,unsigned n)
{
    assert(ctx==&chip);
    if(chip.fail_read)return GSL_IO;
    if(reg==0) {assert(n==2);p[0]=chip.bad_id?0:0x48;p[1]=0xc1;}
    else if(reg==0x21){assert(n==1);*p=chip.readback_bad && chip.control==0x2a?0:chip.control;}
    else {assert(reg==0x17 && n==7);memcpy(p,chip.packet,7);}
    return 0;
}
static int write_sensor(void *ctx,unsigned reg,const uint8_t *p,unsigned n)
{
    assert(ctx==&chip && reg==0x21 && n==1);chip.writes++;
    chip.control=*p; /* The byte may take effect even if completion is lost. */
    return chip.fail_write?GSL_IO:0;
}
static void settle(void *ctx,unsigned us){assert(ctx==&chip && us==35000);chip.waits++;}
static const struct gsl_sensor_io sensor={&chip,read_sensor,write_sensor,settle};
static void reset_chip(struct gsl_sensor *s)
{memset(&chip,0,sizeof(chip));memset(s,0,sizeof(*s));chip.control=0x12;}
static unsigned sensor_tests(void)
{
    struct gsl_sensor s;struct gsl_vector v={123,456,789};unsigned cases=0;
    reset_chip(&s);chip.bad_id=1;
    assert(gsl_sensor_begin(&sensor,&s)==GSL_IDENTITY && !chip.writes && !s.active);cases++;
    reset_chip(&s);chip.fail_read=1;
    assert(gsl_sensor_begin(&sensor,&s)==GSL_IO && !chip.writes);cases++;
    reset_chip(&s);assert(gsl_sensor_begin(&sensor,&s)==GSL_OK);
    assert(s.previous==0x12 && s.changed && s.active && chip.control==0x2a && chip.waits==1);
    assert(gsl_sensor_begin(&sensor,&s)==GSL_ARGUMENT);cases++;
    assert(gsl_sensor_sample(&sensor,&s,&v)==GSL_NOT_READY && v.x==123);cases++;
    const uint8_t packet[]={1,0x80,0,0xff,0xff,0x7f,0xff};memcpy(chip.packet,packet,7);
    assert(gsl_sensor_sample(&sensor,&s,&v)==GSL_OK && v.x==32767 && v.y==-1 && v.z==-32768);cases++;
    assert(gsl_sensor_restore(&sensor,&s)==GSL_OK && chip.control==0x12 && !s.active && !s.changed);
    assert(gsl_sensor_sample(&sensor,&s,&v)==GSL_NOT_READY);cases++;
    reset_chip(&s);chip.fail_write=1;
    assert(gsl_sensor_begin(&sensor,&s)==GSL_IO && chip.writes==2 && !s.active);
    assert(s.changed && s.cleanup_error==GSL_IO && chip.control==0x12);cases++;
    reset_chip(&s);chip.readback_bad=1;
    assert(gsl_sensor_begin(&sensor,&s)==GSL_READBACK && chip.writes==2 && !s.active);
    assert(s.cleanup_error==0 && !s.changed);cases++;
    return cases;
}
static unsigned calibration(void)
{
    struct gsl_calibration c={{{1000,2000,3000},{0,0,0},{-1000,-2000,-3000}},{100,100,100},0};
    struct gsl_vector v={1000,2000,3000};
    assert(!gsl_position(&c,&v));c.valid=7;
    assert(gsl_calibration_valid(&c) && gsl_position(&c,&v)==1);
    v=c.centre[1];assert(gsl_position(&c,&v)==2);
    v=c.centre[2];assert(gsl_position(&c,&v)==3);
    v.x+=101;assert(gsl_position(&c,&v)==0);
    c.centre[1]=c.centre[0];assert(!gsl_calibration_valid(&c) && !gsl_position(&c,&v));
    c.centre[1]=(struct gsl_vector){0,0,0};c.radius[1]=0;assert(!gsl_calibration_valid(&c));
    c.radius[1]=100;c.centre[1].x=32768;assert(!gsl_calibration_valid(&c));
    struct gsl_filter f={0};
    assert(!gsl_filter_sample(&f,1,100));assert(!gsl_filter_sample(&f,2,150));
    assert(!gsl_filter_sample(&f,2,249));assert(gsl_filter_sample(&f,2,250)==2);
    assert(!gsl_filter_sample(&f,0,260));assert(!gsl_filter_sample(&f,1,270));
    assert(gsl_filter_sample(&f,1,370)==1);assert(!gsl_filter_sample(&f,4,380));
    return 3;
}
static uint32_t pin_regs[2][2];
static unsigned pin_writes;
static unsigned drop_latch;
static unsigned drop_release,drop_cfg;
static unsigned io_unavailable;
static uint32_t pin_read(void *ctx,unsigned p,unsigned o)
{(void)ctx;assert(p<2 && (o==0 || o==4));return o==4 && io_unavailable?0xffffffff:pin_regs[p][o/4];}
static void pin_write(void *ctx,unsigned p,unsigned o,uint32_t v)
{
    (void)ctx;assert(p<2 && (o==0 || o==4));
    if(o==0 && (v&(1u<<9)))assert(!(pin_regs[p][1]&2));
    if(o==4 && (v&2))assert(!(pin_regs[p][0]&(1u<<9)));
    if(o==4 && drop_latch){pin_writes++;return;}
    if(o==0 && drop_cfg){drop_cfg--;pin_writes++;return;}
    if(o==0 && drop_release && (pin_regs[p][0]&(1u<<9)) && !(v&(1u<<9))) {
        drop_release--;pin_writes++;return;
    }
    pin_regs[p][o/4]=v;pin_writes++;
}
static unsigned pins(void)
{
    const struct gsl_pins_io io={0,pin_read,pin_write};
    struct gsl_pins s={0};
    for(unsigned i=0;i<2;i++){pin_regs[i][0]=0x801;pin_regs[i][1]=3;}
    assert(!gsl_pins_claim(&io,&s) && s.claimed==3);
    assert(pin_regs[0][0]==0x1800 && pin_regs[0][1]==1);
    gsl_pins_low(&io,&s,0,1);assert(pin_regs[0][0]&(1u<<9));
    gsl_pins_low(&io,&s,0,0);assert(!(pin_regs[0][0]&(1u<<9)));
    assert(!gsl_pins_restore(&io,&s) && !s.claimed);
    assert(pin_regs[0][0]==0x801 && pin_regs[0][1]==3);
    memset(&s,0,sizeof(s));pin_regs[0][0]=0xa01;unsigned before=pin_writes;
    assert(gsl_pins_claim(&io,&s) && pin_writes==before && !s.claimed);
    pin_regs[0][0]=0x801;assert(!gsl_pins_claim(&io,&s));
    pin_regs[0][0]^=1u<<12;before=pin_writes;
    gsl_pins_low(&io,&s,0,1);assert(s.fault && pin_writes==before);
    assert(gsl_pins_restore(&io,&s) && (s.claimed&1));
    memset(&s,0,sizeof(s));drop_latch=1;
    for(unsigned i=0;i<2;i++){pin_regs[i][0]=0x801;pin_regs[i][1]=3;}
    assert(gsl_pins_claim(&io,&s) && s.claimed==1 && pin_regs[0][0]==0x801);
    drop_latch=0;assert(!gsl_pins_restore(&io,&s));
    memset(&s,0,sizeof(s));assert(!gsl_pins_claim(&io,&s));
    pin_regs[0][1]|=2;
    gsl_pins_low(&io,&s,0,1);
    assert(s.fault && !(pin_regs[0][0]&(1u<<9)));
    assert(!gsl_pins_restore(&io,&s));
    memset(&s,0,sizeof(s));assert(!gsl_pins_claim(&io,&s));
    gsl_pins_low(&io,&s,0,1);drop_release=1;gsl_pins_low(&io,&s,0,0);
    assert(s.fault && (s.current[0]&(1u<<9)) && (pin_regs[0][0]&(1u<<9)));
    assert(!gsl_pins_restore(&io,&s) && !s.claimed);
    assert(pin_regs[0][0]==0x801 && pin_regs[0][1]==3);
    memset(&s,0,sizeof(s));drop_cfg=1;
    assert(gsl_pins_claim(&io,&s) && s.claimed==1 && pin_regs[0][1]==1);
    assert(!gsl_pins_restore(&io,&s) && pin_regs[0][1]==3);
    memset(&s,0,sizeof(s));assert(!gsl_pins_claim(&io,&s));
    gsl_pins_low(&io,&s,0,1);drop_release=2;gsl_pins_low(&io,&s,0,0);
    assert(gsl_pins_restore(&io,&s) && (s.claimed&1) && !(pin_regs[0][1]&2));
    assert(!gsl_pins_restore(&io,&s));
    memset(&s,0,sizeof(s));assert(!gsl_pins_claim(&io,&s));drop_latch=1;
    assert(gsl_pins_restore(&io,&s) && s.claimed==3);
    drop_latch=0;assert(!gsl_pins_restore(&io,&s));
    memset(&s,0,sizeof(s));assert(!gsl_pins_claim(&io,&s));io_unavailable=1;
    assert(gsl_pins_restore(&io,&s) && s.claimed==3);
    io_unavailable=0;assert(!gsl_pins_restore(&io,&s));
    return 10;
}
int main(void)
{
    unsigned a=transport(),b=sensor_tests(),c=calibration(),d=pins();
    printf("Slider core: %u open-drain transport/fault cases, %u AK09970 transaction cases, %u calibration/debounce groups and %u GPIO lease groups passed\n",a,b,c,d);
    return 0;
}
