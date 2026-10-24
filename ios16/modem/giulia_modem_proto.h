/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_MODEM_PROTO_H
#define GIULIA_MODEM_PROTO_H
#include "giulia_modem_contract.h"

/* Pure QMI framing boundary. The resident caller has already authenticated
 * the QRTR source and pending identity. No view or pointer crosses this API.
 * Result bits: [7:0] gmq_error, [23:8] remote_error, [63:24] zero. */
#define GMR_PROTO_RESULT_MASK UINT64_C(0xffffff)
static inline uint16_t gmr_proto_message(uint32_t kind)
{
    switch(kind) {
    case GMR_QMI_SIM:return GMQ_UIM_GET_CARD_STATUS;
    case GMR_QMI_REGISTRATION:return GMQ_NAS_GET_SERVING_SYSTEM;
    case GMR_QMI_START:return GMQ_WDS_START_NETWORK;
    case GMR_QMI_SETTINGS:return GMQ_WDS_GET_CURRENT_SETTINGS;
    case GMR_QMI_STOP:return GMQ_WDS_STOP_NETWORK;
    default:return 0;
    }
}

static inline uint64_t gmr_proto_check(uint32_t kind,uint16_t txn,
    const uint8_t *payload,size_t bytes)
{
    struct gmq_reply reply;uint16_t message=gmr_proto_message(kind);int error;
    if(!message)return GMQ_UNSUPPORTED;
    if(bytes>GMR_PAYLOAD)return GMQ_LIMIT;
    error=gmq_response(payload,bytes,txn,message,&reply);
    return (uint64_t)(unsigned)error |
        ((error==GMQ_OK || error==GMQ_REMOTE)?(uint64_t)reply.remote_error<<8:0);
}

static inline int gmr_proto_result_valid(uint64_t result)
{
    unsigned error=(unsigned)(result&255u);
    return !(result&~GMR_PROTO_RESULT_MASK) && error<=GMQ_UNSUPPORTED &&
        ((error==GMQ_REMOTE) || !(result>>8));
}
#endif
