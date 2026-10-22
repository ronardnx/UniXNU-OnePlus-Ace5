/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * WCN7850 PCI configuration and BAR0 ownership. Enumeration leaves DMA off;
 * gwp_master enables it only after the caller establishes DMA isolation.
 * PCI fields: Linux v6.12 include/uapi/linux/pci_regs.h.
 * The caller owns SM8650 PCIe0 and supplies its captured memory window.
 */
#ifndef GIULIA_WIFI_PCI_H
#define GIULIA_WIFI_PCI_H
#include <stdint.h>
#include <stddef.h>

enum gwp_space { GWP_ENDPOINT, GWP_ROOT, GWP_ATU, GWP_PARF };
enum gwp_error {
    GWP_OK, GWP_BAD_ARGUMENT, GWP_WRONG_DEVICE, GWP_ACTIVE_DEVICE,
    GWP_BAD_CAPABILITY, GWP_BAD_BAR, GWP_NO_SPACE, GWP_WRITE_FAILED,
    GWP_ATU_TIMEOUT, GWP_RESTORE_FAILED
};
static inline const char *gwp_error_name(unsigned error)
{
    static const char *const names[]={"GWP_OK","GWP_BAD_ARGUMENT","GWP_WRONG_DEVICE",
        "GWP_ACTIVE_DEVICE","GWP_BAD_CAPABILITY","GWP_BAD_BAR","GWP_NO_SPACE",
        "GWP_WRITE_FAILED","GWP_ATU_TIMEOUT","GWP_RESTORE_FAILED"};
    return error<sizeof(names)/sizeof(names[0])?names[error]:"GWP_UNKNOWN";
}
struct gwp_io {
    void *context;
    uint32_t (*read)(void *,unsigned,unsigned);
    void (*write)(void *,unsigned,unsigned,uint32_t);
    void (*delay)(void *,unsigned);
};
struct gwp_state {
    uint32_t old_command,old_bar0,old_bar1,old_window,old_root_command;
    uint32_t old_atu[7];
    uint64_t bar_base,bar_bytes;
    uint32_t bar_flags,pcie_cap,msi_cap,msi_control,revision;
    unsigned changed,ready,restore_failed;
    const char *cap_reason;
    uint32_t cap_offset,cap_value,cap_count;
};
static inline uint32_t gwp_read(const struct gwp_io *b,unsigned space,unsigned reg)
{ return b->read(b->context,space,reg); }
static inline void gwp_write(const struct gwp_io *b,unsigned space,unsigned reg,uint32_t value)
{ b->write(b->context,space,reg,value); }

/* Restore only while endpoint DMA is disabled. An outstanding-DMA owner must
 * quiesce the transport first, not free or retarget memory under the device. */
static inline int gwp_restore(const struct gwp_io *b,struct gwp_state *s)
{
    unsigned i;
    if(!s->changed) return 1;
    if(gwp_read(b,GWP_ENDPOINT,0)!=0x110717cbu ||
       (gwp_read(b,GWP_ENDPOINT,4)&4u)) { s->restore_failed=1;return 0; }
    gwp_write(b,GWP_ENDPOINT,4,s->old_command&~7u);
    gwp_write(b,GWP_ATU,0x204,0);
    gwp_write(b,GWP_ENDPOINT,0x10,s->old_bar0);
    if((s->old_bar0&6u)==4u) gwp_write(b,GWP_ENDPOINT,0x14,s->old_bar1);
    gwp_write(b,GWP_ROOT,0x20,s->old_window);
    gwp_write(b,GWP_ROOT,4,s->old_root_command);
    for(i=0;i<7;i++) if(i!=1) gwp_write(b,GWP_ATU,0x200+4*i,s->old_atu[i]);
    gwp_write(b,GWP_ATU,0x204,s->old_atu[1]);
    gwp_write(b,GWP_ENDPOINT,4,s->old_command);
    if((gwp_read(b,GWP_ENDPOINT,4)&0xffffu)!=s->old_command ||
       gwp_read(b,GWP_ENDPOINT,0x10)!=s->old_bar0 ||
       ((s->old_bar0&6u)==4u && gwp_read(b,GWP_ENDPOINT,0x14)!=s->old_bar1) ||
       gwp_read(b,GWP_ROOT,0x20)!=s->old_window ||
       (gwp_read(b,GWP_ROOT,4)&0xffffu)!=s->old_root_command) s->restore_failed=1;
    for(i=0;i<7;i++) if(gwp_read(b,GWP_ATU,0x200+4*i)!=s->old_atu[i]) s->restore_failed=1;
    s->ready=0;
    if(!s->restore_failed) s->changed=0;
    return !s->restore_failed;
}

static inline int gwp_capabilities(const struct gwp_io *b,struct gwp_state *s)
{
    uint64_t visited=0;
    unsigned count=0,offset;
    uint32_t value;
    s->cap_reason="status-cap-list-missing";s->cap_offset=4;
    s->cap_value=gwp_read(b,GWP_ENDPOINT,4);
    if(s->cap_value==0xffffffffu || !(s->cap_value&0x100000u)) return 0;
    s->cap_offset=0x34;s->cap_value=gwp_read(b,GWP_ENDPOINT,0x34);
    offset=s->cap_value&255u;
    while(offset) {
        s->cap_reason="invalid-or-cyclic-pointer";s->cap_offset=offset;
        if(offset<0x40 || offset>0xfc || (offset&3) || ++count>48 ||
           (visited&(1ull<<(offset/4)))) return 0;
        s->cap_count=count;
        visited|=1ull<<(offset/4);
        s->cap_reason="unreadable-capability";
        value=s->cap_value=gwp_read(b,GWP_ENDPOINT,offset);
        if(value==0xffffffffu) return 0;
        if((value&255u)==5) {
            s->cap_reason="duplicate-or-truncated-msi";
            unsigned bytes=(value&0x800000u)?14u:10u;
            if(value&0x1000000u)bytes+=10u; /* mask + pending + alignment */
            if(s->msi_cap || offset>0x100u-bytes) return 0;
            s->msi_cap=offset;s->msi_control=value>>16;
            s->cap_reason="msi-already-enabled";
            if(s->msi_control&1) return 0;
        }
        if((value&255u)==0x10) {
            s->cap_reason="duplicate-or-truncated-pcie";
            if(s->pcie_cap || offset>0xe0) return 0;
            s->pcie_cap=offset;
        }
        offset=(value>>8)&255u;
    }
    s->cap_reason=!s->pcie_cap?"pcie-capability-missing":!s->msi_cap?"msi-capability-missing":"ok";
    return s->pcie_cap && s->msi_cap;
}

static inline int gwp_enumerate(const struct gwp_io *b,struct gwp_state *s,
                                uint64_t window_base,uint64_t window_bytes)
{
    uint32_t command,header,lo,hi=0,expected,readback;
    uint64_t mask,size,base,end;
    unsigned i;int error=GWP_OK;
    if(!b || !s || !b->read || !b->write || !b->delay || !window_bytes ||
       window_base>0xffffffffull || window_bytes>0x100000000ull-window_base)
        return GWP_BAD_ARGUMENT;
    { unsigned char *p=(unsigned char *)s;for(i=0;i<sizeof(*s);i++) p[i]=0; }
    if(gwp_read(b,GWP_ENDPOINT,0)!=0x110717cbu) return GWP_WRONG_DEVICE;
    command=gwp_read(b,GWP_ENDPOINT,4);header=gwp_read(b,GWP_ENDPOINT,0xc);
    if(command==0xffffffffu || header==0xffffffffu || ((header>>16)&0x7fu)) return GWP_WRONG_DEVICE;
    if(command&4u) return GWP_ACTIVE_DEVICE;
    if(!gwp_capabilities(b,s)) return GWP_BAD_CAPABILITY;
    s->old_command=command&0xffffu;s->old_bar0=gwp_read(b,GWP_ENDPOINT,0x10);
    s->old_bar1=gwp_read(b,GWP_ENDPOINT,0x14);s->revision=gwp_read(b,GWP_ENDPOINT,8);
    if((s->old_bar0&1u) || ((s->old_bar0&6u)!=0 && (s->old_bar0&6u)!=4)) return GWP_BAD_BAR;
    s->old_window=gwp_read(b,GWP_ROOT,0x20);
    s->old_root_command=gwp_read(b,GWP_ROOT,4)&0xffffu;
    for(i=0;i<7;i++) {
        s->old_atu[i]=gwp_read(b,GWP_ATU,0x200+4*i);
        if(s->old_atu[i]==0xffffffffu) return GWP_BAD_ARGUMENT;
    }
    if(s->old_window==0xffffffffu || s->old_root_command==0xffffu ||
       (s->old_atu[1]&0x80000000u)) return GWP_ACTIVE_DEVICE;
    s->changed=1;
    /* PCI status bits are W1C: write only the low command halfword. */
    gwp_write(b,GWP_ENDPOINT,4,s->old_command&~7u);
    if(gwp_read(b,GWP_ENDPOINT,4)&7u) { error=GWP_WRITE_FAILED;goto fail; }
    gwp_write(b,GWP_ENDPOINT,0x10,0xffffffffu);
    if((s->old_bar0&6u)==4) gwp_write(b,GWP_ENDPOINT,0x14,0xffffffffu);
    lo=gwp_read(b,GWP_ENDPOINT,0x10);
    if((s->old_bar0&6u)==4) hi=gwp_read(b,GWP_ENDPOINT,0x14);
    gwp_write(b,GWP_ENDPOINT,0x10,s->old_bar0);
    if((s->old_bar0&6u)==4) gwp_write(b,GWP_ENDPOINT,0x14,s->old_bar1);
    if((lo&15u)!=(s->old_bar0&15u)) { error=GWP_BAD_BAR;goto fail; }
    mask=((uint64_t)hi<<32)|(lo&~15u);
    size=(s->old_bar0&6u)==4 ? (~mask)+1 : (uint32_t)(~(uint32_t)mask+1u);
    if(size<4096 || (size&(size-1)) || size>window_bytes) { error=GWP_BAD_BAR;goto fail; }
    base=(window_base+size-1)&~(size-1);end=base+size-1;
    if(base<window_base || base-window_base>window_bytes-size) { error=GWP_NO_SPACE;goto fail; }
    s->bar_flags=s->old_bar0&15u;s->bar_base=base;s->bar_bytes=size;
    gwp_write(b,GWP_ATU,0x204,0);gwp_write(b,GWP_ATU,0x200,0);
    gwp_write(b,GWP_ATU,0x208,(uint32_t)base);gwp_write(b,GWP_ATU,0x20c,0);
    gwp_write(b,GWP_ATU,0x210,(uint32_t)end);gwp_write(b,GWP_ATU,0x214,(uint32_t)base);
    gwp_write(b,GWP_ATU,0x218,0);gwp_write(b,GWP_ATU,0x204,0x80000000u);
    for(i=0;i<100;i++) {
        readback=gwp_read(b,GWP_ATU,0x204);
        if(readback!=0xffffffffu && (readback&0x80000000u)) break;
        b->delay(b->context,100);
    }
    if(i==100) { error=GWP_ATU_TIMEOUT;goto fail; }
    if(gwp_read(b,GWP_ATU,0x200)!=0 || gwp_read(b,GWP_ATU,0x208)!=(uint32_t)base ||
       gwp_read(b,GWP_ATU,0x20c)!=0 || gwp_read(b,GWP_ATU,0x210)!=(uint32_t)end ||
       gwp_read(b,GWP_ATU,0x214)!=(uint32_t)base || gwp_read(b,GWP_ATU,0x218)!=0) {
        error=GWP_WRITE_FAILED;goto fail;
    }
    expected=((uint32_t)(base>>16)&0xfff0u)|((uint32_t)end&0xfff00000u);
    gwp_write(b,GWP_ROOT,0x20,expected);
    gwp_write(b,GWP_ROOT,4,s->old_root_command|2u);
    gwp_write(b,GWP_ENDPOINT,0x10,(uint32_t)base|s->bar_flags);
    if((s->old_bar0&6u)==4) gwp_write(b,GWP_ENDPOINT,0x14,0);
    gwp_write(b,GWP_ENDPOINT,4,(s->old_command&~5u)|2u|0x400u);
    if(gwp_read(b,GWP_ENDPOINT,0x10)!=((uint32_t)base|s->bar_flags) ||
       ((s->old_bar0&6u)==4 && gwp_read(b,GWP_ENDPOINT,0x14)!=0) ||
       (gwp_read(b,GWP_ENDPOINT,4)&7u)!=2 || gwp_read(b,GWP_ROOT,0x20)!=expected ||
       !(gwp_read(b,GWP_ROOT,4)&2u)) {
        error=GWP_WRITE_FAILED;goto fail;
    }
    s->ready=1;return GWP_OK;
fail:
    return gwp_restore(b,s) ? error : GWP_RESTORE_FAILED;
}
struct gwp_sid_state {unsigned changed,slot;uint32_t old_entry,old_config,entry;};
static inline unsigned gwp_bdf_hash(unsigned bdf)
{
    unsigned crc=0;
    for(unsigned byte=0;byte<2;byte++) {
        crc^=(bdf>>(byte?0:8))&255;
        for(unsigned bit=0;bit<8;bit++) crc=((crc<<1)^((crc&128)?7:0))&255;
    }
    return crc;
}
static inline int gwp_sid_restore(const struct gwp_io *b,struct gwp_sid_state *s)
{
    if(!s->changed)return 1;
    if(gwp_read(b,GWP_ENDPOINT,0)!=0x110717cbu || (gwp_read(b,GWP_ENDPOINT,4)&4))return 0;
    uint32_t current=gwp_read(b,GWP_PARF,0x2000+s->slot*4),config=gwp_read(b,GWP_PARF,0x2c00);
    if((current!=s->entry && current!=s->old_entry) ||
       (config!=s->old_config && config!=(s->old_config&~1u)))return 0;
    gwp_write(b,GWP_PARF,0x2000+s->slot*4,s->old_entry);
    gwp_write(b,GWP_PARF,0x2c00,s->old_config);
    if(gwp_read(b,GWP_PARF,0x2000+s->slot*4)!=s->old_entry ||
       gwp_read(b,GWP_PARF,0x2c00)!=s->old_config)return 0;
    s->changed=0;return 1;
}
static inline int gwp_sid_claim(const struct gwp_io *b,struct gwp_sid_state *s)
{
    if(!s || s->changed || gwp_read(b,GWP_ENDPOINT,0)!=0x110717cbu ||
       (gwp_read(b,GWP_ENDPOINT,4)&4))return 0;
    s->slot=gwp_bdf_hash(0x100);
    s->old_entry=gwp_read(b,GWP_PARF,0x2000+s->slot*4);
    s->old_config=gwp_read(b,GWP_PARF,0x2c00);
    /* PCIe0 is dedicated to this endpoint. Preserve all other table entries;
       do not appropriate a colliding requester or its chain. */
    if(s->old_config==0xffffffffu || (s->old_entry && s->old_entry>>16!=0x100))return 0;
    s->entry=0x01000100u|(s->old_entry&255u);s->changed=1;
    gwp_write(b,GWP_PARF,0x2000+s->slot*4,s->entry);
    gwp_write(b,GWP_PARF,0x2c00,s->old_config&~1u);
    if(gwp_read(b,GWP_PARF,0x2000+s->slot*4)!=s->entry ||
       gwp_read(b,GWP_PARF,0x2c00)!=(s->old_config&~1u)) {gwp_sid_restore(b,s);return 0;}
    return 1;
}
struct gwp_msi_state {unsigned changed,cap,masked,wide,words;uint32_t old[5];};
static inline int gwp_msi_disable(const struct gwp_io *b,const struct gwp_msi_state *s)
{
    if(!s->changed)return 1;
    if(gwp_read(b,GWP_ENDPOINT,0)!=0x110717cbu)return 0;
    uint32_t v=gwp_read(b,GWP_ENDPOINT,s->cap);
    if(v==0xffffffffu)return 0;
    gwp_write(b,GWP_ENDPOINT,s->cap,v&~0x10000u);
    v=gwp_read(b,GWP_ENDPOINT,s->cap);
    return v!=0xffffffffu && !(v&0x10000u);
}
static inline int gwp_msi_restore(const struct gwp_io *b,struct gwp_msi_state *s)
{
    if(!s->changed)return 1;
    if((gwp_read(b,GWP_ENDPOINT,4)&4) || !gwp_msi_disable(b,s))return 0;
    for(unsigned i=1;i<s->words;i++)gwp_write(b,GWP_ENDPOINT,s->cap+i*4,s->old[i]);
    gwp_write(b,GWP_ENDPOINT,s->cap,s->old[0]);
    for(unsigned i=0;i<s->words;i++)if(gwp_read(b,GWP_ENDPOINT,s->cap+i*4)!=s->old[i])return 0;
    s->changed=0;return 1;
}
static inline int gwp_msi_sink(const struct gwp_io *b,const struct gwp_state *pci,
                              struct gwp_msi_state *s,uint64_t arena,size_t bytes,uint64_t sink)
{
    if(!pci->ready || !s || s->changed || (sink&3) || sink<arena || bytes<4 || sink-arena>bytes-4 ||
       gwp_read(b,GWP_ENDPOINT,0)!=0x110717cbu || (gwp_read(b,GWP_ENDPOINT,4)&4))return 0;
    s->cap=pci->msi_cap;
    if(s->cap<0x40 || s->cap>0xf4 || (s->cap&3))return 0;
    s->old[0]=gwp_read(b,GWP_ENDPOINT,s->cap);
    if(s->old[0]==0xffffffffu || (s->old[0]&255u)!=5 || (s->old[0]&0x10000u))return 0;
    s->wide=(s->old[0]>>23)&1;
    s->masked=(s->old[0]>>24)&1;
    s->words=3u+s->wide+s->masked;
    if((!s->wide && sink>>32) || s->cap>0x100u-4u*(s->words+s->masked))return 0;
    unsigned data_word=2u+s->wide,mask_word=data_word+1;
    for(unsigned i=1;i<s->words;i++)s->old[i]=gwp_read(b,GWP_ENDPOINT,s->cap+i*4);
    s->changed=1;
    if(s->wide)gwp_write(b,GWP_ENDPOINT,s->cap+8,(uint32_t)(sink>>32));
    gwp_write(b,GWP_ENDPOINT,s->cap+4,(uint32_t)sink);
    gwp_write(b,GWP_ENDPOINT,s->cap+data_word*4,s->old[data_word]&0xffff0000u);
    if(s->masked)gwp_write(b,GWP_ENDPOINT,s->cap+mask_word*4,s->old[mask_word]&~1u);
    uint32_t control=(s->old[0]&~0x710000u)|0x10000u; /* one vector, enabled */
    gwp_write(b,GWP_ENDPOINT,s->cap,control);
    if(gwp_read(b,GWP_ENDPOINT,s->cap)!=control ||
       gwp_read(b,GWP_ENDPOINT,s->cap+4)!=(uint32_t)sink ||
       (s->wide && gwp_read(b,GWP_ENDPOINT,s->cap+8)!=(uint32_t)(sink>>32)) ||
       (gwp_read(b,GWP_ENDPOINT,s->cap+data_word*4)&0xffffu) ||
       (s->masked && (gwp_read(b,GWP_ENDPOINT,s->cap+mask_word*4)&1))) {
        gwp_msi_restore(b,s);return 0;
    }
    return 1;
}
static inline int gwp_master(const struct gwp_io *b,const struct gwp_state *s,int enable,int dma_ready)
{
    if(!s->ready || (enable && !dma_ready) || gwp_read(b,GWP_ENDPOINT,0)!=0x110717cbu)return 0;
    uint32_t value=gwp_read(b,GWP_ENDPOINT,4);
    if(value==0xffffffffu)return 0;
    if(enable) {
        /* Root-port BME gates upstream requests too. Enabling only the endpoint
         * leaves firmware reads and MSI writes rejected as Unsupported Requests.
         * Keep this behind dma_ready, publish the bridge first, and leave it
         * enabled until endpoint transactions drain and gwp_restore runs.
         * Write only Command bits; Status in the upper half contains W1C bits.
         */
        uint32_t root=gwp_read(b,GWP_ROOT,4);
        if(root==0xffffffffu)return 0;
        uint32_t expected=(root&0xffffu)|6u;
        gwp_write(b,GWP_ROOT,4,expected);
        root=gwp_read(b,GWP_ROOT,4);
        if(root==0xffffffffu || (root&0xffffu)!=expected)return 0;
    }
    value&=0xffffu;value=enable?(value|6u|0x400u):(value&~4u);
    gwp_write(b,GWP_ENDPOINT,4,value);
    uint32_t actual=gwp_read(b,GWP_ENDPOINT,4);
    if(actual==0xffffffffu || ((actual>>2)&1u)!=(unsigned)!!enable)return 0;
    if(enable)return (actual&6u)==6u;
    for(unsigned i=0;i<100;i++) {
        uint32_t status=gwp_read(b,GWP_ENDPOINT,s->pcie_cap+8);
        if(status==0xffffffffu)return 0;
        if(!(status&0x200000u)) {b->delay(b->context,1000);return 1;}
        b->delay(b->context,1000);
    }
    return 0;
}
#endif
