/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_MODEM_CONTRACT_H
#define GIULIA_MODEM_CONTRACT_H
#include <stddef.h>
#include <stdint.h>
#include "giulia_modem_qmi.h"

/* Host ABI, never an Apple UserClient or a QMI wire structure. All entrypoints
 * run on one serial executor. submit accepts work; only a matching event may
 * complete it. Providers must copy actions before submit returns. */
#define GMR_ABI_VERSION 1u
#define GMR_SLOTS 2u
#define GMR_BEARERS 8u
#define GMR_SERVICES 24u
#define GMR_PENDING 32u
#define GMR_EVENTS 32u
#define GMR_STORAGE_CACHE 16u
#define GMR_PAYLOAD 2048u
#define GMR_NO_SLOT 255u
#define GMR_NO_BEARER 255u
#define GMR_TIMEOUT_MS UINT64_C(30000)
#define GMR_IPA_AP_READY 1u
#define GMR_IPA_Q6_READY 2u
#define GMR_IPA_MAP_VALID 4u
#define GMR_WDA_RAW_IP 1u
#define GMR_WDA_QMAP 2u
#define GMR_BIND_SUBSCRIPTION 1u
#define GMR_BIND_MUX 2u
#define GMR_BIND_ENDPOINT 4u
#define GMR_DMA_IOVA 1u
#define GMR_DMA_GSI 2u
#define GMR_DMA_ENDPOINT 4u
#define GMR_DMA_RX 8u
#define GMR_DMA_TX 16u
#define GMR_NET_IFNET 1u
#define GMR_NET_ADDRESS 2u
#define GMR_NET_ROUTE 4u
#define GMR_NET_DNS 8u
#define GMR_REMOVE_NETWORK 1u
#define GMR_REMOVE_RX_DRAINED 2u
#define GMR_REMOVE_TX_DRAINED 4u
#define GMR_REMOVE_DMA_STOPPED 8u
#define GMR_REMOVE_COMPLETE 15u

enum gmr_result { GMR_OK=0, GMR_ARGUMENT=1, GMR_STATE=2, GMR_BUSY=3,
    GMR_FULL=4, GMR_STALE=5, GMR_PROTOCOL=6, GMR_REMOTE=7,
    GMR_TIMEOUT=8, GMR_UNSUPPORTED=9, GMR_IO=10, GMR_CANCELLED=11,
    GMR_UNCERTAIN=12 };
enum gmr_lifecycle { GMR_OFF, GMR_ACQUIRING, GMR_STORAGE_STARTING,
    GMR_BOOTING, GMR_IPC_STARTING, GMR_ONLINE, GMR_SUSPENDING,
    GMR_SUSPENDED, GMR_RESUMING, GMR_STOPPING, GMR_ISOLATED };
enum gmr_sim_state { GMR_SIM_UNKNOWN, GMR_SIM_ABSENT, GMR_SIM_LOCKED,
    GMR_SIM_READY, GMR_SIM_ERROR };
enum gmr_registration { GMR_REG_UNKNOWN, GMR_REG_SEARCHING,
    GMR_REG_REGISTERED, GMR_REG_DENIED, GMR_REG_OFFLINE };
enum gmr_bearer_state { GMR_BEARER_DOWN, GMR_BEARER_STARTING,
    GMR_BEARER_SETTINGS, GMR_BEARER_CONFIGURING, GMR_BEARER_UP,
    GMR_BEARER_STOPPING, GMR_BEARER_UNCERTAIN };
enum gmr_ims_state { GMR_IMS_OFF, GMR_IMS_REGISTERING, GMR_IMS_REGISTERED,
    GMR_IMS_FAILED };
enum gmr_call_state { GMR_CALL_IDLE, GMR_CALL_STARTING,
    GMR_CALL_NETWORK_ACTIVE, GMR_CALL_ACTIVE, GMR_CALL_STOPPING,
    GMR_CALL_UNCERTAIN };
enum gmr_sms_state { GMR_SMS_IDLE, GMR_SMS_SUBMITTING, GMR_SMS_ACCEPTED,
    GMR_SMS_FAILED, GMR_SMS_UNCERTAIN, GMR_SMS_DELIVERED };

enum gmr_command_kind { GMR_CMD_STOP=1, GMR_CMD_SUSPEND, GMR_CMD_RESUME,
    GMR_CMD_SSR, GMR_CMD_QUERY_SIM, GMR_CMD_QUERY_REGISTRATION,
    GMR_CMD_CONNECT, GMR_CMD_DISCONNECT, GMR_CMD_STORAGE_WRITE,
    GMR_CMD_STORAGE_FLUSH, GMR_CMD_IMS_REGISTER, GMR_CMD_VOICE_START,
    GMR_CMD_VOICE_STOP, GMR_CMD_SMS_SEND, GMR_CMD_SELECT_SLOT,
    GMR_CMD_PACKET_TX, GMR_CMD_RF_MODE };
enum gmr_action_kind { GMR_ACT_NONE, GMR_ACT_RESOURCES_ACQUIRE,
    GMR_ACT_STORAGE_START, GMR_ACT_FIRMWARE_BOOT, GMR_ACT_IPC_START,
    GMR_ACT_QMI, GMR_ACT_DATA_CONFIGURE, GMR_ACT_DATA_REMOVE,
    GMR_ACT_STORAGE_WRITE, GMR_ACT_STORAGE_FLUSH, GMR_ACT_IMS_REGISTER,
    GMR_ACT_VOICE_START, GMR_ACT_VOICE_STOP, GMR_ACT_SMS_SEND,
    GMR_ACT_MEDIA_START, GMR_ACT_MEDIA_DRAIN, GMR_ACT_DATA_DRAIN,
    GMR_ACT_IPC_STOP, GMR_ACT_FIRMWARE_STOP, GMR_ACT_MEMORY_RECLAIM,
    GMR_ACT_STORAGE_STOP, GMR_ACT_RESOURCES_RELEASE,
    GMR_ACT_SUSPEND, GMR_ACT_RESUME, GMR_ACT_SELECT_SLOT,
    GMR_ACT_DATA_TX, GMR_ACT_RF_MODE, GMR_ACT_IPA_PREPARE,
    GMR_ACT_WDA_FORMAT, GMR_ACT_WDS_BIND, GMR_ACT_DMA_CONFIGURE,
    GMR_ACT_NETWORK_CONFIGURE, GMR_ACT_IPC_PREPARE, GMR_ACT_STORAGE_REPLY };
enum gmr_event_kind { GMR_EV_COMPLETE=1, GMR_EV_QMI_RESPONSE,
    GMR_EV_SERVICE_UP, GMR_EV_SERVICE_DOWN, GMR_EV_SIM_REMOVED,
    GMR_EV_FATAL, GMR_EV_STORAGE_REQUEST, GMR_EV_REGISTRATION_LOST,
    GMR_EV_DATA_COMPLETION, GMR_EV_DATA_RX, GMR_EV_IMS_STATUS,
    GMR_EV_SMS_DELIVERED, GMR_EV_VOICE_ENDED, GMR_EV_SIM_CHANGED,
    GMR_EV_NAS_CHANGED, GMR_EV_DATA_ERROR, GMR_EV_DATA_DELIVERED };
enum gmr_qmi_kind { GMR_QMI_NONE, GMR_QMI_SIM, GMR_QMI_REGISTRATION,
    GMR_QMI_START, GMR_QMI_SETTINGS, GMR_QMI_STOP };

struct gmr_identity {
    uint64_t epoch,opid;
    uint32_t slot_generation,endpoint_generation,bearer_generation,node,port;
    uint16_t service,txn;
    uint8_t slot,bearer;
};
struct gmr_command {
    uint32_t kind;
    uint64_t opid; /* zero allocates; nonzero must exceed all earlier ids */
    uint8_t slot,bearer,ip_family;
    uint16_t bytes;
    uint32_t object;
    uint64_t offset;
    char apn[GMQ_MAX_APN+1u];
    uint8_t payload[GMR_PAYLOAD];
};
struct gmr_ipv6 {
    uint32_t present,mtu;
    uint8_t address[16],gateway[16],dns1[16],dns2[16];
    uint8_t prefix,gateway_prefix;
};
struct gmr_action {
    uint32_t kind,qmi_kind;
    struct gmr_identity id;
    /* Reverse request identity, preserved WRITE -> FLUSH -> REPLY. Zero for
     * local requests. peer.opid is a provider-assigned monotonic cookie per
     * endpoint, stable across retransmission of one logical wire request. */
    struct gmr_identity peer;
    uint64_t deadline_ms,offset;
    uint32_t object,handle;
    uint16_t bytes;
    uint8_t ip_family,mux;
    struct gmq_ipv4 ipv4;
    struct gmr_ipv6 ipv6;
    uint8_t payload[GMR_PAYLOAD];
};
struct gmr_event {
    uint32_t kind,action_kind;
    struct gmr_identity id;
    int32_t status; /* a gmr_result; no unvalidated modem result */
    uint32_t value; /* write bytes, media/network session or DMA byte count */
    uint32_t flags; /* NAS: bit0 packet attached, bit1 roaming, bit2 roaming known */
    uint64_t offset;
    uint16_t bytes;
    uint8_t payload[GMR_PAYLOAD];
};
struct gmr_slot {
    uint32_t generation,sim,registration,ims,call,sms;
    uint32_t call_id,sms_id;
    uint8_t packet_attached,roaming,roaming_known;
};
struct gmr_bearer {
    uint32_t state,handle,slot_generation,endpoint_generation,generation;
    uint8_t slot,mux,ip_family;
    uint32_t ipa_flags,wda_flags,bind_flags,dma_flags,network_flags;
    char apn[GMQ_MAX_APN+1u];
    struct gmq_ipv4 ipv4;
    struct gmr_ipv6 ipv6;
    uint64_t rx_bytes,tx_bytes,rx_delivered;
};
struct gmr_service {
    uint16_t service;
    uint8_t slot,available;
    uint32_t generation,node,port;
    uint64_t storage_highwater;
};
struct gmr_snapshot {
    uint32_t abi,lifecycle,last_error,pending_count,queued_events;
    uint64_t epoch,last_opid,now_ms,stale_events,protocol_errors;
    uint64_t fault_epoch,fault_count;
    uint32_t last_fault; /* survives recovery; last_error describes current work */
    uint32_t resources_owned,storage_running,firmware_running,ipc_running,ipc_prepared;
    uint32_t storage_dirty,storage_writes,storage_durable,uncertain_actions;
    uint8_t selected_slot,restart_requested,rf_enabled,rf_mode_known;
    struct gmr_slot slots[GMR_SLOTS];
    struct gmr_bearer bearers[GMR_BEARERS];
    struct gmr_service services[GMR_SERVICES];
};
struct gmr_ops {
    void *ctx;
    int (*submit)(void *ctx,const struct gmr_action *action);
    /* Borrowed read-only snapshot on the serial executor. Never reenter. */
    void (*changed)(void *ctx,const struct gmr_snapshot *snapshot);
    /* Framing-checked RX bytes, borrowed until return. Zero means copied into
     * the receiver's bounded queue. DATA_DELIVERED/DATA_ERROR with this exact
     * identity must follow; acceptance is not ifnet delivery. */
    int (*packet)(void *ctx,const struct gmr_identity *id,const uint8_t *bytes,size_t size);
    /* Optional resident pure-parser adapter. Called after exact pending/source
     * checks, on the serial executor. It borrows action/event for this call
     * only, must not reenter the core, and returns the scalar proto verdict.
     * The adapter may copy bytes to an authenticated hotload request, never
     * publish these pointers. No hardware/resource effects are permitted. */
    uint64_t (*qmi_check)(void *ctx,const struct gmr_action *,const struct gmr_event *);
};
struct gmr_pending {
    uint8_t used;
    struct gmr_action action;
};
struct gmr_rx_pending {
    uint8_t used;
    uint16_t bytes;
    struct gmr_identity id;
};
enum gmr_storage_state { GMR_STORAGE_UNUSED, GMR_STORAGE_WRITING,
    GMR_STORAGE_FLUSHING, GMR_STORAGE_REPLYING, GMR_STORAGE_COMPLETE };
struct gmr_storage_record {
    uint32_t state,object,reply_status;
    uint64_t offset;
    uint16_t bytes;
    struct gmr_identity peer;
    uint8_t payload[GMR_PAYLOAD];
};
struct gmr_runtime {
    struct gmr_ops ops;
    struct gmr_snapshot state;
    struct gmr_pending pending[GMR_PENDING];
    struct gmr_event events[GMR_EVENTS];
    struct gmr_rx_pending rx_pending[GMR_EVENTS];
    struct gmr_storage_record storage[GMR_STORAGE_CACHE];
    uint32_t head,count,dispatching,teardown_stage,unresolved_startup;
    uint64_t next_opid;
    uint16_t next_txn;
};

/* Definitions are in giulia_modem_runtime.h. The core owns no heap, locks,
 * threads or MMIO. Each caller must use the same serial executor. */
#endif
