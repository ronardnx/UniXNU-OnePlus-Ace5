#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ios16/giulia_wifi_qmi.h"
struct fixture {
    struct gwq_state s;struct gwq_io io;
    unsigned phase,requests,chunks[5],resumes,drop,wrong_id,remote_fail,secure,missing_board;
    unsigned corrupt,fail_asset,fail_alloc,overlap,allocs,modes,configs;
    unsigned second_mem_type,fail_alloc_after,memory_responses;
    unsigned dev_mem_present,dev_mem_bytes,dev_mem_overflow;
    unsigned missing_chip,chip_bytes,board_bytes,board_id,asset_calls;
    unsigned legacy_ready_only,ready_in_response;
    unsigned pmu_present,pmu_count,pmu_truncated,pmu_extra,pmu_bad_flag,pmu_bad_name,pmu_full_name;
    unsigned delay_message,pump_step_ms,drop_server;
    unsigned late_indication_batch,late_protocol_error;
    uint64_t clock_ms,server_at_ms,memory_at_ms,ready_at_ms,response_delay_ms,ready_tail_ms;
    unsigned platform_calls,host_caps;
    uint64_t host_features;
    int platform_error;
    uint64_t platform_delay_ms;
    size_t offset[5],used;uint8_t reg[6201],board[12301],phy[299660],ram[1048576];
};
static int packet(struct fixture *f,unsigned type,const void *payload,size_t n,unsigned confirm,int v3) {
    uint8_t b[8192]={0};size_t h=v3?16:32;
    if(v3){b[0]=3;b[1]=(uint8_t)type;b[2]=(uint8_t)confirm;gwq_w32(b+4,(uint32_t)n);
        gwq_w16(b+8,2);gwq_w16(b+10,0x1234);gwq_w16(b+12,1);gwq_w16(b+14,type==1?0x4000:0xfffe);}
    else{gwq_w32(b,1);gwq_w32(b+4,type);gwq_w32(b+8,2);gwq_w32(b+12,0x1234);
        gwq_w32(b+16,confirm);gwq_w32(b+20,(uint32_t)n);gwq_w32(b+24,1);gwq_w32(b+28,type==1?0x4000:GWQ_CTRL);}
    memcpy(b+h,payload,n);return gwq_receive(&f->s,&f->io,b,h+((n+3)&~(size_t)3));
}
static int indication(struct fixture *f,unsigned msg,const void *v,size_t n) {
    uint8_t b[2048]={4};gwq_w16(b+3,(uint16_t)msg);gwq_w16(b+5,(uint16_t)n);if(n)memcpy(b+7,v,n);
    return packet(f,1,b,n+7,1,1);
}
static int send_packet(void *ctx,const void *v,size_t n) {
    struct fixture *f=ctx;const uint8_t *p=v;assert(n>=32 && gwq_u32(p)==1 && gwq_u32(p+20)<=n-32);
    unsigned type=gwq_u32(p+4);
    if(type==2)return 0;
    if(type==7){assert(gwq_u32(p+8)==1 && gwq_u32(p+12)==0x4000 && gwq_u32(p+24)==2 && gwq_u32(p+28)==0x1234);
        assert(gwq_u32(p+32)==7 && gwq_u32(p+36)==1 && gwq_u32(p+40)==0x4000);f->resumes++;return 0;}
    assert(type==1 && n>=39 && gwq_u32(p+24)==2 && gwq_u32(p+28)==0x1234 && p[32]==0);
    unsigned msg=gwq_u16(p+35);size_t len=gwq_u16(p+37),z=0;const uint8_t *t=p+39,*a;
    assert(len+7==gwq_u32(p+20) && gwq_tlvs(t,len));f->requests++;
    if(f->drop)return 0;
    if(msg==0x20) { a=gwq_field(t,len,0x15,&z);assert(a&&z==4&&gwq_u32(a)==0x4b4e454c); }
    if(msg==0x34) {
        a=gwq_field(t,len,0x1c,&z);assert(a&&z==1&&*a==0);
        /* Official FEATURE_LIST is exactly one LE u64, without a validity
         * byte or array count. Only QDSS_CFG_MISS is declared; reset and
         * sleep-clock policy remain absent. No resource assets precede it. */
        static const uint8_t expected[]={8,0,0,0,0,0,0,0};
        a=gwq_field(t,len,0x27,&z);assert(a&&z==sizeof(expected)&&!memcmp(a,expected,sizeof(expected)));
        assert(!gwq_field(t,len,0x13,&z) && !f->asset_calls && !f->platform_calls);
        f->host_features=gwq_u64(a);f->host_caps++;assert(f->host_caps==1);f->phase=2;
    }
    if(msg==0x36) {
        f->memory_responses++;
        a=gwq_field(t,len,1,&z);assert(a && z==35 && a[0]==2);
        assert(gwq_u32(a+9)==65536 && gwq_u32(a+13)==1 && !a[17]);
        assert(gwq_u32(a+26)==4096 && gwq_u32(a+30)==f->second_mem_type && gwq_u64(a+18)==0x20010000);
        if(!f->fail_alloc)assert(gwq_u64(a+1)==0x20000000);f->phase=3;
    }
    if(msg==0x25) {
        a=gwq_field(t,len,0x15,&z);assert(a&&z==1);unsigned kind=*a;assert(kind==1||kind==4);
        const uint8_t *source=kind==1?f->board:f->reg;size_t size=kind==1?sizeof(f->board):sizeof(f->reg);
        a=gwq_field(t,len,0x10,&z);assert(a&&z==4&&gwq_u32(a)==f->board_id);
        a=gwq_field(t,len,0x11,&z);assert(a&&z==4&&gwq_u32(a)==size-f->offset[kind]);
        a=gwq_field(t,len,0x12,&z);assert(a&&z==4&&gwq_u32(a)==f->chunks[kind]);
        a=gwq_field(t,len,0x13,&z);assert(a&&z>=2&&gwq_u16(a)==z-2&&z<=6146);
        assert(!memcmp(a+2,source+f->offset[kind],z-2));f->offset[kind]+=z-2;f->chunks[kind]++;
        a=gwq_field(t,len,0x14,&z);assert(a&&z==1&&*a==(f->offset[kind]==size));
    }
    if(msg==0x3c) {
        a=gwq_field(t,len,1,&z);assert(a&&z==8);uint64_t pa=gwq_u64(a);
        assert(pa>=0x20000000 && pa-0x20000000<=sizeof(f->ram)-sizeof(f->phy));
        assert(!memcmp(f->ram+(pa-0x20000000),f->phy,sizeof(f->phy)));
        a=gwq_field(t,len,2,&z);assert(a&&z==4&&gwq_u32(a)==sizeof(f->phy));f->phase=4;
    }
    if(msg==0x23)f->configs++;
    if(msg==0x22){a=gwq_field(t,len,1,&z);assert(a&&z==4&&gwq_u32(a)==0);f->modes++;}
    uint8_t reply[1024]={2};gwq_w16(reply+1,(uint16_t)(gwq_u16(p+33)+f->wrong_id));gwq_w16(reply+3,(uint16_t)msg);
    size_t bytes=0;uint8_t result[4]={0};if(f->remote_fail){result[0]=1;result[2]=0x34;}
    gwq_tlv(reply+7,sizeof(reply)-7,&bytes,2,result,4);
    if(msg==0x24) {
        /* Optional flags live in the host C struct, never in these wire TLVs. */
        uint8_t b[12]={0};gwq_w32(b,0x1234);gwq_w32(b+4,0x55667788);
        if(!f->missing_chip)gwq_tlv(reply+7,sizeof(reply)-7,&bytes,0x10,b,f->chip_bytes);
        gwq_w32(b,f->board_id);
        if(!f->missing_board)gwq_tlv(reply+7,sizeof(reply)-7,&bytes,0x11,b,f->board_bytes);
        if(f->dev_mem_present) {
            uint8_t memory[80]={0};
            gwq_w64(memory,f->dev_mem_overflow?UINT64_MAX-4095:UINT64_C(0x1c20000));
            gwq_w64(memory+8,0x10000);
            gwq_w64(memory+16,UINT64_C(0x100002000));gwq_w64(memory+24,0x2000);
            assert(f->dev_mem_bytes<=sizeof(memory));
            assert(gwq_tlv(reply+7,sizeof(reply)-7,&bytes,0x1c,memory,f->dev_mem_bytes));
        }
        if(f->pmu_present) {
            uint8_t pmu[1+17*48+1]={0};assert(f->pmu_count<=17);pmu[0]=(uint8_t)f->pmu_count;
            for(unsigned i=0;i<f->pmu_count;i++) {
                uint8_t *e=pmu+1+48*i;memcpy(e,"VDD095_MX_PMU",13);
                if(f->pmu_full_name)memset(e,'P',32);
                if(f->pmu_bad_name)e[0]='\n';
                gwq_w32(e+32,f->pmu_bad_flag?2:1);gwq_w32(e+36,976+i);
                gwq_w32(e+40,i&1);gwq_w32(e+44,512+i);
            }
            size_t pn=1+48*f->pmu_count-f->pmu_truncated+f->pmu_extra;
            assert(gwq_tlv(reply+7,sizeof(reply)-7,&bytes,0x22,pmu,pn));
        }
    }
    if(f->corrupt)reply[8]=255;
    if(msg==f->delay_message)f->clock_ms+=f->response_delay_ms;
    gwq_w16(reply+5,(uint16_t)bytes);int error=packet(f,1,reply,7+bytes,0,0);
    if(!error && msg==0x3c && f->ready_in_response) {
        f->phase=40;return indication(f,0x38,NULL,0);
    }
    return error;
}
static int pump(void *ctx,unsigned us) {
    struct fixture *f=ctx;assert(us==1000);f->clock_ms+=f->pump_step_ms;
    if(f->phase==0 && (f->drop_server || f->clock_ms<f->server_at_ms))return 0;
    if(f->phase==0){uint8_t b[20]={0};gwq_w32(b,4);gwq_w32(b+4,0x45);gwq_w32(b+8,0x101);
        gwq_w32(b+12,2);gwq_w32(b+16,0x1234);f->phase=1;return packet(f,4,b,20,0,1);}
    if(f->phase==2){uint8_t b[40]={1,32,0,2}; /* TLV 1: count and two segments, first has cfg */
        gwq_w32(b+4,65536);gwq_w32(b+8,1);b[12]=1;gwq_w64(b+13,0);gwq_w32(b+21,65536);b[25]=(uint8_t)f->secure;
        gwq_w32(b+26,4096);gwq_w32(b+30,f->second_mem_type);b[34]=0;f->phase=20;return indication(f,0x35,b,35);}
    if(f->phase==3 && f->clock_ms>=f->memory_at_ms){f->phase=30;return indication(f,0x37,NULL,0);}
    if(f->phase==4 && f->clock_ms>=f->ready_at_ms){
        f->phase=40;int error=indication(f,f->legacy_ready_only?0x21:0x38,NULL,0);
        f->clock_ms+=f->ready_tail_ms;
        if(!error && f->late_indication_batch) {
            error=indication(f,0x61,NULL,0);
            if(!error){f->clock_ms++;error=indication(f,0x38,NULL,0);}
            if(!error && f->late_protocol_error)error=indication(f,0x35,NULL,0);
        }
        return error;
    }
    return 0;
}
static uint64_t now_ms(void *ctx){return ((struct fixture *)ctx)->clock_ms;}
static int allocate(void *ctx,size_t size,uint64_t *pa,void **va) {
    struct fixture *f=ctx;if(f->fail_alloc || (f->fail_alloc_after && f->allocs>=f->fail_alloc_after))return 1;
    size_t start=(f->used+4095)&~(size_t)4095;if(f->overlap)start=0;
    if(size>sizeof(f->ram)-start)return 1;*pa=0x20000000+start;*va=f->ram+start;
    f->used=start+size;f->allocs++;return 0;
}
static void clean(void *ctx,const void *v,size_t n){struct fixture *f=ctx;assert((const uint8_t *)v>=f->ram && (const uint8_t *)v+n<=f->ram+sizeof(f->ram));}
static int asset(void *ctx,unsigned kind,unsigned major,uint32_t chip,uint32_t board,const void **v,size_t *n) {
    struct fixture *f=ctx;assert(major==2 && chip==0x1234 && board==f->board_id);f->asset_calls++;if(f->fail_asset)return 1;
    assert(f->host_caps==1 && f->host_features==UINT64_C(8));
    assert(!f->pmu_present || (f->platform_calls==1 && f->s.pmu_configured && !f->s.platform_error));
    if(kind==GWQ_REGDB){*v=f->reg;*n=sizeof(f->reg);}else if(kind==GWQ_BOARD){*v=f->board;*n=sizeof(f->board);}
    else{assert(kind==GWQ_PHY);*v=f->phy;*n=sizeof(f->phy);}return 0;
}
static int configure_pmu(void *ctx,const struct gwq_pmu_param *params,unsigned count) {
    struct fixture *f=ctx;
    assert(f->s.pmu_present && f->s.pmu_valid && params==f->s.pmu && count==f->pmu_count);
    assert(f->s.last_message==0x24 && !f->asset_calls && !f->chunks[1] && !f->chunks[4]);
    f->platform_calls++;f->clock_ms+=f->platform_delay_ms;return f->platform_error;
}
static struct fixture *make(void) {
    struct fixture *f=calloc(1,sizeof(*f));assert(f);f->io=(struct gwq_io){f,send_packet,pump,allocate,clean,asset,0x20000000,sizeof(f->ram),now_ms,configure_pmu};
    f->chip_bytes=8;f->board_bytes=4;f->board_id=0xff;f->second_mem_type=GWQ_MEM_BDF;
    f->pump_step_ms=1;
    for(size_t i=0;i<sizeof(f->reg);i++)f->reg[i]=(uint8_t)(i*7);
    for(size_t i=0;i<sizeof(f->board);i++)f->board[i]=(uint8_t)(i*11);
    for(size_t i=0;i<sizeof(f->phy);i++)f->phy[i]=(uint8_t)(i*13);
    return f;
}
int main(void) {
    struct fixture *f=make();assert(!gwq_initialize(&f->s,&f->io,2));
    assert(f->s.provisioned && f->s.fw_ready && f->chunks[4]==2 && f->chunks[1]==3 && f->resumes==3 && f->allocs==3);
    assert(f->s.cap_response_bytes==25 && f->s.chip_info_present && f->s.chip_info_bytes==8);
    assert(f->s.chip_family==0x55667788 && f->s.board_info_present && f->s.board_info_bytes==4 && !f->s.board_default);
    assert(f->s.indication_count==3 && !f->s.unknown_indications && f->s.last_indication==0x38);
    assert(f->s.indication_mask==((1ull<<0x35)|(1ull<<0x37)|(1ull<<0x38)));
    assert(!f->s.pmu_present && !f->s.pmu_valid);
    assert(!f->s.dev_mem_present && !f->s.dev_mem_valid);
    assert(!f->platform_calls && !f->s.pmu_configured && !f->s.platform_error);
    assert(f->host_caps==1 && f->host_features==UINT64_C(8));
    assert(!f->s.initializing && f->s.timeout_phase==GWQ_TIMEOUT_NONE);
    assert(f->s.host_cap_ms==1 && f->s.memory_ready_ms==3 && f->s.cap_ms==3 && f->s.m3_ms==3 && f->s.fw_ready_ms==4);
    assert(f->s.elapsed_ms==4);
    uint8_t pipes[20]={0},svc[12]={0};assert(!gwq_configure(&f->s,&f->io,pipes,1,svc,1));assert(f->configs==1&&f->modes==1);free(f);
    /* The phone CNSS contract includes BDF, DPD, QDSS and all other defined
     * nonsecure segment types. Every response must name allocated backing. */
    for(unsigned type=0;type<GWQ_MEM_TYPES;type++) {
        f=make();f->second_mem_type=type;
        assert(!gwq_initialize(&f->s,&f->io,2) && f->memory_responses==1 && f->allocs==3);
        assert(f->s.allocated[1].type==type && f->s.allocated[1].address==0x20010000);
        assert(f->s.mem_request_count==2 && f->s.mem_requested_bytes==69632);
        assert(f->s.mem_failure_index==UINT32_MAX && f->s.mem_failure_type==UINT32_MAX);free(f);
    }
    for(unsigned fail=0;fail<2;fail++) {
        f=make();if(fail)f->fail_alloc_after=1;else f->fail_alloc=1;
        assert(gwq_initialize(&f->s,&f->io,2)==GWQ_MEMORY && f->allocs==fail);
        assert(!f->memory_responses && !f->s.mem_response_sent && !f->s.provisioned);
        assert(f->s.mem_failure_index==fail && f->s.mem_failure_type==(fail?GWQ_MEM_BDF:GWQ_MEM_DDR));free(f);
    }
    f=make();f->second_mem_type=GWQ_MEM_TYPES;
    assert(gwq_initialize(&f->s,&f->io,2)==GWQ_MEMORY && !f->allocs && !f->memory_responses);
    assert(f->s.mem_failure_index==1 && f->s.mem_failure_type==GWQ_MEM_TYPES);free(f);
    f=make();f->io.dma_bytes=65536;
    assert(gwq_initialize(&f->s,&f->io,2)==GWQ_MEMORY && !f->allocs && !f->memory_responses);free(f);
    f=make();f->dev_mem_present=1;f->dev_mem_bytes=64;
    assert(!gwq_initialize(&f->s,&f->io,2) && f->s.dev_mem_present && f->s.dev_mem_valid);
    assert(f->s.dev_mem[0].start==0x1c20000 && f->s.dev_mem[0].bytes==0x10000);
    assert(f->s.dev_mem[1].start==UINT64_C(0x100002000) && f->s.dev_mem[1].bytes==0x2000);
    assert(!f->s.dev_mem[2].start && !f->s.dev_mem[2].bytes && !f->s.dev_mem[3].bytes);free(f);
    const unsigned bad_dev_lengths[]={0,1,16,63,65};
    for(unsigned i=0;i<sizeof(bad_dev_lengths)/sizeof(bad_dev_lengths[0]);i++) {
        f=make();f->dev_mem_present=1;f->dev_mem_bytes=bad_dev_lengths[i];
        assert(gwq_initialize(&f->s,&f->io,2)==GWQ_WIRE && f->s.dev_mem_present && !f->s.dev_mem_valid);
        assert(!f->asset_calls && !f->s.provisioned);free(f);
    }
    f=make();f->dev_mem_present=f->dev_mem_overflow=1;f->dev_mem_bytes=64;
    assert(gwq_initialize(&f->s,&f->io,2)==GWQ_WIRE && !f->s.dev_mem_valid && !f->asset_calls);free(f);
    unsigned *flag;for(unsigned i=0;i<9;i++) {
        f=make();switch(i){case 0:flag=&f->drop;break;case 1:flag=&f->wrong_id;break;case 2:flag=&f->remote_fail;break;
            case 3:flag=&f->secure;break;case 4:flag=&f->missing_chip;break;case 5:flag=&f->corrupt;break;
            case 6:flag=&f->fail_asset;break;case 7:flag=&f->fail_alloc;break;default:flag=&f->overlap;break;}
        *flag=1;assert(gwq_initialize(&f->s,&f->io,2));assert(!f->s.provisioned && !f->modes);
        if(i==3)assert(!f->allocs);if(i==2)assert(f->s.remote_error==0x10034);free(f);
    }
    /* Match the vendor's absent-board semantics while preserving a reported
     * board exactly. Missing-chip and malformed-present fields remain fatal. */
    f=make();f->missing_board=1;assert(!gwq_initialize(&f->s,&f->io,2));
    assert(f->s.cap_response_bytes==18 && f->s.chip_info_present && f->s.chip_info_bytes==8);
    assert(!f->s.board_info_present && !f->s.board_info_bytes && f->s.board_default && f->s.board==0xff);
    assert(f->s.provisioned && f->asset_calls==3);free(f);
    f=make();f->board_id=0x123;assert(!gwq_initialize(&f->s,&f->io,2));
    assert(f->s.board==0x123 && !f->s.board_default);free(f);
    for(unsigned which=0;which<2;which++)for(unsigned size=0;size<=9;size++) {
        if(size==(which?4u:8u))continue;
        f=make();if(which)f->board_bytes=size;else f->chip_bytes=size;
        assert(gwq_initialize(&f->s,&f->io,2)==GWQ_IDENTITY && !f->s.provisioned && !f->asset_calls);
        assert(f->s.chip_info_present && f->s.chip_info_bytes==f->chip_bytes);
        assert(f->s.board_info_present && f->s.board_info_bytes==f->board_bytes && !f->s.board_default);free(f);
    }
    f=make();f->missing_chip=1;assert(gwq_initialize(&f->s,&f->io,2)==GWQ_IDENTITY);
    assert(!f->s.chip_info_present && !f->s.chip_info_bytes && f->s.board_info_present && !f->asset_calls);free(f);
    f=make();f->missing_board=f->fail_asset=1;assert(gwq_initialize(&f->s,&f->io,2)==GWQ_ASSET);
    assert(f->s.board_default && f->asset_calls==1 && !f->s.provisioned);free(f);
    f=make();f->legacy_ready_only=1;assert(gwq_initialize(&f->s,&f->io,2)==GWQ_TIMEOUT_ERROR);
    assert(f->s.provisioned && !f->s.fw_ready && f->s.last_message==0x3c);
    assert(f->s.timeout_phase==GWQ_TIMEOUT_MISSION && f->s.elapsed_ms==GWQ_MISSION_TIMEOUT && !f->s.initializing);
    assert(f->s.indication_count==3 && f->s.last_indication==0x21 && f->s.unknown_indications==1);
    assert(f->s.last_unknown_indication==0x21 && (f->s.indication_mask&(1ull<<0x21)));free(f);
    /* A ready indication may share a poll batch with the M3 response; it must
     * survive until gwq_provision marks the completed request as provisioned. */
    f=make();f->ready_in_response=1;assert(!gwq_initialize(&f->s,&f->io,2));
    assert(f->s.provisioned && f->s.fw_ready && f->s.indication_count==3);free(f);
    /* Mission readiness gets the vendor's 40-second overall deadline while
     * every individual RPC and QRTR discovery still expires after 10 seconds.
     * Count real monotonic time, not nominal one-ms polling iterations. */
    f=make();f->clock_ms=1000000;f->ready_at_ms=f->clock_ms+20000;
    assert(!gwq_initialize(&f->s,&f->io,2) && f->s.elapsed_ms==20000 && f->s.fw_ready_ms==20000);
    assert(f->s.mission_started_ms==1000000 && f->s.m3_ms==3);free(f);
    f=make();f->ready_at_ms=GWQ_MISSION_TIMEOUT-1;f->ready_tail_ms=2;
    assert(!gwq_initialize(&f->s,&f->io,2) && f->s.fw_ready_ms==39999 && f->s.elapsed_ms==40001);
    f->clock_ms+=GWQ_MISSION_TIMEOUT;
    assert(!gwq_configure(&f->s,&f->io,pipes,1,svc,1) && f->s.configured);free(f);
    /* One actual polling batch completes at 39,999 ms, then delivers an
     * unrelated indication and a duplicate ready after the deadline. */
    f=make();f->ready_at_ms=39999;f->ready_tail_ms=2;f->late_indication_batch=1;
    assert(!gwq_initialize(&f->s,&f->io,2) && f->s.timeout_phase==GWQ_TIMEOUT_NONE);
    assert(f->s.fw_ready_ms==39999 && f->s.elapsed_ms==40002 && f->s.indication_count==5);
    assert(f->s.unknown_indications==1 && f->s.last_unknown_indication==0x61);free(f);
    /* Completion only disarms the deadline: a later illegal memory request
     * still fails protocol state checks, including within that same batch. */
    f=make();f->ready_at_ms=39999;f->ready_tail_ms=2;f->late_indication_batch=f->late_protocol_error=1;
    assert(gwq_initialize(&f->s,&f->io,2)==GWQ_STATE && f->s.timeout_phase==GWQ_TIMEOUT_NONE);
    assert(f->s.fw_ready_ms==39999 && f->s.elapsed_ms==40002);free(f);
    f=make();f->ready_at_ms=GWQ_MISSION_TIMEOUT;
    assert(gwq_initialize(&f->s,&f->io,2)==GWQ_TIMEOUT_ERROR && !f->s.fw_ready);
    assert(f->s.timeout_phase==GWQ_TIMEOUT_MISSION && f->s.fw_ready_ms==40000);free(f);
    f=make();f->legacy_ready_only=1;f->pump_step_ms=7;
    assert(gwq_initialize(&f->s,&f->io,2)==GWQ_TIMEOUT_ERROR);
    assert(f->s.elapsed_ms>=40000 && f->s.elapsed_ms<40007 && f->s.timeout_phase==GWQ_TIMEOUT_MISSION);free(f);
    for(unsigned ms=9999;ms<=10000;ms++) {
        f=make();f->delay_message=0x20;f->response_delay_ms=ms;
        int result=gwq_initialize(&f->s,&f->io,2);
        if(ms==9999)assert(!result && f->s.host_cap_ms==10000);
        else assert(result==GWQ_TIMEOUT_ERROR && f->s.timeout_phase==GWQ_TIMEOUT_RPC && f->s.rpc_elapsed_ms==10000);
        free(f);
    }
    f=make();f->memory_at_ms=35000;f->delay_message=0x3c;f->response_delay_ms=6000;
    assert(gwq_initialize(&f->s,&f->io,2)==GWQ_TIMEOUT_ERROR && f->s.timeout_phase==GWQ_TIMEOUT_MISSION);
    assert(f->s.rpc_elapsed_ms==6000 && !f->s.provisioned && f->s.elapsed_ms==41000);free(f);
    for(unsigned ms=9999;ms<=10000;ms++) {
        f=make();f->server_at_ms=ms;int result=gwq_initialize(&f->s,&f->io,2);
        if(ms==9999)assert(!result);
        else assert(result==GWQ_TIMEOUT_ERROR && f->s.timeout_phase==GWQ_TIMEOUT_SERVER && !f->requests);
        free(f);
    }
    f=make();f->drop_server=1;assert(gwq_initialize(&f->s,&f->io,2)==GWQ_TIMEOUT_ERROR);
    assert(f->s.timeout_phase==GWQ_TIMEOUT_SERVER && f->s.elapsed_ms==10000);free(f);
    f=make();f->io.now=NULL;assert(gwq_initialize(&f->s,&f->io,2)==GWQ_ARGUMENT && !f->requests);free(f);
    /* Real CAP power metadata uses fixed-width fields, including values whose
     * valid flag is zero. Decode them before the platform callback runs. */
    f=make();f->pmu_present=1;f->pmu_count=2;assert(!gwq_initialize(&f->s,&f->io,2));
    assert(f->s.pmu_present && f->s.pmu_valid && f->s.pmu_bytes==97 && f->s.pmu_count==2);
    assert(!strcmp(f->s.pmu[0].pin_name,"VDD095_MX_PMU") && f->s.pmu[0].wake_valid==1 && f->s.pmu[0].wake_mv==976);
    assert(f->s.pmu[0].sleep_valid==0 && f->s.pmu[0].sleep_mv==512);
    assert(f->s.pmu[1].sleep_valid==1 && f->s.pmu[1].sleep_mv==513);free(f);
    f=make();f->pmu_present=f->pmu_full_name=1;f->pmu_count=16;assert(!gwq_initialize(&f->s,&f->io,2));
    assert(f->s.pmu_valid && f->s.pmu_count==16 && strlen(f->s.pmu[15].pin_name)==32);
    assert(f->s.pmu[15].wake_mv==991);free(f);
    f=make();f->pmu_present=1;assert(!gwq_initialize(&f->s,&f->io,2));
    assert(f->s.pmu_valid && f->s.pmu_bytes==1 && !f->s.pmu_count);free(f);
    for(unsigned i=0;i<6;i++) {
        f=make();f->pmu_present=1;f->pmu_count=1;
        switch(i) {
            case 0:f->pmu_truncated=1;break;
            case 1:f->pmu_extra=1;break;
            case 2:f->pmu_count=17;break;
            case 3:f->pmu_bad_flag=1;break;
            case 4:f->pmu_bad_name=1;break;
            default:f->pmu_count=0;f->pmu_truncated=1;break;
        }
        assert(gwq_initialize(&f->s,&f->io,2)==GWQ_WIRE && f->s.pmu_present && !f->s.pmu_valid);
        assert(!f->s.provisioned && !f->asset_calls && !f->platform_calls);free(f);
    }
    for(unsigned i=0;i<2;i++) {
        f=make();f->pmu_present=1;f->pmu_count=1;f->platform_error=i?-7:42;f->platform_delay_ms=25;
        assert(gwq_initialize(&f->s,&f->io,2)==GWQ_PLATFORM_ERROR);
        assert(f->platform_calls==1 && f->s.platform_error==f->platform_error && f->s.pmu_config_ms==28);
        assert(!f->s.pmu_configured && !f->s.provisioned && !f->asset_calls && f->s.last_message==0x24);free(f);
    }
    f=make();f->pmu_present=1;f->pmu_count=1;f->platform_delay_ms=25;
    assert(!gwq_initialize(&f->s,&f->io,2) && f->s.pmu_configured && f->platform_calls==1);
    assert(f->s.pmu_config_ms==28 && f->s.cap_ms==3 && f->s.m3_ms==28 && !f->s.platform_error);free(f);
    f=make();f->pmu_present=1;f->pmu_count=1;f->io.configure_pmu=NULL;
    assert(gwq_initialize(&f->s,&f->io,2)==GWQ_PLATFORM_ERROR && f->s.platform_error==-1);
    assert(!f->platform_calls && !f->asset_calls && !f->s.pmu_configured);free(f);
    f=make();f->io.configure_pmu=NULL;
    assert(!gwq_initialize(&f->s,&f->io,2) && !f->s.pmu_configured);free(f);
    f=make();f->pmu_present=1;f->pmu_count=1;f->platform_delay_ms=GWQ_MISSION_TIMEOUT;
    assert(gwq_initialize(&f->s,&f->io,2)==GWQ_TIMEOUT_ERROR && f->s.timeout_phase==GWQ_TIMEOUT_MISSION);
    assert(f->s.pmu_configured && !f->s.platform_error && f->s.pmu_config_ms==40003 && !f->asset_calls);free(f);
    /* Reject malformed framing, duplicate TLVs, wrong service and reordered
     * firmware indications, without allocating or accepting false readiness. */
    uint8_t dup[]={2,4,0,0,0,0,0,2,0,0};assert(!gwq_tlvs(dup,sizeof(dup)));
    assert(!gwq_tlvs(dup,2));assert(!gwq_tlvs(dup,6));
    for(unsigned i=0;i<32;i++){f=make();uint8_t b[32]={1};assert(gwq_receive(&f->s,&f->io,b,i));free(f);}
    f=make();f->s.server=1;f->s.node=2;f->s.port=0x1234;assert(indication(f,0x38,NULL,0)==GWQ_STATE);free(f);
    f=make();f->s.server=1;f->s.node=2;f->s.port=0x1234;
    assert(!indication(f,0x61,NULL,0) && f->s.indication_count==1 && f->s.unknown_indications==1);
    assert(f->s.last_unknown_indication==0x61 && !f->s.indication_mask && !f->s.fw_ready);free(f);
    /* Data routed to another QRTR client is still ignored, but retain only
     * routing metadata so missing FW_INIT_DONE can be distinguished from a
     * destination/source mismatch without retaining packet payloads. */
    f=make();uint8_t filtered[40]={0};gwq_w32(filtered,1);gwq_w32(filtered+4,1);
    gwq_w32(filtered+8,2);gwq_w32(filtered+12,0x1234);gwq_w32(filtered+20,7);
    gwq_w32(filtered+24,1);gwq_w32(filtered+28,0x4000);filtered[32]=4;gwq_w16(filtered+35,0x38);
    f->s.response[0]=0xa5;f->s.response_bytes=1;
    assert(!gwq_receive(&f->s,&f->io,filtered,sizeof(filtered)) && f->s.filtered_data_packets==1);
    f->s.server=1;f->s.node=2;f->s.port=0x1234;f->s.mem_ready=1;
    const unsigned at[]={8,12,24,28};const uint32_t wrong[]={9,99,3,0x4010};
    const uint32_t correct[]={2,0x1234,1,0x4000};
    for(unsigned i=0;i<4;i++) {
        gwq_w32(filtered+at[i],wrong[i]);
        assert(!gwq_receive(&f->s,&f->io,filtered,sizeof(filtered)) && f->s.filtered_data_packets==i+2);
        assert(f->s.filtered_src_node==gwq_u32(filtered+8) && f->s.filtered_src_port==gwq_u32(filtered+12));
        assert(f->s.filtered_dst_node==gwq_u32(filtered+24) && f->s.filtered_dst_port==gwq_u32(filtered+28));
        assert(!f->s.fw_ready && !f->s.indication_count && f->s.response_bytes==1 && f->s.response[0]==0xa5);
        gwq_w32(filtered+at[i],correct[i]);
    }
    assert(!gwq_receive(&f->s,&f->io,filtered,sizeof(filtered)) && f->s.fw_ready);
    assert(f->s.filtered_data_packets==5 && f->s.indication_count==1);free(f);
    f=make();uint8_t bad[20]={0};gwq_w32(bad,4);gwq_w32(bad+4,0x45);gwq_w32(bad+8,0x201);
    gwq_w32(bad+12,2);gwq_w32(bad+16,123);assert(!packet(f,4,bad,20,0,0)&&!f->s.server);free(f);
    puts("QRTR/QMI: complete firmware exchange, all vendor memory types, allocation failure without zero-IOVA responses, explicit QDSS-config absence, identity diagnostics, segmented assets, DMA ownership and malformed/failure cases passed");
    return 0;
}
