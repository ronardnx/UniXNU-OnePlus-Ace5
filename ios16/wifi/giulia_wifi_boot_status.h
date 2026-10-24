/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Cached boot diagnostics. One serialized driver worker publishes; terminal
 * readers never dereference runtime/MMIO/DMA pointers or wait on its lock.
 * Each field is atomic as well as the generation, avoiding C++ data races.
 */
#ifndef GIULIA_WIFI_BOOT_STATUS_H
#define GIULIA_WIFI_BOOT_STATUS_H
#include <stdint.h>
enum gwb_field {GWB_TIME,GWB_ACTIVE,GWB_STAGE,GWB_ERROR,GWB_MAPPING_ERROR,
    GWB_MHI_STAGE,GWB_EE,GWB_MHI_ERROR,GWB_QMI_STEP,GWB_QMI_ERROR,
    GWB_REG_STATUS_PRESENT,GWB_REG_STATUS,GWB_LAST_RPC,GWB_LAST_IND,
    GWB_TIMEOUT,GWB_ELAPSED,GWB_MEMORY_READY,GWB_PROVISIONED,GWB_FW_READY,
    GWB_PDC_CONFIGURED,GWB_PDC_CONSUMED,GWB_PMU_CONSUMED,GWB_QMP_PHASE,
    GWB_QMP_PENDING,GWB_QMP_RETAINED,GWB_QUARANTINE,GWB_INTERFACE_READY,
    GWB_PIN_VALID,GWB_PIN_POWER,GWB_PIN_PHY,GWB_PIN_RF,
    GWB_MHI_MASTER,GWB_MHI_RUNNING,GWB_MHI_STOP_PHASE,GWB_MHI_STOP_ERROR,
    GWB_MHI_RESET_COMPLETE,GWB_MHI_READY_COMPLETE,GWB_MHI_MASTER_STOPPED,GWB_MHI_STOP_EE,
    GWB_CE_ACTIVE,GWB_DP_PREPARED,GWB_DP_ACTIVE,GWB_DP_QUARANTINE,
    GWB_SMMU_ACTIVE,GWB_SMMU_CHANGED,
    GWB_PROTOCOL_EPOCH,GWB_PROTOCOL_CALLS,GWB_PROTOCOL_NATIVE,GWB_PROTOCOL_FALLBACK,
    GWB_PROTOCOL_REJECTED,GWB_PROTOCOL_MISMATCH,GWB_PROTOCOL_OVERSIZE,
    GWB_PROTOCOL_QMI,GWB_PROTOCOL_WMI,GWB_FIELDS};
struct gwb_snapshot {uint64_t value[GWB_FIELDS];};
struct gwb_mailbox {unsigned sequence;uint64_t value[GWB_FIELDS];};
static inline void gwb_publish(struct gwb_mailbox *box,const struct gwb_snapshot *value) {
    unsigned sequence=__atomic_load_n(&box->sequence,__ATOMIC_SEQ_CST);
    __atomic_store_n(&box->sequence,sequence+1u,__ATOMIC_SEQ_CST);
    for(unsigned i=0;i<GWB_FIELDS;i++)__atomic_store_n(&box->value[i],value->value[i],__ATOMIC_SEQ_CST);
    sequence+=2u;if(!sequence)sequence=2; /* zero means never published */
    __atomic_store_n(&box->sequence,sequence,__ATOMIC_SEQ_CST);
}
static inline int gwb_read(const struct gwb_mailbox *box,struct gwb_snapshot *value) {
    for(unsigned retry=0;retry<3;retry++) {
        unsigned before=__atomic_load_n(&box->sequence,__ATOMIC_SEQ_CST);
        if(!before || (before&1u))continue;
        struct gwb_snapshot copy;
        for(unsigned i=0;i<GWB_FIELDS;i++)copy.value[i]=__atomic_load_n(&box->value[i],__ATOMIC_SEQ_CST);
        if(__atomic_load_n(&box->sequence,__ATOMIC_SEQ_CST)==before){*value=copy;return 1;}
    }
    return 0;
}
#endif
