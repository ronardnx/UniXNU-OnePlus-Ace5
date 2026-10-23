#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/display/giulia_display.h"
int main(void) {
    size_t bytes=(size_t)GD_STRIDE*GD_HEIGHT,srcbytes=(size_t)GD_CANVAS_W*GD_CANVAS_H*4;
    uint32_t *allocation=malloc(bytes+128),*input=malloc(srcbytes+128);assert(allocation&&input);
    memset(allocation,0xa5,bytes+128);memset(input,0x5a,srcbytes+128);
    uint32_t *dst=allocation+16,*src=input+16;
    for(unsigned y=0;y<GD_CANVAS_H;y++)for(unsigned x=0;x<GD_CANVAS_W;x++)src[y*GD_CANVAS_W+x]=0xff000000|(y<<12)|x;
    struct gd_rect full={0,0,GD_CANVAS_W,GD_CANVAS_H};
    assert(gd_present(dst,bytes,src,srcbytes,0,GD_CANVAS_W,GD_CANVAS_H,GD_CANVAS_W*4,full,full));
    for(unsigned y=0;y<GD_HEIGHT;y++)for(unsigned x=0;x<GD_WIDTH;x++)
        assert(dst[y*GD_WIDTH+x]==src[(y*GD_CANVAS_H/GD_HEIGHT)*GD_CANVAS_W+x*GD_CANVAS_W/GD_WIDTH]);
    for(unsigned i=0;i<16;i++){assert(allocation[i]==0xa5a5a5a5&&allocation[16+bytes/4+i]==0xa5a5a5a5);assert(input[i]==0x5a5a5a5a);}
    assert(dst[0]==src[0] && dst[GD_WIDTH*GD_HEIGHT-1]==src[GD_CANVAS_W*GD_CANVAS_H-1]);
    struct gd_rect tile={0,0,100,100},clipped={-50,-50,100,100};
    assert(gd_present(dst,bytes,src,srcbytes,0,GD_CANVAS_W,GD_CANVAS_H,GD_CANVAS_W*4,tile,clipped));
    assert(dst[0]==src[50*GD_CANVAS_W+50]);
    uint32_t before=dst[0];struct gd_rect invalid=tile;invalid.x=0x7fffffff;
    assert(!gd_present(dst,bytes,src,srcbytes,0,GD_CANVAS_W,GD_CANVAS_H,GD_CANVAS_W*4,invalid,full)&&dst[0]==before);
    assert(!gd_present(dst,bytes-1,src,srcbytes,0,GD_CANVAS_W,GD_CANVAS_H,GD_CANVAS_W*4,full,full));
    assert(!gd_present(dst,bytes,src,srcbytes-1,0,GD_CANVAS_W,GD_CANVAS_H,GD_CANVAS_W*4,full,full));
    assert(!gd_present(dst,bytes,src,srcbytes,4,GD_CANVAS_W,GD_CANVAS_H,GD_CANVAS_W*4,full,full));
    assert(gd_touch(-1,GD_WIDTH)==0 && gd_touch(0,GD_WIDTH)==0 && gd_touch(GD_WIDTH-1,GD_WIDTH)==65535);
    assert(gd_touch(GD_HEIGHT-1,GD_HEIGHT)==65535 && gd_touch(0x7fffffff,GD_HEIGHT)==65535);
    for(unsigned x=1;x<GD_WIDTH;x++)assert(gd_touch(x,GD_WIDTH)>gd_touch(x-1,GD_WIDTH));
    for(unsigned y=1;y<GD_HEIGHT;y++)assert(gd_touch(y,GD_HEIGHT)>gd_touch(y-1,GD_HEIGHT));
    free(allocation);free(input);puts("Display: all 3,513,920 native pixels, clipping, memory guards and full-panel touch verified");return 0;
}
