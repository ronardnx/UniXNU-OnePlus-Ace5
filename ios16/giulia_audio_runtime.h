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
    int (*pcm_stop)(void *,struct gat_transport *);
    int (*ipc_quiesce)(void *,struct gat_transport *);
};
struct gar_state {
    struct gab_state boot;
    struct gat_transport transport;
    struct gsipa speaker;
    /* playback denotes a currently active output attempt, not audible proof. */
    unsigned stage,result,graph_attempted,graph_running,playback,quarantined;
    uint32_t token;
    int backend;
};
static inline int gar_fail(struct gar_state *s,unsigned result,int backend)
{
    s->quarantined=1;s->stage=GAR_QUARANTINE;s->playback=0;
    s->transport.spf_ready=0;
    s->result=result;s->backend=backend;return (int)result;
}
static inline int gar_ops_valid(const struct gar_ops *o)
{
    return o && gab_ops_valid(&o->boot) && o->ipc_open && o->ipc_poll &&
        o->delay_ms && o->pcm_available && o->pcm_start && o->pcm_write &&
        o->pcm_stop && o->ipc_quiesce && o->speaker.verify_sha256 &&
        o->speaker.read && o->speaker.write && o->speaker.reset && o->speaker.delay_us;
}
static inline int gar_start(const struct gar_ops *o,struct gar_state *s,const void *pack,size_t bytes)
{
    if(!s || !gar_ops_valid(o))return GAR_ARGUMENT;
    if(s->quarantined)return GAR_UNCERTAIN;
    if(s->stage!=GAR_OFF)return GAR_BUSY;
    s->stage=GAR_BOOTING;s->playback=0;
    int e=gab_start(&o->boot,&s->boot,pack,bytes);
    if(e) {
        if(s->boot.quarantined)return gar_fail(s,GAR_BOOT,e);
        s->stage=GAR_OFF;s->backend=e;return (int)(s->result=GAR_BOOT);
    }
    s->stage=GAR_IPC;
    e=o->ipc_open(o->cookie,&s->transport);
    if(e!=GAT_OK)return gar_fail(s,GAR_TRANSPORT,e);
    unsigned sent=0;
    if(++s->token==0)++s->token;
    for(unsigned ms=0;ms<5000;ms++) {
        e=gat_start(&s->transport);
        if(e<0)return gar_fail(s,GAR_TRANSPORT,e);
        e=o->ipc_poll(o->cookie,&s->transport);
        if(e<0)return gar_fail(s,GAR_TRANSPORT,e);
        if(!sent && gat_channel_ready(&s->transport)) {
            e=gat_spf_query(&s->transport,s->token);
            if(e<0)return gar_fail(s,GAR_TRANSPORT,e);
            sent=e==GAT_OK;
        }
        if(sent && s->transport.spf_received) {
            if(!s->transport.spf_ready)return gar_fail(s,GAR_TRANSPORT,(int)s->transport.spf_status);
            s->stage=GAR_SPF_READY;return (int)(s->result=GAR_OK);
        }
        if((e=o->delay_ms(o->cookie,1)))return gar_fail(s,GAR_TRANSPORT,e);
    }
    return gar_fail(s,GAR_TIMEOUT,0);
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
    int observed=o->ipc_poll(o->cookie,&s->transport);
    if(observed<0 || !s->transport.spf_ready)return gar_fail(s,GAR_TRANSPORT,observed);
    if(o->pcm_available(o->cookie)!=1)return (int)(s->result=GAR_PLATFORM);
    int e=gsipa_init(&s->speaker,(const uint8_t *)sipa,sipa_bytes,&o->speaker);
    if(e){s->backend=e;return (int)(s->result=GAR_AMP);}
    /* Do not reset or unmute a PA until a real calibrated graph has started. */
    s->stage=GAR_PCM;s->graph_attempted=1;
    e=o->pcm_start(o->cookie,&s->transport);
    if(e) {
        int cleanup=o->pcm_stop(o->cookie,&s->transport);
        if(cleanup)return gar_fail(s,GAR_UNCERTAIN,cleanup);
        s->graph_attempted=0;s->stage=GAR_SPF_READY;s->backend=e;return (int)(s->result=GAR_GRAPH);
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
    /* Always mute/reset PA before stopping the MI2S graph, including I/O errors. */
    int amp_cleanup=gsipa_stop(&s->speaker);
    s->playback=0;
    /* If the PA cannot be confirmed muted, retain its clocked graph and all
     * backing. Removing MI2S underneath an uncertain live PA is not cleanup. */
    if(amp_cleanup)return gar_fail(s,GAR_UNCERTAIN,amp_cleanup);
    int graph_cleanup=o->pcm_stop(o->cookie,&s->transport);
    if(graph_cleanup)return gar_fail(s,GAR_UNCERTAIN,graph_cleanup);
    s->graph_attempted=s->graph_running=0;s->stage=GAR_SPF_READY;s->backend=e;
    return (int)(s->result=e?error_kind:(unsigned)GAR_OK);
}
static inline int gar_stop(const struct gar_ops *o,struct gar_state *s)
{
    if(!s || !gar_ops_valid(o))return GAR_ARGUMENT;
    if(s->quarantined)return GAR_UNCERTAIN;
    if(s->stage==GAR_OFF)return GAR_OK;
    if(s->graph_attempted || s->playback)return GAR_BUSY;
    int e=o->ipc_quiesce(o->cookie,&s->transport);
    if(e)return gar_fail(s,GAR_TRANSPORT,e);
    e=gab_stop(&o->boot,&s->boot);
    if(e)return gar_fail(s,GAR_BOOT,e);
    gat_zero(&s->transport,sizeof(s->transport));
    s->stage=GAR_OFF;return (int)(s->result=GAR_OK);
}
#endif
