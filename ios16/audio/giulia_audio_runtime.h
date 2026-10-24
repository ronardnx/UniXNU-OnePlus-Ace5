/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Serialized ADSP -> GLINK/SPF -> calibrated PCM -> stereo amp lifecycle.
 * No component may turn its own successful setup into a playback claim.
 * Platform callbacks must report actual correlated completions, not dispatch.
 */
#ifndef GIULIA_AUDIO_RUNTIME_H
#define GIULIA_AUDIO_RUNTIME_H
#include "giulia_audio_boot.h"
#include "giulia_audio_transport.h"
#include "giulia_audio_speaker.h"

enum { GAR_OFF, GAR_BOOTING, GAR_IPC, GAR_SPF_READY, GAR_PCM, GAR_SPEAKER,
       GAR_QUARANTINE };
enum { GAR_OK, GAR_ARGUMENT, GAR_PLATFORM, GAR_BOOT, GAR_TRANSPORT,
       GAR_TIMEOUT, GAR_GRAPH, GAR_AMP, GAR_BUSY, GAR_UNCERTAIN, GAR_PCM_WRITE };
struct gar_ops {
    struct gab_ops boot;
    struct gsipa_ops speaker;
    void *cookie;
    /* Bind an exclusively owned, fresh GLINK edge after this boot's ready.
     * poll must attach a late RX FIFO and check fatal/SSR on every call. */
    int (*ipc_open)(void *,struct gat_transport *);
    int (*ipc_poll)(void *,struct gat_transport *);
    int (*delay_ms)(void *,unsigned);
    /* Resident monotonic elapsed milliseconds; polling/dispatch time counts
     * toward deadlines. Clock rollback is an uncertain platform failure. */
    uint64_t (*now_ms)(void *);
    /* Prerequisite check is read-only. The PCM provider must own mappings,
     * calibrated graph assets, MI2S clocks and completion tracking. */
    int (*pcm_available)(void *);
    /* Return 0 only after START acknowledgement, resolved module dependencies,
     * selected calibration/media, and running MI2S/DMA are all established. */
    int (*pcm_start)(void *,struct gat_transport *);
    /* Copy into provider-owned DMA backing and wait for actual DSP consumption,
     * checking fatal/SSR during all waits. Never retain the input pointer on
     * any return path. Uncertain DMA backing remains pinned by the provider. */
    int (*pcm_write)(void *,struct gat_transport *,const void *,size_t);
    /* 0 only after correlated graph STOP/CLOSE and DSP DMA consumption/drain;
     * retain graph/MI2S/SMMU/backing on uncertain completion or SSR. */
    int (*pcm_stop)(void *,struct gat_transport *);
    /* Close admission to every IPC client, issue SMP2P stop bit, await this
     * epoch's stop-ack/sysmon, then stop IRQ/RX work, remove/drain channels and
     * flush workers. 0 means actual completion, not dispatch or gat_zero.
     * Keep the latched IRQ status/lease readable through PAS and unprepare. */
    int (*ipc_quiesce)(void *,struct gat_transport *);
};
struct gar_state {
    struct gab_state boot;
    struct gat_transport transport;
    struct gsipa speaker;
    /* playback denotes a currently active output attempt, not audible proof. */
    unsigned stage,result,cause_result,failure_stage,graph_attempted,graph_running,playback,quarantined;
    uint32_t token;
    /* backend is the primary operation error; cleanup_backend is independent.
     * result may be GAR_UNCERTAIN while cause_result identifies its trigger. */
    int backend,cleanup_backend;
};
static inline int gar_fail(struct gar_state *s,unsigned result,int backend)
{
    if(s->quarantined)return (int)s->result;
    s->failure_stage=s->stage;
    s->quarantined=1;s->stage=GAR_QUARANTINE;s->playback=0;
    s->transport.spf_ready=0;
    s->result=s->cause_result=result;s->backend=backend;s->cleanup_backend=0;
    return (int)result;
}
static inline int gar_cleanup_fail(struct gar_state *s,unsigned cause,int backend,int cleanup)
{
    if(s->quarantined)return (int)s->result;
    (void)gar_fail(s,GAR_UNCERTAIN,backend);
    s->cause_result=cause;s->cleanup_backend=cleanup;return GAR_UNCERTAIN;
}
static inline int gar_ops_valid(const struct gar_ops *o)
{
    return o && gab_ops_valid(&o->boot) && o->ipc_open && o->ipc_poll &&
        o->delay_ms && o->now_ms && o->pcm_available && o->pcm_start && o->pcm_write &&
        o->pcm_stop && o->ipc_quiesce && o->speaker.verify_sha256 &&
        o->speaker.read && o->speaker.write && o->speaker.reset && o->speaker.delay_us;
}
/* Local prerequisite for a resident hotload adapter, not an ownership grant.
 * The adapter must additionally close admission, drain its worker/calls and
 * validate the provider's real IRQ/DMA/resource epoch. Never export hardware
 * objects or zero this state to manufacture a successful STOP. */
static inline int gar_quiesced(const struct gar_state *s)
{
    if(!s)return 0;
    for(unsigned i=0;i<GAT_INTENTS_MAX;i++)
        if(s->transport.remote[i].used || s->transport.remote[i].busy)return 0;
    return s->stage==GAR_OFF && !s->quarantined &&
        !s->boot.prepared && !s->boot.running && !s->boot.quarantined &&
        !s->boot.metadata_mask && !s->boot.init_attempted_mask && !s->boot.authenticated_mask &&
        !s->boot.cleanup_result && !s->cleanup_backend &&
        !s->boot.metadata_physical[0] && !s->boot.metadata_physical[1] &&
        !s->graph_attempted && !s->graph_running && !s->playback &&
        gsipa_quiesced(&s->speaker) && !s->transport.started &&
        !s->transport.tx.fifo && !s->transport.tx.head && !s->transport.tx.tail &&
        !s->transport.rx.fifo && !s->transport.rx.head && !s->transport.rx.tail &&
        !s->transport.ctx && !s->transport.epoch &&
        !s->transport.version_ack && !s->transport.open_sent && !s->transport.open_ack &&
        !s->transport.remote_open && !s->transport.local_intent && !s->transport.intent_requested &&
        !s->transport.error && !s->transport.packet_bytes && !s->transport.remaining &&
        !s->transport.spf_pending && !s->transport.spf_received && !s->transport.spf_ready;
}
static inline int gar_start(const struct gar_ops *o,struct gar_state *s,const void *pack,size_t bytes)
{
    if(!s || !gar_ops_valid(o))return GAR_ARGUMENT;
    if(s->quarantined)return GAR_UNCERTAIN;
    if(s->stage!=GAR_OFF)return GAR_BUSY;
    if(!gar_quiesced(s))return gar_fail(s,GAR_UNCERTAIN,0);
    s->stage=GAR_BOOTING;s->playback=0;s->backend=0;s->cleanup_backend=0;s->cause_result=GAR_OK;
    int e=gab_start(&o->boot,&s->boot,pack,bytes);
    if(e) {
        if(s->boot.quarantined) {
            (void)gar_fail(s,GAR_BOOT,e);s->cleanup_backend=s->boot.cleanup_result;
            return GAR_BOOT;
        }
        s->stage=GAR_OFF;s->backend=e;s->cause_result=GAR_BOOT;return (int)(s->result=GAR_BOOT);
    }
    s->stage=GAR_IPC;
    e=o->ipc_open(o->cookie,&s->transport);
    if(e!=GAT_OK)return gar_fail(s,GAR_TRANSPORT,e);
    if(!s->transport.epoch)return gar_fail(s,GAR_PLATFORM,0);
    /* Separate GLINK and SPF budgets. OnePlus spf-core uses 1s per query and
     * 50ms between attempts; this platform retains a 5s total SPF window.
     * Every poll still checks the provider's lease/fatal/SSR observations. */
    uint64_t last=o->now_ms(o->cookie),now=last,deadline;
    if(last>UINT64_MAX-5000u)return gar_fail(s,GAR_TIMEOUT,0);
    deadline=last+5000u;
    while(!gat_channel_ready(&s->transport)) {
        now=o->now_ms(o->cookie);
        if(now<last||now>=deadline)return gar_fail(s,GAR_TIMEOUT,0);
        last=now;
        e=gat_start(&s->transport);if(e<0)return gar_fail(s,GAR_TRANSPORT,e);
        e=o->ipc_poll(o->cookie,&s->transport);if(e<0)return gar_fail(s,GAR_TRANSPORT,e);
        now=o->now_ms(o->cookie);
        if(now<last||now>=deadline)return gar_fail(s,GAR_TIMEOUT,0);
        last=now;
        if(!gat_channel_ready(&s->transport)&&(e=o->delay_ms(o->cookie,1)))
            return gar_fail(s,GAR_TRANSPORT,e);
    }
    now=o->now_ms(o->cookie);
    if(now<last||now>UINT64_MAX-5000u)return gar_fail(s,GAR_TIMEOUT,0);
    last=now;deadline=now+5000u;
    unsigned sent=0;uint64_t query_deadline=0,next_query=now;
    for(;;) {
        now=o->now_ms(o->cookie);
        if(now<last||now>=deadline)return gar_fail(s,GAR_TIMEOUT,(int)s->transport.spf_error);
        last=now;
        if(sent && now>=query_deadline) {
            gat_spf_expire(&s->transport);sent=0;
            next_query=deadline-now>50u?now+50u:deadline;
        }
        e=gat_start(&s->transport);
        if(e<0)return gar_fail(s,GAR_TRANSPORT,e);
        e=o->ipc_poll(o->cookie,&s->transport);
        if(e<0)return gar_fail(s,GAR_TRANSPORT,e);
        now=o->now_ms(o->cookie);
        if(now<last||now>=deadline)return gar_fail(s,GAR_TIMEOUT,(int)s->transport.spf_error);
        last=now;
        if(sent && now>=query_deadline) {
            gat_spf_expire(&s->transport);sent=0;
            next_query=deadline-now>50u?now+50u:deadline;
        }
        if(sent && s->transport.spf_received) {
            if(s->transport.spf_ready) {
                s->stage=GAR_SPF_READY;return (int)(s->result=GAR_OK);
            }
            sent=0;next_query=deadline-now>50u?now+50u:deadline;
        }
        if(!sent && now>=next_query && gat_channel_ready(&s->transport)) {
            uint32_t token=s->token+1;if(!token)++token;
            e=gat_spf_query(&s->transport,token);
            if(e<0)return gar_fail(s,GAR_TRANSPORT,e);
            if(e==GAT_OK) {
                s->token=token;sent=1;query_deadline=deadline-now>1000u?now+1000u:deadline;
            }
        }
        if((e=o->delay_ms(o->cookie,1)))return gar_fail(s,GAR_TRANSPORT,e);
    }
}
/* Feed only explicitly supplied interleaved signed PCM: 48 kHz, stereo,
 * 32-bit physical slots. Buffer must remain immutable until write completion.
 * A PCM provider may reject mismatched valid-bit/graph media configuration. */
static inline int gar_play(const struct gar_ops *o,struct gar_state *s,
                          const void *sipa,size_t sipa_bytes,const void *pcm,size_t bytes)
{
    if(!s || !gar_ops_valid(o) || !sipa || !pcm || !bytes || (bytes&7) || bytes>48000u*8u)
        return GAR_ARGUMENT;
    if(s->quarantined)return GAR_UNCERTAIN;
    if(s->stage!=GAR_SPF_READY || !s->boot.running || !s->transport.spf_ready)return GAR_BUSY;
    /* Never overwrite residual output ownership with a fresh gsipa_init. */
    if(s->graph_attempted || s->graph_running || s->playback || !gsipa_quiesced(&s->speaker))
        return gar_fail(s,GAR_UNCERTAIN,0);
    s->backend=0;s->cleanup_backend=0;s->cause_result=GAR_OK;
    int observed=o->ipc_poll(o->cookie,&s->transport);
    if(observed<0 || !s->transport.spf_ready)return gar_fail(s,GAR_TRANSPORT,observed);
    if(o->pcm_available(o->cookie)!=1)return (int)(s->cause_result=s->result=GAR_PLATFORM);
    int e=gsipa_init(&s->speaker,(const uint8_t *)sipa,sipa_bytes,&o->speaker);
    if(e){s->backend=e;return (int)(s->cause_result=s->result=GAR_AMP);}
    /* Do not reset or unmute a PA until a real calibrated graph has started. */
    s->stage=GAR_PCM;s->graph_attempted=1;
    e=o->pcm_start(o->cookie,&s->transport);
    if(e) {
        int cleanup=o->pcm_stop(o->cookie,&s->transport);
        if(cleanup)return gar_cleanup_fail(s,GAR_GRAPH,e,cleanup);
        s->graph_attempted=0;s->stage=GAR_SPF_READY;s->backend=e;
        return (int)(s->cause_result=s->result=GAR_GRAPH);
    }
    s->graph_running=1;
    unsigned error_kind=GAR_AMP;
    e=gsipa_probe(&s->speaker);
    if(!e)e=gsipa_start(&s->speaker,GSIPA_REQUIRED);
    if(!e) {
        s->stage=GAR_SPEAKER;s->playback=1;
        error_kind=GAR_PCM_WRITE;
        e=o->pcm_write(o->cookie,&s->transport,pcm,bytes);
    }
    /* probe/start already perform their own rollback. Preserve that first
     * cleanup result; a second stop must not overwrite it or replay a partly
     * written shutdown program. A successfully rolled-back PA needs no extra
     * hardware operation. Active/probed PAs still stop before the MI2S graph. */
    int amp_cleanup=s->speaker.cleanup_error;
    if(!amp_cleanup && !gsipa_quiesced(&s->speaker))amp_cleanup=gsipa_stop(&s->speaker);
    s->playback=0;
    /* If the PA cannot be confirmed muted, retain its clocked graph and all
     * backing. Removing MI2S underneath an uncertain live PA is not cleanup. */
    if(amp_cleanup)return gar_cleanup_fail(s,e?error_kind:(unsigned)GAR_OK,e,amp_cleanup);
    int graph_cleanup=o->pcm_stop(o->cookie,&s->transport);
    if(graph_cleanup)return gar_cleanup_fail(s,e?error_kind:(unsigned)GAR_OK,e,graph_cleanup);
    s->graph_attempted=s->graph_running=0;s->stage=GAR_SPF_READY;s->backend=e;
    return (int)(s->cause_result=s->result=e?error_kind:(unsigned)GAR_OK);
}
static inline int gar_stop(const struct gar_ops *o,struct gar_state *s)
{
    if(!s || !gar_ops_valid(o))return GAR_ARGUMENT;
    if(s->quarantined)return GAR_UNCERTAIN;
    if(s->stage==GAR_OFF)return gar_quiesced(s)?GAR_OK:gar_fail(s,GAR_UNCERTAIN,0);
    if(s->graph_attempted || s->graph_running || s->playback)return GAR_BUSY;
    if(!gsipa_quiesced(&s->speaker) || s->stage!=GAR_SPF_READY || !s->boot.running)
        return gar_fail(s,GAR_UNCERTAIN,0);
    int e=o->ipc_quiesce(o->cookie,&s->transport);
    if(e)return gar_fail(s,GAR_TRANSPORT,e);
    e=gab_stop(&o->boot,&s->boot);
    if(e) {
        (void)gar_fail(s,GAR_BOOT,e);s->cleanup_backend=s->boot.cleanup_result;
        return GAR_BOOT;
    }
    gat_zero(&s->transport,sizeof(s->transport));
    s->stage=GAR_OFF;s->backend=0;s->cleanup_backend=0;s->cause_result=GAR_OK;
    return gar_quiesced(s)?(int)(s->result=GAR_OK):gar_fail(s,GAR_UNCERTAIN,0);
}
#endif
