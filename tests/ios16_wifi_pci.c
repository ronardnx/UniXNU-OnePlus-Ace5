/* PCI register model: BAR probes, command W1C semantics and dropped writes. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/wifi/giulia_wifi_pci.h"
static struct {
    uint32_t ep[1024],root[1024],atu[1024],parf[0x3000/4],masklo,maskhi;
    unsigned probe0,probe1,writes,drop,timeout,delay,allow_master;
} h,initial;
static uint32_t read_reg(void *context,unsigned space,unsigned offset) {
    (void)context;assert(!(offset&3) && offset<(space==GWP_PARF?0x3000u:4096u));
    if(space==GWP_PARF)return h.parf[offset/4];
    if(space==GWP_ENDPOINT) {
        if(offset==0x10 && h.probe0) return h.masklo;
        if(offset==0x14 && h.probe1) return h.maskhi;
        return h.ep[offset/4];
    }
    if(space==GWP_ROOT) return h.root[offset/4];
    assert(space==GWP_ATU);
    if(offset==0x204 && h.timeout) return 0;
    return h.atu[offset/4];
}
static void write_reg(void *context,unsigned space,unsigned offset,uint32_t value) {
    (void)context;assert(!(offset&3) && offset<(space==GWP_PARF?0x3000u:4096u));
    if(space==GWP_ENDPOINT && offset==4) {
        assert(!(value&4) || h.allow_master); /* enumeration must never grant DMA */
        /* A root port with BME clear rejects upstream DMA/MSI as unsupported. */
        assert(!(value&4) || (h.root[1]&6)==6);
        assert(!(value&0xffff0000u)); /* do not clear W1C status */
    }
    if(space==GWP_ROOT && offset==4) assert(!(value&0xffff0000u));
    if(++h.writes==h.drop) return;
    if(space==GWP_PARF){h.parf[offset/4]=value;return;}
    if(space==GWP_ENDPOINT) {
        if(offset==0x10 || offset==0x14) {
            assert(!(h.ep[1]&7));
            unsigned *probe=offset==0x10?&h.probe0:&h.probe1;
            *probe=value==0xffffffffu;
            if(*probe) return;
        }
        if(offset==4) h.ep[1]=(h.ep[1]&0xffff0000u)|value;
        else h.ep[offset/4]=value;
    } else if(space==GWP_ROOT) {
        if(offset==4) h.root[1]=(h.root[1]&0xffff0000u)|value;
        else h.root[offset/4]=value;
    } else { assert(space==GWP_ATU);h.atu[offset/4]=value; }
}
static void delay(void *context,unsigned us) {(void)context;h.delay+=us;assert(h.delay<=110000);}
static const struct gwp_io io={0,read_reg,write_reg,delay};
static void init(void) {
    memset(&h,0,sizeof(h));h.ep[0]=0x110717cb;h.ep[1]=0x100000;
    h.ep[0x10/4]=4;h.ep[0x34/4]=0x50;h.ep[0x50/4]=0x00807005;h.ep[0x70/4]=0x10;
    h.root[0x20/4]=0xfff0;h.masklo=0xffe00004u;h.maskhi=0xffffffffu;h.parf[0x2c00/4]=0x11;initial=h;
}
static int run(struct gwp_state *s) {return gwp_enumerate(&io,s,0x60300000,0x3d00000);}
static void restored(void) {
    assert(!memcmp(h.ep,initial.ep,sizeof(h.ep)) && !memcmp(h.root,initial.root,sizeof(h.root)));
    assert(!memcmp(h.atu,initial.atu,sizeof(h.atu)) && !h.probe0 && !h.probe1);
    assert(!memcmp(h.parf,initial.parf,sizeof(h.parf)));
}
int main(void) {
    struct gwp_state s;unsigned cases=0,total;
    init();assert(run(&s)==GWP_OK);assert(s.ready && s.bar_base==0x60400000 && s.bar_bytes==0x200000);
    assert(s.msi_cap==0x50 && s.pcie_cap==0x70 && (h.ep[1]&7)==2);total=h.writes;cases++;
    assert(gwp_restore(&io,&s));restored();cases++;
    for(unsigned drop=1;drop<=total;drop++) {
        init();h.drop=drop;int result=run(&s);
        if(result==GWP_OK) {
            assert((h.ep[1]&7)==2 && h.ep[4]==0x60400004 && h.atu[0x214/4]==0x60400000);
            h.drop=0;assert(gwp_restore(&io,&s));
        } else assert(!s.ready && result!=GWP_RESTORE_FAILED);
        restored();cases++;
    }
    init();h.ep[0]=0xffffffffu;assert(run(&s)==GWP_WRONG_DEVICE && !h.writes);cases++;
    init();h.ep[1]|=4;assert(run(&s)==GWP_ACTIVE_DEVICE && !h.writes);cases++;
    init();h.ep[0x50/4]|=0x10000;assert(run(&s)==GWP_BAD_CAPABILITY && !h.writes);cases++;
    init();h.ep[0x70/4]=0x5010;assert(run(&s)==GWP_BAD_CAPABILITY && !h.writes);cases++;
    init();h.ep[0x34/4]=0x51;assert(run(&s)==GWP_BAD_CAPABILITY && !h.writes);cases++;
    init();h.ep[0x50/4]&=~0x800000u;initial=h;
    assert(run(&s)==GWP_OK && !(s.msi_control&0x80));assert(gwp_restore(&io,&s));restored();cases++;
    init();h.atu[0x204/4]=0x80000000u;assert(run(&s)==GWP_ACTIVE_DEVICE && !h.writes);cases++;
    init();h.masklo=0xfffffff4;assert(run(&s)==GWP_BAD_BAR);restored();cases++;
    init();h.masklo=0xfe800004;assert(run(&s)==GWP_BAD_BAR);restored();cases++;
    init();h.masklo=0xf8000004;assert(run(&s)==GWP_BAD_BAR);restored();cases++;
    init();h.timeout=1;assert(run(&s)==GWP_ATU_TIMEOUT);restored();cases++;
    init();assert(gwp_enumerate(&io,&s,0xfffff000,0x2000)==GWP_BAD_ARGUMENT && !h.writes);cases++;
    init();h.ep[4]=0;h.masklo=0xffe00000;initial=h;
    assert(run(&s)==GWP_OK && s.bar_bytes==0x200000 && s.bar_flags==0);assert(gwp_restore(&io,&s));restored();cases++;
    init();assert(run(&s)==GWP_OK);h.ep[1]|=4;unsigned writes=h.writes;
    assert(!gwp_restore(&io,&s) && h.writes==writes && s.restore_failed);cases++;
    struct gwp_sid_state sid={0};struct gwp_msi_state msi={0};
    init();assert(run(&s)==GWP_OK);assert(gwp_bdf_hash(0x100)==0x15);
    assert(gwp_sid_claim(&io,&sid) && h.parf[(0x2000+0x15*4)/4]==0x01000100 && h.parf[0x2c00/4]==0x10);cases++;
    assert(gwp_msi_sink(&io,&s,&msi,0x98000000,0x4000000,0x98006000));
    assert(h.ep[0x54/4]==0x98006000 && h.ep[0x58/4]==0 && (h.ep[0x50/4]&0x710000)==0x10000);cases++;
    writes=h.writes;assert(!gwp_master(&io,&s,1,0) && h.writes==writes);cases++;
    h.allow_master=1;assert(gwp_master(&io,&s,1,1) && (h.ep[1]&4) && (h.root[1]&6)==6);cases++;
    assert(gwp_msi_disable(&io,&msi));assert(gwp_master(&io,&s,0,0));
    assert(gwp_msi_restore(&io,&msi) && gwp_sid_restore(&io,&sid) && gwp_restore(&io,&s));restored();cases++;
    /* Both bridge and endpoint enable writes can fail. A failed bridge write
     * must never enable the endpoint; partial success must remain restorable. */
    for(unsigned drop=1;drop<=2;drop++) {
        init();h.root[1]=0xa5100100u;initial=h;
        assert(run(&s)==GWP_OK);h.allow_master=1;h.drop=h.writes+drop;
        assert(!gwp_master(&io,&s,1,1) && !(h.ep[1]&4));
        assert((h.root[1]&0xffff0000u)==0xa5100000u);
        if(drop==1)assert((h.root[1]&0xffffu)==0x102);
        else assert((h.root[1]&0xffffu)==0x106);
        h.drop=0;assert(gwp_master(&io,&s,0,0) && gwp_restore(&io,&s));restored();cases++;
    }
    init();assert(run(&s)==GWP_OK);h.root[1]=0xffffffffu;writes=h.writes;h.allow_master=1;
    assert(!gwp_master(&io,&s,1,1) && h.writes==writes && !(h.ep[1]&4));cases++;
    /* Preserve a previously enabled bridge, including unrelated Command and
     * sticky Status bits; do not close the upstream path before DMA drains. */
    init();h.root[1]=0xa5100104u;initial=h;assert(run(&s)==GWP_OK);h.allow_master=1;
    assert(gwp_master(&io,&s,1,1) && h.root[1]==0xa5100106u);
    h.ep[(s.pcie_cap+8)/4]=0x200000u;
    assert(!gwp_master(&io,&s,0,0) && !(h.ep[1]&4) && h.root[1]==0xa5100106u);cases++;
    h.ep[(s.pcie_cap+8)/4]=0;
    assert(gwp_master(&io,&s,0,0) && gwp_restore(&io,&s));restored();cases++;
    init();assert(run(&s)==GWP_OK);memset(&sid,0,sizeof(sid));h.parf[(0x2000+0x15*4)/4]=0x02000100;
    writes=h.writes;assert(!gwp_sid_claim(&io,&sid) && h.writes==writes);cases++;
    init();assert(run(&s)==GWP_OK);memset(&msi,0,sizeof(msi));writes=h.writes;
    assert(!gwp_msi_sink(&io,&s,&msi,0x98000000,0x4000000,0x10000000) && h.writes==writes);cases++;
    /* Both PCI MSI layouts, with and without per-vector masking. Verify that
     * a 32-bit device's adjacent capability/pending dword is never overwritten,
     * including failed stores and restoration. */
    for(unsigned wide=0;wide<2;wide++)for(unsigned masked=0;masked<2;masked++) {
        unsigned msi_writes=0;
        for(unsigned drop=0;drop<=msi_writes;drop++) {
            init();h.ep[0x50/4]=0x7005u|(wide<<23)|(masked<<24);
            h.ep[0x54/4]=0x12340000;h.ep[0x58/4]=0xabc00000;
            h.ep[0x5c/4]=0xfffe0001;h.ep[0x60/4]=0xfffffff1;h.ep[0x64/4]=0xfacecafe;
            initial=h;assert(run(&s)==GWP_OK);memset(&msi,0,sizeof(msi));
            writes=h.writes;if(drop)h.drop=writes+drop;
            int ok=gwp_msi_sink(&io,&s,&msi,0x20000000,0x4000000,0x20006000);
            if(!drop) {assert(ok);msi_writes=h.writes-writes;}
            unsigned end=0x50/4+3+wide+masked;
            assert(!memcmp(h.ep+end,initial.ep+end,(0x70/4-end)*4));
            if(ok) {
                assert(h.ep[0x54/4]==0x20006000);
                assert(!(h.ep[0x50/4+2+wide]&0xffffu));
                if(wide)assert(h.ep[0x58/4]==0);
                if(masked)assert(!(h.ep[0x50/4+3+wide]&1));
            }
            h.drop=0;assert(gwp_msi_restore(&io,&msi));assert(gwp_restore(&io,&s));restored();cases++;
        }
    }
    init();h.ep[0x50/4]&=~0x800000u;assert(run(&s)==GWP_OK);memset(&msi,0,sizeof(msi));writes=h.writes;
    assert(!gwp_msi_sink(&io,&s,&msi,0x120000000ull,0x4000000,0x120006000ull) && h.writes==writes);cases++;
    init();h.ep[0x70/4]=0x5010;assert(run(&s)==GWP_BAD_CAPABILITY);
    assert(!strcmp(s.cap_reason,"invalid-or-cyclic-pointer") && s.cap_offset==0x50);cases++;
    init();h.ep[0x50/4]=0x00807001;assert(run(&s)==GWP_BAD_CAPABILITY);
    assert(!strcmp(s.cap_reason,"msi-capability-missing"));cases++;
    assert(!strcmp(gwp_error_name(GWP_BAD_CAPABILITY),"GWP_BAD_CAPABILITY"));
    printf("Wi-Fi PCI enumeration: %u BAR, capability, rollback, ownership and dropped-write cases passed\n",cases);
}
