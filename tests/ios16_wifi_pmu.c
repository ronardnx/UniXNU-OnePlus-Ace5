#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/giulia_wifi_pmu.h"
static const struct qpp_entry observed[]={
    {"VDD_PMU_AON_I",1,850,0,0},
    {"VDD095_PMU",1,930,1,420},
    {"VDD095_MX_PMU",1,950,1,796},
    {"VDD095_PMU_BT",1,900,0,0},
    {"VDD09_PMU_RFA_I",1,850,0,0},
    {"VDD13_PMU_PCIE_I",1,1025,0,0},
    {"VDD13_PMU_RFA_I",1,1300,0,0},
    {"VDD19_PMU_PCIE_I",1,1850,0,0},
    {"VDD19_PMU_RFA_I",1,1850,0,0}
};
static void exact_command(const struct qpp_command *command,const char *expected,
                          unsigned rail,unsigned sleep,unsigned mv)
{
    assert(command && command->rail==rail && command->sleep==sleep && command->mv==mv);
    size_t n=strlen(expected);assert(n<64);
    for(unsigned i=0;i<64;i++) {
        unsigned byte=(command->words[i/4]>>(8*(i&3)))&255u;
        assert(byte==(i<n?(unsigned char)expected[i]:0));
    }
}
int main(void)
{
    struct qpp_plan plan;unsigned cases=0;
    assert(qpp_make(observed,9,&plan)==QPP_OK && plan.valid && plan.rail_count==5 && plan.command_count==7);
    const unsigned wake[]={966,946,914,1314,1866},sleep[]={790,414,0,0,0};
    for(unsigned i=0;i<QPP_RAILS;i++) {
        assert(plan.rail[i].wake_valid && plan.rail[i].wake_mv==wake[i]);
        assert(plan.rail[i].sleep_valid==(i<2) && plan.rail[i].sleep_mv==sleep[i]);
    }
    /* Independent literal oracle: exact vendor spelling, first-seen rail
     * order, decimal values and every byte of the 64-byte padded wire image. */
    exact_command(qpp_get_command(&plan,0),"{class: wlan_pdc, ss: rf, res: s3c.v, upval: 914}",QPP_S3C,0,914);
    exact_command(qpp_get_command(&plan,1),"{class: wlan_pdc, ss: bb, res: s4i.v, upval: 946}",QPP_S4I,0,946);
    exact_command(qpp_get_command(&plan,2),"{class: wlan_pdc, ss: bb, res: s4i.v, dwnval: 414}",QPP_S4I,1,414);
    exact_command(qpp_get_command(&plan,3),"{class: wlan_pdc, ss: bb, res: s2c.v, upval: 966}",QPP_S2C,0,966);
    exact_command(qpp_get_command(&plan,4),"{class: wlan_pdc, ss: bb, res: s2c.v, dwnval: 790}",QPP_S2C,1,790);
    exact_command(qpp_get_command(&plan,5),"{class: wlan_pdc, ss: rf, res: s1c.v, upval: 1314}",QPP_S1C,0,1314);
    exact_command(qpp_get_command(&plan,6),"{class: wlan_pdc, ss: rf, res: s6c.v, upval: 1866}",QPP_S6C,0,1866);
    assert(!qpp_get_command(&plan,7));cases++;
    struct qpp_entry reverse[9];for(unsigned i=0;i<9;i++)reverse[i]=observed[8-i];
    struct qpp_plan other;assert(qpp_make(reverse,9,&other)==QPP_OK);
    assert(!memcmp(plan.rail,other.rail,sizeof(plan.rail)) && other.order[0]==QPP_S6C);
    assert(other.command_count==7);cases++;
    /* Reject a bad final entry before any caller can retrieve a command.
     * A failed new build also revokes an older valid output plan. */
    struct qpp_entry bad_cap[9];memcpy(bad_cap,observed,sizeof(observed));
    strcpy(bad_cap[8].name,"UNKNOWN_PIN");other=plan;
    assert(qpp_make(bad_cap,9,&other)==QPP_NAME && other.bad_entry==8 && !other.valid);
    assert(!qpp_get_command(&other,0));cases++;
    for(unsigned i=0;i<7;i++) {
        struct qpp_entry bad=observed[0];other=plan;
        switch(i) {
        case 0:memset(bad.name,'X',sizeof(bad.name));break;
        case 1:strcpy(bad.name,"VDD_PMU");break;
        case 2:bad.wake_valid=2;break;
        case 3:bad.wake_mv=0;break;
        case 4:bad.wake_mv=UINT32_MAX;break;
        case 5:bad.sleep_valid=1;bad.sleep_mv=1;break;
        default:bad.sleep_valid=2;break;
        }
        assert(qpp_make(&bad,1,&other)!=QPP_OK && !other.valid && !qpp_get_command(&other,0));cases++;
    }
    /* Wake constraints are from the captured regulator nodes. Test either
     * side of both limits, after the stock 4mV rounding +14mV adjustment. */
    const unsigned entry_for_rail[]={2,1,3,6,8};
    const unsigned minimum[]={500,300,904,1224,1856},maximum[]={1036,1004,1064,1348,2000};
    for(unsigned rail=0;rail<5;rail++)for(unsigned side=0;side<4;side++) {
        struct qpp_entry e=observed[entry_for_rail[rail]];e.sleep_valid=0;
        e.wake_mv=side==0?minimum[rail]-18:side==1?minimum[rail]-14:
            side==2?maximum[rail]-16:maximum[rail]-12;
        int result=qpp_make(&e,1,&other);
        if(side==1 || side==2)assert(result==QPP_OK && other.valid && other.command_count==1);
        else assert(result==QPP_RANGE && !other.valid && other.bad_rail==rail && !qpp_get_command(&other,0));
        cases++;
    }
    struct qpp_entry retention=observed[0];retention.wake_valid=0;retention.sleep_valid=1;retention.sleep_mv=612;
    assert(qpp_make(&retention,1,&other)==QPP_OK && !other.rail[QPP_S3C].wake_valid && other.command_count==1);
    exact_command(qpp_get_command(&other,0),"{class: wlan_pdc, ss: rf, res: s3c.v, dwnval: 606}",QPP_S3C,1,606);cases++;
    retention.sleep_mv=1065;assert(qpp_make(&retention,1,&other)==QPP_OK); /* ceil1065=1068, minus6=1062 */
    retention.sleep_mv=1069;assert(qpp_make(&retention,1,&other)==QPP_RANGE && !other.valid);cases++;
    struct qpp_entry duplicate[2]={observed[0],observed[0]};
    assert(qpp_make(duplicate,2,&other)==QPP_DUPLICATE && !other.valid);cases++;
    struct qpp_entry ignored=observed[0];ignored.wake_valid=0;ignored.wake_mv=UINT32_MAX;
    ignored.sleep_valid=0;ignored.sleep_mv=UINT32_MAX;
    assert(qpp_make(&ignored,1,&other)==QPP_OK && other.valid && !other.command_count && !other.rail_count);
    assert(!qpp_get_command(&other,0));cases++;
    other=plan;assert(qpp_make(NULL,9,&other)==QPP_ARGUMENT && !other.valid && !qpp_get_command(&other,0));
    assert(qpp_make(observed,0,&other)==QPP_ARGUMENT && qpp_make(observed,17,&other)==QPP_ARGUMENT &&
           qpp_make(observed,9,NULL)==QPP_ARGUMENT && !qpp_get_command(NULL,0));cases++;
    struct qpp_command command;
    assert(!qpp_format(&command,QPP_RAILS,0,900) && !qpp_format(&command,QPP_S3C,2,900));
    assert(!qpp_format(&command,QPP_S3C,0,0) && !qpp_format(&command,QPP_S3C,0,900));
    assert(!qpp_format(&command,QPP_S3C,1,2000) && !qpp_format(NULL,QPP_S3C,0,916));cases++;
    puts("Wi-Fi PMU captured commands: s3c=914, s4i=946/414, s2c=966/790, s1c=1314, s6c=1866 mV");
    printf("Wi-Fi PMU: %u exact-command, first-seen-order, merged-range, retention and all-or-nothing cases passed\n",cases);
}
