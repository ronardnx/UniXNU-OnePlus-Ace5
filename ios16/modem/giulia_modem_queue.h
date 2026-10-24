/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_MODEM_QUEUE_H
#define GIULIA_MODEM_QUEUE_H
#include "giulia_modem_contract.h"

/* The service queue is resident. A queued request belongs to the state seen
 * at acceptance, not whichever subscription/bearer happens to exist when the
 * worker eventually runs. No callback or hardware owner is copied here. */
struct gmr_queued_command {
    struct gmr_command command;
    uint64_t epoch;
    uint32_t lifecycle,slot_generation,bearer_generation;
    uint8_t slot,bearer;
};

static inline void gmr_queue_capture(struct gmr_queued_command *queued,
    const struct gmr_command *command,const struct gmr_snapshot *state)
{
    queued->command=*command;queued->epoch=state->epoch;
    queued->lifecycle=state->lifecycle;
    queued->slot=GMR_NO_SLOT;queued->bearer=GMR_NO_BEARER;
    queued->slot_generation=queued->bearer_generation=0;
    if(command->kind==GMR_CMD_RF_MODE)queued->slot=state->selected_slot;
    else if(command->kind!=GMR_CMD_STOP && command->kind!=GMR_CMD_SSR &&
        command->kind!=GMR_CMD_SUSPEND && command->kind!=GMR_CMD_RESUME &&
        command->kind!=GMR_CMD_STORAGE_WRITE && command->kind!=GMR_CMD_STORAGE_FLUSH)
        queued->slot=command->slot;
    if(queued->slot<GMR_SLOTS)queued->slot_generation=state->slots[queued->slot].generation;
    if(command->kind==GMR_CMD_CONNECT || command->kind==GMR_CMD_DISCONNECT ||
        command->kind==GMR_CMD_PACKET_TX)queued->bearer=command->bearer;
    if(queued->bearer<GMR_BEARERS)
        queued->bearer_generation=state->bearers[queued->bearer].generation;
}

static inline int gmr_queue_current(const struct gmr_queued_command *queued,
    const struct gmr_snapshot *state)
{
    /* Shutdown always targets the current owner, including stop() racing an
     * SSR. A stale STOP must never let an automatic restart escape teardown. */
    if(queued->command.kind==GMR_CMD_STOP)return GMR_OK;
    if(queued->epoch!=state->epoch || queued->lifecycle!=state->lifecycle)return GMR_STALE;
    if(queued->slot<GMR_SLOTS && queued->slot_generation!=state->slots[queued->slot].generation)
        return GMR_STALE;
    if(queued->command.kind==GMR_CMD_RF_MODE && queued->slot!=state->selected_slot)return GMR_STALE;
    if(queued->bearer<GMR_BEARERS &&
        queued->bearer_generation!=state->bearers[queued->bearer].generation)return GMR_STALE;
    return GMR_OK;
}
#endif
