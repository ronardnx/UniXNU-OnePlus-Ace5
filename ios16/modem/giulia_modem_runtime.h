/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_MODEM_RUNTIME_H
#define GIULIA_MODEM_RUNTIME_H
#include "giulia_modem_contract.h"
#include "giulia_modem_ip.h"

/* One production state machine for IOKit and deterministic integration runs.
 * No provider can advance state by returning success from submit(). */
static inline int gmr_event(struct gmr_runtime *,const struct gmr_event *);
static inline int gmr_start(struct gmr_runtime *);
static inline int gmr_begin_stop(struct gmr_runtime *,int);
static inline int gmr_teardown_next(struct gmr_runtime *);
static inline int gmr_drain(struct gmr_runtime *);
static inline void gmr_invalidate(struct gmr_runtime *);

static inline void gmr_notify(struct gmr_runtime *r) {
    r->state.queued_events=r->count;
    if(r->ops.changed){r->dispatching++;r->ops.changed(r->ops.ctx,&r->state);r->dispatching--;}
}
static inline int gmr_init(struct gmr_runtime *r,const struct gmr_ops *ops) {
    unsigned i;
    if(!r || !ops || !ops->submit)return GMR_ARGUMENT;
    gmq_zero(r,sizeof(*r));r->ops=*ops;r->state.abi=GMR_ABI_VERSION;
    r->state.lifecycle=GMR_OFF;r->next_opid=1;r->next_txn=1;
    for(i=0;i<GMR_SLOTS;i++)r->state.slots[i].generation=1;
    for(i=0;i<GMR_BEARERS;i++)r->state.bearers[i].slot=GMR_NO_SLOT;
    return GMR_OK;
}
static inline int gmr_snapshot(const struct gmr_runtime *r,struct gmr_snapshot *out) {
    if(!r || !out)return GMR_ARGUMENT;
    *out=r->state;out->queued_events=r->count;return GMR_OK;
}
static inline int gmr_identity_equal(const struct gmr_identity *a,const struct gmr_identity *b) {
    return a->epoch==b->epoch && a->opid==b->opid && a->slot==b->slot &&
        a->bearer==b->bearer && a->slot_generation==b->slot_generation &&
        a->endpoint_generation==b->endpoint_generation && a->bearer_generation==b->bearer_generation && a->node==b->node &&
        a->port==b->port && a->service==b->service && a->txn==b->txn;
}
static inline struct gmr_storage_record *gmr_storage_find(struct gmr_runtime *r,const struct gmr_identity *peer) {
    unsigned i;if(!peer->epoch)return NULL;
    for(i=0;i<GMR_STORAGE_CACHE;i++)if(r->storage[i].state &&
        gmr_identity_equal(&r->storage[i].peer,peer))return &r->storage[i];
    return NULL;
}
static inline struct gmr_service *gmr_service_find(struct gmr_runtime *r,uint16_t service,uint8_t slot) {
    unsigned i;
    for(i=0;i<GMR_SERVICES;i++)if(r->state.services[i].service==service &&
        r->state.services[i].slot==slot && r->state.services[i].generation)
        return &r->state.services[i];
    return NULL;
}
static inline int gmr_action_pending(const struct gmr_runtime *r,uint32_t kind) {
    unsigned i;for(i=0;i<GMR_PENDING;i++)if(r->pending[i].used &&
        r->pending[i].action.kind==kind)return 1;
    return 0;
}
static inline int gmr_op_pending(const struct gmr_runtime *r,uint64_t opid) {
    unsigned i;for(i=0;i<GMR_PENDING;i++)if(r->pending[i].used &&
        r->pending[i].action.id.opid==opid)return 1;
    return 0;
}
static inline uint64_t gmr_operation(struct gmr_runtime *r,uint64_t requested) {
    uint64_t id=requested;
    if(id) {
        if(id<r->next_opid || id==UINT64_MAX || gmr_op_pending(r,id))return 0;
        r->next_opid=id+1;
    }
    else {
        do {id=r->next_opid++;if(!r->next_opid)r->next_opid=1;}
        while(!id || gmr_op_pending(r,id));
    }
    r->state.last_opid=id;return id;
}
static inline void gmr_action_init(struct gmr_runtime *r,struct gmr_action *a,
    uint32_t kind,uint64_t opid,uint8_t slot,uint8_t bearer) {
    gmq_zero(a,sizeof(*a));a->kind=kind;a->id.epoch=r->state.epoch;
    a->id.opid=opid;a->id.slot=slot;a->id.bearer=bearer;
    if(slot<GMR_SLOTS)a->id.slot_generation=r->state.slots[slot].generation;
    if(bearer<GMR_BEARERS)a->id.bearer_generation=r->state.bearers[bearer].generation;
    a->deadline_ms=r->state.now_ms>UINT64_MAX-GMR_TIMEOUT_MS?UINT64_MAX:
        r->state.now_ms+GMR_TIMEOUT_MS;
}
/* Failed/unknown effects remain visible; a failed reclaim never permits release. */
static inline void gmr_failure(struct gmr_runtime *r,const struct gmr_action *a,int error,int uncertain) {
    r->state.last_error=(uint32_t)error;
    if(uncertain)r->state.uncertain_actions++;
    if(uncertain && ((a->kind>=GMR_ACT_RESOURCES_ACQUIRE && a->kind<=GMR_ACT_IPC_START) ||
        a->kind==GMR_ACT_IPC_PREPARE))r->unresolved_startup=1;
    if(a->id.bearer<GMR_BEARERS && (a->kind==GMR_ACT_QMI ||
        a->kind==GMR_ACT_DATA_CONFIGURE || a->kind==GMR_ACT_DATA_REMOVE || a->kind==GMR_ACT_DATA_TX ||
        a->kind==GMR_ACT_IPA_PREPARE || a->kind==GMR_ACT_WDA_FORMAT ||
        a->kind==GMR_ACT_WDS_BIND || a->kind==GMR_ACT_DMA_CONFIGURE ||
        a->kind==GMR_ACT_NETWORK_CONFIGURE)) {
        struct gmr_bearer *b=&r->state.bearers[a->id.bearer];
        if(a->qmi_kind==GMR_QMI_START && !uncertain && !b->ipa_flags)b->state=GMR_BEARER_DOWN;
        else b->state=GMR_BEARER_UNCERTAIN;
    }
    if(a->id.slot<GMR_SLOTS) {
        struct gmr_slot *s=&r->state.slots[a->id.slot];
        if(a->kind==GMR_ACT_IMS_REGISTER)s->ims=GMR_IMS_FAILED;
        if(a->kind==GMR_ACT_SMS_SEND)s->sms=uncertain?GMR_SMS_UNCERTAIN:GMR_SMS_FAILED;
        if(a->kind==GMR_ACT_VOICE_START || a->kind==GMR_ACT_VOICE_STOP ||
            a->kind==GMR_ACT_MEDIA_START)s->call=GMR_CALL_UNCERTAIN;
    }
    if(a->kind==GMR_ACT_STORAGE_WRITE && uncertain)r->state.storage_dirty=1;
    if(a->kind==GMR_ACT_STORAGE_REPLY) {
        struct gmr_storage_record *record=gmr_storage_find(r,&a->peer);
        /* Durability is established already; replaying this acknowledgement
         * is safe even when the first reply's delivery is uncertain. */
        if(record)record->state=GMR_STORAGE_COMPLETE;
    }
    if(r->state.lifecycle!=GMR_ONLINE || a->kind==GMR_ACT_STORAGE_FLUSH ||
        a->kind==GMR_ACT_STORAGE_WRITE || a->kind==GMR_ACT_SELECT_SLOT) {
        if(r->state.lifecycle!=GMR_ISOLATED)gmr_invalidate(r);
        r->state.lifecycle=GMR_ISOLATED;
    }
}
static inline int gmr_emit(struct gmr_runtime *r,const struct gmr_action *a) {
    unsigned i;int e;
    for(i=0;i<GMR_PENDING && r->pending[i].used;i++);
    if(i==GMR_PENDING){gmr_failure(r,a,GMR_FULL,0);return GMR_FULL;}
    r->pending[i].used=1;r->pending[i].action=*a;r->state.pending_count++;
    /* Reservation precedes callback, so a synchronous callback is enqueued. */
    r->dispatching++;e=r->ops.submit(r->ops.ctx,a);r->dispatching--;
    if(e) {
        r->pending[i].used=0;r->state.pending_count--;
        gmr_failure(r,a,e,0);return e;
    }
    if(!r->dispatching && r->count)(void)gmr_drain(r);
    return GMR_OK;
}
static inline int gmr_simple(struct gmr_runtime *r,uint32_t kind) {
    struct gmr_action a;uint64_t opid=gmr_operation(r,0);
    gmr_action_init(r,&a,kind,opid,GMR_NO_SLOT,GMR_NO_BEARER);return gmr_emit(r,&a);
}
static inline int gmr_storage_reply(struct gmr_runtime *r,struct gmr_storage_record *record,uint64_t opid) {
    struct gmr_action a;
    gmr_action_init(r,&a,GMR_ACT_STORAGE_REPLY,opid,record->peer.slot,GMR_NO_BEARER);
    a.id=record->peer;a.id.opid=opid;a.id.bearer=GMR_NO_BEARER;a.peer=record->peer;
    a.object=record->object;a.offset=record->offset;a.bytes=record->bytes;a.handle=record->reply_status;
    record->state=GMR_STORAGE_REPLYING;return gmr_emit(r,&a);
}
static inline int gmr_storage_request(struct gmr_runtime *r,const struct gmr_event *v) {
    struct gmr_storage_record *record,*free_record=NULL,*oldest=NULL;
    struct gmr_service *service;struct gmr_action a;uint64_t opid;unsigned i,j;
    if(!v->id.opid || !v->bytes || v->offset>UINT64_MAX-v->bytes)return GMR_ARGUMENT;
    record=gmr_storage_find(r,&v->id);
    if(record) {
        if(record->object!=v->value || record->offset!=v->offset || record->bytes!=v->bytes)return GMR_PROTOCOL;
        for(j=0;j<v->bytes;j++)if(record->payload[j]!=v->payload[j])return GMR_PROTOCOL;
        if(record->state!=GMR_STORAGE_COMPLETE)return GMR_BUSY;
        return gmr_storage_reply(r,record,gmr_operation(r,0));
    }
    service=gmr_service_find(r,v->id.service,v->id.slot);
    if(!service || v->id.opid<=service->storage_highwater)return GMR_STALE;
    if(gmr_action_pending(r,GMR_ACT_STORAGE_WRITE) || gmr_action_pending(r,GMR_ACT_STORAGE_FLUSH))return GMR_BUSY;
    for(i=0;i<GMR_STORAGE_CACHE;i++) {
        if(!r->storage[i].state && !free_record)free_record=&r->storage[i];
        if(r->storage[i].state==GMR_STORAGE_COMPLETE && (!oldest || r->storage[i].peer.opid<oldest->peer.opid))
            oldest=&r->storage[i];
    }
    record=free_record?free_record:oldest;if(!record)return GMR_FULL;
    gmq_zero(record,sizeof(*record));record->peer=v->id;record->object=v->value;
    record->offset=v->offset;record->bytes=v->bytes;gmq_copy(record->payload,v->payload,v->bytes);
    record->state=GMR_STORAGE_WRITING;service->storage_highwater=v->id.opid;
    opid=gmr_operation(r,0);gmr_action_init(r,&a,GMR_ACT_STORAGE_WRITE,opid,v->id.slot,GMR_NO_BEARER);
    a.id=v->id;a.id.opid=opid;a.id.bearer=GMR_NO_BEARER;a.peer=v->id;
    a.object=v->value;a.offset=v->offset;a.bytes=v->bytes;gmq_copy(a.payload,v->payload,v->bytes);
    return gmr_emit(r,&a);
}
static inline int gmr_qmi(struct gmr_runtime *r,uint32_t qkind,uint64_t opid,
    uint8_t slot,uint8_t bearer,const char *apn) {
    struct gmr_action a;struct gmr_service *s;uint16_t service;size_t used=0;int e;
    service=(uint16_t)(qkind==GMR_QMI_SIM?GMQ_SERVICE_UIM:
        qkind==GMR_QMI_REGISTRATION?GMQ_SERVICE_NAS:GMQ_SERVICE_WDS);
    s=gmr_service_find(r,service,slot);
    if(!s || !s->available)return GMR_STATE;
    /* Do not reuse a wire transaction in this epoch: a late reply cannot be
     * distinguished by an AP-only operation id once the wire id wraps. */
    if(!r->next_txn)return GMR_FULL;
    gmr_action_init(r,&a,GMR_ACT_QMI,opid,slot,bearer);a.qmi_kind=qkind;
    a.id.service=service;a.id.node=s->node;a.id.port=s->port;
    a.id.endpoint_generation=s->generation;a.id.txn=r->next_txn++;
    switch(qkind) {
    case GMR_QMI_SIM:e=gmq_build_uim_status(a.payload,sizeof(a.payload),a.id.txn,&used);break;
    case GMR_QMI_REGISTRATION:e=gmq_build_nas_serving(a.payload,sizeof(a.payload),a.id.txn,&used);break;
    case GMR_QMI_START:e=gmq_build_wds_start(a.payload,sizeof(a.payload),a.id.txn,apn,&used);break;
    case GMR_QMI_SETTINGS:e=gmq_build_wds_settings(a.payload,sizeof(a.payload),a.id.txn,&used);break;
    case GMR_QMI_STOP:
        a.handle=r->state.bearers[bearer].handle;
        e=gmq_build_wds_stop(a.payload,sizeof(a.payload),a.id.txn,a.handle,&used);break;
    default:return GMR_ARGUMENT;
    }
    if(e)return GMR_ARGUMENT;
    if(qkind==GMR_QMI_START && r->state.bearers[bearer].ip_family==6)
        a.payload[used-1u]=6; /* WDS start IP Family Preference TLV 0x19 */
    a.bytes=(uint16_t)used;return gmr_emit(r,&a);
}
static inline void gmr_invalidate(struct gmr_runtime *r) {
    unsigned i;
    r->state.epoch++;if(!r->state.epoch)r->state.epoch=1;
    for(i=0;i<GMR_PENDING;i++)if(r->pending[i].used) {
        /* Potential effects are retained by resource/storage/bearer state. */
        if(r->pending[i].action.kind==GMR_ACT_STORAGE_WRITE)r->state.storage_dirty=1;
        if((r->pending[i].action.kind>=GMR_ACT_RESOURCES_ACQUIRE && r->pending[i].action.kind<=GMR_ACT_IPC_START) ||
            r->pending[i].action.kind==GMR_ACT_IPC_PREPARE)r->unresolved_startup=1;
        r->state.uncertain_actions++;r->pending[i].used=0;
    }
    r->state.pending_count=0;
    r->next_txn=1;
    gmq_zero(r->rx_pending,sizeof(r->rx_pending));
    gmq_zero(r->storage,sizeof(r->storage));
    gmq_zero(r->state.services,sizeof(r->state.services));
    r->state.rf_mode_known=0;
    for(i=0;i<GMR_SLOTS;i++) {
        r->state.slots[i].generation++;if(!r->state.slots[i].generation)r->state.slots[i].generation=1;
        r->state.slots[i].sim=GMR_SIM_UNKNOWN;r->state.slots[i].registration=GMR_REG_UNKNOWN;
        r->state.slots[i].packet_attached=0;r->state.slots[i].roaming_known=0;r->state.slots[i].ims=GMR_IMS_OFF;
        if(r->state.slots[i].call!=GMR_CALL_IDLE)r->state.slots[i].call=GMR_CALL_UNCERTAIN;
        if(r->state.slots[i].sms==GMR_SMS_SUBMITTING)r->state.slots[i].sms=GMR_SMS_UNCERTAIN;
    }
    for(i=0;i<GMR_BEARERS;i++)if(r->state.bearers[i].state!=GMR_BEARER_DOWN)
        r->state.bearers[i].state=GMR_BEARER_UNCERTAIN;
}
static inline int gmr_start_internal(struct gmr_runtime *r) {
    int e;
    if(!r)return GMR_ARGUMENT;
    if(r->state.lifecycle!=GMR_OFF || r->state.pending_count ||
        r->state.resources_owned || r->state.uncertain_actions)return GMR_STATE;
    r->state.epoch++;if(!r->state.epoch)r->state.epoch=1;
    r->state.last_error=GMR_OK;r->state.lifecycle=GMR_ACQUIRING;
    e=gmr_simple(r,GMR_ACT_RESOURCES_ACQUIRE);gmr_notify(r);return e;
}
static inline int gmr_start(struct gmr_runtime *r) {
    if(!r)return GMR_ARGUMENT;
    if(r->dispatching)return GMR_BUSY;
    return gmr_start_internal(r);
}
static inline int gmr_teardown_next(struct gmr_runtime *r) {
    unsigned i;int active;
    for(;;)switch(++r->teardown_stage) {
    case 1:
        if(r->state.storage_running)return gmr_simple(r,GMR_ACT_STORAGE_FLUSH);
        break;
    case 2:
        active=0;for(i=0;i<GMR_SLOTS;i++)if(r->state.slots[i].call!=GMR_CALL_IDLE)active=1;
        if(active)return gmr_simple(r,GMR_ACT_MEDIA_DRAIN);
        break;
    case 3:
        if(r->state.ipc_running || r->state.firmware_running)return gmr_simple(r,GMR_ACT_DATA_DRAIN);
        break;
    case 4:if(r->state.ipc_running || r->state.ipc_prepared)return gmr_simple(r,GMR_ACT_IPC_STOP);break;
    case 5:if(r->state.firmware_running)return gmr_simple(r,GMR_ACT_FIRMWARE_STOP);break;
    case 6:if(r->state.resources_owned)return gmr_simple(r,GMR_ACT_MEMORY_RECLAIM);break;
    case 7:if(r->state.storage_running)return gmr_simple(r,GMR_ACT_STORAGE_STOP);break;
    case 8:if(r->state.resources_owned)return gmr_simple(r,GMR_ACT_RESOURCES_RELEASE);break;
    default:
        r->state.lifecycle=GMR_OFF;r->state.uncertain_actions=0;
        if(r->state.restart_requested){r->state.restart_requested=0;return gmr_start_internal(r);}
        return GMR_OK;
    }
}
static inline int gmr_begin_stop(struct gmr_runtime *r,int restart) {
    if(r->state.lifecycle==GMR_OFF)return restart?gmr_start_internal(r):GMR_OK;
    if(r->state.lifecycle==GMR_STOPPING) {
        /* An external shutdown supersedes SSR's automatic restart while
         * retaining the already running ordered drain and its ownership. */
        if(!restart){r->state.restart_requested=0;return GMR_OK;}
        return GMR_BUSY;
    }
    if(r->unresolved_startup)return GMR_UNCERTAIN;
    /* An in-flight acquisition/boot cannot be treated as absent. Require the
     * provider to resolve its effects before any destructive teardown. */
    if(gmr_action_pending(r,GMR_ACT_RESOURCES_ACQUIRE) ||
        gmr_action_pending(r,GMR_ACT_FIRMWARE_BOOT) ||
        gmr_action_pending(r,GMR_ACT_IPC_PREPARE) ||
        gmr_action_pending(r,GMR_ACT_IPC_START) ||
        gmr_action_pending(r,GMR_ACT_STORAGE_START)) {
        gmr_invalidate(r);r->state.lifecycle=GMR_ISOLATED;
        r->state.last_error=GMR_UNCERTAIN;return GMR_UNCERTAIN;
    }
    gmr_invalidate(r);r->state.restart_requested=(uint8_t)(restart!=0);
    r->state.lifecycle=GMR_STOPPING;r->teardown_stage=0;return gmr_teardown_next(r);
}
static inline int gmr_slot_has_bearer(const struct gmr_runtime *r,unsigned slot) {
    unsigned i;for(i=0;i<GMR_BEARERS;i++)if(r->state.bearers[i].slot==slot &&
        r->state.bearers[i].state!=GMR_BEARER_DOWN)return 1;
    return 0;
}
static inline int gmr_slot_has_up_bearer(const struct gmr_runtime *r,unsigned slot) {
    unsigned i;for(i=0;i<GMR_BEARERS;i++)if(r->state.bearers[i].slot==slot &&
        r->state.bearers[i].state==GMR_BEARER_UP &&
        r->state.bearers[i].slot_generation==r->state.slots[slot].generation)return 1;
    return 0;
}
static inline int gmr_bind_action(struct gmr_runtime *r,struct gmr_action *a,uint16_t service) {
    struct gmr_service *ep=gmr_service_find(r,service,a->id.slot);
    if(!ep || !ep->available)return GMR_STATE;
    a->id.service=service;a->id.node=ep->node;a->id.port=ep->port;
    a->id.endpoint_generation=ep->generation;return GMR_OK;
}
static inline int gmr_data_stage(struct gmr_runtime *r,uint32_t kind,uint64_t opid,uint8_t bearer) {
    struct gmr_action a;struct gmr_bearer *b=&r->state.bearers[bearer];int e;
    uint16_t service=(uint16_t)(kind==GMR_ACT_IPA_PREPARE?0x31u:
        kind==GMR_ACT_WDA_FORMAT?0x1au:GMQ_SERVICE_WDS);
    gmr_action_init(r,&a,kind,opid,b->slot,bearer);a.ip_family=b->ip_family;a.mux=b->mux;
    a.handle=b->handle;a.ipv4=b->ipv4;a.ipv6=b->ipv6;
    a.object=kind==GMR_ACT_IPA_PREPARE?7u:kind==GMR_ACT_WDA_FORMAT?3u:
        kind==GMR_ACT_WDS_BIND?7u:kind==GMR_ACT_DMA_CONFIGURE?31u:7u;
    if(kind==GMR_ACT_NETWORK_CONFIGURE && ((b->ip_family==4 &&
        (b->ipv4.present&(GMQ_IPV4_DNS1|GMQ_IPV4_DNS2))) ||
        (b->ip_family==6 && (b->ipv6.present&12u))))a.object|=GMR_NET_DNS;
    e=gmr_bind_action(r,&a,service);
    if(e){gmr_failure(r,&a,e,0);return e;}
    return gmr_emit(r,&a);
}
/* Preserve the per-bearer incarnation after clearing its current session.
 * Epoch/slot/endpoint can all stay unchanged across a normal reconnect. */
static inline void gmr_bearer_clear(struct gmr_bearer *b) {
    uint32_t generation=b->generation;
    gmq_zero(b,sizeof(*b));b->slot=GMR_NO_SLOT;b->generation=generation;
}
static inline int gmr_bearer_remove(struct gmr_runtime *r,uint64_t opid,uint8_t bearer) {
    struct gmr_bearer *b=&r->state.bearers[bearer];struct gmr_action a;unsigned i;
    /* Retire executor receipts and actions so locally queued packets/TX can
     * never be submitted after removal starts. Already submitted DMA remains
     * the provider's responsibility: the completion must prove all four
     * teardown conditions before WDS stop can authorize reuse of this slot. */
    for(i=0;i<GMR_PENDING;i++)if(r->pending[i].used &&
        r->pending[i].action.id.bearer==bearer &&
        r->pending[i].action.id.bearer_generation==b->generation) {
        r->pending[i].used=0;r->state.pending_count--;
    }
    for(i=0;i<GMR_EVENTS;i++)if(r->rx_pending[i].used &&
        r->rx_pending[i].id.bearer==bearer &&
        r->rx_pending[i].id.bearer_generation==b->generation)r->rx_pending[i].used=0;
    gmr_action_init(r,&a,GMR_ACT_DATA_REMOVE,opid,b->slot,bearer);
    a.handle=b->handle;a.object=GMR_REMOVE_COMPLETE;b->state=GMR_BEARER_STOPPING;
    return gmr_emit(r,&a);
}
static inline int gmr_submit(struct gmr_runtime *r,const struct gmr_command *c) {
    struct gmr_action a;struct gmr_slot *s;struct gmr_bearer *b;uint64_t opid;int e;
    if(!r || !c || c->bytes>GMR_PAYLOAD)return GMR_ARGUMENT;
    if(r->dispatching)return GMR_BUSY;
    if(c->kind==GMR_CMD_STOP || c->kind==GMR_CMD_SSR) {
        e=gmr_begin_stop(r,c->kind==GMR_CMD_SSR);gmr_notify(r);return e;
    }
    if(c->kind==GMR_CMD_RESUME) {
        if(r->state.lifecycle!=GMR_SUSPENDED)return GMR_STATE;
        r->state.lifecycle=GMR_RESUMING;e=gmr_simple(r,GMR_ACT_RESUME);gmr_notify(r);return e;
    }
    if(r->state.lifecycle!=GMR_ONLINE)return GMR_STATE;
    if(c->kind==GMR_CMD_SUSPEND) {
        if(r->state.pending_count || r->state.storage_dirty)return GMR_BUSY;
        for(unsigned i=0;i<GMR_EVENTS;i++)if(r->rx_pending[i].used)return GMR_BUSY;
        r->state.lifecycle=GMR_SUSPENDING;e=gmr_simple(r,GMR_ACT_SUSPEND);gmr_notify(r);return e;
    }
    if(c->kind!=GMR_CMD_STORAGE_WRITE && c->kind!=GMR_CMD_STORAGE_FLUSH && c->slot>=GMR_SLOTS)
        return GMR_ARGUMENT;
    if(c->kind<GMR_CMD_QUERY_SIM || c->kind>GMR_CMD_RF_MODE)return GMR_ARGUMENT;
    opid=gmr_operation(r,c->opid);if(!opid)return GMR_BUSY;
    s=c->slot<GMR_SLOTS?&r->state.slots[c->slot]:NULL;
    switch(c->kind) {
    case GMR_CMD_QUERY_SIM:return gmr_qmi(r,GMR_QMI_SIM,opid,c->slot,GMR_NO_BEARER,NULL);
    case GMR_CMD_QUERY_REGISTRATION:return gmr_qmi(r,GMR_QMI_REGISTRATION,opid,c->slot,GMR_NO_BEARER,NULL);
    case GMR_CMD_CONNECT:
        if(c->bearer>=GMR_BEARERS || c->apn[GMQ_MAX_APN])return GMR_ARGUMENT;
        if(c->ip_family && c->ip_family!=4 && c->ip_family!=6)return GMR_UNSUPPORTED;
        { size_t used=0;if(gmq_build_wds_start(a.payload,sizeof(a.payload),1,c->apn,&used))return GMR_ARGUMENT; }
        b=&r->state.bearers[c->bearer];
        if(b->state!=GMR_BEARER_DOWN || s->sim!=GMR_SIM_READY ||
            s->registration!=GMR_REG_REGISTERED || !s->packet_attached)return GMR_STATE;
        if(b->generation==UINT32_MAX)return GMR_FULL;
        b->generation++;
        b->slot=c->slot;b->slot_generation=s->generation;b->mux=(uint8_t)(c->bearer+1);
        b->ip_family=c->ip_family?c->ip_family:4;
        gmq_copy(b->apn,c->apn,sizeof(b->apn));
        b->state=GMR_BEARER_STARTING;
        e=gmr_data_stage(r,GMR_ACT_IPA_PREPARE,opid,c->bearer);
        gmr_notify(r);return e;
    case GMR_CMD_DISCONNECT:
        if(c->bearer>=GMR_BEARERS)return GMR_ARGUMENT;
        b=&r->state.bearers[c->bearer];
        if(b->slot!=c->slot || (b->state!=GMR_BEARER_UP && b->state!=GMR_BEARER_UNCERTAIN))return GMR_STATE;
        e=gmr_bearer_remove(r,opid,c->bearer);gmr_notify(r);return e;
    case GMR_CMD_STORAGE_WRITE:case GMR_CMD_STORAGE_FLUSH:
        if(!r->state.storage_running || gmr_action_pending(r,GMR_ACT_STORAGE_WRITE) ||
            gmr_action_pending(r,GMR_ACT_STORAGE_FLUSH))return GMR_BUSY;
        if(c->kind==GMR_CMD_STORAGE_WRITE && !c->bytes)return GMR_ARGUMENT;
        if(c->offset>UINT64_MAX-c->bytes)return GMR_ARGUMENT;
        gmr_action_init(r,&a,c->kind==GMR_CMD_STORAGE_WRITE?GMR_ACT_STORAGE_WRITE:GMR_ACT_STORAGE_FLUSH,
            opid,GMR_NO_SLOT,GMR_NO_BEARER);
        a.object=c->object;a.offset=c->offset;a.bytes=c->bytes;
        gmq_copy(a.payload,c->payload,c->bytes);break;
    case GMR_CMD_IMS_REGISTER:
        if(s->sim!=GMR_SIM_READY || s->registration!=GMR_REG_REGISTERED ||
            !gmr_slot_has_up_bearer(r,c->slot) || s->ims==GMR_IMS_REGISTERING)return GMR_STATE;
        gmr_action_init(r,&a,GMR_ACT_IMS_REGISTER,opid,c->slot,GMR_NO_BEARER);
        s->ims=GMR_IMS_REGISTERING;break;
    case GMR_CMD_VOICE_START:
        if(s->call!=GMR_CALL_IDLE || s->sim!=GMR_SIM_READY ||
            s->registration!=GMR_REG_REGISTERED)return GMR_STATE;
        gmr_action_init(r,&a,GMR_ACT_VOICE_START,opid,c->slot,GMR_NO_BEARER);
        s->call=GMR_CALL_STARTING;a.bytes=c->bytes;gmq_copy(a.payload,c->payload,c->bytes);break;
    case GMR_CMD_VOICE_STOP:
        if(s->call!=GMR_CALL_ACTIVE && s->call!=GMR_CALL_NETWORK_ACTIVE)return GMR_STATE;
        gmr_action_init(r,&a,GMR_ACT_VOICE_STOP,opid,c->slot,GMR_NO_BEARER);
        a.handle=s->call_id;s->call=GMR_CALL_STOPPING;break;
    case GMR_CMD_SMS_SEND:
        if(s->sms==GMR_SMS_SUBMITTING || s->sms==GMR_SMS_ACCEPTED ||
            s->sms==GMR_SMS_UNCERTAIN)return GMR_BUSY;
        if(s->sim!=GMR_SIM_READY || s->registration!=GMR_REG_REGISTERED ||
            !c->bytes)return GMR_STATE;
        gmr_action_init(r,&a,GMR_ACT_SMS_SEND,opid,c->slot,GMR_NO_BEARER);
        s->sms=GMR_SMS_SUBMITTING;a.bytes=c->bytes;gmq_copy(a.payload,c->payload,c->bytes);break;
    case GMR_CMD_SELECT_SLOT:
        if(r->state.pending_count || gmr_slot_has_bearer(r,0) || gmr_slot_has_bearer(r,1) ||
            r->state.slots[0].call!=GMR_CALL_IDLE || r->state.slots[1].call!=GMR_CALL_IDLE)return GMR_BUSY;
        gmr_action_init(r,&a,GMR_ACT_SELECT_SLOT,opid,c->slot,GMR_NO_BEARER);break;
    case GMR_CMD_PACKET_TX:
        if(c->bearer>=GMR_BEARERS)return GMR_ARGUMENT;
        b=&r->state.bearers[c->bearer];
        if(b->state!=GMR_BEARER_UP || b->slot!=c->slot || b->slot_generation!=s->generation)
            return GMR_STATE;
        if(!gmr_packet_valid(c->payload,c->bytes,b->ip_family))return GMR_PROTOCOL;
        gmr_action_init(r,&a,GMR_ACT_DATA_TX,opid,c->slot,c->bearer);
        a.handle=b->handle;a.bytes=c->bytes;gmq_copy(a.payload,c->payload,c->bytes);
        a.id.service=GMQ_SERVICE_WDS;
        { struct gmr_service *ep=gmr_service_find(r,GMQ_SERVICE_WDS,c->slot);
          if(!ep || !ep->available || ep->generation!=b->endpoint_generation)return GMR_STATE;
          a.id.node=ep->node;a.id.port=ep->port;a.id.endpoint_generation=ep->generation; }
        break;
    case GMR_CMD_RF_MODE:
        if(c->object>1)return GMR_ARGUMENT;
        if(r->state.pending_count || gmr_slot_has_bearer(r,0) || gmr_slot_has_bearer(r,1) ||
            r->state.slots[0].call!=GMR_CALL_IDLE || r->state.slots[1].call!=GMR_CALL_IDLE)return GMR_BUSY;
        gmr_action_init(r,&a,GMR_ACT_RF_MODE,opid,GMR_NO_SLOT,GMR_NO_BEARER);
        a.object=c->object;break;
    default:return GMR_UNSUPPORTED;
    }
    e=GMR_OK;
    if(a.kind==GMR_ACT_IMS_REGISTER)e=gmr_bind_action(r,&a,0x21u);
    else if(a.kind==GMR_ACT_VOICE_START || a.kind==GMR_ACT_VOICE_STOP)e=gmr_bind_action(r,&a,0x09u);
    else if(a.kind==GMR_ACT_SMS_SEND)e=gmr_bind_action(r,&a,0x05u);
    else if(a.kind==GMR_ACT_SELECT_SLOT)e=gmr_bind_action(r,&a,GMQ_SERVICE_UIM);
    else if(a.kind==GMR_ACT_RF_MODE) {
        a.id.slot=r->state.selected_slot;a.id.slot_generation=r->state.slots[a.id.slot].generation;
        e=gmr_bind_action(r,&a,0x02u);
    }
    if(e)gmr_failure(r,&a,e,0);
    else e=gmr_emit(r,&a);
    gmr_notify(r);return e;
}

static inline int gmr_qmi_complete(struct gmr_runtime *r,const struct gmr_action *a,const struct gmr_event *v) {
    struct gmr_slot *s=&r->state.slots[a->id.slot];
    struct gmr_bearer *b=a->id.bearer<GMR_BEARERS?&r->state.bearers[a->id.bearer]:NULL;
    int e=GMQ_UNSUPPORTED;
    switch(a->qmi_kind) {
    case GMR_QMI_SIM: {
        struct gmq_sim_status sim;uint16_t ix;unsigned pin;
        e=gmq_parse_uim_status(v->payload,v->bytes,a->id.txn,&sim);if(e)break;
        ix=a->id.slot?sim.secondary_gw:sim.primary_gw;
        s->sim=GMR_SIM_UNKNOWN;
        if(a->id.slot<sim.card_count && sim.cards[a->id.slot].card_state==0)s->sim=GMR_SIM_ABSENT;
        if(ix!=65535u && gmq_index_valid(&sim,ix)) {
            const struct gmq_card *card=&sim.cards[ix>>8];const struct gmq_app *app=&card->apps[ix&255u];
            pin=app->upin_replaces_pin1?card->upin_state:app->pin1_state;
            if(card->card_state==0)s->sim=GMR_SIM_ABSENT;
            else if(card->card_state!=1)s->sim=GMR_SIM_ERROR;
            else if((app->type==1 || app->type==2) && app->state==7 && (pin==2 || pin==3))
                s->sim=GMR_SIM_READY;
            else s->sim=GMR_SIM_LOCKED;
        }
        if(s->sim!=GMR_SIM_READY && (gmr_slot_has_bearer(r,a->id.slot) ||
            s->call!=GMR_CALL_IDLE || s->sms==GMR_SMS_SUBMITTING || s->ims==GMR_IMS_REGISTERED))
            return gmr_begin_stop(r,1);
        return GMR_OK;
    }
    case GMR_QMI_REGISTRATION: {
        struct gmq_serving serving;
        e=gmq_parse_nas_serving(v->payload,v->bytes,a->id.txn,&serving);if(e)break;
        s->registration=serving.registration==1?GMR_REG_REGISTERED:
            serving.registration==2?GMR_REG_SEARCHING:serving.registration==3?GMR_REG_DENIED:GMR_REG_OFFLINE;
        s->packet_attached=(uint8_t)(serving.ps_attach==1);
        s->roaming=(uint8_t)(serving.roaming_present && serving.roaming==0);
        s->roaming_known=serving.roaming_present;
        if((s->registration!=GMR_REG_REGISTERED || !s->packet_attached) && gmr_slot_has_bearer(r,a->id.slot))
            return gmr_begin_stop(r,1);
        return GMR_OK;
    }
    case GMR_QMI_START:
        e=gmq_parse_wds_start(v->payload,v->bytes,a->id.txn,&b->handle);if(e)break;
        b->endpoint_generation=a->id.endpoint_generation;b->state=GMR_BEARER_SETTINGS;
        e=gmr_qmi(r,GMR_QMI_SETTINGS,a->id.opid,a->id.slot,a->id.bearer,NULL);
        if(e)gmr_failure(r,a,e,0);
        return e;
    case GMR_QMI_SETTINGS:
        if(b->ip_family==6) {
            e=gmr_parse_ipv6(v->payload,v->bytes,a->id.txn,&b->ipv6);if(e)break;
            if(!gmr_ipv6_usable(&b->ipv6)){e=GMQ_WIRE;break;}
        } else {
            e=gmq_parse_wds_settings(v->payload,v->bytes,a->id.txn,&b->ipv4);if(e)break;
            if(!gmr_ipv4_usable(&b->ipv4)){e=GMQ_WIRE;break;}
        }
        b->state=GMR_BEARER_CONFIGURING;
        return gmr_data_stage(r,GMR_ACT_DMA_CONFIGURE,a->id.opid,a->id.bearer);
    case GMR_QMI_STOP:
        e=gmq_parse_wds_stop(v->payload,v->bytes,a->id.txn);if(e)break;
        gmr_bearer_clear(b);return GMR_OK;
    default:break;
    }
    e=e==GMQ_REMOTE?GMR_REMOTE:GMR_PROTOCOL;
    gmr_failure(r,a,e,e!=GMR_REMOTE);return e;
}
static inline int gmr_complete(struct gmr_runtime *r,const struct gmr_action *a,const struct gmr_event *v) {
    unsigned i;struct gmr_action next;struct gmr_slot *s=a->id.slot<GMR_SLOTS?&r->state.slots[a->id.slot]:NULL;
    if(v->status) {gmr_failure(r,a,v->status,1);return v->status;}
    if(a->kind==GMR_ACT_QMI)return gmr_qmi_complete(r,a,v);
    switch(a->kind) {
    case GMR_ACT_RESOURCES_ACQUIRE:
        r->state.resources_owned=1;r->state.lifecycle=GMR_STORAGE_STARTING;
        return gmr_simple(r,GMR_ACT_STORAGE_START);
    case GMR_ACT_STORAGE_START:
        r->state.storage_running=1;r->state.lifecycle=GMR_IPC_STARTING;return gmr_simple(r,GMR_ACT_IPC_PREPARE);
    case GMR_ACT_IPC_PREPARE:
        r->state.ipc_prepared=1;r->state.lifecycle=GMR_BOOTING;return gmr_simple(r,GMR_ACT_FIRMWARE_BOOT);
    case GMR_ACT_FIRMWARE_BOOT:
        r->state.firmware_running=1;r->state.lifecycle=GMR_IPC_STARTING;return gmr_simple(r,GMR_ACT_IPC_START);
    case GMR_ACT_IPC_START:r->state.ipc_running=1;r->state.lifecycle=GMR_ONLINE;return GMR_OK;
    case GMR_ACT_IPA_PREPARE:case GMR_ACT_WDA_FORMAT:case GMR_ACT_WDS_BIND:
    case GMR_ACT_DMA_CONFIGURE:case GMR_ACT_NETWORK_CONFIGURE: {
        struct gmr_bearer *b=&r->state.bearers[a->id.bearer];
        if((v->value&a->object)!=a->object){gmr_failure(r,a,GMR_PROTOCOL,1);return GMR_PROTOCOL;}
        if(a->kind==GMR_ACT_IPA_PREPARE){b->ipa_flags=v->value;return gmr_data_stage(r,GMR_ACT_WDA_FORMAT,a->id.opid,a->id.bearer);}
        if(a->kind==GMR_ACT_WDA_FORMAT){b->wda_flags=v->value;return gmr_data_stage(r,GMR_ACT_WDS_BIND,a->id.opid,a->id.bearer);}
        if(a->kind==GMR_ACT_WDS_BIND) {
            int e;b->bind_flags=v->value;
            e=gmr_qmi(r,GMR_QMI_START,a->id.opid,a->id.slot,a->id.bearer,b->apn);
            if(e)gmr_failure(r,a,e,0);
            return e;
        }
        if(a->kind==GMR_ACT_DMA_CONFIGURE){b->dma_flags=v->value;return gmr_data_stage(r,GMR_ACT_NETWORK_CONFIGURE,a->id.opid,a->id.bearer);}
        b->network_flags=v->value;b->state=GMR_BEARER_UP;return GMR_OK;
    }
    case GMR_ACT_DATA_REMOVE:
        if((v->value&GMR_REMOVE_COMPLETE)!=GMR_REMOVE_COMPLETE) {
            gmr_failure(r,a,GMR_PROTOCOL,1);return GMR_PROTOCOL;
        }
        if(!r->state.bearers[a->id.bearer].handle) {
            gmr_bearer_clear(&r->state.bearers[a->id.bearer]);return GMR_OK;
        }
        {int e=gmr_qmi(r,GMR_QMI_STOP,a->id.opid,a->id.slot,a->id.bearer,NULL);
         if(e)gmr_failure(r,a,e,0);
         return e;}
    case GMR_ACT_DATA_TX:
        if(v->value!=a->bytes){gmr_failure(r,a,GMR_IO,1);return GMR_IO;}
        r->state.bearers[a->id.bearer].tx_bytes+=v->value;return GMR_OK;
    case GMR_ACT_STORAGE_WRITE:
        r->state.storage_dirty=1;
        if(v->value!=a->bytes){gmr_failure(r,a,GMR_IO,1);return GMR_IO;}
        r->state.storage_writes++;
        if(a->peer.epoch) {
            struct gmr_storage_record *record=gmr_storage_find(r,&a->peer);
            if(!record){gmr_failure(r,a,GMR_PROTOCOL,1);return GMR_PROTOCOL;}
            record->state=GMR_STORAGE_FLUSHING;
        }
        gmr_action_init(r,&next,GMR_ACT_STORAGE_FLUSH,a->id.opid,GMR_NO_SLOT,GMR_NO_BEARER);
        /* Preserve reverse-service address and transaction through durability;
         * only this completion authorizes the provider's remote write reply. */
        next.id=a->id;next.peer=a->peer;next.object=a->object;next.offset=a->offset;return gmr_emit(r,&next);
    case GMR_ACT_STORAGE_FLUSH:
        r->state.storage_durable=r->state.storage_writes;r->state.storage_dirty=0;
        if(a->peer.epoch) {
            struct gmr_storage_record *record=gmr_storage_find(r,&a->peer);
            if(!record){gmr_failure(r,a,GMR_PROTOCOL,1);return GMR_PROTOCOL;}
            record->reply_status=GMR_OK;return gmr_storage_reply(r,record,a->id.opid);
        }
        if(r->state.lifecycle==GMR_STOPPING)return gmr_teardown_next(r);
        return GMR_OK;
    case GMR_ACT_STORAGE_REPLY: {
        struct gmr_storage_record *record=gmr_storage_find(r,&a->peer);
        if(!record)return GMR_STALE;
        record->state=GMR_STORAGE_COMPLETE;return GMR_OK;
    }
    case GMR_ACT_IMS_REGISTER:
        /* QMI/provider acknowledgement is not an IMS registration indication. */
        return GMR_OK;
    case GMR_ACT_VOICE_START:
        if(!v->value){gmr_failure(r,a,GMR_PROTOCOL,1);return GMR_PROTOCOL;}
        s->call=GMR_CALL_NETWORK_ACTIVE;s->call_id=v->value;
        gmr_action_init(r,&next,GMR_ACT_MEDIA_START,a->id.opid,a->id.slot,GMR_NO_BEARER);
        next.handle=v->value;return gmr_emit(r,&next);
    case GMR_ACT_MEDIA_START:s->call=GMR_CALL_ACTIVE;return GMR_OK;
    case GMR_ACT_VOICE_STOP:
        gmr_action_init(r,&next,GMR_ACT_MEDIA_DRAIN,a->id.opid,a->id.slot,GMR_NO_BEARER);
        next.handle=s->call_id;return gmr_emit(r,&next);
    case GMR_ACT_SMS_SEND:s->sms=GMR_SMS_ACCEPTED;s->sms_id=v->value;return GMR_OK;
    case GMR_ACT_SELECT_SLOT:
        r->state.selected_slot=a->id.slot;
        for(i=0;i<GMR_SLOTS;i++) {
            r->state.slots[i].generation++;r->state.slots[i].sim=GMR_SIM_UNKNOWN;
            if(!r->state.slots[i].generation)r->state.slots[i].generation=1;
            r->state.slots[i].registration=GMR_REG_UNKNOWN;r->state.slots[i].packet_attached=0;
            r->state.slots[i].roaming=0;r->state.slots[i].roaming_known=0;
            r->state.slots[i].ims=GMR_IMS_OFF;r->state.slots[i].sms=GMR_SMS_IDLE;r->state.slots[i].sms_id=0;
        }
        return GMR_OK;
    case GMR_ACT_RF_MODE:
        r->state.rf_enabled=(uint8_t)a->object;
        r->state.rf_mode_known=1;
        if(!a->object)for(i=0;i<GMR_SLOTS;i++) {
            r->state.slots[i].registration=GMR_REG_OFFLINE;r->state.slots[i].packet_attached=0;
            r->state.slots[i].roaming=0;r->state.slots[i].roaming_known=0;
            r->state.slots[i].ims=GMR_IMS_OFF;
        }
        return GMR_OK;
    case GMR_ACT_SUSPEND:r->state.lifecycle=GMR_SUSPENDED;return GMR_OK;
    case GMR_ACT_RESUME:r->state.lifecycle=GMR_ONLINE;return GMR_OK;
    case GMR_ACT_MEDIA_DRAIN:
        if(s){s->call=GMR_CALL_IDLE;s->call_id=0;return GMR_OK;}
        for(i=0;i<GMR_SLOTS;i++){r->state.slots[i].call=GMR_CALL_IDLE;r->state.slots[i].call_id=0;}
        break;
    case GMR_ACT_DATA_DRAIN:
        for(i=0;i<GMR_BEARERS;i++)gmr_bearer_clear(&r->state.bearers[i]);
        break;
    case GMR_ACT_IPC_STOP:r->state.ipc_running=0;r->state.ipc_prepared=0;break;
    case GMR_ACT_FIRMWARE_STOP:r->state.firmware_running=0;break;
    case GMR_ACT_MEMORY_RECLAIM:break;
    case GMR_ACT_STORAGE_STOP:r->state.storage_running=0;break;
    case GMR_ACT_RESOURCES_RELEASE:r->state.resources_owned=0;break;
    default:return GMR_UNSUPPORTED;
    }
    return gmr_teardown_next(r);
}
/* Unsolicited events have no transaction, but must name the live service. */
static inline int gmr_source_current(struct gmr_runtime *r,const struct gmr_identity *id) {
    struct gmr_service *s;
    if(id->epoch!=r->state.epoch || id->slot>=GMR_SLOTS ||
        id->slot_generation!=r->state.slots[id->slot].generation)return 0;
    s=gmr_service_find(r,id->service,id->slot);
    return s && s->available && s->generation==id->endpoint_generation &&
        s->node==id->node && s->port==id->port;
}
static inline int gmr_process_event(struct gmr_runtime *r,const struct gmr_event *v) {
    unsigned i;struct gmr_service *service;struct gmr_action a;struct gmr_slot *slot;
    if(v->id.epoch!=r->state.epoch){r->state.stale_events++;return GMR_STALE;}
    if(r->state.lifecycle==GMR_ISOLATED && v->kind!=GMR_EV_FATAL)return GMR_STATE;
    if(v->kind==GMR_EV_FATAL) {
        if(r->state.lifecycle==GMR_OFF)return GMR_STATE;
        r->state.last_fault=(uint32_t)(v->status?v->status:GMR_IO);
        r->state.last_error=r->state.last_fault;r->state.fault_epoch=v->id.epoch;
        if(r->state.fault_count!=UINT64_MAX)r->state.fault_count++;
        return gmr_begin_stop(r,1);
    }
    if(v->kind==GMR_EV_SERVICE_UP) {
        if(r->state.lifecycle!=GMR_ONLINE && r->state.lifecycle!=GMR_IPC_STARTING &&
            r->state.lifecycle!=GMR_BOOTING)return GMR_STATE;
        if(v->id.slot>=GMR_SLOTS || !v->id.service || !v->id.port || !v->id.endpoint_generation ||
            v->id.slot_generation!=r->state.slots[v->id.slot].generation)return GMR_ARGUMENT;
        service=gmr_service_find(r,v->id.service,v->id.slot);
        if(service && service->available && service->generation==v->id.endpoint_generation &&
            service->node==v->id.node && service->port==v->id.port)return GMR_OK;
        if(service && v->id.endpoint_generation<=service->generation){r->state.stale_events++;return GMR_STALE;}
        if(service && service->available)return gmr_begin_stop(r,1);
        if(!service)for(i=0;i<GMR_SERVICES;i++)if(!r->state.services[i].generation){service=&r->state.services[i];break;}
        if(!service)return GMR_FULL;
        service->service=v->id.service;service->slot=v->id.slot;service->available=1;
        service->generation=v->id.endpoint_generation;service->node=v->id.node;service->port=v->id.port;return GMR_OK;
    }
    if(v->kind==GMR_EV_COMPLETE || v->kind==GMR_EV_QMI_RESPONSE || v->kind==GMR_EV_DATA_COMPLETION) {
        for(i=0;i<GMR_PENDING;i++)if(r->pending[i].used && gmr_identity_equal(&r->pending[i].action.id,&v->id))break;
        if(i==GMR_PENDING){r->state.stale_events++;return GMR_STALE;}
        a=r->pending[i].action;
        if(v->action_kind!=a.kind || ((a.kind==GMR_ACT_QMI)!=(v->kind==GMR_EV_QMI_RESPONSE)) ||
            (v->kind==GMR_EV_DATA_COMPLETION && a.kind!=GMR_ACT_DATA_TX)) {
            r->state.protocol_errors++;return GMR_PROTOCOL;
        }
        if(a.id.slot<GMR_SLOTS && a.id.slot_generation!=r->state.slots[a.id.slot].generation) {
            r->state.stale_events++;return GMR_STALE;
        }
        if(a.id.bearer<GMR_BEARERS &&
            a.id.bearer_generation!=r->state.bearers[a.id.bearer].generation) {
            r->state.stale_events++;return GMR_STALE;
        }
        r->pending[i].used=0;r->state.pending_count--;
        return gmr_complete(r,&a,v);
    }
    if(!gmr_source_current(r,&v->id)){r->state.stale_events++;return GMR_STALE;}
    slot=&r->state.slots[v->id.slot];
    switch(v->kind) {
    case GMR_EV_SERVICE_DOWN:
        service=gmr_service_find(r,v->id.service,v->id.slot);service->available=0;
        return gmr_begin_stop(r,1);
    case GMR_EV_SIM_REMOVED:
        if(v->id.service!=GMQ_SERVICE_UIM)return GMR_PROTOCOL;
        i=(unsigned)gmr_begin_stop(r,1);r->state.slots[v->id.slot].sim=GMR_SIM_ABSENT;return (int)i;
    case GMR_EV_SIM_CHANGED:
        if(v->id.service!=GMQ_SERVICE_UIM)return GMR_PROTOCOL;
        if(gmr_slot_has_bearer(r,v->id.slot) || slot->call!=GMR_CALL_IDLE)return gmr_begin_stop(r,1);
        for(i=0;i<GMR_PENDING;i++)if(r->pending[i].used && r->pending[i].action.id.slot==v->id.slot) {
            if(r->pending[i].action.kind!=GMR_ACT_QMI)return gmr_begin_stop(r,1);
            r->pending[i].used=0;r->state.pending_count--;
        }
        slot->generation++;slot->sim=GMR_SIM_UNKNOWN;slot->registration=GMR_REG_UNKNOWN;slot->packet_attached=0;
        slot->roaming=0;slot->roaming_known=0;
        if(!slot->generation)slot->generation=1;
        slot->ims=GMR_IMS_OFF;slot->sms=GMR_SMS_IDLE;slot->sms_id=0;
        return gmr_qmi(r,GMR_QMI_SIM,gmr_operation(r,0),v->id.slot,GMR_NO_BEARER,NULL);
    case GMR_EV_REGISTRATION_LOST:
        if(v->id.service!=GMQ_SERVICE_NAS)return GMR_PROTOCOL;
        return gmr_begin_stop(r,1);
    case GMR_EV_NAS_CHANGED:
        if(v->id.service!=GMQ_SERVICE_NAS || v->value>GMR_REG_OFFLINE || (v->flags&~7u))return GMR_PROTOCOL;
        if((v->value!=GMR_REG_REGISTERED || !(v->flags&1u)) && gmr_slot_has_bearer(r,v->id.slot))
            return gmr_begin_stop(r,1);
        slot->registration=v->value;slot->packet_attached=(uint8_t)(v->flags&1u);
        slot->roaming=(uint8_t)((v->flags>>1)&1u);slot->roaming_known=(uint8_t)((v->flags>>2)&1u);return GMR_OK;
    case GMR_EV_IMS_STATUS:
        if(v->id.service!=0x21u || v->value>GMR_IMS_FAILED)return GMR_PROTOCOL;
        slot->ims=v->value;return GMR_OK;
    case GMR_EV_SMS_DELIVERED:
        if(v->id.service!=0x05u || slot->sms!=GMR_SMS_ACCEPTED || slot->sms_id!=v->value)return GMR_STALE;
        slot->sms=v->status?GMR_SMS_FAILED:GMR_SMS_DELIVERED;return GMR_OK;
    case GMR_EV_VOICE_ENDED:
        if(v->id.service!=0x09u || slot->call==GMR_CALL_IDLE || slot->call_id!=v->value)return GMR_STALE;
        for(i=0;i<GMR_PENDING;i++)if(r->pending[i].used && r->pending[i].action.id.slot==v->id.slot &&
            (r->pending[i].action.kind==GMR_ACT_MEDIA_START || r->pending[i].action.kind==GMR_ACT_VOICE_STOP)) {
            r->pending[i].used=0;r->state.pending_count--;
        }
        gmr_action_init(r,&a,GMR_ACT_MEDIA_DRAIN,gmr_operation(r,0),v->id.slot,GMR_NO_BEARER);
        slot->call=GMR_CALL_STOPPING;a.handle=v->value;return gmr_emit(r,&a);
    case GMR_EV_DATA_RX: {
        struct gmr_bearer *b;int e;unsigned free_index=GMR_EVENTS;
        if(v->id.service!=GMQ_SERVICE_WDS || v->id.bearer>=GMR_BEARERS)return GMR_PROTOCOL;
        b=&r->state.bearers[v->id.bearer];
        if(b->state!=GMR_BEARER_UP || b->slot!=v->id.slot || b->slot_generation!=v->id.slot_generation ||
            b->endpoint_generation!=v->id.endpoint_generation ||
            b->generation!=v->id.bearer_generation)return GMR_STALE;
        if(!gmr_packet_valid(v->payload,v->bytes,b->ip_family))return GMR_PROTOCOL;
        if(!r->ops.packet)return GMR_UNSUPPORTED;
        if(!v->id.opid)return GMR_ARGUMENT;
        for(i=0;i<GMR_EVENTS;i++) {
            if(r->rx_pending[i].used && gmr_identity_equal(&r->rx_pending[i].id,&v->id))return GMR_BUSY;
            if(!r->rx_pending[i].used && free_index==GMR_EVENTS)free_index=i;
        }
        if(free_index==GMR_EVENTS)return GMR_FULL;
        e=r->ops.packet(r->ops.ctx,&v->id,v->payload,v->bytes);if(e)return e;
        r->rx_pending[free_index].id=v->id;r->rx_pending[free_index].bytes=v->bytes;
        r->rx_pending[free_index].used=1;
        b->rx_bytes+=v->bytes;return GMR_OK;
    }
    case GMR_EV_DATA_ERROR:case GMR_EV_DATA_DELIVERED: {
        struct gmr_bearer *b;
        if(v->id.service!=GMQ_SERVICE_WDS || v->id.bearer>=GMR_BEARERS)return GMR_PROTOCOL;
        for(i=0;i<GMR_EVENTS;i++)if(r->rx_pending[i].used &&
            gmr_identity_equal(&r->rx_pending[i].id,&v->id))break;
        if(i==GMR_EVENTS)return GMR_STALE;
        if(v->kind==GMR_EV_DATA_DELIVERED && (!v->status && v->value!=r->rx_pending[i].bytes))return GMR_PROTOCOL;
        r->rx_pending[i].used=0;b=&r->state.bearers[v->id.bearer];
        if(b->state!=GMR_BEARER_UP || b->slot!=v->id.slot ||
            b->generation!=v->id.bearer_generation)return GMR_STALE;
        if(v->kind==GMR_EV_DATA_DELIVERED && !v->status){b->rx_delivered+=v->value;return GMR_OK;}
        r->state.last_error=(uint32_t)(v->status?v->status:GMR_IO);
        return gmr_bearer_remove(r,gmr_operation(r,0),v->id.bearer);
    }
    case GMR_EV_STORAGE_REQUEST:
        if(v->id.service!=0x0eu && v->id.service!=0x1cu)return GMR_PROTOCOL;
        if(!r->state.storage_running)return GMR_BUSY;
        return gmr_storage_request(r,v);
    default:return GMR_UNSUPPORTED;
    }
}
static inline int gmr_drain(struct gmr_runtime *r) {
    int first=GMR_OK,result;unsigned n=0;
    if(r->dispatching)return GMR_OK;
    r->dispatching=1;
    while(r->count) {
        struct gmr_event e=r->events[r->head];
        uint64_t stale_before=r->state.stale_events,protocol_before=r->state.protocol_errors;
        r->head=(r->head+1u)%GMR_EVENTS;r->count--;
        result=gmr_process_event(r,&e);if(!n++)first=result;
        if(result==GMR_STALE && r->state.stale_events==stale_before)r->state.stale_events++;
        if(result==GMR_PROTOCOL && r->state.protocol_errors==protocol_before)r->state.protocol_errors++;
        if(result && result!=GMR_STALE)r->state.last_error=(uint32_t)result;
    }
    r->dispatching=0;gmr_notify(r);return first;
}
static inline int gmr_event(struct gmr_runtime *r,const struct gmr_event *e) {
    if(!r || !e || e->bytes>GMR_PAYLOAD || e->status<0 || e->status>GMR_UNCERTAIN)return GMR_ARGUMENT;
    if(r->count==GMR_EVENTS)return GMR_FULL;
    r->events[(r->head+r->count)%GMR_EVENTS]=*e;r->count++;
    return gmr_drain(r);
}
static inline int gmr_tick(struct gmr_runtime *r,uint64_t now_ms) {
    unsigned i;int result=GMR_OK;
    if(!r || now_ms<r->state.now_ms)return GMR_ARGUMENT;
    if(r->dispatching)return GMR_BUSY;
    r->state.now_ms=now_ms;
    for(i=0;i<GMR_PENDING;i++)if(r->pending[i].used &&
        r->pending[i].action.deadline_ms<=now_ms) {
        struct gmr_action a=r->pending[i].action;
        r->pending[i].used=0;r->state.pending_count--;
        gmr_failure(r,&a,GMR_TIMEOUT,1);result=GMR_TIMEOUT;
    }
    gmr_notify(r);return result;
}
static inline int gmr_cancel(struct gmr_runtime *r,uint64_t opid) {
    unsigned i;int found=0;
    if(!r || !opid)return GMR_ARGUMENT;
    if(r->dispatching)return GMR_BUSY;
    for(i=0;i<GMR_PENDING;i++)if(r->pending[i].used && r->pending[i].action.id.opid==opid) {
        struct gmr_action a=r->pending[i].action;r->pending[i].used=0;r->state.pending_count--;found=1;
        gmr_failure(r,&a,GMR_CANCELLED,1);
    }
    if(found)gmr_notify(r);
    return found?GMR_OK:GMR_STALE;
}
#endif
