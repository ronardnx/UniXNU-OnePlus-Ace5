/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * A complete lifecycle in one executable. Only the hardware/remote boundary
 * is simulated; lifecycle, QMI parsing, IP/QMAP and resource arbitration are
 * the exact production headers used by the kernel service.
 */
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>
#include <algorithm>
#include "../ios16/modem/giulia_modem_runtime.h"
#include "../ios16/modem/giulia_modem_qmap.h"
#include "../ios16/modem/giulia_modem_data.h"
#include "../ios16/modem/giulia_modem_qmux.h"
#include "../ios16/platform/giulia_platform_resources.h"
#include "../ios16/modem/giulia_modem_apple_abi.h"
#include "modem_board_fixture.h"

#define CHECK(x) do { if(!(x)) {std::fprintf(stderr,"system scenario: %s:%d: %s\n",__FILE__,__LINE__,#x);std::abort();} } while(0)
static uint64_t total_actions,total_packets,total_scenarios,action_mask,total_qmux;
static void le16(std::vector<uint8_t>& b,uint16_t x){b.push_back((uint8_t)x);b.push_back((uint8_t)(x>>8));}
static std::vector<uint8_t> number(uint32_t x){return {(uint8_t)x,(uint8_t)(x>>8),(uint8_t)(x>>16),(uint8_t)(x>>24)};}
static void tlv(std::vector<uint8_t>& b,uint8_t tag,const std::vector<uint8_t>& v)
{b.push_back(tag);le16(b,(uint16_t)v.size());b.insert(b.end(),v.begin(),v.end());}

struct System {
    gmr_runtime core{};
    gpr_broker broker{};
    std::deque<gmr_action> work;
    std::vector<gpr_token> leases;
    gpr_token stream{},memory{},cx_vote{},bw_vote{};
    gpr_token peer_tokens[4]{};
    int wifi_owner=1,audio_owner=2,gpu_owner=3;
    uint64_t lease_epoch=0,now=1,packet_cookie=0x10000;
    uint32_t next_endpoint=1,next_sms_id=91;
    bool memory_remote=false,dma_active=false,firmware=false,ipc=false,storage=false;
    bool ipa[GMR_BEARERS]{},wda[GMR_BEARERS]{},bound[GMR_BEARERS]{},mapped[GMR_BEARERS]{},network[GMR_BEARERS]{};
    gmr_identity ring_binding[GMR_BEARERS]{};
    std::vector<uint8_t> disk,dirty_disk,last_packet;
    unsigned flushes=0,peer_acks=0,received_packets=0;
    bool boot_request_sent=false;
    unsigned boot_ack_target=0;
    struct gmr_event boot_request{};
    uint32_t fault_kind=0;
    int fault_status=GMR_IO;
    bool drop_fault=false,partial_write=false;
    bool partial_transfer=false,card_present[2]{true,true};

    static int submit(void *p,const gmr_action *a) {
        auto& s=*(System*)p;
        if(s.work.size()>=GMR_PENDING)return GMR_FULL;
        s.work.push_back(*a);return GMR_OK;
    }
    static int packet(void *p,const gmr_identity *,const uint8_t *bytes,size_t n) {
        auto& s=*(System*)p;s.last_packet.assign(bytes,bytes+n);s.received_packets++;
        return GMR_OK;
    }
    System():disk(4096),dirty_disk(4096) {
        gmr_ops ops{};ops.ctx=this;ops.submit=submit;ops.packet=packet;
        CHECK(gmr_init(&core,&ops)==GMR_OK);CHECK(gmr_tick(&core,now)==GMR_OK);
        CHECK(gpr_open_epoch(&broker,&wifi_owner,1)==GPR_OK);
        CHECK(gpr_open_epoch(&broker,&audio_owner,1)==GPR_OK);
        CHECK(gpr_open_epoch(&broker,&gpu_owner,1)==GPR_OK);
        gpr_key qmp{GPR_IPCC,0,0,0,0x406000,0x1000};
        gpr_key audio{GPR_IPCC,0,3,0,0x406000,0x1000};
        gpr_key cx{GPR_VOTE_MAX,1,1,0,0,0};
        gpr_key bw{GPR_VOTE_SUM,1,2,0,0,0};
        CHECK(gpr_claim(&broker,&qmp,&wifi_owner,1,0,0,&peer_tokens[0])==GPR_OK);
        CHECK(gpr_claim(&broker,&audio,&audio_owner,1,0,0,&peer_tokens[1])==GPR_OK);
        CHECK(gpr_claim(&broker,&cx,&gpu_owner,1,0,768,&peer_tokens[2])==GPR_OK);
        CHECK(gpr_claim(&broker,&bw,&audio_owner,1,0,200,&peer_tokens[3])==GPR_OK);
    }
    void peers_intact() {
        CHECK(gpr_owns(&broker,&peer_tokens[0],&wifi_owner,1));
        CHECK(gpr_owns(&broker,&peer_tokens[1],&audio_owner,1));
        CHECK(gpr_owns(&broker,&peer_tokens[2],&gpu_owner,1));
        CHECK(gpr_owns(&broker,&peer_tokens[3],&audio_owner,1));
        gpr_key cx{GPR_VOTE_MAX,1,1,0,0,0};uint64_t aggregate=0;
        CHECK(gpr_aggregate(&broker,&cx,&aggregate)==GPR_OK && aggregate==768);
    }
    void claim(gpr_key key,uint64_t vote=0,gpr_token *save=nullptr) {
        gpr_token token{};CHECK(gpr_claim(&broker,&key,this,lease_epoch,0,vote,&token)==GPR_OK);
        leases.push_back(token);if(save)*save=token;
    }
    void resources(uint64_t epoch) {
        CHECK(leases.empty());lease_epoch=epoch;
        CHECK(gpr_open_epoch(&broker,this,lease_epoch)==GPR_OK);
        claim({GPR_IPCC,0,2,0,0x406000,0x1000});
        claim({GPR_IPCC,0,2,2,0x406000,0x1000});
        claim({GPR_MEMORY,0,0,0,0x86b00000,0x4900000},0,&memory);
        claim({GPR_MEMORY,0,0,0,0x8b400000,0x800000});
        claim({GPR_MEMORY,0,0,0,0x84800000,0x200000});
        claim({GPR_MEMORY,0,0,0,0x8bc00000,0xf400000});
        claim({GPR_MEMORY,0,0,0,0x9b000000,0x80000});
        claim({GPR_DMA_STREAM,0,0x4a0,0,0x15000000,0},0,&stream);
        claim({GPR_SMEM_ITEM,1,478,0,0,0});
        claim({GPR_VOTE_MAX,1,1,0,0,0},384,&cx_vote);
        claim({GPR_VOTE_SUM,1,2,0,0,0},300,&bw_vote);
        gpr_key all{GPR_IPCC,0,0,0,0x400000,0x140000};gpr_token token{};
        CHECK(gpr_claim(&broker,&all,&gpu_owner,1,GPR_IPCC_APERTURE,0,&token)==GPR_CONFLICT);
        CHECK(gpr_open_epoch(&broker,this,lease_epoch+1)==GPR_BUSY);
        peers_intact();
    }
    gmr_command command(uint32_t kind,uint8_t slot=0,uint8_t bearer=GMR_NO_BEARER) {
        gmr_command c{};c.kind=kind;c.slot=slot;c.bearer=bearer;return c;
    }
    void issue(uint32_t kind,uint8_t slot=0,uint8_t bearer=GMR_NO_BEARER) {
        auto c=command(kind,slot,bearer);CHECK(gmr_submit(&core,&c)==GMR_OK);
    }
    gmr_identity source(uint16_t service,unsigned slot,unsigned bearer=GMR_NO_BEARER) {
        auto *ep=gmr_service_find(&core,service,(uint8_t)slot);CHECK(ep && ep->available);
        gmr_identity id{};id.epoch=core.state.epoch;id.slot=(uint8_t)slot;id.bearer=(uint8_t)bearer;
        id.slot_generation=core.state.slots[slot].generation;id.endpoint_generation=ep->generation;
        if(bearer<GMR_BEARERS)id.bearer_generation=core.state.bearers[bearer].generation;
        id.node=ep->node;id.port=ep->port;id.service=service;return id;
    }
    void advertise(unsigned slot,uint16_t service) {
        struct gmr_event e{};e.kind=GMR_EV_SERVICE_UP;e.id.epoch=core.state.epoch;e.id.slot=(uint8_t)slot;
        e.id.slot_generation=core.state.slots[slot].generation;e.id.endpoint_generation=next_endpoint++;
        e.id.node=9;e.id.port=0x4000+slot*0x100+service;e.id.service=service;
        CHECK(gmr_event(&core,&e)==GMR_OK);
    }
    void services() {
        for(unsigned slot=0;slot<2;slot++)
            for(uint16_t id:{1,2,3,5,9,11,14,0x1a,0x1c,0x21,0x31}) {
                auto *found=gmr_service_find(&core,id,(uint8_t)slot);
                if(!found || !found->available)advertise(slot,id);
            }
    }
    struct gmr_event reply(const gmr_action& a) {
        struct gmr_event e{};e.kind=GMR_EV_QMI_RESPONSE;e.action_kind=a.kind;e.id=a.id;
        CHECK(a.bytes>=7 && a.payload[0]==0);
        std::vector<uint8_t> body;tlv(body,2,{0,0,0,0});
        switch(a.qmi_kind) {
        case GMR_QMI_SIM: {
            CHECK(a.id.service==11 && a.payload[3]==0x2f && a.bytes==7);
            std::vector<uint8_t> cards{0,0,255,255,0,1,255,255,2};
            if(!card_present[0])cards[0]=cards[1]=255;
            if(!card_present[1])cards[4]=cards[5]=255;
            const uint8_t card[]={1,2,3,10,0,1,2,7,2,0,0,0,0,0,2,3,10,3,3,10};
            for(unsigned slot=0;slot<2;slot++) {
                if(card_present[slot])cards.insert(cards.end(),card,card+sizeof(card));
                else cards.insert(cards.end(),{0,0,0,0,0,0});
            }
            tlv(body,0x10,cards);break;
        }
        case GMR_QMI_REGISTRATION:
            CHECK(a.id.service==3 && a.payload[3]==0x24 && a.bytes==7);
            if(card_present[a.id.slot])tlv(body,1,{1,1,1,2,1,8});
            else tlv(body,1,{0,2,2,2,0});
            tlv(body,0x10,{1});break;
        case GMR_QMI_START:
            CHECK(a.id.service==1 && a.payload[3]==0x20);
            CHECK(bound[a.id.bearer] && wda[a.id.bearer] && ipa[a.id.bearer]);
            tlv(body,1,number(0x550001+a.id.bearer));break;
        case GMR_QMI_SETTINGS:
            CHECK(a.payload[3]==0x2d && a.bytes==14);
            if(core.state.bearers[a.id.bearer].ip_family==6) {
                std::vector<uint8_t> addr(17);addr[0]=0x20;addr[1]=1;addr[2]=0x0d;addr[3]=0xb8;
                addr[15]=(uint8_t)(2+a.id.bearer);addr[16]=64;tlv(body,0x25,addr);
                addr[15]=1;tlv(body,0x26,addr);addr.resize(16);addr[15]=53;tlv(body,0x27,addr);
                tlv(body,0x2b,{6});
            } else {
                tlv(body,0x1e,number(0x0a000002+a.id.bearer));tlv(body,0x20,number(0x0a000001));
                tlv(body,0x21,number(0xffffff00));tlv(body,0x15,number(0x0a000035));tlv(body,0x2b,{4});
            }
            tlv(body,0x29,number(1500));break;
        case GMR_QMI_STOP:CHECK(!network[a.id.bearer] && !mapped[a.id.bearer]);break;
        default:CHECK(false);
        }
        std::vector<uint8_t> wire{2};le16(wire,a.id.txn);le16(wire,(uint16_t)(a.payload[3]|a.payload[4]<<8));
        le16(wire,(uint16_t)body.size());wire.insert(wire.end(),body.begin(),body.end());
        CHECK(wire.size()<=sizeof(e.payload));e.bytes=(uint16_t)wire.size();std::copy(wire.begin(),wire.end(),e.payload);
        return e;
    }
    bool live(const gmr_action& a) {
        for(const auto& p:core.pending)if(p.used && p.action.kind==a.kind && gmr_identity_equal(&p.action.id,&a.id))return true;
        return false;
    }
    bool step() {
        if(work.empty())return false;
        auto a=work.front();work.pop_front();
        if(!live(a))return true;
        total_actions++;action_mask|=UINT64_C(1)<<a.kind;
        if(a.kind==fault_kind && drop_fault)return true;
        struct gmr_event e{};e.id=a.id;e.action_kind=a.kind;
        e.kind=a.kind==GMR_ACT_QMI?GMR_EV_QMI_RESPONSE:GMR_EV_COMPLETE;
        if(a.kind==fault_kind){e.status=fault_status;fault_kind=0;CHECK(gmr_event(&core,&e)!=GMR_OK);return true;}
        if(a.kind==GMR_ACT_QMI)e=reply(a);
        else switch(a.kind) {
        case GMR_ACT_RESOURCES_ACQUIRE:resources(a.id.epoch);break;
        case GMR_ACT_STORAGE_START:CHECK(!leases.empty());storage=true;break;
        case GMR_ACT_IPC_PREPARE:CHECK(storage);ipc=true;break;
        case GMR_ACT_FIRMWARE_BOOT:
            CHECK(storage && ipc);
            if(!boot_request_sent) {
                boot_request_sent=true;boot_ack_target=peer_acks+1;advertise(0,14);
                boot_request={};boot_request.kind=GMR_EV_STORAGE_REQUEST;
                boot_request.id=source(14,0);boot_request.id.opid=packet_cookie++;
                boot_request.id.txn=17;boot_request.value=1;boot_request.offset=16;boot_request.bytes=4;
                boot_request.payload[0]=0xa5;boot_request.payload[1]=0x86;
                CHECK(gmr_event(&core,&boot_request)==GMR_OK);
                CHECK(gmr_event(&core,&boot_request)==GMR_BUSY);
            }
            if(peer_acks<boot_ack_target){work.push_back(a);return true;}
            CHECK(gpr_transfer_begin(&broker,&memory,this,lease_epoch,0x100)==GPR_OK);
            if(partial_transfer) {
                CHECK(gpr_transfer_complete(&broker,&memory,this,lease_epoch,GPR_TRANSFER_UNCERTAIN)==GPR_UNCERTAIN);
                e.status=GMR_UNCERTAIN;break;
            }
            CHECK(gpr_transfer_complete(&broker,&memory,this,lease_epoch,GPR_TRANSFER_DONE)==GPR_OK);
            CHECK(gpr_release(&broker,&memory,this,lease_epoch)==GPR_REMOTE);
            memory_remote=true;firmware=true;break;
        case GMR_ACT_IPC_START:CHECK(firmware && storage && ipc);break;
        case GMR_ACT_IPA_PREPARE:
            CHECK(firmware && ipc && GM_AP_WAN_EP==2 && GM_AP_WAN_CHANNEL==11 && GM_Q6_WAN_EP==12);
            ipa[a.id.bearer]=true;e.value=GMR_IPA_AP_READY|GMR_IPA_Q6_READY|GMR_IPA_MAP_VALID;break;
        case GMR_ACT_WDA_FORMAT:
            CHECK(ipa[a.id.bearer]);wda[a.id.bearer]=true;e.value=GMR_WDA_RAW_IP|GMR_WDA_QMAP;break;
        case GMR_ACT_WDS_BIND:
            CHECK(wda[a.id.bearer] && a.mux==a.id.bearer+1);
            bound[a.id.bearer]=true;e.value=GMR_BIND_SUBSCRIPTION|GMR_BIND_MUX|GMR_BIND_ENDPOINT;break;
        case GMR_ACT_DMA_CONFIGURE:
            CHECK(bound[a.id.bearer] && a.handle);
            if(!dma_active){CHECK(gpr_mark_dma(&broker,&stream,this,lease_epoch)==GPR_OK);dma_active=true;}
            CHECK(gpr_release(&broker,&stream,this,lease_epoch)==GPR_DMA_ACTIVE);
            ring_binding[a.id.bearer]=a.id;
            mapped[a.id.bearer]=true;e.value=31;break;
        case GMR_ACT_NETWORK_CONFIGURE:
            CHECK(mapped[a.id.bearer] && (a.ip_family==4?gmr_ipv4_usable(&a.ipv4):gmr_ipv6_usable(&a.ipv6)));
            network[a.id.bearer]=true;e.value=15;break;
        case GMR_ACT_DATA_REMOVE:
            network[a.id.bearer]=mapped[a.id.bearer]=false;
            if(std::none_of(std::begin(mapped),std::end(mapped),[](bool v){return v;}) && dma_active) {
                CHECK(gpr_quiesce(&broker,&stream,this,lease_epoch)==GPR_OK);dma_active=false;
            }
            e.value=GMR_REMOVE_COMPLETE;
            break;
        case GMR_ACT_DATA_TX: {
            CHECK(network[a.id.bearer] && mapped[a.id.bearer] && dma_active);
            uint8_t qmap[GMR_PAYLOAD+16];size_t used=0;gmap_packet frame{};
            CHECK(gmd_transmit(&core.state,&a,5,qmap,sizeof(qmap),&used)==GMR_OK);
            CHECK(gmap_decode(qmap,used,5,&frame)==GMAP_OK && frame.mux==a.id.bearer+1);
            CHECK(frame.bytes==a.bytes && !std::memcmp(frame.ip,a.payload,a.bytes));
            last_packet.assign(frame.ip,frame.ip+frame.bytes);e.value=a.bytes;total_packets++;break;
        }
        case GMR_ACT_STORAGE_WRITE:
            CHECK(storage && a.object==1 && a.offset<=dirty_disk.size() && a.bytes<=dirty_disk.size()-a.offset);
            std::copy(a.payload,a.payload+a.bytes,dirty_disk.begin()+a.offset);
            CHECK(core.state.storage_durable==flushes || core.state.storage_writes==core.state.storage_durable);
            e.value=partial_write?a.bytes-1:a.bytes;break;
        case GMR_ACT_STORAGE_FLUSH:CHECK(storage);disk=dirty_disk;flushes=core.state.storage_writes;break;
        case GMR_ACT_STORAGE_REPLY:
            CHECK(storage && disk==dirty_disk && a.peer.service==14 && a.peer.opid && a.handle==GMR_OK);
            peer_acks++;break;
        case GMR_ACT_IMS_REGISTER:CHECK(network[0] || network[1]);break;
        case GMR_ACT_VOICE_START:e.value=77;break;
        case GMR_ACT_MEDIA_START:CHECK(a.handle==77);break;
        case GMR_ACT_SMS_SEND:CHECK(a.bytes);e.value=next_sms_id++;break;
        case GMR_ACT_VOICE_STOP:CHECK(a.handle==77);break;
        case GMR_ACT_MEDIA_DRAIN:break;
        case GMR_ACT_DATA_DRAIN:
            std::fill(std::begin(network),std::end(network),false);
            std::fill(std::begin(mapped),std::end(mapped),false);
            if(dma_active){CHECK(gpr_quiesce(&broker,&stream,this,lease_epoch)==GPR_OK);dma_active=false;}break;
        case GMR_ACT_IPC_STOP:CHECK(!dma_active);ipc=false;break;
        case GMR_ACT_FIRMWARE_STOP:CHECK(!ipc);firmware=false;break;
        case GMR_ACT_MEMORY_RECLAIM:
            CHECK(!firmware && !dma_active);
            if(memory_remote) {
                CHECK(gpr_transfer_begin(&broker,&memory,this,lease_epoch,GPR_LOCAL_DOMAIN)==GPR_OK);
                CHECK(gpr_transfer_complete(&broker,&memory,this,lease_epoch,GPR_TRANSFER_DONE)==GPR_OK);
                memory_remote=false;
            }break;
        case GMR_ACT_STORAGE_STOP:CHECK(disk==dirty_disk);storage=false;break;
        case GMR_ACT_RESOURCES_RELEASE:
            CHECK(!storage && !memory_remote && !dma_active);
            for(auto it=leases.rbegin();it!=leases.rend();++it)CHECK(gpr_release(&broker,&*it,this,lease_epoch)==GPR_OK);
            leases.clear();boot_request_sent=false;CHECK(gpr_retire_epoch(&broker,this,lease_epoch)==GPR_OK);peers_intact();break;
        case GMR_ACT_SUSPEND:CHECK(disk==dirty_disk);break;
        case GMR_ACT_RESUME:case GMR_ACT_SELECT_SLOT:case GMR_ACT_RF_MODE:break;
        default:std::fprintf(stderr,"unmodelled boundary action %u\n",a.kind);CHECK(false);
        }
        int result=gmr_event(&core,&e);
        if((partial_write && a.kind==GMR_ACT_STORAGE_WRITE) ||
           (partial_transfer && a.kind==GMR_ACT_FIRMWARE_BOOT))CHECK(result!=GMR_OK);
        else CHECK(result==GMR_OK);
        peers_intact();return true;
    }
    void settle() {unsigned count=0;while(step())CHECK(++count<256);}
    void start() {CHECK(gmr_start(&core)==GMR_OK);CHECK(core.state.lifecycle!=GMR_ONLINE);settle();CHECK(core.state.lifecycle==GMR_ONLINE);services();}
    void register_slots() {for(unsigned i=0;i<2;i++){issue(GMR_CMD_QUERY_SIM,(uint8_t)i);issue(GMR_CMD_QUERY_REGISTRATION,(uint8_t)i);}settle();
        CHECK(core.state.slots[0].sim==GMR_SIM_READY && core.state.slots[1].sim==GMR_SIM_READY);}
    void apple_qmux() {
        auto m=std::make_unique<gmux_context>();gmux_output output{};
        CHECK(gmux_init(m.get(),core.state.epoch,0,core.state.slots[0].generation)==GMR_OK);
        auto endpoint=source(11,0);CHECK(gmux_set_endpoint(m.get(),&endpoint)==GMR_OK);
        const uint8_t allocate[]={1,15,0,0,0,0,0,7,0x22,0,4,0,1,1,0,11};
        const uint8_t allocated[]={1,23,0,0x80,0,0,1,7,0x22,0,12,0,2,4,0,0,0,0,0,1,2,0,11,1};
        CHECK(giulia_modem_apple_abi_validate(GIULIA_MODEM_APPLE_ABP,2,2,sizeof(allocate),0,0)==0);
        CHECK(gmux_from_host(m.get(),core.state.epoch,1,allocate,sizeof(allocate),&output)==GMR_OK);
        CHECK(output.kind==(GMUX_HOST|GMUX_CLIENT_ALLOCATED));
        CHECK(output.bytes==sizeof(allocated) && !std::memcmp(output.data,allocated,sizeof(allocated)));
        uint32_t generation=output.client_generation;
        CHECK(gmux_bind_client(m.get(),11,1,generation,0x9001)==GMR_OK);
        const uint8_t request[]={1,12,0,0,11,1,0,0x34,0x12,0x2f,0,0,0};
        CHECK(gmux_from_host(m.get(),core.state.epoch,2,request,sizeof(request),&output)==GMR_OK);
        CHECK(output.kind==GMUX_PEER && output.local_port==0x9001);
        const uint8_t qrtr[]={0,1,0,0x2f,0,0,0};
        CHECK(output.bytes==sizeof(qrtr) && !std::memcmp(output.data,qrtr,sizeof(qrtr)));
        CHECK(gmux_from_host(m.get(),core.state.epoch,2,request,sizeof(request),&output)==GMR_BUSY);
        const uint8_t response[]={2,1,0,0x2f,0,7,0,2,4,0,0,0,0,0};
        const uint8_t restored[]={1,19,0,0x80,11,1,2,0x34,0x12,0x2f,0,7,0,2,4,0,0,0,0,0};
        CHECK(gmux_from_peer(m.get(),&endpoint,1,generation,0x9001,response,sizeof(response),&output)==GMR_OK);
        CHECK(output.bytes==sizeof(restored) && !std::memcmp(output.data,restored,sizeof(restored)));
        CHECK(core.state.slots[0].sim==GMR_SIM_UNKNOWN); // opaque transport never invents SIM status
        CHECK(gmux_from_host(m.get(),core.state.epoch,2,request,sizeof(request),&output)==GMR_OK && output.kind==GMUX_HOST);
        const uint8_t release[]={1,16,0,0,0,0,0,8,0x23,0,5,0,1,2,0,11,1};
        CHECK(gmux_from_host(m.get(),core.state.epoch,3,release,sizeof(release),&output)==GMR_BUSY);
        CHECK(gmux_unbind_client(m.get(),11,1,generation)==GMR_OK);
        CHECK(gmux_from_host(m.get(),core.state.epoch,3,release,sizeof(release),&output)==GMR_OK);
        CHECK(output.kind&(GMUX_CLIENT_RELEASED));
        CHECK(gmux_reset(m.get(),core.state.epoch+1,core.state.slots[0].generation+1)==GMR_OK);
        CHECK(gmux_from_peer(m.get(),&endpoint,1,generation,0x9001,response,sizeof(response),&output)==GMR_STALE);
        total_qmux+=9;
    }
    void connect(unsigned slot,unsigned bearer,unsigned family=4) {
        auto c=command(GMR_CMD_CONNECT,(uint8_t)slot,(uint8_t)bearer);c.ip_family=(uint8_t)family;
        std::strcpy(c.apn,"test.example");CHECK(gmr_submit(&core,&c)==GMR_OK);
        CHECK(core.state.bearers[bearer].state!=GMR_BEARER_UP);settle();
        CHECK(core.state.bearers[bearer].state==GMR_BEARER_UP);
    }
    void traffic(unsigned slot,unsigned bearer) {
        auto c=command(GMR_CMD_PACKET_TX,(uint8_t)slot,(uint8_t)bearer);
        bool v6=core.state.bearers[bearer].ip_family==6;
        c.bytes=v6?48:28;c.payload[0]=v6?0x60:0x45;
        if(v6){c.payload[5]=8;c.payload[6]=17;c.payload[7]=64;c.payload[8]=0x20;c.payload[9]=1;}
        else {c.payload[3]=28;c.payload[8]=64;c.payload[9]=17;c.payload[12]=10;c.payload[16]=10;}
        c.payload[c.bytes-1]=0x85;CHECK(gmr_submit(&core,&c)==GMR_OK);settle();
        struct gmr_event rx{};rx.kind=GMR_EV_DATA_RX;rx.id=source(1,slot,bearer);rx.id.opid=packet_cookie++;
        rx.bytes=c.bytes;std::copy(c.payload,c.payload+c.bytes,rx.payload);
        uint8_t wire[GMR_PAYLOAD+16];size_t wire_bytes=0,consumed=0;
        CHECK(gmap_encode(wire,sizeof(wire),rx.payload,rx.bytes,core.state.bearers[bearer].mux,5,&wire_bytes)==GMAP_OK);
        CHECK(gmd_receive(&core,&ring_binding[bearer],rx.id.opid,wire,wire_bytes,5,&consumed)==GMR_OK && consumed==wire_bytes);
        CHECK(last_packet.size()==c.bytes && last_packet.back()==0x85);
        uint8_t saved=wire[1];wire[1]=254;
        CHECK(gmd_receive(&core,&ring_binding[bearer],packet_cookie++,wire,wire_bytes,5,&consumed)==GMR_STALE && !consumed);
        wire[1]=saved;
        auto old_binding=ring_binding[bearer];old_binding.epoch--;
        CHECK(gmd_receive(&core,&old_binding,packet_cookie++,wire,wire_bytes,5,&consumed)==GMR_STALE);
        struct gmr_event delivered=rx;delivered.kind=GMR_EV_DATA_DELIVERED;delivered.bytes=0;delivered.value=rx.bytes;
        CHECK(gmr_event(&core,&delivered)==GMR_OK);
        CHECK(gmr_event(&core,&delivered)==GMR_STALE);
        CHECK(core.state.bearers[bearer].rx_bytes==core.state.bearers[bearer].rx_delivered);total_packets++;
    }
    void media() {
        issue(GMR_CMD_IMS_REGISTER);settle();CHECK(core.state.slots[0].ims==GMR_IMS_REGISTERING);
        struct gmr_event e{};e.kind=GMR_EV_IMS_STATUS;e.id=source(0x21,0);e.value=GMR_IMS_REGISTERED;
        CHECK(gmr_event(&core,&e)==GMR_OK);
        auto c=command(GMR_CMD_VOICE_START);c.bytes=1;c.payload[0]=1;CHECK(gmr_submit(&core,&c)==GMR_OK);settle();
        CHECK(core.state.slots[0].call==GMR_CALL_ACTIVE);
        c=command(GMR_CMD_SMS_SEND);c.bytes=3;c.payload[0]=1;c.payload[1]=2;c.payload[2]=3;
        CHECK(gmr_submit(&core,&c)==GMR_OK);settle();CHECK(core.state.slots[0].sms==GMR_SMS_ACCEPTED);
        uint32_t first_sms=core.state.slots[0].sms_id;
        CHECK(gmr_submit(&core,&c)==GMR_BUSY && core.state.slots[0].sms_id==first_sms);
        e={};e.kind=GMR_EV_SMS_DELIVERED;e.id=source(5,0);e.value=first_sms;
        CHECK(gmr_event(&core,&e)==GMR_OK);CHECK(core.state.slots[0].sms==GMR_SMS_DELIVERED);
        CHECK(gmr_submit(&core,&c)==GMR_OK);
        CHECK(gmr_submit(&core,&c)==GMR_BUSY);settle();
        CHECK(core.state.slots[0].sms==GMR_SMS_ACCEPTED && core.state.slots[0].sms_id!=first_sms);
        CHECK(gmr_event(&core,&e)==GMR_STALE);e.value=core.state.slots[0].sms_id;
        CHECK(gmr_event(&core,&e)==GMR_OK && core.state.slots[0].sms==GMR_SMS_DELIVERED);
    }
    void persist() {
        auto c=command(GMR_CMD_STORAGE_WRITE,GMR_NO_SLOT);c.object=1;c.offset=64;c.bytes=4;
        c.payload[0]=0x12;c.payload[1]=0x34;c.payload[2]=0x56;c.payload[3]=0x78;
        CHECK(gmr_submit(&core,&c)==GMR_OK);CHECK(step());
        CHECK(core.state.storage_dirty && disk!=dirty_disk);settle();
        CHECK(disk==dirty_disk && !core.state.storage_dirty);
    }
    void shutdown() {issue(GMR_CMD_STOP);settle();CHECK(core.state.lifecycle==GMR_OFF && leases.empty());peers_intact();}
};

static void normal_lifecycle() {
    auto s=std::make_unique<System>();
    CHECK(giulia_modem_apple_abi_validate(GIULIA_MODEM_APPLE_BASEBAND,1,1,0,0,0)==0);
    CHECK(giulia_modem_apple_abi_validate(GIULIA_MODEM_APPLE_BASEBAND,1,0,0,0,0)!=0);
    s->start();s->apple_qmux();
    uint32_t writes=s->core.state.storage_writes;
    CHECK(gmr_event(&s->core,&s->boot_request)==GMR_OK);s->settle();
    CHECK(s->core.state.storage_writes==writes && s->peer_acks==2);
    auto altered=s->boot_request;altered.payload[0]^=1;
    CHECK(gmr_event(&s->core,&altered)==GMR_PROTOCOL);
    s->register_slots();s->connect(0,0);s->connect(1,1,6);
    struct gmr_event roaming{};roaming.kind=GMR_EV_NAS_CHANGED;roaming.id=s->source(3,0);
    roaming.value=GMR_REG_REGISTERED;roaming.flags=7;
    CHECK(gmr_event(&s->core,&roaming)==GMR_OK);
    CHECK(s->core.state.slots[0].roaming && s->core.state.slots[0].roaming_known);
    s->traffic(0,0);roaming.flags=5;CHECK(gmr_event(&s->core,&roaming)==GMR_OK);
    CHECK(!s->core.state.slots[0].roaming && s->core.state.slots[0].roaming_known);
    roaming.flags=13;CHECK(gmr_event(&s->core,&roaming)==GMR_PROTOCOL);
    s->traffic(0,0);s->traffic(1,1);s->media();s->persist();
    s->issue(GMR_CMD_SUSPEND);s->settle();CHECK(s->core.state.lifecycle==GMR_SUSPENDED);
    auto c=s->command(GMR_CMD_PACKET_TX,0,0);CHECK(gmr_submit(&s->core,&c)==GMR_STATE);
    s->issue(GMR_CMD_RESUME);s->settle();s->traffic(0,0);
    s->issue(GMR_CMD_VOICE_STOP);s->settle();CHECK(s->core.state.slots[0].call==GMR_CALL_IDLE);
    s->issue(GMR_CMD_DISCONNECT,0,0);s->issue(GMR_CMD_DISCONNECT,1,1);s->settle();
    s->issue(GMR_CMD_SELECT_SLOT,1);s->settle();CHECK(s->core.state.selected_slot==1);
    c=s->command(GMR_CMD_RF_MODE,1);c.object=0;CHECK(gmr_submit(&s->core,&c)==GMR_OK);s->settle();
    CHECK(s->core.state.slots[1].registration==GMR_REG_OFFLINE);
    c.object=1;CHECK(gmr_submit(&s->core,&c)==GMR_OK);s->settle();s->register_slots();s->connect(1,2);s->traffic(1,2);
    s->shutdown();total_scenarios++;
}

static void recovery_with_old_completions() {
    auto s=std::make_unique<System>();s->start();s->register_slots();s->connect(0,0);s->media();s->persist();
    s->issue(GMR_CMD_QUERY_REGISTRATION);CHECK(!s->work.empty());auto old=s->work.front();
    auto old_reply=s->reply(old);uint64_t epoch=s->core.state.epoch;
    struct gmr_event fatal{};fatal.kind=GMR_EV_FATAL;fatal.id.epoch=epoch;fatal.status=GMR_IO;
    CHECK(gmr_event(&s->core,&fatal)==GMR_OK);
    CHECK(s->core.state.epoch!=epoch);CHECK(gmr_event(&s->core,&old_reply)==GMR_STALE);
    CHECK(s->core.state.last_fault==GMR_IO && s->core.state.fault_epoch==epoch && s->core.state.fault_count==1);
    s->settle();CHECK(s->core.state.lifecycle==GMR_ONLINE);s->services();s->register_slots();s->connect(0,0);s->traffic(0,0);
    CHECK(s->disk==s->dirty_disk);
    CHECK(gmr_event(&s->core,&fatal)==GMR_STALE && s->core.state.fault_count==1);
    s->issue(GMR_CMD_SSR);
    CHECK(s->core.state.lifecycle==GMR_STOPPING && s->core.state.restart_requested);
    epoch=s->core.state.epoch;s->issue(GMR_CMD_STOP);
    CHECK(!s->core.state.restart_requested);s->settle();
    CHECK(s->core.state.lifecycle==GMR_OFF && s->core.state.epoch==epoch && s->leases.empty());
    CHECK(!s->core.state.pending_count && s->work.empty());s->peers_intact();total_scenarios++;
}

static void coherent_failure_scenarios() {
    const unsigned faults[]={GMR_ACT_FIRMWARE_BOOT,GMR_ACT_IPC_START,GMR_ACT_IPA_PREPARE,
        GMR_ACT_WDA_FORMAT,GMR_ACT_WDS_BIND,GMR_ACT_DMA_CONFIGURE,GMR_ACT_NETWORK_CONFIGURE,
        GMR_ACT_STORAGE_FLUSH,GMR_ACT_DATA_DRAIN,GMR_ACT_MEMORY_RECLAIM};
    for(unsigned kind:faults) {
        auto s=std::make_unique<System>();
        if(kind==GMR_ACT_FIRMWARE_BOOT || kind==GMR_ACT_IPC_START) {
            s->fault_kind=kind;CHECK(gmr_start(&s->core)==GMR_OK);s->settle();
            CHECK(s->core.state.lifecycle==GMR_ISOLATED && !s->leases.empty());
        } else {
            s->start();s->register_slots();
            if(kind>=GMR_ACT_IPA_PREPARE && kind<=GMR_ACT_NETWORK_CONFIGURE) {
                s->fault_kind=kind;auto c=s->command(GMR_CMD_CONNECT,0,0);std::strcpy(c.apn,"test.example");
                CHECK(gmr_submit(&s->core,&c)==GMR_OK);s->settle();CHECK(s->core.state.bearers[0].state!=GMR_BEARER_UP);
            } else {
                s->connect(0,0);s->traffic(0,0);s->media();s->fault_kind=kind;
                if(kind==GMR_ACT_STORAGE_FLUSH) {
                    auto c=s->command(GMR_CMD_STORAGE_WRITE,GMR_NO_SLOT);c.object=1;c.bytes=1;c.payload[0]=1;
                    CHECK(gmr_submit(&s->core,&c)==GMR_OK);s->settle();CHECK(s->disk!=s->dirty_disk);
                } else {s->issue(GMR_CMD_STOP);s->settle();}
                CHECK(s->core.state.lifecycle==GMR_ISOLATED && !s->leases.empty());
            }
        }
        s->peers_intact();total_scenarios++;
    }
    auto s=std::make_unique<System>();s->start();s->register_slots();s->connect(0,0);s->media();
    s->fault_kind=GMR_ACT_DATA_TX;s->drop_fault=true;
    auto c=s->command(GMR_CMD_PACKET_TX,0,0);c.bytes=20;c.payload[0]=0x45;c.payload[3]=20;
    CHECK(gmr_submit(&s->core,&c)==GMR_OK);s->settle();
    CHECK(gmr_tick(&s->core,GMR_TIMEOUT_MS+10)==GMR_TIMEOUT);
    CHECK(s->core.state.bearers[0].state!=GMR_BEARER_UP);
    auto sms=s->command(GMR_CMD_SMS_SEND);sms.bytes=1;sms.payload[0]=1;
    CHECK(gmr_submit(&s->core,&sms)==GMR_OK);
    uint64_t sms_operation=s->core.state.last_opid;uint32_t old_sms_id=s->core.state.slots[0].sms_id;
    CHECK(gmr_cancel(&s->core,sms_operation)==GMR_OK && s->core.state.slots[0].sms==GMR_SMS_UNCERTAIN);
    CHECK(gmr_submit(&s->core,&sms)==GMR_BUSY && s->core.state.slots[0].sms_id==old_sms_id);
    s->peers_intact();total_scenarios++;
}

static void subscription_service_and_queue_recovery() {
    auto s=std::make_unique<System>();s->start();s->register_slots();s->connect(0,0);s->connect(1,1,6);s->media();
    auto old=s->source(1,0,0);s->card_present[0]=false;
    struct gmr_event removed{};removed.kind=GMR_EV_SIM_REMOVED;removed.id=s->source(11,0);
    CHECK(gmr_event(&s->core,&removed)==GMR_OK);s->settle();s->services();
    s->issue(GMR_CMD_QUERY_SIM,0);s->issue(GMR_CMD_QUERY_REGISTRATION,0);
    s->issue(GMR_CMD_QUERY_SIM,1);s->issue(GMR_CMD_QUERY_REGISTRATION,1);s->settle();
    CHECK(s->core.state.slots[0].sim==GMR_SIM_ABSENT && s->core.state.slots[1].sim==GMR_SIM_READY);
    auto c=s->command(GMR_CMD_CONNECT,0,0);std::strcpy(c.apn,"test.example");
    CHECK(gmr_submit(&s->core,&c)==GMR_STATE);s->connect(1,1,6);s->traffic(1,1);
    struct gmr_event lost{};lost.kind=GMR_EV_SERVICE_DOWN;lost.id=s->source(1,1);
    CHECK(gmr_event(&s->core,&lost)==GMR_OK);s->settle();s->services();
    CHECK(old.epoch!=s->core.state.epoch);s->card_present[0]=true;s->register_slots();s->connect(0,0);
    // Exhaust the actual bounded RX-receipt queue while the interface is slow.
    std::vector<struct gmr_event> receipts;
    uint8_t packet[20]={0x45,0,0,20};uint8_t frame[32];size_t bytes=0,consumed=0;
    CHECK(gmap_encode(frame,sizeof(frame),packet,sizeof(packet),1,5,&bytes)==GMAP_OK);
    for(unsigned i=0;i<GMR_EVENTS;i++) {
        struct gmr_event delivered{};delivered.kind=GMR_EV_DATA_DELIVERED;
        delivered.id=s->source(1,0,0);delivered.id.opid=s->packet_cookie++;delivered.value=20;
        CHECK(gmd_receive(&s->core,&s->ring_binding[0],delivered.id.opid,frame,bytes,5,&consumed)==GMR_OK);
        receipts.push_back(delivered);
    }
    auto binding=s->ring_binding[0];
    CHECK(gmd_receive(&s->core,&binding,s->packet_cookie++,frame,bytes,5,&consumed)==GMR_FULL && !consumed);
    c=s->command(GMR_CMD_SUSPEND);CHECK(gmr_submit(&s->core,&c)==GMR_BUSY);
    for(const auto& receipt:receipts)CHECK(gmr_event(&s->core,&receipt)==GMR_OK);
    s->issue(GMR_CMD_SUSPEND);s->settle();s->issue(GMR_CMD_RESUME);s->settle();s->traffic(0,0);
    s->shutdown();total_scenarios++;
}

static void reordered_queries_and_ambiguous_transfer() {
    auto s=std::make_unique<System>();s->start();
    for(unsigned slot=0;slot<2;slot++){s->issue(GMR_CMD_QUERY_SIM,(uint8_t)slot);s->issue(GMR_CMD_QUERY_REGISTRATION,(uint8_t)slot);}
    CHECK(s->work.size()==4);std::reverse(s->work.begin(),s->work.end());
    auto pending=s->work.front();auto reply=s->reply(pending);auto malformed=reply;
    malformed.kind=GMR_EV_COMPLETE;CHECK(gmr_event(&s->core,&malformed)==GMR_PROTOCOL);
    CHECK(s->core.state.pending_count==4);
    malformed=reply;malformed.id.port++;CHECK(gmr_event(&s->core,&malformed)==GMR_STALE);
    s->settle();CHECK(gmr_event(&s->core,&reply)==GMR_STALE);
    s->issue(GMR_CMD_QUERY_REGISTRATION);pending=s->work.front();reply=s->reply(pending);
    CHECK(gmr_cancel(&s->core,pending.id.opid)==GMR_OK);CHECK(gmr_event(&s->core,&reply)==GMR_STALE);
    s->settle();s->connect(0,0);s->traffic(0,0);s->shutdown();total_scenarios++;

    auto uncertain=std::make_unique<System>();uncertain->partial_transfer=true;
    CHECK(gmr_start(&uncertain->core)==GMR_OK);uncertain->settle();
    CHECK(uncertain->core.state.lifecycle==GMR_ISOLATED);
    CHECK(gpr_release(&uncertain->broker,&uncertain->memory,uncertain.get(),uncertain->lease_epoch)==GPR_QUARANTINED);
    CHECK(gpr_open_epoch(&uncertain->broker,uncertain.get(),uncertain->lease_epoch+1)==GPR_BUSY);
    CHECK(gmr_start(&uncertain->core)!=GMR_OK);uncertain->peers_intact();total_scenarios++;
}

static void reconnect_retains_dma_incarnation() {
    auto s=std::make_unique<System>();s->start();s->register_slots();s->connect(0,0);
    const auto old_binding=s->ring_binding[0];const uint64_t epoch=s->core.state.epoch;
    auto tx=s->command(GMR_CMD_PACKET_TX,0,0);tx.bytes=20;tx.payload[0]=0x45;tx.payload[3]=20;
    uint8_t wire[64];size_t bytes=0,consumed=0;
    CHECK(gmap_encode(wire,sizeof(wire),tx.payload,tx.bytes,1,5,&bytes)==GMAP_OK);
    struct gmr_event delivered{};delivered.kind=GMR_EV_DATA_DELIVERED;
    delivered.id=s->source(1,0,0);delivered.id.opid=s->packet_cookie++;delivered.value=tx.bytes;
    CHECK(gmd_receive(&s->core,&old_binding,delivered.id.opid,wire,bytes,5,&consumed)==GMR_OK);
    CHECK(gmr_submit(&s->core,&tx)==GMR_OK);
    auto submitted=s->work.front();s->work.pop_front();
    uint8_t transmitted[64];size_t transmitted_bytes=0;
    CHECK(gmd_transmit(&s->core.state,&submitted,5,transmitted,sizeof(transmitted),&transmitted_bytes)==GMR_OK);
    // One TX has crossed the provider boundary; another is still locally queued.
    CHECK(gmr_submit(&s->core,&tx)==GMR_OK);auto queued=s->work.front();
    s->issue(GMR_CMD_DISCONNECT,0,0);
    CHECK(!s->live(submitted) && !s->live(queued));
    for(const auto& receipt:s->core.rx_pending)CHECK(!receipt.used);
    auto ims=s->command(GMR_CMD_IMS_REGISTER);CHECK(gmr_submit(&s->core,&ims)==GMR_STATE);
    auto connect=s->command(GMR_CMD_CONNECT,0,0);std::strcpy(connect.apn,"test.example");
    CHECK(gmr_submit(&s->core,&connect)==GMR_STATE);
    auto remove=s->work.back();CHECK(remove.kind==GMR_ACT_DATA_REMOVE);
    struct gmr_event incomplete{};incomplete.kind=GMR_EV_COMPLETE;
    incomplete.id=remove.id;incomplete.action_kind=remove.kind;
    incomplete.value=GMR_REMOVE_NETWORK|GMR_REMOVE_RX_DRAINED;
    CHECK(gmr_event(&s->core,&incomplete)==GMR_PROTOCOL);
    CHECK(s->dma_active && s->mapped[0] && s->core.state.bearers[0].state==GMR_BEARER_UNCERTAIN);
    CHECK(gmr_submit(&s->core,&connect)==GMR_STATE);
    s->issue(GMR_CMD_DISCONNECT,0,0);s->settle();
    CHECK(!s->dma_active && !s->mapped[0] && s->core.state.bearers[0].state==GMR_BEARER_DOWN);
    CHECK(gmr_submit(&s->core,&connect)==GMR_OK);
    CHECK(gmr_submit(&s->core,&ims)==GMR_STATE);s->settle();
    CHECK(s->core.state.bearers[0].state==GMR_BEARER_UP && s->core.state.epoch==epoch);
    CHECK(s->ring_binding[0].bearer_generation>old_binding.bearer_generation);
    CHECK(s->ring_binding[0].endpoint_generation==old_binding.endpoint_generation);
    struct gmr_event old_completion{};old_completion.kind=GMR_EV_DATA_COMPLETION;
    old_completion.action_kind=submitted.kind;old_completion.id=submitted.id;old_completion.value=submitted.bytes;
    CHECK(gmr_event(&s->core,&old_completion)==GMR_STALE);
    old_completion.id=queued.id;CHECK(gmr_event(&s->core,&old_completion)==GMR_STALE);
    CHECK(gmr_event(&s->core,&delivered)==GMR_STALE);
    unsigned packets=s->received_packets;
    CHECK(gmd_receive(&s->core,&old_binding,s->packet_cookie++,wire,bytes,5,&consumed)==GMR_STALE && !consumed);
    CHECK(s->received_packets==packets && !s->core.state.bearers[0].tx_bytes && !s->core.state.bearers[0].rx_delivered);
    CHECK(gmd_transmit(&s->core.state,&submitted,5,transmitted,sizeof(transmitted),&transmitted_bytes)==GMR_STALE);
    s->traffic(0,0);s->issue(GMR_CMD_SUSPEND);s->settle();
    s->issue(GMR_CMD_RESUME);s->settle();s->traffic(0,0);s->shutdown();total_scenarios++;
}

extern "C" void modem_service_scenario(void);
int main() {
    normal_lifecycle();recovery_with_old_completions();coherent_failure_scenarios();
    subscription_service_and_queue_recovery();reordered_queries_and_ambiguous_transfer();reconnect_retains_dma_incarnation();
    modem_service_scenario();total_scenarios++;
    std::printf("{\"schema\":1,\"backend\":\"deterministic-hardware-and-peer\",\"scenarios\":%llu,\"actions\":%llu,\"ip_packets\":%llu,\"qmux_exchanges\":%llu,\"action_mask\":%llu,\"hardware_validated\":false}\n",
        (unsigned long long)total_scenarios,(unsigned long long)total_actions,
        (unsigned long long)total_packets,(unsigned long long)total_qmux,(unsigned long long)action_mask);
}
