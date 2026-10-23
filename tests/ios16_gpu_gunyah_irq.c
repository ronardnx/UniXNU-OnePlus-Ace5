/* RAM-only GIC fixtures: no interrupt controller, device or HVC is touched. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_gunyah_irq.h"

struct fixture {
    uint32_t regs[0x400/4];
    struct ggirq_state state;
    struct ggirq_io io;
    uint64_t clock;
    unsigned reads,writes,barriers,checks,clock_calls;
    unsigned rwp,step,regress,context,lose_context,stuck_enable,become_active;
};
static struct fixture f;
static const unsigned owner1=1,owner2=2;
static uint32_t read32(void *cookie,unsigned offset)
{
    struct fixture *p=cookie;++p->reads;assert(!(offset&3) && offset<p->io.bytes);
    if(offset==0 && p->rwp) {
        if(p->rwp!=~0u)--p->rwp;
        return UINT32_C(0x80000000);
    }
    return p->regs[offset/4];
}
static void write32(void *cookie,unsigned offset,uint32_t value)
{
    struct fixture *p=cookie;++p->writes;
    /* Only per-SPI clear-enable writes are permitted. This model rejects
     * pending/active/EOI writes and preserves every unrelated interrupt. */
    assert(offset>=0x180 && offset<0x200 && !(offset&3));
    assert(value && !(value&(value-1)));
    unsigned index=(offset-0x180)/4;
    if(!p->stuck_enable)p->regs[0x100/4+index]&=~value;
    if(p->become_active)p->regs[0x300/4+index]|=value;
}
static void barrier(void *cookie){++((struct fixture *)cookie)->barriers;}
static uint64_t now_us(void *cookie)
{
    struct fixture *p=cookie;++p->clock_calls;
    if(p->regress && p->clock_calls==2)return p->clock-1;
    p->clock+=p->step;return p->clock;
}
static int uncontended(void *cookie)
{
    struct fixture *p=cookie;++p->checks;
    return p->context && !(p->lose_context && p->checks>1);
}
static void reset(void)
{
    memset(&f,0,sizeof(f));f.clock=100;f.context=1;f.regs[4/4]=31;
    f.io.cookie=&f;f.io.bytes=sizeof(f.regs);f.io.read32=read32;
    f.io.write32=write32;f.io.barrier=barrier;f.io.now_us=now_us;f.io.uncontended=uncontended;
}
static void enable(unsigned intid)
{f.regs[(0x100+4*(intid/32))/4]|=UINT32_C(1)<<(intid%32);}
static void pending(unsigned intid)
{f.regs[(0x200+4*(intid/32))/4]|=UINT32_C(1)<<(intid%32);}
static void active(unsigned intid)
{f.regs[(0x300+4*(intid/32))/4]|=UINT32_C(1)<<(intid%32);}
static int claim(unsigned tx,unsigned rx)
{return ggirq_claim(&f.io,&f.state,&owner1,17,tx,rx);}
static int owns(unsigned tx,unsigned rx)
{return ggirq_owns(&f.io,&f.state,&owner1,17,tx,rx);}
static void retained(unsigned tx,unsigned rx)
{
    assert(f.state.attempted && f.state.reserved && f.state.quarantined);
    assert(!owns(tx,rx));
    unsigned reads=f.reads,writes=f.writes;
    assert(ggirq_claim(&f.io,&f.state,&owner2,18,tx,rx)==GGIRQ_USED);
    assert(f.reads==reads && f.writes==writes && f.state.epoch==17 && f.state.owner==&owner1);
}
static void exact_masks_and_pending(void)
{
    const unsigned ids[][2]={{32,33},{207,409},{1000,1019}};
    for(unsigned n=0;n<sizeof(ids)/sizeof(*ids);n++) {
        reset();unsigned tx=ids[n][0],rx=ids[n][1];
        enable(tx);enable(rx);enable(tx^1);pending(tx);pending(rx);
        uint32_t pending_before[32];memcpy(pending_before,f.regs+0x200/4,sizeof(pending_before));
        f.rwp=3;
        assert(claim(tx,rx)==GGIRQ_OK && f.writes==2 && f.barriers==2);
        assert(f.state.claimed && !f.state.quarantined && f.state.pending[0] && f.state.pending[1]);
        assert(!f.state.enabled[0] && !f.state.enabled[1] && !f.state.active[0] && !f.state.active[1]);
        assert(!memcmp(pending_before,f.regs+0x200/4,sizeof(pending_before)));
        assert(owns(tx,rx));
        if((tx^1)!=rx)assert(f.regs[(0x100+4*(tx/32))/4]&(UINT32_C(1)<<((tx^1)%32)));
        unsigned reads=f.reads,writes=f.writes;
        assert(!ggirq_owns(&f.io,&f.state,&owner2,17,tx,rx));
        assert(!ggirq_owns(&f.io,&f.state,&owner1,18,tx,rx));
        assert(!owns(rx,tx) && f.reads==reads && f.writes==writes && !f.state.quarantined);
        assert(!ggirq_dispatch(&f.io,&f.state,31) && !ggirq_dispatch(&f.io,&f.state,tx+2));
        assert(!f.state.quarantined && f.writes==writes);
        assert(ggirq_dispatch(&f.io,&f.state,rx));retained(tx,rx);
        assert(!memcmp(pending_before,f.regs+0x200/4,sizeof(pending_before)));
    }
}
static void refuse_other_handler(void)
{
    reset();f.context=0;enable(44);enable(77);pending(77);
    assert(claim(44,77)==GGIRQ_CONFLICT);
    assert(f.state.attempted && f.state.quarantined && !f.state.reserved);
    assert(!f.reads && !f.writes && !ggirq_dispatch(&f.io,&f.state,44));
    assert(ggirq_claim(&f.io,&f.state,&owner2,18,44,77)==GGIRQ_USED);
    assert(!f.writes); /* Never mask an IRQ on an unproven foreign controller. */
}
static void acquire_faults(void)
{
    for(unsigned mode=0;mode<7;mode++) {
        reset();enable(44);enable(77);pending(77);unsigned expected=GGIRQ_ACTIVE;
        if(mode==0)active(44);
        if(mode==1)f.become_active=1;
        if(mode==2){f.stuck_enable=1;expected=GGIRQ_HARDWARE;}
        if(mode==3){f.rwp=~0u;expected=GGIRQ_TIMEOUT;}
        if(mode==4){f.rwp=~0u;f.step=1001;expected=GGIRQ_TIMEOUT;}
        if(mode==5){f.regress=1;expected=GGIRQ_TIMEOUT;}
        if(mode==6){f.lose_context=1;expected=GGIRQ_CONFLICT;}
        assert(claim(44,77)==(int)expected && f.state.last_result==expected);
        retained(44,77);assert(f.writes==4 && f.state.pending[1]);
        assert(f.regs[(0x200+4*(77/32))/4]&(UINT32_C(1)<<(77%32)));
        assert(f.clock_calls<=GGIRQ_POLLS+2);
    }
    reset();f.regs[4/4]=0;
    assert(claim(44,77)==GGIRQ_HARDWARE && !f.state.reserved && !f.writes);
}
static void live_drift(void)
{
    for(unsigned mode=0;mode<5;mode++) {
        reset();assert(claim(44,77)==GGIRQ_OK);
        if(mode==0)enable(44);
        if(mode==1)active(77);
        if(mode==2)f.rwp=1;
        if(mode==3)f.context=0;
        if(mode==4)ggirq_invalidate(&f.io,&f.state);
        assert(!owns(44,77));retained(44,77);
    }
    reset();assert(claim(44,77)==GGIRQ_OK);pending(77);
    assert(owns(44,77) && f.state.pending[1] && !f.state.quarantined);
}
static void arguments(void)
{
    const unsigned ids[][2]={{0,77},{31,77},{44,1020},{44,UINT32_MAX},{44,44}};
    for(unsigned i=0;i<sizeof(ids)/sizeof(*ids);i++) {
        reset();assert(claim(ids[i][0],ids[i][1])==GGIRQ_ARGUMENT);
        assert(!f.state.attempted && !f.reads && !f.writes);
    }
    reset();assert(ggirq_claim(NULL,&f.state,&owner1,17,44,77)==GGIRQ_ARGUMENT);
    assert(ggirq_claim(&f.io,NULL,&owner1,17,44,77)==GGIRQ_ARGUMENT);
    assert(ggirq_claim(&f.io,&f.state,NULL,17,44,77)==GGIRQ_ARGUMENT);
    assert(ggirq_claim(&f.io,&f.state,&owner1,0,44,77)==GGIRQ_ARGUMENT);
    f.io.bytes=GGIRQ_REG_BYTES-1;assert(claim(44,77)==GGIRQ_ARGUMENT);
    assert(!f.state.attempted && !f.reads && !f.writes);
}
static uint32_t random_state=0x35a7b619;
static uint32_t random_word(void)
{random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state;}
static void malformed_fuzz(void)
{
    for(unsigned i=0;i<20000;i++) {
        reset();unsigned tx=random_word()%1035,rx=random_word()%1035;
        f.regs[4/4]=random_word()&31;f.context=random_word()&1;
        for(unsigned j=0;j<32;j++) {
            f.regs[0x100/4+j]=random_word();
            f.regs[0x200/4+j]=random_word();
            f.regs[0x300/4+j]=random_word();
        }
        uint32_t pending_before[32],active_before[32];
        memcpy(pending_before,f.regs+0x200/4,sizeof(pending_before));
        memcpy(active_before,f.regs+0x300/4,sizeof(active_before));
        int result=claim(tx,rx);
        if(result==GGIRQ_OK)assert(owns(tx,rx) && f.state.claimed);
        else if(result!=GGIRQ_ARGUMENT)assert(f.state.attempted && f.state.quarantined);
        assert(!memcmp(pending_before,f.regs+0x200/4,sizeof(pending_before)));
        assert(!memcmp(active_before,f.regs+0x300/4,sizeof(active_before)));
    }
}
int main(void)
{
    exact_masks_and_pending();refuse_other_handler();acquire_faults();live_drift();arguments();malformed_fuzz();
    puts("GPU Gunyah IRQ: dynamic SPI masks, pending preservation, active/handler refusal, retained quarantine and 20000 malformed fixtures passed");
    return 0;
}
