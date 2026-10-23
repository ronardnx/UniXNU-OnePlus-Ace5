"""Verify station wire fixtures against the cached Linux v6.12 ath12k header.

No downloads, kernel builds, or driver-generated definitions are used.
The C test uses the committed fixture without requiring the source cache.
"""
import argparse
import hashlib
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SHA256 = '0d418d2e17b272f364c7631e188011240a564fda518e84668fa003acc51a8a8c'
STRUCTS = '''ath12k_wmi_mac_addr_params ath12k_wmi_ppe_threshold_params
    ath12k_wmi_ssid_params wmi_vdev_start_request_cmd wmi_vdev_start_resp_event
    wmi_vdev_up_cmd wmi_vdev_set_param_cmd ath12k_wmi_resource_config_params wmi_peer_create_cmd wmi_peer_delete_cmd
    wmi_peer_reorder_queue_setup_cmd wmi_peer_assoc_complete_cmd
    ath12k_wmi_vht_rate_set_params wmi_vdev_install_key_cmd
    wmi_vdev_install_key_compl_event wmi_peer_assoc_conf_event
    wmi_mgmt_send_cmd wmi_mgmt_tx_compl_event ath12k_wmi_mgmt_rx_params
    wmi_vdev_stopped_event wmi_peer_delete_resp_event
    wmi_reg_chan_list_cc_ext_event ath12k_wmi_reg_rule_ext_params
    wmi_start_scan_cmd wmi_stop_scan_cmd wmi_scan_event wmi_roam_event wmi_peer_sta_kickout_event wmi_pdev_set_param_cmd'''.split()
MACROS = '''MAX_HE_NSS WMI_MAX_NUM_SS ATH12K_WMI_SSID_LEN WMI_MAX_HECAP_PHY_SIZE
    WMI_MAX_EHTCAP_MAC_SIZE WMI_MAX_EHTCAP_PHY_SIZE ATH_MAX_ANTENNA
    WMI_REG_CLIENT_MAX WMI_IE_BITMAP_SIZE WMI_SCAN_FLAG_PASSIVE WMI_SCAN_FLAG_STRICT_PASSIVE_ON_PCHN
    WMI_SCAN_FLAG_FORCE_ACTIVE_ON_DFS WMI_SCAN_ADD_BCAST_PROBE_REQ'''.split()
CONSTANTS = '''WMI_VDEV_START_REQUEST_CMDID WMI_VDEV_UP_CMDID WMI_VDEV_STOP_CMDID
    WMI_VDEV_DOWN_CMDID WMI_VDEV_INSTALL_KEY_CMDID WMI_PEER_CREATE_CMDID
    WMI_VDEV_SET_PARAM_CMDID WMI_VDEV_PARAM_TX_ENCAP_TYPE WMI_VDEV_PARAM_RX_DECAP_TYPE
    WMI_START_SCAN_CMDID WMI_STOP_SCAN_CMDID WMI_SCAN_EVENTID WMI_TAG_START_SCAN_CMD WMI_TAG_STOP_SCAN_CMD
    WMI_TAG_SCAN_EVENT WMI_ROAM_EVENTID WMI_PEER_STA_KICKOUT_EVENTID WMI_TAG_ROAM_EVENT WMI_TAG_PEER_STA_KICKOUT_EVENT
    WMI_PDEV_SET_PARAM_CMDID WMI_TAG_PDEV_SET_PARAM_CMD WMI_PDEV_PARAM_PMF_QOS
    WMI_PEER_DELETE_CMDID WMI_PEER_SET_PARAM_CMDID WMI_PEER_ASSOC_CMDID
    WMI_PEER_REORDER_QUEUE_SETUP_CMDID WMI_MGMT_TX_SEND_CMDID
    WMI_VDEV_START_RESP_EVENTID WMI_VDEV_STOPPED_EVENTID
    WMI_VDEV_INSTALL_KEY_COMPLETE_EVENTID WMI_PEER_ASSOC_CONF_EVENTID
    WMI_PEER_DELETE_RESP_EVENTID WMI_MGMT_RX_EVENTID WMI_MGMT_TX_COMPLETION_EVENTID
    WMI_REG_CHAN_LIST_CC_EXT_EVENTID WMI_TAG_ARRAY_BYTE WMI_TAG_ARRAY_STRUCT
    WMI_TAG_VDEV_START_REQUEST_CMD WMI_TAG_VDEV_START_RESPONSE_EVENT
    WMI_TAG_VDEV_UP_CMD WMI_TAG_VDEV_SET_PARAM_CMD WMI_TAG_VDEV_STOPPED_EVENT WMI_TAG_PEER_CREATE_CMD
    WMI_TAG_PEER_DELETE_CMD WMI_TAG_PEER_ASSOC_COMPLETE_CMD WMI_TAG_VHT_RATE_SET
    WMI_TAG_VDEV_INSTALL_KEY_CMD WMI_TAG_VDEV_INSTALL_KEY_COMPLETE_EVENT
    WMI_TAG_PEER_ASSOC_CONF_EVENT WMI_TAG_PEER_DELETE_RESP_EVENT
    WMI_TAG_REORDER_QUEUE_SETUP_CMD WMI_TAG_MGMT_TX_SEND_CMD
    WMI_TAG_MGMT_TX_COMPL_EVENT WMI_TAG_MGMT_RX_HDR
    WMI_TAG_REG_CHAN_LIST_CC_EXT_EVENT WMI_TAG_REGULATORY_RULE_EXT_STRUCT'''.split()


def fixture(source):
    raw = (source / 'wmi.h').read_bytes()
    assert hashlib.sha256(raw).hexdigest() == SHA256, 'Linux v6.12 wmi.h hash mismatch'
    header = raw.decode()
    constants = {}
    for enum in ('wmi_cmd_group', 'wmi_tlv_cmd_id', 'wmi_tlv_event_id', 'wmi_tlv_tag', 'wmi_tlv_vdev_param', 'wmi_tlv_pdev_param'):
        body = re.search(r'enum ' + enum + r' \{(.*?)\n\};', header, re.S)[1]
        body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
        value = -1
        for item in body.split(','):
            if not item.strip():
                continue
            pair = item.strip().split('=', 1)
            name = pair[0].strip()
            if len(pair) == 1:
                value += 1
            else:
                expr = pair[1].strip()
                group = re.fullmatch(r'WMI_(?:TLV_CMD|TLV_EV|EVT_GRP_START_ID)\((\w+)\)', expr)
                if group:
                    value = (constants[group[1]] << 12) | 1
                else:
                    value = constants[expr] if expr in constants else int(expr, 0)
            constants[name] = value
    out = ['''/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (c) 2018-2021 The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * Generated from Linux v6.12 ath12k/wmi.h by ios16_wifi_station_oracle.py.
 * https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/ath/ath12k/wmi.h
 * Independent definitions; never regenerate from the Giulia implementation.
 */
#ifndef ATH12K_WIFI_STATION_ORACLE_H
#define ATH12K_WIFI_STATION_ORACLE_H
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef int32_t a_sle32;
#define __packed __attribute__((packed))
#define ETH_ALEN 6
''']
    for name in MACROS:
        out.append(re.search(r'^#define ' + name + r'\s+[^\n]+', header, re.M)[0] + '\n')
    for name in CONSTANTS:
        out.append(f'#define {name} 0x{constants[name]:x}u\n')
    for name in STRUCTS:
        out.append(re.search(r'^struct\s+' + name + r' \{.*?^\}(?: __packed)?;', header, re.M | re.S)[0] + '\n')
    out.append('#endif\n')
    return ''.join(out)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / '.cache/wifi-native')
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    generated = fixture(args.source)
    path = ROOT / 'tests/fixtures/ath12k_wifi_station_layout.h'
    if args.write:
        path.write_text(generated, encoding='utf-8', newline='\n')
    else:
        assert path.read_text(encoding='utf-8') == generated, 'station oracle drifted'
    print(f'Linux v6.12 station source oracle: SHA-256, {len(STRUCTS)} structures, {len(CONSTANTS)} command/event/tag IDs verified')
