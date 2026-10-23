/* Offline memory payload fixtures only: no device, HVC, assignment or READY. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_gunyah_memory.h"

static const uint8_t share_wire[]={
    0,0,0,0,5,0,0,0,2,0,0,0,0x45,0,6,0,0x73,0,6,0,
    1,0,0,0,0,0x30,0xe2,0xd4,0,0,0,0,0,0xd0,0x2d,0,0,0,0,0,
    0,0,0,0
};
static const uint8_t shared_prefix[]={
    0x78,0x56,0x34,0x12,0,2,0,0,0x45,0,0,0,5,0,0,0,0xaa,0xbb,0xcc,0xdd
};
static const uint8_t participant[]={
    0x78,0x56,0x34,0x12,0x73,0,0,0,0xaa,0xbb,0xcc,0xdd
};
static void builders(void)
{
    uint8_t out[80],saved[80];memset(out,0x5a,sizeof(out));memcpy(saved,out,sizeof(out));
    assert(GGMEM_SHARE==UINT32_C(0x51000013) && GGMEM_NOTIFY==UINT32_C(0x51000017));
    assert(GGMEM_SHARED==UINT32_C(0x51100011) && GGMEM_RELEASED==UINT32_C(0x51100012));
    assert(GGMEM_ACCEPTED==UINT32_C(0x51100013) && sizeof(share_wire)==44);
    assert(!gg_mem_build_share(NULL,44,0x45,0x73));
    for(size_t n=0;n<44;n++) {
        assert(!gg_mem_build_share(out,n,0x45,0x73));assert(!memcmp(out,saved,sizeof(out)));
    }
    const uint16_t invalid[][2]={{0,0x73},{0x45,0},{UINT16_MAX,0x73},
        {0x45,UINT16_MAX},{0x45,0x45}};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++) {
        assert(!gg_mem_build_share(out,sizeof(out),invalid[i][0],invalid[i][1]));
        assert(!memcmp(out,saved,sizeof(out)));
    }
    assert(gg_mem_build_share(out,sizeof(out),0x45,0x73)==44);
    assert(!memcmp(out,share_wire,44) && !memcmp(out+44,saved+44,sizeof(out)-44));
    assert(gg_mem_build_share(out,sizeof(out),0x4132,0xfffe)==44);
    assert(out[12]==0x32 && out[13]==0x41 && out[16]==0xfe && out[17]==0xff);
    assert(gg_mem_build_share(out,sizeof(out),3,4)==44); /* Dynamic VMID 3 is legal. */
    assert(out[12]==3 && out[16]==4 && out[14]==6 && out[18]==6);

    const uint8_t notify_shared[]={0x78,0x56,0x34,0x12,1,0,0,0,0xaa,0xbb,0xcc,0xdd,
        1,0,0,0,0x73,0,0,0};
    memcpy(out,saved,sizeof(out));
    for(size_t n=0;n<20;n++) {
        assert(!gg_mem_build_notify_shared(out,n,0x12345678,0xddccbbaa,0x73));
        assert(!memcmp(out,saved,sizeof(out)));
    }
    assert(!gg_mem_build_notify_shared(NULL,20,0,0,0x73));
    assert(!gg_mem_build_notify_shared(out,20,0,0,0));
    assert(!gg_mem_build_notify_shared(out,20,0,0,UINT16_MAX));
    assert(!memcmp(out,saved,sizeof(out)));
    assert(gg_mem_build_notify_shared(out,sizeof(out),0x12345678,0xddccbbaa,0x73)==20);
    assert(!memcmp(out,notify_shared,20) && !memcmp(out+20,saved+20,sizeof(out)-20));
    assert(gg_mem_build_notify_shared(out,sizeof(out),UINT32_MAX,0,0x4132)==20);
    assert(gg_mem_u32(out)==UINT32_MAX && !gg_mem_u32(out+8) && gg_mem_u16(out+16)==0x4132);

    for(unsigned flag=0;flag<256;flag++) {
        memcpy(out,saved,sizeof(out));
        size_t bytes=gg_mem_build_notify_owner(out,sizeof(out),0x12345678,0xddccbbaa,flag);
        if(flag==2 || flag==4) {
            const uint8_t owner[]={0x78,0x56,0x34,0x12,(uint8_t)flag,0,0,0,0xaa,0xbb,0xcc,0xdd};
            assert(bytes==12 && !memcmp(out,owner,12));
            assert(!memcmp(out+12,saved+12,sizeof(out)-12));
        } else assert(!bytes && !memcmp(out,saved,sizeof(out)));
    }
    memcpy(out,saved,sizeof(out));
    for(size_t n=0;n<12;n++) {
        assert(!gg_mem_build_notify_owner(out,n,0,0,2));assert(!memcmp(out,saved,sizeof(out)));
    }
    assert(!gg_mem_build_notify_owner(NULL,12,0,0,2));
    assert(!gg_mem_build_notify_owner(out,12,0,0,256+2));
    assert(!memcmp(out,saved,sizeof(out)));
}
static void replies(void)
{
    uint8_t payload[]={0x78,0x56,0x34,0x12,0};uint32_t handle=0x11223344;
    for(size_t n=0;n<6;n++) {
        if(n==4)continue;
        assert(gg_mem_parse_share_reply(payload,n,&handle)==GGMEM_INVALID);
        assert(handle==0x11223344);
    }
    assert(gg_mem_parse_share_reply(NULL,4,&handle)==GGMEM_INVALID && handle==0x11223344);
    assert(gg_mem_parse_share_reply(payload,4,NULL)==GGMEM_INVALID);
    assert(!gg_mem_parse_share_reply(payload,4,&handle) && handle==0x12345678);
    memset(payload,0,4);assert(!gg_mem_parse_share_reply(payload,4,&handle) && !handle);
    memset(payload,0xff,4);assert(!gg_mem_parse_share_reply(payload,4,&handle) && handle==UINT32_MAX);
    assert(!gg_mem_parse_notify_reply(NULL,0) && !gg_mem_parse_notify_reply(payload,0));
    assert(gg_mem_parse_notify_reply(payload,1)==GGMEM_INVALID);
    assert(gg_mem_parse_notify_reply(NULL,1)==GGMEM_INVALID);
}
static void participant_events(void)
{
    uint8_t payload[16];struct gg_mem_participant output,saved;
    for(unsigned released=0;released<2;released++) {
        uint32_t id=released?GGMEM_RELEASED:GGMEM_ACCEPTED;
        memset(&output,0x5a,sizeof(output));memcpy(&saved,&output,sizeof(output));
        for(size_t n=0;n<16;n++) {
            if(n==12)continue;
            assert(gg_mem_parse_participant(id,participant,n,0x12345678,0x73,0xddccbbaa,&output)==GGMEM_INVALID);
            assert(!memcmp(&output,&saved,sizeof(output)));
        }
        for(unsigned fault=0;fault<8;fault++) {
            memcpy(payload,participant,12);uint32_t handle=0x12345678,tag=0xddccbbaa;
            uint16_t peer=0x73;uint32_t message=id;
            if(fault==0)payload[0]++;
            if(fault==1)payload[4]++;
            if(fault==2)payload[6]=1;
            if(fault==3)payload[7]=1;
            if(fault==4)payload[8]++;
            if(fault==5)peer=0;
            if(fault==6)peer=UINT16_MAX;
            if(fault==7)message=GGMEM_SHARED;
            assert(gg_mem_parse_participant(message,payload,12,handle,peer,tag,&output)==GGMEM_INVALID);
            assert(!memcmp(&output,&saved,sizeof(output)));
        }
        assert(gg_mem_parse_participant(id,NULL,12,0x12345678,0x73,0xddccbbaa,&output)==GGMEM_INVALID);
        assert(!memcmp(&output,&saved,sizeof(output)));
        assert(gg_mem_parse_participant(id,participant,12,0x12345678,0x73,0xddccbbaa,NULL)==GGMEM_INVALID);
        assert(!gg_mem_parse_participant(id,participant,12,0x12345678,0x73,0xddccbbaa,&output));
        assert(output.message_id==id && output.handle==0x12345678 && output.participant_vmid==0x73);
        assert(output.mem_info_tag==0xddccbbaa);
        memset(payload,0,12);payload[4]=3;
        assert(!gg_mem_parse_participant(id,payload,12,0,3,0,&output));
        memset(payload,0xff,12);payload[4]=0xfe;payload[5]=0xff;payload[6]=payload[7]=0;
        assert(!gg_mem_parse_participant(id,payload,12,UINT32_MAX,0xfffe,UINT32_MAX,&output));
    }
}
static void shared_is_not_parcel_evidence(void)
{
    uint8_t payload[80];struct gg_mem_shared_prefix output,saved;
    memset(&output,0x5a,sizeof(output));memcpy(&saved,&output,sizeof(output));
    for(size_t n=0;n<20;n++) {
        assert(gg_mem_parse_shared_prefix(shared_prefix,n,&output)==GGMEM_INVALID);
        assert(!memcmp(&output,&saved,sizeof(output)));
    }
    for(unsigned fault=0;fault<11;fault++) {
        memcpy(payload,shared_prefix,20);size_t n=20;
        if(fault==0)payload[4]=1;
        if(fault==1)payload[5]=1;
        if(fault==2)payload[6]=1;
        if(fault==3)payload[7]=1;
        if(fault==4)payload[8]=0;
        if(fault==5){payload[8]=0xff;payload[9]=0xff;}
        if(fault==6)payload[10]=1;
        if(fault==7)payload[11]=1;
        if(fault==8)payload[12]=6;
        if(fault==9)n=GGMEM_MAX_PAYLOAD+1;
        if(fault==10)n=(size_t)-1;
        assert(gg_mem_parse_shared_prefix(payload,n,&output)==GGMEM_INVALID);
        assert(!memcmp(&output,&saved,sizeof(output)));
        assert(gg_mem_parse_shared(payload,n,&output)==GGMEM_INVALID);
        assert(!memcmp(&output,&saved,sizeof(output)));
    }
    assert(gg_mem_parse_shared_prefix(NULL,20,&output)==GGMEM_INVALID);
    assert(gg_mem_parse_shared_prefix(shared_prefix,20,NULL)==GGMEM_INVALID);
    assert(gg_mem_parse_shared(NULL,20,&output)==GGMEM_INVALID);
    assert(gg_mem_parse_shared(shared_prefix,20,NULL)==GGMEM_INVALID);
    assert(!memcmp(&output,&saved,sizeof(output)));
    assert(gg_mem_parse_shared(shared_prefix,20,&output)==GGMEM_UNSUPPORTED);
    assert(!memcmp(&output,&saved,sizeof(output)));
    assert(!gg_mem_parse_shared_prefix(shared_prefix,20,&output));
    assert(output.handle==0x12345678 && output.label==5 && output.mem_info_tag==0xddccbbaa);
    assert(output.owner_vmid==0x45 && !output.mem_type && output.trans_type==2 && !output.flags);
    assert(!output.unparsed_bytes);
    /* Guessed ACL/SGL tails, zero tails and overflowing counts are all
     * unsupported. A diagnostic prefix cannot establish a shared range. */
    for(unsigned tail=0;tail<3;tail++) {
        memset(payload,tail==2?0xff:0,sizeof(payload));memcpy(payload,shared_prefix,20);
        if(tail==1)memcpy(payload+20,share_wire+8,36);
        memcpy(&saved,&output,sizeof(output));
        assert(gg_mem_parse_shared(payload,sizeof(payload),&output)==GGMEM_UNSUPPORTED);
        assert(!memcmp(&output,&saved,sizeof(output)));
        assert(!gg_mem_parse_shared_prefix(payload,sizeof(payload),&output));
        assert(output.unparsed_bytes==sizeof(payload)-20);
    }
}
static uint32_t random_state=0x651ab329;
static uint32_t random_word(void)
{random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state;}
static void malformed_fuzz(void)
{
    uint8_t storage[258];struct gg_mem_participant event,event_saved;
    struct gg_mem_shared_prefix prefix,prefix_saved;uint32_t handle;
    for(unsigned i=0;i<20000;i++) {
        for(unsigned j=0;j<sizeof(storage);j++)storage[j]=(uint8_t)random_word();
        uint8_t *p=storage+1;size_t n=random_word()%257;
        if(i&1)memcpy(p,shared_prefix,20);
        memset(&event,0x5a,sizeof(event));memcpy(&event_saved,&event,sizeof(event));
        memset(&prefix,0x5a,sizeof(prefix));memcpy(&prefix_saved,&prefix,sizeof(prefix));
        int code=gg_mem_parse_participant(i&1?GGMEM_ACCEPTED:GGMEM_RELEASED,p,n,
            0x12345678,0x73,0xddccbbaa,&event);
        if(code)assert(!memcmp(&event,&event_saved,sizeof(event)));
        code=gg_mem_parse_shared_prefix(p,n,&prefix);
        if(code)assert(!memcmp(&prefix,&prefix_saved,sizeof(prefix)));
        memcpy(&prefix_saved,&prefix,sizeof(prefix));
        assert(gg_mem_parse_shared(p,n,&prefix)!=GGMEM_OK);
        assert(!memcmp(&prefix,&prefix_saved,sizeof(prefix)));
        handle=0x5a5a5a5a;code=gg_mem_parse_share_reply(p,n,&handle);
        if(code)assert(handle==0x5a5a5a5a);
        assert((gg_mem_parse_notify_reply(p,n)==GGMEM_OK)==(n==0));
    }
}
int main(void)
{
    builders();replies();participant_events();shared_is_not_parcel_evidence();malformed_fuzz();
    puts("GPU Gunyah memory: exact share/notify bytes, dynamic VMID ACL, participant events, unsupported shared tails and 20000 malformed fixtures passed");
    return 0;
}
