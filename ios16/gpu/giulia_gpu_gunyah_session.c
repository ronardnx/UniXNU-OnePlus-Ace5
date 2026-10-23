/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#if !defined(GG_GUNYAH_SESSION_STANDALONE)
#include <pexpert/arm64/board_config.h>
#include <sys/param.h>
#include <sys/systm.h>
#endif
#if defined(GG_GUNYAH_SESSION_STANDALONE) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "giulia_gpu_gunyah_session.h"

static int ggrs_range(const void *p,size_t bytes)
{return p && bytes && bytes<=UINTPTR_MAX-(uintptr_t)p;}

static int ggrs_outside(const struct ggrs_session *s,const void *p,size_t bytes)
{
    return ggrs_range(s,sizeof(*s)) && ggrs_range(p,bytes) &&
        ((uintptr_t)p+bytes<=(uintptr_t)s || (uintptr_t)s+sizeof(*s)<=(uintptr_t)p);
}

static int ggrs_enter(struct ggrs_session *s)
{
    unsigned expected=0;
    if(!s)return GGRS_ARGUMENT;
    unsigned initialized=__atomic_load_n(&s->initialized,__ATOMIC_ACQUIRE);
    if(initialized!=1)return initialized==2?GGRS_BUSY:GGRS_ARGUMENT;
    return __atomic_compare_exchange_n(&s->busy,&expected,1u,0,__ATOMIC_ACQUIRE,
        __ATOMIC_RELAXED)?GGRS_OK:GGRS_BUSY;
}

static void ggrs_loss(struct ggrs_session *s)
{
    __atomic_store_n(&s->loss_pending,1u,__ATOMIC_RELEASE);
    s->invalid=1;s->rm.quarantined=1;gg_rq_invalidate(&s->snapshot);
}

static int ggrs_lost(const struct ggrs_session *s)
{return __atomic_load_n(&s->loss_pending,__ATOMIC_ACQUIRE)!=0;}

static int ggrs_leave(struct ggrs_session *s,unsigned result)
{
    if(ggrs_lost(s)) {
        ggrs_loss(s);
        if(result==GGRS_OK || result==GGRS_IDLE || result==GGRS_BUDGET)result=GGRS_LEASE;
    }
    s->timing=0;s->last_result=result;
    __atomic_store_n(&s->busy,0u,__ATOMIC_RELEASE);return (int)result;
}

/* One clock history covers callbacks, all seven RPCs and the final lease
 * check. Never rebase it at an RPC boundary. No callback can be preempted,
 * but an overrun prevents later I/O and prevents a successful result. */
static int ggrs_clock(struct ggrs_session *s)
{
    if(ggrs_lost(s) || s->time_failed)return 0;
    if(!s->timing)return 1;
    uint64_t now=s->owner.now_us(s->owner.cookie);
    if(ggrs_lost(s))return 0;
    if(now<s->last_us || now-s->start_us>=s->timeout_us) {
        s->time_failed=1;return 0;
    }
    s->last_us=now;return 1;
}

static int ggrs_valid(void *cookie,uint64_t epoch)
{
    struct ggrs_session *s=(struct ggrs_session *)cookie;
    return epoch==s->epoch && !s->invalid && !ggrs_lost(s) &&
        s->owner.valid(s->owner.cookie,epoch)==1 && !s->invalid &&
        !ggrs_lost(s) && ggrs_clock(s);
}

static int ggrs_send(void *cookie,const void *p,size_t bytes,unsigned flags)
{
    struct ggrs_session *s=(struct ggrs_session *)cookie;
    if(!ggrs_valid(s,s->epoch))return -1;
    int result=s->owner.send(s->owner.cookie,p,bytes,flags);
    return ggrs_clock(s)?result:-1;
}

static int ggrs_receive(void *cookie,void *p,size_t capacity,size_t *bytes)
{
    struct ggrs_session *s=(struct ggrs_session *)cookie;
    if(!ggrs_valid(s,s->epoch))return -1;
    int result=s->owner.receive(s->owner.cookie,p,capacity,bytes);
    return ggrs_clock(s)?result:-1;
}

static uint64_t ggrs_now(void *cookie)
{
    struct ggrs_session *s=(struct ggrs_session *)cookie;
    if(s->timing) {
        (void)ggrs_clock(s);return s->last_us;
    }
    return s->owner.now_us(s->owner.cookie);
}

static int ggrs_delay(void *cookie,unsigned us)
{
    struct ggrs_session *s=(struct ggrs_session *)cookie;
    if(!ggrs_valid(s,s->epoch))return -1;
    if(s->timing && us>=s->timeout_us-(unsigned)(s->last_us-s->start_us)) {
        s->time_failed=1;return -1;
    }
    int result=s->owner.delay_us(s->owner.cookie,us);
    return ggrs_clock(s)?result:-1;
}

static int ggrs_deliver(void *cookie,uint32_t id,const void *p,size_t bytes)
{
    struct ggrs_session *s=(struct ggrs_session *)cookie;
    int codec=gg_rq_notification(&s->snapshot,id,(const uint8_t *)p,bytes);
    int handled=s->owner.notification(s->owner.cookie,id,p,bytes);
    if(codec || handled!=1 || !ggrs_clock(s)){ggrs_loss(s);return 0;}
    return 1;
}

static int ggrs_native_live(void *cookie,uint64_t epoch,uint64_t tx,uint64_t rx)
{
    struct ggrs_session *s=(struct ggrs_session *)cookie;
    return !ggrs_lost(s) && s->native_live &&
        s->native_live(s->native_lease_cookie,epoch,tx,rx)==1 && ggrs_clock(s);
}

static int ggrs_native_valid(void *cookie,uint64_t epoch)
{
    struct ggrs_session *s=(struct ggrs_session *)cookie;
    return ggh_hvc_valid(&s->native_hvc,epoch);
}

static int ggrs_native_send(void *cookie,const void *p,size_t bytes,unsigned flags)
{
    struct ggrs_session *s=(struct ggrs_session *)cookie;
    return ggh_hvc_send(&s->native_hvc,p,bytes,flags);
}

static int ggrs_native_receive(void *cookie,void *p,size_t capacity,size_t *bytes)
{
    struct ggrs_session *s=(struct ggrs_session *)cookie;
    return ggh_hvc_recv(&s->native_hvc,p,capacity,bytes);
}

static uint64_t ggrs_native_now(void *cookie)
{
    struct ggrs_session *s=(struct ggrs_session *)cookie;
    return s->events.now_us(s->events.cookie);
}

static int ggrs_native_delay(void *cookie,unsigned us)
{
    struct ggrs_session *s=(struct ggrs_session *)cookie;
    return s->events.delay_us(s->events.cookie,us);
}

static int ggrs_native_notification(void *cookie,uint32_t id,const void *p,size_t bytes)
{
    struct ggrs_session *s=(struct ggrs_session *)cookie;
    return s->events.notification(s->events.cookie,id,p,bytes);
}

static int ggrs_init_begin(struct ggrs_session *s)
{
    unsigned expected=0;
    if(!__atomic_compare_exchange_n(&s->initialized,&expected,2u,0,
            __ATOMIC_ACQUIRE,__ATOMIC_RELAXED))
        return expected==2?GGRS_BUSY:GGRS_ARGUMENT;
    if(s->busy || s->attempted || s->invalid || s->epoch || s->rm.epoch ||
       s->rm.quarantined || s->snapshot.epoch || ggrs_lost(s)) {
        __atomic_store_n(&s->initialized,0u,__ATOMIC_RELEASE);return GGRS_ARGUMENT;
    }
    return GGRS_OK;
}

static int ggrs_init_finish(struct ggrs_session *s,const struct ggrm_io *io,uint64_t epoch)
{
    s->owner=*io;s->epoch=epoch;
    unsigned result=GGRS_OK;
    if(ggrm_init(&s->rm,epoch)!=GGRM_OK || gg_rq_begin(&s->snapshot,epoch)) {
        ggrs_loss(s);result=GGRS_ARGUMENT;
    } else if(!ggrs_valid(s,epoch)) {
        ggrs_loss(s);result=GGRS_LEASE;
    }
    s->last_result=result;
    __atomic_store_n(&s->initialized,1u,__ATOMIC_RELEASE);return (int)result;
}

int ggrs_init(struct ggrs_session *s,const struct ggrm_io *io,uint64_t epoch)
{
    if(!ggrs_range(s,sizeof(*s)) || !ggrs_outside(s,io,sizeof(*io)) || !epoch ||
       !io->valid || !io->send || !io->receive || !io->now_us ||
       !io->delay_us || !io->notification)return GGRS_ARGUMENT;
    int result=ggrs_init_begin(s);if(result)return result;
    struct ggrm_io copy=*io;
    return ggrs_init_finish(s,&copy,epoch);
}

int ggrs_init_hvc(struct ggrs_session *s,const struct ggh_hvc *hvc,
    const struct ggrs_events *events,uint64_t epoch)
{
    if(!ggrs_range(s,sizeof(*s)) || !ggrs_outside(s,hvc,sizeof(*hvc)) ||
       !ggrs_outside(s,events,sizeof(*events)) || !epoch || hvc->epoch!=epoch ||
       !events->now_us || !events->delay_us || !events->notification)return GGRS_ARGUMENT;
    int result=ggrs_init_begin(s);if(result)return result;
    s->native_hvc=*hvc;s->events=*events;
    s->native_live=hvc->live;s->native_lease_cookie=hvc->lease_cookie;
    s->native_hvc.live=ggrs_native_live;s->native_hvc.lease_cookie=s;
    struct ggrm_io io={s,ggrs_native_valid,ggrs_native_send,ggrs_native_receive,
        ggrs_native_now,ggrs_native_delay,ggrs_native_notification};
    return ggrs_init_finish(s,&io,epoch);
}

int ggrs_probe(struct ggrs_session *s,unsigned timeout_us)
{
    int result=ggrs_enter(s);if(result)return result;
    if(!timeout_us || timeout_us>3000000)return ggrs_leave(s,GGRS_ARGUMENT);
    if(s->invalid || ggrs_lost(s) || s->attempted)return ggrs_leave(s,GGRS_INVALID);
    s->attempted=1;
    s->timeout_us=timeout_us;
    s->start_us=s->last_us=s->owner.now_us(s->owner.cookie);s->timing=1;
    if(!ggrs_valid(s,s->epoch)) {
        ggrs_loss(s);return ggrs_leave(s,s->time_failed?GGRS_RPC:GGRS_LEASE);
    }
    struct ggrm_io bridge={s,ggrs_valid,ggrs_send,ggrs_receive,ggrs_now,ggrs_delay,ggrs_deliver};
    for(unsigned step=0;step<GGRQ_STEP_COMPLETE;step++) {
        uint32_t id=0;size_t bytes=0;
        if(!ggrs_valid(s,s->epoch)) {
            ggrs_loss(s);return ggrs_leave(s,s->time_failed?GGRS_RPC:GGRS_LEASE);
        }
        if(gg_rq_build_query(&s->snapshot,&id,s->request,sizeof(s->request),&bytes)) {
            ggrs_loss(s);return ggrs_leave(s,GGRS_CODEC);
        }
        result=ggrm_call(&bridge,&s->rm,id,s->request,bytes,
            timeout_us-(unsigned)(s->last_us-s->start_us));
        if(result!=GGRM_OK) {
            ggrs_loss(s);return ggrs_leave(s,
                !s->time_failed && result==GGRM_LEASE?GGRS_LEASE:GGRS_RPC);
        }
        if(gg_rq_accept_reply(&s->snapshot,s->epoch,id,s->rm.reply.data,s->rm.reply.bytes)) {
            ggrs_loss(s);return ggrs_leave(s,GGRS_CODEC);
        }
    }
    if(!gg_rq_snapshot_complete(&s->snapshot) || !ggrs_valid(s,s->epoch)) {
        ggrs_loss(s);return ggrs_leave(s,s->time_failed?GGRS_RPC:GGRS_LEASE);
    }
    return ggrs_leave(s,GGRS_OK);
}

int ggrs_poll(struct ggrs_session *s,unsigned frame_budget,unsigned timeout_us)
{
    int result=ggrs_enter(s);if(result)return result;
    if(!frame_budget || frame_budget>GGRS_MAX_POLL_FRAMES ||
       !timeout_us || timeout_us>3000000)return ggrs_leave(s,GGRS_ARGUMENT);
    if(s->invalid || ggrs_lost(s) || !gg_rq_snapshot_complete(&s->snapshot))
        return ggrs_leave(s,GGRS_INVALID);
    if(s->rm.busy || s->rm.quarantined || s->rm.epoch!=s->epoch ||
       s->rm.notification.active || s->rm.notification.done) {
        ggrs_loss(s);return ggrs_leave(s,GGRS_CODEC);
    }
    s->poll_frames=s->poll_notifications=0;
    uint64_t start=s->owner.now_us(s->owner.cookie);
    if(start<s->last_us) {
        s->time_failed=1;ggrs_loss(s);return ggrs_leave(s,GGRS_RPC);
    }
    s->timeout_us=timeout_us;s->start_us=s->last_us=start;s->timing=1;
    for(unsigned frame=0;frame<frame_budget;frame++) {
        if(!ggrs_valid(s,s->epoch)) {
            ggrs_loss(s);return ggrs_leave(s,s->time_failed?GGRS_RPC:GGRS_LEASE);
        }
        size_t bytes=0;
        int status=ggrs_receive(s,s->rm.rx,sizeof(s->rm.rx),&bytes);
        if(!ggrs_valid(s,s->epoch)) {
            ggrs_loss(s);return ggrs_leave(s,s->time_failed?GGRS_RPC:GGRS_LEASE);
        }
        if(status==1) {
            if(bytes || s->rm.notification.active) {
                ggrs_loss(s);return ggrs_leave(s,GGRS_CODEC);
            }
            return ggrs_leave(s,GGRS_IDLE);
        }
        if(status) {ggrs_loss(s);return ggrs_leave(s,GGRS_RPC);}
        ++s->poll_frames;++s->rm.received_frames;
        /* There is no outstanding request. Never adopt a late reply as an
         * idle notification, even if it matches the last query's seq/id. */
        if(bytes<8 || bytes>GGRM_FRAME ||
           ((s->rm.rx[1]&3u)!=0 && (s->rm.rx[1]&3u)!=3) ||
           !ggrm_receive_frame(&s->rm,0,bytes)) {
            ggrs_loss(s);return ggrs_leave(s,GGRS_CODEC);
        }
        if(s->rm.notification.done) {
            int handled=ggrs_deliver(s,s->rm.notification.id,
                s->rm.notification.data,s->rm.notification.bytes);
            ++s->poll_notifications;
            if(!handled)return ggrs_leave(s,GGRS_CODEC);
            if(!ggrs_valid(s,s->epoch)) {
                ggrs_loss(s);return ggrs_leave(s,s->time_failed?GGRS_RPC:GGRS_LEASE);
            }
            ggrm_message_reset(&s->rm.notification);
        }
    }
    if(s->rm.notification.active) {
        ggrs_loss(s);return ggrs_leave(s,GGRS_CODEC);
    }
    return ggrs_leave(s,GGRS_BUDGET);
}

int ggrs_notification(struct ggrs_session *s,uint32_t id,const void *p,size_t bytes)
{
    int result=ggrs_enter(s);if(result)return result;
    if(!ggrs_valid(s,s->epoch)){ggrs_loss(s);return ggrs_leave(s,GGRS_LEASE);}
    if(!ggrs_deliver(s,id,p,bytes))return ggrs_leave(s,GGRS_CODEC);
    if(!ggrs_valid(s,s->epoch)){ggrs_loss(s);return ggrs_leave(s,GGRS_LEASE);}
    return ggrs_leave(s,GGRS_OK);
}

int ggrs_invalidate(struct ggrs_session *s)
{
    if(!s)return GGRS_ARGUMENT;
    unsigned initialized=__atomic_load_n(&s->initialized,__ATOMIC_ACQUIRE);
    if(initialized!=1 && initialized!=2)return GGRS_ARGUMENT;
    __atomic_store_n(&s->loss_pending,1u,__ATOMIC_RELEASE);
    if(initialized==2)return GGRS_INVALID;
    int result=ggrs_enter(s);if(result)return result==GGRS_BUSY?GGRS_INVALID:result;
    ggrs_loss(s);return ggrs_leave(s,GGRS_INVALID);
}

int ggrs_copy_snapshot(struct ggrs_session *s,struct gg_rq_snapshot *out)
{
    int result=ggrs_enter(s);if(result)return result;
    if(!ggrs_outside(s,out,sizeof(*out)) ||
       (uintptr_t)out%__alignof__(*out))return ggrs_leave(s,GGRS_ARGUMENT);
    if(!ggrs_valid(s,s->epoch)){ggrs_loss(s);return ggrs_leave(s,GGRS_LEASE);}
    if(!gg_rq_snapshot_complete(&s->snapshot))return ggrs_leave(s,GGRS_INVALID);
    struct gg_rq_snapshot copy=s->snapshot;
    if(!ggrs_valid(s,s->epoch)){ggrs_loss(s);return ggrs_leave(s,GGRS_LEASE);}
    result=ggrs_leave(s,GGRS_OK);
    if(result==GGRS_OK)*out=copy;
    return result;
}
#endif
