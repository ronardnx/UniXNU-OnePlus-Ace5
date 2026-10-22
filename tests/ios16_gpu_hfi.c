#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/giulia_gpu_hfi.h"
static uint32_t memory[GGH_BYTES/4+16],before[GGH_BYTES/4+16];
static unsigned barriers;
static void barrier(void *cookie){(void)cookie;++barriers;}
static struct ggh_io io={memory,GGH_BYTES,GGH_IOVA,0,barrier};
static void reset(void) {
    memset(memory,0xa5,sizeof(memory));assert(!ggh_init(memory,GGH_BYTES,GGH_IOVA));barriers=0;
}
static volatile uint32_t *header(unsigned q){return memory+6+12*q;}
static volatile uint32_t *ring(unsigned q){return memory+GGH_RING_OFFSET/4+q*1024;}
int main(void) {
    unsigned cases=0;
    reset();assert(memory[1]==210 && memory[2]==6 && memory[3]==12 && memory[4]==17);
    for(unsigned q=0;q<17;++q) {
        volatile uint32_t *h,*p;unsigned r,w;
        assert(!ggh_queue(&io,q,&h,&p,&r,&w) && !r && !w);
        assert(h[1]==0x60010350u+4096*q && h[3]==1024);
    }
    assert(memory[GGH_BYTES/4]==0xa5a5a5a5);
    for(unsigned start=0;start<1024;start+=4)for(unsigned n=1;n<=255;n+=13) {
        uint32_t msg[256],out[256];unsigned count=999;
        reset();header(0)[10]=header(0)[11]=start;
        msg[0]=(n<<8)|12u;for(unsigned i=1;i<n;++i)msg[i]=0x12340000+i;
        assert(!ggh_send(&io,msg,n));unsigned end=(start+((n+3)&~3u))&1023;
        assert(header(0)[11]==end && barriers==3);
        for(unsigned i=0;i<((n+3)&~3u);++i)assert(ring(0)[(start+i)&1023]==(i<n?msg[i]:0xfafafafa));
        for(unsigned q=1;q<=2;++q) {
            header(q)[10]=start;header(q)[11]=end;
            for(unsigned i=0;i<1024;++i)ring(q)[i]=ring(0)[i];
            assert(!ggh_receive(&io,q,out,256,&count) && count==n);
            assert(!memcmp(out,msg,n*4) && header(q)[10]==end);
            assert(ggh_receive(&io,q,out,256,&count)==GGH_EMPTY && !count);
        }
        ++cases;
    }
    { uint32_t msg[]={0x40c,1,2,3};
      reset();header(0)[10]=4;memcpy(before,memory,sizeof(memory));
      assert(ggh_send(&io,msg,4)==GGH_FULL && !memcmp(before,memory,sizeof(memory)));
      for(unsigned field=0;field<6;++field) {
          reset();memory[field]^=1;memcpy(before,memory,sizeof(memory));
          assert(ggh_send(&io,msg,4)==GGH_CORRUPT && !memcmp(before,memory,sizeof(memory)));
      }
      for(unsigned field=0;field<12;++field) {
          if(field>=5 && field<10)continue;
          reset();header(0)[field]^=field>=10?1:0x80000000;memcpy(before,memory,sizeof(memory));
          assert(ggh_send(&io,msg,4)==GGH_CORRUPT && !memcmp(before,memory,sizeof(memory)));
      }
    }
    { uint32_t out[256];unsigned count;
      for(unsigned bad=0;bad<3;++bad) {
          reset();header(1)[11]=4;ring(1)[0]=bad==0?0:bad==1?0x80c:0xff0c;
          memcpy(before,memory,sizeof(memory));
          assert(ggh_receive(&io,1,out,256,&count)==GGH_CORRUPT && !count);
          assert(!memcmp(before,memory,sizeof(memory)));
      }
    }
    reset();memcpy(before,memory,sizeof(memory));
    assert(ggh_init(memory,GGH_BYTES-1,GGH_IOVA)==GGH_INVALID);
    assert(ggh_init(memory,GGH_BYTES,0xfffff000)==GGH_INVALID);
    assert(ggh_init((char *)memory+1,GGH_BYTES,GGH_IOVA)==GGH_INVALID);
    assert(!memcmp(before,memory,sizeof(memory)));
    printf("HFI: %u wrap/size cases, 17 queue layout, alignment, padding, full/empty and corrupt-input rejection passed\n",cases);
}
