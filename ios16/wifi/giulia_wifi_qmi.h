/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * WCN7850 WLANFW over QRTR/IPCR. Protocol provenance: Linux v6.12
 * net/qrtr/{af_qrtr,ns}.c, ath12k/qmi.{c,h}; OnePlus SM8650 cnss2/qmi.c
 * pinned in hardware/sm8650-wifi-firmware.json. Single serialized caller.
 * No memory supplied by the device is mapped. All advertised DMA allocations
 * remain owned until MHI has stopped and PCIe transactions have drained.
 */
#ifndef GIULIA_WIFI_QMI_H
#define GIULIA_WIFI_QMI_H
#include <stdint.h>
#include <stddef.h>
#include "giulia_wifi_protocol.h"
#define GWQ_PACKET 8192u
#define GWQ_CTRL 0xfffffffeu
#define GWQ_SEGMENTS 52u
#define GWQ_DEVICE_MEMORY_REGIONS 4u
#define GWQ_TIMEOUT 10000u /* One QMI RPC or service discovery, milliseconds. */
/* CNSS mission boot watchdog: default QMI timeout + 30000 ms, distinct from
 * each RPC's deadline. Start once MHI channels are usable, before discovery. */
#define GWQ_MISSION_TIMEOUT 40000u
/* OnePlus WLANFW cnss_feature_v01 and Linux WCN7850 host capability:
 * no QDSS configuration file is supplied by this transport. This is a u64
 * FEATURE_LIST bit, separate from the PC-specific PERST policy (bit 4). */
#define GWQ_HOST_FEATURE_QDSS_CFG_MISS (UINT64_C(1)<<3)
/* cnss_get_pinctrl sets WLAN_EN_SUPPORT for this board's wlan-en-gpio=16. */
#define GWQ_HOST_FEATURE_WLAN_EN (UINT64_C(1)<<2)
#define GWQ_HOST_FEATURES (GWQ_HOST_FEATURE_WLAN_EN|GWQ_HOST_FEATURE_QDSS_CFG_MISS)
#define GWQ_HOST_DDR_RANGES 3u
enum gwq_error { GWQ_OK,GWQ_ARGUMENT,GWQ_WIRE,GWQ_TRANSPORT,GWQ_TIMEOUT_ERROR,
    GWQ_SERVER_LOST,GWQ_REMOTE_ERROR,GWQ_MEMORY,GWQ_IDENTITY,GWQ_ASSET,GWQ_STATE,
    GWQ_PLATFORM_ERROR,GWQ_ALREADY_REGISTERED };
enum gwq_boot_step { GWQ_BOOT_NONE,GWQ_BOOT_DISCOVERY,GWQ_BOOT_REGISTER,
    GWQ_BOOT_HOST_CAP,GWQ_BOOT_MEMORY,GWQ_BOOT_CAP,GWQ_BOOT_PMU,
    GWQ_BOOT_REGDB,GWQ_BOOT_BDF,GWQ_BOOT_M3,GWQ_BOOT_WAIT_INIT,
    GWQ_BOOT_READY,GWQ_BOOT_CONFIG,GWQ_BOOT_MODE,GWQ_BOOT_CONFIGURED };
static inline const char *gwq_boot_name(unsigned step) {
    static const char *const names[]={"none","discovery","register","host-cap",
        "memory","cap","pmu","regdb","bdf","m3","wait-fw-init-done",
        "ready","wlan-config","wlan-mode","configured"};
    return step<sizeof(names)/sizeof(names[0])?names[step]:"invalid";
}
enum gwq_timeout_phase { GWQ_TIMEOUT_NONE,GWQ_TIMEOUT_RPC,GWQ_TIMEOUT_SERVER,GWQ_TIMEOUT_MISSION };
enum gwq_asset { GWQ_REGDB,GWQ_BOARD,GWQ_PHY };
/* OnePlus WLANFW wlfw_mem_type_enum_v01. CNSS allocates every requested
 * segment; the PC ath12k policy of declining BDF/DPD/etc. does not describe
 * this phone's vendor firmware contract. Secure carveouts remain rejected. */
enum gwq_memory_type { GWQ_MEM_MSA,GWQ_MEM_DDR,GWQ_MEM_BDF,GWQ_MEM_M3,
    GWQ_MEM_CAL,GWQ_MEM_DPD,GWQ_MEM_QDSS,GWQ_MEM_HANG,GWQ_MEM_MLO,
    GWQ_MEM_PAGEABLE,GWQ_MEM_AFC,GWQ_MEM_LPASS,GWQ_MEM_CALDB,GWQ_MEM_TYPES };
struct gwq_pmu_param;
struct gwq_io {
    void *context;
    int (*send)(void *,const void *,size_t);
    int (*pump)(void *,unsigned); /* poll MHI and wait at most this many us */
    int (*allocate)(void *,size_t,uint64_t *,void **);
    void (*clean)(void *,const void *,size_t);
    int (*asset)(void *,unsigned,unsigned,uint32_t,uint32_t,const void **,size_t *);
    uint64_t dma_base,dma_bytes;
    uint64_t (*now)(void *); /* Monotonic milliseconds, including time in pump. */
    int (*configure_pmu)(void *,const struct gwq_pmu_param *,unsigned);
    /* Complete active SMMU aperture, distinct from the QMI allocation pool
     * above: MHI/CE rings and later data-path buffers must also be covered. */
    uint64_t host_iova_base,host_iova_bytes;
    gwpr_select_fn protocol;
};
struct gwq_span { uint64_t address; uint32_t bytes,type; };
struct gwq_device_memory {uint64_t start,bytes;};
struct gwq_pmu_param {
    char pin_name[33];
    uint32_t wake_valid,wake_mv,sleep_valid,sleep_mv;
};
struct gwq_state {
    unsigned boot_step,registration_status_present;
    uint64_t registration_status;
    unsigned pin_indications,pin_valid;
    uint32_t pin_result[3]; /* Power, PHY IO, RF; firmware bitmaps, not booleans. */
    unsigned error,major,hello,server,waiting,replied,registered,host_cap;
    unsigned mem_pending,mem_ready,fw_ready,provisioned,configured,mem_rounds;
    unsigned mem_response_sent,allocations;
    uint32_t mem_request_count,mem_failure_index,mem_failure_type;
    uint64_t mem_requested_bytes;
    uint32_t node,port,chip,board,soc,firmware,remote_error,last_message;
    uint32_t cap_response_bytes,chip_info_present,chip_info_bytes,chip_family;
    uint32_t board_info_present,board_info_bytes,board_default;
    unsigned dev_mem_present,dev_mem_valid;
    struct gwq_device_memory dev_mem[GWQ_DEVICE_MEMORY_REGIONS];
    uint32_t last_indication,indication_count,unknown_indications,last_unknown_indication;
    uint64_t indication_mask;
    uint32_t pmu_present,pmu_bytes,pmu_count,pmu_valid,pmu_bad_entry;
    struct gwq_pmu_param pmu[16];
    unsigned initializing,timeout_phase;
    uint64_t mission_started_ms,elapsed_ms,host_cap_ms,memory_ready_ms,cap_ms,m3_ms,fw_ready_ms;
    uint64_t rpc_started_ms,rpc_elapsed_ms,server_received_ms;
    unsigned pmu_configured;
    int platform_error;
    uint64_t pmu_config_ms;
    uint32_t filtered_data_packets,filtered_src_node,filtered_src_port,filtered_dst_node,filtered_dst_port;
    uint16_t transaction,pending_transaction,pending_message;
    size_t response_bytes,mem_bytes;
    struct gwq_span allocated[GWQ_SEGMENTS+4];
    uint8_t response[2048],mem_request[1824],request[7000],wire[GWQ_PACKET];
};
/* These wire pointers may name uncached DMA buffers with unaligned TLVs. */
static inline uint16_t gwq_u16(const void *v) { const volatile uint8_t *p=(const volatile uint8_t *)v;return (uint16_t)(p[0]|(unsigned)p[1]<<8); }
static inline uint32_t gwq_u32(const void *v) { const volatile uint8_t *p=(const volatile uint8_t *)v;return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static inline uint64_t gwq_u64(const void *v) { const uint8_t *p=(const uint8_t *)v;return gwq_u32(p)|(uint64_t)gwq_u32(p+4)<<32; }
static inline void gwq_w16(void *v,uint16_t n) { volatile uint8_t *p=(volatile uint8_t *)v;p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8); }
static inline void gwq_w32(void *v,uint32_t n) { volatile uint8_t *p=(volatile uint8_t *)v;for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(i*8)); }
static inline void gwq_w64(void *v,uint64_t n) { uint8_t *p=(uint8_t *)v;gwq_w32(p,(uint32_t)n);gwq_w32(p+4,(uint32_t)(n>>32)); }
static inline void gwq_copy(void *v,const void *s,size_t n) { volatile uint8_t *d=(volatile uint8_t *)v;const volatile uint8_t *p=(const volatile uint8_t *)s;for(size_t i=0;i<n;i++)d[i]=p[i]; }
static inline int gwq_fail(struct gwq_state *s,unsigned e) { if(!s->error)s->error=e;return (int)s->error; }
static inline int gwq_mission_at(struct gwq_state *s,uint64_t now) {
    if(!s->initializing)return 0;
    s->elapsed_ms=now-s->mission_started_ms;
    /* A poll batch can contain more packets after the in-time final ready
     * indication. Once the handshake completed, later traffic cannot turn it
     * into a mission timeout; normal wire/state validation still applies. */
    if(s->provisioned && s->fw_ready)return 0;
    if(s->elapsed_ms<GWQ_MISSION_TIMEOUT)return 0;
    s->timeout_phase=GWQ_TIMEOUT_MISSION;return gwq_fail(s,GWQ_TIMEOUT_ERROR);
}
static inline int gwq_mission_check(struct gwq_state *s,const struct gwq_io *io)
{return gwq_mission_at(s,io->now(io->context));}
static inline int gwq_rpc_check(struct gwq_state *s,const struct gwq_io *io) {
    uint64_t now=io->now(io->context);s->rpc_elapsed_ms=now-s->rpc_started_ms;
    if(s->rpc_elapsed_ms>=GWQ_TIMEOUT) {
        s->timeout_phase=GWQ_TIMEOUT_RPC;return gwq_fail(s,GWQ_TIMEOUT_ERROR);
    }
    return gwq_mission_at(s,now);
}
static inline uint64_t gwq_reply_ms(const struct gwq_state *s)
{return s->rpc_started_ms+s->rpc_elapsed_ms-s->mission_started_ms;}
/* Validate complete TLV framing and uniqueness before interpreting any field. */
static inline int gwq_tlvs(const uint8_t *p,size_t n) {
    return gwpr_status(gwpr_baseline(GWPR_QMI,0,0,p,n))==GWPR_VALID;
}
static inline const uint8_t *gwq_field(const uint8_t *p,size_t n,unsigned t,size_t *bytes) {
    size_t o=0;*bytes=0;
    while(o+3<=n) { size_t z=gwq_u16(p+o+1);if(z>n-o-3)return NULL;
        if(p[o]==t){*bytes=z;return p+o+3;}o+=3+z; }
    return NULL;
}
static inline int gwq_tlv(uint8_t *p,size_t cap,size_t *n,unsigned t,const void *v,size_t z) {
    if(*n>cap || z>65535 || cap-*n<3 || z>cap-*n-3 || (z&&!v))return 0;
    p[*n]=(uint8_t)t;gwq_w16(p+*n+1,(uint16_t)z);if(z)gwq_copy(p+*n+3,v,z);*n+=z+3;return 1;
}
static inline void gwq_num(uint8_t *p,size_t *n,unsigned t,uint64_t v,unsigned z) {
    uint8_t b[8];gwq_w64(b,v);(void)gwq_tlv(p,7000,n,t,b,z);
}
static inline int gwq_control(struct gwq_state *s,const struct gwq_io *io,unsigned command,
    uint32_t sn,uint32_t sp,uint32_t dn,uint32_t dp,const uint32_t body[5]) {
    uint8_t b[52]={0};gwq_w32(b,1);gwq_w32(b+4,command);gwq_w32(b+8,sn);gwq_w32(b+12,sp);
    gwq_w32(b+20,20);gwq_w32(b+24,dn);gwq_w32(b+28,dp);
    for(unsigned i=0;i<5;i++)gwq_w32(b+32+4*i,body[i]);
    if(io->send(io->context,b,sizeof(b)))return gwq_fail(s,GWQ_TRANSPORT);return 0;
}
/* Called only from MHI's validated IPCR receive callback. Unrelated services
 * are ignored; malformed packets for our server stop firmware initialization. */
static inline int gwq_receive(struct gwq_state *s,const struct gwq_io *io,const void *v,size_t n) {
    const uint8_t *p=(const uint8_t *)v;size_t h,z;uint32_t type,sn,sp,dn,dp,confirm;
    if(!s || !io || !io->send || !io->now || !p || n<16 || n>GWQ_PACKET)return GWQ_ARGUMENT;
    if(s->error)return (int)s->error;
    if(p[0]==1) {
        if(n<32 || gwq_u32(p)!=1)return gwq_fail(s,GWQ_WIRE);
        h=32;type=gwq_u32(p+4);sn=gwq_u32(p+8);sp=gwq_u32(p+12);confirm=gwq_u32(p+16);
        z=gwq_u32(p+20);dn=gwq_u32(p+24);dp=gwq_u32(p+28);
    } else if(p[0]==3) {
        h=16u+p[3];type=p[1];confirm=p[2]&1;z=gwq_u32(p+4);sn=gwq_u16(p+8);sp=gwq_u16(p+10);
        dn=gwq_u16(p+12);dp=gwq_u16(p+14);
        if(sp==0xfffe)sp=GWQ_CTRL;if(dp>=0xfffe)dp=GWQ_CTRL;
    } else return gwq_fail(s,GWQ_WIRE);
    if(h>n || !z || z>n-h || ((z+3)&~(size_t)3)!=n-h || confirm>1)return gwq_fail(s,GWQ_WIRE);
    p+=h;
    if(type!=1) {
        if(z!=20 || gwq_u32(p)!=type)return gwq_fail(s,GWQ_WIRE);
        if(type==2 && !s->hello) { uint32_t b[5]={2,0,0,0,0};s->hello=1;
            return gwq_control(s,io,2,1,GWQ_CTRL,sn,GWQ_CTRL,b); }
        if(type==4 && gwq_u32(p+4)==0x45 && gwq_u32(p+8)==0x101) {
            uint32_t node=gwq_u32(p+12),port=gwq_u32(p+16);
            if(!node || !port || port>=GWQ_CTRL || (s->server && (s->node!=node || s->port!=port)))return gwq_fail(s,GWQ_STATE);
            if(!s->server)s->server_received_ms=io->now(io->context);
            s->node=node;s->port=port;s->server=1;
        } else if(s->server && ((type==3 && sn==s->node) ||
            (type==5 && gwq_u32(p+12)==s->node && gwq_u32(p+16)==s->port)))return gwq_fail(s,GWQ_SERVER_LOST);
        return 0;
    }
    if(confirm) { uint32_t b[5]={7,dn,dp,0,0};
        if(gwq_control(s,io,7,dn,dp,sn,sp,b))return (int)s->error; }
    if(!s->server || sn!=s->node || sp!=s->port || dn!=1 || dp!=0x4000) {
        s->filtered_data_packets++;s->filtered_src_node=sn;s->filtered_src_port=sp;
        s->filtered_dst_node=dn;s->filtered_dst_port=dp;return 0;
    }
    if(z<7 || gwq_u16(p+5)!=z-7 || !gwq_tlvs(p+7,z-7))return gwq_fail(s,GWQ_WIRE);
    uint16_t txn=gwq_u16(p+1),msg=gwq_u16(p+3);unsigned flag=p[0];p+=7;z-=7;
    if(flag==2) {
        uint64_t verdict=io->protocol?io->protocol(io->context,GWPR_QMI,msg,flag,p,z):
            gwpr_baseline(GWPR_QMI,msg,flag,p,z);
        if(gwpr_status(verdict)!=GWPR_VALID)return gwq_fail(s,GWQ_WIRE);
        if(!s->waiting || txn!=s->pending_transaction || msg!=s->pending_message)return 0;
        if(gwq_rpc_check(s,io))return (int)s->error;
        size_t bytes;const uint8_t *r=gwq_field(p,z,2,&bytes);
        if(!r || bytes!=4 || z>sizeof(s->response) || s->replied)return gwq_fail(s,GWQ_WIRE);
        s->remote_error=(uint32_t)gwq_u16(r)<<16|gwq_u16(r+2);
        if(s->remote_error)return gwq_fail(s,GWQ_REMOTE_ERROR);
        gwq_copy(s->response,p,z);s->response_bytes=z;s->replied=1;
    } else if(flag==4) {
        s->last_indication=msg;s->indication_count++;
        if(msg<64)s->indication_mask|=1ull<<msg;
        uint64_t now=io->now(io->context),arrived=now-s->mission_started_ms;
        if(msg==0x37)s->memory_ready_ms=arrived;
        /* Record a late indication, but never promote it to readiness after
         * the mission deadline. The packet's arrival time is authoritative. */
        if(gwq_mission_at(s,now))return (int)s->error;
        uint64_t verdict=io->protocol?io->protocol(io->context,GWPR_QMI,msg,flag,p,z):
            gwpr_baseline(GWPR_QMI,msg,flag,p,z);
        if(gwpr_status(verdict)!=GWPR_VALID)return gwq_fail(s,GWQ_WIRE);
        if(msg==0x35) {
            if(!s->host_cap || s->mem_pending || s->provisioned || z>sizeof(s->mem_request))return gwq_fail(s,GWQ_STATE);
            gwq_copy(s->mem_request,p,z);s->mem_bytes=z;s->mem_pending=1;
        } else if(msg==0x37) {
            if(!s->mem_response_sent || s->mem_pending)return gwq_fail(s,GWQ_STATE);s->mem_ready=1;
        } else if(msg==0x38) {
            /* The pinned vendor decoder validates these optional scalar
             * fields before invoking FW_INIT_DONE. Framed TLVs alone do not
             * establish that a known field contains its complete value. */
            static const unsigned width[3]={4,2,8};
            for(unsigned i=0;i<3;i++) {
                size_t bytes;const uint8_t *value=gwq_field(p,z,0x10+i,&bytes);
                if(value && bytes!=width[i])return gwq_fail(s,GWQ_WIRE);
            }
            if(!s->mem_ready)return gwq_fail(s,GWQ_STATE);
            if(!s->fw_ready)s->fw_ready_ms=arrived;
            s->fw_ready=1;
        } else if(msg==0x2c) {
            /* Vendor PIN_CONNECT_RESULT: optional u32 TLVs 0x10..0x12.
             * Validate the entire indication before updating diagnostics.
             * Pin results never substitute for FW_INIT_DONE. */
            unsigned valid=0;uint32_t result[3]={0};
            for(unsigned i=0;i<3;i++) {
                size_t bytes;const uint8_t *value=gwq_field(p,z,0x10+i,&bytes);
                if(!value)continue;
                if(bytes!=4)return gwq_fail(s,GWQ_WIRE);
                valid|=1u<<i;result[i]=gwq_u32(value);
            }
            for(unsigned i=0;i<3;i++)if(valid&(1u<<i))s->pin_result[i]=result[i];
            s->pin_valid|=valid;s->pin_indications++;
        } else {
            /* In the vendor HST/HSP path only FW_INIT_DONE (0x38) starts the
             * WLAN driver. Legacy FW_READY (0x21) denotes calibration, so keep
             * unhandled indications observable without promoting readiness. */
            s->last_unknown_indication=msg;s->unknown_indications++;
        }
    } else return gwq_fail(s,GWQ_WIRE);
    return 0;
}
static inline int gwq_request(struct gwq_state *s,const struct gwq_io *io,uint16_t msg,size_t n) {
    if(s->error)return (int)s->error;
    if(!s->server || s->waiting || n>sizeof(s->request) || !io->pump || !io->now)return gwq_fail(s,GWQ_STATE);
    if(gwq_mission_check(s,io))return (int)s->error;
    if(!gwq_tlvs(s->request,n))return gwq_fail(s,GWQ_ARGUMENT);
    uint8_t *b=s->wire;size_t total=(39+n+3)&~(size_t)3;
    for(size_t i=0;i<total;i++)b[i]=0;
    gwq_w32(b,1);gwq_w32(b+4,1);gwq_w32(b+8,1);gwq_w32(b+12,0x4000);
    gwq_w32(b+20,(uint32_t)(7+n));gwq_w32(b+24,s->node);gwq_w32(b+28,s->port);
    if(!++s->transaction)++s->transaction;
    s->pending_transaction=s->transaction;s->pending_message=msg;s->last_message=msg;s->waiting=1;s->replied=0;
    gwq_w16(b+33,s->transaction);gwq_w16(b+35,msg);gwq_w16(b+37,(uint16_t)n);gwq_copy(b+39,s->request,n);
    s->rpc_started_ms=io->now(io->context);s->rpc_elapsed_ms=0;
    if(io->send(io->context,b,total))gwq_fail(s,GWQ_TRANSPORT);
    while(!s->error && !s->replied) {
        if(gwq_rpc_check(s,io))break;
        if(io->pump(io->context,1000))gwq_fail(s,GWQ_TRANSPORT);
    }
    s->waiting=0;
    return (int)s->error;
}
static inline int gwq_start(struct gwq_state *s,const struct gwq_io *io,unsigned major) {
    if(!s || !io || !io->send || !io->pump || !io->asset || !io->allocate || !io->clean || !io->now ||
        (major!=1 && major!=2) || s->registered || s->host_cap)return GWQ_ARGUMENT;
    /* Never advertise host physical RAM or an unchecked/wrapping IOVA span.
     * Validate before sending even discovery/registration traffic. */
    if(!io->host_iova_base || !io->host_iova_bytes ||
       ((io->host_iova_base|io->host_iova_bytes)&4095) ||
       io->host_iova_bytes>UINT64_MAX-io->host_iova_base || !io->dma_bytes ||
       io->dma_base<io->host_iova_base || io->dma_bytes>io->host_iova_bytes ||
       io->dma_base-io->host_iova_base>io->host_iova_bytes-io->dma_bytes)
        return gwq_fail(s,GWQ_MEMORY);
    s->boot_step=GWQ_BOOT_DISCOVERY;s->major=major;uint32_t b[5]={2,0,0,0,0};
    uint64_t discovery_started=io->now(io->context);unsigned discover=!s->server;
    if(gwq_control(s,io,2,1,GWQ_CTRL,0xffffffffu,GWQ_CTRL,b))return (int)s->error;
    while(!s->server && !s->error) {
        if(io->now(io->context)-discovery_started>=GWQ_TIMEOUT) {
            s->timeout_phase=GWQ_TIMEOUT_SERVER;return gwq_fail(s,GWQ_TIMEOUT_ERROR);
        }
        if(gwq_mission_check(s,io))return (int)s->error;
        if(io->pump(io->context,1000))gwq_fail(s,GWQ_TRANSPORT);
    }
    if(s->error)return (int)s->error;
    if(discover && s->server_received_ms-discovery_started>=GWQ_TIMEOUT) {
        s->timeout_phase=GWQ_TIMEOUT_SERVER;return gwq_fail(s,GWQ_TIMEOUT_ERROR);
    }
    s->boot_step=GWQ_BOOT_REGISTER;
    /* HST/HSP subscribes to FW_INIT_DONE, not legacy calibration FW_READY. */
    size_t n=0;gwq_num(s->request,&n,0x14,1,1);gwq_num(s->request,&n,0x15,0x4b4e454c,4);
    gwq_num(s->request,&n,0x16,1,1);gwq_num(s->request,&n,0x17,1,1);gwq_num(s->request,&n,0x18,1,1);
    if(gwq_request(s,io,0x20,n))return (int)s->error;
    size_t status_bytes;const uint8_t *status=gwq_field(s->response,s->response_bytes,0x10,&status_bytes);
    s->registration_status_present=status!=NULL;
    if(status) {
        if(status_bytes!=8)return gwq_fail(s,GWQ_WIRE);
        s->registration_status=gwq_u64(status);
        /* CNSS server-arrive stops before HOST_CAP on -EALREADY. This
         * cold-boot-only driver cannot adopt another client's DMA/session;
         * report the condition rather than inventing readiness or waiting. */
        if(s->registration_status&UINT64_C(1))return gwq_fail(s,GWQ_ALREADY_REGISTERED);
    }
    s->registered=1;s->boot_step=GWQ_BOOT_HOST_CAP;
    n=0;gwq_num(s->request,&n,0x10,1,4);gwq_num(s->request,&n,0x14,1,1);
    gwq_num(s->request,&n,0x16,1,1);gwq_num(s->request,&n,0x17,1,1);
    gwq_num(s->request,&n,0x1a,0,1);gwq_num(s->request,&n,0x1c,0,1);
    /* The phone uses CNSS/platform sleep clock policy, not the PC ath12k
     * NM_MODEM/NM_CLK policy. Do not change its PERST reset policy. */
    const char platform[]="OnePlus-Ace5-XNU";gwq_tlv(s->request,sizeof(s->request),&n,0x1e,platform,sizeof(platform)-1);
    /* OnePlus SMMU-S1 HOST_CAP: TLV 0x1f is a STATIC_ARRAY of three
     * {u64 start,u64 size}, with no count or validity byte on the wire.
     * Unused ranges stay zero. Only our mapped arena is accessible, never
     * the stock Android IOVA/IPA pool from the device tree. */
    uint8_t ranges[16*GWQ_HOST_DDR_RANGES]={0};
    gwq_w64(ranges,io->host_iova_base);gwq_w64(ranges+8,io->host_iova_bytes);
    gwq_tlv(s->request,sizeof(s->request),&n,0x1f,ranges,sizeof(ranges));
    gwq_num(s->request,&n,0x27,GWQ_HOST_FEATURES,8);
    s->host_cap=1;int error=gwq_request(s,io,0x34,n);
    if(!error)s->host_cap_ms=gwq_reply_ms(s);return error;
}
static inline int gwq_alloc(struct gwq_state *s,const struct gwq_io *io,size_t n,uint32_t type,uint64_t *pa,void **va) {
    *pa=0;*va=NULL;
    if(!n || n>io->dma_bytes || s->allocations>=GWQ_SEGMENTS+4)return 0;
    if(io->allocate(io->context,n,pa,va) || !*va || !*pa || (*pa&4095) || *pa<io->dma_base ||
        *pa-io->dma_base>io->dma_bytes-n){*pa=0;*va=NULL;return 0;}
    for(unsigned i=0;i<s->allocations;i++) { struct gwq_span *r=&s->allocated[i];
        if(*pa<r->address+r->bytes && r->address<*pa+n){gwq_fail(s,GWQ_MEMORY);*pa=0;*va=NULL;return 0;} }
    struct gwq_span *r=&s->allocated[s->allocations++];r->address=*pa;r->bytes=(uint32_t)n;r->type=type;
    return 1;
}
static inline int gwq_memory(struct gwq_state *s,const struct gwq_io *io) {
    s->boot_step=GWQ_BOOT_MEMORY;
    if(!s->mem_pending || ++s->mem_rounds>3 || s->mem_ready)return gwq_fail(s,GWQ_STATE);
    s->mem_request_count=0;s->mem_requested_bytes=0;
    s->mem_failure_index=s->mem_failure_type=UINT32_MAX;
    size_t n;const uint8_t *p=gwq_field(s->mem_request,s->mem_bytes,1,&n);
    if(!p || !n || !p[0] || p[0]>GWQ_SEGMENTS)return gwq_fail(s,GWQ_MEMORY);
    unsigned count=p[0];size_t o=1;uint32_t sizes[GWQ_SEGMENTS],types[GWQ_SEGMENTS];
    if(s->allocations>GWQ_SEGMENTS+4-count)return gwq_fail(s,GWQ_MEMORY);
    s->mem_request_count=count;
    for(unsigned i=0;i<count;i++) {
        if(n-o<9)return gwq_fail(s,GWQ_MEMORY);
        sizes[i]=gwq_u32(p+o);types[i]=gwq_u32(p+o+4);unsigned cfg=p[o+8];o+=9;
        s->mem_failure_index=i;s->mem_failure_type=types[i];
        if(!sizes[i] || types[i]>=GWQ_MEM_TYPES || cfg>2 || n-o<cfg*13 ||
           s->mem_requested_bytes>io->dma_bytes || sizes[i]>io->dma_bytes-s->mem_requested_bytes)
            return gwq_fail(s,GWQ_MEMORY);
        s->mem_requested_bytes+=sizes[i];
        for(unsigned j=0;j<cfg;j++,o+=13) {
            uint64_t off=gwq_u64(p+o);uint32_t z=gwq_u32(p+o+8);
            if(p[o+12] || off>sizes[i] || z>sizes[i]-off)return gwq_fail(s,GWQ_MEMORY);
        }
    }
    if(o!=n)return gwq_fail(s,GWQ_MEMORY);
    /* Validate the whole request before making any allocation or response. */
    uint8_t response[1+17*GWQ_SEGMENTS];response[0]=(uint8_t)count;
    for(unsigned i=0;i<count;i++) {
        uint64_t pa=0;void *va=NULL;unsigned type=types[i];
        s->mem_failure_index=i;s->mem_failure_type=type;
        /* cnss_pci_alloc_fw_mem fails the event on allocation failure; never
         * publish a nonzero length at IOVA zero and wait for FW_INIT_DONE.
         * Earlier allocations stay owned until the transport is quiescent. */
        if(!gwq_alloc(s,io,sizes[i],type,&pa,&va))return gwq_fail(s,GWQ_MEMORY);
        volatile uint8_t *b=(volatile uint8_t *)va;
        for(size_t j=0;j<sizes[i];j++)b[j]=0;
        io->clean(io->context,va,sizes[i]);
        uint8_t *r=response+1+i*17;gwq_w64(r,pa);gwq_w32(r+8,sizes[i]);gwq_w32(r+12,type);r[16]=0;
    }
    s->mem_failure_index=s->mem_failure_type=UINT32_MAX;
    n=0;gwq_tlv(s->request,sizeof(s->request),&n,1,response,1+17*count);
    s->mem_pending=0;s->mem_response_sent=1;return gwq_request(s,io,0x36,n);
}
static inline int gwq_download(struct gwq_state *s,const struct gwq_io *io,unsigned kind,unsigned bdf_type) {
    const void *v=NULL;size_t size=0;
    if(io->asset(io->context,kind,s->major,s->chip,s->board,&v,&size) || !v || !size || size>1024*1024)
        return gwq_fail(s,GWQ_ASSET);
    const uint8_t *p=(const uint8_t *)v;
    for(size_t off=0,seg=0;off<size;seg++) {
        size_t take=size-off;if(take>6144)take=6144;size_t n=0;
        gwq_num(s->request,&n,1,1,1);gwq_num(s->request,&n,0x10,s->board,4);
        /* WLANFW total_size is remaining bytes for each segment, not the
         * original file size (both the vendor driver and ath12k do this). */
        gwq_num(s->request,&n,0x11,size-off,4);gwq_num(s->request,&n,0x12,seg,4);
        s->request[n]=0x13;gwq_w16(s->request+n+1,(uint16_t)(take+2));gwq_w16(s->request+n+3,(uint16_t)take);
        gwq_copy(s->request+n+5,p+off,take);n+=5+take;
        gwq_num(s->request,&n,0x14,off+take==size,1);gwq_num(s->request,&n,0x15,bdf_type,1);
        if(gwq_request(s,io,0x25,n))return (int)s->error;off+=take;
    }
    return 0;
}
/* CAP 0x22 is wlfw_pmu_cfg_v01, not a QMI string array: one count byte and
 * 48-byte entries, each with a fixed 32-byte name and four LE u32 fields.
 * Decode bounded metadata only; the platform callback validates the board
 * mapping and applies it before resource provisioning. The wire parser never
 * writes AOP/QMP or interprets transport consumption as voltage readback. */
static inline int gwq_pmu_cap(struct gwq_state *s) {
    size_t n;const uint8_t *p=gwq_field(s->response,s->response_bytes,0x22,&n);
    s->pmu_present=p!=NULL;s->pmu_bytes=(uint32_t)n;s->pmu_count=0;
    s->pmu_valid=0;s->pmu_bad_entry=0xffffffffu;
    if(!p)return 0;
    if(!n)return gwq_fail(s,GWQ_WIRE);
    s->pmu_count=p[0];
    if(s->pmu_count>16 || n!=1u+48u*s->pmu_count)return gwq_fail(s,GWQ_WIRE);
    for(unsigned i=0;i<s->pmu_count;i++) {
        const uint8_t *entry=p+1+48*i;struct gwq_pmu_param *out=&s->pmu[i];
        s->pmu_bad_entry=i;
        if(!entry[0])return gwq_fail(s,GWQ_WIRE);
        for(unsigned j=0;j<32 && entry[j];j++) {
            unsigned c=entry[j];
            if(!((c>='A'&&c<='Z') || (c>='a'&&c<='z') || (c>='0'&&c<='9') ||
                 c=='_' || c=='-' || c=='.'))return gwq_fail(s,GWQ_WIRE);
        }
        gwq_copy(out->pin_name,entry,32);out->pin_name[32]=0;
        out->wake_valid=gwq_u32(entry+32);out->wake_mv=gwq_u32(entry+36);
        out->sleep_valid=gwq_u32(entry+40);out->sleep_mv=gwq_u32(entry+44);
        if(out->wake_valid>1 || out->sleep_valid>1)return gwq_fail(s,GWQ_WIRE);
    }
    s->pmu_bad_entry=0xffffffffu;s->pmu_valid=1;return 0;
}
/* CAP device memory is a STATIC_ARRAY of four {u64 start,u64 size}
 * entries, unlike REQUEST_MEM's counted array. These are device addresses,
 * not host physical RAM and not an authorization to map arbitrary memory.
 * DP must check the required CMEM extent/alignment before any BAR access. */
static inline int gwq_device_mem_cap(struct gwq_state *s) {
    size_t bytes;const uint8_t *p=gwq_field(s->response,s->response_bytes,0x1c,&bytes);
    s->dev_mem_present=p!=NULL;s->dev_mem_valid=0;
    for(unsigned i=0;i<GWQ_DEVICE_MEMORY_REGIONS;i++)s->dev_mem[i].start=s->dev_mem[i].bytes=0;
    if(!p)return 0;
    if(bytes!=16u*GWQ_DEVICE_MEMORY_REGIONS)return gwq_fail(s,GWQ_WIRE);
    for(unsigned i=0;i<GWQ_DEVICE_MEMORY_REGIONS;i++) {
        uint64_t start=gwq_u64(p+16*i),size=gwq_u64(p+16*i+8);
        if(size>UINT64_MAX-start)return gwq_fail(s,GWQ_WIRE);
        s->dev_mem[i].start=start;s->dev_mem[i].bytes=size;
    }
    s->dev_mem_valid=1;return 0;
}
static inline int gwq_provision(struct gwq_state *s,const struct gwq_io *io) {
    if(!s->mem_ready || s->provisioned)return gwq_fail(s,GWQ_STATE);
    s->boot_step=GWQ_BOOT_CAP;
    if(gwq_request(s,io,0x24,0))return (int)s->error;
    s->cap_ms=gwq_reply_ms(s);
    size_t chip_bytes,board_bytes,z;
    const uint8_t *chip=gwq_field(s->response,s->response_bytes,0x10,&chip_bytes);
    const uint8_t *board=gwq_field(s->response,s->response_bytes,0x11,&board_bytes);
    /* Retain only the identity metadata for diagnostics. The CAP reply also
     * has optional device serial fields, which must not be logged wholesale. */
    s->cap_response_bytes=(uint32_t)s->response_bytes;
    s->chip_info_present=chip!=NULL;s->chip_info_bytes=(uint32_t)chip_bytes;
    s->board_info_present=board!=NULL;s->board_info_bytes=(uint32_t)board_bytes;
    s->chip=s->chip_family=s->board=0;s->board_default=board==NULL;
    if(chip && chip_bytes==8){s->chip=gwq_u32(chip);s->chip_family=gwq_u32(chip+4);}
    if(board && board_bytes==4)s->board=gwq_u32(board);
    /* OnePlus CNSS cnss_wlfw_tgt_cap_send_sync explicitly assigns board 0xff
     * when board_info_valid is absent. This is the protocol's default-board
     * identity, not a fallback for malformed or unknown explicit board IDs.
     * The asset callback still has to match the pinned chip/board catalog. */
    if(!board)s->board=0xff;
    if(!chip || chip_bytes!=8 || (board && board_bytes!=4))return gwq_fail(s,GWQ_IDENTITY);
    const uint8_t *p;
    p=gwq_field(s->response,s->response_bytes,0x12,&z);if(p && z==4)s->soc=gwq_u32(p);
    p=gwq_field(s->response,s->response_bytes,0x13,&z);if(p && z>=4)s->firmware=gwq_u32(p);
    if(gwq_device_mem_cap(s) || gwq_pmu_cap(s))return (int)s->error;
    /* CNSS applies the validated CAP PMU table before firmware resource
     * provisioning. The platform owns AOP serialization, board constraints and
     * retained/uncertain writes; never send assets after a platform failure. */
    if(s->pmu_present) {
        s->boot_step=GWQ_BOOT_PMU;
        if(gwq_mission_check(s,io))return (int)s->error;
        s->platform_error=io->configure_pmu?
            io->configure_pmu(io->context,s->pmu,s->pmu_count):-1;
        s->pmu_config_ms=io->now(io->context)-s->mission_started_ms;
        if(s->platform_error)return gwq_fail(s,GWQ_PLATFORM_ERROR);
        s->pmu_configured=1;
        if(gwq_mission_check(s,io))return (int)s->error;
    }
    s->boot_step=GWQ_BOOT_REGDB;
    if(gwq_download(s,io,GWQ_REGDB,4))return (int)s->error;
    s->boot_step=GWQ_BOOT_BDF;
    if(gwq_download(s,io,GWQ_BOARD,1))return (int)s->error;
    s->boot_step=GWQ_BOOT_M3;
    const void *phy=NULL;size_t size=0;uint64_t pa;void *va;
    if(io->asset(io->context,GWQ_PHY,s->major,s->chip,s->board,&phy,&size) || !phy || !size || size>1024*1024)
        return gwq_fail(s,GWQ_ASSET);
    if(!gwq_alloc(s,io,size,0x3c,&pa,&va))return gwq_fail(s,GWQ_MEMORY);
    gwq_copy(va,phy,size);io->clean(io->context,va,size);
    size_t n=0;gwq_num(s->request,&n,1,pa,8);gwq_num(s->request,&n,2,size,4);
    if(gwq_request(s,io,0x3c,n))return (int)s->error;
    s->m3_ms=gwq_reply_ms(s);s->provisioned=1;s->boot_step=GWQ_BOOT_WAIT_INIT;return 0;
}
static inline int gwq_initialize(struct gwq_state *s,const struct gwq_io *io,unsigned major) {
    if(!s || !io || !io->now || s->initializing)return GWQ_ARGUMENT;
    if(s->error)return (int)s->error;
    s->initializing=1;s->timeout_phase=GWQ_TIMEOUT_NONE;s->mission_started_ms=io->now(io->context);
    int e=gwq_start(s,io,major);if(e)gwq_fail(s,(unsigned)e);
    while(!s->error) {
        /* Both notifications may have completed within the deadline even if
         * the polling worker was scheduled again after it. Receive timestamps
         * were already checked; do not reject a completed in-time handshake. */
        if(s->provisioned && s->fw_ready)break;
        if(gwq_mission_check(s,io))break;
        if(s->mem_pending && gwq_memory(s,io))break;
        if(s->mem_ready && !s->provisioned && gwq_provision(s,io))break;
        if(s->provisioned && s->fw_ready)break;
        if(io->pump(io->context,1000))gwq_fail(s,GWQ_TRANSPORT);
    }
    s->elapsed_ms=io->now(io->context)-s->mission_started_ms;s->initializing=0;
    if(!s->error)s->boot_step=GWQ_BOOT_READY;
    return (int)s->error;
}
/* Caller must initialize the matching Copy Engine rings before WLAN_MODE.
 * Both arrays contain the exact QMI wire entries, not compiler structs. */
static inline int gwq_configure(struct gwq_state *s,const struct gwq_io *io,
    const uint8_t *pipes,unsigned npipes,const uint8_t *services,unsigned nservices) {
    if(!s->provisioned || !s->fw_ready || s->configured || !pipes || !services || !npipes || npipes>12 || !nservices || nservices>24)
        return gwq_fail(s,GWQ_STATE);
    s->boot_step=GWQ_BOOT_CONFIG;
    size_t n=0;const char version[]="Giulia-XNU-1";
    gwq_tlv(s->request,sizeof(s->request),&n,0x10,version,sizeof(version)-1);
    uint8_t table[1+24*20];table[0]=(uint8_t)npipes;gwq_copy(table+1,pipes,npipes*20);
    gwq_tlv(s->request,sizeof(s->request),&n,0x11,table,1+npipes*20);
    table[0]=(uint8_t)nservices;gwq_copy(table+1,services,nservices*12);
    gwq_tlv(s->request,sizeof(s->request),&n,0x12,table,1+nservices*12);
    if(gwq_request(s,io,0x23,n))return (int)s->error;
    s->boot_step=GWQ_BOOT_MODE;
    n=0;gwq_num(s->request,&n,1,0,4); /* QMI_WLANFW_MISSION = 0 */
    gwq_num(s->request,&n,0x10,0,1);
    if(gwq_request(s,io,0x22,n))return (int)s->error;s->configured=1;s->boot_step=GWQ_BOOT_CONFIGURED;return 0;
}
#endif
