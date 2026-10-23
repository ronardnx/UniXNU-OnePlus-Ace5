/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_GPU_GMU_H
#define GIULIA_GPU_GMU_H
#include "giulia_gpu_pack.h"
#include "giulia_gpu_diag.h"
/* Word offsets in the vendor GPU register namespace, NOT byte offsets into
 * the GMU mapping. A real adapter must translate/map the qualified windows.
 */
enum { GGM_ITCM=0x1b400, GGM_DTCM=0x1c400, GGM_RESET=0x1f800,
    GGM_FW_READY=0x1f81c, GGM_HFI_STATUS=0x1f980, GGM_HFI_INIT=0x1f986 };
enum { GGM_OK, GGM_ARGUMENT, GGM_BUSY, GGM_FIRMWARE, GGM_PREREQUISITE,
    GGM_IO, GGM_TIMEOUT, GGM_READBACK };
struct gg_gmu_ops {
    void *cookie;
    int (*read_word)(void *,uint32_t,uint32_t *);
    int (*write_word)(void *,uint32_t,uint32_t);
    int (*barrier)(void *);
    int (*wait_us)(void *,unsigned);
    /* Must observe powered/clocked GMU, held exclusive leases, live SID 5
     * native4K translation, clean tables, fault handling and stopped masters. */
    int (*preconditions)(void *);
    /* Verify every non-TCM payload against the actual owned RAM mapping. */
    int (*ram_matches)(void *,uint32_t,const uint8_t *,size_t);
    /* HWSCHED order: PDC/RSC, secure CX init, register/version/IRQ config,
     * DDR bandwidth vote. Must fail until all real adapters are implemented. */
    int (*configure_boot)(void *);
    /* After FW ready: discover GPU features and reset/publish HFI queues. */
    int (*prepare_hfi)(void *);
    int (*healthy)(void *); /* 1 only with no observed hardware fault. */
    gg_trace_fn trace;
};
struct gg_gmu_state {
    unsigned attempted, writes, loaded, reset_released, fw_ready, hfi_ready;
    unsigned reset_confirmed, quarantined, retain_resources, error, cleanup_error, phase;
    struct gg_diagnostics diag;
};
static inline int gg_gmu_ops_valid(const struct gg_gmu_ops *o)
{
    return o && o->read_word && o->write_word && o->barrier && o->wait_us &&
        o->preconditions && o->ram_matches && o->configure_boot && o->prepare_hfi && o->healthy;
}
static inline int gg_gmu_read(const struct gg_gmu_ops *o,struct gg_gmu_state *s,uint32_t reg,uint32_t *v,unsigned cleanup)
{
    int e=o->read_word(o->cookie,reg,v);
    gg_record_full(&s->diag,o->trace,o->cookie,GGD_GMU,s->phase,GGD_READ,
        0x03d00000ull+4ull*reg,0,e?0:*v,GGD_ADDRESS|GGD_NATIVE|(e?0:GGD_OBSERVED),0xffffffffu,e?GGM_IO:0,e,cleanup);
    return e;
}
static inline int gg_gmu_write(const struct gg_gmu_ops *o,struct gg_gmu_state *s,uint32_t reg,uint32_t v,unsigned cleanup)
{
    int e; ++s->writes; e=o->write_word(o->cookie,reg,v);
    gg_record_full(&s->diag,o->trace,o->cookie,GGD_GMU,s->phase,GGD_WRITE,
        0x03d00000ull+4ull*reg,v,0,GGD_ADDRESS|GGD_EXPECTED|GGD_NATIVE,0xffffffffu,e?GGM_IO:0,e,cleanup);
    return e;
}
static inline int gg_gmu_barrier(const struct gg_gmu_ops *o,struct gg_gmu_state *s,unsigned cleanup)
{
    int e=o->barrier(o->cookie);
    gg_record_full(&s->diag,o->trace,o->cookie,GGD_GMU,s->phase,GGD_BARRIER,0,0,0,GGD_NATIVE,0,e?GGM_IO:0,e,cleanup);
    return e;
}
static inline int gg_gmu_reset_held(const struct gg_gmu_ops *o,struct gg_gmu_state *s,unsigned cleanup)
{
    uint32_t v=0; int e=gg_gmu_read(o,s,GGM_RESET,&v,cleanup);
    if (!e && v!=1) gg_record(&s->diag,o->trace,o->cookie,GGD_GMU,s->phase,GGD_VALIDATE,
        0x03d00000ull+4ull*GGM_RESET,1,v,7,GGM_BUSY,cleanup);
    return !e && v==1;
}
static inline int gg_gmu_abort(const struct gg_gmu_ops *o,struct gg_gmu_state *s,int error)
{
    gg_record(&s->diag,o->trace,o->cookie,GGD_GMU,s->phase,GGD_VALIDATE,0,0,0,0,error,0);
    s->phase=7;
    s->error=(unsigned)error; s->retain_resources=1; s->quarantined=1;
    s->fw_ready=s->hfi_ready=s->reset_confirmed=0;
    if (gg_gmu_write(o,s,GGM_RESET,1,1) || gg_gmu_barrier(o,s,1) || !gg_gmu_reset_held(o,s,1))
        s->cleanup_error=GGM_IO;
    else { s->reset_confirmed=1; s->reset_released=0; }
    /* Reset readback alone never proves that outstanding DMA has drained. */
    return error;
}
static inline int gg_gmu_load(const struct gg_gmu_ops *o,struct gg_gmu_state *s,
    const void *bytes,size_t length,struct gg_fw_block *scratch,size_t capacity)
{
    struct gg_pack pack;
    size_t count=0,i,j;
    if (!gg_gmu_ops_valid(o) || !s || !scratch) return GGM_ARGUMENT;
    if (s->attempted || s->quarantined) return GGM_BUSY;
    s->attempted=1; s->phase=1;
    if (!gg_pack_open(bytes,length,&pack) ||
        gg_fw_plan(pack.asset[0].data,pack.asset[0].size,scratch,capacity,&count)!=GG_FW_OK) {
        gg_record(&s->diag,o->trace,o->cookie,GGD_GMU,1,GGD_VALIDATE,0,GG_FIRMWARE_PACK_BYTES,length,6,GGM_FIRMWARE,0);
        return s->error=GGM_FIRMWARE;
    }
    {int ready=o->preconditions(o->cookie);
        gg_record(&s->diag,o->trace,o->cookie,GGD_GMU,1,GGD_VALIDATE,0,1,(uint64_t)ready,6,ready==1?0:GGM_PREREQUISITE,0);
        if(ready!=1 || !gg_gmu_reset_held(o,s,0)) return s->error=GGM_PREREQUISITE;
    }
    for (i=0;i<count;++i) {
        const struct gg_fw_block *b=scratch+i;
        if (b->size && b->region!=GG_ITCM && b->region!=GG_DTCM) {
            int matched=o->ram_matches(o->cookie,b->address,pack.asset[0].data+b->payload_offset,b->size);
            gg_record(&s->diag,o->trace,o->cookie,GGD_GMU,1,GGD_RAM,b->address,1,(uint64_t)matched,7,matched==1?0:GGM_PREREQUISITE,0);
            if(matched!=1) return s->error=GGM_PREREQUISITE;
        }
    }
    s->retain_resources=1;
    for (i=0;i<count;++i) {
        const struct gg_fw_block *b=scratch+i;
        uint32_t reg;
        if (!b->size || (b->region!=GG_ITCM && b->region!=GG_DTCM)) continue;
        if (!gg_gmu_reset_held(o,s,0)) return gg_gmu_abort(o,s,GGM_BUSY);
        reg=b->region==GG_ITCM?GGM_ITCM+b->address/4:GGM_DTCM+(b->address-0x10004000u)/4;
        for (j=0;j<b->size;j+=4) {
            if (gg_gmu_write(o,s,reg+(uint32_t)(j/4),gg_le32(pack.asset[0].data+b->payload_offset+j),0))
                return gg_gmu_abort(o,s,GGM_IO);
        }
        if (gg_gmu_barrier(o,s,0)) return gg_gmu_abort(o,s,GGM_IO);
        for (j=0;j<b->size;j+=4) {
            uint32_t v;
            if (gg_gmu_read(o,s,reg+(uint32_t)(j/4),&v,0)) return gg_gmu_abort(o,s,GGM_IO);
            if (v!=gg_le32(pack.asset[0].data+b->payload_offset+j)) {
                gg_record(&s->diag,o->trace,o->cookie,GGD_GMU,s->phase,GGD_VALIDATE,
                    0x03d00000ull+4ull*reg+j,gg_le32(pack.asset[0].data+b->payload_offset+j),v,7,GGM_READBACK,0);
                return gg_gmu_abort(o,s,GGM_READBACK);
            }
        }
    }
    if (!gg_gmu_reset_held(o,s,0) || o->healthy(o->cookie)!=1) return gg_gmu_abort(o,s,GGM_PREREQUISITE);
    s->loaded=1; s->reset_confirmed=1; return GGM_OK;
}
static inline int gg_gmu_poll(const struct gg_gmu_ops *o,struct gg_gmu_state *s,uint32_t reg,uint32_t mask,uint32_t expected)
{
    unsigned i; uint32_t value=0;
    for (i=0;i<10000;++i) {
        if (o->healthy(o->cookie)!=1 || gg_gmu_read(o,s,reg,&value,0)) return GGM_IO;
        if ((value&mask)==expected) return gg_gmu_barrier(o,s,0)?GGM_IO:GGM_OK;
        if (o->wait_us(o->cookie,10)) return GGM_IO;
    }
    gg_record_masked(&s->diag,o->trace,o->cookie,GGD_GMU,s->phase,GGD_POLL,0x03d00000ull+4ull*reg,expected,value,7,mask,GGM_TIMEOUT,0);
    return GGM_TIMEOUT;
}
/* Only reaches the HFI transport-ready handshake, NOT the acknowledged
 * HWSCHED feature/performance/bandwidth/start command protocol. */
static inline int gg_gmu_start(const struct gg_gmu_ops *o,struct gg_gmu_state *s)
{
    uint32_t value; int e;
    if (!gg_gmu_ops_valid(o) || !s) return GGM_ARGUMENT;
    if (!s->loaded || s->quarantined || s->reset_released || s->hfi_ready) return GGM_BUSY;
    s->phase=2;
    if (o->preconditions(o->cookie)!=1) {
        s->quarantined=1; s->retain_resources=1; s->error=GGM_PREREQUISITE;
        gg_record(&s->diag,o->trace,o->cookie,GGD_GMU,s->phase,GGD_VALIDATE,0,1,0,GGD_EXPECTED,GGM_PREREQUISITE,0);
        return GGM_PREREQUISITE; /* No MMIO if power/ownership cannot be proved. */
    }
    if (!gg_gmu_reset_held(o,s,0)) return gg_gmu_abort(o,s,GGM_PREREQUISITE);
    e=o->configure_boot(o->cookie);
    gg_record_full(&s->diag,o->trace,o->cookie,GGD_GMU,s->phase,GGD_CONFIGURE,0,0,0,GGD_NATIVE,0,e?GGM_PREREQUISITE:0,e,0);
    if(e) return gg_gmu_abort(o,s,GGM_PREREQUISITE);
    if (!gg_gmu_reset_held(o,s,0) || gg_gmu_read(o,s,GGM_HFI_STATUS,&value,0))
        return gg_gmu_abort(o,s,GGM_PREREQUISITE);
    if(value&1) {
        gg_record_masked(&s->diag,o->trace,o->cookie,GGD_GMU,s->phase,GGD_VALIDATE,0x03d00000ull+GGM_HFI_STATUS*4,0,value,7,1,GGM_READBACK,0);
        return gg_gmu_abort(o,s,GGM_READBACK);
    }
    s->phase=3;
    if (gg_gmu_write(o,s,GGM_FW_READY,0,0) || gg_gmu_barrier(o,s,0) ||
        gg_gmu_read(o,s,GGM_FW_READY,&value,0)) return gg_gmu_abort(o,s,GGM_IO);
    if(value) {
        gg_record_masked(&s->diag,o->trace,o->cookie,GGD_GMU,s->phase,GGD_VALIDATE,0x03d00000ull+GGM_FW_READY*4,0,value,7,0xffffffffu,GGM_READBACK,0);
        return gg_gmu_abort(o,s,GGM_READBACK);
    }
    s->reset_confirmed=0; s->reset_released=1;
    if (gg_gmu_write(o,s,GGM_RESET,0,0) || gg_gmu_barrier(o,s,0)) return gg_gmu_abort(o,s,GGM_IO);
    s->phase=4; e=gg_gmu_poll(o,s,GGM_FW_READY,0x1ff,0x100);
    if (e) return gg_gmu_abort(o,s,e);
    s->fw_ready=1; s->phase=5;
    e=o->prepare_hfi(o->cookie);
    gg_record_full(&s->diag,o->trace,o->cookie,GGD_GMU,s->phase,GGD_CONFIGURE,0,0,0,GGD_NATIVE,0,e?GGM_PREREQUISITE:0,e,0);
    if(e) return gg_gmu_abort(o,s,GGM_PREREQUISITE);
    if (gg_gmu_write(o,s,GGM_HFI_INIT,1,0) || gg_gmu_barrier(o,s,0)) return gg_gmu_abort(o,s,GGM_IO);
    s->phase=6; e=gg_gmu_poll(o,s,GGM_HFI_STATUS,1,1);
    if (e) return gg_gmu_abort(o,s,e);
    s->hfi_ready=1; return GGM_OK;
}
#endif
