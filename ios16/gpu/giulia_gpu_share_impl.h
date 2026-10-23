/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Included after the GiuliaGPU definition. Binds the primary share transaction
 * to this service: the channel comes from the Gunyah owner, the control page is
 * an owned 16 KiB Normal NC allocation, and the SCM transport is the same native
 * path the secure-CX code already uses.
 *
 * The authority callback is deliberately a refusal: proving primary role and
 * exclusive carveout ownership needs a platform backend that does not exist
 * yet. Until it does, `gpu.share` stops at GGSR_AUTHORITY with the share state
 * still zero and no RM/SCM traffic.
 */
#ifndef GIULIA_GPU_SHARE_IMPL_H
#define GIULIA_GPU_SHARE_IMPL_H

int GiuliaGPU::shareLive(void *cookie,uint64_t epoch)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self || !self->gunyahOwner || !self->gunyahPinned)return 0;
    if(!self->manualMemoryPinned || self->platformPoisoned || self->gmuFaulted)return 0;
    return self->gunyahOwner->epoch==epoch?1:0;
}

int GiuliaGPU::shareIdentity(void *cookie,uint64_t epoch,uint16_t *self_vmid,uint16_t *peer_vmid)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    struct gg_rq_snapshot snapshot;
    if(!self || !self->gunyahOwner || !self_vmid || !peer_vmid)return 0;
    /* Read-only: the owner copies its query transcript, never a fresh query. */
    if(ggown_copy_snapshot(self->gunyahOwner,&snapshot))return 0;
    if(snapshot.epoch!=epoch || snapshot.invalid)return 0;
    *self_vmid=snapshot.self_vmid;
    *peer_vmid=snapshot.peer_vmid;
    return 1;
}

/* Both the gate and ggsh's own assign-time authorize land here. A backend that
 * can prove primary role, exclusive carveout ownership and the absence of a
 * competing sharing driver replaces this body; a constant-1 stub would authorise
 * a second owner and must never be added here. */
int GiuliaGPU::shareAuthorize(void *cookie,uint64_t epoch,uint16_t self_vmid,
    uint16_t peer_vmid,uint64_t physical,uint32_t bytes)
{
    (void)cookie;
    IOLog("GIULIA GPU SHARE: authority=unavailable epoch=%#llx self=%u peer=%u pa=%#llx bytes=%#x reason=no-primary-carveout-proof\n",
        (unsigned long long)epoch,self_vmid,peer_vmid,(unsigned long long)physical,bytes);
    return 0;
}

int GiuliaGPU::sharePublish(void *cookie,const void *bytes,size_t length)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self || !bytes || length!=GGS_CONTROL_BYTES)return 0;
    if(!giulia_gpu_cache_publish(bytes,length))return 0;
    gpuHfiBarrier(NULL);
    return 1;
}

int GiuliaGPU::shareCommand()
{
    struct ggrm_io io;
    struct ggrm_state *channel=NULL;
    uint64_t epoch=0;
    int result;

    if(!hostPrepared || !manualMemoryPinned || mode<3) {
        IOLog("GIULIA GPU SHARE: rejected reason=pin-or-mode host_prepared=%u pinned=%u mode=%u\n",
            hostPrepared,manualMemoryPinned,mode);
        return GGSR_ARGUMENT;
    }
    if(platformPoisoned || gmuFaulted) {
        IOLog("GIULIA GPU SHARE: rejected reason=platform-unhealthy poisoned=%u gmu_faulted=%u\n",
            platformPoisoned,gmuFaulted);
        return GGSR_ARGUMENT;
    }
    if(!shareControlReady && !shareControlAllocate()) {
        IOLog("GIULIA GPU SHARE: rejected reason=control-allocation\n");
        return GGSR_ARGUMENT;
    }
    if(!gunyahOwner || !gunyahPinned) {
        IOLog("GIULIA GPU SHARE: rejected reason=no-owner owner=%u pinned=%u\n",
            gunyahOwner!=NULL,gunyahPinned);
        return GGSR_ARGUMENT;
    }
    epoch=gunyahOwner->epoch;
    result=ggown_get_channel(gunyahOwner,&io,&channel);
    if(result) {
        IOLog("GIULIA GPU SHARE: rejected reason=channel code=%d\n",result);
        return GGSR_CHANNEL;
    }
    shareOwner.cookie=this;
    shareOwner.valid=shareLive;
    shareOwner.authorize=shareAuthorize;
    shareOwner.publish=sharePublish;
    shareOwner.rm=io;
    shareOwner.scm.cookie=this;
    shareOwner.scm.call=platformScmCall;
    shareOwner.scm.delay_ms=platformScmDelay;
    shareOwner.scm.prerequisites=platformScmPrerequisites;
    shareOwner.control=shareControlCPU;
    shareOwner.control_pa=shareControlPA;
    shareOwner.control_bytes=(uint32_t)GGS_CONTROL_BYTES;
    shareGate.cookie=this;
    shareGate.live=shareLive;
    shareGate.identity=shareIdentity;
    shareGate.authority=shareAuthorize;
    result=ggsr_prepare(&shareRuntime,&shareOwner,channel,epoch,&shareGate);
    IOLog("GIULIA GPU SHARE: prepare code=%s prepared=%u live=%u identity=%u authority=%u refused=%u\n",
        ggsr_code_name((unsigned)result),shareRuntime.prepared,shareRuntime.live_checks,
        shareRuntime.identity_checks,shareRuntime.authority_checks,shareRuntime.refused);
    if(result) {
        shareReport();
        return result;
    }
    result=ggsr_finish(&shareRuntime,2000);
    IOLog("GIULIA GPU SHARE: finish code=%s runs=%u parcel=%#x\n",
        ggsr_code_name((unsigned)result),shareRuntime.runs,shareRuntime.snapshot.frozen.parcel);
    shareReport();
    return result;
}

void GiuliaGPU::shareReport()
{
    IOLog("GIULIA GPU SHARE: control_ready=%u pa=%#llx prepared=%u finished=%u runs=%u refused=%u last=%s\n",
        shareControlReady,(unsigned long long)shareControlPA,shareRuntime.prepared,
        shareRuntime.finished,shareRuntime.runs,shareRuntime.refused,
        ggsr_code_name(shareRuntime.last));
    IOLog("GIULIA GPU SHARE: snapshot_taken=%u epoch=%#llx parcel=%#x self=%u peer=%u fence_import_abi=1\n",
        shareRuntime.snapshot.taken,(unsigned long long)shareRuntime.snapshot.frozen.epoch,
        shareRuntime.snapshot.frozen.parcel,shareRuntime.snapshot.frozen.self_vmid,
        shareRuntime.snapshot.frozen.peer_vmid);
}
#endif
