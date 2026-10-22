/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#if defined(GG_HOST_TEST)
#include <stdio.h>
#else
#include <pexpert/arm64/board_config.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <libkern/libkern.h>
#endif
#if defined(GG_HOST_TEST) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "giulia_gpu_control.h"
static void failures(const struct gg_diagnostics *d)
{
    if(d->failure.sequence)giulia_gpu_log_event(NULL,&d->failure);
    if(d->cleanup_failure.sequence)giulia_gpu_log_event(NULL,&d->cleanup_failure);
}
static void power_summary(const struct gg_power_state *s,int e)
{
    if(!s)return;
    if(e)failures(&s->diag);
    printf("GIULIA GPU RESULT: subsystem=power code=%d phase=%s held_mask=%#x ready=%u quarantined=%u\n",
        e,gg_phase_name(GGD_POWER,s->stage),s->held,s->ready,s->quarantined);
}
static void smmu_summary(const struct gg_smmu_state *s,int e)
{
    if(!s)return;
    if(e)failures(&s->diag);
    printf("GIULIA GPU RESULT: subsystem=smmu code=%d cleanup_code=%u bank=%u slot=%u sid=5 active=%u retain_resources=%u quarantined=%u\n",
        e,(unsigned)s->cleanup,s->bank,s->slot,s->active,s->retain_resources,s->quarantined);
}
static void gmu_summary(const struct gg_gmu_state *s,int e)
{
    if(!s)return;
    if(e)failures(&s->diag);
    printf("GIULIA GPU RESULT: subsystem=gmu code=%d cleanup_code=%u phase=%s writes=%u loaded=%u reset_released=%u reset_confirmed=%u fw_ready=%u hfi_ready=%u retain_resources=%u quarantined=%u\n",
        e,s->cleanup_error,gg_phase_name(GGD_GMU,s->phase),s->writes,s->loaded,s->reset_released,s->reset_confirmed,s->fw_ready,s->hfi_ready,s->retain_resources,s->quarantined);
}
__attribute__((used)) int giulia_gpu_power_acquire(const struct gg_power_ops *o,struct gg_power_state *s) {int e=gg_power_acquire(o,s);power_summary(s,e);return e;}
__attribute__((used)) int giulia_gpu_power_release(const struct gg_power_ops *o,struct gg_power_state *s) {int e=gg_power_release(o,s);power_summary(s,e);return e;}
__attribute__((used)) ArmSmmu4KStatus giulia_gpu_smmu_start(const struct gg_smmu_ops *o,struct gg_smmu_state *s,const ArmSmmu4KContextConfig *c,unsigned b,unsigned slot) {ArmSmmu4KStatus e=gg_smmu_start(o,s,c,b,slot);smmu_summary(s,e);return e;}
__attribute__((used)) ArmSmmu4KStatus giulia_gpu_smmu_stop(const struct gg_smmu_ops *o,struct gg_smmu_state *s,const ArmSmmu4KContextConfig *c) {ArmSmmu4KStatus e=gg_smmu_stop(o,s,c);smmu_summary(s,e);return e;}
__attribute__((used)) int giulia_gpu_gmu_load(const struct gg_gmu_ops *o,struct gg_gmu_state *s,const void *p,size_t n,struct gg_fw_block *b,size_t cap) {int e=gg_gmu_load(o,s,p,n,b,cap);gmu_summary(s,e);return e;}
__attribute__((used)) int giulia_gpu_gmu_start(const struct gg_gmu_ops *o,struct gg_gmu_state *s) {int e=gg_gmu_start(o,s);gmu_summary(s,e);return e;}
__attribute__((used)) void giulia_gpu_log_event(void *cookie,const struct gg_event *e)
{
    char address[24],expected[24],observed[24],native[24];
    (void)cookie;
    if (!e) return;
    if(e->valid&GGD_ADDRESS) snprintf(address,sizeof(address),"%#llx",(unsigned long long)e->address);
    else snprintf(address,sizeof(address),"unavailable");
    if(e->valid&GGD_EXPECTED) snprintf(expected,sizeof(expected),"%#llx",(unsigned long long)e->expected);
    else snprintf(expected,sizeof(expected),"unavailable");
    if(e->valid&GGD_OBSERVED) snprintf(observed,sizeof(observed),"%#llx",(unsigned long long)e->observed);
    else snprintf(observed,sizeof(observed),"unavailable");
    if(e->valid&GGD_NATIVE) snprintf(native,sizeof(native),"%d",e->native_error);
    else snprintf(native,sizeof(native),"unavailable");
    printf("GIULIA GPU EVENT: seq=%llu subsystem=%s phase=%s(%u) op=%s address_space=%s address=%s expected=%s observed=%s mask=%#llx code=%d code_name=%s native_code=%s cleanup=%u cause=%s\n",
        (unsigned long long)e->sequence,gg_subsystem_name(e->subsystem),gg_phase_name(e->subsystem,e->phase),e->phase,
        gg_operation_name(e->operation),(e->valid&GGD_ADDRESS)?(e->operation==GGD_RAM?"gmu-iova":"physical"):"none",
        address,expected,observed,(unsigned long long)e->mask,e->error,gg_error_name(e->subsystem,e->error),native,e->cleanup,e->error?"unconfirmed":"none");
}
#endif
