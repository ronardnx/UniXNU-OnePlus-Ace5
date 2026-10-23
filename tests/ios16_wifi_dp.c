/* Host model of WCN7850 MMIO/HTT/DMA, independent device-side producers. */
#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "fixtures/ath12k_wifi_layout.h"
#include "../ios16/wifi/giulia_wifi_dp.h"
#define AT(type,field) offsetof(struct type,field)
static unsigned shift(uint32_t mask) {unsigned n=0;assert(mask);while(!(mask&1)){mask>>=1;n++;}return n;}
static uint32_t field(uint32_t mask,unsigned value) {assert(!((uint32_t)value&~(mask>>shift(mask))));return (uint32_t)value<<shift(mask);}
static uint32_t value(uint32_t mask,uint32_t word) {return (word&mask)>>shift(mask);}
_Static_assert(sizeof(struct hal_tcl_data_cmd)==32,"upstream TCL size");
_Static_assert(sizeof(struct hal_reo_dest_ring)==32,"upstream REO size");
_Static_assert(sizeof(struct hal_rx_msdu_link)==128,"upstream link size");
_Static_assert(sizeof(struct hal_rx_desc_wcn7850)==GWD_RX_DESC_BYTES,"upstream RX size");
_Static_assert(AT(hal_wbm_completion_ring_tx,rate_stats.info0)==20,"upstream TX rate offset");
_Static_assert(AT(hal_rx_desc_wcn7850,msdu_end.info12)==96,"upstream RX rate offset");
_Static_assert(GWD_QUEUES+GWD_QUEUE_BANK_BYTES<=GWD_MGMT,"queue/mgmt separation");
_Static_assert(sizeof(struct hal_tlv_64_hdr)+sizeof(struct hal_reo_get_queue_stats)==48,"upstream REO command stride");
_Static_assert(sizeof(struct hal_tlv_64_hdr)+sizeof(struct hal_reo_get_queue_stats_status)==112,"upstream REO status stride");
struct reg {uint32_t address,value;};
struct fixture {
    struct gwd_state s;struct gwd_io io;struct reg regs[256];unsigned reg_count;
    unsigned writes,write_fail,deny,not_idle,wire_pending,wire_calls,setup_calls,filter_calls;
    unsigned received,completed,peer_events,last_token,last_status,wire_fail,version_major,misc_status,receive_fail;
    unsigned reo_commands,reo_cycle_cmd,reo_fault,reo_fault_at,reo_defer_tail,reo_deferred,reo_mask;
    uint8_t reo_cached[17];uint64_t reo_invalidated_at;
    uint64_t now;uint8_t *ram;
};
static unsigned checks;
static uint32_t readreg(void *v,uint32_t a) {
    struct fixture *f=v;assert(!(a&3) && a>=0xa34000 && a<0xa47000);
    uint32_t status=0;
    for(unsigned i=0;i<GWD_RINGS;i++)if(!f->s.ring[i].lmac && a==f->s.ring[i].reg+16)status=f->misc_status;
    for(unsigned i=0;i<f->reg_count;i++)if(f->regs[i].address==a)return f->regs[i].value|status;
    return status;
}
static void writereg(void *v,uint32_t a,uint32_t value) {
    struct fixture *f=v;assert(!(a&3) && a>=0xa34000 && a<0xa47000);
    if(++f->writes==f->write_fail)return;
    unsigned i;for(i=0;i<f->reg_count;i++)if(f->regs[i].address==a)break;
    if(i==f->reg_count){assert(i<256);f->regs[i].address=a;f->reg_count++;}f->regs[i].value=value;
}
static int ready(void *v,uint64_t dma,size_t n) {
    struct fixture *f=v;assert(dma==f->io.dma && n==GWD_BYTES);return !f->deny;
}
static void syncmem(void *v,size_t offset,size_t n,int to_device) {
    struct fixture *f=v;assert(offset<GWD_BYTES && n && n<=GWD_BYTES-offset);
    assert(to_device==0 || to_device==1);assert(f->ram==f->io.memory);
}
static int sendwire(void *v,unsigned pipe,const void *buffer,size_t n) {
    struct fixture *f=v;const uint8_t *p=buffer;
    assert(n>=12 && n<=256 && gwq_u16(p+2)==n-8 && !p[1] && !p[4]);f->wire_calls++;
    if(f->wire_fail)return 1;
    if(pipe==0) {
        assert(n==16 && !p[0] && gwq_u32(p+8)==0x03000002 && gwq_u32(p+12)==8);f->wire_pending=1;
    } else {
        assert(pipe==4 && p[0]==7);unsigned type=p[8];
        if(type==0){assert(n==12);f->wire_pending=2;}
        else if(type==0xb) {
            assert(n==60 && gwq_u64(p+12)>=f->io.dma && gwq_u64(p+12)<f->io.dma+GWD_BYTES);
            unsigned ring=p[10],kind=p[11],mac=p[9];
            assert((ring==5 && kind==2 && mac==0) || (ring==0 && kind==1 && (mac==1||mac==2)) || (ring==7 && kind==0 && mac==1));
            assert(gwq_u64(p+24)>=f->io.dma && gwq_u64(p+24)<f->io.dma+1024);
            assert(gwq_u64(p+32)>=f->io.dma && gwq_u64(p+32)<f->io.dma+1024);f->setup_calls++;
        } else {
            assert(type==0xc && n==76 && (p[9]==1 || p[9]==2) && gwq_u32(p+12)==2048);
            uint32_t subscribed=gwq_u32(p+8+AT(htt_rx_ring_selection_cfg_cmd,pkt_type_en_flags3));
            assert(subscribed==(HTT_RX_FP_DATA_PKT_FILTER_TLV_FLASG3_MCAST|
                HTT_RX_FP_DATA_PKT_FILTER_TLV_FLASG3_UCAST|HTT_RX_FP_DATA_PKT_FILTER_TLV_FLASG3_NULL_DATA));
            assert(gwq_u32(p+32)==13 && gwq_u32(p+36)==(400u|(272u<<16)));
            assert(gwq_u32(p+40)==144u<<16 && gwq_u32(p+44)==0);f->filter_calls++;
        }
    }
    return 0;
}
static int pump(void *v,unsigned us) {
    struct fixture *f=v;f->now+=(us+999)/1000;
    if(f->wire_pending==1) {
        uint8_t p[12]={3,0,0,3,0,7,0,8};f->wire_pending=0;assert(!gwd_htc_control(&f->s,p,sizeof(p)));
    } else if(f->wire_pending==2) {
        uint8_t p[4]={0};gwq_w32(p,f->version_major<<16);f->wire_pending=0;
        return gwd_receive_htt(&f->s,&f->io,7,p,sizeof(p));
    }
    return 0;
}
static uint64_t now(void *v){return ((struct fixture *)v)->now;}
static int idle(void *v){return !((struct fixture *)v)->not_idle;}
static int receive(void *v,const void *p,size_t n) {
    struct fixture *f=v;assert(n==60 && ((const uint8_t *)p)[6]==2);if(f->receive_fail)return 1;f->received++;return 0;
}
static void complete(void *v,uint32_t token,unsigned status) {
    struct fixture *f=v;f->completed++;f->last_token=token;f->last_status=status;
}
static void peer(void *v,unsigned vdev,const uint8_t *mac,unsigned id,int mapped) {
    struct fixture *f=v;assert(vdev==0 && mac[0]==2 && id==7 && (mapped==0||mapped==1));f->peer_events++;
}
static struct fixture *make(void) {
    struct fixture *f=calloc(1,sizeof(*f));assert(f);assert(!posix_memalign((void **)&f->ram,0x4000,GWD_BYTES));
    f->io=(struct gwd_io){f,readreg,writereg,ready,syncmem,sendwire,pump,now,idle,receive,complete,peer,f->ram,0x28000000};
    f->version_major=3;f->reo_mask=0x1ffff;return f;
}
static void destroy(struct fixture *f) {
    f->write_fail=0;f->not_idle=0;assert(gwd_stop(&f->s,&f->io));
    for(unsigned i=0;i<f->reg_count;i++)assert(f->regs[i].value==0);
    free(f->ram);free(f);checks++;
}
static const uint8_t ap[6]={2,3,4,5,6,7};
enum reo_fault {
    REO_EXEC_FAILED=1,REO_WRONG_TAG,REO_WRONG_ID,REO_FLUSH_BUSY,REO_SHORT_STATUS,
    REO_BLOCK_ERROR,REO_UNUSED_BLOCK_DETAIL,REO_FLUSH_LOCKED,REO_FLUSH_ALL_LOCKED,
    REO_EXEC_BLOCKED,REO_RESOURCE_BLOCKED,REO_LONG_STATUS,REO_SHORT_FLUSH
};
static void start(struct fixture *f,int encrypted) {
    assert(!gwd_prepare(&f->s,&f->io));assert(!gwd_connect(&f->s,&f->io));assert(!gwd_start(&f->s,&f->io));
    assert(f->setup_calls==4 && f->filter_calls==2);
    assert(!gwd_select_peer(&f->s,0,0,0,ap,encrypted));
    uint8_t p[16]={0};gwq_w32(p,0x1e|(7u<<16));memcpy(p+4,ap,6);gwq_w16(p+10,0x123);gwq_w16(p+12,0xa);
    assert(!gwd_receive_htt(&f->s,&f->io,7,p,sizeof(p)));assert(f->peer_events==1);
    for(unsigned tid=0;tid<17;tid++) {
        uint64_t pa=0;assert(!gwd_reo_queue(&f->s,&f->io,tid,&pa));assert(pa==f->io.dma+GWD_QUEUES+tid*1024);
        const uint8_t *q=f->ram+GWD_QUEUES+tid*1024;assert(gwq_u32(q)==0xddbeef84 && gwq_u32(q+4)==tid);
        if(tid<16)assert(gwq_u32(q+128)==0xadbeef94 && gwq_u32(q+256)==0xbdbeef94 && gwq_u32(q+384)==0xcdbeef94);
    }
    if(!encrypted)assert(!gwd_authorize(&f->s,1));
}
static void device_tx(struct fixture *f,unsigned status) {
    struct gwd_ring *r=&f->s.ring[GWD_TCL],*c=&f->s.ring[GWD_TX_COMPLETE];
    unsigned tail=gwq_u32(f->ram+r->remote)/8,head=gwq_u32(f->ram+c->remote)/8;
    assert(tail!=r->head);const uint8_t *d=f->ram+r->desc+tail*32;
    uint64_t pa=(uint64_t)gwq_u32(d)|((uint64_t)(gwq_u32(d+4)&255)<<32);
    assert(pa>=f->io.dma+GWD_TX_BUFFERS && pa<f->io.dma+GWD_TX_BUFFERS+GWD_TX_COUNT*2048);
    assert(value(BUFFER_ADDR_INFO1_RET_BUF_MGR,gwq_u32(d+AT(hal_tcl_data_cmd,buf_addr_info.info1)))==5);
    assert(value(HAL_TCL_DATA_CMD_INFO1_CMD_NUM,gwq_u32(d+AT(hal_tcl_data_cmd,info1)))==HTT_TCL_META_DATA_TYPE);
    unsigned bank=value(HAL_TCL_DATA_CMD_INFO0_BANK_ID,gwq_u32(d+AT(hal_tcl_data_cmd,info0)));
    assert(bank<=1);
    assert(value(HAL_TCL_DATA_CMD_INFO2_DATA_LEN,gwq_u32(d+AT(hal_tcl_data_cmd,info2)))==(bank?78u:60u));
    assert(gwq_u32(d+AT(hal_tcl_data_cmd,info3))==HAL_TCL_DATA_CMD_INFO3_TID_OVERWRITE);
    assert(gwq_u32(d+AT(hal_tcl_data_cmd,info4))==(field(HAL_TCL_DATA_CMD_INFO4_CACHE_SET_NUM,10)|field(HAL_TCL_DATA_CMD_INFO4_SEARCH_INDEX,0x123)));
    assert(gwq_u32(d+AT(hal_tcl_data_cmd,info0))==field(HAL_TCL_DATA_CMD_INFO0_BANK_ID,bank));
    if(bank) {
        static const uint8_t snap[8]={0xaa,0xaa,3,0,0,0,0x88,0x8e};
        const uint8_t *frame=f->ram+(pa-f->io.dma);
        assert(gwq_u16(frame)==0x0108 && !memcmp(frame+4,ap,6) && frame[10]==2);
        assert(!memcmp(frame+24,snap,sizeof(snap)));
        assert(readreg(f,HAL_TCL_SW_CONFIG_BANK_ADDR+4)==
            (field(HAL_TX_BANK_CONFIG_ENCAP_TYPE,1)|HAL_TX_BANK_CONFIG_ADDRY_EN|HAL_TX_BANK_CONFIG_VDEV_ID_CHECK_EN));
    }
    uint8_t *out=f->ram+c->desc+head*32;memset(out,0,32);memcpy(out,d,8);gwq_w32(out+8,status<<13);
    gwq_w32(f->ram+r->remote,((tail+1)%r->count)*8);gwq_w32(f->ram+c->remote,((head+1)%c->count)*8);
}
static unsigned device_rx(struct fixture *f,int encrypted,uint64_t pn,int eapol,unsigned error) {
    struct gwd_ring *r=&f->s.ring[GWD_REFILL],*c=&f->s.ring[GWD_REO];
    unsigned tail=gwq_u32(f->ram+r->remote)/2,head=gwq_u32(f->ram+c->remote)/8;
    assert(tail!=r->head);const uint8_t *d=f->ram+r->desc+tail*8;
    uint32_t cookie=value(BUFFER_ADDR_INFO1_SW_COOKIE,gwq_u32(d+AT(ath12k_buffer_addr,info1)));unsigned slot=cookie&511;
    assert(value(BUFFER_ADDR_INFO1_RET_BUF_MGR,gwq_u32(d+AT(ath12k_buffer_addr,info1)))==6);
    uint8_t *p=f->ram+GWD_RX_BUFFERS+slot*2048;memset(p,0,2048);
    gwq_w64(p+AT(hal_rx_desc_wcn7850,msdu_end_tag),field(HAL_TLV_64_HDR_TAG,197)|field(HAL_TLV_64_HDR_LEN,sizeof(struct rx_msdu_end_wcn7850)));
    gwq_w64(p+AT(hal_rx_desc_wcn7850,mpdu_start_tag),field(HAL_TLV_64_HDR_TAG,194)|field(HAL_TLV_64_HDR_LEN,sizeof(struct rx_mpdu_start_qcn9274)));
    gwq_w32(p+AT(hal_rx_desc_wcn7850,msdu_end.info10),field(RX_MSDU_END_INFO10_MSDU_LENGTH,60));
    gwq_w32(p+AT(hal_rx_desc_wcn7850,msdu_end.info11),field(RX_MSDU_END_INFO11_DECAP_FORMAT,2));
    gwq_w32(p+AT(hal_rx_desc_wcn7850,msdu_end.info13),error);
    gwq_w32(p+AT(hal_rx_desc_wcn7850,msdu_end.info14),RX_MSDU_END_INFO14_MSDU_DONE);
    gwq_w16(p+AT(hal_rx_desc_wcn7850,mpdu_start.sw_peer_id),7);
    memcpy(p+AT(hal_rx_desc_wcn7850,mpdu_start.addr2),ap,6);
    uint32_t flags=RX_MPDU_START_INFO4_MPDU_FCTRL_VALID|RX_MPDU_START_INFO4_MAC_ADDR2_VALID;
    if(encrypted) {
        flags|=RX_MPDU_START_INFO4_ENCRYPTED|RX_MPDU_START_INFO4_ENCRYPT_INFO_VALID;
        gwq_w32(p+AT(hal_rx_desc_wcn7850,mpdu_start.info2),field(RX_MPDU_START_INFO2_ENC_TYPE,6));
        gwq_w64(p+AT(hal_rx_desc_wcn7850,mpdu_start.pn),pn);
    }
    gwq_w32(p+AT(hal_rx_desc_wcn7850,mpdu_start.info4),flags);
    uint8_t *frame=p+AT(hal_rx_desc_wcn7850,msdu_payload);frame[0]=2;frame[6]=2;frame[12]=eapol?0x88:8;frame[13]=eapol?0x8e:0;
    uint8_t *out=f->ram+c->desc+head*sizeof(struct hal_reo_dest_ring);memset(out,0,sizeof(struct hal_reo_dest_ring));memcpy(out,d,sizeof(struct ath12k_buffer_addr));
    gwq_w32(out+AT(hal_reo_dest_ring,rx_msdu_info.info0),RX_MSDU_DESC_INFO0_FIRST_MSDU_IN_MPDU|RX_MSDU_DESC_INFO0_LAST_MSDU_IN_MPDU|field(RX_MSDU_DESC_INFO0_MSDU_LENGTH,60));
    gwq_w32(out+AT(hal_reo_dest_ring,info0),field(HAL_REO_DEST_RING_INFO0_PUSH_REASON,1));
    gwq_w32(f->ram+r->remote,((tail+1)%r->count)*2);gwq_w32(f->ram+c->remote,((head+1)%c->count)*8);return slot;
}
static uint32_t device_tx_rate(struct fixture *f,unsigned reason,unsigned source,unsigned type,unsigned code,unsigned bw,int valid) {
    struct gwd_ring *r=&f->s.ring[GWD_TX_COMPLETE];unsigned head=gwq_u32(f->ram+r->remote)/r->entry;
    device_tx(f,reason);uint8_t *d=f->ram+r->desc+head*sizeof(struct hal_wbm_completion_ring_tx);
    uint32_t raw=field(HAL_TX_RATE_STATS_INFO0_PKT_TYPE,type)|field(HAL_TX_RATE_STATS_INFO0_MCS,code)|
        field(HAL_TX_RATE_STATS_INFO0_BW,bw)|field(HAL_TX_RATE_STATS_INFO0_SGI,HAL_TX_RATE_STATS_SGI_32US)|
        (valid?HAL_TX_RATE_STATS_INFO0_VALID:0);
    gwq_w32(d+AT(hal_wbm_completion_ring_tx,rate_stats.info0),raw);
    gwq_w32(d+AT(hal_wbm_completion_ring_tx,info0),field(HAL_WBM_COMPL_TX_INFO0_REL_SRC_MODULE,source)|
        field(HTT_TX_WBM_COMP_INFO0_STATUS,reason));return raw;
}
static uint32_t device_rx_rate(struct fixture *f,unsigned slot,unsigned type,unsigned code,unsigned bw) {
    uint8_t *p=f->ram+GWD_RX_BUFFERS+slot*GWD_BUFFER;
    uint32_t raw=0x5a|field(RX_MSDU_END_INFO12_PKT_TYPE,type)|field(RX_MSDU_END_INFO12_RATE_MCS,code)|
        field(RX_MSDU_END_INFO12_RECV_BW,bw)|field(RX_MSDU_END_INFO12_SGI,RX_MSDU_START_SGI_3_2_US)|
        field(RX_MSDU_END_WCN7850_INFO12_MIMO_SS_BITMAP,0x80);
    gwq_w32(p+AT(hal_rx_desc_wcn7850,msdu_end.info12),raw);
    gwq_w32(p+AT(hal_rx_desc_wcn7850,msdu_end.ppdu_start_timestamp_31_0),0xdeadbeef);return raw;
}
static void observed_rates(void) {
    for(unsigned type=0;type<16;type++)for(unsigned code=0;code<16;code++)for(unsigned bw=0;bw<8;bw++)
        assert(gwd_legacy_rate_kbps(type,code,bw)==(bw?0:oracle_legacy_rate_kbps(type,code)));
    struct fixture *f=make();start(f,0);uint8_t frame[60]={0};frame[6]=2;frame[12]=8;
    assert(!f->s.tx_rate.valid && !f->s.rx_rate.valid);
    for(unsigned i=0;i<sizeof(oracle_legacy_rates)/sizeof(oracle_legacy_rates[0]);i++) {
        const struct oracle_legacy_rate *rate=&oracle_legacy_rates[i];f->now+=7;
        assert(!gwd_send(&f->s,&f->io,frame,sizeof(frame),i));
        uint32_t raw=device_tx_rate(f,HAL_WBM_TQM_REL_REASON_FRAME_ACKED,HAL_WBM_REL_SRC_MODULE_TQM,
            rate->packet_type,rate->code,HAL_TX_RATE_STATS_BW_20,1);
        assert(!gwd_poll(&f->s,&f->io,64));
        assert(f->s.tx_rate.valid && f->s.tx_rate.kbps==rate->kbps && f->s.tx_rate.raw==raw && f->s.tx_rate.observed_ms==f->now);
        assert(!f->s.rx_rate.valid);
    }
    f->now++;assert(!gwd_send(&f->s,&f->io,frame,sizeof(frame),50));
    uint32_t raw=device_tx_rate(f,HAL_WBM_TQM_REL_REASON_CMD_REMOVE_TX,HAL_WBM_REL_SRC_MODULE_TQM,
        HAL_TX_RATE_STATS_PKT_TYPE_11B,ATH12K_HW_RATE_CCK_SP_2M,HAL_TX_RATE_STATS_BW_20,1);
    assert(!gwd_poll(&f->s,&f->io,64) && f->s.tx_rate.kbps==2000 && f->s.tx_rate.raw==raw);
    uint64_t observed=f->s.tx_rate.observed_ms;
    /* Missing VALID, FW-overlay words and unsent/drop TQM statuses cannot
     * replace the last measured transmit rate, even with plausible bits. */
    for(unsigned reason=0;reason<15;reason++) {
        f->now++;assert(!gwd_send(&f->s,&f->io,frame,sizeof(frame),51+reason));
        device_tx_rate(f,reason,HAL_WBM_REL_SRC_MODULE_TQM,HAL_TX_RATE_STATS_PKT_TYPE_11A,
            ATH12K_HW_RATE_OFDM_54M,HAL_TX_RATE_STATS_BW_20,reason!=0 && reason!=2);
        assert(!gwd_poll(&f->s,&f->io,64) && f->s.tx_rate.kbps==2000 && f->s.tx_rate.observed_ms==observed);
    }
    f->now++;assert(!gwd_send(&f->s,&f->io,frame,sizeof(frame),70));
    device_tx_rate(f,HAL_WBM_REL_HTT_TX_COMP_STATUS_OK,HAL_WBM_REL_SRC_MODULE_FW,
        HAL_TX_RATE_STATS_PKT_TYPE_11A,ATH12K_HW_RATE_OFDM_54M,HAL_TX_RATE_STATS_BW_20,1);
    assert(!gwd_poll(&f->s,&f->io,64) && f->s.tx_rate.kbps==2000 && f->s.tx_rate.observed_ms==observed);
    /* Real but unsupported PHY samples are unknown, never a fabricated rate
     * from an MCS without NSS or the BSS's maximum supported legacy rate. */
    f->now++;assert(!gwd_send(&f->s,&f->io,frame,sizeof(frame),71));
    raw=device_tx_rate(f,HAL_WBM_TQM_REL_REASON_FRAME_ACKED,HAL_WBM_REL_SRC_MODULE_TQM,
        HAL_TX_RATE_STATS_PKT_TYPE_11N,7,HAL_TX_RATE_STATS_BW_40,1);
    assert(!gwd_poll(&f->s,&f->io,64) && !f->s.tx_rate.valid && !f->s.tx_rate.kbps);
    assert(f->s.tx_rate.raw==raw && f->s.tx_rate.observed_ms==f->now);
    for(unsigned i=0;i<sizeof(oracle_legacy_rates)/sizeof(oracle_legacy_rates[0]);i++) {
        const struct oracle_legacy_rate *rate=&oracle_legacy_rates[i];f->now+=3;
        unsigned slot=device_rx(f,0,0,0,0);raw=device_rx_rate(f,slot,rate->packet_type,rate->code,RX_MSDU_START_RECV_BW_20MHZ);
        assert(!gwd_poll(&f->s,&f->io,64));
        assert(f->s.rx_rate.valid && f->s.rx_rate.kbps==rate->kbps && f->s.rx_rate.raw==raw && f->s.rx_rate.observed_ms==f->now);
        assert(!f->s.tx_rate.valid);
    }
    observed=f->s.rx_rate.observed_ms;
    for(unsigned fault=0;fault<6;fault++) {
        f->now++;unsigned slot=device_rx(f,0,0,0,0);uint8_t *p=f->ram+GWD_RX_BUFFERS+slot*GWD_BUFFER;
        device_rx_rate(f,slot,RX_MSDU_START_PKT_TYPE_11B,ATH12K_HW_RATE_CCK_LP_1M,RX_MSDU_START_RECV_BW_20MHZ);
        if(fault==0)gwq_w32(p+AT(hal_rx_desc_wcn7850,msdu_end.info14),0);
        if(fault==1)gwq_w32(p+AT(hal_rx_desc_wcn7850,msdu_end.info13),RX_MSDU_END_INFO13_FCS_ERR);
        if(fault==2)gwq_w16(p+AT(hal_rx_desc_wcn7850,mpdu_start.sw_peer_id),8);
        if(fault==3)gwq_w32(p+AT(hal_rx_desc_wcn7850,msdu_end.info10),59);
        if(fault==4)f->s.authorized=0;
        if(fault==5)f->receive_fail=1;
        assert(!gwd_poll(&f->s,&f->io,64) && f->s.rx_rate.kbps==54000 && f->s.rx_rate.observed_ms==observed);
        f->s.authorized=1;f->receive_fail=0;
    }
    f->now++;unsigned slot=device_rx(f,0,0,0,0);
    raw=device_rx_rate(f,slot,RX_MSDU_START_PKT_TYPE_11AX,11,RX_MSDU_START_RECV_BW_80MHZ);
    assert(!gwd_poll(&f->s,&f->io,64) && !f->s.rx_rate.valid && !f->s.rx_rate.kbps);
    assert(f->s.rx_rate.raw==raw && f->s.rx_rate.observed_ms==f->now);
    destroy(f);
}
static void normal(void) {
    struct fixture *f=make();start(f,0);uint8_t frame[60]={0};frame[6]=2;frame[12]=8;
    for(unsigned i=0;i<63;i++)assert(!gwd_send(&f->s,&f->io,frame,60,i));
    assert(gwd_send(&f->s,&f->io,frame,60,63)==GWD_FULL);
    for(unsigned i=0;i<63;i++)device_tx(f,0);assert(!gwd_poll(&f->s,&f->io,128));assert(f->completed==63 && !f->s.tx_pending);
    for(unsigned i=0;i<2000;i++) {
        assert(!gwd_send(&f->s,&f->io,frame,60,i));device_tx(f,i&1);device_rx(f,0,0,0,0);
        assert(!gwd_poll(&f->s,&f->io,64));assert(f->last_token==i && f->last_status==(i&1));
    }
    assert(f->received==2000 && !f->s.tx_pending);destroy(f);
}
static void firmware_completions(void) {
    for(unsigned status=0;status<16;status++) {
        struct fixture *f=make();start(f,0);uint8_t frame[60]={0};frame[6]=2;frame[12]=8;
        assert(!gwd_send(&f->s,&f->io,frame,sizeof(frame),status+1));
        struct gwd_ring *r=&f->s.ring[GWD_TX_COMPLETE];
        unsigned head=gwq_u32(f->ram+r->remote)/r->entry;device_tx(f,status);
        uint8_t *d=f->ram+r->desc+head*sizeof(struct hal_wbm_release_ring);
        gwq_w32(d+AT(hal_wbm_release_ring,info0),
            field(HAL_WBM_COMPL_TX_INFO0_REL_SRC_MODULE,HAL_WBM_REL_SRC_MODULE_FW)|
            field(HTT_TX_WBM_COMP_INFO0_STATUS,status));
        int error=gwd_poll(&f->s,&f->io,64);
        if(oracle_htt_tx_releases_dma(status)) {
            assert(!error && f->completed==1 && f->last_token==status+1 && f->last_status==status);
            assert(!f->s.tx_owned[0] && !f->s.tx_pending && !f->s.quarantine);
        } else {
            /* A notification/unknown event is not a completion even when it
             * happens to carry the address and cookie of an outstanding TX. */
            assert(error==GWD_FIRMWARE && !f->completed && f->s.tx_owned[0] && f->s.tx_pending==1);
            assert(f->s.quarantine && !r->tail);
            f->not_idle=1;assert(!gwd_stop(&f->s,&f->io));
            assert(f->s.prepared && f->s.tx_owned[0] && f->s.tx_pending==1);f->not_idle=0;
        }
        destroy(f);
    }
    /* The same numeric status is a normal TQM queue-removal completion. */
    struct fixture *f=make();start(f,0);uint8_t frame[60]={0};frame[6]=2;frame[12]=8;
    assert(!gwd_send(&f->s,&f->io,frame,sizeof(frame),77));device_tx(f,5);
    assert(!gwd_poll(&f->s,&f->io,64) && f->completed==1 && f->last_status==5 && !f->s.tx_pending);
    destroy(f);
}
static void open_mcbc(void) {
    struct fixture *f=make();start(f,0);
    for(unsigned broadcast=0;broadcast<2;broadcast++) {
        unsigned slot=device_rx(f,0,0,0,0);uint8_t *p=f->ram+GWD_RX_BUFFERS+slot*GWD_BUFFER;
        uint8_t *frame=p+AT(hal_rx_desc_wcn7850,msdu_payload);
        if(broadcast)memset(frame,0xff,6);else frame[0]=1;
        gwq_w32(p+AT(hal_rx_desc_wcn7850,msdu_end.info13),RX_MSDU_END_INFO13_MCAST_BCAST);
        assert(!gwd_poll(&f->s,&f->io,64) && f->received==broadcast+1 && !f->s.rx_dropped);
    }
    destroy(f);
}
static void security(void) {
    struct fixture *f=make();start(f,1);uint8_t frame[60]={0};frame[6]=2;frame[12]=8;
    assert(gwd_send(&f->s,&f->io,frame,60,1)==GWD_NOT_READY);
    frame[12]=0x88;frame[13]=0x8e;assert(!gwd_send(&f->s,&f->io,frame,60,1));device_tx(f,0);
    device_rx(f,0,0,1,0);assert(!gwd_poll(&f->s,&f->io,64) && f->received==1);
    assert(gwd_authorize(&f->s,1)==GWD_NOT_READY);
    assert(!gwd_key_ready(&f->s,0,1,5) && !gwd_key_ready(&f->s,1,0,10));assert(!gwd_authorize(&f->s,1));
    /* M4 and group-key responses stay unprotected after firmware key ACKs. */
    assert(!gwd_send(&f->s,&f->io,frame,60,2));device_tx(f,0);assert(!gwd_poll(&f->s,&f->io,64));
    device_rx(f,1,6,0,0);assert(!gwd_poll(&f->s,&f->io,64) && f->received==2);
    device_rx(f,1,6,0,0);assert(!gwd_poll(&f->s,&f->io,64) && f->received==2 && f->s.rx_dropped==1);
    device_rx(f,0,0,0,0);assert(!gwd_poll(&f->s,&f->io,64) && f->received==2);
    device_rx(f,1,7,0,1u<<29);assert(!gwd_poll(&f->s,&f->io,64) && f->received==2);
    device_rx(f,1,7,0,0);assert(!gwd_poll(&f->s,&f->io,64) && f->received==3);
    uint8_t unmap[4];gwq_w32(unmap,4|(7u<<16));assert(!gwd_receive_htt(&f->s,&f->io,7,unmap,4));
    assert(!f->s.peer_mapped && !f->s.authorized && f->peer_events==2);destroy(f);
}
static unsigned device_group_rx(struct fixture *f,unsigned key,uint64_t pn,unsigned tid) {
    unsigned slot=device_rx(f,1,pn,0,0);uint8_t *p=f->ram+GWD_RX_BUFFERS+slot*GWD_BUFFER;
    /* Follow ath12k_hw_wcn7850_rx_desc_get_crypto_hdr(), not the ambiguous
     * rx_desc.h comment: hardware KEY_ID is the index before IV conversion. */
    static const uint8_t iv_key_id[4]={0x20,0x60,0xa0,0xe0};assert(key<4);
    gwq_w32(p+AT(hal_rx_desc_wcn7850,mpdu_start.info5),field(RX_MPDU_START_INFO5_KEY_ID,key));
    const struct hal_rx_desc_wcn7850 *rx=(const struct hal_rx_desc_wcn7850 *)p;
    assert(oracle_wcn7850_ccmp_key_id_octet(rx)==iv_key_id[key]);
    /* WCN7850 RX_MSDU_END_INFO13_MCAST_BCAST records addr1/RA, which
     * selects group encryption independently of the decapsulated Ethernet DA. */
    gwq_w32(p+AT(hal_rx_desc_wcn7850,msdu_end.info13),RX_MSDU_END_INFO13_MCAST_BCAST);
    assert(oracle_wcn7850_is_da_mcbc(rx));
    p[AT(hal_rx_desc_wcn7850,msdu_payload)]=1;
    if(tid<16) {
        gwq_w32(p+AT(hal_rx_desc_wcn7850,mpdu_start.info4),
            gwq_u32(p+AT(hal_rx_desc_wcn7850,mpdu_start.info4))|RX_MPDU_START_INFO4_MPDU_QOS_CTRL_VALID);
        gwq_w32(p+AT(hal_rx_desc_wcn7850,mpdu_start.info2),field(RX_MPDU_START_INFO2_ENC_TYPE,6)|field(RX_MPDU_START_INFO2_TID,tid));
    }
    return slot;
}
static void group_keys(void) {
    struct fixture *f=make();start(f,1);assert(!gwd_key_ready(&f->s,0,1,5));
    assert(!gwd_key_ready(&f->s,0,0,500));
    for(unsigned key=1;key<4;key++)assert(!gwd_key_ready(&f->s,key,0,key*10));
    assert(!gwd_authorize(&f->s,1));
    for(unsigned key=1;key<4;key++) {
        device_group_rx(f,key,key*10+1,16);assert(!gwd_poll(&f->s,&f->io,64));
        assert(f->received==key && f->s.pn[key][16]==key*10+1 && f->s.pn[0][16]==500);
        device_group_rx(f,key,key*10+1,16);assert(!gwd_poll(&f->s,&f->io,64));
        assert(f->received==key && f->s.rx_dropped==key);
    }
    /* A different TID has its own replay counter under the same GTK. */
    device_group_rx(f,2,21,7);assert(!gwd_poll(&f->s,&f->io,64));
    assert(f->received==4 && f->s.pn[2][7]==21 && f->s.pn[2][16]==21);
    device_group_rx(f,2,21,7);assert(!gwd_poll(&f->s,&f->io,64) && f->received==4);
    device_group_rx(f,0,501,16);assert(!gwd_poll(&f->s,&f->io,64));
    assert(f->received==5 && f->s.pn[0][16]==501 && f->s.pn[4][16]==5);
    /* A missing GTK must not borrow another installed GTK or its counter. */
    f->s.key_valid[3]=0;device_group_rx(f,3,32,16);
    assert(!gwd_poll(&f->s,&f->io,64) && f->received==5 && f->s.pn[3][16]==31);
    /* An AP may use a unicast receiver for an Ethernet multicast MSDU.
     * Its protected MPDU uses the PTK, regardless of Ethernet DA bit 0. */
    unsigned slot=device_rx(f,1,6,0,0);uint8_t *p=f->ram+GWD_RX_BUFFERS+slot*GWD_BUFFER;
    p[AT(hal_rx_desc_wcn7850,msdu_payload)]=1;
    gwq_w32(p+AT(hal_rx_desc_wcn7850,mpdu_start.info5),field(RX_MPDU_START_INFO5_KEY_ID,1));
    assert(!oracle_wcn7850_is_da_mcbc((const struct hal_rx_desc_wcn7850 *)p));
    assert(!gwd_poll(&f->s,&f->io,64));
    assert(f->received==6 && f->s.pn[4][16]==6 && f->s.pn[1][16]==11);
    /* The inverse descriptor/DA combination must still use its GTK. */
    slot=device_group_rx(f,2,22,16);p=f->ram+GWD_RX_BUFFERS+slot*GWD_BUFFER;
    p[AT(hal_rx_desc_wcn7850,msdu_payload)]=2;
    assert(!gwd_poll(&f->s,&f->io,64));
    assert(f->received==7 && f->s.pn[2][16]==22 && f->s.pn[4][16]==6);
    /* Neither mismatched Ethernet destination can borrow the other key. */
    f->s.key_valid[4]=0;slot=device_rx(f,1,7,0,0);p=f->ram+GWD_RX_BUFFERS+slot*GWD_BUFFER;
    p[AT(hal_rx_desc_wcn7850,msdu_payload)]=1;
    assert(!gwd_poll(&f->s,&f->io,64) && f->received==7 && f->s.pn[4][16]==6);
    f->s.key_valid[4]=1;f->s.key_valid[2]=0;slot=device_group_rx(f,2,23,16);
    p=f->ram+GWD_RX_BUFFERS+slot*GWD_BUFFER;p[AT(hal_rx_desc_wcn7850,msdu_payload)]=2;
    assert(!gwd_poll(&f->s,&f->io,64) && f->received==7 && f->s.pn[2][16]==22 && f->s.pn[4][16]==6);
    destroy(f);
}
static void failures(void) {
    struct fixture *f=make();f->deny=1;assert(gwd_prepare(&f->s,&f->io)==GWD_DMA && !f->writes);destroy(f);
    f=make();writereg(f,0xa44910,0x40);unsigned writes=f->writes;
    assert(gwd_prepare(&f->s,&f->io)==GWD_INHERITED && f->writes==writes);
    writereg(f,0xa44910,0);destroy(f);
    f=make();assert(!gwd_prepare(&f->s,&f->io));unsigned total=f->writes;destroy(f);
    for(unsigned i=1;i<=total;i++) {
        f=make();f->write_fail=i;int result=gwd_prepare(&f->s,&f->io);
        // Dropping a zero-valued write cannot change observable state.
        if(result)assert(result==GWD_REGISTER && f->s.quarantine);
        destroy(f);
    }
    f=make();assert(!gwd_prepare(&f->s,&f->io));f->version_major=4;assert(!gwd_connect(&f->s,&f->io));assert(gwd_start(&f->s,&f->io));destroy(f);
    f=make();assert(!gwd_prepare(&f->s,&f->io));f->wire_fail=1;assert(gwd_connect(&f->s,&f->io)==GWD_TRANSPORT);destroy(f);
    f=make();start(f,0);f->not_idle=1;assert(!gwd_stop(&f->s,&f->io) && f->s.quarantine && f->s.prepared);destroy(f);
    f=make();start(f,0);gwq_w32(f->ram+f->s.ring[GWD_REO].remote,3);
    assert(gwd_poll(&f->s,&f->io,64)==GWD_POINTER && f->s.quarantine);destroy(f);
    f=make();start(f,0);device_rx(f,0,0,0,0);gwq_w32(f->ram+f->s.ring[GWD_REO].desc+4,0xffffffff);
    assert(gwd_poll(&f->s,&f->io,64)==GWD_COOKIE && !f->received);destroy(f);
    f=make();start(f,0);gwq_w32(f->ram+f->s.ring[GWD_RX_ERROR].remote,8);
    assert(gwd_poll(&f->s,&f->io,64)==GWD_FRAME && f->s.quarantine);destroy(f);
}
static void lifetime(void) {
    struct fixture *f=make();start(f,0);uint8_t frame[60]={0};frame[6]=2;frame[12]=8;
    /* Cross the actual 20-bit generation boundary on a free TX slot. */
    f->s.tx_cookie[0]=0xfffc0;assert(!gwd_send(&f->s,&f->io,frame,60,17));
    assert(f->s.tx_cookie[0]==0);device_tx(f,0);assert(!gwd_poll(&f->s,&f->io,64));
    assert(f->completed==1 && f->last_token==17 && !f->s.tx_pending);
    /* RX generation wraps only after device DMA has completed. */
    unsigned slot=device_rx(f,0,0,0,0);struct gwd_ring *r=&f->s.ring[GWD_REO];
    f->s.rx_cookie[slot]=0xffe00|slot;
    gwq_w32(f->ram+r->desc+4,0x600|(f->s.rx_cookie[slot]<<12));
    assert(!gwd_poll(&f->s,&f->io,64) && f->received==1 && f->s.rx_cookie[slot]==slot);
    destroy(f);
    f=make();start(f,0);assert(!gwd_send(&f->s,&f->io,frame,60,1));device_tx(f,0);
    gwq_w32(f->ram+f->s.ring[GWD_TX_COMPLETE].desc,f->io.dma);
    assert(gwd_poll(&f->s,&f->io,64)==GWD_COOKIE && f->s.tx_owned[0] && !f->completed);destroy(f);
}
static void device_unmap(struct fixture *f) {
    uint8_t unmap[4];gwq_w32(unmap,4|(7u<<16));assert(!gwd_receive_htt(&f->s,&f->io,7,unmap,sizeof(unmap)));
}
static void observed_rate_lifetime(void) {
    struct fixture *f=make();start(f,0);uint8_t frame[60]={0};frame[6]=2;frame[12]=8;f->now+=10;
    assert(!gwd_send(&f->s,&f->io,frame,sizeof(frame),80));
    device_tx_rate(f,HAL_WBM_TQM_REL_REASON_FRAME_ACKED,HAL_WBM_REL_SRC_MODULE_TQM,
        HAL_TX_RATE_STATS_PKT_TYPE_11A,ATH12K_HW_RATE_OFDM_54M,HAL_TX_RATE_STATS_BW_20,1);
    unsigned slot=device_rx(f,0,0,0,0);
    device_rx_rate(f,slot,RX_MSDU_START_PKT_TYPE_11B,ATH12K_HW_RATE_CCK_LP_11M,RX_MSDU_START_RECV_BW_20MHZ);
    assert(!gwd_poll(&f->s,&f->io,64) && f->s.tx_rate.valid && f->s.rx_rate.valid);
    struct gwd_rate_sample tx=f->s.tx_rate,rx=f->s.rx_rate;
    assert(gwd_select_peer(&f->s,0,0,0,ap,0)==GWD_ARGUMENT);
    assert(f->s.tx_rate.kbps==54000 && f->s.rx_rate.kbps==11000);
    assert(!gwd_send(&f->s,&f->io,frame,sizeof(frame),81));device_unmap(f);
    assert(!f->s.tx_rate.valid && !f->s.rx_rate.valid && !f->s.tx_rate.observed_ms && !f->s.rx_rate.observed_ms);
    device_tx_rate(f,HAL_WBM_TQM_REL_REASON_FRAME_ACKED,HAL_WBM_REL_SRC_MODULE_TQM,
        HAL_TX_RATE_STATS_PKT_TYPE_11A,ATH12K_HW_RATE_OFDM_6M,HAL_TX_RATE_STATS_BW_20,1);
    assert(!gwd_poll(&f->s,&f->io,64) && !f->s.tx_pending && !f->s.tx_rate.valid && !f->s.rx_rate.valid);
    destroy(f);
    /* A successful selection and stop independently clear any retained sample
     * snapshot. Failed selections above must preserve the current peer's rate. */
    f=make();assert(!gwd_prepare(&f->s,&f->io) && !gwd_connect(&f->s,&f->io) && !gwd_start(&f->s,&f->io));
    f->s.tx_rate=tx;f->s.rx_rate=rx;assert(!gwd_select_peer(&f->s,0,0,0,ap,0));
    assert(!f->s.tx_rate.valid && !f->s.rx_rate.valid && !f->s.tx_rate.raw && !f->s.rx_rate.raw);
    f->s.tx_rate=tx;f->s.rx_rate=rx;assert(gwd_stop(&f->s,&f->io));
    assert(!f->s.tx_rate.valid && !f->s.rx_rate.valid && !f->s.tx_rate.kbps && !f->s.rx_rate.kbps);
    destroy(f);
}
static void device_remap(struct fixture *f) {
    uint8_t map[16]={0};gwq_w32(map,0x1e|(7u<<16));memcpy(map+4,ap,6);
    assert(!gwd_select_peer(&f->s,0,0,0,ap,1));assert(!gwd_receive_htt(&f->s,&f->io,7,map,sizeof(map)));
    for(unsigned tid=0;tid<17;tid++) {
        uint64_t address;assert(!gwd_reo_queue(&f->s,&f->io,tid,&address));assert(address==f->io.dma+GWD_QUEUES+tid*1024);
    }
}
/* Parse host commands with upstream types, execute a separate cache model,
 * then DMA a device status. Never use the driver's pending ID or phase to
 * synthesize the response. Source-tail visibility can lag status visibility. */
static int device_reo(struct fixture *f) {
    struct gwd_ring *cmd=&f->s.ring[GWD_REO_COMMAND],*status=&f->s.ring[GWD_REO_STATUS];
    unsigned tail=gwq_u32(f->ram+cmd->remote)/cmd->entry;
    if(tail==cmd->head || f->reo_deferred)return 0;
    assert(cmd->entry*4==sizeof(struct hal_tlv_64_hdr)+sizeof(struct hal_reo_get_queue_stats));
    const uint8_t *p=f->ram+cmd->desc+tail*cmd->entry*4;
    unsigned tag=value(HAL_TLV_HDR_TAG,gwq_u32(p));
    assert(value(HAL_TLV_HDR_LEN,gwq_u32(p))==sizeof(struct hal_reo_update_rx_queue));
    const uint8_t *body=p+sizeof(struct hal_tlv_64_hdr);
    unsigned id=value(HAL_REO_CMD_HDR_INFO0_CMD_NUMBER,gwq_u32(body+AT(hal_reo_update_rx_queue,cmd.info0)));
    assert(id && (gwq_u32(body)&HAL_REO_CMD_HDR_INFO0_STATUS_REQUIRED));
    unsigned sequence=f->reo_cycle_cmd++,tid=0,part=0,updates=0,commands=0;
    for(unsigned i=0;i<17;i++)if(f->reo_mask&(1u<<i)){updates++;commands+=i==16?2:5;}
    assert(sequence<commands);
    if(sequence<updates) {
        unsigned at=sequence;
        while(!(f->reo_mask&(1u<<tid)) || at){if(f->reo_mask&(1u<<tid))at--;tid++;assert(tid<17);}
        assert(tag==HAL_REO_UPDATE_RX_REO_QUEUE);
        assert(gwq_u32(body+AT(hal_reo_update_rx_queue,info0))==HAL_REO_UPD_RX_QUEUE_INFO0_UPD_VLD);
        assert(!(gwq_u32(body+AT(hal_reo_update_rx_queue,info1))&HAL_REO_UPD_RX_QUEUE_INFO1_VLD));
        assert(gwq_u32(f->ram+GWD_QUEUES+tid*1024+8)&1);
        f->reo_cached[tid]=tid==16?1:15;
        gwq_w32(f->ram+GWD_QUEUES+tid*1024+8,gwq_u32(f->ram+GWD_QUEUES+tid*1024+8)&~1u);
        if(sequence==updates-1)f->reo_invalidated_at=f->now;
    } else {
        assert(tag==HAL_REO_FLUSH_CACHE);unsigned at=sequence-updates;
        for(;;tid++) {
            assert(tid<17);if(!(f->reo_mask&(1u<<tid)))continue;
            unsigned lines=tid==16?1:4;if(at<lines){part=lines-1-at;break;}at-=lines;
        }
        assert(f->now-f->reo_invalidated_at>=1000);
        assert(!gwq_u32(body+AT(hal_reo_flush_cache,info0)));
        assert(f->reo_cached[tid]&(1u<<part));f->reo_cached[tid]&=(uint8_t)~(1u<<part);
    }
    assert(gwq_u32(body+AT(hal_reo_update_rx_queue,queue_addr_lo))==f->io.dma+GWD_QUEUES+tid*1024+part*128);
    unsigned fault=++f->reo_commands==f->reo_fault_at?f->reo_fault:0;
    unsigned head=gwq_u32(f->ram+status->remote)/status->entry;assert((head+1)%status->count!=status->tail);
    uint8_t *out=f->ram+status->desc+head*status->entry*4;memset(out,0,status->entry*4);
    unsigned reply=tag==HAL_REO_UPDATE_RX_REO_QUEUE?HAL_REO_UPDATE_RX_REO_QUEUE_STATUS:HAL_REO_FLUSH_CACHE_STATUS;
    unsigned bytes=tag==HAL_REO_UPDATE_RX_REO_QUEUE?sizeof(struct hal_reo_status_hdr):sizeof(struct hal_reo_flush_cache_status);
    if(fault==REO_WRONG_TAG)reply^=1;if(fault==REO_WRONG_ID)id=id==65535?1:id+1;
    if(fault==REO_SHORT_STATUS)bytes=4;if(fault==REO_LONG_STATUS)bytes=status->entry*4;
    if(fault==REO_SHORT_FLUSH)bytes=sizeof(struct hal_reo_flush_cache_status)-4;
    gwq_w64(out,field(HAL_TLV_HDR_TAG,reply)|field(HAL_TLV_HDR_LEN,bytes));
    gwq_w32(out+sizeof(struct hal_tlv_64_hdr)+AT(hal_reo_status_hdr,info0),
        field(HAL_REO_STATUS_HDR_INFO0_STATUS_NUM,id)|field(HAL_REO_STATUS_HDR_INFO0_EXEC_STATUS,
            fault==REO_EXEC_FAILED?2:fault==REO_EXEC_BLOCKED?1:fault==REO_RESOURCE_BLOCKED?3:0));
    uint32_t detail=0;
    if(fault==REO_FLUSH_BUSY || fault==REO_FLUSH_LOCKED || fault==REO_FLUSH_ALL_LOCKED)
        detail=field(HAL_REO_FLUSH_CACHE_STATUS_INFO0_FLUSH_ERR,
            fault==REO_FLUSH_BUSY?1:fault==REO_FLUSH_LOCKED?2:3);
    if(fault==REO_BLOCK_ERROR || fault==REO_UNUSED_BLOCK_DETAIL)
        detail=field(HAL_REO_FLUSH_CACHE_STATUS_INFO0_BLOCK_ERR_CODE,1)|
            (fault==REO_BLOCK_ERROR?HAL_REO_FLUSH_CACHE_STATUS_INFO0_IS_ERR:0);
    if(detail)gwq_w32(out+sizeof(struct hal_tlv_64_hdr)+AT(hal_reo_flush_cache_status,info0),detail);
    unsigned consumed=((tail+1)%cmd->count)*cmd->entry;
    if(f->reo_defer_tail)f->reo_deferred=consumed?consumed:UINT32_MAX;
    else gwq_w32(f->ram+cmd->remote,consumed);
    gwq_w32(f->ram+status->remote,((head+1)%status->count)*status->entry);
    if(f->reo_cycle_cmd==commands)f->reo_cycle_cmd=0;
    return 1;
}
static void device_reclaim_complete(struct fixture *f) {
    for(unsigned steps=0;steps<300;steps++) {
        int result=gwd_reo_reclaim(&f->s,&f->io,1);if(!result)return;assert(result==GWD_NOT_READY);
        f->now+=f->s.reclaim_phase==GWD_RECLAIM_AGE?1000:1;
        assert(!gwd_poll(&f->s,&f->io,128));(void)device_reo(f);
    }
    assert(!"REO reclaim did not complete");
}
static void queue_banks(void) {
    struct fixture *f=make();start(f,1);f->s.reo_cmd_seq=65530;
    assert(gwd_reo_reclaim(&f->s,&f->io,1)==GWD_NOT_READY && !f->s.reclaim_phase);
    memset(f->ram+GWD_MGMT,0xa5,GWD_MGMT_BYTES);
    for(unsigned cycle=0;cycle<300;cycle++) {
        assert(!gwd_key_ready(&f->s,0,1,1));device_unmap(f);
        assert(!f->s.peer_selected && !f->s.key_valid[4]);
        assert(gwd_reo_reclaim(&f->s,&f->io,0)==GWD_NOT_READY && !f->s.reclaim_phase);
        assert(gwd_select_peer(&f->s,0,0,0,ap,1)==GWD_NOT_READY);
        for(unsigned tid=0;tid<17;tid++)f->ram[GWD_QUEUES+tid*1024+100]=0x5a;
        device_reclaim_complete(f);
        assert(!f->s.queues && !f->s.next_queue_bank && !f->s.quarantine && f->reo_commands==(cycle+1)*82);
        for(unsigned tid=0;tid<17;tid++)assert(!f->reo_cached[tid] && f->ram[GWD_QUEUES+tid*1024+100]==0x5a);
        device_remap(f);
    }
    for(unsigned i=0;i<GWD_MGMT_BYTES;i++)assert(f->ram[GWD_MGMT+i]==0xa5);
    destroy(f);
}
static void reclaim_failures(void) {
    for(unsigned fault=1;fault<=REO_SHORT_FLUSH;fault++) {
        if(fault==REO_UNUSED_BLOCK_DETAIL)continue;
        struct fixture *f=make();start(f,0);device_unmap(f);
        f->reo_fault=fault;
        f->reo_fault_at=fault==REO_FLUSH_BUSY || fault==REO_BLOCK_ERROR?18:
            fault==REO_FLUSH_LOCKED || fault==REO_FLUSH_ALL_LOCKED || fault==REO_SHORT_FLUSH?82:1;
        assert(gwd_reo_reclaim(&f->s,&f->io,1)==GWD_NOT_READY);
        for(unsigned steps=0;steps<100 && !f->s.error;steps++) {
            f->now+=f->s.reclaim_phase==GWD_RECLAIM_AGE?1000:1;
            (void)gwd_poll(&f->s,&f->io,128);if(!f->s.error)(void)device_reo(f);
        }
        assert(f->s.error==GWD_FIRMWARE && f->s.quarantine && f->s.queues==0x1ffff && f->s.next_queue_bank);
        assert(gwd_reo_reclaim(&f->s,&f->io,1)==GWD_FIRMWARE);destroy(f);
    }
    struct fixture *f=make();start(f,0);device_unmap(f);
    /* BLOCK_ERR_CODE is explicitly undefined when IS_ERR is clear. */
    f->reo_fault=REO_UNUSED_BLOCK_DETAIL;f->reo_fault_at=18;
    device_reclaim_complete(f);assert(!f->s.quarantine && !f->s.queues);destroy(f);
    f=make();start(f,0);device_unmap(f);
    assert(gwd_reo_reclaim(&f->s,&f->io,1)==GWD_NOT_READY);assert(!gwd_poll(&f->s,&f->io,128));
    f->now+=GWD_REO_TIMEOUT_MS;assert(gwd_poll(&f->s,&f->io,128)==GWD_TIMEOUT && f->s.quarantine && f->s.queues);destroy(f);
    f=make();start(f,0);device_unmap(f);
    assert(gwd_reo_reclaim(&f->s,&f->io,1)==GWD_NOT_READY);assert(!gwd_poll(&f->s,&f->io,128));
    for(unsigned i=0;i<10;i++){f->now+=100;assert(!gwd_poll(&f->s,&f->io,128) && f->s.reo_pending && f->s.queues);}
    f->reo_defer_tail=1;assert(device_reo(f));assert(!gwd_poll(&f->s,&f->io,128) && f->s.reo_ack);
    assert(f->s.reclaim_tid==0 && f->s.reo_pending && gwd_select_peer(&f->s,0,0,0,ap,1)==GWD_NOT_READY);
    gwq_w32(f->ram+f->s.ring[GWD_REO_COMMAND].remote,f->reo_deferred==UINT32_MAX?0:f->reo_deferred);
    f->reo_deferred=f->reo_defer_tail=0;device_reclaim_complete(f);assert(!f->s.queues);destroy(f);
}
static void reclaim_visibility(void) {
    /* Source-tail movement alone does not prove successful execution. */
    struct fixture *f=make();start(f,0);device_unmap(f);
    assert(gwd_reo_reclaim(&f->s,&f->io,1)==GWD_NOT_READY);
    assert(!gwd_poll(&f->s,&f->io,128));assert(device_reo(f));
    struct gwd_ring *status=&f->s.ring[GWD_REO_STATUS];unsigned producer=gwq_u32(f->ram+status->remote);
    gwq_w32(f->ram+status->remote,0);f->now+=1000;
    assert(!gwd_poll(&f->s,&f->io,128));
    assert(f->s.reo_pending && !f->s.reo_ack && !f->s.reclaim_tid && f->reo_commands==1 && f->s.queues);
    gwq_w32(f->ram+status->remote,producer);device_reclaim_complete(f);destroy(f);
    /* An acknowledgement alone cannot recycle the command slot. If its
     * source tail never becomes visible, retain DMA ownership after timeout. */
    f=make();start(f,0);device_unmap(f);f->reo_defer_tail=1;
    assert(gwd_reo_reclaim(&f->s,&f->io,1)==GWD_NOT_READY);
    assert(!gwd_poll(&f->s,&f->io,128));assert(device_reo(f));assert(!gwd_poll(&f->s,&f->io,128));
    assert(f->s.reo_ack && f->s.reo_pending && !f->s.reclaim_tid);
    f->now=f->s.reo_since+GWD_REO_TIMEOUT_MS-1;
    assert(!gwd_poll(&f->s,&f->io,128));f->ram[GWD_QUEUES+100]=0x5a;f->now++;
    assert(gwd_poll(&f->s,&f->io,128)==GWD_TIMEOUT);
    gwq_w32(f->ram+f->s.ring[GWD_REO_COMMAND].remote,f->reo_deferred);
    assert(gwd_reo_reclaim(&f->s,&f->io,1)==GWD_TIMEOUT);
    assert(f->s.quarantine && f->s.queues==0x1ffff && f->ram[GWD_QUEUES+100]==0x5a);
    f->not_idle=1;assert(!gwd_stop(&f->s,&f->io));
    assert(f->s.prepared && f->s.quarantine && f->s.queues==0x1ffff);destroy(f);
    /* Even a matching success first observed at the deadline is too late to
     * recover a timed-out association or permit queue reuse. */
    f=make();start(f,0);device_unmap(f);
    assert(gwd_reo_reclaim(&f->s,&f->io,1)==GWD_NOT_READY);assert(!gwd_poll(&f->s,&f->io,128));
    f->now=f->s.reo_since+GWD_REO_TIMEOUT_MS;assert(device_reo(f));
    assert(gwd_poll(&f->s,&f->io,128)==GWD_TIMEOUT && f->s.queues && f->s.quarantine);destroy(f);
    /* All invalidations must finish before the full aging interval begins. */
    f=make();start(f,0);device_unmap(f);assert(gwd_reo_reclaim(&f->s,&f->io,1)==GWD_NOT_READY);
    while(f->reo_commands<17){assert(!gwd_poll(&f->s,&f->io,128));assert(device_reo(f));f->now++;}
    assert(!gwd_poll(&f->s,&f->io,128));assert(f->s.reclaim_phase==GWD_RECLAIM_AGE);
    f->now+=GWD_REO_AGE_MS-1;assert(!gwd_poll(&f->s,&f->io,128));
    assert(f->s.reclaim_phase==GWD_RECLAIM_AGE && !f->s.reo_pending && f->reo_commands==17);
    f->now++;assert(!gwd_poll(&f->s,&f->io,128));assert(f->s.reclaim_phase==GWD_RECLAIM_FLUSH);
    device_reclaim_complete(f);destroy(f);
}
static void device_address(uint8_t out[8],uint64_t address,unsigned rbm,unsigned cookie) {
    gwq_w32(out+AT(ath12k_buffer_addr,info0),(uint32_t)address);
    gwq_w32(out+AT(ath12k_buffer_addr,info1),(uint32_t)(address>>32)|
        field(BUFFER_ADDR_INFO1_RET_BUF_MGR,rbm)|field(BUFFER_ADDR_INFO1_SW_COOKIE,cookie));
}
/* Device routes an owned RX buffer to an error ring instead of normal REO. */
static unsigned device_error_buffer(struct fixture *f,uint8_t address[8]) {
    struct gwd_ring *r=&f->s.ring[GWD_REO];unsigned head=gwq_u32(f->ram+r->remote)/r->entry;
    unsigned slot=device_rx(f,0,0,0,0);memcpy(address,f->ram+r->desc+head*sizeof(struct hal_reo_dest_ring),8);
    gwq_w32(f->ram+r->remote,head*r->entry);return slot;
}
static void error_recycling(void) {
    struct fixture *f=make();start(f,0);uint8_t address[8];
    unsigned slot=device_error_buffer(f,address);struct gwd_ring *r=&f->s.ring[GWD_RX_ERROR];
    uint8_t *d=f->ram+r->desc;memcpy(d,address,8);gwq_w32(d+AT(hal_wbm_release_ring,info0),1);
    gwq_w32(f->ram+r->remote,r->entry);assert(!gwd_poll(&f->s,&f->io,64));
    assert(f->s.rx_dropped==1 && f->s.rx_owned[slot] && !f->s.quarantine);destroy(f);
    for(unsigned which=0;which<2;which++) {
        f=make();start(f,0);unsigned slots[2];
        for(unsigned i=0;i<2;i++) {
            uint8_t *link=f->ram+GWD_LINK_DESC+i*sizeof(struct hal_rx_msdu_link);
            slots[i]=device_error_buffer(f,address);
            memcpy(link+AT(hal_rx_msdu_link,msdu_link[0].buf_addr_info),address,8);
            if(!i)device_address(link+AT(hal_rx_msdu_link,buf_addr_info),f->io.dma+GWD_LINK_DESC+sizeof(struct hal_rx_msdu_link),1,1);
        }
        r=&f->s.ring[which?GWD_REO_ERROR:GWD_RXDMA_ERROR];d=f->ram+r->desc;
        device_address(d,f->io.dma+GWD_LINK_DESC,1,0);
        if(which)gwq_w32(d+AT(hal_reo_dest_ring,info0),HAL_REO_DEST_RING_INFO0_BUFFER_TYPE);
        gwq_w32(f->ram+r->remote,r->entry);assert(!gwd_poll(&f->s,&f->io,64));
        assert(f->s.rx_dropped==2 && f->s.rx_owned[slots[0]] && f->s.rx_owned[slots[1]]);
        struct gwd_ring *release=&f->s.ring[GWD_LINK_RELEASE];assert(release->head==2);
        for(unsigned i=0;i<2;i++) {
            const uint8_t *entry=f->ram+release->desc+i*sizeof(struct hal_wbm_release_ring);
            assert(gwq_u32(entry)==f->io.dma+GWD_LINK_DESC+i*sizeof(struct hal_rx_msdu_link));
            assert(gwq_u32(entry+AT(hal_wbm_release_ring,info0))==(4u|(1u<<6)));
        }
        destroy(f);
    }
    f=make();start(f,0);slot=device_error_buffer(f,address);
    uint8_t *link=f->ram+GWD_LINK_DESC;
    memcpy(link+AT(hal_rx_msdu_link,msdu_link[0].buf_addr_info),address,8);
    device_address(link+AT(hal_rx_msdu_link,buf_addr_info),f->io.dma+GWD_LINK_DESC,1,0);
    r=&f->s.ring[GWD_RXDMA_ERROR];device_address(f->ram+r->desc,f->io.dma+GWD_LINK_DESC,1,0);
    gwq_w32(f->ram+r->remote,r->entry);
    assert(gwd_poll(&f->s,&f->io,64)==GWD_COOKIE && f->s.rx_owned[slot] && !f->s.rx_dropped);
    assert(!f->s.ring[GWD_LINK_RELEASE].head);destroy(f);
}
static void reclaim_lifecycle(void) {
    struct fixture *f=make();start(f,0);uint8_t frame[60]={0};frame[6]=2;frame[12]=8;
    assert(!gwd_send(&f->s,&f->io,frame,sizeof(frame),55));device_unmap(f);
    assert(gwd_reo_reclaim(&f->s,&f->io,1)==GWD_NOT_READY);assert(!gwd_poll(&f->s,&f->io,128));
    assert(f->s.reclaim_phase==GWD_RECLAIM_TX && !f->s.reo_pending && !f->reo_commands);
    device_tx(f,0);assert(!gwd_poll(&f->s,&f->io,128));assert(f->completed==1 && f->s.reo_pending);
    device_reclaim_complete(f);destroy(f);
    f=make();start(f,0);assert(!gwd_send(&f->s,&f->io,frame,sizeof(frame),56));device_unmap(f);
    assert(gwd_reo_reclaim(&f->s,&f->io,1)==GWD_NOT_READY);f->now+=GWD_REO_TIMEOUT_MS;
    assert(gwd_poll(&f->s,&f->io,128)==GWD_TIMEOUT && f->s.tx_pending && f->s.queues);destroy(f);
    /* A stale duplicate cannot acknowledge the next command, including after
     * the status ring entry has already been retired and reused. */
    f=make();start(f,0);device_unmap(f);assert(gwd_reo_reclaim(&f->s,&f->io,1)==GWD_NOT_READY);
    assert(!gwd_poll(&f->s,&f->io,128));assert(device_reo(f));
    struct gwd_ring *status=&f->s.ring[GWD_REO_STATUS];uint8_t stale[112];memcpy(stale,f->ram+status->desc,sizeof(stale));
    assert(!gwd_poll(&f->s,&f->io,128));assert(f->s.reo_pending==2);
    memcpy(f->ram+status->desc+sizeof(stale),stale,sizeof(stale));gwq_w32(f->ram+status->remote,2*status->entry);
    assert(gwd_poll(&f->s,&f->io,128)==GWD_FIRMWARE && f->s.queues && f->s.quarantine);destroy(f);
    f=make();start(f,0);device_unmap(f);assert(gwd_reo_reclaim(&f->s,&f->io,1)==GWD_NOT_READY);
    assert(!gwd_poll(&f->s,&f->io,128));gwq_w32(f->ram+f->s.ring[GWD_REO_COMMAND].remote,3);
    assert(gwd_poll(&f->s,&f->io,128)==GWD_POINTER && f->s.queues);destroy(f);
    /* Leave the final flush status unconsumed, then inject old-peer packets
     * and a link-descriptor error. Neither may survive into the next peer. */
    f=make();start(f,0);device_unmap(f);assert(gwd_reo_reclaim(&f->s,&f->io,1)==GWD_NOT_READY);
    for(unsigned steps=0;f->reo_commands<82;steps++) {
        assert(steps<200);f->now+=f->s.reclaim_phase==GWD_RECLAIM_AGE?1000:1;
        assert(!gwd_poll(&f->s,&f->io,128));(void)device_reo(f);
    }
    uint8_t address[8];(void)device_error_buffer(f,address);
    memcpy(f->ram+GWD_LINK_DESC+AT(hal_rx_msdu_link,msdu_link[0].buf_addr_info),address,8);
    struct gwd_ring *error=&f->s.ring[GWD_RXDMA_ERROR];device_address(f->ram+error->desc,f->io.dma+GWD_LINK_DESC,1,0);
    gwq_w32(f->ram+error->remote,error->entry);
    for(unsigned i=0;i<3;i++)device_rx(f,0,0,0,0);
    assert(!gwd_poll(&f->s,&f->io,1));assert(f->s.reclaim_phase==GWD_RECLAIM_DRAIN && f->s.queues && !f->received);
    assert(gwd_reo_reclaim(&f->s,&f->io,1)==GWD_NOT_READY);
    assert(!gwd_poll(&f->s,&f->io,128));assert(f->s.queues && f->s.rx_dropped==4 && !f->received);
    struct gwd_ring *release=&f->s.ring[GWD_LINK_RELEASE];assert(release->head==1);
    gwq_w32(f->ram+release->remote,release->head*release->entry);
    assert(!gwd_poll(&f->s,&f->io,128));assert(!gwd_reo_reclaim(&f->s,&f->io,1) && !f->s.queues);destroy(f);
    /* A failed partial setup needs only the TIDs actually handed to firmware. */
    f=make();assert(!gwd_prepare(&f->s,&f->io));assert(!gwd_connect(&f->s,&f->io));assert(!gwd_start(&f->s,&f->io));
    assert(!gwd_select_peer(&f->s,0,0,0,ap,0));uint8_t map[16]={0};gwq_w32(map,0x1e|(7u<<16));memcpy(map+4,ap,6);
    assert(!gwd_receive_htt(&f->s,&f->io,7,map,sizeof(map)));uint64_t dma;
    assert(!gwd_reo_queue(&f->s,&f->io,5,&dma));assert(!gwd_reo_queue(&f->s,&f->io,16,&dma));
    f->reo_mask=(1u<<5)|(1u<<16);device_unmap(f);device_reclaim_complete(f);
    assert(f->reo_commands==7 && !f->s.queues && !f->reo_cached[5] && !f->reo_cached[16]);destroy(f);
    f=make();assert(!gwd_prepare(&f->s,&f->io));assert(!gwd_connect(&f->s,&f->io));assert(!gwd_start(&f->s,&f->io));
    device_rx(f,0,0,0,0);assert(gwd_reo_reclaim(&f->s,&f->io,1)==GWD_NOT_READY);
    assert(!gwd_poll(&f->s,&f->io,128));assert(!gwd_reo_reclaim(&f->s,&f->io,1) && !f->reo_commands && f->s.rx_dropped==1);destroy(f);
}
static void registers(void) {
    struct fixture *f=make();f->misc_status=0x80;start(f,0);
    assert(f->s.ring[GWD_TCL].reg==HAL_SEQ_WCSS_UMAC_TCL_REG+HAL_TCL1_RING_BASE_LSB);
    assert(f->s.ring[GWD_TCL].ptr==HAL_SEQ_WCSS_UMAC_TCL_REG+HAL_TCL1_RING_HP);
    assert(f->s.ring[GWD_REO].reg==HAL_SEQ_WCSS_UMAC_REO_REG+ORACLE_hal_reo1_ring_base_lsb);
    assert(f->s.ring[GWD_REO_ERROR].reg==HAL_SEQ_WCSS_UMAC_REO_REG+ORACLE_hal_reo2_sw0_ring_base);
    assert(f->s.ring[GWD_TX_COMPLETE].reg==HAL_SEQ_WCSS_UMAC_WBM_REG+ORACLE_hal_wbm0_release_ring_base_lsb);
    assert(f->s.ring[GWD_LINK_RELEASE].reg==HAL_SEQ_WCSS_UMAC_WBM_REG+ORACLE_hal_wbm_sw_release_ring_base_lsb);
    assert(f->s.ring[GWD_REO_COMMAND].reg==HAL_SEQ_WCSS_UMAC_REO_REG+ORACLE_hal_reo_cmd_ring_base);
    assert(f->s.ring[GWD_REO_COMMAND].ptr==HAL_SEQ_WCSS_UMAC_REO_REG+HAL_REO_CMD_HP);
    assert(f->s.ring[GWD_REO_STATUS].reg==HAL_SEQ_WCSS_UMAC_REO_REG+ORACLE_hal_reo_status_ring_base);
    assert(f->s.ring[GWD_REO_STATUS].ptr==HAL_SEQ_WCSS_UMAC_REO_REG+HAL_REO_STATUS_HP);
    uint32_t bank=field(HAL_TX_BANK_CONFIG_ENCAP_TYPE,2)|HAL_TX_BANK_CONFIG_ADDRY_EN|HAL_TX_BANK_CONFIG_VDEV_ID_CHECK_EN;
    assert(readreg(f,HAL_TCL_SW_CONFIG_BANK_ADDR)==bank);
    assert(readreg(f,HAL_TCL_SW_CONFIG_BANK_ADDR+4)==
        (field(HAL_TX_BANK_CONFIG_ENCAP_TYPE,1)|HAL_TX_BANK_CONFIG_ADDRY_EN|HAL_TX_BANK_CONFIG_VDEV_ID_CHECK_EN));destroy(f);
}
static void encapsulation(void) {
    struct fixture *f=make();start(f,1);uint8_t frame[1518];
    for(unsigned i=0;i<sizeof(frame);i++)frame[i]=(uint8_t)i;
    frame[0]=2;frame[6]=2;frame[12]=0x88;frame[13]=0x8e;
    for(unsigned installed=0;installed<2;installed++) {
        if(installed){assert(!gwd_key_ready(&f->s,0,1,0));assert(!gwd_key_ready(&f->s,1,0,0));assert(!gwd_authorize(&f->s,1));}
        for(unsigned size=0;size<3;size++) {
            const unsigned lengths[3]={14,99,1518};unsigned n=lengths[size];
            struct gwd_ring *r=&f->s.ring[GWD_TCL];unsigned at=r->head;
            assert(!gwd_send(&f->s,&f->io,frame,n,100+installed*3+size));
            const uint8_t *d=f->ram+r->desc+at*sizeof(struct hal_tcl_data_cmd);
            assert(value(HAL_TCL_DATA_CMD_INFO0_BANK_ID,gwq_u32(d+AT(hal_tcl_data_cmd,info0)))==1);
            assert(value(HAL_TCL_DATA_CMD_INFO2_DATA_LEN,gwq_u32(d+AT(hal_tcl_data_cmd,info2)))==n+18);
            const uint8_t *p=f->ram+(gwq_u32(d)-f->io.dma);
            assert(gwq_u16(p)==0x0108 && !memcmp(p+4,ap,6));
            assert(!memcmp(p+10,frame+6,6) && !memcmp(p+16,frame,6));
            assert(!memcmp(p+32,frame+14,n-14));
        }
    }
    /* Non-EAPOL after authorization keeps the Ethernet bank and exact bytes. */
    frame[12]=8;frame[13]=0;struct gwd_ring *r=&f->s.ring[GWD_TCL];unsigned at=r->head;
    assert(!gwd_send(&f->s,&f->io,frame,sizeof(frame),200));
    const uint8_t *d=f->ram+r->desc+at*sizeof(struct hal_tcl_data_cmd);
    assert(!value(HAL_TCL_DATA_CMD_INFO0_BANK_ID,gwq_u32(d+AT(hal_tcl_data_cmd,info0))));
    assert(value(HAL_TCL_DATA_CMD_INFO2_DATA_LEN,gwq_u32(d+AT(hal_tcl_data_cmd,info2)))==sizeof(frame));
    assert(!memcmp(f->ram+(gwq_u32(d)-f->io.dma),frame,sizeof(frame)));destroy(f);
}
int main(void) {
    normal();observed_rates();observed_rate_lifetime();firmware_completions();open_mcbc();security();group_keys();failures();lifetime();queue_banks();reclaim_failures();reclaim_visibility();error_recycling();reclaim_lifecycle();registers();encapsulation();
    printf("Wi-Fi DP: %u initialization/failure cases, upstream layout/HAL/rate oracle, observed TX/RX rates, 2000 TCL/WBM + RXDMA/REO recycles, 300 REO invalidate/flush/reconnect cycles, FW TX ownership, MCBC, error-link recycling, native EAPOL, controlled port and per-GTK/TID CCMP replay checks passed\n",checks);
    return 0;
}
