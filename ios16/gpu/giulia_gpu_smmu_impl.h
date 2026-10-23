/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * SID 5 context activation adapter, included after the GiuliaGPU definition.
 *
 * The vendor ArmSmmu4K library owns the register sequence (fault check, table
 * publication, disabled-context readback, SCTLR enable, stream attach). This
 * file supplies exactly the three facts the library refuses to infer: an
 * exclusive bank/ASID/slot lease, a quiescence proof, and real table
 * publication. It does not reorder or shortcut anything.
 *
 * Ordering mirrors upstream: lease -> fault check of the target stream ->
 * publish tables -> quiescence again -> program the bank -> attach the stream.
 * A refusal from any callback happens before the first write; the stage
 * returns GGSK_ENTER_REFUSED then, and only a partially programmed context is
 * reported as a failure (which quarantines the stack).
 */
#ifndef GIULIA_GPU_SMMU_IMPL_H
#define GIULIA_GPU_SMMU_IMPL_H

enum { GGS_SMMU_BASE=0x3da0000, GGS_SMMU_BYTES=0x40000,
       GGS_GMU_BANK=2, GGS_GMU_SLOT=0, GGS_GMU_ASID=0x750, GGS_GMU_SID=5 };

int GiuliaGPU::smmuRead32(void *cookie,size_t at,uint32_t *value)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    uint32_t observed;
    if(!self || !value || (at&3) || at>GGS_SMMU_BYTES-4)return 1;
    observed=self->platformRead(GGS_SMMU_BASE+at);
    if(observed==0xffffffffu)return 1;
    *value=observed;
    return 0;
}

int GiuliaGPU::smmuRead64(void *cookie,size_t at,uint64_t *value)
{
    uint32_t low=0,high=0;
    if(!value)return 1;
    if(smmuRead32(cookie,at,&low) || smmuRead32(cookie,at+4,&high))return 1;
    *value=(uint64_t)low|((uint64_t)high<<32);
    return 0;
}

/* Raw transport only: the library journals its own expectations and readbacks,
 * so adding a second check here would both duplicate and mask its failures. */
int GiuliaGPU::smmuWrite32(void *cookie,size_t at,uint32_t value)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self || (at&3) || at>GGS_SMMU_BYTES-4)return 1;
    self->platformWrite(GGS_SMMU_BASE+at,value);
    return 0;
}

int GiuliaGPU::smmuWrite64(void *cookie,size_t at,uint64_t value)
{
    if(smmuWrite32(cookie,at,(uint32_t)value))return 1;
    return smmuWrite32(cookie,at+4,(uint32_t)(value>>32));
}

int GiuliaGPU::smmuBarrier(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self)return 1;
    gpuHfiBarrier(NULL);
    return 0;
}

/* One exclusive lease, and only for the captured GMU master. The GPU bank 0 is
 * left alone: this adapter exists for the SID 5 translation the GMU needs. */
int GiuliaGPU::smmuLease(void *cookie,unsigned bank,unsigned asid,unsigned slot)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self || self->smmuLeaseHeld)return 1;
    if(bank!=GGS_GMU_BANK || slot!=GGS_GMU_SLOT || asid!=GGS_GMU_ASID)return 1;
    if(self->gmuState.loaded || self->gmuActive)return 1; /* no master using it yet */
    self->smmuLeaseHeld=true;
    self->smmuBank=bank;
    self->smmuSlot=slot;
    self->smmuAsid=asid;
    IOLog("GIULIA GPU SMMU: lease bank=%u slot=%u asid=%#x sid=%u exclusive=1\n",
        bank,slot,asid,GGS_GMU_SID);
    return 0;
}

int GiuliaGPU::smmuQuiescent(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self)return 0;
    /* Read-only: power observed, no fault, every bank disabled, every stream
     * in its reset state. Any other answer must stop the activation. */
    return self->platformIdle()?1:0;
}

int GiuliaGPU::smmuPublish(void *cookie,const ArmSmmu4KConfig *lower,
    const ArmSmmu4KResult *tables)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    IOByteCount contiguous=0;
    uint64_t pa=0;
    size_t bytes;
    if(!self || !lower || !tables || !self->tableMemory || !self->gmuTablesBuilt)return 1;
    pa=self->tableMemory->getPhysicalSegment(0,&contiguous,kIOMemoryMapperNone);
    bytes=(size_t)self->tableMemory->getLength();
    if(pa!=lower->table_physical || contiguous<bytes)return 1;
    if(!giulia_gpu_cache_publish(self->tableMemory->getBytesNoCopy(),bytes))return 1;
    gpuHfiBarrier(NULL);
    IOLog("GIULIA GPU SMMU: tables published root=%#llx bytes=%llu table_pages=%lu leaf_pages=%lu\n",
        (unsigned long long)pa,(unsigned long long)bytes,
        (unsigned long)tables->table_pages,(unsigned long)tables->leaf_pages);
    return 0;
}

int GiuliaGPU::smmuRelease(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self)return 1;
    if(!self->smmuLeaseHeld)return 1;
    self->smmuLeaseHeld=false;
    IOLog("GIULIA GPU SMMU: lease released bank=%u slot=%u; unmatched stream faults\n",
        self->smmuBank,self->smmuSlot);
    return 0;
}

void GiuliaGPU::smmuInit()
{
    bzero(&smmuState,sizeof(smmuState));
    bzero(&smmuOps,sizeof(smmuOps));
    smmuOps.io.cookie=this;
    smmuOps.io.register_bytes=GGS_SMMU_BYTES;
    smmuOps.io.read32=smmuRead32;
    smmuOps.io.read64=smmuRead64;
    smmuOps.io.write32=smmuWrite32;
    smmuOps.io.write64=smmuWrite64;
    smmuOps.io.barrier=smmuBarrier;
    smmuOps.lease=smmuLease;
    smmuOps.quiescent=smmuQuiescent;
    smmuOps.publish_tables=smmuPublish;
    smmuOps.release_lease=smmuRelease;
    smmuOps.trace=NULL;
    smmuLeaseHeld=false;
    IOLog("GIULIA GPU SMMU: adapter base=%#x bytes=%#x bank=%u slot=%u asid=%#x sid=%u\n",
        (unsigned)GGS_SMMU_BASE,(unsigned)GGS_SMMU_BYTES,(unsigned)GGS_GMU_BANK,
        (unsigned)GGS_GMU_SLOT,(unsigned)GGS_GMU_ASID,(unsigned)GGS_GMU_SID);
}

int GiuliaGPU::stackEnterSmmu(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    ArmSmmu4KContextConfig config;
    ArmSmmu4KStatus status;
    if(!self)return 1;
    if(self->smmuActive)return 0;
    if(self->mode<3 || !self->powerLeaseHeld || !self->platformPrepared ||
       !self->platformOwned || self->platformPoisoned || !self->gmuTablesBuilt ||
       self->gmuState.loaded || self->gmuActive)
        return GGSK_ENTER_REFUSED;
    if(!self->platformIdle()) {
        IOLog("GIULIA GPU SMMU: activation refused reason=not-quiescent\n");
        return GGSK_ENTER_REFUSED;
    }
    bzero(&config,sizeof(config));
    config.lower=&self->gmuContextConfig;
    config.lower_tables=&self->gmuTableResult;
    config.id0=self->platformRead(GGS_SMMU_BASE);
    config.id2=self->platformRead(GGS_SMMU_BASE+8);
    config.asid=GGS_GMU_ASID;
    config.coherent_walk=0;      /* matches the native 4K profile in audit */
    config.stall_on_fault=0;
    config.continue_after_fault=0;
    status=gg_smmu_start(&self->smmuOps,&self->smmuState,&config,GGS_GMU_BANK,GGS_GMU_SLOT);
    if(status==ArmSmmu4KSuccess) {
        self->smmuActive=true;
        IOLog("GIULIA GPU SMMU: active=1 bank=%u slot=%u asid=%#x sid=%u writes=%u tlbi=%u\n",
            (unsigned)GGS_GMU_BANK,(unsigned)GGS_GMU_SLOT,(unsigned)GGS_GMU_ASID,
            (unsigned)GGS_GMU_SID,self->smmuState.context.writes,
            self->smmuState.context.tlb_synchronized);
        return 0;
    }
    IOLog("GIULIA GPU SMMU: start result=%u writes=%u readback=%u quarantined=%u error=%u\n",
        (unsigned)status,self->smmuState.context.writes,self->smmuState.context.readback_valid,
        self->smmuState.quarantined,(unsigned)self->smmuState.error);
    /* A context that was partially programmed must not be presented as a clean
     * refusal: the framework quarantines on a nonzero return. */
    if(self->smmuState.context.writes || self->smmuState.context.must_retain_tables ||
       self->smmuState.quarantined)
        return 1;
    return GGSK_ENTER_REFUSED;
}

int GiuliaGPU::stackLeaveSmmu(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self)return 1;
    if(!self->smmuActive)return 0;
    /* Translation is retained to reboot: stopping the GPU domain here would
     * leave the GMU walking tables this service may no longer own. */
    IOLog("GIULIA GPU SMMU: active=1 retained_until_reboot=1 bank=%u slot=%u\n",
        self->smmuBank,self->smmuSlot);
    return 0;
}
#endif
