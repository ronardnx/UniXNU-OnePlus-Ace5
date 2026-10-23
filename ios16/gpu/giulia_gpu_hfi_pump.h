/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Bounded HFI request pump for the GPU worker.
 *
 * The firmware writes F2H messages into queue 1; the host answers on queue 0.
 * This unit is the whole per-poll policy:
 *
 *   - only F2H_MSG_MEM_ALLOC commands are dispatched to the responder;
 *   - anything else is counted and skipped, never interpreted;
 *   - a refused request stops the pump and is reported, because the firmware
 *     would otherwise wait forever for a reply it will never get;
 *   - a reply that cannot be queued quarantines the pump: dropping it silently
 *     would leave the firmware waiting on a sequence number that was consumed.
 *
 * Everything runs against the same `ggh_io` the service already owns, so the
 * host fixture can drive real queue memory instead of a model.
 */
#ifndef GIULIA_GPU_HFI_PUMP_H
#define GIULIA_GPU_HFI_PUMP_H
#include "giulia_gpu_hfi.h"
#include "giulia_gpu_hfi_alloc.h"

enum ggpp_code {
    GGPP_OK=0, GGPP_ARGUMENT, GGPP_EMPTY, GGPP_RECEIVE, GGPP_REFUSED, GGPP_SEND,
    GGPP_QUARANTINED
};

struct ggpp_state {
    struct gga_state alloc;
    unsigned packets,received,replied,dropped,refused,stalled,quarantined,last;
    unsigned last_header;
};

static inline const char *ggpp_code_name(unsigned code)
{
    const char *const names[]={"ok","argument","empty","receive","refused","send","quarantined"};
    return code<sizeof(names)/sizeof(names[0])?names[code]:"invalid";
}

static inline int ggpp_pump(const struct ggh_io *io,const struct gga_ops *ops,
    struct ggpp_state *state,unsigned budget,uint32_t *scratch,unsigned scratch_words)
{
    unsigned i;
    if (!io || !ops || !state || !scratch || scratch_words<GGA_HDR_WORDS) return GGPP_ARGUMENT;
    if (state->quarantined) return GGPP_QUARANTINED;
    if (!budget) return GGPP_OK;
    for (i=0;i<budget;++i) {
        uint32_t reply[GGA_HDR_WORDS];
        unsigned words=0,reply_words=0;
        int error=ggh_receive(io,1,scratch,scratch_words,&words);
        if (error==GGH_EMPTY) return GGPP_OK;
        if (error) {
            state->last=(unsigned)GGPP_RECEIVE;
            state->quarantined=1;
            return GGPP_RECEIVE;
        }
        ++state->received;
        ++state->packets;
        state->last_header=scratch[0];
        if ((scratch[0]&0xffu)!=GGA_MSG_ID || ((scratch[0]>>16)&0xfu)!=0) {
            /* ACKs, error and log blocks are consumed and left to their own
             * backends; the pump must not guess at them. */
            ++state->dropped;
            continue;
        }
        error=gga_handle(ops,&state->alloc,scratch,words,reply,GGA_HDR_WORDS,&reply_words);
        if (error!=GGA_OK) {
            ++state->refused;
            state->last=(unsigned)GGPP_REFUSED;
            state->stalled=1;
            return GGPP_REFUSED;
        }
        error=ggh_send(io,reply,reply_words);
        if (error) {
            state->last=(unsigned)GGPP_SEND;
            state->quarantined=1;
            return GGPP_SEND;
        }
        ++state->replied;
    }
    state->last=(unsigned)GGPP_OK;
    return GGPP_OK;
}
#endif
