"""Pinned transport evidence and executable SMEM/GLINK/GPR peer regressions."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AudioTransport(unittest.TestCase):
    def test_manifest_agrees_with_captured_board_mapping(self):
        manifest = json.loads((ROOT / 'hardware/sm8650-audio-transport.json').read_text())
        board = json.loads((ROOT / 'hardware/sm8650-audio.json').read_text())
        self.assertFalse(manifest['hardware_validated'])
        self.assertFalse(manifest['playback_validated'])
        self.assertEqual(manifest['glink']['channel'], board['transport']['gpr_channel'])
        for transport_key, board_key in (('inbound_item', 'smem_in'),
                                         ('outbound_item', 'smem_out'),
                                         ('fatal_bit', 'fatal_bit'),
                                         ('ready_bit', 'ready_bit'),
                                         ('ipcc_signal', 'ipcc_signal')):
            self.assertEqual(manifest['smp2p'][transport_key],
                             board['transport']['smp2p'][board_key])
        glink = board['source_details']['glink']
        for transport_key, board_key in (('descriptor_item', 'descriptor_smem_id'),
                                         ('tx_item', 'tx_fifo_smem_id'),
                                         ('rx_item', 'rx_fifo_smem_id')):
            self.assertEqual(manifest['smem'][transport_key], glink[board_key])
        board_sources = {record['path']: record for record in board['source_files']}
        for source in manifest['source_files']:
            self.assertRegex(source['commit'], r'^[0-9a-f]{40}$')
            self.assertIn('/blob/' + source['commit'] + '/', source['url'])
            if source['path'] in board_sources:
                self.assertEqual(source['sha256'], board_sources[source['path']]['sha256'])

    def test_cached_pinned_source_integrity_when_available(self):
        manifest = json.loads((ROOT / 'hardware/sm8650-audio-transport.json').read_text())
        cache = ROOT / '.cache/audio-source'
        if not cache.is_dir():
            self.skipTest('Pinned upstream source cache is optional for host-only CI')
        verified = 0
        for source in manifest['source_files']:
            path = cache / source['path']
            if not path.is_file():
                continue
            with self.subTest(source=source['path']):
                data = path.read_bytes()
                self.assertEqual(len(data), source['bytes'])
                self.assertEqual(hashlib.sha256(data).hexdigest(), source['sha256'])
                verified += 1
        if not verified:
            self.skipTest('Transport source subset is not present in the optional cache')

    def test_native_peer_regressions(self):
        compiler = os.environ.get('CC') or next(
            (path for name in ('clang', 'cc', 'gcc') if (path := shutil.which(name))), None)
        if not compiler:
            self.skipTest('Install a host C compiler or run scripts/test-ios16-audio.sh in WSL')
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / ('audio-transport.exe' if os.name == 'nt' else 'audio-transport')
            command = [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                       str(ROOT / 'tests/ios16_audio_transport.c'), '-o', str(binary)]
            # The shell suite always uses sanitizers. This entry also supports
            # compilers/platforms whose sanitizer runtimes are not installed.
            build = subprocess.run(command, capture_output=True, text=True, timeout=90)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('SMEM, SMP2P, GLINK and token-correlated SPF tests passed', result.stdout)

    def test_cpp14_header_compatibility(self):
        compiler = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            self.skipTest('Install a host C++ compiler for the IOKit header compatibility check')
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run(
                [compiler, '-x', 'c++', '-std=c++14', '-Wall', '-Wextra', '-Werror',
                 '-c', str(ROOT / 'tests/ios16_audio_transport.c'),
                 '-o', str(Path(directory) / 'audio-transport.o')],
                capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
