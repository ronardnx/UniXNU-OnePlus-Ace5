/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_MODEM_DATA_H
#define GIULIA_MODEM_DATA_H
#include "giulia_modem_runtime.h"
#include "giulia_modem_qmap.h"

/* Invoke on the same executor as the core. Native DMA owners retain the
 * immutable identity from DMA_CONFIGURE with each mux/ring binding. A shared
 * ring must retain those bindings with its submitted descriptors until drain;
 * looking up the current bearer to label a completed old descriptor is invalid.
 * The nonzero packet cookie identifies one receipt. Version is negotiated WDA. */
static inline int gmd_receive(struct gmr_runtime *r,const struct gmr_identity *binding,
    uint64_t cookie,const uint8_t *bytes,size_t size,unsigned version,size_t *consumed)
{
    struct gmap_packet frame;struct gmr_event event;
    struct gmr_service *endpoint;struct gmr_bearer *bearer;int result;
    if(!r || !consumed || !cookie || !binding || binding->bearer>=GMR_BEARERS ||
        binding->slot>=GMR_SLOTS || binding->service!=GMQ_SERVICE_WDS)return GMR_ARGUMENT;
    *consumed=0;
    if(binding->epoch!=r->state.epoch || r->state.lifecycle!=GMR_ONLINE)return GMR_STALE;
    result=gmap_decode(bytes,size,version,&frame);
    if(result)return result==GMAP_UNSUPPORTED?GMR_UNSUPPORTED:GMR_PROTOCOL;
    bearer=&r->state.bearers[binding->bearer];
    if(bearer->state!=GMR_BEARER_UP || bearer->mux!=frame.mux || bearer->slot!=binding->slot ||
        !binding->bearer_generation || bearer->generation!=binding->bearer_generation ||
        bearer->slot_generation!=binding->slot_generation ||
        bearer->slot_generation!=r->state.slots[bearer->slot].generation)return GMR_STALE;
    endpoint=gmr_service_find(r,GMQ_SERVICE_WDS,bearer->slot);
    if(!endpoint || !endpoint->available || endpoint->generation!=bearer->endpoint_generation ||
        endpoint->generation!=binding->endpoint_generation || endpoint->node!=binding->node ||
        endpoint->port!=binding->port)return GMR_STALE;
    if(!gmr_packet_valid(frame.ip,frame.bytes,bearer->ip_family))return GMR_PROTOCOL;
    if((bearer->ip_family==4 && (bearer->ipv4.present&GMQ_IPV4_MTU) && frame.bytes>bearer->ipv4.mtu) ||
       (bearer->ip_family==6 && (bearer->ipv6.present&16u) && frame.bytes>bearer->ipv6.mtu))return GMR_PROTOCOL;
    gmq_zero(&event,sizeof(event));event.kind=GMR_EV_DATA_RX;
    event.id=*binding;event.id.opid=cookie;event.id.txn=0;
    event.bytes=(uint16_t)frame.bytes;gmq_copy(event.payload,frame.ip,frame.bytes);
    result=gmr_event(r,&event);if(!result)*consumed=frame.consumed;return result;
}

/* Use a core snapshot from the action's executor, never an independently
 * maintained "connected" flag. The caller retains DMA ownership until the
 * corresponding action completion is observed. */
static inline int gmd_transmit(const struct gmr_snapshot *s,const struct gmr_action *a,
    unsigned version,uint8_t *wire,size_t capacity,size_t *used)
{
    const struct gmr_bearer *b;unsigned i;const struct gmr_service *endpoint=NULL;
    if(!s || !a || !used)return GMR_ARGUMENT;
    *used=0;
    if(a->kind!=GMR_ACT_DATA_TX || a->id.bearer>=GMR_BEARERS || a->id.slot>=GMR_SLOTS)return GMR_ARGUMENT;
    b=&s->bearers[a->id.bearer];
    if(s->lifecycle!=GMR_ONLINE || a->id.epoch!=s->epoch || b->state!=GMR_BEARER_UP ||
       b->slot!=a->id.slot || b->slot_generation!=a->id.slot_generation ||
       b->generation!=a->id.bearer_generation ||
       s->slots[b->slot].generation!=a->id.slot_generation || b->handle!=a->handle ||
       a->id.service!=GMQ_SERVICE_WDS || a->id.endpoint_generation!=b->endpoint_generation)return GMR_STALE;
    for(i=0;i<GMR_SERVICES;i++)if(s->services[i].available && s->services[i].slot==b->slot &&
        s->services[i].service==GMQ_SERVICE_WDS){endpoint=&s->services[i];break;}
    if(!endpoint || endpoint->generation!=a->id.endpoint_generation || endpoint->node!=a->id.node ||
       endpoint->port!=a->id.port)return GMR_STALE;
    if(!gmr_packet_valid(a->payload,a->bytes,b->ip_family))return GMR_PROTOCOL;
    if((b->ip_family==4 && (b->ipv4.present&GMQ_IPV4_MTU) && a->bytes>b->ipv4.mtu) ||
       (b->ip_family==6 && (b->ipv6.present&16u) && a->bytes>b->ipv6.mtu))return GMR_PROTOCOL;
    return gmap_encode(wire,capacity,a->payload,a->bytes,b->mux,version,used)==GMAP_OK?GMR_OK:GMR_PROTOCOL;
}
#endif
