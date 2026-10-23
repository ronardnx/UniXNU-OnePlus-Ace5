/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Wire facts: libqmi d9e8af5cae7fdf9749baf87606568d3af4d03082,
 * src/libqmi-glib/qmi-message.{c,h}, qmi-enums-private.h, qmi-errors.h,
 * data/qmi-service-ctl.json. Original implementation, no libqmi dependency.
 */
#ifndef GIULIA_MODEM_QMUX_H
#define GIULIA_MODEM_QMUX_H
#include "giulia_modem_contract.h"

#define GMUX_HEADER 6u
#define GMUX_FRAME (GMR_PAYLOAD+GMUX_HEADER)
#define GMUX_CLIENTS 32u
#define GMUX_RECORDS 32u
#define GMUX_CTL_ALLOCATE 0x0022u
#define GMUX_CTL_RELEASE 0x0023u
enum gmux_output_kind { GMUX_NONE=0, GMUX_HOST=1, GMUX_PEER=2,
    GMUX_CLIENT_ALLOCATED=4, GMUX_CLIENT_RELEASED=8 };
enum gmux_client_state { GMUX_CLIENT_FREE, GMUX_CLIENT_LIVE, GMUX_CLIENT_RETIRED };
enum gmux_record_state { GMUX_RECORD_FREE, GMUX_RECORD_PENDING,
    GMUX_RECORD_DONE, GMUX_RECORD_CANCELLED };
struct gmux_view {
    uint8_t service,client,sender,flags;
    uint16_t transaction,message;
    const uint8_t *qmi,*tlvs;
    size_t qmi_bytes,tlv_bytes;
};
/* Output is a serialized frame or actual QRTR payload, never a pretend QMUX
 * prefix for QRTR. local_port identifies the dedicated socket for this CID.
 * id is suitable for the same epoch/slot/endpoint checks as gmr_identity. */
struct gmux_output {
    uint32_t kind,client_generation,local_port;
    uint8_t client;
    struct gmr_identity id;
    size_t bytes;
    uint8_t data[GMUX_FRAME];
};
struct gmux_endpoint { uint8_t live;struct gmr_identity id; };
struct gmux_client {
    uint8_t state,service,cid;
    uint32_t generation,local_port;
    struct gmr_identity endpoint;
};
struct gmux_record {
    uint8_t state,service,client,reply_client;
    uint16_t host_txn,message,wire_txn;
    uint32_t client_generation,local_port;
    uint64_t cookie;
    struct gmr_identity id;
    size_t request_bytes,reply_bytes;
    uint8_t request[GMUX_FRAME],reply[GMUX_FRAME];
};
/* One context per proven Apple QMUX transport/subscription. Routing never
 * calls gmr_event or changes SIM/NAS/data state. Unknown service TLVs are
 * forwarded only after framing validation; their semantics remain opaque.
 * All methods require the same serial executor as the runtime. */
struct gmux_context {
    uint64_t epoch,host_highwater;
    uint32_t slot_generation,next_client_generation;
    uint16_t next_cid,next_txn;
    uint8_t slot,draining;
    struct gmux_endpoint endpoints[GMR_SERVICES];
    struct gmux_client clients[GMUX_CLIENTS];
    struct gmux_record records[GMUX_RECORDS];
};

static inline int gmux_bytes_equal(const uint8_t *a,const uint8_t *b,size_t n) {
    size_t i;for(i=0;i<n;i++)if(a[i]!=b[i])return 0;return 1;
}
/* Strict complete-frame decoder. Stream framing is the ABP adapter's job:
 * gather length+1 bytes; concatenated/truncated/compound frames are rejected. */
static inline int gmux_decode(const void *data,size_t bytes,struct gmux_view *out) {
    const uint8_t *p=(const uint8_t *)data;struct gmux_view v;size_t h;
    if(!p || !out)return GMR_ARGUMENT;
    if(bytes<12u || bytes>GMUX_FRAME || p[0]!=1 || gmq_u16(p+1)!=bytes-1u ||
        (p[3]!=0 && p[3]!=0x80))return GMR_PROTOCOL;
    gmq_zero(&v,sizeof(v));v.sender=p[3];v.service=p[4];v.client=p[5];v.flags=p[6];
    h=v.service?7u:6u;
    if(bytes<GMUX_HEADER+h || (!v.service && v.client))return GMR_PROTOCOL;
    if(v.service) {
        if(v.flags!=0 && v.flags!=2 && v.flags!=4)return GMR_UNSUPPORTED;
        v.transaction=gmq_u16(p+7);v.message=gmq_u16(p+9);v.tlv_bytes=gmq_u16(p+11);
    } else {
        if(v.flags!=0 && v.flags!=1 && v.flags!=2)return GMR_UNSUPPORTED;
        v.transaction=p[7];v.message=gmq_u16(p+8);v.tlv_bytes=gmq_u16(p+10);
    }
    if(v.tlv_bytes!=bytes-GMUX_HEADER-h || (!v.sender && v.flags) ||
        (v.sender && !v.flags))return GMR_PROTOCOL;
    v.qmi=p+GMUX_HEADER;v.qmi_bytes=bytes-GMUX_HEADER;v.tlvs=p+GMUX_HEADER+h;
    if(gmq_tlvs(v.tlvs,v.tlv_bytes))return GMR_PROTOCOL;
    *out=v;return GMR_OK;
}
static inline int gmux_encode(void *data,size_t cap,uint8_t service,uint8_t client,
    const void *qmi,size_t bytes,size_t *used) {
    uint8_t *p=(uint8_t *)data;const uint8_t *q=(const uint8_t *)qmi;size_t h=service?7u:6u;
    struct gmux_view check;
    if(!used)return GMR_ARGUMENT;
    *used=0;if(!p || !q || (!service && client))return GMR_ARGUMENT;
    if(bytes<h || bytes>GMR_PAYLOAD || cap<bytes+GMUX_HEADER)return GMR_FULL;
    if(gmq_u16(q+(service?5u:4u))!=bytes-h || gmq_tlvs(q+h,bytes-h))return GMR_PROTOCOL;
    if((service && q[0]!=0 && q[0]!=2 && q[0]!=4) || (!service && q[0]>2))return GMR_UNSUPPORTED;
    /* Inputs must not overlap, as for the existing gmq builders. */
    p[0]=1;gmq_w16(p+1,(uint16_t)(bytes+5u));p[3]=q[0]?0x80:0;p[4]=service;p[5]=client;
    gmq_copy(p+GMUX_HEADER,q,bytes);
    if(gmux_decode(p,bytes+GMUX_HEADER,&check))return GMR_PROTOCOL;
    *used=bytes+GMUX_HEADER;return GMR_OK;
}
static inline int gmux_init(struct gmux_context *m,uint64_t epoch,uint8_t slot,uint32_t slot_generation) {
    if(!m || !epoch || slot>=GMR_SLOTS || !slot_generation)return GMR_ARGUMENT;
    gmq_zero(m,sizeof(*m));m->epoch=epoch;m->slot=slot;m->slot_generation=slot_generation;
    m->next_client_generation=1;m->next_cid=1;m->next_txn=1;return GMR_OK;
}
static inline struct gmux_endpoint *gmux_find_endpoint(struct gmux_context *m,uint16_t service) {
    unsigned i;for(i=0;i<GMR_SERVICES;i++)if(m->endpoints[i].id.service==service &&
        m->endpoints[i].id.endpoint_generation)return &m->endpoints[i];
    return NULL;
}
static inline struct gmux_client *gmux_find_client(struct gmux_context *m,uint8_t service,uint8_t cid) {
    unsigned i;for(i=0;i<GMUX_CLIENTS;i++)if(m->clients[i].state && m->clients[i].cid==cid &&
        m->clients[i].service==service)return &m->clients[i];
    return NULL;
}
static inline int gmux_source_equal(const struct gmr_identity *a,const struct gmr_identity *b) {
    return a->epoch==b->epoch && a->slot==b->slot && a->slot_generation==b->slot_generation &&
        a->service==b->service && a->endpoint_generation==b->endpoint_generation &&
        a->node==b->node && a->port==b->port;
}
static inline void gmux_cancel_service(struct gmux_context *m,uint16_t service) {
    unsigned i;
    for(i=0;i<GMUX_RECORDS;i++)if(m->records[i].state &&
        (m->records[i].service==service || m->records[i].id.service==service))
        m->records[i].state=GMUX_RECORD_CANCELLED;
    for(i=0;i<GMUX_CLIENTS;i++)if(m->clients[i].state && m->clients[i].service==service)
        m->clients[i].state=GMUX_CLIENT_RETIRED;
}
static inline int gmux_set_endpoint(struct gmux_context *m,const struct gmr_identity *id) {
    struct gmux_endpoint *e;unsigned i;
    if(!m || !id || !id->service || id->service>255u || !id->port || !id->endpoint_generation)return GMR_ARGUMENT;
    if(id->epoch!=m->epoch || id->slot!=m->slot || id->slot_generation!=m->slot_generation)return GMR_STALE;
    e=gmux_find_endpoint(m,id->service);
    if(e) {
        if(e->live && gmux_source_equal(&e->id,id))return GMR_OK;
        if(id->endpoint_generation<=e->id.endpoint_generation)return GMR_STALE;
        gmux_cancel_service(m,id->service);
    } else {
        for(i=0;i<GMR_SERVICES;i++)if(!m->endpoints[i].id.endpoint_generation){e=&m->endpoints[i];break;}
        if(!e)return GMR_FULL;
    }
    e->live=1;e->id=*id;return GMR_OK;
}
static inline int gmux_drop_endpoint(struct gmux_context *m,const struct gmr_identity *id) {
    struct gmux_endpoint *e;
    if(!m || !id)return GMR_ARGUMENT;
    e=gmux_find_endpoint(m,id->service);
    if(!e || !e->live || !gmux_source_equal(&e->id,id))return GMR_STALE;
    e->live=0;gmux_cancel_service(m,id->service);return GMR_OK;
}
/* QMUX Allocate creates a virtual CID only. Before publishing that reply to
 * Apple, the adapter must create a dedicated QRTR socket, call bind_client
 * with its actual nonzero local port, and retain that socket's generation.
 * No two live virtual clients may share a local QRTR port. */
static inline int gmux_bind_client(struct gmux_context *m,uint8_t service,uint8_t cid,
    uint32_t generation,uint32_t local_port) {
    struct gmux_client *c;unsigned i;
    if(!m || !local_port)return GMR_ARGUMENT;
    c=gmux_find_client(m,service,cid);
    if(!c || c->state!=GMUX_CLIENT_LIVE || c->generation!=generation || c->endpoint.epoch!=m->epoch)return GMR_STALE;
    if(c->local_port)return c->local_port==local_port?GMR_OK:GMR_BUSY;
    for(i=0;i<GMUX_CLIENTS;i++)if(m->clients[i].local_port==local_port)return GMR_BUSY;
    c->local_port=local_port;return GMR_OK;
}
/* Called only after the native socket is actually closed/quiesced. Release
 * CID refuses a bound client, so it cannot acknowledge a leaked route. */
static inline int gmux_unbind_client(struct gmux_context *m,uint8_t service,uint8_t cid,uint32_t generation) {
    struct gmux_client *c;unsigned i;int ports=0;
    if(!m)return GMR_ARGUMENT;
    c=gmux_find_client(m,service,cid);if(!c || c->generation!=generation)return GMR_STALE;
    c->local_port=0;
    for(i=0;i<GMUX_RECORDS;i++)if(m->records[i].state &&
        ((m->records[i].service==service && m->records[i].client==cid) ||
         (!m->records[i].service && m->records[i].id.service==service && m->records[i].reply_client==cid)))
        m->records[i].state=GMUX_RECORD_CANCELLED;
    if(c->state==GMUX_CLIENT_RETIRED)gmq_zero(c,sizeof(*c));
    else if(m->next_client_generation)c->generation=m->next_client_generation++;
    else c->state=GMUX_CLIENT_RETIRED;
    for(i=0;i<GMUX_CLIENTS;i++)if(m->clients[i].local_port)ports=1;
    if(!ports)m->draining=0;
    return GMR_OK;
}
static inline int gmux_reset(struct gmux_context *m,uint64_t epoch,uint32_t slot_generation) {
    unsigned i;
    if(!m || epoch<m->epoch || !slot_generation ||
        (epoch==m->epoch && slot_generation<=m->slot_generation))return GMR_ARGUMENT;
    /* A subscription switch can advance only slot_generation. Preserve the
     * cookie/CID/transaction highwaters then: old host frames have no slot
     * generation on the wire and must not alias a newly allocated client. */
    if(epoch!=m->epoch){m->host_highwater=0;m->next_cid=1;m->next_txn=1;}
    m->epoch=epoch;m->slot_generation=slot_generation;
    gmq_zero(m->endpoints,sizeof(m->endpoints));gmq_zero(m->records,sizeof(m->records));m->draining=0;
    for(i=0;i<GMUX_CLIENTS;i++) {
        if(m->clients[i].local_port){m->clients[i].state=GMUX_CLIENT_RETIRED;m->draining=1;}
        else gmq_zero(&m->clients[i],sizeof(m->clients[i]));
    }
    return GMR_OK;
}
static inline int gmux_cancel(struct gmux_context *m,uint64_t cookie) {
    unsigned i;if(!m || !cookie)return GMR_ARGUMENT;
    for(i=0;i<GMUX_RECORDS;i++)if(m->records[i].state && m->records[i].cookie==cookie) {
        m->records[i].state=GMUX_RECORD_CANCELLED;return GMR_OK;
    }
    return GMR_STALE;
}
static inline struct gmux_record *gmux_reserve(struct gmux_context *m) {
    unsigned i;struct gmux_record *old=NULL;
    for(i=0;i<GMUX_RECORDS;i++) {
        if(!m->records[i].state)return &m->records[i];
        if(m->records[i].state!=GMUX_RECORD_PENDING && (!old || m->records[i].cookie<old->cookie))old=&m->records[i];
    }
    return old;
}
static inline int gmux_ctl_reply(const struct gmux_view *v,uint16_t error,
    uint8_t service,uint8_t cid,struct gmux_output *out) {
    uint8_t q[18];size_t n=13u;
    gmq_zero(q,sizeof(q));q[0]=1;q[1]=(uint8_t)v->transaction;gmq_w16(q+2,v->message);
    q[6]=2;gmq_w16(q+7,4);gmq_w16(q+9,(uint16_t)(error!=0));gmq_w16(q+11,error);
    if(!error){q[13]=1;gmq_w16(q+14,2);q[16]=service;q[17]=cid;n=18;}
    gmq_w16(q+4,(uint16_t)(n-6u));out->kind|=GMUX_HOST;
    return gmux_encode(out->data,sizeof(out->data),0,0,q,n,&out->bytes);
}
/* cookie is an adapter-assigned monotonic logical request id, stable for a
 * retry; changing frame bytes under one cookie is a protocol error. The raw
 * host (service,CID,transaction) remains independently checked while pending.
 * A completed cached retry returns the exact reply, never another peer send.
 * Evicted old cookies are stale. Request transaction reuse with a new cookie
 * is allowed only after the preceding request has completed/cancelled. */
static inline int gmux_from_host(struct gmux_context *m,uint64_t epoch,uint64_t cookie,
    const void *frame,size_t bytes,struct gmux_output *out) {
    struct gmux_view v;struct gmux_record *record;struct gmux_client *c=NULL;
    struct gmux_endpoint *endpoint=NULL;unsigned i;int e;uint8_t service=0,cid=0;uint16_t error=0;
    struct gmq_reply fields;const uint8_t *field;size_t field_bytes;
    if(!m || !out || !cookie)return GMR_ARGUMENT;
    gmq_zero(out,sizeof(*out));
    if(epoch!=m->epoch)return GMR_STALE;
    if(m->draining)return GMR_BUSY;
    e=gmux_decode(frame,bytes,&v);if(e)return e;
    if(v.sender || v.flags)return GMR_PROTOCOL;
    for(i=0;i<GMUX_RECORDS;i++)if(m->records[i].state && m->records[i].cookie==cookie) {
        record=&m->records[i];
        if(record->request_bytes!=bytes || !gmux_bytes_equal(record->request,(const uint8_t *)frame,bytes))return GMR_PROTOCOL;
        if(record->state==GMUX_RECORD_PENDING)return GMR_BUSY;
        if(record->state==GMUX_RECORD_CANCELLED)return GMR_CANCELLED;
        out->kind=GMUX_HOST;out->bytes=record->reply_bytes;out->id=record->id;
        out->client=record->reply_client;out->client_generation=record->client_generation;
        out->local_port=record->local_port;
        gmq_copy(out->data,record->reply,record->reply_bytes);return GMR_OK;
    }
    if(cookie<=m->host_highwater)return GMR_STALE;
    for(i=0;i<GMUX_RECORDS;i++)if(m->records[i].state==GMUX_RECORD_PENDING &&
        m->records[i].service==v.service && m->records[i].client==v.client && m->records[i].host_txn==v.transaction)
        return GMR_BUSY;
    record=gmux_reserve(m);if(!record)return GMR_FULL;
    if(v.service) {
        c=gmux_find_client(m,v.service,v.client);
        if(!c || c->state!=GMUX_CLIENT_LIVE || !c->local_port)return GMR_STATE;
        endpoint=gmux_find_endpoint(m,v.service);
        if(!endpoint || !endpoint->live || !gmux_source_equal(&endpoint->id,&c->endpoint))return GMR_STALE;
        if(!m->next_txn)return GMR_FULL;
    } else {
        fields.tlvs=v.tlvs;fields.bytes=v.tlv_bytes;fields.remote_error=0;
        field=gmq_field(&fields,1,&field_bytes);
        if(v.message!=GMUX_CTL_ALLOCATE && v.message!=GMUX_CTL_RELEASE)error=94;
        else if(!field || field_bytes!=(v.message==GMUX_CTL_ALLOCATE?1u:2u))error=1;
        else {
            service=field[0];cid=v.message==GMUX_CTL_RELEASE?field[1]:0;
            if(!service)error=31;
            else if(v.message==GMUX_CTL_ALLOCATE) {
                endpoint=gmux_find_endpoint(m,service);
                if(!endpoint || !endpoint->live)error=31;
                else {
                    for(i=0;i<GMUX_CLIENTS;i++)if(!m->clients[i].state){c=&m->clients[i];break;}
                    if(!c || m->next_cid>=255u || !m->next_client_generation)error=5;
                }
            } else {
                c=gmux_find_client(m,service,cid);
                if(!c)error=7;
                else if(c->local_port) {
                    /* Return a close hint without committing/caching success. */
                    out->client=c->cid;out->client_generation=c->generation;
                    out->local_port=c->local_port;out->id=c->endpoint;return GMR_BUSY;
                }
            }
        }
    }
    gmq_zero(record,sizeof(*record));record->cookie=cookie;record->service=v.service;
    record->client=v.client;record->host_txn=v.transaction;record->message=v.message;
    record->request_bytes=bytes;gmq_copy(record->request,frame,bytes);m->host_highwater=cookie;
    if(!v.service) {
        if(!error && v.message==GMUX_CTL_ALLOCATE) {
            gmq_zero(c,sizeof(*c));c->state=GMUX_CLIENT_LIVE;c->service=service;c->cid=(uint8_t)m->next_cid++;
            c->generation=m->next_client_generation++;c->endpoint=endpoint->id;cid=c->cid;
            out->kind=GMUX_CLIENT_ALLOCATED;out->id=c->endpoint;out->client_generation=c->generation;
        } else if(!error) {
            out->kind=GMUX_CLIENT_RELEASED;out->id=c->endpoint;out->client_generation=c->generation;
            for(i=0;i<GMUX_RECORDS;i++)if(&m->records[i]!=record && m->records[i].state &&
                ((m->records[i].service==service && m->records[i].client==cid) ||
                 (!m->records[i].service && m->records[i].id.service==service && m->records[i].reply_client==cid)))
                m->records[i].state=GMUX_RECORD_CANCELLED;
            gmq_zero(c,sizeof(*c));
        }
        out->client=cid;out->id.epoch=m->epoch;out->id.opid=cookie;
        out->id.slot=m->slot;out->id.slot_generation=m->slot_generation;
        out->id.txn=v.transaction;out->id.bearer=GMR_NO_BEARER;
        e=gmux_ctl_reply(&v,error,service,cid,out);if(e)return e;
        record->state=GMUX_RECORD_DONE;record->reply_bytes=out->bytes;
        record->id=out->id;record->client_generation=out->client_generation;record->reply_client=cid;
        gmq_copy(record->reply,out->data,out->bytes);return GMR_OK;
    }
    record->state=GMUX_RECORD_PENDING;record->client_generation=c->generation;record->local_port=c->local_port;
    record->reply_client=c->cid;
    record->wire_txn=m->next_txn++;record->id=c->endpoint;record->id.opid=cookie;
    record->id.txn=record->wire_txn;record->id.bearer=GMR_NO_BEARER;
    out->kind=GMUX_PEER;out->client=c->cid;out->client_generation=c->generation;
    out->local_port=c->local_port;out->id=record->id;out->bytes=v.qmi_bytes;
    gmq_copy(out->data,v.qmi,v.qmi_bytes);gmq_w16(out->data+1,record->wire_txn);return GMR_OK;
}
/* Route by the actual receiving QRTR socket + virtual-client generation,
 * never infer a client from a server endpoint (several clients may share it).
 * Responses restore the host's original 16-bit transaction. Indications have
 * no pending transaction and are delivered only to this bound live client. */
static inline int gmux_from_peer(struct gmux_context *m,const struct gmr_identity *source,
    uint8_t cid,uint32_t client_generation,uint32_t local_port,const void *qmi,size_t bytes,
    struct gmux_output *out) {
    const uint8_t *p=(const uint8_t *)qmi;struct gmux_client *c;struct gmux_endpoint *ep;
    struct gmux_record *record=NULL;struct gmq_reply response;unsigned i;uint16_t txn,msg;int e;
    if(!m || !source || !p || !out)return GMR_ARGUMENT;
    gmq_zero(out,sizeof(*out));
    if(source->epoch!=m->epoch || source->service>255u)return GMR_STALE;
    c=gmux_find_client(m,(uint8_t)source->service,cid);ep=gmux_find_endpoint(m,source->service);
    if(!c || c->state!=GMUX_CLIENT_LIVE || c->generation!=client_generation || !local_port ||
        c->local_port!=local_port || !ep || !ep->live || !gmux_source_equal(&ep->id,source) ||
        !gmux_source_equal(&c->endpoint,source))return GMR_STALE;
    if(bytes<GMQ_HEADER || bytes>GMR_PAYLOAD || gmq_u16(p+5)!=bytes-GMQ_HEADER ||
        gmq_tlvs(p+GMQ_HEADER,bytes-GMQ_HEADER))return GMR_PROTOCOL;
    if(p[0]!=2 && p[0]!=4)return GMR_UNSUPPORTED;
    txn=gmq_u16(p+1);msg=gmq_u16(p+3);
    if(p[0]==2) {
        for(i=0;i<GMUX_RECORDS;i++)if(m->records[i].state==GMUX_RECORD_PENDING &&
            m->records[i].service==source->service && m->records[i].client==cid &&
            m->records[i].client_generation==client_generation && m->records[i].local_port==local_port &&
            m->records[i].wire_txn==txn && m->records[i].message==msg &&
            gmux_source_equal(&m->records[i].id,source)){record=&m->records[i];break;}
        if(!record)return GMR_STALE;
        e=gmq_response(p,bytes,txn,msg,&response);
        if(e!=GMQ_OK && e!=GMQ_REMOTE)return GMR_PROTOCOL;
    }
    e=gmux_encode(out->data,sizeof(out->data),(uint8_t)source->service,cid,p,bytes,&out->bytes);if(e)return e;
    out->kind=GMUX_HOST;out->client=cid;out->client_generation=client_generation;
    out->local_port=local_port;out->id=*source;
    if(record) {
        gmq_w16(out->data+7,record->host_txn);out->id=record->id;
        record->state=GMUX_RECORD_DONE;record->reply_bytes=out->bytes;
        gmq_copy(record->reply,out->data,out->bytes);
    }
    return GMR_OK;
}
#endif
