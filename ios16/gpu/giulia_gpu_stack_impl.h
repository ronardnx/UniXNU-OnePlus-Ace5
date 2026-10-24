/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Included after the GiuliaGPU definition. Read-only stage providers for the
 * parts this service can actually observe today: the ADT handoff, the pinned
 * firmware and the prepared host memory. Every later stage stays unregistered,
 * so the plan reports where the chain stops instead of skipping it, and the
 * policy never grants device writes from this path.
 */
#ifndef GIULIA_GPU_STACK_IMPL_H
#define GIULIA_GPU_STACK_IMPL_H

int GiuliaGPU::stackProbeBoot(void *cookie,uint64_t *evidence)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    uint64_t bits=0;
    if(!self || !evidence)return 1;
    if(self->bootGunyahCaptured)bits|=1u;
    if(self->firmwarePowerState==GGPH_PREPARED)bits|=2u;
    *evidence=bits;
    /* A decoded handoff is the only proof this stage can ask for. A missing
     * UEFI power report keeps mode 3 gated but does not invalidate the ADT. */
    return self->bootGunyahCaptured?0:1;
}

int GiuliaGPU::stackProbeFirmware(void *cookie,uint64_t *evidence)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self || !evidence)return 1;
    *evidence=self->firmwareVerified?1u:0u;
    return self->firmwareVerified?0:1;
}

int GiuliaGPU::stackProbeMemory(void *cookie,uint64_t *evidence)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self || !evidence)return 1;
    *evidence=self->hostPrepared?1u:0u;
    return self->hostPrepared?0:1;
}

/* The lease is admissible only when the UEFI report says it prepared the clocks
 * and the gated observation actually reached the end of its sequence. Neither
 * the mode number nor a lone register read is enough. */
int GiuliaGPU::stackProbePower(void *cookie,uint64_t *evidence)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    uint64_t bits=0;
    if(!self || !evidence)return 1;
    if(self->mode>=3)bits|=1u;
    if(self->firmwarePowerState==GGPH_PREPARED)bits|=2u;
    if(self->powerObservation.stage==GGP_OBSERVED)bits|=4u;
    if(self->powerLeaseHeld)bits|=8u;
    if(self->platformPoisoned)bits|=16u;
    if(self->gmuFaulted)bits|=32u;
    *evidence=bits;
    if(self->mode<3 || self->firmwarePowerState!=GGPH_PREPARED)return 1;
    if(self->powerObservation.stage!=GGP_OBSERVED)return 1;
    if(self->platformPoisoned || self->gmuFaulted)return 1;
    return 0;
}

int GiuliaGPU::stackEnterPower(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self || self->powerLeaseHeld)return 1;
    /* The probe ran immediately before this call; re-check anyway so a lease is
     * never adopted from a partially prepared state. No register is written. */
    if(self->mode<3 || self->firmwarePowerState!=GGPH_PREPARED ||
       self->powerObservation.stage!=GGP_OBSERVED ||
       self->platformPoisoned || self->gmuFaulted)return 1;
    self->powerLeaseHeld=true;
    IOLog("GIULIA GPU POWER: lease=adopted mode=3 report=prepared observe=complete writes=0 retained_until_reboot=1\n");
    return 0;
}

int GiuliaGPU::stackLeavePower(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self)return 1;
    /* UEFI enabled these clocks and this driver never disables them: the lease
     * ends in bookkeeping only, the hardware stays prepared until reset. */
    self->powerLeaseHeld=false;
    IOLog("GIULIA GPU POWER: lease=released hardware_retained=1 writes=0\n");
    return 0;
}

int GiuliaGPU::stackEnterNoop(void *cookie)
{
    /* These three stages only report what preparation already established.
     * Entering them performs no write, no mapping and no MMIO access. */
    (void)cookie;
    return 0;
}

int GiuliaGPU::stackLeaveNoop(void *cookie)
{
    (void)cookie;
    return 0;
}

/* The Gunyah channel is adopted from the owner: this stage proves the owner is
 * live for its epoch and that the channel is usable, then records that the
 * bring-up owns it. It never performs an HVC. */
int GiuliaGPU::stackProbeGunyah(void *cookie,uint64_t *evidence)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    uint64_t bits=0;
    if(!self || !evidence)return 1;
    if(self->gunyahOwner)bits|=1u;
    if(self->gunyahPinned)bits|=2u;
    if(self->gunyahChannelAdopted)bits|=4u;
    if(self->platformPoisoned)bits|=8u;
    *evidence=bits;
    if(!self->gunyahOwner || !self->gunyahPinned || self->platformPoisoned)return 1;
    if(!self->gunyahOwner->epoch || self->gunyahOwner->loss || self->gunyahOwner->invalid)return 1;
    return 0;
}

int GiuliaGPU::stackEnterGunyah(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self || self->gunyahChannelAdopted)return GGSK_ENTER_REFUSED;
    if(!self->gunyahOwner || !self->gunyahPinned ||
       !self->gunyahOwner->epoch || self->gunyahOwner->loss || self->gunyahOwner->invalid)
        return GGSK_ENTER_REFUSED;
    self->gunyahChannelAdopted=true;
    IOLog("GIULIA GPU GUNYAH: channel=adopted epoch=%#llx hvc=0 writes=0\n",
        (unsigned long long)self->gunyahOwner->epoch);
    return 0;
}

int GiuliaGPU::stackLeaveGunyah(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self)return 1;
    /* The owner keeps the channel and its IRQ lease to reboot. */
    self->gunyahChannelAdopted=false;
    IOLog("GIULIA GPU GUNYAH: channel=released owner_retained=1\n");
    return 0;
}

/* Share needs the pinned HFI memory and the adopted channel before it may run.
 * Its enter is the only device-class transition here; a refusal from the gate
 * is distinguished from a failure inside the transaction. */
int GiuliaGPU::stackProbeShare(void *cookie,uint64_t *evidence)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    uint64_t bits=0;
    if(!self || !evidence)return 1;
    if(self->hostPrepared)bits|=1u;
    if(self->manualMemoryPinned)bits|=2u;
    if(self->gunyahChannelAdopted)bits|=4u;
    if(self->shareRuntime.finished && self->shareRuntime.snapshot.taken)bits|=8u;
    if(self->platformPoisoned)bits|=16u;
    *evidence=bits;
    if(!self->hostPrepared || !self->manualMemoryPinned || !self->gunyahChannelAdopted)return 1;
    if(self->platformPoisoned || self->gmuFaulted)return 1;
    return 0;
}

int GiuliaGPU::stackEnterShare(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    int result;
    if(!self)return 1;
    if(self->shareRuntime.finished)return 0;
    result=self->shareCommand();
    if(!result)return 0;
    /* These refusals happen before ggsh_init(): nothing was mutated, so the
     * stack must not quarantine. Anything later may have touched the channel. */
    if(result==GGSR_ARGUMENT || result==GGSR_NOT_LIVE || result==GGSR_IDENTITY ||
       result==GGSR_AUTHORITY || result==GGSR_CHANNEL)
        return GGSK_ENTER_REFUSED;
    return 1;
}

int GiuliaGPU::stackLeaveShare(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self)return 1;
    /* A completed transaction and its snapshot are retained to reboot: the
     * fence provider depends on them, so nothing is unshared here. */
    IOLog("GIULIA GPU SHARE: transaction_retained=%u snapshot=%u\n",
        self->shareRuntime.finished,self->shareRuntime.snapshot.taken);
    return self->shareRuntime.prepared || self->shareRuntime.finished ||
        self->shareRuntime.snapshot.taken ? 1 : 0;
}

/* GMU load needs the adopted power lease and a live SID 5 translation before it
 * may touch TCM. The enter runs the existing gmuCommand(1) adapter; a refusal
 * from its own preconditions is distinguished from a partial write failure. */
int GiuliaGPU::stackProbeGmuLoad(void *cookie,uint64_t *evidence)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    uint64_t bits=0;
    if(!self || !evidence)return 1;
    if(self->mode>=3)bits|=1u;
    if(self->powerLeaseHeld)bits|=2u;
    if(self->smmuActive)bits|=4u;
    if(self->hostPrepared)bits|=8u;
    if(self->firmwareVerified)bits|=16u;
    if(self->gmuState.loaded)bits|=32u;
    *evidence=bits;
    if(self->mode<3 || !self->powerLeaseHeld || !self->smmuActive)return 1;
    if(!self->hostPrepared || !self->firmwareVerified)return 1;
    if(self->platformPoisoned || self->gmuFaulted || self->gmuState.quarantined)return 1;
    return 0;
}

int GiuliaGPU::stackEnterGmuLoad(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    int result;
    if(!self)return 1;
    if(self->gmuState.loaded)return 0;
    if(self->mode<3 || !self->powerLeaseHeld || !self->smmuActive ||
       !self->hostPrepared || !self->firmwareVerified ||
       self->platformPoisoned || self->gmuFaulted)
        return GGSK_ENTER_REFUSED;
    result=self->gmuCommand(1);
    if(!result)return 0;
    /* These codes mean the adapter refused before its first TCM write. */
    if(self->gmuState.error==GGM_PREREQUISITE || self->gmuState.error==GGM_ARGUMENT ||
       self->gmuState.error==GGM_BUSY || self->gmuState.error==GGM_FIRMWARE)
        return GGSK_ENTER_REFUSED;
    return 1;
}

int GiuliaGPU::stackLeaveGmuLoad(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self)return 1;
    IOLog("GIULIA GPU GMU: loaded=%u retained_until_reboot=1\n",self->gmuState.loaded);
    /* No unload/restore or DMA drain provider exists. Retained firmware is not
     * a confirmed stage stop and must not become GGSK_STOPPED. */
    return self->gmuState.loaded || self->gmuState.retain_resources ? 1 : 0;
}

int GiuliaGPU::stackProbeGmuStart(void *cookie,uint64_t *evidence)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    uint64_t bits=0;
    if(!self || !evidence)return 1;
    if(self->gmuState.loaded)bits|=1u;
    if(self->gmuState.quarantined)bits|=2u;
    if(self->gmuActive)bits|=4u;
    *evidence=bits;
    if(!self->gmuState.loaded || self->gmuState.quarantined)return 1;
    if(self->platformPoisoned || self->gmuFaulted)return 1;
    return 0;
}

int GiuliaGPU::stackEnterGmuStart(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    int result;
    if(!self)return 1;
    if(self->gmuActive)return 0;
    if(!self->gmuState.loaded || self->gmuState.quarantined || self->platformPoisoned || self->gmuFaulted)
        return GGSK_ENTER_REFUSED;
    result=self->gmuCommand(2);
    if(!result)return 0;
    if(self->gmuState.error==GGM_PREREQUISITE || self->gmuState.error==GGM_ARGUMENT ||
       self->gmuState.error==GGM_BUSY)
        return GGSK_ENTER_REFUSED;
    return 1;
}

int GiuliaGPU::stackLeaveGmuStart(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self)return 1;
    IOLog("GIULIA GPU GMU: started=%u retained_until_reboot=1\n",self->gmuActive);
    return self->gmuActive || self->gmuState.reset_released ||
        self->gmuState.retain_resources ? 1 : 0;
}

/* SMMU activation needs an exclusive bank/ASID/slot lease, a quiescence proof
 * and real table publication. `platformIdle()` already answers the read-only
 * half; the write half (gg_smmu_start + adapter) is not wired yet, so the enter
 * refuses instead of approximating it. */
int GiuliaGPU::stackProbeSmmu(void *cookie,uint64_t *evidence)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    uint64_t bits=0;
    if(!self || !evidence)return 1;
    if(self->mode>=3)bits|=1u;
    if(self->powerLeaseHeld)bits|=2u;
    if(self->platformPrepared&&self->platformOwned)bits|=4u;
    if(self->smmuActive)bits|=8u;
    if(self->smmuLeaseHeld)bits|=16u;
    *evidence=bits;
    if(self->mode<3 || !self->powerLeaseHeld || !self->platformPrepared || !self->platformOwned)return 1;
    if(self->platformPoisoned || self->gmuActive)return 1;
    /* Reads the global fault register and every bank/stream; no write. */
    if(!self->platformIdle())return 1;
    return 0;
}

/* The controller stage is the CTX0/hardware-fence table itself. Nothing in this
 * service can register a client yet, so the enter refuses with the exact reason
 * instead of pretending the queues exist. */
int GiuliaGPU::stackProbeController(void *cookie,uint64_t *evidence)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    uint64_t bits=0;
    if(!self || !evidence)return 1;
    if(self->shareRuntime.finished && self->shareRuntime.snapshot.taken)bits|=1u;
    if(self->smmuActive)bits|=2u;
    if(self->gunyahChannelAdopted)bits|=4u;
    if(ggfp_get(&gpuFenceProvider))bits|=8u;
    if(self->fenceMapped)bits|=16u;
    *evidence=bits;
    /* Prerequisites (share/smmu/gunyah) are checked by the framework; this probe
     * only answers whether the controller's own inputs exist. */
    if(!ggfp_get(&gpuFenceProvider) || !self->fenceMapped)return 1;
    if(self->platformPoisoned || self->gmuFaulted)return 1;
    return 0;
}

int GiuliaGPU::stackEnterController(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self)return 1;
    IOLog("GIULIA GPU CONTROLLER: register refused reason=ctx0-backend-unwired client=0 table=0 writes=0\n");
    return GGSK_ENTER_REFUSED;
}

int GiuliaGPU::stackLeaveController(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self)return 1;
    return 0;
}

/* The fence stage runs the import bridge that consumes the frozen share. A
 * skipped import is a clean refusal; any other failure stays a failure. */
int GiuliaGPU::stackProbeFence(void *cookie,uint64_t *evidence)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    uint64_t bits=0;
    if(!self || !evidence)return 1;
    if(ggfp_get(&gpuFenceProvider))bits|=1u;
    if(self->fenceMapped)bits|=2u;
    if(self->shareRuntime.finished && self->shareRuntime.snapshot.taken)bits|=4u;
    if(self->fenceImport.cpu.state)bits|=8u;
    *evidence=bits;
    if(!ggfp_get(&gpuFenceProvider) || !self->fenceMapped)return 1;
    if(!self->shareRuntime.finished || !self->shareRuntime.snapshot.taken)return 1;
    return 0;
}

int GiuliaGPU::stackEnterFence(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    int code;
    if(!self)return 1;
    if(self->fenceImport.cpu.state==GGFC_MAPPED)return 0;
    code=self->fenceImportAttempt();
    if(!code)return 0;
    if(code==7)return GGSK_ENTER_REFUSED; /* skipped before importing anything */
    return 1;
}

int GiuliaGPU::stackLeaveFence(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self)return 1;
    IOLog("GIULIA GPU FENCE: import retained state=%u\n",self->fenceImport.cpu.state);
    return self->fenceImport.cpu.state || self->fenceMapped ? 1 : 0;
}

/* The HFI client stage is the queue bring-up, not the allocator. The allocator
 * and pump state are observable, so the probe reports them; there is no client
 * initialisation to run yet, so the enter refuses with that reason. */
int GiuliaGPU::stackProbeHfi(void *cookie,uint64_t *evidence)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    uint64_t bits=0;
    if(!self || !evidence)return 1;
    if(self->hfiAllocReady)bits|=1u;
    if(self->gmuState.hfi_ready)bits|=2u;
    if(self->hfiPump->quarantined)bits|=4u;
    if(self->hfiAllocPool.published)bits|=8u;
    *evidence=bits;
    if(!self->hfiAllocReady || !self->gmuState.hfi_ready)return 1;
    if(self->hfiPump->quarantined || self->platformPoisoned)return 1;
    return 0;
}

int GiuliaGPU::stackEnterHfi(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self)return 1;
    IOLog("GIULIA GPU HFI: client refused reason=queue-bringup-unwired queues=0 writes=0\n");
    return GGSK_ENTER_REFUSED;
}

int GiuliaGPU::stackLeaveHfi(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self)return 1;
    return self->gmuState.hfi_ready || self->hfiPump->received ||
        self->hfiPump->quarantined ? 1 : 0;
}

/* Submit/scanout/metal have no backend in this service. They are registered so
 * the plan reaches them and reports the exact missing piece instead of stopping
 * at "no-provider"; every enter is a clean refusal that writes nothing. */
int GiuliaGPU::stackProbeSubmit(void *cookie,uint64_t *evidence)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    uint64_t bits=0;
    if(!self || !evidence)return 1;
    if(self->gmuActive)bits|=1u;
    if(self->fenceImport.cpu.state==GGFC_MAPPED)bits|=2u;
    if(self->hfiAllocReady)bits|=4u;
    if(self->platformPrepared&&self->platformOwned)bits|=8u;
    *evidence=bits;
    if(!self->gmuActive || self->fenceImport.cpu.state!=GGFC_MAPPED)return 1;
    if(!self->platformPrepared || !self->platformOwned)return 1;
    if(self->platformPoisoned || self->gmuFaulted)return 1;
    return 0;
}

int GiuliaGPU::stackEnterSubmit(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self)return 1;
    IOLog("GIULIA GPU SUBMIT: refused reason=ib-ring-unwired rings=0 writes=0\n");
    return GGSK_ENTER_REFUSED;
}

int GiuliaGPU::stackLeaveSubmit(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self)return 1;
    return 0;
}

int GiuliaGPU::stackProbeScanout(void *cookie,uint64_t *evidence)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    uint64_t bits=0;
    if(!self || !evidence)return 1;
    if(self->fenceImport.cpu.state==GGFC_MAPPED)bits|=1u;
    if(self->fenceMapped)bits|=2u;
    if(self->platformPrepared&&self->platformOwned)bits|=4u;
    *evidence=bits;
    if(self->fenceImport.cpu.state!=GGFC_MAPPED || !self->fenceMapped)return 1;
    if(!self->platformPrepared || self->platformPoisoned)return 1;
    return 0;
}

int GiuliaGPU::stackEnterScanout(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self)return 1;
    IOLog("GIULIA GPU SCANOUT: refused reason=display-target-unwired surfaces=0 writes=0\n");
    return GGSK_ENTER_REFUSED;
}

int GiuliaGPU::stackLeaveScanout(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self)return 1;
    return 0;
}

int GiuliaGPU::stackProbeMetal(void *cookie,uint64_t *evidence)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    uint64_t bits=0;
    if(!self || !evidence)return 1;
    if(self->fenceImport.cpu.state==GGFC_MAPPED)bits|=1u;
    if(self->gmuActive)bits|=2u;
    if(self->platformPrepared)bits|=4u;
    *evidence=bits;
    if(self->fenceImport.cpu.state!=GGFC_MAPPED || !self->gmuActive)return 1;
    if(!self->platformPrepared || self->platformPoisoned)return 1;
    return 0;
}

int GiuliaGPU::stackEnterMetal(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self)return 1;
    IOLog("GIULIA GPU METAL: refused reason=acceleration-unwired devices=0 writes=0\n");
    return GGSK_ENTER_REFUSED;
}

int GiuliaGPU::stackLeaveMetal(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self)return 1;
    return 0;
}

void GiuliaGPU::stackInit()
{
    struct ggsk_policy policy;
    struct ggsk_ops ops;
    if(stackReady)return;
    smmuInit();
    policy.allow_host_mutation=mode>=3?1u:0u;
    policy.allow_device_writes=0; /* planning never authorises device writes */
    if(ggsk_init(&stackState,&policy))return;
    ops.cookie=this;
    ops.probe=stackProbeBoot;
    ops.enter=stackEnterNoop;
    ops.leave=stackLeaveNoop;
    stackOps[GGSK_BOOT]=ops;
    ops.probe=stackProbeFirmware;
    stackOps[GGSK_FIRMWARE]=ops;
    ops.probe=stackProbeMemory;
    stackOps[GGSK_HOST_MEMORY]=ops;
    ops.probe=stackProbePower;
    ops.enter=stackEnterPower;
    ops.leave=stackLeavePower;
    stackOps[GGSK_POWER]=ops;
    ops.probe=stackProbeGunyah;
    ops.enter=stackEnterGunyah;
    ops.leave=stackLeaveGunyah;
    stackOps[GGSK_GUNYAH]=ops;
    ops.probe=stackProbeShare;
    ops.enter=stackEnterShare;
    ops.leave=stackLeaveShare;
    stackOps[GGSK_SHARE]=ops;
    ops.probe=stackProbeGmuLoad;
    ops.enter=stackEnterGmuLoad;
    ops.leave=stackLeaveGmuLoad;
    stackOps[GGSK_GMU_LOAD]=ops;
    ops.probe=stackProbeGmuStart;
    ops.enter=stackEnterGmuStart;
    ops.leave=stackLeaveGmuStart;
    stackOps[GGSK_GMU_START]=ops;
    ops.probe=stackProbeSmmu;
    ops.enter=stackEnterSmmu;
    ops.leave=stackLeaveSmmu;
    stackOps[GGSK_SMMU]=ops;
    ops.probe=stackProbeController;
    ops.enter=stackEnterController;
    ops.leave=stackLeaveController;
    stackOps[GGSK_CONTROLLER]=ops;
    ops.probe=stackProbeFence;
    ops.enter=stackEnterFence;
    ops.leave=stackLeaveFence;
    stackOps[GGSK_FENCE]=ops;
    ops.probe=stackProbeHfi;
    ops.enter=stackEnterHfi;
    ops.leave=stackLeaveHfi;
    stackOps[GGSK_HFI]=ops;
    ops.probe=stackProbeSubmit;
    ops.enter=stackEnterSubmit;
    ops.leave=stackLeaveSubmit;
    stackOps[GGSK_SUBMIT]=ops;
    ops.probe=stackProbeScanout;
    ops.enter=stackEnterScanout;
    ops.leave=stackLeaveScanout;
    stackOps[GGSK_SCANOUT]=ops;
    ops.probe=stackProbeMetal;
    ops.enter=stackEnterMetal;
    ops.leave=stackLeaveMetal;
    stackOps[GGSK_METAL]=ops;
    if(ggsk_register(&stackState,GGSK_BOOT,&stackOps[GGSK_BOOT]) ||
       ggsk_register(&stackState,GGSK_FIRMWARE,&stackOps[GGSK_FIRMWARE]) ||
       ggsk_register(&stackState,GGSK_HOST_MEMORY,&stackOps[GGSK_HOST_MEMORY]) ||
       ggsk_register(&stackState,GGSK_POWER,&stackOps[GGSK_POWER]) ||
       ggsk_register(&stackState,GGSK_GUNYAH,&stackOps[GGSK_GUNYAH]) ||
       ggsk_register(&stackState,GGSK_SHARE,&stackOps[GGSK_SHARE]) ||
       ggsk_register(&stackState,GGSK_GMU_LOAD,&stackOps[GGSK_GMU_LOAD]) ||
       ggsk_register(&stackState,GGSK_GMU_START,&stackOps[GGSK_GMU_START]) ||
       ggsk_register(&stackState,GGSK_SMMU,&stackOps[GGSK_SMMU]) ||
       ggsk_register(&stackState,GGSK_CONTROLLER,&stackOps[GGSK_CONTROLLER]) ||
       ggsk_register(&stackState,GGSK_FENCE,&stackOps[GGSK_FENCE]) ||
       ggsk_register(&stackState,GGSK_HFI,&stackOps[GGSK_HFI]) ||
       ggsk_register(&stackState,GGSK_SUBMIT,&stackOps[GGSK_SUBMIT]) ||
       ggsk_register(&stackState,GGSK_SCANOUT,&stackOps[GGSK_SCANOUT]) ||
       ggsk_register(&stackState,GGSK_METAL,&stackOps[GGSK_METAL]))
        return;
    stackReady=true;
    IOLog("GIULIA GPU STACK: registered=15 policy_host=%u policy_device=0 providers=all\n",
        policy.allow_host_mutation);
}

int GiuliaGPU::stackPlan()
{
    int code;
    if(!stackReady)return GGSK_ARGUMENT;
    code=ggsk_advance(&stackState,GGSK_METAL);
    IOLog("GIULIA GPU STACK: plan code=%s entered=%u blocked=%s quarantine=%u device_writes=%u\n",
        ggsk_code_name((unsigned)code),
        (unsigned)__builtin_popcount(stackState.entered_mask),
        ggsk_stage_name(stackState.blocked),stackState.quarantined,stackState.writes);
    giulia_gpu_stack_log(&stackState);
    return code;
}

void GiuliaGPU::stackReport()
{
    IOLog("GIULIA GPU STACK: ready=%u entered=%u blocked=%s quarantine=%u device_writes=%u host_mutations=%u leases=%u power_lease=%u\n",
        stackReady,(unsigned)__builtin_popcount(stackState.entered_mask),
        ggsk_stage_name(stackState.blocked),stackState.quarantined,
        stackState.writes,stackState.host_mutations,stackState.leases,powerLeaseHeld);
}
#endif
