"""Bounded synthetic ACDB graph parsing and optional local native AML parity."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('graph', ROOT/'scripts/ios16_audio_graph.py')
graph = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(graph)


def u32(*values):
    return struct.pack('<'+str(len(values))+'I', *values)


def chunks():
    # Two graphs share the same single-subgraph topology; only one has an alias.
    return {
        b'GKVT': u32(2, 1, 1, 0xa2000000, 0, 2, 1, 0xa1000000, 0xa2000000, 20),
        b'GKVL': u32(1, 1, 0xa2000001, 0, 16,
                     2, 1, 0xa100000e, 0xa2000001, 0, 16),
        b'POOL': u32(12, 1, 0xb0000002, 0, 0, 8, 4)+b'abc\0',
        b'GALS': u32(1, 1, 1, 0xa2000000, 0xa2000001, 20),
    }


def fixture(tables=None):
    tables = chunks() if tables is None else tables
    payload = b''.join(name+u32(len(data))+data for name, data in tables.items())
    return b'ACDB'+u32(0, len(payload))+payload


class AudioGraph(unittest.TestCase):
    def test_enumerates_graphs_independently_of_optional_alias(self):
        parsed = graph.parse_acdb(fixture())
        self.assertEqual(parsed['graph_count'], 2)
        self.assertEqual(parsed['alias_count'], 1)
        self.assertEqual(parsed['graphs'][0]['alias'], 'abc')
        self.assertIsNone(parsed['graphs'][1]['alias'])
        self.assertEqual(parsed['graphs'][1]['subgraphs'],
                         [{'id': 0xb0000002, 'destinations': []}])
        tables = chunks()
        del tables[b'GALS']
        self.assertEqual(graph.parse_acdb(fixture(tables))['graph_count'], 2)

    def test_header_truncation_compression_and_duplicate_chunks(self):
        raw = fixture()
        for blob in (raw[:11], raw[:-1], raw+b'bad', b'FAIL'+raw[4:],
                     raw[:4]+u32(1)+raw[8:]):
            with self.subTest(blob=blob[:16]), self.assertRaises(ValueError):
                graph.parse_acdb(blob)
        duplicate = raw[12:]+b'POOL'+u32(0)
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            graph.parse_acdb(b'ACDB'+u32(0, len(duplicate))+duplicate)

    def test_key_count_stride_offset_and_sort_checks(self):
        for name, offset, value in ((b'GKVT', 0, 65), (b'GKVT', 4, 65),
                                   (b'GKVT', 8, 100001), (b'GKVT', 16, 0xffffffff),
                                   (b'GKVT', 28, 0xa2000000),
                                   (b'GKVL', 0, 2), (b'GKVL', 4, 100001),
                                   (b'GKVL', 12, 0xffffffff), (b'GKVL', 16, 0xffffffff)):
            tables = chunks()
            changed = bytearray(tables[name])
            struct.pack_into('<I', changed, offset, value)
            tables[name] = bytes(changed)
            with self.subTest(name=name, offset=offset), self.assertRaises(ValueError):
                graph.parse_acdb(fixture(tables))

    def test_pool_subgraph_and_alias_bounds(self):
        for offset, value in ((0, 0xffffffff), (4, 100001), (12, 100001),
                              (20, 6), (24, 257)):
            tables = chunks()
            changed = bytearray(tables[b'POOL'])
            struct.pack_into('<I', changed, offset, value)
            tables[b'POOL'] = bytes(changed)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                graph.parse_acdb(fixture(tables))
        tables = chunks()
        tables[b'POOL'] = tables[b'POOL'][:-1]+b'x'
        with self.assertRaisesRegex(ValueError, 'alias'):
            graph.parse_acdb(fixture(tables))

    def test_pin_checked_before_parsing(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'fixture.acdb'
            path.write_bytes(fixture())
            with self.assertRaisesRegex(ValueError, 'SHA256'):
                graph.verify_acdb(path)
            parsed = graph.verify_acdb(path, hashlib.sha256(fixture()).hexdigest())
            self.assertEqual(parsed['graph_count'], 2)

    def test_native_vector_blob_parser(self):
        self.assertEqual(graph.key_vectors(u32(0, 1, 8, 9), 2), [(), ((8, 9),)])
        for raw, count in ((u32(65), 1), (u32(1, 8), 1), (u32(0, 1), 1)):
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                graph.key_vectors(raw, count)

    def test_apm_module_list_ids_and_padding(self):
        payload = u32(1, 0xb0000002, 0xe0000002, 1, 0x0700100a, 0x42c8)
        raw = u32(1, 0x08001002, len(payload), 0)+payload
        parsed = graph.apm_parameters(raw)
        module = parsed[0]['module_lists'][0]['modules'][0]
        self.assertEqual(module, {'module_id': 0x0700100a, 'instance_id': 0x42c8})
        for bad in (raw[:-1], raw[:12]+u32(1)+raw[16:], raw[:16]+u32(100001)+raw[20:]):
            with self.assertRaises(ValueError):
                graph.apm_parameters(bad)

    def test_amdb_capi_v3_name_tag_and_bounds(self):
        record = struct.pack('<HHIIHHI', 2, 3, 2, 0x1700e900, 4, 4, 0)+b'a.so'+b'tag0'
        raw = u32(2, 1, 1)+record
        entry = graph.amdb_registration(raw)[0]
        self.assertEqual((entry['module_id'], entry['filename'], entry['tag']),
                         (0x1700e900, 'a.so', 'tag0'))
        for bad in (raw[:-1], raw+b'x', u32(3)+raw[4:], raw[:24]+struct.pack('<HH', 65535, 4)+raw[28:]):
            with self.assertRaises(ValueError):
                graph.amdb_registration(bad)

    def test_native_stock_enumeration_and_export_when_available(self):
        library = ROOT/'.cache/audio-acdb-host/libacdb.so'
        firmware = ROOT/'.cache/audio-speaker-stock/acdb_cal.acdb'
        if os.name != 'posix' or not library.exists() or not firmware.exists():
            self.skipTest('Optional pinned stock + host-built official AML parity test (WSL/Linux)')
        parsed = graph.verify_acdb(firmware)
        host = graph.AcdbHost(library, firmware)
        try:
            native = host.get_vectors(21, graph.UintList())
            self.assertEqual(set(native), {g['key_vector'] for g in parsed['graphs']})
            for entry in parsed['graphs']:
                self.assertEqual(host.graph(entry['key_vector'])[0], entry['subgraphs'])
        finally:
            host.close()
        key = ((0xa1000000, 0xa100000e), (0xa2000000, 0xa2000001),
               (0xab000000, 1), (0xac000000, 0xac000002))
        selected = next(g for g in parsed['graphs'] if g['key_vector'] == key)
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)/'export'
            report = graph.export_graph(firmware, library, selected, output)
            self.assertFalse(report['playback_verified'])
            self.assertEqual(len(report['calibration_key_vectors']), 22)
            self.assertEqual(report['dynamic_module_dependencies'][0]['filename'], 'libsixth_rx_v3.so')
            modules = [m for p in report['apm_parameters'] for ml in p.get('module_lists', [])
                       for m in ml['modules']]
            self.assertIn({'module_id': 0x0700100a, 'instance_id': 0x42c8}, modules)
            for file in report['files']:
                data = (output/file['path']).read_bytes()
                self.assertEqual(len(data), file['bytes'])
                self.assertEqual(hashlib.sha256(data).hexdigest(), file['sha256'])
            self.assertEqual(json.loads((output/'manifest.json').read_text())['status'],
                             'static-graph-exported-runtime-calibration-unverified')

    def test_apm_c_transaction_engine(self):
        compiler = os.environ.get('CC') or next(
            (path for name in ('clang', 'cc', 'gcc') if (path := shutil.which(name))), None)
        if not compiler:
            self.skipTest('Set CC or run under WSL with a C compiler for APM control tests')
        with tempfile.TemporaryDirectory() as folder:
            binary = Path(folder)/('graph.exe' if os.name == 'nt' else 'graph')
            build = subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                                    str(ROOT/'tests/ios16_audio_graph.c'), '-o', str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout+build.stderr)
            command = [str(binary)]
            local = ROOT/'.cache/audio-acdb-host/speaker-ll-mbdrc-v4'
            if (local/'manifest.json').exists():
                command.append(str(local))
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            self.assertIn('APM graph transaction checks passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
