/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Kernel entry points and bounded logging for the bring-up stack. All state
 * transitions live in giulia_gpu_stack.h; this unit only exports them to the
 * service and formats one line per stage. No hardware access happens here.
 */
#if defined(GG_HOST_TEST)
#include <stdio.h>
#else
#include <pexpert/arm64/board_config.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <libkern/libkern.h>
#endif
#if defined(GG_HOST_TEST) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "giulia_gpu_stack.h"

static void ggsk_line(unsigned stage,const struct ggsk_slot *slot)
{
    printf("GIULIA GPU STACK: stage=%u name=%s class=%s state=%s code=%s evidence=%#llx probes=%u enters=%u leaves=%u\n",
        stage,ggsk_stage_name(stage),ggsk_class_name(ggsk_desc_of(stage)->klass),
        ggsk_state_name(slot->state),ggsk_code_name(slot->code),
        (unsigned long long)slot->evidence,slot->probes,slot->enters,slot->leaves);
}

void giulia_gpu_stack_log(const struct ggsk *s)
{
    unsigned stage;
    if (!s) return;
    for (stage=0;stage<GGSK_STAGES;++stage) ggsk_line(stage,&s->slot[stage]);
    printf("GIULIA GPU STACK: entered=%u/%u reached=%u blocked=%u(%s) quarantine=%u device_writes=%u host_mutations=%u\n",
        (unsigned)__builtin_popcount(s->entered_mask),GGSK_STAGES,s->reached,s->blocked,
        s->blocked<GGSK_STAGES?ggsk_stage_name(s->blocked):"none",
        s->quarantined,s->writes,s->host_mutations);
    printf("GIULIA GPU STACK: frozen=%u attempts=%u policy_host=%u policy_device=%u acceleration=0\n",
        s->frozen,s->attempts,s->policy.allow_host_mutation,s->policy.allow_device_writes);
}

__attribute__((used)) int giulia_gpu_stack_init(struct ggsk *s,const struct ggsk_policy *policy)
{
    int error=ggsk_init(s,policy);
    printf("GIULIA GPU STACK: init code=%s policy_host=%u policy_device=%u\n",
        ggsk_code_name((unsigned)error),policy?policy->allow_host_mutation:0u,
        policy?policy->allow_device_writes:0u);
    return error;
}

__attribute__((used)) int giulia_gpu_stack_register(struct ggsk *s,unsigned stage,const struct ggsk_ops *ops)
{
    int error=ggsk_register(s,stage,ops);
    if (error) printf("GIULIA GPU STACK: register stage=%s rejected code=%s\n",
        ggsk_stage_name(stage),ggsk_code_name((unsigned)error));
    return error;
}

__attribute__((used)) int giulia_gpu_stack_plan(struct ggsk *s)
{
    int error=ggsk_probe_all(s);
    printf("GIULIA GPU STACK: plan result=%s device_writes=0\n",ggsk_code_name((unsigned)error));
    giulia_gpu_stack_log(s);
    return error;
}

__attribute__((used)) int giulia_gpu_stack_advance(struct ggsk *s,unsigned target)
{
    int error=ggsk_advance(s,target);
    printf("GIULIA GPU STACK: advance target=%s code=%s quarantine=%u\n",
        ggsk_stage_name(target),ggsk_code_name((unsigned)error),s?s->quarantined:0u);
    giulia_gpu_stack_log(s);
    return error;
}

__attribute__((used)) int giulia_gpu_stack_retreat(struct ggsk *s)
{
    int error=ggsk_retreat(s);
    printf("GIULIA GPU STACK: retreat code=%s quarantine=%u entered=%u\n",
        ggsk_code_name((unsigned)error),s?s->quarantined:0u,
        s?(unsigned)__builtin_popcount(s->entered_mask):0u);
    giulia_gpu_stack_log(s);
    return error;
}
#endif
