/* Synthetic register/SMEM peer tests. These do not assert physical boot. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ios16/giulia_audio_native.h"

struct fixture {
    struct gan_native native;
    struct gan_lease lease;
    uint8_t *smem,*dtb,*adsp;
    volatile uint8_t *in;
    unsigned allow,maps,unmaps,writes,reads,barriers,ipcc0,ipcc2,map_fail;
    unsigned owner,busy,fail_read_after_write,fail_ipcc,fail_delay;
    unsigned fault_checks,fault_after_checks;
    int fault_status;
    uint64_t delayed_us,publish_at,fault_at;
};
static int lease(void *p,struct gan_lease *l)
{struct fixture *f=(struct fixture *)p;*l=f->lease;return f->allow?1:0;}
static int latched_fault(void *p)
{
    struct fixture *f=(struct fixture *)p;
    assert(f->allow&&(f->lease.proofs&GAN_PROOF_REQUIRED)==GAN_PROOF_REQUIRED);
    f->fault_checks++;
    if(f->fault_after_checks&&f->fault_checks>=f->fault_after_checks)f->fault_status=GAN_FAULT_WATCHDOG;
    return f->fault_status;
}
static volatile void *map_nc(void *p,uint64_t pa,size_t bytes)
{
    struct fixture *f=(struct fixture *)p;
    if(++f->maps==f->map_fail)return 0;
    if(pa==GAN_SMEM_PHYSICAL&&bytes==GAN_SMEM_BYTES)return f->smem;
    if(pa==GAB_DTB_PHYSICAL&&bytes==GAB_DTB_BYTES)return f->dtb;
    if(pa==GAB_ADSP_PHYSICAL&&bytes==GAB_ADSP_BYTES)return f->adsp;
    assert(0);return 0;
}
static void unmap_nc(void *p,volatile void *address,size_t bytes)
{
    struct fixture *f=(struct fixture *)p;
    assert((address==f->smem&&bytes==GAN_SMEM_BYTES)||
        (address==f->dtb&&bytes==GAB_DTB_BYTES)||(address==f->adsp&&bytes==GAB_ADSP_BYTES));
    f->unmaps++;
}
static int read32(void *p,uint64_t pa,uint32_t *value)
{
    struct fixture *f=(struct fixture *)p;f->reads++;assert(pa==GAN_MUTEX_PHYSICAL);
    if(f->fail_read_after_write&&f->writes)return -1;
    *value=f->owner;return 0;
}
static int write32(void *p,uint64_t pa,uint32_t value)
{
    struct fixture *f=(struct fixture *)p;f->writes++;
    if(pa==GAN_MUTEX_PHYSICAL) {
        assert(value==0||value==1);
        if(value==1&&!f->busy&&!f->owner)f->owner=1;
        if(!value) {assert(f->owner==1);f->owner=0;}
    } else {
        assert(pa==UINT64_C(0x40600c));
        assert(value==0x30000||value==0x30002);
        if(f->fail_ipcc)return -1;
        if(value==0x30000)f->ipcc0++;else f->ipcc2++;
    }
    return 0;
}
static void barrier(void *p) {((struct fixture *)p)->barriers++;}
static int delay_us(void *p,unsigned us)
{
    struct fixture *f=(struct fixture *)p;assert(us&&us<=1000);f->delayed_us+=us;
    if(f->fail_delay)return -1;
    if(f->fault_at&&f->delayed_us>=f->fault_at)f->fault_status=GAN_FAULT_WATCHDOG;
    if(f->publish_at&&f->delayed_us>=f->publish_at) {
        assert(f->in);gat_put32(f->in+36,GAT_SMP2P_READY);
    }
    return 0;
}
static void create(struct fixture *f)
{
    const struct gan_ops ops={f,lease,latched_fault,map_nc,unmap_nc,read32,write32,barrier,delay_us,0};
    memset(f,0,sizeof(*f));
    f->smem=(uint8_t *)calloc(1,GAN_SMEM_BYTES);
    f->dtb=(uint8_t *)calloc(1,GAB_DTB_BYTES);f->adsp=(uint8_t *)calloc(1,GAB_ADSP_BYTES);
    assert(f->smem&&f->dtb&&f->adsp);f->allow=1;f->lease.epoch=42;
    f->lease.proofs=GAN_PROOF_REQUIRED;f->lease.occupied_count=1;
    f->lease.occupied[0].physical=0x84000000;f->lease.occupied[0].bytes=0x100000;
    uint8_t *toc=f->smem+GAN_SMEM_BYTES-4096,*e=toc+32,*part=f->smem+4096;
    size_t part_bytes=GAN_SMEM_BYTES-8192;
    gat_put32(toc,0x434f5424);gat_put32(toc+4,1);gat_put32(toc+8,1);
    gat_put32(e,4096);gat_put32(e+4,(uint32_t)part_bytes);
    gat_put16(e+12,0);gat_put16(e+14,2);gat_put32(e+16,32);
    gat_put32(part,0x54525024);gat_put16(part+4,0);gat_put16(part+6,2);
    gat_put32(part+8,(uint32_t)part_bytes);gat_put32(part+12,32);gat_put32(part+16,(uint32_t)part_bytes);
    assert(gan_init(&f->native,&ops)==GAN_OK);
}
static void destroy(struct fixture *f)
{free(f->smem);free(f->dtb);free(f->adsp);}
static volatile uint8_t *item(struct fixture *f,unsigned id,size_t expected)
{
    volatile void *p=0;size_t bytes=0;
    assert(gat_smem_get(&f->native.partition,id,&p,&bytes)==GAT_OK&&bytes==expected);
    return (volatile uint8_t *)p;
}
static volatile uint8_t *peer_alloc(struct fixture *f,unsigned id,size_t bytes)
{
    /* Simulate the other processor's correctly serialized SMEM allocation. */
    assert(gat_smem_alloc_locked(&f->native.partition,id,bytes,barrier,f)==GAT_OK);
    return item(f,id,bytes);
}
static void inbound(struct fixture *f,uint32_t state)
{
    f->in=peer_alloc(f,443,GAT_SMP2P_BYTES);
    assert(gat_smp2p_init_outbound(f->in,GAT_SMP2P_BYTES,barrier,f,1)==GAT_OK);
    gat_put16(f->in+8,2);gat_put16(f->in+10,0);
    for(unsigned i=0;i<16;i++)f->in[20+i]=0;
    const char name[]="slave-kernel";
    for(unsigned i=0;i<sizeof(name);i++)f->in[20+i]=(uint8_t)name[i];
    gat_put32(f->in+36,state);
}
static void boot_ready(struct fixture *f)
{
    create(f);assert(gan_preflight(&f->native)==GAN_OK);inbound(f,0);
    assert(gan_prepare(&f->native)==GAN_OK&&gan_auth_complete(&f->native)==GAN_OK);
    gat_put32(f->in+36,GAT_SMP2P_READY);assert(gan_wait_ready(&f->native,5000)==GAN_OK);
}
static void peer_frame(struct fixture *f,unsigned cmd,unsigned cid,uint32_t p2,const uint8_t *data,size_t bytes)
{
    volatile uint8_t *d=item(f,478,32),*rx=item(f,480,16384);
    uint32_t head=gat_le32(d+12);uint8_t h[8];gat_put16(h,(uint16_t)cmd);
    gat_put16(h+2,(uint16_t)cid);gat_put32(h+4,p2);
    for(size_t i=0;i<gat_align8(8+bytes);i++)rx[(head+i)%16384]=i<8?h[i]:i<8+bytes?data[i-8]:0;
    gat_put32(d+12,(uint32_t)((head+gat_align8(8+bytes))%16384));
}
static void peer_drain(struct fixture *f)
{
    assert(gan_transport_poll(&f->native,64)>=0);
    volatile uint8_t *d=item(f,478,32);gat_put32(d,gat_le32(d+4));
}
static void test_preflight(void)
{
    struct fixture f;create(&f);f.allow=0;
    assert(gan_preflight(&f.native)==GAN_LEASE&&!f.maps&&!f.writes);
    f.allow=1;f.lease.proofs&=~GAN_PROOF_POWER_CLOCKS;
    assert(gan_preflight(&f.native)==GAN_LEASE&&!f.maps&&!f.writes);
    f.lease.proofs=GAN_PROOF_REQUIRED;f.lease.occupied[0].physical=GAB_ADSP_PHYSICAL;
    assert(gan_preflight(&f.native)==GAN_RANGE&&!f.maps&&!f.writes);
    f.lease.occupied[0].physical=UINT64_MAX-1;f.lease.occupied[0].bytes=8;
    assert(gan_preflight(&f.native)==GAN_RANGE&&!f.maps);
    f.lease.occupied[0].physical=0x84000000;f.lease.occupied[0].bytes=0x100000;
    f.map_fail=2;assert(gan_preflight(&f.native)==GAN_MAP&&f.unmaps==1&&!f.native.mapped&&!f.writes);
    f.map_fail=0;assert(gan_preflight(&f.native)==GAN_OK&&!f.writes);
    unsigned maps=f.maps;assert(gan_preflight(&f.native)==GAN_OK&&f.maps==maps&&!f.writes);
    assert(gan_prepare(&f.native)==GAN_OK&&f.ipcc2==1&&f.owner==0);
    assert(gan_preflight(&f.native)==GAN_STATE);
    destroy(&f);
}
static void test_lifecycle_and_transport(void)
{
    struct fixture f;create(&f);struct gan_native *n=&f.native;
    assert(gan_prepare(n)==GAN_STATE&&gan_auth_complete(n)==GAN_STATE&&!f.writes);
    assert(gan_preflight(n)==GAN_OK);inbound(&f,0);
    volatile uint8_t *d=peer_alloc(&f,478,32);gat_put32(d,64);gat_put32(d+4,72);gat_put32(d+8,80);gat_put32(d+12,88);
    assert(gan_prepare(n)==GAN_OK&&f.ipcc2==1&&!f.ipcc0);
    assert(gat_le32(d)==64&&!gat_le32(d+4)&&!gat_le32(d+8)&&gat_le32(d+12)==88);
    volatile void *rx=0;size_t rb=0;assert(gat_smem_get(&n->partition,480,&rx,&rb)==GAT_WAIT);
    const uint8_t data[4]={1,2,3,4};
    assert(gan_memory_write(n,GAB_DTB_PHYSICAL,data,4)==GAN_OK&&!memcmp(f.dtb,data,4));
    assert(gan_memory_write(n,GAB_ADSP_PHYSICAL+16,data,4)==GAN_OK&&!memcmp(f.adsp+16,data,4));
    assert(gan_memory_write(n,GAB_ADSP_PHYSICAL+16,0,4)==GAN_OK&&f.adsp[16]==0);
    assert(gan_memory_write(n,GAB_ADSP_PHYSICAL+GAB_ADSP_BYTES-1,data,4)==GAN_RANGE);
    assert(gan_memory_write(n,UINT64_MAX-1,data,4)==GAN_RANGE);
    assert(gan_memory_write(n,GAN_SMEM_PHYSICAL,data,4)==GAN_RANGE);
    assert(gan_memory_sync(n,GAB_ADSP_PHYSICAL,32)==GAN_OK);
    assert(gan_wait_ready(n,5000)==GAN_STATE);
    assert(gan_auth_complete(n)==GAN_OK&&gan_auth_complete(n)==GAN_STATE);
    assert(gan_memory_write(n,GAB_ADSP_PHYSICAL,data,4)==GAN_STATE);
    f.publish_at=2000;assert(gan_wait_ready(n,5000)==GAN_OK&&f.delayed_us==2000);
    struct gat_transport external;
    assert(gan_ipc_open(n,&external)==GAT_OK&&n->active_transport==&external);
    /* Remote resets its owned indices when it boots. AP never does this. */
    gat_put32(d,0);gat_put32(d+12,0);
    assert(gan_transport_poll(n,32)==GAT_WAIT&&external.started&&!external.rx.fifo);
    peer_alloc(&f,480,16384);assert(gan_transport_poll(n,32)==GAT_WAIT&&external.rx.fifo);
    peer_frame(&f,0,1,0,0,0);peer_frame(&f,1,1,0,0,0);peer_drain(&f);
    peer_frame(&f,2,9,10,(const uint8_t *)"adsp_apps",10);peer_frame(&f,4,1,0,0,0);peer_drain(&f);
    assert(gat_channel_ready(&external));
    uint8_t intent[8];gat_put32(intent,4096);gat_put32(intent+4,55);
    peer_frame(&f,5,9,1,intent,8);peer_drain(&f);
    assert(gat_spf_query(&external,88)==GAT_OK);
    uint8_t response[36];memset(response,0,sizeof(response));gat_put32(response,28);
    gat_put32(response+8,(28u<<8)|(6u<<4));response[12]=3;response[13]=2;
    gat_put32(response+16,1);gat_put32(response+20,3);gat_put32(response+24,88);
    gat_put32(response+28,GAT_SPF_RESPONSE);gat_put32(response+32,1);
    peer_frame(&f,9,9,external.liid,response,sizeof(response));peer_drain(&f);
    assert(external.spf_ready&&n->ready&&f.ipcc0>0);
    assert(gan_release(n)==GAN_STATE&&!f.unmaps);
    assert(gan_shutdown_complete(n)==GAN_OK&&!external.spf_ready);
    assert(gan_release(n)==GAN_OK&&f.unmaps==3&&!n->mapped);
    assert(gan_preflight(n)==GAN_LEASE); /* retired boot lease cannot be reused */
    f.lease.epoch++;assert(gan_preflight(n)==GAN_OK);assert(gan_release(n)==GAN_OK);
    destroy(&f);
}
static void test_stale_ready_and_faults(void)
{
    struct fixture f;create(&f);assert(gan_preflight(&f.native)==GAN_OK);inbound(&f,GAT_SMP2P_READY);
    assert(gan_prepare(&f.native)==GAN_OK&&gan_auth_complete(&f.native)==GAN_OK);
    assert(gan_wait_ready(&f.native,3)==GAN_TIMEOUT&&f.delayed_us==3000&&!f.native.ready);
    assert(gan_release(&f.native)==GAN_STATE&&!f.unmaps);destroy(&f);
    boot_ready(&f);gat_put32(f.in+36,GAT_SMP2P_FATAL|GAT_SMP2P_READY);
    assert(gan_transport_poll(&f.native,32)==GAT_CLOSED&&!f.native.ready);destroy(&f);
    boot_ready(&f);unsigned writes=f.writes;f.lease.epoch++;
    assert(gan_transport_poll(&f.native,32)==GAN_LEASE&&!f.native.ready&&f.writes==writes);destroy(&f);
    boot_ready(&f);f.fail_ipcc=1;
    assert(gan_transport_poll(&f.native,32)==GAN_IO&&!f.native.ready);destroy(&f);
}
static void test_hardware_lock_failures(void)
{
    struct fixture f;create(&f);assert(gan_preflight(&f.native)==GAN_OK);
    f.owner=2;f.busy=1;uint32_t before=gat_le32(f.native.partition.base+12);
    assert(gan_prepare(&f.native)==GAN_TIMEOUT&&f.delayed_us==GAN_LOCK_TIMEOUT_US);
    assert(f.owner==2&&gat_le32(f.native.partition.base+12)==before&&!f.ipcc0&&!f.ipcc2);destroy(&f);
    create(&f);assert(gan_preflight(&f.native)==GAN_OK);f.owner=1;
    assert(gan_prepare(&f.native)==GAN_LEASE&&!f.writes&&f.owner==1);destroy(&f);
    create(&f);assert(gan_preflight(&f.native)==GAN_OK);f.fail_read_after_write=1;
    assert(gan_prepare(&f.native)==GAN_IO&&f.native.hwlock_owned==2&&f.owner==1);
    assert(gan_release(&f.native)==GAN_STATE&&!f.unmaps);destroy(&f);
    create(&f);assert(gan_preflight(&f.native)==GAN_OK);peer_alloc(&f,478,40);
    assert(gan_preflight(&f.native)==GAT_INVALID&&!f.writes&&!f.native.mapped);destroy(&f);
    /* A corruption appearing after preflight is still rejected under lock. */
    create(&f);assert(gan_preflight(&f.native)==GAN_OK);peer_alloc(&f,478,40);
    assert(gan_prepare(&f.native)==GAT_INVALID&&!f.owner&&!f.ipcc0&&!f.ipcc2);destroy(&f);
}
static void test_latched_irq_faults(void)
{
    struct fixture f;struct gan_native missing;create(&f);
    struct gan_ops incomplete=f.native.ops;incomplete.latched_fault=0;
    assert(gan_init(&missing,&incomplete)==GAN_ARGUMENT);
    f.allow=0;f.fault_status=GAN_FAULT_WATCHDOG;
    assert(gan_preflight(&f.native)==GAN_LEASE&&!f.fault_checks&&!f.writes&&!f.maps);
    f.allow=1;
    assert(gan_preflight(&f.native)==GAN_FAULT&&!f.writes&&!f.maps);
    assert(f.native.fault_bits==GAN_FAULT_WATCHDOG&&f.native.error==GAN_FAULT);
    f.fault_status=0;assert(gan_preflight(&f.native)==GAN_STATE);destroy(&f);

    create(&f);assert(gan_preflight(&f.native)==GAN_OK&&gan_prepare(&f.native)==GAN_OK);
    unsigned writes=f.writes;const uint8_t data[4]={1,2,3,4};
    f.fault_status=GAN_FAULT_FATAL|GAN_FAULT_WATCHDOG;
    assert(gan_memory_write(&f.native,GAB_ADSP_PHYSICAL,data,4)==GAN_FAULT);
    assert(!f.adsp[0]&&!f.adsp[1]&&f.writes==writes&&f.native.fault_bits==3);
    assert(gan_release(&f.native)==GAN_STATE&&!f.unmaps);destroy(&f);

    create(&f);assert(gan_preflight(&f.native)==GAN_OK&&gan_prepare(&f.native)==GAN_OK);
    f.fault_status=-7;writes=f.writes;
    assert(gan_memory_sync(&f.native,GAB_ADSP_PHYSICAL,4)==GAN_IO&&f.writes==writes);
    assert(!f.native.ready&&f.native.error==GAN_IO);destroy(&f);

    create(&f);assert(gan_preflight(&f.native)==GAN_OK);inbound(&f,0);
    assert(gan_prepare(&f.native)==GAN_OK&&gan_auth_complete(&f.native)==GAN_OK);
    f.fault_at=2000;writes=f.writes;
    assert(gan_wait_ready(&f.native,5000)==GAN_FAULT&&f.delayed_us==2000&&f.writes==writes);
    assert(!f.native.ready&&f.native.fault_bits==GAN_FAULT_WATCHDOG);destroy(&f);

    boot_ready(&f);struct gat_transport external;
    assert(gan_ipc_open(&f.native,&external)==GAT_OK);
    external.spf_ready=external.spf_pending=1;
    volatile uint8_t *desc=item(&f,478,32);uint32_t head=gat_le32(desc+4);
    f.fault_status=GAN_FAULT_WATCHDOG;writes=f.writes;
    assert(gan_transport_poll(&f.native,32)==GAN_FAULT&&f.writes==writes);
    assert(gat_le32(desc+4)==head&&!external.spf_ready&&!external.spf_pending);
    assert(external.error==GAN_FAULT&&!f.native.ready);
    /* An already-observed IRQ fault also blocks later direct GPR calls. */
    assert(gat_spf_query(&external,200)==GAN_FAULT&&f.writes==writes);
    destroy(&f);

    /* Fault arriving specifically in the IPCC callback must propagate back
     * through GLINK before the incoming frame is acknowledged/consumed. */
    boot_ready(&f);peer_alloc(&f,480,16384);assert(gan_transport_poll(&f.native,32)==GAT_WAIT);
    peer_frame(&f,0,1,0,0,0);desc=item(&f,478,32);uint32_t tail=gat_le32(desc+8);
    f.fault_after_checks=f.fault_checks+4;writes=f.writes;
    assert(gan_transport_poll(&f.native,32)==GAN_FAULT&&f.writes==writes);
    assert(gat_le32(desc+8)==tail&&!f.native.ready);destroy(&f);

    create(&f);assert(gan_preflight(&f.native)==GAN_OK);f.fault_status=GAN_FAULT_WATCHDOG;
    assert(gan_notify(&f.native,2)==GAN_FAULT&&!f.writes&&!f.ipcc2);destroy(&f);

    create(&f);assert(gan_preflight(&f.native)==GAN_OK);f.owner=2;f.busy=1;f.fault_at=100;
    assert(gan_prepare(&f.native)==GAN_FAULT&&f.delayed_us==100&&f.writes==1);
    assert(f.owner==2&&!f.native.hwlock_owned);destroy(&f);

    /* Fault after lock acquisition, before the first shared allocation: no
     * further MMIO or shared-memory write, and retained uncertain resources. */
    create(&f);assert(gan_preflight(&f.native)==GAN_OK);
    uint32_t allocated=gat_le32(f.native.partition.base+12);
    f.fault_after_checks=f.fault_checks+4;
    assert(gan_prepare(&f.native)==GAN_FAULT&&f.writes==1&&f.owner==1);
    assert(f.native.hwlock_owned==1&&gat_le32(f.native.partition.base+12)==allocated);
    assert(gan_release(&f.native)==GAN_STATE&&!f.unmaps);destroy(&f);
}
int main(void)
{
    test_preflight();test_lifecycle_and_transport();test_stale_ready_and_faults();test_hardware_lock_failures();
    test_latched_irq_faults();
    puts("audio native: lease, carveout, hwlock, IPCC, IRQ faults, fresh-ready and late GLINK/SPF tests passed");return 0;
}
