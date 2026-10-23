/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Share snapshot -> fence import bridge, included after the GiuliaGPU
 * definition.
 *
 * `ggfn_import` is the only supported way to turn a registered provider plus a
 * completed share transaction into a CPU-side fence state machine. This file
 * supplies the three authorities it requires:
 *
 *   - the provider descriptor exactly as registered (never a copy we made up);
 *   - `ggsn_share_cb`, which re-asks the frozen snapshot and the live share
 *     state and reports divergence instead of repeating an old parcel;
 *   - the real Normal NC mapper and the native pmap auditor.
 *
 * The import is attempted only after a share transaction actually finished.
 * With authority still unproven that never happens, so this bridge is honest
 * about its refusal instead of importing a synthetic identity.
 */
#ifndef GIULIA_GPU_FENCE_IMPORT_IMPL_H
#define GIULIA_GPU_FENCE_IMPORT_IMPL_H

int GiuliaGPU::fenceImportAttempt()
{
    const struct ggf_provider *provider=ggfp_get(&gpuFenceProvider);
    struct ggfn_inputs inputs;
    struct ggfn_status status;
    uint32_t page_bytes=0;
    int code;

    if(!provider) {
        IOLog("GIULIA GPU FENCE: import skipped reason=provider-not-registered completion_verified=0\n");
        return 7;
    }
    if(!fenceMapped || fenceProvider!=provider) {
        IOLog("GIULIA GPU FENCE: import skipped reason=carveout-not-mapped completion_verified=0\n");
        return 7;
    }
    if(!shareRuntime.finished || !shareRuntime.snapshot.taken) {
        /* The only source of the primary identity is a completed transaction. */
        IOLog("GIULIA GPU FENCE: import skipped reason=share-not-frozen prepared=%u finished=%u snapshot=%u completion_verified=0\n",
            shareRuntime.prepared,shareRuntime.finished,shareRuntime.snapshot.taken);
        return 7;
    }
    page_bytes=(uint32_t)provider->layout.page_bytes;
    if(!page_bytes) {
        IOLog("GIULIA GPU FENCE: import skipped reason=page-bytes-missing\n");
        return 8;
    }
    inputs.provider=provider;
    inputs.share=ggsn_share_cb;
    inputs.map=giulia_gpu_fence_native_map;
    inputs.audit=giulia_gpu_fence_native_audit;
    inputs.cookie=&shareRuntime.binding;
    code=ggfn_import(&fenceImport,&inputs,page_bytes);
    bzero(&status,sizeof(status));
    (void)ggfn_status(&fenceImport,&status);
    IOLog("GIULIA GPU FENCE: import code=%d state=%u proofs=%#x epoch=%#llx parcel=%#x page_bytes=%u cpu_state=%u\n",
        code,status.state,status.proofs,(unsigned long long)status.epoch,status.parcel,
        status.page_bytes,status.cpu_state);
    if(code) {
        IOLog("GIULIA GPU FENCE: import retained result=%u invalidated=%u completion_verified=0\n",
            status.result,status.invalidated);
        return code;
    }
    IOLog("GIULIA GPU FENCE: imported=1 parcel=%#x self=%u peer=%u completion_verified=0\n",
        status.parcel,status.self_vmid,status.peer_vmid);
    return 0;
}

void GiuliaGPU::fenceImportReport()
{
    struct ggfn_status status;
    bzero(&status,sizeof(status));
    if(!ggfn_status(&fenceImport,&status)) {
        IOLog("GIULIA GPU FENCE: import_state=%u cpu_state=%u proofs=%#x epoch=%#llx parcel=%#x invalidated=%u\n",
            status.state,status.cpu_state,status.proofs,(unsigned long long)status.epoch,
            status.parcel,status.invalidated);
        return;
    }
    IOLog("GIULIA GPU FENCE: import_state=unavailable\n");
}
#endif
