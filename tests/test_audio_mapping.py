"""Reproducible audio mapping and local evidence integrity."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
from ios16_audio import HEADER, load_mapping, render_header, sha256, verify_dts, verify_files


class AudioMapping(unittest.TestCase):
    def test_generated_header_matches_pinned_manifest(self):
        mapping = load_mapping()
        self.assertEqual(HEADER.read_text(), render_header(mapping))
        subprocess.run([sys.executable, str(ROOT/'scripts/check-audio-mapping.py')], check=True)

    def test_board_topology_and_distinct_address_spaces(self):
        data = load_mapping()
        regions = {r['id']: r for r in data['regions']}
        self.assertEqual(regions['RX_SWR']['dt_cells'], [0x6ad0000, 0])
        self.assertEqual(regions['RX_SWR']['bytes'], 0x60a8)
        self.assertFalse(regions['WSA']['enabled'])
        self.assertFalse(regions['WSA2_SWR']['enabled'])
        self.assertEqual(regions['ADSP_RAM']['kind'], 'reserved')
        self.assertFalse(data['audio_dma']['xnu_owned'])
        self.assertEqual(data['source_details']['board']['speaker_codec_names'],
                         ['sipa.4-006c', 'sipa.4-006d'])
        self.assertEqual(data['source_details']['wcd939x']['regmap_bus_slave'], 'tx')
        self.assertEqual([(r['spi'], r['intid']) for r in data['gic_interrupts']],
                         [(486,518),(229,261),(155,187),(496,528),(520,552),(471,503),(356,388)])
        self.assertFalse(data['playback'])
        self.assertFalse(data['recording'])

    def test_invalid_resource_is_rejected(self):
        data = load_mapping()
        for field, value in [('base', 2**64), ('bytes', -1), ('id', 'bad-id'),
                             ('enabled', 'disabled'), ('kind', 'i2c')]:
            invalid = copy.deepcopy(data)
            invalid['regions'][0][field] = value
            with self.subTest(field=field), tempfile.TemporaryDirectory() as directory:
                path = Path(directory)/'map.json'
                path.write_text(json.dumps(invalid))
                with self.assertRaises(ValueError):
                    load_mapping(path)

    def test_evidence_hash_size_and_path_confinement(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder/'source.c').write_bytes(b'fixture\n')
            records = [{'path':'source.c', 'bytes':8, 'sha256':sha256(b'fixture\n')}]
            self.assertEqual(verify_files(folder, records), 1)
            (folder/'source.c').write_bytes(b'changed\n')
            with self.assertRaises(ValueError): verify_files(folder, records)
            records[0]['path'] = '../outside'
            with self.assertRaises(ValueError): verify_files(folder, records)

    def test_dts_is_pinned_and_child_cannot_impersonate_parent(self):
        fixture = b'/dts-v1/;\n/ {\n codec {\n  reg = <0x10 0x20>;\n };\n};\n'
        data = {'android_device_tree':{'decoded_sha256':sha256(fixture)},
                'dt_checks':[{'node':'codec','property':'reg','type':'cells','value':[16,32]}]}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'fixture.dts'
            path.write_bytes(fixture)
            self.assertEqual(verify_dts(data, path), 1)
            path.write_bytes(fixture.replace(b'0x20', b'0x30'))
            with self.assertRaisesRegex(ValueError, 'hash mismatch'): verify_dts(data, path)
            nested = fixture.replace(b'  reg = <0x10 0x20>;', b'  child {\n   reg = <0x10 0x20>;\n  };')
            path.write_bytes(nested)
            data['android_device_tree']['decoded_sha256'] = sha256(nested)
            with self.assertRaises(ValueError): verify_dts(data, path)


if __name__ == '__main__':
    unittest.main()
