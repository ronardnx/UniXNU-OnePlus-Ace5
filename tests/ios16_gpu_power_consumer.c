#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/giulia_gpu_power_handoff.h"

static struct gpp_report prepared(void)
{
    struct gpp_report r;
    unsigned i;
    gpp_report_init(&r);
    r.phase=GPP_PHASE_DONE;r.attempted=r.confirmed=0x1ffe;
    r.valid_reads=0x3fff;r.retained=1;
    r.flags=GPP_READY|GPP_CFG_INHERITED|GPP_ACD_CONSUMED|GPP_PROVIDER_UNBOUNDED;
    r.values[0]=1;r.values[2]=0x80000000u;
    for(i=3;i<=10;i++)r.values[i]=1;
    r.values[11]=1;r.values[12]=625000000;r.values[13]=200000000;
    gpp_report_seal(&r);return r;
}

int main(void)
{
    struct gpp_report r=prepared(),out;
    unsigned char unaligned[257];unsigned i;
    memcpy(unaligned+1,&r,sizeof(r));
    assert(ggph_decode(unaligned+1,256,&out)==GGPH_PREPARED);
    assert(ggph_qmp_available(unaligned+1,256));
    assert(ggph_qmp_available(NULL,0));
    assert(!ggph_qmp_available(NULL,256));
    for(i=0;i<sizeof(r);i++) {
        struct gpp_report bad=r;
        ((unsigned char *)&bad)[i]^=1;
        assert(!ggph_decode(&bad,sizeof(bad),&out));
        assert(!ggph_qmp_available(&bad,sizeof(bad)));
    }
    for(i=0;i<513;i++)if(i!=256)assert(!ggph_decode(&r,i,&out));
    for(i=0;i<22;i++) {
        struct gpp_report bad=r;
        if(i<14)bad.valid_reads&=~(1u<<i);
        else if(i==14)bad.values[12]=260000000;
        else if(i==15)bad.values[13]=0;
        else if(i==16)bad.values[11]=0;
        else if(i==17)bad.protocol_status=UINT64_C(0x8000000000000007);
        else if(i==18)bad.flags|=GPP_QMP_UNCERTAIN;
        else if(i==19)bad.confirmed&=~(1u<<12);
        else if(i==20)bad.values[3]|=0x80000000u;
        else bad.retained=0;
        gpp_report_seal(&bad);
        assert(!ggph_decode(&bad,sizeof(bad),&out));
    }
    r=prepared();r.flags&=~GPP_READY;r.code=GPP_ERR_ACD;r.phase=GPP_PHASE_ACD;
    r.firmware_status=UINT64_C(0x8000000000000012);
    r.flags|=GPP_QMP_UNCERTAIN;gpp_report_seal(&r);
    assert(ggph_decode(&r,sizeof(r),&out)==GGPH_RECORDED);
    assert(out.firmware_status==UINT64_C(0x8000000000000012));
    assert(!ggph_qmp_available(&r,sizeof(r)));
    r.flags&=~GPP_QMP_UNCERTAIN;gpp_report_seal(&r);
    assert(ggph_qmp_available(&r,sizeof(r)));
    gpp_report_init(&r);
    assert(!ggph_decode(&r,sizeof(r),&out));
    assert(!ggph_qmp_available(&r,sizeof(r)));
    puts("GPU handoff consumer: CRC, unaligned report, truthful readiness, 64-bit errors and QMP quarantine passed");
    return 0;
}
