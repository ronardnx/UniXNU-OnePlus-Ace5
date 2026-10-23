"""Modem mapping checks: provider resolution, scoping, corruption and source pins."""
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import ios16_modem_board as modem


# An independent, deliberately small numeric DTS fixture. It contains only
# public hardware resources; no captured /chosen node or device identifiers.
FIXTURE = '''/dts-v1/;
/ {
    reserved-memory {
        mpss@8bc00000 {
            reg = <0 0x8bc00000 0 0xf400000>;
            no-map;
            phandle = <0x11>;
        };
        dtb@9b000000 {
            reg = <0 0x9b000000 0 0x80000>;
            no-map;
            phandle = <0x12>;
        };
    };
    soc {
        ipcc@406000 {
            reg = <0x406000 0x1000>;
            #mbox-cells = <2>;
            #interrupt-cells = <3>;
            phandle = <0x20>;
        };
        modem@4080000 {
            compatible = "qcom,pineapple-modem-pas";
            reg = <0x4080000 0x10000>;
            status = "ok";
            memory-region = <0x11 0x12>;
            glink-edge {
                qcom,remote-pid = <1>;
                mboxes = <0x20 2 0>;
                interrupt-parent = <0x20>;
                interrupts = <2 0 1>;
                qcom,modem_qrtr {
                    qcom,glink-channels = "IPCRTR";
                };
            };
        };
        adsp@3000000 {
            glink-edge {
                qcom,remote-pid = <2>;
                mboxes = <0x20 3 0>;
            };
        };
    };
};
'''

IPA_UAPI = '''enum ipa_hw_type { IPA_HW_v5_5 = 24, };
enum ipa_client_type { IPA_CLIENT_APPS_WAN_PROD = 34,
IPA_CLIENT_Q6_WAN_PROD = 52, IPA_CLIENT_TEST_PROD = 62, };
'''
IPA_INTERNAL = '''enum ipa_platform { IPA_PLAT_TYPE_MSM = 1, };
enum ipa_ees { IPA_EE_AP = 0, IPA_EE_Q6 = 1, };
'''
IPA_TABLE = '''#define IPA_v5_5_GROUP_UL (0)
[IPA_5_5][IPA_CLIENT_APPS_WAN_PROD] = {
 true, IPA_v5_5_GROUP_UL, true, IPA_SEQ, QMB_DDR,
 {2, 11, 25, 32, IPA_EE_AP, GSI_SMART_PRE_FETCH, 3}, IPA_TX_INSTANCE_NA },
[IPA_5_5][IPA_CLIENT_Q6_WAN_PROD] = {
 true, IPA_v5_5_GROUP_UL, true, IPA_SEQ, QMB_DDR,
 {12, 11, 16, 28, IPA_EE_Q6, GSI_SMART_PRE_FETCH, 2}, IPA_TX_INSTANCE_NA },
[IPA_5_5][IPA_CLIENT_TEST_PROD] = {
 true, IPA_v5_5_GROUP_UL, true, IPA_SEQ, QMB_DDR,
 {2, 11, 25, 32, IPA_EE_AP, GSI_SMART_PRE_FETCH, 3}, IPA_TX_INSTANCE_NA },
'''


def fixture_map():
    return {
        'dt_nodes': {
            'mpss': {'path': '/reserved-memory/mpss@8bc00000',
                     'cells': {'reg': [0, 0x8bc00000, 0, 0xf400000]}, 'flags': ['no-map']},
            'dtb': {'path': '/reserved-memory/dtb@9b000000',
                    'cells': {'reg': [0, 0x9b000000, 0, 0x80000]}, 'flags': ['no-map']},
            'ipcc': {'path': '/soc/ipcc@406000',
                     'cells': {'reg': [0x406000, 0x1000], '#mbox-cells': [2], '#interrupt-cells': [3]}},
            'modem': {'path': '/soc/modem@4080000',
                      'cells': {'reg': [0x4080000, 0x10000]},
                      'absent': ['power-domains'],
                      'strings': {'compatible': ['qcom,pineapple-modem-pas'], 'status': ['ok']}},
            'glink': {'path': '/soc/modem@4080000/glink-edge',
                      'cells': {'qcom,remote-pid': [1], 'interrupts': [2, 0, 1]}},
            'qrtr': {'path': '/soc/modem@4080000/glink-edge/qcom,modem_qrtr',
                     'strings': {'qcom,glink-channels': ['IPCRTR']}},
        },
        'dt_references': [
            {'source': 'modem', 'property': 'memory-region',
             'specifiers': [{'target': 'mpss'}, {'target': 'dtb'}]},
            {'source': 'glink', 'property': 'mboxes',
             'specifiers': [{'target': 'ipcc', 'args': [2, 0]}]},
            {'source': 'glink', 'property': 'interrupt-parent',
             'specifiers': [{'target': 'ipcc'}]},
        ],
        'android_device_tree': {'decoded_sha256': hashlib.sha256(FIXTURE.encode()).hexdigest()},
    }


class ModemBoard(unittest.TestCase):
    def test_ipa_tuple_order_and_per_environment_channels(self):
        xr = IPA_TABLE.replace('[IPA_5_5]', '[IPA_5_5_XR]')
        data = modem.ipa_routes_from_sources(IPA_TABLE + xr, IPA_UAPI, IPA_INTERNAL)
        self.assertEqual(len(data['production_routes']), 2)
        self.assertEqual(len(data['test_aliases']), 1)
        first = data['production_routes'][0]
        self.assertEqual((first['client_id'], first['endpoint'], first['gsi_channel'],
                          first['execution_environment'], first['ipa_if_tlv'], first['ipa_if_aos']),
                         (34, 2, 11, 0, 25, 32))

    def test_ipa_rejects_missing_fields_and_conflicting_resources(self):
        cases = [IPA_TABLE.replace('16, 28, IPA_EE_Q6', '16, IPA_EE_Q6'),
                 IPA_TABLE.replace('{12, 11', '{2, 11'),
                 IPA_TABLE.replace('IPA_EE_Q6', 'IPA_EE_AP'),
                 IPA_TABLE + IPA_TABLE,
                 IPA_TABLE + '\n[IPA_5_5][IPA_CLIENT_UNKNOWN] = {unparsed},']
        for changed in cases:
            with self.subTest(changed=changed[-50:]), self.assertRaises(ValueError):
                modem.ipa_routes_from_sources(changed, IPA_UAPI, IPA_INTERNAL)

    def test_ipa_rejects_wrong_hardware_enum(self):
        with self.assertRaisesRegex(ValueError, 'version/platform'):
            modem.ipa_routes_from_sources(IPA_TABLE, IPA_UAPI.replace('= 24', '= 23'), IPA_INTERNAL)

    def test_pinned_ipa_sources_when_available(self):
        data = modem.load_manifest()
        if not all((ROOT / item['path']).is_file() for item in data['local_evidence']):
            self.skipTest('Additional local source cache is optional for portable CI')
        self.assertEqual(modem.verify_files(data['local_evidence'], ROOT), len(data['local_evidence']))
        self.assertEqual(modem.verify_ipa_sources(data), 31)
        self.assertEqual(len(data['ipa_endpoint_map']['test_aliases']), 10)
        routes = {row['client']: row for row in data['ipa_endpoint_map']['production_routes']}
        for client, expected in [('APPS_WAN_PROD', (2, 11, 0)),
                                 ('APPS_WAN_CONS', (24, 1, 0)),
                                 ('APPS_WAN_COAL_CONS', (23, 4, 0)),
                                 ('Q6_WAN_PROD', (12, 0, 1)),
                                 ('Q6_WAN_CONS', (22, 7, 1))]:
            row = routes['IPA_CLIENT_' + client]
            self.assertEqual((row['endpoint'], row['gsi_channel'], row['execution_environment']), expected)

    def test_scoped_glink_and_resolved_memory(self):
        result = modem.verify_decoded_dts(fixture_map(), FIXTURE)
        self.assertEqual(result['nodes_verified'], 6)
        self.assertEqual(result['references_verified'], 3)
        self.assertFalse(result['hardware_validated'])

    def test_phandles_are_resolved_not_hardcoded(self):
        changed = FIXTURE.replace('0x11', '0xa1').replace('0x12', '0xa2').replace('0x20', '0xa3')
        modem.verify_decoded_dts(fixture_map(), changed)

    def test_reference_corruption_is_rejected(self):
        cases = [
            ('memory-region = <0x11 0x12>', 'memory-region = <0x12 0x11>'),
            ('memory-region = <0x11 0x12>', 'memory-region = <0x11 0x99>'),
            ('mboxes = <0x20 2 0>', 'mboxes = <0x20 3 0>'),
            ('interrupt-parent = <0x20>', 'interrupt-parent = <0x99>'),
        ]
        for before, after in cases:
            with self.subTest(after=after), self.assertRaisesRegex(ValueError, 'reference mismatch'):
                modem.verify_decoded_dts(fixture_map(), FIXTURE.replace(before, after))

    def test_duplicate_and_invalid_provider_handles(self):
        for value in ('0x11', '0', '0xffffffff'):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, 'phandle'):
                modem.verify_decoded_dts(fixture_map(), FIXTURE.replace('phandle = <0x12>', 'phandle = <' + value + '>'))

    def test_missing_or_wrong_resources_fail(self):
        cases = [
            ('0xf400000', '0xf000000'),
            ('status = "ok";', 'status = "disabled";'),
            ('qcom,remote-pid = <1>;', 'qcom,remote-pid = <2>;'),
            ('no-map;', ''),
            ('#mbox-cells = <2>;', '#mbox-cells = <1>;'),
            ('qcom,glink-channels = "IPCRTR";', ''),
            ('status = "ok";', 'status = "ok";\npower-domains = <1 2>;'),
        ]
        for before, after in cases:
            with self.subTest(before=before), self.assertRaises(ValueError):
                modem.verify_decoded_dts(fixture_map(), FIXTURE.replace(before, after, 1))

    def test_descendant_cannot_impersonate_missing_parent_property(self):
        modified = FIXTURE.replace('status = "ok";', '')
        modified = modified.replace('glink-edge {', 'glink-edge {\nstatus = "ok";', 1)
        with self.assertRaisesRegex(ValueError, 'property: status'):
            modem.verify_decoded_dts(fixture_map(), modified)

    def test_descendant_cannot_impersonate_direct_child(self):
        changed = FIXTURE.replace('ipcc@406000 {', 'unexpected {\nipcc@406000 {', 1)
        changed = changed.replace('phandle = <0x20>;\n        };', 'phandle = <0x20>;\n};\n};', 1)
        with self.assertRaisesRegex(ValueError, 'Missing decoded DTS node: /soc/ipcc'):
            modem.verify_decoded_dts(fixture_map(), changed)

    def test_quoted_braces_do_not_change_node_scope(self):
        changed = FIXTURE.replace('status = "ok";', 'status = "ok";\nnote = "{ public }";')
        # The structural index handles quoted braces. This only checks indexing,
        # as the reused property reader intentionally has a narrower grammar.
        self.assertEqual(set(modem.index_nodes(changed)), set(modem.index_nodes(FIXTURE)))

    def test_duplicate_and_truncated_nodes_are_rejected(self):
        duplicate = FIXTURE.replace('soc {', 'soc {\nipcc@406000 {};', 1)
        for changed in (duplicate, FIXTURE[:-4], FIXTURE + '}'):
            with self.subTest(changed=changed[-30:]), self.assertRaises(ValueError):
                modem.verify_decoded_dts(fixture_map(), changed)

    def test_hash_gate_rejects_even_semantically_identical_new_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'running.dts'
            path.write_bytes(FIXTURE.encode())
            modem.verify_dts(fixture_map(), path)
            path.write_bytes((FIXTURE + '\n').encode())
            with self.assertRaisesRegex(ValueError, 'hash mismatch'):
                modem.verify_dts(fixture_map(), path)

    def test_source_integrity_and_path_confinement(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = b'pinned public modem source\n'
            (root / 'modem.c').write_bytes(raw)
            data = {'source_files': [{'path': 'modem.c', 'bytes': len(raw),
                     'sha256': hashlib.sha256(raw).hexdigest()}]}
            self.assertEqual(modem.verify_sources(data, root), 1)
            (root / 'modem.c').write_bytes(raw + b'changed')
            with self.assertRaisesRegex(ValueError, 'hash/size mismatch'):
                modem.verify_sources(data, root)
            data['source_files'][0]['path'] = '../modem.c'
            with self.assertRaisesRegex(ValueError, 'escapes source root'):
                modem.verify_sources(data, root)

    def test_manifest_covers_modem_dependencies_without_runtime_claims(self):
        data = modem.load_manifest()
        for key in ('hardware_validated', 'modem_boot_validated',
                    'network_registration_validated', 'packet_data_validated'):
            self.assertFalse(data[key])
        expected = {'modem', 'mpss', 'mpss_dtb', 'dsm', 'dsm2', 'qlink_log', 'cma',
                    'smem', 'hwlock', 'ipcc', 'smp2p', 'smp2p_out', 'smp2p_in',
                    'ipa', 'ipa_fw', 'ipa_gsi', 'rmnet', 'gic', 'smmu',
                    'rpmh_clock', 'cx', 'mx', 'qmp', 'mc_virt', 'aggre2_noc',
                    'gem_noc', 'cnoc_main'}
        self.assertTrue(expected <= data['dt_nodes'].keys())
        self.assertEqual(data['dt_nodes']['modem']['strings']['interrupt-names'],
                         ['wdog', 'fatal', 'handover', 'ready', 'stop-ack', 'shutdown-ack'])
        self.assertEqual(data['source_details']['remoteproc']['pas_id'], 4)
        self.assertEqual(data['source_details']['remoteproc']['dtb_pas_id'], 38)
        for source in data['source_files']:
            self.assertIn('/blob/' + source['commit'] + '/', source['url'])
            self.assertRegex(source['sha256'], r'^[0-9a-f]{64}$')

    def test_captured_board_when_available(self):
        data = modem.load_manifest()
        path = ROOT / data['android_device_tree']['decoded_path']
        if not path.is_file():
            self.skipTest('Captured running DT is an optional local artifact')
        result = modem.verify_dts(data, path)
        self.assertGreaterEqual(result['references_verified'], 24)

    def test_cached_sources_when_available(self):
        data = modem.load_manifest()
        root = ROOT / '.cache/audio-source'
        if not all((root / item['path']).is_file() for item in data['source_files']):
            self.skipTest('Pinned OnePlus source cache is optional for portable CI')
        self.assertEqual(modem.verify_sources(data, root), len(data['source_files']))


if __name__ == '__main__':
    unittest.main()
