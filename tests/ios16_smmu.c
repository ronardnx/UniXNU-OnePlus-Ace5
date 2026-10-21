#include <assert.h>
#include <string.h>
#include "../ios16/giulia_smmu.h"
static uint32_t regs[0x100000/4];
static unsigned writes;
static uint32_t read_reg(unsigned at) { assert(at<sizeof(regs) && !(at&3)); return regs[at/4]; }
static void write_reg(unsigned at,uint32_t v) { assert(at<sizeof(regs) && !(at&3)); regs[at/4]=v; writes++; }
static void setup(void) {
 memset(regs,0,sizeof(regs)); writes=0;
 regs[0x20/4]=0x4c017e6b; regs[0x24/4]=0x60000047; regs[0x28/4]=0x5111;
 regs[0x808/4]=0x80000060; regs[0xc08/4]=2;
 regs[0x82000/4]=0x12345679; /* live firmware bank must stay untouched */
}
int main(void) {
 setup(); assert(gs_plan(read_reg)==0);
 assert(gs_take(read_reg,write_reg,0)!=0 && writes==0);
 assert(gs_take(read_reg,write_reg,1)==0);
 assert(regs[0xc08/4]==70 && regs[0x82000/4]==0x12345679);
 assert(gs_restore(read_reg,write_reg,0)!=0);
 assert(gs_restore(read_reg,write_reg,1)==0 && regs[0xc08/4]==2);
 setup(); regs[0x804/4]=0x80000040; regs[0xc04/4]=70;
 assert(gs_take(read_reg,write_reg,1)!=0 && writes==0);
 setup(); regs[0x804/4]=0x80000060;
 assert(gs_take(read_reg,write_reg,1)!=0 && writes==0);
 setup(); regs[0xc6000/4]=1;
 assert(gs_take(read_reg,write_reg,1)!=0 && writes==0);
 setup(); regs[0x24/4]=0x60000046;
 assert(gs_take(read_reg,write_reg,1)!=0 && writes==0);
 return 0;
}
