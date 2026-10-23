"""Offline source oracle: verify Linux v6.12 layouts and WCN7850 DP semantics.

The committed fixture is extracted from independently downloaded upstream files,
never from giulia_wifi_dp.h. This script does not download or build anything.
The C oracle also works without this optional source cache.
"""
import argparse
import hashlib
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
HASHES = {
    'hal_desc.h': 'ec51186d6e434c55dbe5327907d445c2e2e7ceefec9f315f5667b4c926a9de48',
    'rx_desc.h': '6745559e235ed23de4443867ae6873374d9572afa1b347915402778c17a584c8',
    'hal.h': '6dfcdf9b695305de86d75ef825088dd6d708c735f7fa7c2209ddb40333b450e4',
    'hal_tx.h': 'f89b88f31cfa206f2eb338747d690b315991cbb1bb940f2e99185bfbbb9be865',
    'hal.c': '09c1d8e2b2853f0491552cf518e5d4209cefef25e942353ecf45d8a8dc420485',
    'dp.h': '1f6b571e99766294c5f5b589c8fd852b1d9e072b15abf0cac1d19442fee4cc3e',
    'dp_tx.c': '89342e82ba35a5e0fff590961ce34e664a17f5af426c77d795f583bf882cda2a',
    'hw.h': '0d55385d2f458a84563f9589e78418906718542c60ba318d9214a8ce73c5cf0c',
    'mac.c': 'a3e221e1a3ae2733dbe6b21e2bdcdf26f5838e94dd498fa3fef330730138aa28',
    'ath12k-hw.c': '8cf23c67667ed56f9ba2cab3673a8c7f7e5888092db9f82643f649ce2b771652',
}
STRUCTS = {
    'hal_desc.h': '''ath12k_buffer_addr rx_mpdu_desc rx_msdu_desc rx_msdu_ext_desc
        hal_tcl_data_cmd hal_tx_rate_stats hal_wbm_completion_ring_tx
        hal_reo_dest_ring hal_reo_entrance_ring hal_desc_header
        hal_rx_msdu_details hal_rx_msdu_link hal_wbm_release_ring
        hal_rx_mpdu_link_ptr hal_rx_reo_queue_ext hal_rx_reo_queue
        hal_tlv_64_hdr hal_reo_cmd_hdr hal_reo_get_queue_stats hal_reo_update_rx_queue
        hal_reo_flush_cache hal_reo_status_hdr hal_reo_get_queue_stats_status
        hal_reo_flush_cache_status'''.split(),
    'rx_desc.h': '''rx_msdu_end_wcn7850 rx_mpdu_start_qcn9274 rx_pkt_hdr_tlv
        hal_rx_desc_wcn7850'''.split(),
    'dp.h': ['htt_srng_setup_cmd', 'htt_rx_ring_selection_cfg_cmd'],
}
MACROS = '''RX_BE_PADDING0_BYTES HAL_RX_BE_PKT_HDR_TLV_LEN
    BUFFER_ADDR_INFO1_RET_BUF_MGR BUFFER_ADDR_INFO1_SW_COOKIE
    HAL_TLV_64_HDR_TAG HAL_TLV_64_HDR_LEN
    RX_MSDU_DESC_INFO0_FIRST_MSDU_IN_MPDU RX_MSDU_DESC_INFO0_LAST_MSDU_IN_MPDU
    RX_MSDU_DESC_INFO0_MSDU_LENGTH HAL_REO_DEST_RING_INFO0_BUFFER_TYPE
    HAL_REO_DEST_RING_INFO0_PUSH_REASON HAL_TCL_DATA_CMD_INFO0_BANK_ID HAL_TCL_DATA_CMD_INFO1_CMD_NUM
    HAL_TCL_DATA_CMD_INFO2_DATA_LEN HAL_TCL_DATA_CMD_INFO3_TID_OVERWRITE
    HAL_TCL_DATA_CMD_INFO3_PMAC_ID HAL_TCL_DATA_CMD_INFO3_VDEV_ID
    HAL_TCL_DATA_CMD_INFO4_SEARCH_INDEX HAL_TCL_DATA_CMD_INFO4_CACHE_SET_NUM
    RX_MSDU_END_INFO10_MSDU_LENGTH RX_MSDU_END_INFO11_DECAP_FORMAT
    RX_MSDU_END_INFO14_MSDU_DONE RX_MSDU_END_INFO14_DECRYPT_STATUS_CODE
    RX_MPDU_START_INFO4_MPDU_FCTRL_VALID RX_MPDU_START_INFO4_MAC_ADDR2_VALID
    RX_MPDU_START_INFO4_ENCRYPT_INFO_VALID RX_MPDU_START_INFO4_ENCRYPTED
    RX_MPDU_START_INFO4_MPDU_QOS_CTRL_VALID RX_MPDU_START_INFO4_MORE_FRAG_FLAG
    RX_MPDU_START_INFO2_TID RX_MPDU_START_INFO2_ENC_TYPE RX_MPDU_START_INFO5_KEY_ID
    RX_MSDU_END_INFO13_FCS_ERR RX_MSDU_END_INFO13_BUFFER_FRAGMENT RX_MSDU_END_INFO13_MCAST_BCAST
    HTT_TCL_META_DATA_TYPE HTT_TCL_META_DATA_VDEV_ID HTT_TCL_META_DATA_PDEV_ID
    HTT_TX_WBM_COMP_INFO0_STATUS HAL_WBM_COMPL_TX_INFO0_REL_SRC_MODULE
    HAL_TX_RATE_STATS_INFO0_VALID HAL_TX_RATE_STATS_INFO0_BW
    HAL_TX_RATE_STATS_INFO0_PKT_TYPE HAL_TX_RATE_STATS_INFO0_SGI HAL_TX_RATE_STATS_INFO0_MCS
    RX_MSDU_END_INFO12_PKT_TYPE RX_MSDU_END_INFO12_SGI RX_MSDU_END_INFO12_RATE_MCS
    RX_MSDU_END_INFO12_RECV_BW RX_MSDU_END_WCN7850_INFO12_MIMO_SS_BITMAP
    HAL_SEQ_WCSS_UMAC_REO_REG HAL_SEQ_WCSS_UMAC_WBM_REG HAL_SEQ_WCSS_UMAC_TCL_REG
    HAL_TCL_SW_CONFIG_BANK_ADDR
    HAL_TCL1_RING_BASE_LSB HAL_TCL1_RING_HP HAL_REO1_RING_HP HAL_REO_SW0_RING_HP
    HAL_WBM0_RELEASE_RING_HP HAL_WBM1_RELEASE_RING_HP HAL_WBM_IDLE_LINK_RING_HP
    HAL_WBM_SW_RELEASE_RING_HP HAL_WBM2SW_REL_ERR_RING_NUM
    HAL_TX_BANK_CONFIG_ENCAP_TYPE HAL_TX_BANK_CONFIG_ADDRY_EN
    HAL_TX_BANK_CONFIG_VDEV_ID_CHECK_EN HAL_REO_CMD_HP HAL_REO_STATUS_HP
    HAL_TLV_HDR_TAG HAL_TLV_HDR_LEN HAL_REO_CMD_HDR_INFO0_CMD_NUMBER
    HAL_REO_CMD_HDR_INFO0_STATUS_REQUIRED HAL_REO_UPD_RX_QUEUE_INFO0_UPD_VLD
    HAL_REO_UPD_RX_QUEUE_INFO1_VLD HAL_REO_STATUS_HDR_INFO0_STATUS_NUM
    HAL_REO_STATUS_HDR_INFO0_EXEC_STATUS HAL_REO_FLUSH_CACHE_INFO0_CACHE_ADDR_HI
    HAL_REO_FLUSH_CACHE_INFO0_FLUSH_WO_INVALIDATE HAL_REO_FLUSH_CACHE_INFO0_BLOCK_CACHE_USAGE
    HAL_REO_FLUSH_CACHE_STATUS_INFO0_IS_ERR HAL_REO_FLUSH_CACHE_STATUS_INFO0_BLOCK_ERR_CODE
    HAL_REO_FLUSH_CACHE_STATUS_INFO0_FLUSH_ERR'''.split()
VALUES = '''HAL_REO_UPDATE_RX_REO_QUEUE HAL_REO_UPDATE_RX_REO_QUEUE_STATUS
    HAL_REO_FLUSH_CACHE HAL_REO_FLUSH_CACHE_STATUS'''.split()
ENUMS = '''hal_wbm_rel_src_module hal_wbm_htt_tx_comp_status hal_wbm_tqm_rel_reason
    hal_tx_rate_stats_bw hal_tx_rate_stats_pkt_type hal_tx_rate_stats_sgi
    ath12k_hw_rate_cck ath12k_hw_rate_ofdm rx_msdu_start_pkt_type
    rx_msdu_start_sgi rx_msdu_start_recv_bw'''.split()
ENUM_FLAGS = '''HTT_RX_FP_DATA_PKT_FILTER_TLV_FLASG3_MCAST
    HTT_RX_FP_DATA_PKT_FILTER_TLV_FLASG3_UCAST
    HTT_RX_FP_DATA_PKT_FILTER_TLV_FLASG3_NULL_DATA'''.split()


def rx_semantics(source):
    """Extract HAL statements, adapting only descriptor and endian/bit helpers.

    The adapters provide the Linux endian and bitfield operations on the host.
    No KEY_ID or MCBC interpretation is handwritten in the generated helpers.
    hal.c's pinned full-file hash also covers the surrounding source functions.
    """
    crypto = re.search(r'^static void ath12k_hw_wcn7850_rx_desc_get_crypto_hdr\(.*?^\}',
                       source, re.M | re.S)
    assert crypto, 'missing WCN7850 crypto-header function'
    key = re.search(r'\tkey_id = .*?;\n\tcrypto_hdr\[3\] = [^;]+;', crypto[0], re.S)
    assert key and 'RX_MPDU_START_INFO5_KEY_ID' in key[0], 'missing WCN7850 KEY_ID conversion'
    mcbc = re.search(r'^static bool ath12k_hw_wcn7850_rx_desc_is_da_mcbc\(.*?^\}',
                     source, re.M | re.S)
    assert mcbc, 'missing WCN7850 MCBC function'
    group = re.search(r'\treturn .*?;', mcbc[0], re.S)
    assert group and 'RX_MSDU_END_INFO13_MCAST_BCAST' in group[0], 'missing WCN7850 MCBC return'

    def adapt(statements):
        return (statements.replace('desc->u.wcn7850.', 'desc->')
                .replace('__le32_to_cpu', 'oracle_le32_to_cpu')
                .replace('u32_get_bits', 'oracle_u32_get_bits'))

    return '''
/* hal.c: host adapters plus exact WCN7850 KEY_ID/MCBC statements. */
static inline uint32_t oracle_le32_to_cpu(__le32 value) {
    const uint8_t *p = (const uint8_t *)&value;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint32_t oracle_u32_get_bits(uint32_t word, uint32_t mask) {
    while (!(mask & 1)) { word >>= 1; mask >>= 1; }
    return word & mask;
}
static inline uint8_t oracle_wcn7850_ccmp_key_id_octet(const struct hal_rx_desc_wcn7850 *desc) {
    unsigned int key_id;
    uint8_t crypto_hdr[4] = {0};
''' + adapt(key[0]) + '''
    return crypto_hdr[3];
}
static inline int oracle_wcn7850_is_da_mcbc(const struct hal_rx_desc_wcn7850 *desc) {
''' + adapt(group[0]) + '\n}\n'


def tx_completion_semantics(source):
    """Derive DMA-release cases from the pinned HTT TX completion dispatch.

    Preserve the upstream case labels. Each branch becomes a boolean showing
    whether it calls a TX completion/free helper; unrelated status reporting
    and skb plumbing are omitted. This is not a firmware-notification parser.
    """
    function = re.search(r'^ath12k_dp_tx_process_htt_tx_complete\(.*?^\}', source, re.M | re.S)
    assert function, 'missing HTT TX completion dispatch'
    switch = re.search(r'\tswitch \(wbm_status\) \{.*?\n\t\}', function[0], re.S)
    assert switch, 'missing HTT TX status switch'
    blocks = re.findall(r'((?:\tcase [^:\n]+:\n)+)(.*?)(?=\t(?:case |default:))', switch[0], re.S)
    assert blocks and sum(labels.count('case ') for labels, _ in blocks) == switch[0].count('case ')
    out = ['\n/* dp_tx.c: DMA-release classification from the upstream status cases. */\n',
           'static inline int oracle_htt_tx_releases_dma(unsigned wbm_status) {\n',
           '    switch (wbm_status) {\n']
    for labels, body in blocks:
        releases = bool(re.search(r'ath12k_dp_tx_(?:htt_tx_complete_buf|free_txbuf)\(', body))
        out.extend([labels, '        return ' + str(int(releases)) + ';\n'])
    out.append('    default: return 0;\n    }\n}\n')
    return ''.join(out)


def legacy_rate_semantics(source):
    """Resolve the independently pinned mac.c rate table via hw.h's enums."""
    table = re.search(r'^static struct ieee80211_rate ath12k_legacy_rates\[\] = \{(.*?)^\};',
                      source, re.M | re.S)
    assert table, 'missing legacy hardware rate table'
    entries = re.findall(r'\{([^{}]+)\}', table[1])
    assert len(entries) == 12, 'unexpected legacy rate table'
    out = ['\n/* mac.c: bitrate is in 100 kbps; HW codes resolve through hw.h. */\n',
           'struct oracle_legacy_rate {unsigned packet_type, code, kbps;};\n',
           'static const struct oracle_legacy_rate oracle_legacy_rates[] = {\n']
    for entry in entries:
        bitrate = re.search(r'\.bitrate = (\d+)', entry)
        hardware = re.search(r'\.hw_value = (ATH12K_HW_RATE_\w+)', entry)
        short = re.search(r'\.hw_value_short = (ATH12K_HW_RATE_\w+)', entry)
        assert bitrate and hardware, entry
        packet_type = 'HAL_TX_RATE_STATS_PKT_TYPE_11B' if '_CCK_' in hardware[1] else 'HAL_TX_RATE_STATS_PKT_TYPE_11A'
        for code in [hardware[1]] + ([short[1]] if short else []):
            out.append(f'    {{{packet_type}, {code}, {bitrate[1]}u * 100u}},\n')
    out.append('''};
static inline uint32_t oracle_legacy_rate_kbps(unsigned packet_type, unsigned code) {
    for (unsigned i = 0; i < sizeof(oracle_legacy_rates) / sizeof(oracle_legacy_rates[0]); i++)
        if (oracle_legacy_rates[i].packet_type == packet_type && oracle_legacy_rates[i].code == code)
            return oracle_legacy_rates[i].kbps;
    return 0;
}
''')
    return ''.join(out)


def fixture(source):
    files = {}
    for name, digest in HASHES.items():
        data = source.joinpath(name).read_bytes().replace(b'\r\n', b'\n').rstrip() + b'\n'
        assert hashlib.sha256(data).hexdigest() == digest, f'not pinned Linux v6.12: {name}'
        files[name] = data.decode()
    out = ['''/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (c) 2018-2021 The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * Generated by tests/ios16_wifi_dp_oracle.py from Linux v6.12 ath12k:
 * https://github.com/torvalds/linux/tree/v6.12/drivers/net/wireless/ath/ath12k
 * Exact layouts and extracted HAL statements form an independent oracle.
 * Do not regenerate from the Giulia driver. See the generator for source hashes.
 */
#ifndef ATH12K_WIFI_LAYOUT_ORACLE_H
#define ATH12K_WIFI_LAYOUT_ORACLE_H
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef uint64_t u64;
#define __packed __attribute__((packed))
#define ETH_ALEN 6
#define BIT(n) (UINT32_C(1) << (n))
#define GENMASK(h,l) ((UINT32_MAX >> (31-(h))) & (UINT32_MAX << (l)))
''']
    corpus = '\n'.join(files.values())
    for name in MACROS:
        match = re.search(r'^#define ' + name + r'\s+[^\n]+', corpus, re.M)
        assert match, name
        assert not match[0].endswith('\\'), name
        out.append(match[0] + '\n')
    for name in VALUES:
        match = re.search(r'^\s*' + name + r'\s*=\s*(\d+)\b', corpus, re.M)
        assert match, name
        out.append('#define ' + name + ' ' + match[1] + '\n')
    for name in ENUMS:
        match = re.search(r'^enum ' + name + r' \{.*?^\};', corpus, re.M | re.S)
        assert match, name
        out.append(match[0] + '\n')
    for name in ENUM_FLAGS:
        match = re.search(r'^\s*' + name + r'\s*=\s*(BIT\(\d+\)),', corpus, re.M)
        assert match, name
        out.append('#define ' + name + ' ' + match[1] + '\n')
    for file, structs in STRUCTS.items():
        out.append('\n/* ' + file + ' */\n')
        for name in structs:
            match = re.search(r'^struct ' + name + r' \{.*?^\}(?: __packed)?;', files[file], re.M | re.S)
            assert match, name
            out.append(match[0] + '\n')
    out.append(rx_semantics(files['hal.c']))
    out.append(tx_completion_semantics(files['dp_tx.c']))
    out.append(legacy_rate_semantics(files['mac.c']))
    regs = re.search(r'wcn7850_regs = \{(.*?)\n\};', files['ath12k-hw.c'], re.S)[1]
    for name, value in re.findall(r'\.(\w+)\s*=\s*(0x[\da-fA-F]+)', regs):
        out.append('#define ORACLE_' + name + ' ' + value + '\n')
    out.append('#endif\n')
    return ''.join(out)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / '.cache/wifi-native')
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    generated = fixture(args.source)
    target = ROOT / 'tests/fixtures/ath12k_wifi_layout.h'
    if args.write:
        target.parent.mkdir(exist_ok=True)
        target.write_text(generated, encoding='utf-8', newline='\n')
    else:
        assert target.read_text(encoding='utf-8') == generated, 'upstream layout fixture drifted'
    print(f'Linux v6.12 source oracle: {len(HASHES)} hashes, {sum(map(len, STRUCTS.values()))} exact structures, {len(MACROS)} field macros, {len(VALUES)} command/status tags, {len(ENUMS)} enums, {len(ENUM_FLAGS)} HTT RX flags, 3 DP semantic excerpts and legacy hardware rate table verified')
