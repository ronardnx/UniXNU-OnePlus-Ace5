/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Ace 5 CAP PMU -> AOP WLAN PDC voltage commands. Pure validation/formatting;
 * this header never accesses hardware. Validate the entire CAP before sending.
 * Provenance: OnePlus modules 453d8875f815bf9dfad9035982861226a4823a6c,
 * cnss2/power.c: cnss_aop_ol_cpr_cfg_setup / cnss_aop_set_vreg_param,
 * cnss_utils/wlan_firmware_service_v01.{c,h}; captured Ace 5 board DTS.
 * Sleep settings use CAP retention values, not the ACTIVE minimum constraints.
 */
#ifndef GIULIA_WIFI_PMU_H
#define GIULIA_WIFI_PMU_H
#include <stdint.h>
#include <stddef.h>
#define QPP_COMMAND_BYTES 64u
#define QPP_MAX_COMMANDS 10u
enum qpp_rail { QPP_S2C,QPP_S4I,QPP_S3C,QPP_S1C,QPP_S6C,QPP_RAILS };
enum qpp_error { QPP_OK,QPP_ARGUMENT,QPP_NAME,QPP_FLAG,QPP_RANGE,QPP_DUPLICATE,QPP_FORMAT };
/* Parsed host values, never an unaligned cast of the 48-byte CAP wire entry. */
struct qpp_entry {char name[32];uint32_t wake_valid,wake_mv,sleep_valid,sleep_mv;};
struct qpp_vote {uint32_t wake_valid,wake_mv,sleep_valid,sleep_mv;};
struct qpp_command {uint32_t words[16];unsigned rail,sleep;uint32_t mv;};
struct qpp_plan {
    struct qpp_vote rail[QPP_RAILS];
    struct qpp_command commands[QPP_MAX_COMMANDS];
    unsigned order[QPP_RAILS],rail_count,command_count,valid,error,bad_entry,bad_rail;
};
static inline const char *qpp_resource(unsigned rail)
{
    static const char *const names[]={"s2c","s4i","s3c","s1c","s6c"};
    return rail<QPP_RAILS?names[rail]:"";
}
static inline const char *qpp_pdc(unsigned rail)
{return rail<QPP_RAILS?(rail<QPP_S3C?"bb":"rf"):"";}
/* Captured regulator-min/max-microvolt, in mV: smpc2, smpi4, smpc3, smpc1,
 * smpc6. These bounds apply to the merged wake setting, not each PMU pin.
 * E.g. the PCIE and RF 1.3V pins share s1c and the greater request wins. */
static const uint32_t qpp_wake_min[QPP_RAILS]={500,300,904,1224,1856};
static const uint32_t qpp_wake_max[QPP_RAILS]={1036,1004,1064,1348,2000};
static inline int qpp_name(const char name[32])
{
    static const char *const names[]={"VDD095_MX_PMU","VDD095_PMU","VDD_PMU_AON_I",
        "VDD095_PMU_BT","VDD09_PMU_RFA_I","VDD13_PMU_PCIE_I","VDD13_PMU_RFA_I",
        "VDD19_PMU_PCIE_I","VDD19_PMU_RFA_I"};
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++) {
        unsigned j=0;
        while(j<32 && name[j] && names[i][j] && name[j]==names[i][j])j++;
        if(j<32 && !name[j] && !names[i][j])return (int)i;
    }
    return -1;
}
static inline int qpp_fail(struct qpp_plan *p,unsigned e,unsigned entry,unsigned rail)
{
    if(p){p->valid=0;p->error=e;p->bad_entry=entry;p->bad_rail=rail;}
    return (int)e;
}
static inline int qpp_append(struct qpp_command *cmd,unsigned *at,const char *text)
{
    while(*text) {
        if(*at>=QPP_COMMAND_BYTES-1)return 0;
        cmd->words[*at/4]|=(uint32_t)(unsigned char)*text++<<(8*(*at&3));(*at)++;
    }
    return 1;
}
static inline int qpp_format(struct qpp_command *out,unsigned rail,unsigned sleep,uint32_t mv)
{
    if(!out || rail>=QPP_RAILS || sleep>1 || !mv || mv>qpp_wake_max[rail])return 0;
    if(!sleep && mv<qpp_wake_min[rail])return 0;
    struct qpp_command cmd={{0},0,0,0};unsigned at=0,n=0;char reverse[10],number[11];
    uint32_t value=mv;
    do {reverse[n++]=(char)('0'+value%10);value/=10;}while(value);
    for(unsigned i=0;i<n;i++)number[i]=reverse[n-1-i];number[n]=0;
    if(!qpp_append(&cmd,&at,"{class: wlan_pdc, ss: ") || !qpp_append(&cmd,&at,qpp_pdc(rail)) ||
       !qpp_append(&cmd,&at,", res: ") || !qpp_append(&cmd,&at,qpp_resource(rail)) ||
       !qpp_append(&cmd,&at,".v, ") || !qpp_append(&cmd,&at,sleep?"dwnval":"upval") ||
       !qpp_append(&cmd,&at,": ") || !qpp_append(&cmd,&at,number) || !qpp_append(&cmd,&at,"}"))return 0;
    cmd.rail=rail;cmd.sleep=sleep;cmd.mv=mv;*out=cmd;return 1;
}
static inline const struct qpp_command *qpp_get_command(const struct qpp_plan *p,unsigned index)
{
    return p && p->valid && p->command_count<=QPP_MAX_COMMANDS && index<p->command_count?
        &p->commands[index]:NULL;
}
static inline int qpp_make(const struct qpp_entry *entries,size_t count,struct qpp_plan *out)
{
    if(out)out->valid=0; /* A rejected new CAP must not leave an old plan usable. */
    if(!entries || !out || !count || count>16)return qpp_fail(out,QPP_ARGUMENT,UINT32_MAX,UINT32_MAX);
    static const unsigned rails[]={QPP_S2C,QPP_S4I,QPP_S3C,QPP_S3C,QPP_S3C,
        QPP_S1C,QPP_S1C,QPP_S6C,QPP_S6C};
    struct qpp_plan plan;unsigned seen=0,seen_rails=0;
    /* Host RAM only; no QMP callback is reachable until this function returns. */
    unsigned char *zero=(unsigned char *)&plan;for(size_t i=0;i<sizeof(plan);i++)zero[i]=0;
    plan.bad_entry=plan.bad_rail=UINT32_MAX;
    for(size_t i=0;i<count;i++) {
        const struct qpp_entry *e=entries+i;int index=qpp_name(e->name);
        if(index<0)return qpp_fail(out,QPP_NAME,(unsigned)i,UINT32_MAX);
        unsigned rail=rails[index];
        if(seen&(1u<<(unsigned)index))return qpp_fail(out,QPP_DUPLICATE,(unsigned)i,rail);
        seen|=1u<<(unsigned)index;
        if(e->wake_valid>1 || e->sleep_valid>1)return qpp_fail(out,QPP_FLAG,(unsigned)i,rail);
        if(!e->wake_valid && !e->sleep_valid)continue;
        if(!(seen_rails&(1u<<rail))) {plan.order[plan.rail_count++]=rail;seen_rails|=1u<<rail;}
        struct qpp_vote *v=&plan.rail[rail];
        if(e->wake_valid) {
            uint64_t mv=(((uint64_t)e->wake_mv+3)&~UINT64_C(3))+14;
            if(!e->wake_mv || mv>UINT32_MAX)return qpp_fail(out,QPP_RANGE,(unsigned)i,rail);
            if(!v->wake_valid || mv>v->wake_mv)v->wake_mv=(uint32_t)mv;
            v->wake_valid=1;
        }
        if(e->sleep_valid) {
            uint64_t mv=((uint64_t)e->sleep_mv+3)&~UINT64_C(3);
            if(mv<6 || mv-6>UINT32_MAX)return qpp_fail(out,QPP_RANGE,(unsigned)i,rail);
            mv-=6;
            if(!v->sleep_valid || mv>v->sleep_mv)v->sleep_mv=(uint32_t)mv;
            v->sleep_valid=1;
        }
    }
    /* Merge every PMU pin first, then validate every resulting rail before
     * generating any sendable command. Retention may be below ACTIVE minimum. */
    for(unsigned rail=0;rail<QPP_RAILS;rail++) {
        const struct qpp_vote *v=&plan.rail[rail];
        if((v->wake_valid && (v->wake_mv<qpp_wake_min[rail] || v->wake_mv>qpp_wake_max[rail])) ||
           (v->sleep_valid && (!v->sleep_mv || v->sleep_mv>qpp_wake_max[rail])))
            return qpp_fail(out,QPP_RANGE,UINT32_MAX,rail);
    }
    /* Match vendor first-seen regulator order, upval followed by dwnval. */
    for(unsigned i=0;i<plan.rail_count;i++) {
        unsigned rail=plan.order[i];const struct qpp_vote *v=&plan.rail[rail];
        for(unsigned sleep=0;sleep<2;sleep++) {
            if(!(sleep?v->sleep_valid:v->wake_valid))continue;
            if(plan.command_count>=QPP_MAX_COMMANDS ||
               !qpp_format(&plan.commands[plan.command_count],rail,sleep,sleep?v->sleep_mv:v->wake_mv))
                return qpp_fail(out,QPP_FORMAT,UINT32_MAX,rail);
            plan.command_count++;
        }
    }
    plan.valid=1;*out=plan;return QPP_OK;
}
#endif
