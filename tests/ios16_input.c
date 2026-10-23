#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static unsigned enabled=1,ready=7,count,fail_at;
struct event {unsigned page,usage,down,x,y;};
static struct event events[1024];
static int gs_enabled(void){return (int)enabled;}
static unsigned giulia_remote_ready(void){return ready;}
static int giulia_remote_touch(unsigned down,unsigned x,unsigned y)
{assert(count<1024);events[count++]=(struct event){0,10,down,x,y};return (ready&1)!=0;}
static int giulia_remote_hid(unsigned page,unsigned usage,unsigned down)
{assert(count<1024);events[count++]=(struct event){page,usage,down,0,0};return (ready&6)!=0 && count!=fail_at;}
#include "../ios16/giulia_input.inc"
int main(void)
{
    assert(!ginput_command("ping",0));
    assert(ginput_command("input.touch move 1 2",0) && !count);
    assert(ginput_command("input.touch down 65536 2",0) && !count);
    assert(ginput_command("input.touch down 1 2 junk",0) && !count);
    assert(ginput_command("input.touch down -1 2",0) && !count);
    assert(ginput_command("input.touch down 9999999999999999999999 2",0) && !count);
    assert(ginput_command("input.touch down 65535 65535",1) && count==1);
    assert(events[0].down && events[0].x==65535 && events[0].y==65535);
    assert(ginput_command("input.touch move 0 32768",2) && count==2 && ginput.touch);
    ginput_tick(3001);assert(ginput.touch);
    assert(ginput_command("input.keepalive",3001));
    ginput_tick(6000);assert(ginput.touch);
    ginput_tick(6001);assert(!ginput.touch && count==3 && !events[2].down);
    count=0;
    assert(ginput_command("input.key 4 1 3",10));
    assert(count==3 && events[0].usage==224 && events[1].usage==225 && events[2].usage==4);
    assert(ginput_command("input.button home 1",10));
    assert(events[3].page==12 && events[3].usage==0x40 && events[3].down);
    assert(ginput_command("input.button back 1",10));
    assert(events[4].page==7 && events[4].usage==41);
    assert(ginput_command("input.release",11) && !ginput_held() && count==10);
    for(unsigned i=5;i<count;i++)assert(!events[i].down);
    count=0;
    assert(ginput_command("input.key 225 1 0",0) && ginput.modifiers==2 && count==1);
    assert(ginput_command("input.key 225 0 2",0) && !ginput.modifiers && count==2 && !events[1].down);
    count=0;
    assert(ginput_command("input.key 4 2 0",0) && !count);
    assert(ginput_command("input.key 4 1 256",0) && !count);
    assert(ginput_command("input.key 0 1 0",0) && !count);
    assert(ginput_command("input.button power 2",0) && !count);
    enabled=0;
    assert(ginput_command("input.touch down 1 2",0) && !count);
    assert(ginput_command("input.key 4 1 0",0) && !count);
    enabled=1;ready=6;
    assert(ginput_command("input.touch down 1 2",0) && !count);
    assert(ginput_command("input.key 4 1 0",0) && count==1);
    assert(ginput_command("input.release",0) && count==2 && !ginput_held());
    ready=1;count=0;
    assert(ginput_command("input.key 4 1 0",0) && !count);
    assert(ginput_command("input.touch down 1 2",0) && count==1);
    assert(ginput_command("input.touch cancel 1 2",0) && count==2 && !ginput_held());
    ready=7;count=0;fail_at=2;
    assert(ginput_command("input.key 4 1 3",0) && count==3 && !ginput_held());
    assert(events[0].usage==224 && events[0].down);
    assert(events[1].usage==225 && events[1].down);
    assert(events[2].usage==224 && !events[2].down);
    puts("Remote input: strict bounds, real HID routing, modifiers, buttons, service readiness, lease and complete release passed");
    return 0;
}
