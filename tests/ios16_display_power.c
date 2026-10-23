#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/giulia_display_power.h"
static struct {
    uint32_t registers[0x1000],fifo[2],irq_original;
    unsigned writes,commands,fifo_words,reset_after_start,started,polls,complete_after;
    unsigned stuck,missing_done,drop_ack,restore_drop,setup_drop,all_ones,done_generated,drop_final_ack;
} hw;
static uint32_t read32(void *cookie,unsigned offset)
{
    assert(cookie==&hw && offset<0x4000 && !(offset&3));
    if(offset==0x110 && hw.started) {
        hw.polls++;
        if(hw.all_ones)return 0xffffffff;
        if(!hw.stuck && hw.polls>=hw.complete_after) {
            hw.registers[8/4]&=~2u;
            if(!hw.missing_done && !hw.done_generated) {
                hw.registers[0x110/4]|=1;hw.done_generated=1;
            }
        }
    }
    return hw.registers[offset/4];
}
static void write32(void *cookie,unsigned offset,uint32_t value)
{
    assert(cookie==&hw);hw.writes++;
    if(offset==0x110) {
        /* Only DMA_DONE is W1C here; every other status must be written 0. */
        assert((value&GDP_IRQ_STATUS_MASK)==1);
        assert((value&~GDP_IRQ_STATUS_MASK)==(hw.irq_original&~GDP_IRQ_STATUS_MASK));
        if(!hw.drop_ack && !(hw.started && hw.drop_final_ack)) {
            uint32_t old=hw.registers[offset/4];
            hw.registers[offset/4]=(old&GDP_IRQ_STATUS_MASK&~value)|(value&~GDP_IRQ_STATUS_MASK);
        }
        return;
    }
    if(offset==0x1ec) {if(hw.started)hw.reset_after_start++;return;}
    if(offset==0x17c) {assert(hw.fifo_words<2);hw.fifo[hw.fifo_words++]=value;return;}
    if(offset==0x90) {
        assert(value==1 && hw.fifo_words==2);hw.commands++;hw.started=1;
        hw.registers[8/4]|=2;return;
    }
    assert(offset==0x3c || offset==0x4c || offset==0x15c);
    if(offset==0x4c && value==8 && hw.restore_drop)return;
    if(offset==0x15c && value==0x30006 && hw.setup_drop)return;
    hw.registers[offset/4]=value;
}
static void pause_us(void *cookie,unsigned us){assert(cookie==&hw && (us==10 || us==1));}
static const struct gdp_io io={&hw,read32,write32,pause_us};
static void reset(struct gdp_state *state)
{
    memset(&hw,0,sizeof(hw));memset(state,0,sizeof(*state));
    hw.registers[1]=0x1f5;hw.registers[8/4]=5;
    hw.registers[0x84/4]=0x80000004;hw.registers[0x3c/4]=0x14000000;
    hw.registers[0x4c/4]=8;hw.complete_after=3;
    hw.irq_original=0xf7ffffff; /* Exercise all mixed W1C/enable fields. */
    hw.registers[0x110/4]=hw.irq_original;
}
static void restored(void)
{
    assert(hw.registers[0x3c/4]==0x14000000 && hw.registers[0x4c/4]==8 && !hw.registers[0x15c/4]);
    assert((hw.registers[0x110/4]&~GDP_IRQ_STATUS_MASK)==(hw.irq_original&~GDP_IRQ_STATUS_MASK));
    assert((hw.registers[0x110/4]&GDP_IRQ_STATUS_MASK)==(hw.irq_original&GDP_IRQ_STATUS_MASK&~1u));
}
int main(void)
{
    struct gdp_state state;unsigned cases=0;
    for(unsigned on=0;on<2;on++) {
        reset(&state);assert(gdp_power(&io,&state,on)==GDP_OK);
        assert(state.known && state.on==on && state.transitions==1 && !state.frozen);
        assert(hw.fifo[0]==(0x80050000u|(on?0x29u:0x28u)) && hw.fifo[1]==0);
        assert(hw.commands==1 && hw.polls==4);restored();cases++;
        unsigned writes=hw.writes;
        assert(gdp_power(&io,&state,on)==GDP_OK && hw.writes==writes && state.requests==2);cases++;
    }
    reset(&state);hw.registers[1]=0;
    assert(gdp_power(&io,&state,0)==GDP_UNSUPPORTED && !hw.writes);cases++;
    reset(&state);hw.registers[8/4]|=2;
    assert(gdp_power(&io,&state,0)==GDP_BUSY && !hw.writes);cases++;
    reset(&state);hw.registers[0x15c/4]=1;
    assert(gdp_power(&io,&state,0)==GDP_BUSY && !hw.writes);cases++;
    reset(&state);hw.registers[0x84/4]=0;
    assert(gdp_power(&io,&state,0)==GDP_UNSUPPORTED && !hw.writes);cases++;
    reset(&state);hw.drop_ack=1;
    assert(gdp_power(&io,&state,0)==GDP_IO && !hw.commands && !state.known);cases++;
    reset(&state);hw.setup_drop=1;
    assert(gdp_power(&io,&state,0)==GDP_IO && !hw.commands && !state.known);restored();cases++;
    reset(&state);hw.stuck=1;
    assert(gdp_power(&io,&state,0)==GDP_TIMEOUT && state.frozen && !state.known);
    assert(hw.polls==10000 && !hw.reset_after_start && hw.registers[0x15c/4]==0x30006);
    unsigned writes=hw.writes;
    assert(gdp_power(&io,&state,1)==GDP_FROZEN && hw.writes==writes);cases++;
    reset(&state);hw.missing_done=1;
    assert(gdp_power(&io,&state,0)==GDP_TIMEOUT && state.frozen && !state.known);
    assert(!hw.reset_after_start && hw.registers[0x15c/4]==0x30006);cases++;
    reset(&state);hw.registers[0x68/4]=1;
    assert(gdp_power(&io,&state,0)==GDP_ACK && !state.known && state.frozen);
    assert(!hw.reset_after_start && hw.registers[0x15c/4]==0x30006);cases++;
    reset(&state);hw.restore_drop=1;
    assert(gdp_power(&io,&state,0)==GDP_RESTORE && state.frozen && state.known && !state.on);cases++;
    reset(&state);hw.all_ones=1;
    assert(gdp_power(&io,&state,0)==GDP_IO && state.frozen && !state.known);cases++;
    reset(&state);hw.drop_final_ack=1;
    assert(gdp_power(&io,&state,0)==GDP_IO && state.frozen && state.known && !state.on);
    assert(!hw.reset_after_start && hw.registers[0x15c/4]==0x30006);cases++;
    reset(&state);
    assert(gdp_power(&io,&state,2)==GDP_ARGUMENT && !hw.writes);
    assert(gdp_power(0,&state,0)==GDP_ARGUMENT);cases++;
    printf("Native display power: %u DCS on/off, W1C preservation, inherited-state, duplicate, stale-done, fault and restoration cases passed\n",cases);
    return 0;
}
