/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Bounded CPU-owned management queue. WMI RX copies; the worker authenticates
 * only after its device descriptor is retired. Never store device pointers. */
#ifndef GIULIA_WIFI_SECURITY_H
#define GIULIA_WIFI_SECURITY_H
#include "giulia_wifi_sae.h"
#include "giulia_wifi_pmf.h"
#define GNS_QUEUE_COUNT 8u
struct gns_packet {unsigned kind,frequency,status;size_t bytes;uint8_t data[GWT_MGMT_MAX];};
struct gns_state {
    struct gsa_state sae;struct gpm_state pmf;
    unsigned head,tail,count,accepted,pending_ptk;uint8_t ptk[16];
    struct gns_packet packets[GNS_QUEUE_COUNT];
};
#endif
