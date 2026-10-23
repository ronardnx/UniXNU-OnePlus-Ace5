#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe/HallPreparePolicy.h"
#define FAILURE UINT64_C(0x8000000000000042)
struct model {
    unsigned calls,fail,created,issued,waited,enabled,observed;
    unsigned wrong_set,wrong_get,off,zero_request;
    GHP_U32 address,slave;
};
static GHP_U64 step(struct model *m) { return ++m->calls==m->fail ? FAILURE : 0; }
static GHP_U64 resolve(void *p,GHP_U32 *address,GHP_U32 *slave)
{
    struct model *m=p;*address=m->address;*slave=m->slave;return step(m);
}
static GHP_U64 create(void *p) { struct model *m=p;m->created++;return step(m); }
static GHP_U64 issue(void *p,GHP_U32 address,GHP_U32 value,GHP_U32 *request)
{
    static const unsigned offsets[]={0,8,4},values[]={1800,7,1};
    struct model *m=p;unsigned n=m->issued++;
    assert(m->created==1 && n<3 && address==m->address+offsets[n] && value==values[n]);
    *request=m->zero_request ? 0 : n+1;return step(m);
}
static GHP_U64 wait_request(void *p,GHP_U32 request)
{
    struct model *m=p;assert(request==m->issued);m->waited++;return step(m);
}
static GHP_U64 clock_resolve(void *p) { return step(p); }
static GHP_U64 rate(void *p,GHP_U32 value,GHP_U32 *actual)
{
    struct model *m=p;assert(value==19200000 && m->calls==1);*actual=value-m->wrong_set;return step(m);
}
static GHP_U64 get_rate(void *p,GHP_U32 *actual)
{
    struct model *m=p;assert(m->calls==2);*actual=19200000-m->wrong_get;return step(m);
}
static GHP_U64 enable(void *p,unsigned index)
{
    struct model *m=p;assert(index==m->enabled && m->calls==3+index);m->enabled++;return step(m);
}
static GHP_U64 on(void *p,unsigned index,GHP_U32 *value)
{
    struct model *m=p;assert(index==m->observed && m->enabled==3 && m->calls==6+index);m->observed++;
    *value=m->off==index+1 ? 0 : 1;return step(m);
}
int main(void)
{
    struct model m;
    struct ghp_result r;
    struct ghp_power_io power={&m,resolve,create,issue,wait_request};
    struct ghp_clock_io clock={&m,clock_resolve,rate,get_rate,enable,on};
    for(unsigned fail=0;fail<=8;fail++) {
        memset(&m,0,sizeof(m));m.address=0x40020;m.slave=4;m.fail=fail;
        unsigned result=ghp_power_prepare(&power,&r);
        if(!fail)assert(!result && r.ready && r.stage==10 && m.issued==3 && m.waited==3);
        else assert(result && !r.ready && r.status==FAILURE && m.calls==fail);
        if(fail<=2 && fail)assert(!r.attempted && !m.issued);
    }
    for(unsigned invalid=0;invalid<4;invalid++) {
        memset(&m,0,sizeof(m));m.address=0x40020;m.slave=4;
        if(invalid==0)m.address=0;
        if(invalid==1)m.address=0x40021;
        if(invalid==2)m.address=0x30020;
        if(invalid==3)m.slave=3;
        assert(ghp_power_prepare(&power,&r)==GHP_ADDRESS && !m.created && !r.attempted);
    }
    memset(&m,0,sizeof(m));m.address=0x4fff0;m.slave=4;m.zero_request=1;
    assert(!ghp_power_prepare(&power,&r) && m.issued==3 && !m.waited);
    for(unsigned fail=0;fail<=9;fail++) {
        memset(&m,0,sizeof(m));m.fail=fail;
        unsigned result=ghp_clock_prepare(&clock,&r);
        if(!fail)assert(!result && r.ready && r.stage==10 && m.enabled==3 && m.observed==3);
        else {
            assert(result && !r.ready && r.status==FAILURE && m.calls==fail);
            if(fail<=3)assert(!m.enabled && !m.observed);
            else if(fail<=6)assert(m.enabled==fail-3 && !m.observed && r.stage==fail);
            else assert(m.enabled==3 && m.observed==fail-6 && r.stage==fail);
        }
    }
    for(unsigned mismatch=0;mismatch<5;mismatch++) {
        memset(&m,0,sizeof(m));
        if(mismatch==0)m.wrong_set=1;
        else if(mismatch==1)m.wrong_get=1;
        else m.off=mismatch-1;
        assert(ghp_clock_prepare(&clock,&r) && !r.ready);
        if(mismatch<2)assert(!m.enabled);
        else assert(m.enabled==3 && m.observed==m.off && r.stage==6+m.off && r.address==m.off-1);
    }
    assert(ghp_power_prepare(NULL,&r)==GHP_ARGUMENT);
    assert(ghp_clock_prepare(NULL,&r)==GHP_ARGUMENT);
    puts("Hall native preparation: ordered votes, every failure boundary, bad resource, zero request, wrong rate, and stopped clocks passed");
    return 0;
}
