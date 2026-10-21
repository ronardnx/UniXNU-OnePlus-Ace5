#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ios16/giulia_wifi_qmi.h"
struct fixture {
    struct gwq_state s;struct gwq_io io;
    unsigned phase,requests,chunks[5],resumes,drop,wrong_id,remote_fail,secure,missing_board;
    unsigned corrupt,fail_asset,fail_alloc,overlap,allocs,modes,configs;
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
    if(msg==0x34) { a=gwq_field(t,len,0x1c,&z);assert(a&&z==1&&*a==0);f->phase=2; }
    if(msg==0x36) {
        a=gwq_field(t,len,1,&z);assert(a && z==35 && a[0]==2);
        assert(gwq_u32(a+9)==65536 && gwq_u32(a+13)==1 && !a[17]);
        assert(gwq_u32(a+26)==4096 && gwq_u32(a+30)==2 && !gwq_u64(a+18));
        if(!f->fail_alloc)assert(gwq_u64(a+1)==0x20000000);f->phase=3;
    }
    if(msg==0x25) {
        a=gwq_field(t,len,0x15,&z);assert(a&&z==1);unsigned kind=*a;assert(kind==1||kind==4);
        const uint8_t *source=kind==1?f->board:f->reg;size_t size=kind==1?sizeof(f->board):sizeof(f->reg);
        a=gwq_field(t,len,0x10,&z);assert(a&&z==4&&gwq_u32(a)==0xff);
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
    uint8_t reply[512]={2};gwq_w16(reply+1,(uint16_t)(gwq_u16(p+33)+f->wrong_id));gwq_w16(reply+3,(uint16_t)msg);
    size_t bytes=0;uint8_t result[4]={0};if(f->remote_fail){result[0]=1;result[2]=0x34;}
    gwq_tlv(reply+7,sizeof(reply)-7,&bytes,2,result,4);
    if(msg==0x24) {
        uint8_t b[8]={0};gwq_w32(b,0x1234);gwq_tlv(reply+7,sizeof(reply)-7,&bytes,0x10,b,8);
        gwq_w32(b,0xff);if(!f->missing_board)gwq_tlv(reply+7,sizeof(reply)-7,&bytes,0x11,b,4);
    }
    if(f->corrupt)reply[8]=255;
    gwq_w16(reply+5,(uint16_t)bytes);return packet(f,1,reply,7+bytes,0,0);
}
static int pump(void *ctx,unsigned us) {
    struct fixture *f=ctx;assert(us==1000);
    if(f->phase==0){uint8_t b[20]={0};gwq_w32(b,4);gwq_w32(b+4,0x45);gwq_w32(b+8,0x101);
        gwq_w32(b+12,2);gwq_w32(b+16,0x1234);f->phase=1;return packet(f,4,b,20,0,1);}
    if(f->phase==2){uint8_t b[40]={1,32,0,2}; /* TLV 1: count and two segments, first has cfg */
        gwq_w32(b+4,65536);gwq_w32(b+8,1);b[12]=1;gwq_w64(b+13,0);gwq_w32(b+21,65536);b[25]=(uint8_t)f->secure;
        gwq_w32(b+26,4096);gwq_w32(b+30,2);b[34]=0;f->phase=20;return indication(f,0x35,b,35);}
    if(f->phase==3){f->phase=30;return indication(f,0x37,NULL,0);}
    if(f->phase==4){f->phase=40;return indication(f,0x38,NULL,0);}
    return 0;
}
static int allocate(void *ctx,size_t size,uint64_t *pa,void **va) {
    struct fixture *f=ctx;if(f->fail_alloc)return 1;
    size_t start=(f->used+4095)&~(size_t)4095;if(f->overlap)start=0;
    if(size>sizeof(f->ram)-start)return 1;*pa=0x20000000+start;*va=f->ram+start;
    f->used=start+size;f->allocs++;return 0;
}
static void clean(void *ctx,const void *v,size_t n){struct fixture *f=ctx;assert((const uint8_t *)v>=f->ram && (const uint8_t *)v+n<=f->ram+sizeof(f->ram));}
static int asset(void *ctx,unsigned kind,unsigned major,uint32_t chip,uint32_t board,const void **v,size_t *n) {
    struct fixture *f=ctx;assert(major==2 && chip==0x1234 && board==0xff);if(f->fail_asset)return 1;
    if(kind==GWQ_REGDB){*v=f->reg;*n=sizeof(f->reg);}else if(kind==GWQ_BOARD){*v=f->board;*n=sizeof(f->board);}
    else{assert(kind==GWQ_PHY);*v=f->phy;*n=sizeof(f->phy);}return 0;
}
static struct fixture *make(void) {
    struct fixture *f=calloc(1,sizeof(*f));assert(f);f->io=(struct gwq_io){f,send_packet,pump,allocate,clean,asset,0x20000000,sizeof(f->ram)};
    for(size_t i=0;i<sizeof(f->reg);i++)f->reg[i]=(uint8_t)(i*7);
    for(size_t i=0;i<sizeof(f->board);i++)f->board[i]=(uint8_t)(i*11);
    for(size_t i=0;i<sizeof(f->phy);i++)f->phy[i]=(uint8_t)(i*13);
    return f;
}
int main(void) {
    struct fixture *f=make();assert(!gwq_initialize(&f->s,&f->io,2));
    assert(f->s.provisioned && f->s.fw_ready && f->chunks[4]==2 && f->chunks[1]==3 && f->resumes==3 && f->allocs==2);
    uint8_t pipes[20]={0},svc[12]={0};assert(!gwq_configure(&f->s,&f->io,pipes,1,svc,1));assert(f->configs==1&&f->modes==1);free(f);
    unsigned *flag;for(unsigned i=0;i<9;i++) {
        f=make();switch(i){case 0:flag=&f->drop;break;case 1:flag=&f->wrong_id;break;case 2:flag=&f->remote_fail;break;
            case 3:flag=&f->secure;break;case 4:flag=&f->missing_board;break;case 5:flag=&f->corrupt;break;
            case 6:flag=&f->fail_asset;break;case 7:flag=&f->fail_alloc;break;default:flag=&f->overlap;break;}
        *flag=1;assert(gwq_initialize(&f->s,&f->io,2));assert(!f->s.provisioned && !f->modes);
        if(i==3)assert(!f->allocs);if(i==2)assert(f->s.remote_error==0x10034);free(f);
    }
    /* Reject malformed framing, duplicate TLVs, wrong service and reordered
     * firmware indications, without allocating or accepting false readiness. */
    uint8_t dup[]={2,4,0,0,0,0,0,2,0,0};assert(!gwq_tlvs(dup,sizeof(dup)));
    assert(!gwq_tlvs(dup,2));assert(!gwq_tlvs(dup,6));
    for(unsigned i=0;i<32;i++){f=make();uint8_t b[32]={1};assert(gwq_receive(&f->s,&f->io,b,i));free(f);}
    f=make();f->s.server=1;f->s.node=2;f->s.port=0x1234;assert(indication(f,0x38,NULL,0)==GWQ_STATE);free(f);
    f=make();uint8_t bad[20]={0};gwq_w32(bad,4);gwq_w32(bad+4,0x45);gwq_w32(bad+8,0x201);
    gwq_w32(bad+12,2);gwq_w32(bad+16,123);assert(!packet(f,4,bad,20,0,0)&&!f->s.server);free(f);
    puts("QRTR/QMI: complete firmware exchange, segmented assets, DMA ownership and malformed/failure cases passed");
    return 0;
}
