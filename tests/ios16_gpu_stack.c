/* Host fixture only: no phone, no device MMIO, no hypervisor call.
 *
 * The stack is pure sequencing logic, so every property below is checked with
 * providers that only count calls. Passing here proves the framework refuses
 * unsafe transitions; it proves nothing about the A750.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_stack.h"

static unsigned cases;

struct fixture;

struct peer {
    struct fixture *fx;
    unsigned stage,probes,enters,leaves;
    int probe_fail,enter_fail,leave_fail;
};

struct fixture {
    struct ggsk stack;
    struct peer peer[GGSK_STAGES];
    struct ggsk_ops ops[GGSK_STAGES];
    unsigned enter_order[GGSK_STAGES],leave_order[GGSK_STAGES];
    unsigned enters,leaves;
};

static int probe_cb(void *cookie,uint64_t *evidence)
{
    struct peer *p=cookie;
    ++p->probes;
    if (p->probe_fail) { *evidence=0; return 1; }
    *evidence=0x1000u+(uint64_t)p->probes;
    return 0;
}
static int enter_cb(void *cookie)
{
    struct peer *p=cookie;
    ++p->enters;
    p->fx->enter_order[p->fx->enters++]=p->stage;
    return p->enter_fail;
}
static int leave_cb(void *cookie)
{
    struct peer *p=cookie;
    ++p->leaves;
    p->fx->leave_order[p->fx->leaves++]=p->stage;
    return p->leave_fail;
}

static void fixture_init(struct fixture *f,unsigned host,unsigned device)
{
    struct ggsk_policy policy={host,device};
    unsigned stage;
    memset(f,0,sizeof(*f));
    assert(ggsk_init(&f->stack,&policy)==GGSK_OK);
    for (stage=0;stage<GGSK_STAGES;++stage) {
        struct peer *p=&f->peer[stage];
        p->fx=f; p->stage=stage;
        f->ops[stage].cookie=p;
        f->ops[stage].probe=probe_cb;
        f->ops[stage].enter=enter_cb;
        f->ops[stage].leave=leave_cb;
        assert(ggsk_register(&f->stack,stage,&f->ops[stage])==GGSK_OK);
    }
}

static unsigned device_stages(void)
{
    unsigned stage,count=0;
    for (stage=0;stage<GGSK_STAGES;++stage) if (ggsk_desc_of(stage)->klass==GGSK_CLASS_DEVICE) ++count;
    return count;
}

static unsigned device_stages_below(unsigned limit)
{
    unsigned stage,count=0;
    for (stage=0;stage<limit;++stage) if (ggsk_desc_of(stage)->klass==GGSK_CLASS_DEVICE) ++count;
    return count;
}

static unsigned total_probes(const struct fixture *f)
{
    unsigned stage,count=0;
    for (stage=0;stage<GGSK_STAGES;++stage) count+=f->peer[stage].probes;
    return count;
}

static void test_descriptions(void)
{
    unsigned stage;
    for (stage=0;stage<GGSK_STAGES;++stage) {
        const struct ggsk_desc *d=ggsk_desc_of(stage);
        assert(d && d->name && *d->name);
        assert(d->klass<=GGSK_CLASS_DEVICE);
        /* A prerequisite may only name a strictly earlier stage. */
        assert(!(d->prereq&~((1u<<stage)-1u)));
    }
    assert(!ggsk_desc_of(GGSK_STAGES));
    assert(ggsk_probe(0,GGSK_BOOT)==GGSK_ARGUMENT);
    assert(!strcmp(ggsk_stage_name(GGSK_BOOT),"boot-handoff"));
    assert(!strcmp(ggsk_stage_name(GGSK_METAL),"metal-publish"));
    assert(!strcmp(ggsk_stage_name(GGSK_STAGES),"invalid"));
    assert(!strcmp(ggsk_class_name(GGSK_CLASS_RO),"read-only"));
    assert(!strcmp(ggsk_class_name(GGSK_CLASS_HOST),"host-mutation"));
    assert(!strcmp(ggsk_class_name(GGSK_CLASS_DEVICE),"device-write"));
    assert(!strcmp(ggsk_state_name(GGSK_BLOCKED),"blocked"));
    assert(!strcmp(ggsk_code_name(GGSK_QUARANTINED),"quarantined"));
    assert(ggsk_desc_of(GGSK_FIRMWARE)->klass==GGSK_CLASS_RO);
    assert(ggsk_desc_of(GGSK_HOST_MEMORY)->klass==GGSK_CLASS_HOST);
    ++cases;
}

static void test_empty_stack(void)
{
    struct ggsk stack;
    struct ggsk_policy policy={1,1};
    assert(ggsk_init(&stack,&policy)==GGSK_OK);
    assert(ggsk_advance(&stack,GGSK_METAL)==GGSK_NO_PROVIDER);
    assert(stack.entered_mask==0 && stack.writes==0 && stack.host_mutations==0);
    assert(stack.blocked==GGSK_BOOT && stack.reached==0 && stack.quarantined==0);
    assert(stack.frozen==1); /* topology closes even when nothing is bound */
    ++cases;
}

static void test_policy_gates(void)
{
    struct fixture f;
    /* No host mutation: only the two read-only stages may be entered. */
    fixture_init(&f,0,0);
    assert(ggsk_advance(&f.stack,GGSK_METAL)==GGSK_POLICY);
    assert(f.stack.entered_mask==(GGSK_BIT(GGSK_BOOT)|GGSK_BIT(GGSK_FIRMWARE)));
    assert(f.stack.blocked==GGSK_HOST_MEMORY && f.stack.reached==GGSK_HOST_MEMORY);
    assert(f.stack.writes==0 && f.stack.host_mutations==0);
    assert(f.peer[GGSK_HOST_MEMORY].enters==0 && f.peer[GGSK_POWER].enters==0);
    assert(f.stack.slot[GGSK_HOST_MEMORY].code==GGSK_POLICY);
    ++cases;
    /* Host mutation but no device writes: page tables yes, MMIO/HVC no. */
    fixture_init(&f,1,0);
    assert(ggsk_advance(&f.stack,GGSK_METAL)==GGSK_POLICY);
    assert(f.stack.entered_mask==(GGSK_BIT(GGSK_BOOT)|GGSK_BIT(GGSK_FIRMWARE)|GGSK_BIT(GGSK_HOST_MEMORY)));
    assert(f.stack.host_mutations==1 && f.stack.writes==0);
    assert(f.stack.blocked==GGSK_POWER && f.peer[GGSK_POWER].enters==0);
    assert(f.peer[GGSK_GUNYAH].enters==0 && f.peer[GGSK_GUNYAH].probes==0);
    ++cases;
}

static void test_full_chain(void)
{
    struct fixture f;
    unsigned stage;
    fixture_init(&f,1,1);
    assert(ggsk_advance(&f.stack,GGSK_METAL)==GGSK_OK);
    assert(f.stack.entered_mask==(GGSK_BIT(GGSK_STAGES)-1u));
    assert(f.stack.blocked==GGSK_STAGES && f.stack.reached==GGSK_STAGES);
    assert(f.stack.quarantined==0);
    assert(f.stack.writes==device_stages() && f.stack.host_mutations==1);
    assert(f.stack.attempts==GGSK_STAGES);
    for (stage=0;stage<GGSK_STAGES;++stage) {
        assert(f.peer[stage].enters==1 && f.peer[stage].probes==1 && f.peer[stage].leaves==0);
        assert(f.stack.slot[stage].state==GGSK_ENTERED);
        assert(f.stack.slot[stage].evidence!=0);
        assert(f.enter_order[stage]==stage);
    }
    ++cases;
    /* Repeating a satisfied request never re-probes or re-enters. */
    {
        unsigned probes=total_probes(&f);
        assert(ggsk_advance(&f.stack,GGSK_METAL)==GGSK_OK);
        assert(ggsk_advance(&f.stack,GGSK_GMU_LOAD)==GGSK_OK);
        assert(total_probes(&f)==probes);
        for (stage=0;stage<GGSK_STAGES;++stage) assert(f.peer[stage].enters==1);
        ++cases;
    }
}

static void test_missing_provider(void)
{
    struct fixture f;
    fixture_init(&f,1,1);
    f.stack.slot[GGSK_SHARE].ops=0; /* unbound stage in the middle of the chain */
    assert(ggsk_advance(&f.stack,GGSK_METAL)==GGSK_NO_PROVIDER);
    assert(f.stack.slot[GGSK_SHARE].code==GGSK_NO_PROVIDER);
    assert(f.stack.blocked==GGSK_SHARE);
    /* Independent branches still advance: the graph is not a prefix. */
    assert(f.peer[GGSK_SMMU].enters==1 && f.peer[GGSK_GMU_LOAD].enters==1);
    assert(f.peer[GGSK_HFI].enters==1 && f.peer[GGSK_GUNYAH].enters==1);
    assert(f.peer[GGSK_CONTROLLER].enters==0 && f.peer[GGSK_FENCE].enters==0);
    assert(f.peer[GGSK_SUBMIT].enters==0 && f.peer[GGSK_SCANOUT].enters==0);
    assert(f.peer[GGSK_METAL].enters==0 && f.peer[GGSK_METAL].probes==0);
    assert(f.stack.slot[GGSK_CONTROLLER].code==GGSK_PREREQ);
    assert(f.stack.quarantined==0);
    ++cases;
}

static void test_probe_failure(void)
{
    struct fixture f;
    fixture_init(&f,1,1);
    f.peer[GGSK_FENCE].probe_fail=1;
    assert(ggsk_advance(&f.stack,GGSK_METAL)==GGSK_NOT_READY);
    assert(f.stack.blocked==GGSK_FENCE);
    assert(f.peer[GGSK_FENCE].probes==1 && f.peer[GGSK_FENCE].enters==0);
    assert(f.stack.slot[GGSK_FENCE].state==GGSK_BLOCKED);
    assert(f.stack.slot[GGSK_FENCE].evidence==0);
    assert(f.peer[GGSK_SUBMIT].enters==0 && f.peer[GGSK_METAL].enters==0);
    /* A failed observation never mutates hardware, so it is not quarantined. */
    assert(f.stack.quarantined==0);
    assert(f.peer[GGSK_CONTROLLER].enters==1);
    ++cases;
}

static void test_device_failure_quarantines(void)
{
    struct fixture f;
    unsigned stage;
    fixture_init(&f,1,1);
    f.peer[GGSK_GMU_START].enter_fail=1;
    assert(ggsk_advance(&f.stack,GGSK_METAL)==GGSK_ENTER);
    assert(f.stack.quarantined==1 && f.stack.blocked==GGSK_GMU_START);
    assert(f.stack.slot[GGSK_GMU_START].state==GGSK_FAILED);
    assert(!ggsk_entered(&f.stack,GGSK_GMU_START));
    assert(f.peer[GGSK_HFI].enters==0 && f.peer[GGSK_SUBMIT].enters==0);
    /* The walk stops at the failure: no later independent stage is entered. */
    assert(f.peer[GGSK_CONTROLLER].probes==0);
    ++cases;
    assert(ggsk_advance(&f.stack,GGSK_METAL)==GGSK_QUARANTINED);
    assert(f.stack.quarantined==1);
    ++cases;
    /* Retreat still unwinds the confirmed stages, in descending order. */
    assert(ggsk_retreat(&f.stack)==GGSK_OK);
    assert(f.stack.entered_mask==0 && f.stack.quarantined==1);
    assert(f.leaves==GGSK_GMU_START); /* BOOT..GMU_LOAD were entered */
    assert(f.leave_order[0]==GGSK_GMU_LOAD);
    for (stage=1;stage<GGSK_GMU_START;++stage) assert(f.leave_order[stage]<f.leave_order[stage-1]);
    /* Counters are cumulative: hardware contact is never forgotten. */
    assert(f.stack.writes==device_stages_below(GGSK_GMU_START));
    assert(f.stack.writes==5 && f.stack.host_mutations==1);
    assert(ggsk_advance(&f.stack,GGSK_BOOT)==GGSK_QUARANTINED);
    ++cases;
}

static void test_unconfirmed_leave(void)
{
    struct fixture f;
    unsigned stage;
    fixture_init(&f,1,1);
    assert(ggsk_advance(&f.stack,GGSK_METAL)==GGSK_OK);
    f.peer[GGSK_SMMU].leave_fail=1;
    assert(ggsk_retreat(&f.stack)==GGSK_LEAVE);
    assert(f.stack.quarantined==1);
    assert(f.stack.slot[GGSK_SMMU].state==GGSK_FAILED);
    assert(f.peer[GGSK_SMMU].leaves==1);
    assert(ggsk_entered(&f.stack,GGSK_SMMU));
    for (stage=GGSK_SMMU+1;stage<GGSK_STAGES;++stage) {
        assert(f.peer[stage].leaves==1 && !ggsk_entered(&f.stack,stage));
        assert(f.stack.slot[stage].state==GGSK_STOPPED);
    }
    for (stage=0;stage<GGSK_SMMU;++stage) {
        assert(f.peer[stage].leaves==0 && ggsk_entered(&f.stack,stage));
    }
    assert(f.leave_order[0]==GGSK_METAL);
    assert(f.leave_order[f.leaves-1]==GGSK_SMMU);
    ++cases;
}

static void test_registry(void)
{
    struct fixture f;
    struct ggsk_policy policy={0,0};
    struct ggsk stack;
    struct ggsk_ops incomplete={0,0,enter_cb,leave_cb};
    fixture_init(&f,1,1);
    assert(ggsk_register(&f.stack,GGSK_BOOT,&f.ops[GGSK_BOOT])==GGSK_BOUND);
    assert(ggsk_register(&f.stack,GGSK_STAGES,&f.ops[GGSK_BOOT])==GGSK_ARGUMENT);
    assert(ggsk_register(&f.stack,GGSK_GUNYAH,0)==GGSK_ARGUMENT);
    assert(ggsk_register(&f.stack,GGSK_GUNYAH,&incomplete)==GGSK_ARGUMENT);
    assert(ggsk_init(0,&policy)==GGSK_ARGUMENT);
    assert(ggsk_init(&stack,0)==GGSK_ARGUMENT);
    assert(ggsk_advance(0,GGSK_BOOT)==GGSK_ARGUMENT);
    assert(ggsk_advance(&f.stack,GGSK_STAGES)==GGSK_ARGUMENT);
    assert(ggsk_retreat(0)==GGSK_ARGUMENT);
    assert(ggsk_probe_all(0)==GGSK_ARGUMENT);
    /* The topology freezes at the first advance, including unbound stages. */
    f.stack.slot[GGSK_METAL].ops=0;
    assert(ggsk_advance(&f.stack,GGSK_BOOT)==GGSK_OK);
    assert(ggsk_register(&f.stack,GGSK_METAL,&f.ops[GGSK_METAL])==GGSK_FROZEN);
    ++cases;
}

static void test_status_probe(void)
{
    struct fixture f;
    unsigned stage;
    fixture_init(&f,1,1);
    assert(ggsk_probe_all(&f.stack)==GGSK_OK);
    assert(f.stack.entered_mask==0);
    for (stage=0;stage<GGSK_STAGES;++stage) {
        assert(f.peer[stage].probes==1 && f.peer[stage].enters==0);
        assert(f.stack.slot[stage].state==GGSK_READY);
    }
    ++cases;
    assert(ggsk_advance(&f.stack,GGSK_HOST_MEMORY)==GGSK_OK);
    f.peer[GGSK_HOST_MEMORY].probe_fail=1;
    assert(ggsk_probe(&f.stack,GGSK_HOST_MEMORY)==GGSK_NOT_READY);
    /* A status probe never silently downgrades a stage that is already in. */
    assert(f.stack.slot[GGSK_HOST_MEMORY].state==GGSK_ENTERED);
    assert(f.stack.slot[GGSK_HOST_MEMORY].code==GGSK_NOT_READY);
    assert(f.stack.slot[GGSK_HOST_MEMORY].evidence==0);
    assert(ggsk_advance(&f.stack,GGSK_GUNYAH)==GGSK_OK);
    assert(ggsk_entered(&f.stack,GGSK_GUNYAH));
    ++cases;
}

static void test_kernel_entry_points(void)
{
    struct fixture f;
    struct ggsk_policy policy={0,0};
    fixture_init(&f,0,0);
    assert(giulia_gpu_stack_register(&f.stack,GGSK_BOOT,&f.ops[GGSK_BOOT])==GGSK_BOUND);
    assert(giulia_gpu_stack_advance(&f.stack,GGSK_METAL)==GGSK_POLICY);
    assert(giulia_gpu_stack_plan(&f.stack)==GGSK_OK);
    assert(giulia_gpu_stack_retreat(&f.stack)==GGSK_OK);
    assert(f.stack.entered_mask==0);
    assert(giulia_gpu_stack_init(0,&policy)==GGSK_ARGUMENT);
    giulia_gpu_stack_log(&f.stack);
    ++cases;
}

int main(void)
{
    test_descriptions();
    test_empty_stack();
    test_policy_gates();
    test_full_chain();
    test_missing_provider();
    test_probe_failure();
    test_device_failure_quarantines();
    test_unconfirmed_leave();
    test_registry();
    test_status_probe();
    test_kernel_entry_points();
    printf("STACK FIXTURE: %u cases passed (ordering, gating and unwinding only; no hardware evidence)\n",cases);
    return 0;
}
