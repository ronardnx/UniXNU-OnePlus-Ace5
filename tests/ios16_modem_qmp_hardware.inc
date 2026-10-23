/* Low-level AOP/MMIO replacement inside the single modem system scenario.
 * gq_power, lease callbacks, qpp planning and qpt transport are production code. */
static struct { uint32_t ram[QPT_MSGRAM_BYTES/4]; uint64_t now;
    unsigned consumed,ack_close; } modemAop;
static void modem_aop_init(void)
{
    memset(&modemAop,0,sizeof(modemAop));modemAop.ack_close=1;
    modemAop.ram[0]=0x4d41494c;modemAop.ram[1]=1;
    for(unsigned offset:{0x0cu,0x10u,0x14u,0x18u,0x24u,0x28u})modemAop.ram[offset/4]=QPT_UP;
    modemAop.ram[0x1c/4]=0x68;modemAop.ram[0x20/4]=0xc0;
    modemAop.ram[0x2c/4]=modemAop.ram[0x30/4]=QPT_DOWN;
    modemAop.ram[0x34/4]=0x2d8;modemAop.ram[0x38/4]=0x128;
}
static uintptr_t gw_map_region(giulia_wifi_runtime *,unsigned region)
{ assert(region==GWMAP_QMP || region==GWMAP_IPCC);return 1; }
static uint32_t modem_aop_read(void *,uint32_t offset)
{ assert(!(offset&3) && offset<QPT_MSGRAM_BYTES);return modemAop.ram[offset/4]; }
static void modem_aop_write(void *v,uint32_t offset,uint32_t value)
{
    assert(gqp_owns(v,&gw_qmp.state));assert(!(offset&3) && offset<QPT_MSGRAM_BYTES);
    assert(offset==0x2c || offset==0x18 || offset>=0x128);
    modemAop.ram[offset/4]=value;
}
static void modem_aop_barrier(void *) {}
static void modem_aop_kick(void *v)
{
    assert(gqp_owns(v,&gw_qmp.state));
    if(modemAop.ram[0x2c/4]==QPT_UP) {
        modemAop.ram[0x14/4]=QPT_UP;
        if(modemAop.ram[0x18/4]==QPT_UP)modemAop.ram[0x30/4]=QPT_UP;
    } else if(modemAop.ack_close) {
        modemAop.ram[0x14/4]=QPT_DOWN;modemAop.ram[0x30/4]=QPT_DOWN;
    }
    if(modemAop.ram[0x128/4]) {
        assert(modemAop.ram[0x128/4]==QPP_COMMAND_BYTES);
        modemAop.ram[0x128/4]=0;modemAop.consumed++;
    }
}
static uint64_t modem_aop_now(void *) { return modemAop.now; }
static void modem_aop_delay(void *,unsigned us) { modemAop.now+=us; }
static struct qpt_io gqp_io(giulia_wifi_runtime *r)
{ return {r,QPT_MSGRAM_BYTES,modem_aop_read,modem_aop_write,modem_aop_barrier,
    modem_aop_kick,modem_aop_now,modem_aop_delay,gqp_claim,gqp_owns,gqp_release}; }

extern "C" int giulia_platform_qmp_available(void) { return 1; }
