/* Offline RM wire fixtures only: no HVC, device, capability or READY producer.
 * Wire builders below use the pinned rsc_mgr.c ABI independently of the codec.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_gunyah_rm.h"

enum { REQUEST_ID=0x56000001, EVENT_ID=0x56100008, PACKETS=66 };
struct packet { uint8_t data[240];size_t bytes;unsigned push; };
struct fixture {
    struct ggrm_state state;
    struct ggrm_io io;
    struct packet outgoing[62],incoming[PACKETS];
    unsigned out_count,in_count,out_at,in_at;
    unsigned send_calls,receive_calls,delays,events,valid_calls;
    unsigned full,empty,flood,frozen,reenter,uncertain_after;
    int live,send_status,receive_status,notification_status,delay_status;
    unsigned lose_send,lose_receive,lose_notification,lose_delay,change_epoch;
    unsigned late_send,late_receive,late_notification,regress_send;
    unsigned late_valid,regress_valid;
    uint64_t clock;
    uint32_t event_id;
    size_t event_bytes;
    uint8_t event[GGRM_MAX_PAYLOAD];
};
/* The transport is intentionally too large for an owning kernel's stack. */
static struct fixture f;
static uint8_t request[GGRM_MAX_REQUEST],expected[GGRM_MAX_PAYLOAD];

static uint16_t wire16(const uint8_t *p)
{return (uint16_t)((unsigned)p[0]|(unsigned)p[1]<<8);}
static void wire32(uint8_t *p,uint32_t value)
{for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(value>>(i*8));}
static void wire_header(uint8_t *p,unsigned type,unsigned continuations,uint16_t seq,uint32_t id)
{
    p[0]=0x21;p[1]=(uint8_t)(continuations*4+type);
    p[2]=(uint8_t)seq;p[3]=(uint8_t)(seq>>8);wire32(p+4,id);
}
static void expect_request(uint16_t seq,const void *payload,size_t bytes)
{
    const uint8_t *p=(const uint8_t *)payload;
    size_t offset=0;unsigned count=bytes?(unsigned)((bytes-1)/232):0;
    do {
        assert(f.out_count<62);
        struct packet *packet=&f.outgoing[f.out_count++];
        size_t amount=bytes-offset;if(amount>232)amount=232;
        wire_header(packet->data,offset?0:1,count,seq,REQUEST_ID);
        if(amount)memcpy(packet->data+8,p+offset,amount);
        packet->bytes=8+amount;offset+=amount;packet->push=offset==bytes;
    } while(offset<bytes);
}
static struct packet *incoming(unsigned type,unsigned continuations,uint16_t seq,
    uint32_t id,uint32_t error,const void *payload,size_t bytes)
{
    assert(f.in_count<PACKETS);
    struct packet *packet=&f.incoming[f.in_count++];
    size_t header=type==2?12:8;assert(header+bytes<=240);
    wire_header(packet->data,type,continuations,seq,id);
    if(type==2)wire32(packet->data+8,error);
    if(bytes)memcpy(packet->data+header,payload,bytes);
    packet->bytes=header+bytes;return packet;
}
static int valid(void *cookie,uint64_t epoch)
{
    struct fixture *fixture=cookie;++fixture->valid_calls;
    if(fixture->state.received_frames && fixture->late_valid)fixture->clock+=1001;
    if(fixture->state.received_frames && fixture->regress_valid)--fixture->clock;
    assert(epoch==17);return fixture->live;
}
static int send_frame(void *cookie,const void *data,size_t bytes,unsigned push)
{
    struct fixture *fixture=cookie;++fixture->send_calls;
    assert(fixture->out_at<fixture->out_count);
    const struct packet *packet=&fixture->outgoing[fixture->out_at];
    assert(bytes==packet->bytes && push==packet->push);
    assert(!memcmp(data,packet->data,bytes));
    if(fixture->reenter) {
        assert(ggrm_call(&fixture->io,&fixture->state,REQUEST_ID,NULL,0,1000)==GGRM_BUSY);
    }
    if(fixture->lose_send)fixture->live=0;
    if(fixture->change_epoch)fixture->state.epoch=18;
    if(fixture->late_send)fixture->clock+=1001;
    if(fixture->regress_send)--fixture->clock;
    if(fixture->uncertain_after && fixture->out_at==fixture->uncertain_after)return -1;
    if(fixture->send_status)return fixture->send_status;
    if(fixture->full) {
        if(fixture->full!=~0u)--fixture->full;
        return 1;
    }
    ++fixture->out_at;return 0;
}
static int receive_frame(void *cookie,void *data,size_t capacity,size_t *bytes)
{
    struct fixture *fixture=cookie;++fixture->receive_calls;
    assert(capacity==240 && *bytes==0);
    if(fixture->lose_receive)fixture->live=0;
    if(fixture->late_receive)fixture->clock+=1001;
    if(fixture->receive_status) {
        if(fixture->receive_status==2)*bytes=1;
        return fixture->receive_status==2?1:fixture->receive_status;
    }
    if(fixture->empty) {
        if(fixture->empty!=~0u)--fixture->empty;
        return 1;
    }
    if(fixture->flood) {
        wire_header(data,3,0,(uint16_t)fixture->receive_calls,EVENT_ID);
        *bytes=8;return 0;
    }
    if(fixture->in_at==fixture->in_count)return 1;
    const struct packet *packet=&fixture->incoming[fixture->in_at++];
    memcpy(data,packet->data,packet->bytes>capacity?capacity:packet->bytes);
    *bytes=packet->bytes;return 0;
}
static uint64_t now(void *cookie){return ((struct fixture *)cookie)->clock;}
static int delay(void *cookie,unsigned us)
{
    struct fixture *fixture=cookie;++fixture->delays;assert(us==100);
    if(!fixture->frozen)fixture->clock+=us;
    if(fixture->lose_delay)fixture->live=0;
    return fixture->delay_status;
}
static int notification(void *cookie,uint32_t id,const void *payload,size_t bytes)
{
    struct fixture *fixture=cookie;++fixture->events;
    assert(bytes<=sizeof(fixture->event));
    fixture->event_id=id;fixture->event_bytes=bytes;
    memcpy(fixture->event,payload,bytes);
    if(fixture->lose_notification)fixture->live=0;
    if(fixture->late_notification)fixture->clock+=1001;
    return fixture->notification_status;
}
static void reset(void)
{
    memset(&f,0,sizeof(f));f.live=1;f.notification_status=1;f.clock=100;
    f.io.cookie=&f;f.io.valid=valid;f.io.send=send_frame;f.io.receive=receive_frame;
    f.io.now_us=now;f.io.delay_us=delay;f.io.notification=notification;
    assert(ggrm_init(&f.state,17)==GGRM_OK);
}
static void empty_request(void){expect_request(0,NULL,0);}
static void empty_reply(uint16_t seq,uint32_t error)
{incoming(2,0,seq,REQUEST_ID,error,NULL,0);}
static void rejected(unsigned result)
{
    assert(ggrm_call(&f.io,&f.state,REQUEST_ID,NULL,0,1000)==(int)result);
    assert(f.state.quarantined && !f.state.busy && f.state.last_result==result);
    unsigned sent=f.send_calls,received=f.receive_calls,checked=f.valid_calls;
    assert(ggrm_call(&f.io,&f.state,REQUEST_ID,NULL,0,1000)==GGRM_QUARANTINED);
    assert(f.send_calls==sent && f.receive_calls==received && f.valid_calls==checked);
    assert(ggrm_init(&f.state,18)==GGRM_ARGUMENT);
}
static void requests(void)
{
    const size_t sizes[]={0,1,231,232,233,464,465,GGRM_MAX_REQUEST};
    for(size_t i=0;i<sizeof(request);i++)request[i]=(uint8_t)(i*13+9);
    for(unsigned i=0;i<sizeof(sizes)/sizeof(*sizes);i++) {
        reset();expect_request(0,request,sizes[i]);empty_reply(0,0);
        assert(ggrm_call(&f.io,&f.state,REQUEST_ID,request,sizes[i],1000)==GGRM_OK);
        assert(f.out_at==f.out_count && f.in_at==1 && !f.state.quarantined);
        assert(f.state.sent_frames==f.out_count && f.state.received_frames==1);
        assert(f.state.reply.done && !f.state.reply.error && !f.state.reply.bytes);
        assert(f.state.next_sequence==1 && !f.state.busy);
    }
    reset();empty_request();empty_reply(0,0);f.full=3;f.empty=2;f.reenter=1;
    assert(ggrm_call(&f.io,&f.state,REQUEST_ID,NULL,0,1000)==GGRM_OK);
    assert(f.send_calls==4 && f.receive_calls==3 && f.delays==5 && f.state.sent_frames==1);
}
static void remote_errors_and_sequences(void)
{
    const uint32_t errors[]={1,3,14,UINT32_MAX};
    for(unsigned i=0;i<sizeof(errors)/sizeof(*errors);i++) {
        reset();empty_request();expect_request(1,NULL,0);empty_reply(0,errors[i]);empty_reply(1,0);
        assert(ggrm_call(&f.io,&f.state,REQUEST_ID,NULL,0,1000)==GGRM_REMOTE);
        assert(f.state.reply.error==errors[i] && !f.state.quarantined && !f.state.busy);
        assert(ggrm_call(&f.io,&f.state,REQUEST_ID,NULL,0,1000)==GGRM_OK);
        assert(f.state.next_sequence==2 && f.out_at==2 && f.in_at==2);
    }
    reset();empty_request();incoming(2,1,0,REQUEST_ID,3,NULL,0);
    incoming(0,1,0,REQUEST_ID,0,"denied",6);
    assert(ggrm_call(&f.io,&f.state,REQUEST_ID,NULL,0,1000)==GGRM_REMOTE);
    assert(f.state.reply.error==3 && f.state.reply.bytes==6 && f.state.received_frames==2);
    assert(!memcmp(f.state.reply.data,"denied",6) && !f.state.quarantined);
    reset();empty_request();incoming(2,1,0,REQUEST_ID,3,NULL,0);
    rejected(GGRM_TIMEOUT); /* A partial RM error is still unconfirmed. */
    reset();f.state.next_sequence=65535;expect_request(65535,NULL,0);empty_reply(65535,0);
    assert(ggrm_call(&f.io,&f.state,REQUEST_ID,NULL,0,1000)==GGRM_OK);
    assert(wire16(f.outgoing[0].data+2)==65535 && f.state.next_sequence==65536);
    unsigned checks=f.valid_calls;
    assert(ggrm_call(&f.io,&f.state,REQUEST_ID,NULL,0,1000)==GGRM_SEQUENCE);
    assert(f.send_calls==1 && f.receive_calls==1 && f.valid_calls==checks && !f.state.quarantined);
    /* A late duplicate from the preceding completed exchange cannot satisfy
     * the next RPC, even when both calls use the same message ID. */
    reset();empty_request();expect_request(1,NULL,0);empty_reply(0,0);empty_reply(0,0);
    assert(ggrm_call(&f.io,&f.state,REQUEST_ID,NULL,0,1000)==GGRM_OK);
    rejected(GGRM_PROTOCOL);
}
static void notifications_and_fragments(void)
{
    static const uint8_t event[]={1,2,3,4,5,6,7},reply[]={9,8,7,6,5};
    reset();empty_request();
    incoming(3,0,34,EVENT_ID,0,NULL,0);
    incoming(3,2,35,EVENT_ID,0,NULL,0);
    incoming(0,2,35,EVENT_ID,0,NULL,0); /* Header-only continuation is legal. */
    incoming(0,2,35,EVENT_ID,0,event,sizeof(event));
    incoming(2,1,0,REQUEST_ID,0,reply,3);
    incoming(0,1,0,REQUEST_ID,0,reply+3,2);
    assert(ggrm_call(&f.io,&f.state,REQUEST_ID,NULL,0,1000)==GGRM_OK);
    assert(f.events==2 && f.event_id==EVENT_ID && f.event_bytes==sizeof(event));
    assert(!memcmp(f.event,event,sizeof(event)) && f.state.reply.bytes==sizeof(reply));
    assert(!memcmp(f.state.reply.data,reply,sizeof(reply)) && f.state.received_frames==6);
    reset();empty_request();incoming(2,1,0,REQUEST_ID,0,NULL,0);
    incoming(0,1,0,REQUEST_ID,0,NULL,0);
    assert(ggrm_call(&f.io,&f.state,REQUEST_ID,NULL,0,1000)==GGRM_OK);
    assert(!f.state.reply.bytes && f.state.received_frames==2);
    for(size_t i=0;i<sizeof(expected);i++)expected[i]=(uint8_t)(i*7+3);
    reset();empty_request();incoming(2,62,0,REQUEST_ID,0,expected,228);
    for(unsigned i=0;i<62;i++)incoming(0,62,0,REQUEST_ID,0,expected+228+i*232,232);
    assert(ggrm_call(&f.io,&f.state,REQUEST_ID,NULL,0,1000)==GGRM_OK);
    assert(f.state.reply.bytes==GGRM_MAX_REPLY && f.state.received_frames==63);
    assert(!memcmp(f.state.reply.data,expected,GGRM_MAX_REPLY));
    reset();empty_request();incoming(3,62,99,EVENT_ID,0,expected,232);
    for(unsigned i=0;i<62;i++)incoming(0,62,99,EVENT_ID,0,expected+232+i*232,232);
    empty_reply(0,0);
    assert(ggrm_call(&f.io,&f.state,REQUEST_ID,NULL,0,1000)==GGRM_OK);
    assert(f.events==1 && f.event_bytes==GGRM_MAX_PAYLOAD && f.state.received_frames==64);
    assert(!memcmp(f.event,expected,GGRM_MAX_PAYLOAD));
}
static void malformed_wire(void)
{
    /* A valid request was accepted, so every malformed response is ambiguous
     * and must prevent any subsequent RPC on this channel state. */
    for(unsigned fault=0;fault<10;fault++) {
        reset();empty_request();struct packet *p=incoming(2,0,0,REQUEST_ID,0,NULL,0);
        switch(fault) {
        case 0:p->bytes=0;break;
        case 1:p->bytes=7;break;
        case 2:p->bytes=241;break;
        case 3:p->data[0]=0x22;break;
        case 4:p->data[1]=1;break;
        case 5:wire32(p->data+4,0);break;
        case 6:p->data[2]=1;break;
        case 7:wire32(p->data+4,REQUEST_ID+1);break;
        case 8:p->bytes=11;break;
        case 9:p->data[1]=(uint8_t)(63*4+2);break;
        }
        rejected(GGRM_PROTOCOL);assert(f.state.sent_frames==1 && f.state.received_frames==1);
    }
    reset();empty_request();incoming(0,0,0,REQUEST_ID,0,NULL,0);rejected(GGRM_PROTOCOL);
    for(unsigned fault=0;fault<7;fault++) {
        reset();empty_request();incoming(2,1,0,REQUEST_ID,0,NULL,0);
        unsigned type=0,count=1;uint16_t seq=0;uint32_t id=REQUEST_ID;
        if(fault==0)seq=1;
        if(fault==1)id++;
        if(fault==2)count=0;
        if(fault==3)count=2;
        if(fault==4)type=2;
        if(fault==5){type=3;id=EVENT_ID;seq=10;}
        if(fault==6)type=1;
        incoming(type,count,seq,id,0,NULL,0);rejected(GGRM_PROTOCOL);
        assert(f.state.received_frames==2 && !f.events);
    }
    /* Do not silently abandon an unfinished notification to accept a reply
     * or a second notification. The canonical source aborts that assembly. */
    for(unsigned second=2;second<=3;second++) {
        reset();empty_request();incoming(3,1,5,EVENT_ID,0,NULL,0);
        incoming(second,0,second==2?0:6,second==2?REQUEST_ID:EVENT_ID,0,NULL,0);
        rejected(GGRM_PROTOCOL);assert(!f.events);
    }
    reset();empty_request();incoming(3,0,5,EVENT_ID,0,NULL,0);
    incoming(0,0,5,EVENT_ID,0,NULL,0);rejected(GGRM_PROTOCOL);assert(f.events==1);
}
static void transport_and_lease_faults(void)
{
    reset();expect_request(0,request,233);f.uncertain_after=1;
    assert(ggrm_call(&f.io,&f.state,REQUEST_ID,request,233,1000)==GGRM_TRANSPORT);
    assert(f.state.quarantined && f.state.sent_frames==1 && f.send_calls==2 && !f.receive_calls);
    unsigned attempts=f.send_calls;
    assert(ggrm_call(&f.io,&f.state,REQUEST_ID,request,233,1000)==GGRM_QUARANTINED);
    assert(f.send_calls==attempts); /* Never replay an uncertain continuation. */
    for(unsigned failure=0;failure<11;failure++) {
        reset();empty_request();empty_reply(0,0);unsigned result=GGRM_TRANSPORT;
        switch(failure) {
        case 0:f.send_status=-1;break;
        case 1:f.send_status=2;break;
        case 2:f.receive_status=-1;break;
        case 3:f.receive_status=2;break; /* EMPTY cannot also publish bytes. */
        case 4:f.full=1;f.delay_status=-1;break;
        case 5:f.live=0;result=GGRM_LEASE;break;
        case 6:f.lose_send=1;result=GGRM_LEASE;break;
        case 7:f.lose_receive=1;result=GGRM_LEASE;break;
        case 8:f.full=1;f.lose_delay=1;result=GGRM_LEASE;break;
        case 9:f.change_epoch=1;result=GGRM_LEASE;break;
        case 10:f.receive_status=3;break;
        }
        rejected(result);
        if(failure==5)assert(!f.send_calls && !f.receive_calls);
        if(failure==0 || failure==1 || failure==6 || failure==9)assert(!f.receive_calls);
    }
    for(unsigned failure=0;failure<4;failure++) {
        reset();empty_request();incoming(3,0,5,EVENT_ID,0,NULL,0);empty_reply(0,0);
        if(failure==0)f.notification_status=0;
        if(failure==1)f.notification_status=-1;
        if(failure==2)f.lose_notification=1;
        if(failure==3)f.late_notification=1;
        rejected(failure==2?GGRM_LEASE:failure==3?GGRM_TIMEOUT:GGRM_PROTOCOL);
        assert(f.events==1 && f.receive_calls==1);
    }
}
static void deadlines(void)
{
    for(unsigned failure=0;failure<8;failure++) {
        reset();empty_request();empty_reply(0,0);
        if(failure==0)f.full=~0u;
        if(failure==1)f.empty=~0u;
        if(failure==2)f.late_send=1;
        if(failure==3)f.late_receive=1;
        if(failure==4)f.regress_send=1;
        if(failure==5){f.full=~0u;f.frozen=1;}
        if(failure==6)f.late_valid=1;
        if(failure==7)f.regress_valid=1;
        rejected(GGRM_TIMEOUT);
        assert(f.send_calls+f.receive_calls<=GGRM_POLLS);
        if(failure==2)assert(f.state.sent_frames==1 && !f.receive_calls);
        if(failure==3)assert(f.state.received_frames==1 && !f.state.reply.done);
        if(failure==5)assert(f.send_calls==GGRM_POLLS && !f.receive_calls);
    }
    reset();empty_request();f.flood=1;
    rejected(GGRM_TIMEOUT);
    assert(f.send_calls==1 && f.receive_calls==GGRM_POLLS-1 && f.events==GGRM_POLLS-1);
}
static void arguments(void)
{
    reset();unsigned checks=f.valid_calls;
    assert(ggrm_call(NULL,&f.state,REQUEST_ID,NULL,0,1000)==GGRM_ARGUMENT);
    assert(ggrm_call(&f.io,NULL,REQUEST_ID,NULL,0,1000)==GGRM_ARGUMENT);
    assert(ggrm_call(&f.io,&f.state,0,NULL,0,1000)==GGRM_ARGUMENT);
    assert(ggrm_call(&f.io,&f.state,REQUEST_ID,NULL,1,1000)==GGRM_ARGUMENT);
    assert(ggrm_call(&f.io,&f.state,REQUEST_ID,request,sizeof(request)+1,1000)==GGRM_ARGUMENT);
    assert(ggrm_call(&f.io,&f.state,REQUEST_ID,f.state.tx,1,1000)==GGRM_ARGUMENT);
    assert(ggrm_call(&f.io,&f.state,REQUEST_ID,(const void *)(UINTPTR_MAX-1),3,1000)==GGRM_ARGUMENT);
    assert(ggrm_call(&f.io,&f.state,REQUEST_ID,NULL,0,0)==GGRM_ARGUMENT);
    assert(ggrm_call(&f.io,&f.state,REQUEST_ID,NULL,0,3000001)==GGRM_ARGUMENT);
    assert(f.valid_calls==checks && !f.send_calls && !f.receive_calls && !f.state.busy);
    assert(!f.state.quarantined && f.state.next_sequence==0);
    assert(ggrm_init(NULL,17)==GGRM_ARGUMENT && ggrm_init(&f.state,18)==GGRM_ARGUMENT);
    memset(&f.state,0,sizeof(f.state));assert(ggrm_init(&f.state,0)==GGRM_ARGUMENT);
}
static uint32_t random_state=0x357bc129;
static uint32_t random_word(void)
{random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state;}
static void malformed_fuzz(void)
{
    /* Structured starts followed by corrupt headers/lengths stress the
     * assembly boundary, not only rejection of unrelated random first bytes. */
    for(unsigned i=0;i<20000;i++) {
        reset();f.state.message_id=REQUEST_ID;
        wire_header(f.state.rx,2,1,0,REQUEST_ID);wire32(f.state.rx+8,0);
        assert(ggrm_receive_frame(&f.state,0,12));
        for(unsigned j=0;j<240;j++)f.state.rx[j]=(uint8_t)random_word();
        if(i&1)wire_header(f.state.rx,0,1,0,REQUEST_ID);
        (void)ggrm_receive_frame(&f.state,0,random_word()%243);
        assert(f.state.reply.bytes<=GGRM_MAX_REPLY);
    }
}
int main(void)
{
    requests();remote_errors_and_sequences();notifications_and_fragments();
    malformed_wire();transport_and_lease_faults();deadlines();arguments();malformed_fuzz();
    puts("GPU Gunyah RM: pinned wire boundaries, notifications, sequence isolation, quarantine, deadlines and 20000 malformed fixtures passed");
    return 0;
}
