#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/giulia_touch_report.h"

static void contact(uint8_t *p,unsigned id,unsigned kind,unsigned x,unsigned y)
{
    p[0]=id;p[1]=kind;p[2]=x;p[3]=x>>8;p[4]=y;p[5]=y>>8;
}
static void reject(uint8_t code,uint8_t *data,size_t size,unsigned mx,unsigned my,int ready)
{
    struct gt_state s={.active=1,.x={100},.y={200}},before=s;
    struct gt_frame out,before_out;
    memset(&out,0x5a,sizeof(out));before_out=out;
    assert(gt_decode(code,data,size,mx,my,ready,&s,&out)!=0);
    assert(!memcmp(&s,&before,sizeof(s)) && !memcmp(&out,&before_out,sizeof(out)));
}
int main(void)
{
    uint8_t config[sizeof(gt_report_config)];memcpy(config,gt_report_config,sizeof(config));
    assert(gt_config_matches(config,sizeof(config)));
    config[4]=4;assert(!gt_config_matches(config,sizeof(config)));
    assert(!gt_config_matches(NULL,0));
    assert(!gt_config_matches(gt_report_config,sizeof(gt_report_config)-1));
    uint8_t padded[128]={0};memcpy(padded,gt_report_config,sizeof(gt_report_config));
    assert(gt_config_matches(padded,sizeof(padded)));
    padded[127]=1;assert(!gt_config_matches(padded,sizeof(padded)));
    uint8_t data[61]={1};contact(data+1,0,1,20224,44480);
    struct gt_state s={0};struct gt_frame frame={0};
    assert(!gt_decode(0x11,data,7,20224,44480,1,&s,&frame));
    assert(s.active==1 && frame.count==1 && frame.events[0].down);
    assert(frame.events[0].x==1263 && frame.events[0].y==2779);
    data[0]=0;assert(!gt_decode(0x11,data,1,20224,44480,1,&s,&frame));
    assert(!s.active && frame.count==1 && !frame.events[0].down && frame.events[0].x==1263);
    data[0]=1;contact(data+1,9,2,0,0);
    assert(!gt_decode(0x11,data,7,20224,44480,1,&s,&frame));
    assert(s.active==(1<<9) && frame.count==1 && frame.events[0].id==9);
    contact(data+1,9,6,0,0); /* Palm must release a previous finger. */
    assert(!gt_decode(0x11,data,7,20224,44480,1,&s,&frame));
    assert(!s.active && frame.count==1 && !frame.events[0].down);
    for(unsigned kind=0;kind<=9;kind++) {
        if(kind==1 || kind==2 || kind==7) continue;
        contact(data+1,0,kind,0,0);
        assert(!gt_decode(0x11,data,7,20224,44480,1,&s,&frame));
        assert(!s.active && !frame.count);
    }
    data[0]=10;
    for(unsigned i=0;i<10;i++)contact(data+1+6*i,i,1,i*100,i*200);
    assert(!gt_decode(0x11,data,61,20224,44480,1,&s,&frame));
    assert(s.active==GT_MASK && frame.count==10);
    data[0]=0;assert(!gt_decode(0x11,data,1,20224,44480,1,&s,&frame));
    assert(!s.active && frame.count==10);
    data[0]=1;contact(data+1,0,1,1,1);
    for(size_t n=0;n<7;n++)reject(0x11,data,n,20224,44480,1);
    reject(0x11,data,8,20224,44480,1);
    reject(0x23,data,7,20224,44480,1); /* HBP raw frames are not touch reports. */
    reject(0x11,data,7,20224,44480,0);
    reject(0x11,data,7,0,44480,1);reject(0x11,data,7,20224,0,1);
    contact(data+1,10,1,1,1);reject(0x11,data,7,20224,44480,1);
    contact(data+1,0,7,1,1);reject(0x11,data,7,20224,44480,1);
    contact(data+1,0,1,20225,1);reject(0x11,data,7,20224,44480,1);
    contact(data+1,0,1,1,44481);reject(0x11,data,7,20224,44480,1);
    data[0]=2;contact(data+1,0,1,1,1);contact(data+7,0,1,1,1);
    reject(0x11,data,13,20224,44480,1);
    data[0]=11;reject(0x11,data,61,20224,44480,1);
    uint32_t rnd=1;
    for(unsigned n=0;n<20000;n++) {
        for(unsigned i=0;i<sizeof(data);i++){rnd=rnd*1664525+1013904223;data[i]=rnd>>24;}
        struct gt_state before=s;struct gt_frame before_frame=frame;
        int result=gt_decode(0x11,data,n%62,20224,44480,1,&s,&frame);
        if(result){assert(!memcmp(&s,&before,sizeof(s)));assert(!memcmp(&frame,&before_frame,sizeof(frame)));}
        else {assert(!(s.active&~GT_MASK));assert(frame.count<=GT_CONTACTS);}
    }
    puts("TouchCom payload decoder: contact lifecycle, geometry, malformed frames and 20000 bounded inputs passed; no hardware transport exercised");
    return 0;
}
