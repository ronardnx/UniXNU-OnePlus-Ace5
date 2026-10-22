#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe/WifiPcieCesta.h"
static struct {
    unsigned crm[0x3000/4],sm[0x7000/4],clock[0x2000/4];
    unsigned writes,drop,quiet,switch_timeout,power_timeout,delay,foreign,require_running,started;
} h,original;
static unsigned *reg(unsigned region,unsigned offset) {
    assert(!(offset&3));
    if(region==GCE_CRM){assert(offset<sizeof(h.crm));return &h.crm[offset/4];}
    if(region==GCE_SM){assert(offset<sizeof(h.sm));return &h.sm[offset/4];}
    assert(region==GCE_CLOCK&&offset<sizeof(h.clock));return &h.clock[offset/4];
}
static unsigned read_reg(void *c,unsigned region,unsigned offset) {
    (void)c;
    if(region==GCE_CLOCK && offset==0xc) {
        if(h.power_timeout)return 0;
        unsigned ch=h.crm[0x1000/4]==2?1:0;
        if(h.crm[0x1000/4] && h.sm[0x1090/4]==1 && !h.sm[0x1040/4] && !h.sm[0x1048/4]) {
            for(unsigned i=0;i<13;i++)if(h.sm[i]!=gce_code[i])return 0;
            for(unsigned i=0;i<7;i++)if(h.sm[0x1000/4+i]!=gce_branch[i])return 0;
            unsigned a=h.crm[gce_reg(0,ch,0,0,2)/4],b=h.crm[gce_reg(0,ch,1,0,2)/4];
            return a<b?a:b;
        }
        return 0;
    }
    return *reg(region,offset);
}
static void write_reg(void *c,unsigned region,unsigned offset,unsigned value) {
    (void)c;
    /* Explicit whitelist: no global CRM state, voter 1, SW voter, voltage
     * or IRQ registers may be altered, even in error cleanup. */
    if(region==GCE_CRM) {
        unsigned allowed=offset==0x1020;
        for(unsigned type=0;type<2;type++)for(unsigned channel=0;channel<2;channel++)for(unsigned power=0;power<5;power++)
            for(unsigned resource=0;resource<(type?6u:2u);resource++)allowed|=offset==gce_reg(type,channel,power,resource,type?6:2);
        assert(allowed);
    } else {assert(region==GCE_SM);assert(offset<13*4 || (offset>=0x1000&&offset<0x1000+7*4) || offset==0x1040 || offset==0x1048 || offset==0x1090);}
    if(++h.writes==h.drop)return;
    *reg(region,offset)=value;
    if(region==GCE_SM && offset==0x1090 && (value&1))h.started=1;
    if(region==GCE_CRM && offset==0x1020 && !h.switch_timeout &&
       (!h.require_running || !h.started || ((h.sm[0x1090/4]&1) && !h.sm[0x1040/4] && !h.sm[0x1048/4])))
        h.crm[0x1000/4]=value;
}
static void delay(void *c,unsigned us){(void)c;h.delay+=us;assert(h.delay<300000);}
static int quiet(void *c){(void)c;return h.quiet;}
static const struct gce_io io={NULL,read_reg,write_reg,delay,quiet};
static void init(unsigned channel,int started) {
    memset(&h,0,sizeof(h));h.quiet=1;
    h.crm[0]=0x10000;h.crm[1]=0x00122225;h.crm[2]=6u<<26;h.crm[3]=1;
    h.crm[0x1000/4]=channel;h.crm[0x1020/4]=channel;
    for(unsigned type=0;type<2;type++)for(unsigned ch=0;ch<2;ch++)for(unsigned power=0;power<5;power++)
        for(unsigned resource=0;resource<(type?6u:2u);resource++)
            h.crm[gce_reg(type,ch,power,resource,type?6:2)/4]=type?(0x20000000u+ch*100+power*10+resource):(resource?4:0);
    h.sm[0x1040/4]=1;h.sm[0x1048/4]=1;
    if(started){h.sm[0x1090/4]=1;for(unsigned i=0;i<13;i++)h.sm[i]=gce_code[i];for(unsigned i=0;i<7;i++)h.sm[0x1000/4+i]=gce_branch[i];}
    original=h;
}
static void restored(struct gce_state *s) {
    assert(!gce_restore(&io,s)&&!s->changed&&!s->retained);
    if(!original.crm[0x1020/4]) {
        assert(h.crm[0x1020/4]==1 || !h.crm[0x1020/4]);
        original.crm[0x1020/4]=h.crm[0x1020/4];original.crm[0x1000/4]=h.crm[0x1000/4];
    }
    assert(!memcmp(h.crm,original.crm,sizeof(h.crm)));
    assert(!memcmp(h.sm,original.sm,sizeof(h.sm)));
}
int main(void) {
    struct gce_state s;unsigned cases=0,total;
    for(unsigned c=0;c<3;c++)for(unsigned started=0;started<2;started++){
        init(c,started);memset(&s,0,sizeof(s));assert(gce_take(&io,&s)==GCE_OK&&s.active);
        for(unsigned power=0;power<2;power++)assert(h.crm[gce_reg(0,s.target,power,0,2)/4]==GW_STOCK_D0_PERF);
        for(unsigned power=0;power<5;power++)for(unsigned resource=0;resource<6;resource++)
            assert(h.crm[gce_reg(1,s.target,power,resource,6)/4]==original.crm[gce_reg(1,s.source,power,resource,6)/4]);
        restored(&s);cases++;
    }
    init(1,0);memset(&s,0,sizeof(s));assert(!gce_take(&io,&s));total=h.writes;
    /* Dropped stores at every request and rollback write position. */
    for(unsigned drop=1;drop<=total;drop++) {
        init(1,0);memset(&s,0,sizeof(s));h.drop=drop;int e=gce_take(&io,&s);
        if(!e)assert(read_reg(NULL,GCE_CLOCK,0xc)>=GW_STOCK_D0_PERF);
        h.drop=0;restored(&s);cases++;
    }
    init(1,0);memset(&s,0,sizeof(s));assert(!gce_take(&io,&s));unsigned before=h.writes;restored(&s);unsigned restore_writes=h.writes-before;
    for(unsigned drop=1;drop<=restore_writes;drop++) {
        init(1,0);memset(&s,0,sizeof(s));assert(!gce_take(&io,&s));h.drop=h.writes+drop;
        int e=gce_restore(&io,&s);if(e)assert(s.retained);cases++;
    }
    init(1,0);memset(&s,0,sizeof(s));h.quiet=0;assert(gce_take(&io,&s)==GCE_BUSY&&!h.writes);cases++;
    init(1,0);memset(&s,0,sizeof(s));h.crm[3]=0;assert(gce_take(&io,&s)==GCE_GEOMETRY&&!h.writes);cases++;
    init(1,0);memset(&s,0,sizeof(s));h.crm[1]^=0x10;assert(gce_take(&io,&s)==GCE_GEOMETRY&&!h.writes);cases++;
    init(1,0);memset(&s,0,sizeof(s));h.sm[0]=1;assert(gce_take(&io,&s)==GCE_UNOWNED&&!h.writes);cases++;
    init(1,0);memset(&s,0,sizeof(s));h.crm[0x1000/4]=3;assert(gce_take(&io,&s)==GCE_BUSY&&!h.writes);cases++;
    init(1,0);memset(&s,0,sizeof(s));h.crm[0x1024/4]=1;assert(gce_take(&io,&s)==GCE_BUSY&&!h.writes);cases++;
    init(1,0);memset(&s,0,sizeof(s));h.crm[0x1004/4]=1;assert(gce_take(&io,&s)==GCE_BUSY&&!h.writes);cases++;
    init(1,1);memset(&s,0,sizeof(s));h.crm[0x1024/4]=1;h.crm[0x1004/4]=1;original=h;
    assert(!gce_take(&io,&s));restored(&s);cases++;
    init(1,0);memset(&s,0,sizeof(s));h.switch_timeout=1;assert(gce_take(&io,&s)==GCE_SWITCH_TIMEOUT&&s.retained);cases++;
    init(1,0);memset(&s,0,sizeof(s));h.power_timeout=1;assert(gce_take(&io,&s)==GCE_POWER_TIMEOUT);restored(&s);cases++;
    init(2,1);memset(&s,0,sizeof(s));assert(!gce_take(&io,&s));h.crm[gce_reg(0,s.target,0,1,2)/4]=5;
    before=h.writes;assert(gce_restore(&io,&s)==GCE_UNOWNED&&s.retained&&h.writes==before);cases++;
    for(unsigned channel=0;channel<3;channel++) {
        init(channel,0);h.require_running=1;memset(&s,0,sizeof(s));
        unsigned char aux[8]={0xe8,3,0,0,16,0,0,0};
        for(unsigned n=0;n<GCB_NODES;n++)if(gcb_name(n)) {
            assert(gcb_vote(n,aux,8,&s.bw_floor[n]));s.bw_ready|=1u<<n;
        }
        assert(!gce_take(&io,&s));
        for(unsigned power=0;power<5;power++)for(unsigned n=0;n<6;n++) {
            unsigned old=original.crm[gce_reg(1,s.source,power,n,6)/4];
            unsigned expected=power<2?gcb_merge(old,s.bw_floor[n],n):old;
            assert(h.crm[gce_reg(1,s.target,power,n,6)/4]==expected);
        }
        restored(&s);cases++;
    }
    init(1,0);memset(&s,0,sizeof(s));s.bw_ready=GCB_PATH_MASK;h.crm[2]=3u<<26;
    assert(gce_take(&io,&s)==GCE_GEOMETRY&&!h.writes);cases++;
    init(1,0);memset(&s,0,sizeof(s));assert(!gce_take(&io,&s));h.crm[0x1024/4]=1;
    before=h.writes;assert(gce_restore(&io,&s)==GCE_BUSY&&s.retained&&h.writes==before);cases++;
    printf("CESTA/CRM: %u cold/warm, inactive-channel, preserved-vote, dropped-store and bounded-failure cases passed\n",cases);return 0;
}
