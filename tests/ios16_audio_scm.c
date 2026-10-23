#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "../ios16/audio/giulia_audio_scm.h"
struct fixture { unsigned calls,allow,fail,secure;struct ggs_regs last; };
static int authorize(void *v,unsigned c,unsigned id,uint64_t a,uint64_t b)
{(void)c;(void)id;(void)a;(void)b;return ((struct fixture *)v)->allow;}
static int delay(void *v,unsigned ms){(void)v;(void)ms;return 0;}
static int call(void *v,struct ggs_regs *r)
{
    struct fixture *f=(struct fixture *)v;f->calls++;f->last=*r;
    if(f->fail){r->x[0]=2;return 0;}
    r->x[1]=(r->x[0]==0xc2000601u || r->x[0]==0x42000601u || r->x[0]==0x42000207u)?1:f->secure;
    r->x[0]=0;return 0;
}
int main(void)
{
    struct fixture f={0};struct gas_state s={0};struct gas_ops o={&f,call,delay,authorize};
    assert(gas_pas(&o,&s,5,1,0,0)==GAS_PREREQUISITE && !f.calls);
    f.allow=1;
    assert(gas_pas(&o,&s,1,36,0x8d0000000ULL,0)==0 && f.calls==9);
    assert(f.last.x[0]==0x42000201u && f.last.x[1]==0x82 && f.last.x[2]==36 && f.last.x[3]==0x8d0000000ULL);
    assert(gas_pas(&o,&s,2,36,0x9de80000,0xe000)==0 && f.calls==10);
    assert(f.last.x[1]==3 && f.last.x[3]==0x9de80000 && f.last.x[4]==0xe000);
    assert(gas_pas(&o,&s,5,36,0,0)==0 && f.last.x[1]==1);
    unsigned before=f.calls;
    assert(gas_pas(&o,&s,5,13,0,0)==GAS_ARGUMENT);
    assert(gas_pas(&o,&s,1,1,3,0)==GAS_ARGUMENT);
    assert(gas_pas(&o,&s,2,1,0x9de80000,4096)==GAS_ARGUMENT);
    assert(gas_pas(&o,&s,2,1,0x9df00000,0x5281000)==GAS_ARGUMENT);
    assert(gas_pas(&o,&s,5,1,1,0)==GAS_ARGUMENT && f.calls==before);
    f.allow=0;assert(gas_pas(&o,&s,5,1,0,0)==GAS_PREREQUISITE && f.calls==before);
    f.allow=1;f.secure=7;assert(gas_pas(&o,&s,5,1,0,0)==GAS_SECURE && s.secure_result==7);
    f.fail=1;assert(gas_pas(&o,&s,6,1,0,0)==GAS_TRANSPORT && s.exchange.quarantined);
    before=f.calls;assert(gas_pas(&o,&s,6,1,0,0)==GAS_QUARANTINED && f.calls==before);
    puts("Audio PAS frame, authority and failure checks passed");return 0;
}
