#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/terminal/giulia_screen.h"
static struct {unsigned mps;} u={512};
static uint64_t fake_ms;
static unsigned captures,enabled=1,available=1;
static int gs_enabled(void){return (int)enabled;}
static uint64_t mach_absolute_time(void){return fake_ms*1000000;}
static void absolutetime_to_nanoseconds(uint64_t x,uint64_t *out){*out=x;}
static int giulia_screen_capture_size(uint8_t *out,size_t n,unsigned *width,unsigned *height,unsigned max_size)
{
    if(!available)return 0;
    assert(n==GSC_MAX_FRAME_BYTES);captures++;*width=GSC_MAX_WIDTH*max_size/GSC_MAX_HEIGHT;*height=max_size;
    for(unsigned i=0;i<*width * *height * 2;i++)out[i]=(uint8_t)(i*17u+23u);
    return 1;
}
static unsigned giulia_remote_ready(void){return 0;}
static int giulia_remote_touch(unsigned d,unsigned x,unsigned y){(void)d;(void)x;(void)y;return 0;}
static int giulia_remote_hid(unsigned p,unsigned u,unsigned d){(void)p;(void)u;(void)d;return 0;}
#include "../ios16/terminal/giulia_input.inc"
#include "../ios16/terminal/giulia_screen.inc"
static uint32_t le32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
int main(int argc,char **argv)
{
    assert(gsc_crc("123456789",9)==0xcbf43926u);
    static uint8_t frame[GSC_FRAME_BYTES],copy[GSC_FRAME_BYTES],buffer[2050];
    for(unsigned i=0;i<GSC_FRAME_BYTES;i++)frame[i]=(uint8_t)(i*17u+23u);
    uint32_t frame_crc=gsc_crc(frame,sizeof(frame));
    for(unsigned mps=64;mps<=512;mps*=8) {
        uint32_t offset=0;memset(buffer,0xa5,sizeof(buffer));
        FILE *fixture=argc>1 && mps==512?fopen(argv[1],"wb"):NULL;
        if(fixture)fputs("kernel before\n",fixture);
        while(offset<GSC_FRAME_BYTES) {
            unsigned n=gsc_packet(buffer+1,2047,mps,frame,7,0,offset,frame_crc);
            assert(n>GSC_HEADER_BYTES && n<=2047 && n%mps);
            assert(buffer[0]==0xa5 && buffer[2048]==0xa5 && buffer[2049]==0xa5);
            const uint8_t *p=buffer+1;unsigned count=le32(p+28);
            assert(!memcmp(p,gsc_magic,8) && le32(p+8)==7 && le32(p+12)==0);
            assert(p[16]==60 && p[17]==1 && p[18]==183 && p[19]==2);
            assert(le32(p+20)==GSC_FRAME_BYTES && le32(p+24)==offset && count==n-GSC_HEADER_BYTES);
            assert(le32(p+32)==gsc_crc(frame+offset,count) && le32(p+36)==frame_crc);
            assert(le32(p+40)==gsc_crc(p+8,32) && !memcmp(p+44,frame+offset,count));
            memcpy(copy+offset,p+44,count);offset+=count;
            if(fixture){assert(fwrite(p,1,n,fixture)==n);fputs("kernel between\n",fixture);}
        }
        assert(!memcmp(frame,copy,sizeof(frame)));
        if(fixture){fputs("kernel after\n",fixture);assert(fclose(fixture)==0);}
    }
    assert(!gsc_packet(NULL,2047,512,frame,1,0,0,0));
    assert(!gsc_packet(buffer,44,512,frame,1,0,0,0));
    assert(!gsc_packet(buffer,2047,1,frame,1,0,0,0));
    assert(!gsc_packet(buffer,2047,512,frame,0,0,0,0));
    assert(!gsc_packet(buffer,2047,512,frame,1,2,0,0));
    assert(!gsc_packet(buffer,2047,512,frame,1,0,GSC_FRAME_BYTES,0));
    assert(!gsc_packet_size(buffer,2047,512,frame,1,0,0,0,1265,2780));
    assert(!gsc_packet_size(buffer,2047,512,frame,1,0,0,0,1264,2781));
    assert(!gsc_packet_size(buffer,2047,512,frame,1,0,0,0,0,695));
    unsigned length=0;
    assert(gscreen_command("screen.start") && gscreen.live);
    unsigned sent=0,yields=0;
    while(gscreen.active || !sent) {
        if(gscreen_transmit(buffer,2047,&length))sent++;
        else yields++;
    }
    assert(sent>400 && yields==sent-1 && captures==1);
    assert(!gscreen_transmit(buffer,2047,&length)); /* one log turn */
    assert(!gscreen_transmit(buffer,2047,&length) && captures==1);
    fake_ms=1000;assert(gscreen_transmit(buffer,2047,&length) && captures==2);
    assert(gscreen_command("screen.stop") && !gscreen.active && !gscreen.live);
    assert(gscreen_transmit(buffer,2047,&length) && length==44 && le32(buffer+12)==2);
    for(unsigned i=0;i<10;i++)assert(!gscreen_transmit(buffer,2047,&length));
    assert(gscreen_command("screen.snapshot") && gscreen.requested);
    assert(!gscreen_transmit(buffer,2047,&length));fake_ms=2000;
    assert(gscreen_transmit(buffer,2047,&length) && le32(buffer+12)==1);
    assert(!gscreen_command("shell") && !gscreen.live && !gscreen.active);
    assert(gscreen_transmit(buffer,2047,&length) && length==44 && le32(buffer+12)==2);
    enabled=0;assert(gscreen_command("screen.start") && !gscreen.live);enabled=1;
    available=0;assert(gscreen_command("screen.snapshot"));
    fake_ms=3000;
    assert(!gscreen_transmit(buffer,2047,&length) && !gscreen.active && !gscreen.requested);
    available=1;assert(gscreen_command("screen.start"));gscreen_disconnect();
    assert(!gscreen_transmit(buffer,2047,&length) && !gscreen.live);
    assert(gscreen_command("screen.configure 159 30") && gscreen.max_size==695);
    assert(gscreen_command("screen.configure 2781 30") && gscreen.max_size==695);
    assert(gscreen_command("screen.configure 2780 0") && gscreen.max_fps==1);
    assert(gscreen_command("screen.configure 2780 31") && gscreen.max_fps==1);
    assert(gscreen_command("screen.configure 1080 30 trailing") && gscreen.max_size==695);
    assert(gscreen_command("screen.configure 1080 30") && gscreen.max_size==1080 && gscreen.max_fps==30);
    assert(gscreen_command("screen.start"));
    assert(gscreen_transmit(buffer,2047,&length) && gscreen.width==491 && gscreen.height==1080);
    assert(le32(buffer+20)==491u*1080u*2u && gscreen.next_ms==fake_ms+34);
    /* Changing a setting cannot change metadata halfway through a frame. */
    assert(gscreen_command("screen.configure 2780 15"));
    assert(!gscreen_transmit(buffer,2047,&length));
    assert(gscreen_transmit(buffer,2047,&length) && le32(buffer+20)==491u*1080u*2u);
    gscreen_disconnect();assert(gscreen_command("screen.start"));
    assert(gscreen_transmit(buffer,2047,&length) && gscreen.width==1264 && gscreen.height==2780);
    assert(le32(buffer+20)==GSC_MAX_FRAME_BYTES && le32(buffer+36)==gsc_crc(gscreen.frame,GSC_MAX_FRAME_BYTES));
    gscreen_disconnect();
    puts("Screen protocol: CRC/LE metadata, short packets, bounds, log interleave, configurable size/FPS, frame boundaries, stop and disconnect passed");
    return 0;
}
