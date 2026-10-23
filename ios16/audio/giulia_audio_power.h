/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * giulia ADSP power requirements derived from captured running-device.dts and
 * OnePlusOSS/android_kernel_oneplus_sm8650 at
 * d763ed47deec55451366c1187f813f2f309d88a7, drivers/regulator/rpmh-regulator.c
 * and drivers/clk/qcom/clk-rpmh.c. Independently implemented read-only planner.
 *
 * Consumer level 384 is NOT an RPMh wire value: the ARC AUX table supplies
 * its native selector. No RPMh address or selector is hardcoded here.
 * A valid plan neither acquires resources nor establishes platform ownership.
 */
#ifndef GIULIA_AUDIO_POWER_H
#define GIULIA_AUDIO_POWER_H
#include <stdint.h>
#include <stddef.h>

enum { GAP_OK, GAP_ARGUMENT, GAP_RESOURCE, GAP_AUX, GAP_LEVEL };
enum { GAP_LCX, GAP_LMX, GAP_XO, GAP_RESOURCES };
enum { GAP_ARC=3, GAP_LEVEL_REQUIRED=384, GAP_XO_ON=3, GAP_MAX_LEVELS=16,
    GAP_SLEEP=0, GAP_WAKE=1, GAP_ACTIVE=2, GAP_ALL_STATES=7 };

struct gap_resource {
    uint32_t address,slave_id;
    const unsigned char *aux;
    size_t aux_bytes;
};
struct gap_ops {
    void *cookie;
    /* Read-only trusted command-DB lookup; return 0 on success. Keep the AUX
     * data stable until gap_build_plan returns. Do not accept user-supplied
     * resource addresses or invented voltage tables as a native database. */
    int (*lookup)(void *,const char *,struct gap_resource *);
};
struct gap_vote {
    uint32_t address,selector,consumer_level,state_mask;
};
struct gap_plan {
    unsigned valid,result,resource;
    int backend_result;
    struct gap_vote votes[GAP_RESOURCES];
    /* Requests for the native interconnect owner, not encoded BCM/TCS data.
     * Both rproc_ddr and crypto_ddr must be resolved and acknowledged. */
    uint32_t icc_average,icc_peak;
    unsigned icc_paths;
};

static inline int gap_arc_selector(const unsigned char *aux,size_t bytes,
    uint32_t required,uint32_t *selector,uint32_t *level)
{
    unsigned i,padding=0,found=0;
    uint32_t previous=0,selected=0,selected_level=0;
    if(!aux || !selector || !level || !bytes || bytes>GAP_MAX_LEVELS*2 || bytes%2)
        return GAP_AUX;
    for(i=0;i<bytes/2;++i) {
        uint32_t value=(uint32_t)aux[i*2]|((uint32_t)aux[i*2+1]<<8);
        if(i && !value)padding=1;
        if((padding && value) || (value && value<previous))return GAP_AUX;
        if(!padding && !found && value>=required) {
            selected=i;selected_level=value;found=1;
        }
        if(!padding)previous=value;
    }
    if(!found)return GAP_LEVEL;
    *selector=selected;*level=selected_level;return GAP_OK;
}

static inline int gap_build_plan(const struct gap_ops *ops,struct gap_plan *plan)
{
    static const char *const names[GAP_RESOURCES]={"lcx.lvl","lmx.lvl","xo.lvl"};
    unsigned i,j;
    if(!plan)return GAP_ARGUMENT;
    for(i=0;i<sizeof(*plan);++i)((unsigned char *)plan)[i]=0;
    if(!ops || !ops->lookup)return (int)(plan->result=GAP_ARGUMENT);
    for(i=0;i<GAP_RESOURCES;++i) {
        struct gap_resource resource;
        struct gap_vote *vote=&plan->votes[i];
        for(j=0;j<sizeof(resource);++j)((unsigned char *)&resource)[j]=0;
        plan->resource=i;plan->backend_result=ops->lookup(ops->cookie,names[i],&resource);
        if(plan->backend_result || !resource.address || (resource.address&3) ||
           resource.slave_id!=GAP_ARC)return (int)(plan->result=GAP_RESOURCE);
        for(j=0;j<i;++j)if(plan->votes[j].address==resource.address)
            return (int)(plan->result=GAP_RESOURCE);
        vote->address=resource.address;vote->state_mask=GAP_ALL_STATES;
        if(i==GAP_XO) {
            /* clk-rpmh.c encodes the enable state directly, unlike rails. */
            vote->selector=GAP_XO_ON;
        } else {
            int result=gap_arc_selector(resource.aux,resource.aux_bytes,GAP_LEVEL_REQUIRED,
                &vote->selector,&vote->consumer_level);
            if(result)return (int)(plan->result=(unsigned)result);
        }
    }
    plan->icc_average=plan->icc_peak=UINT32_MAX;plan->icc_paths=2;
    plan->valid=1;return GAP_OK;
}
#endif
