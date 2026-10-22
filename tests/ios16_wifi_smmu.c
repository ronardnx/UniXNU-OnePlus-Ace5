#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ios16/giulia_wifi_smmu.h"
static uint32_t registers[0x100000/4],original[0x100000/4];
static uint64_t tables[0x4000/8];
static unsigned writes,drop,quiet,owned,tlb_timeout,delay_us,readback_quirks;
static uint64_t table_pa=0x8c0010000ull,arena_pa=0x8d0000000ull;
static uint32_t read_reg(void *p,unsigned off) {
    (void)p;assert(off<sizeof(registers) && !(off&3));
    if(off>=0x80000 && (off&0xfff)==0x7f4 && tlb_timeout)return 1;
    return registers[off/4];
}
static void write_reg(void *p,unsigned off,uint32_t v) {
    (void)p;assert(off<sizeof(registers) && !(off&3));
    assert(off!=0 && off!=0x70 && off!=0x68); /* no global disable/invalidate */
    assert(off!=0xc08 && off!=0x808 && off!=gws_cb(2) && off!=gws_cb(70));
    if(off==0x1114)assert(v==0x1f300); /* hardware aborts on reset/type-2 CBAR=0 */
    if(++writes==drop)return;
    if(off>=0x80000 && ((off&0xfff)==0x610 || (off&0xfff)==0x7f0))return;
    if(readback_quirks && off==gws_cb(69))v&=~0x1000u;
    if(readback_quirks && off==gws_cb(69)+0x10)v|=0x60u;
    registers[off/4]=v;
}
static void delay(void *p,unsigned us){(void)p;delay_us+=us;assert(delay_us<=20000);}
static int quiescent(void *p){(void)p;return quiet;}
static int owns(void *p,uint64_t pa,size_t bytes) {
    (void)p;return owned && ((pa==table_pa && bytes==0x4000) || (pa==arena_pa && bytes==0x4000000));
}
static void clean(void *p,void *data,size_t n){(void)p;assert(data==tables && n==sizeof(tables));}
static const struct gws_io io={0,read_reg,write_reg,delay,quiescent,owns,clean};
static void init(void) {
    memset(registers,0,sizeof(registers));memset(tables,0xaa,sizeof(tables));
    registers[0x20/4]=0x4c017e6b;registers[0x24/4]=0x60000047;registers[0x28/4]=0x5111;
    registers[0x804/4]=0x80000040;registers[0xc04/4]=1;
    registers[0x808/4]=0x80000060;registers[0xc08/4]=2;
    registers[gws_cb(2)/4]=1;registers[gws_cb(70)/4]=1;
    memcpy(original,registers,sizeof(registers));writes=drop=tlb_timeout=delay_us=readback_quirks=0;quiet=owned=1;
}
static int run(struct gws_state *s) {
    memset(s,0,sizeof(*s));return gws_take(&io,s,tables,table_pa,arena_pa,0x98000000,0x4000000);
}
static int restored(void) {
    assert(registers[gws_cb(69)/4]==0);
    for(unsigned i=0;i<sizeof(registers)/sizeof(registers[0]);i++) {
        unsigned off=i*4;
        if((off>=gws_cb(69) && off<gws_cb(69)+0x1000) || off==0x1114 || off==0x1914)continue;
        assert(registers[i]==original[i]);
    }
    return 1;
}
static uint64_t translate(uint64_t iova) {
    if(iova>=0x100000000ull)return UINT64_MAX;
    uint64_t root=tables[iova>>30];
    if((root&3)!=3 || (root&0x0000fffffffff000ull)!=table_pa+0x1000)return UINT64_MAX;
    uint64_t leaf=tables[512+((iova>>21)&511)];
    if((leaf&3)!=1)return UINT64_MAX;
    assert((leaf&(3ull<<53))==(3ull<<53));
    return (leaf&0x0000ffffffe00000ull)|(iova&0x1fffff);
}
int main(void) {
    struct gws_state s;unsigned cases=0,total;
    init();assert(run(&s)==GWS_OK && gws_live(&io,&s) && s.cb==69 && s.slot==0);total=writes;cases++;
    assert(translate(0x98000000)==arena_pa && translate(0x9bffffff)==arena_pa+0x3ffffff);
    assert(translate(0x97ffffff)==UINT64_MAX && translate(0x9c000000)==UINT64_MAX);
    assert(translate(table_pa)==UINT64_MAX);cases++;
    assert(gws_restore(&io,&s)==GWS_OK && restored());cases++;
    init();readback_quirks=1;assert(run(&s)==GWS_OK && gws_live(&io,&s));
    assert(registers[gws_cb(69)/4]==0x127 && registers[(gws_cb(69)+0x10)/4]==0x38071);
    registers[gws_cb(69)/4]&=~1u;assert(!gws_live(&io,&s)); /* M must still be set. */
    registers[gws_cb(69)/4]|=1u;registers[(gws_cb(69)+0x10)/4]^=1u;
    assert(!gws_live(&io,&s)); /* PASIZE must still match. */
    assert(gws_restore(&io,&s)==GWS_OK && restored());cases++;
    for(unsigned n=1;n<=total;n++) {
        init();drop=n;int result=run(&s);
        if(result==GWS_OK)assert(gws_live(&io,&s));
        drop=0;assert(gws_restore(&io,&s)==GWS_OK);
        assert(restored());cases++;
    }
    init();quiet=0;assert(run(&s)==GWS_NOT_QUIESCENT && !writes);cases++;
    init();owned=0;assert(run(&s)==GWS_ARGUMENT && !writes);cases++;
    init();registers[0x20/4]^=1;assert(run(&s)==GWS_GEOMETRY && !writes);cases++;
    init();registers[0x800/4]=0x800f1400;assert(run(&s)==GWS_SHARED_STREAM && !writes);cases++;
    init();registers[0x800/4]=0x80001401;registers[0x80c/4]=0x80001401;
    assert(run(&s)==GWS_SHARED_STREAM && !writes);cases++;
    init();for(unsigned i=0;i<107;i++)registers[(0x800+i*4)/4]=0x80002000+i;
    assert(run(&s)==GWS_NO_STREAM && !writes);cases++;
    init();for(unsigned i=4;i<70;i++)registers[gws_cb(i)/4]=1;
    assert(run(&s)==GWS_NO_CONTEXT && !writes);cases++;
    init();tlb_timeout=1;assert(run(&s)==GWS_TLB_TIMEOUT && s.quarantined && s.changed);cases++;
    tlb_timeout=0;assert(gws_restore(&io,&s)==GWS_OK);assert(restored());
    init();assert(run(&s)==GWS_OK);quiet=0;unsigned before=writes;
    assert(gws_restore(&io,&s)==GWS_NOT_QUIESCENT && s.quarantined && writes==before);cases++;
    quiet=1;assert(gws_restore(&io,&s)==GWS_OK);
    init();assert(run(&s)==GWS_OK);registers[0x800/4]=0x80001234;before=writes;
    assert(!gws_live(&io,&s) && gws_restore(&io,&s)==GWS_BUSY && writes==before);cases++;
    init();registers[0x800/4]=0x80011400;registers[0xc00/4]=0x10000;
    memcpy(original,registers,sizeof(registers));assert(run(&s)==GWS_OK && gws_live(&io,&s));
    assert(gws_restore(&io,&s)==GWS_OK && restored());cases++;
    init();memset(&s,0,sizeof(s));assert(gws_take(&io,&s,tables,arena_pa,arena_pa,0x98000000,0x4000000)==GWS_ARGUMENT && !writes);cases++;
    init();assert(run(&s)==GWS_OK);registers[(gws_cb(69)+0x30)/4]=0x803500;
    assert(!gws_live(&io,&s) && s.check_offset==gws_cb(69)+0x30 &&
           s.check_value==0x803500 && s.check_expected==0x803520);
    assert(gws_restore(&io,&s)==GWS_OK && restored());cases++;
    printf("Wi-Fi SMMU: %u table-isolation, stream-ownership, rollback and timeout cases passed\n",cases);
}
