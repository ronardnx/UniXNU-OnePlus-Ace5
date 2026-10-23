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

void GiuliaGPU::stackInit()
{
    struct ggsk_policy policy;
    struct ggsk_ops ops;
    if(stackReady)return;
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
    if(ggsk_register(&stackState,GGSK_BOOT,&stackOps[GGSK_BOOT]) ||
       ggsk_register(&stackState,GGSK_FIRMWARE,&stackOps[GGSK_FIRMWARE]) ||
       ggsk_register(&stackState,GGSK_HOST_MEMORY,&stackOps[GGSK_HOST_MEMORY]))
        return;
    stackReady=true;
    IOLog("GIULIA GPU STACK: registered=3 policy_host=%u policy_device=0 providers=boot,firmware,host-memory\n",
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
    IOLog("GIULIA GPU STACK: ready=%u entered=%u blocked=%s quarantine=%u device_writes=%u host_mutations=%u\n",
        stackReady,(unsigned)__builtin_popcount(stackState.entered_mask),
        ggsk_stage_name(stackState.blocked),stackState.quarantined,
        stackState.writes,stackState.host_mutations);
}
#endif
