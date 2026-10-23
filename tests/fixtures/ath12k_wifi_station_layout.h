/* SPDX-License-Identifier: BSD-3-Clause-Clear
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
#define MAX_HE_NSS               8
#define WMI_MAX_NUM_SS                    MAX_HE_NSS
#define ATH12K_WMI_SSID_LEN 32
#define WMI_MAX_HECAP_PHY_SIZE                 (3)
#define WMI_MAX_EHTCAP_MAC_SIZE  2
#define WMI_MAX_EHTCAP_PHY_SIZE  3
#define ATH_MAX_ANTENNA 4
#define WMI_REG_CLIENT_MAX 4
#define WMI_IE_BITMAP_SIZE             8
#define WMI_SCAN_FLAG_PASSIVE        0x1
#define WMI_SCAN_FLAG_STRICT_PASSIVE_ON_PCHN 0x10000
#define WMI_SCAN_FLAG_FORCE_ACTIVE_ON_DFS 0x200
#define WMI_SCAN_ADD_BCAST_PROBE_REQ 0x2
#define WMI_VDEV_START_REQUEST_CMDID 0x5003u
#define WMI_VDEV_UP_CMDID 0x5005u
#define WMI_VDEV_STOP_CMDID 0x5006u
#define WMI_VDEV_DOWN_CMDID 0x5007u
#define WMI_VDEV_INSTALL_KEY_CMDID 0x5009u
#define WMI_PEER_CREATE_CMDID 0x6001u
#define WMI_VDEV_SET_PARAM_CMDID 0x5008u
#define WMI_VDEV_PARAM_TX_ENCAP_TYPE 0x30u
#define WMI_VDEV_PARAM_RX_DECAP_TYPE 0x5bu
#define WMI_START_SCAN_CMDID 0x3001u
#define WMI_STOP_SCAN_CMDID 0x3002u
#define WMI_SCAN_EVENTID 0x3001u
#define WMI_TAG_START_SCAN_CMD 0x4du
#define WMI_TAG_STOP_SCAN_CMD 0x4eu
#define WMI_TAG_SCAN_EVENT 0x24u
#define WMI_ROAM_EVENTID 0xb001u
#define WMI_PEER_STA_KICKOUT_EVENTID 0x6001u
#define WMI_TAG_ROAM_EVENT 0x30u
#define WMI_TAG_PEER_STA_KICKOUT_EVENT 0x2bu
#define WMI_PDEV_SET_PARAM_CMDID 0x4003u
#define WMI_TAG_PDEV_SET_PARAM_CMD 0x52u
#define WMI_PDEV_PARAM_PMF_QOS 0x22u
#define WMI_PEER_DELETE_CMDID 0x6002u
#define WMI_PEER_SET_PARAM_CMDID 0x6004u
#define WMI_PEER_ASSOC_CMDID 0x6005u
#define WMI_PEER_REORDER_QUEUE_SETUP_CMDID 0x6013u
#define WMI_MGMT_TX_SEND_CMDID 0x7008u
#define WMI_VDEV_START_RESP_EVENTID 0x5001u
#define WMI_VDEV_STOPPED_EVENTID 0x5002u
#define WMI_VDEV_INSTALL_KEY_COMPLETE_EVENTID 0x5003u
#define WMI_PEER_ASSOC_CONF_EVENTID 0x6006u
#define WMI_PEER_DELETE_RESP_EVENTID 0x6007u
#define WMI_MGMT_RX_EVENTID 0x7001u
#define WMI_MGMT_TX_COMPLETION_EVENTID 0x7006u
#define WMI_REG_CHAN_LIST_CC_EXT_EVENTID 0x3a003u
#define WMI_TAG_ARRAY_BYTE 0x11u
#define WMI_TAG_ARRAY_STRUCT 0x12u
#define WMI_TAG_VDEV_START_REQUEST_CMD 0x58u
#define WMI_TAG_VDEV_START_RESPONSE_EVENT 0x28u
#define WMI_TAG_VDEV_UP_CMD 0x5cu
#define WMI_TAG_VDEV_SET_PARAM_CMD 0x5fu
#define WMI_TAG_VDEV_STOPPED_EVENT 0x29u
#define WMI_TAG_PEER_CREATE_CMD 0x61u
#define WMI_TAG_PEER_DELETE_CMD 0x62u
#define WMI_TAG_PEER_ASSOC_COMPLETE_CMD 0x65u
#define WMI_TAG_VHT_RATE_SET 0x66u
#define WMI_TAG_VDEV_INSTALL_KEY_CMD 0x60u
#define WMI_TAG_VDEV_INSTALL_KEY_COMPLETE_EVENT 0x2au
#define WMI_TAG_PEER_ASSOC_CONF_EVENT 0x1b2u
#define WMI_TAG_PEER_DELETE_RESP_EVENT 0x1c3u
#define WMI_TAG_REORDER_QUEUE_SETUP_CMD 0x225u
#define WMI_TAG_MGMT_TX_SEND_CMD 0x1a6u
#define WMI_TAG_MGMT_TX_COMPL_EVENT 0x1a7u
#define WMI_TAG_MGMT_RX_HDR 0x2cu
#define WMI_TAG_REG_CHAN_LIST_CC_EXT_EVENT 0x3aau
#define WMI_TAG_REGULATORY_RULE_EXT_STRUCT 0x3a9u
struct ath12k_wmi_mac_addr_params {
	u8 addr[ETH_ALEN];
	u8 padding[2];
} __packed;
struct ath12k_wmi_ppe_threshold_params {
	__le32 numss_m1; /** NSS - 1*/
	__le32 ru_info;
	__le32 ppet16_ppet8_ru3_ru0[WMI_MAX_NUM_SS];
} __packed;
struct ath12k_wmi_ssid_params {
	__le32 ssid_len;
	u8 ssid[ATH12K_WMI_SSID_LEN];
} __packed;
struct wmi_vdev_start_request_cmd {
	__le32 tlv_header;
	__le32 vdev_id;
	__le32 requestor_id;
	__le32 beacon_interval;
	__le32 dtim_period;
	__le32 flags;
	struct ath12k_wmi_ssid_params ssid;
	__le32 bcn_tx_rate;
	__le32 bcn_txpower;
	__le32 num_noa_descriptors;
	__le32 disable_hw_ack;
	__le32 preferred_tx_streams;
	__le32 preferred_rx_streams;
	__le32 he_ops;
	__le32 cac_duration_ms;
	__le32 regdomain;
	__le32 min_data_rate;
	__le32 mbssid_flags; /* uses enum wmi_vdev_mbssid_flags */
	__le32 mbssid_tx_vdev_id;
	__le32 eht_ops;
	__le32 punct_bitmap;
} __packed;
struct wmi_vdev_start_resp_event {
	__le32 vdev_id;
	__le32 requestor_id;
	/* enum wmi_start_event_param */
	__le32 resp_type;
	__le32 status;
	__le32 chain_mask;
	__le32 smps_mode;
	union {
		__le32 mac_id;
		__le32 pdev_id;
	};
	__le32 cfgd_tx_streams;
	__le32 cfgd_rx_streams;
} __packed;
struct wmi_vdev_up_cmd {
	__le32 tlv_header;
	__le32 vdev_id;
	__le32 vdev_assoc_id;
	struct ath12k_wmi_mac_addr_params vdev_bssid;
	struct ath12k_wmi_mac_addr_params tx_vdev_bssid;
	__le32 nontx_profile_idx;
	__le32 nontx_profile_cnt;
} __packed;
struct wmi_vdev_set_param_cmd {
	__le32 tlv_header;
	__le32 vdev_id;
	__le32 param_id;
	__le32 param_value;
} __packed;
struct ath12k_wmi_resource_config_params {
	__le32 tlv_header;
	__le32 num_vdevs;
	__le32 num_peers;
	__le32 num_offload_peers;
	__le32 num_offload_reorder_buffs;
	__le32 num_peer_keys;
	__le32 num_tids;
	__le32 ast_skid_limit;
	__le32 tx_chain_mask;
	__le32 rx_chain_mask;
	__le32 rx_timeout_pri[4];
	__le32 rx_decap_mode;
	__le32 scan_max_pending_req;
	__le32 bmiss_offload_max_vdev;
	__le32 roam_offload_max_vdev;
	__le32 roam_offload_max_ap_profiles;
	__le32 num_mcast_groups;
	__le32 num_mcast_table_elems;
	__le32 mcast2ucast_mode;
	__le32 tx_dbg_log_size;
	__le32 num_wds_entries;
	__le32 dma_burst_size;
	__le32 mac_aggr_delim;
	__le32 rx_skip_defrag_timeout_dup_detection_check;
	__le32 vow_config;
	__le32 gtk_offload_max_vdev;
	__le32 num_msdu_desc;
	__le32 max_frag_entries;
	__le32 num_tdls_vdevs;
	__le32 num_tdls_conn_table_entries;
	__le32 beacon_tx_offload_max_vdev;
	__le32 num_multicast_filter_entries;
	__le32 num_wow_filters;
	__le32 num_keep_alive_pattern;
	__le32 keep_alive_pattern_size;
	__le32 max_tdls_concurrent_sleep_sta;
	__le32 max_tdls_concurrent_buffer_sta;
	__le32 wmi_send_separate;
	__le32 num_ocb_vdevs;
	__le32 num_ocb_channels;
	__le32 num_ocb_schedules;
	__le32 flag1;
	__le32 smart_ant_cap;
	__le32 bk_minfree;
	__le32 be_minfree;
	__le32 vi_minfree;
	__le32 vo_minfree;
	__le32 alloc_frag_desc_for_data_pkt;
	__le32 num_ns_ext_tuples_cfg;
	__le32 bpf_instruction_size;
	__le32 max_bssid_rx_filters;
	__le32 use_pdev_id;
	__le32 max_num_dbs_scan_duty_cycle;
	__le32 max_num_group_keys;
	__le32 peer_map_unmap_version;
	__le32 sched_params;
	__le32 twt_ap_pdev_count;
	__le32 twt_ap_sta_count;
	__le32 max_nlo_ssids;
	__le32 num_pkt_filters;
	__le32 num_max_sta_vdevs;
	__le32 max_bssid_indicator;
	__le32 ul_resp_config;
	__le32 msdu_flow_override_config0;
	__le32 msdu_flow_override_config1;
	__le32 flags2;
	__le32 host_service_flags;
	__le32 max_rnr_neighbours;
	__le32 ema_max_vap_cnt;
	__le32 ema_max_profile_period;
} __packed;
struct wmi_peer_create_cmd {
	__le32 tlv_header;
	__le32 vdev_id;
	struct ath12k_wmi_mac_addr_params peer_macaddr;
	__le32 peer_type;
} __packed;
struct wmi_peer_delete_cmd {
	__le32 tlv_header;
	__le32 vdev_id;
	struct ath12k_wmi_mac_addr_params peer_macaddr;
} __packed;
struct wmi_peer_reorder_queue_setup_cmd {
	__le32 tlv_header;
	__le32 vdev_id;
	struct ath12k_wmi_mac_addr_params peer_macaddr;
	__le32 tid;
	__le32 queue_ptr_lo;
	__le32 queue_ptr_hi;
	__le32 queue_no;
	__le32 ba_window_size_valid;
	__le32 ba_window_size;
} __packed;
struct wmi_peer_assoc_complete_cmd {
	__le32 tlv_header;
	struct ath12k_wmi_mac_addr_params peer_macaddr;
	__le32 vdev_id;
	__le32 peer_new_assoc;
	__le32 peer_associd;
	__le32 peer_flags;
	__le32 peer_caps;
	__le32 peer_listen_intval;
	__le32 peer_ht_caps;
	__le32 peer_max_mpdu;
	__le32 peer_mpdu_density;
	__le32 peer_rate_caps;
	__le32 peer_nss;
	__le32 peer_vht_caps;
	__le32 peer_phymode;
	__le32 peer_ht_info[2];
	__le32 num_peer_legacy_rates;
	__le32 num_peer_ht_rates;
	__le32 peer_bw_rxnss_override;
	struct ath12k_wmi_ppe_threshold_params peer_ppet;
	__le32 peer_he_cap_info;
	__le32 peer_he_ops;
	__le32 peer_he_cap_phy[WMI_MAX_HECAP_PHY_SIZE];
	__le32 peer_he_mcs;
	__le32 peer_he_cap_info_ext;
	__le32 peer_he_cap_info_internal;
	__le32 min_data_rate;
	__le32 peer_he_caps_6ghz;
	__le32 sta_type;
	__le32 bss_max_idle_option;
	__le32 auth_mode;
	__le32 peer_flags_ext;
	__le32 punct_bitmap;
	__le32 peer_eht_cap_mac[WMI_MAX_EHTCAP_MAC_SIZE];
	__le32 peer_eht_cap_phy[WMI_MAX_EHTCAP_PHY_SIZE];
	__le32 peer_eht_ops;
	struct ath12k_wmi_ppe_threshold_params peer_eht_ppet;
} __packed;
struct ath12k_wmi_vht_rate_set_params {
	__le32 tlv_header;
	__le32 rx_max_rate;
	__le32 rx_mcs_set;
	__le32 tx_max_rate;
	__le32 tx_mcs_set;
	__le32 tx_max_mcs_nss;
} __packed;
struct wmi_vdev_install_key_cmd {
	__le32 tlv_header;
	__le32 vdev_id;
	struct ath12k_wmi_mac_addr_params peer_macaddr;
	__le32 key_idx;
	__le32 key_flags;
	__le32 key_cipher;
	__le64 key_rsc_counter;
	__le64 key_global_rsc_counter;
	__le64 key_tsc_counter;
	u8 wpi_key_rsc_counter[16];
	u8 wpi_key_tsc_counter[16];
	__le32 key_len;
	__le32 key_txmic_len;
	__le32 key_rxmic_len;
	__le32 is_group_key_id_valid;
	__le32 group_key_id;

	/* Followed by key_data containing key followed by
	 * tx mic and then rx mic
	 */
} __packed;
struct wmi_vdev_install_key_compl_event {
	__le32 vdev_id;
	struct ath12k_wmi_mac_addr_params peer_macaddr;
	__le32 key_idx;
	__le32 key_flags;
	__le32 status;
} __packed;
struct wmi_peer_assoc_conf_event {
	__le32 vdev_id;
	struct ath12k_wmi_mac_addr_params peer_macaddr;
} __packed;
struct wmi_mgmt_send_cmd {
	__le32 tlv_header;
	__le32 vdev_id;
	__le32 desc_id;
	__le32 chanfreq;
	__le32 paddr_lo;
	__le32 paddr_hi;
	__le32 frame_len;
	__le32 buf_len;
	__le32 tx_params_valid;

	/* This TLV is followed by struct wmi_mgmt_frame */

	/* Followed by struct wmi_mgmt_send_params */
} __packed;
struct wmi_mgmt_tx_compl_event {
	__le32 desc_id;
	__le32 status;
	__le32 pdev_id;
} __packed;
struct ath12k_wmi_mgmt_rx_params {
	__le32 channel;
	__le32 snr;
	__le32 rate;
	__le32 phy_mode;
	__le32 buf_len;
	__le32 status;
	__le32 rssi_ctl[ATH_MAX_ANTENNA];
	__le32 flags;
	a_sle32 rssi;
	__le32 tsf_delta;
	__le32 rx_tsf_l32;
	__le32 rx_tsf_u32;
	__le32 pdev_id;
	__le32 chan_freq;
} __packed;
struct wmi_vdev_stopped_event {
	__le32 vdev_id;
} __packed;
struct wmi_peer_delete_resp_event {
	__le32 vdev_id;
	struct ath12k_wmi_mac_addr_params peer_macaddr;
} __packed;
struct wmi_reg_chan_list_cc_ext_event {
	__le32 status_code;
	__le32 phy_id;
	__le32 alpha2;
	__le32 num_phy;
	__le32 country_id;
	__le32 domain_code;
	__le32 dfs_region;
	__le32 phybitmap;
	__le32 min_bw_2g;
	__le32 max_bw_2g;
	__le32 min_bw_5g;
	__le32 max_bw_5g;
	__le32 num_2g_reg_rules;
	__le32 num_5g_reg_rules;
	__le32 client_type;
	__le32 rnr_tpe_usable;
	__le32 unspecified_ap_usable;
	__le32 domain_code_6g_ap_lpi;
	__le32 domain_code_6g_ap_sp;
	__le32 domain_code_6g_ap_vlp;
	__le32 domain_code_6g_client_lpi[WMI_REG_CLIENT_MAX];
	__le32 domain_code_6g_client_sp[WMI_REG_CLIENT_MAX];
	__le32 domain_code_6g_client_vlp[WMI_REG_CLIENT_MAX];
	__le32 domain_code_6g_super_id;
	__le32 min_bw_6g_ap_sp;
	__le32 max_bw_6g_ap_sp;
	__le32 min_bw_6g_ap_lpi;
	__le32 max_bw_6g_ap_lpi;
	__le32 min_bw_6g_ap_vlp;
	__le32 max_bw_6g_ap_vlp;
	__le32 min_bw_6g_client_sp[WMI_REG_CLIENT_MAX];
	__le32 max_bw_6g_client_sp[WMI_REG_CLIENT_MAX];
	__le32 min_bw_6g_client_lpi[WMI_REG_CLIENT_MAX];
	__le32 max_bw_6g_client_lpi[WMI_REG_CLIENT_MAX];
	__le32 min_bw_6g_client_vlp[WMI_REG_CLIENT_MAX];
	__le32 max_bw_6g_client_vlp[WMI_REG_CLIENT_MAX];
	__le32 num_6g_reg_rules_ap_sp;
	__le32 num_6g_reg_rules_ap_lpi;
	__le32 num_6g_reg_rules_ap_vlp;
	__le32 num_6g_reg_rules_cl_sp[WMI_REG_CLIENT_MAX];
	__le32 num_6g_reg_rules_cl_lpi[WMI_REG_CLIENT_MAX];
	__le32 num_6g_reg_rules_cl_vlp[WMI_REG_CLIENT_MAX];
} __packed;
struct ath12k_wmi_reg_rule_ext_params {
	__le32 tlv_header;
	__le32 freq_info;
	__le32 bw_pwr_info;
	__le32 flag_info;
	__le32 psd_power_info;
} __packed;
struct  wmi_start_scan_cmd {
	__le32 tlv_header;
	__le32 scan_id;
	__le32 scan_req_id;
	__le32 vdev_id;
	__le32 scan_priority;
	__le32 notify_scan_events;
	__le32 dwell_time_active;
	__le32 dwell_time_passive;
	__le32 min_rest_time;
	__le32 max_rest_time;
	__le32 repeat_probe_time;
	__le32 probe_spacing_time;
	__le32 idle_time;
	__le32 max_scan_time;
	__le32 probe_delay;
	__le32 scan_ctrl_flags;
	__le32 burst_duration;
	__le32 num_chan;
	__le32 num_bssid;
	__le32 num_ssids;
	__le32 ie_len;
	__le32 n_probes;
	struct ath12k_wmi_mac_addr_params mac_addr;
	struct ath12k_wmi_mac_addr_params mac_mask;
	u32 ie_bitmap[WMI_IE_BITMAP_SIZE];
	__le32 num_vendor_oui;
	__le32 scan_ctrl_flags_ext;
	__le32 dwell_time_active_2g;
	__le32 dwell_time_active_6g;
	__le32 dwell_time_passive_6g;
	__le32 scan_start_offset;
} __packed;
struct wmi_stop_scan_cmd {
	__le32 tlv_header;
	__le32 requestor;
	__le32 scan_id;
	__le32 req_type;
	__le32 vdev_id;
	__le32 pdev_id;
} __packed;
struct wmi_scan_event {
	__le32 event_type; /* %WMI_SCAN_EVENT_ */
	__le32 reason; /* %WMI_SCAN_REASON_ */
	__le32 channel_freq; /* only valid for WMI_SCAN_EVENT_FOREIGN_CHANNEL */
	__le32 scan_req_id;
	__le32 scan_id;
	__le32 vdev_id;
	/* TSF Timestamp when the scan event (%WMI_SCAN_EVENT_) is completed
	 * In case of AP it is TSF of the AP vdev
	 * In case of STA connected state, this is the TSF of the AP
	 * In case of STA not connected, it will be the free running HW timer
	 */
	__le32 tsf_timestamp;
} __packed;
struct wmi_roam_event {
	__le32 vdev_id;
	__le32 reason;
	__le32 rssi;
} __packed;
struct wmi_peer_sta_kickout_event {
	struct ath12k_wmi_mac_addr_params peer_macaddr;
} __packed;
struct wmi_pdev_set_param_cmd {
	__le32 tlv_header;
	__le32 pdev_id;
	__le32 param_id;
	__le32 param_value;
} __packed;
#endif
