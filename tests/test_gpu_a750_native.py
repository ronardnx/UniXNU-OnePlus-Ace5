import importlib.util
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / '.cache' / 'gpu-source'
HEADER = ROOT / 'ios16' / 'native' / 'giulia_a750_native.h'
MANIFEST = ROOT / 'hardware' / 'giulia-a750-native.json'
CAPTURE = ROOT / 'out' / 'ios16' / 'gpu-native-hwsched-terminal' / '20261023-194032-290592' / 'result.json'

spec = importlib.util.spec_from_file_location('derive_a750', ROOT / 'scripts' / 'derive-a750-native.py')
derive_mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(derive_mod)


def load():
    pins = json.loads((ROOT / 'hardware' / 'sm8650-gpu.json').read_text())
    return pins, derive_mod.derive(VENDOR, pins)


class A750PlatformData(unittest.TestCase):
    def test_generated_header_matches_the_pinned_sources(self):
        self.assertTrue(HEADER.exists(), 'run scripts/derive-a750-native.py first')
        if not VENDOR.exists():
            self.skipTest('vendor sources are fetched by fetch-ios16-gpu-sources.py')
        pins, data = load()
        self.assertEqual(HEADER.read_text(), derive_mod.render(data))

    def test_manifest_binds_the_rendered_bytes(self):
        pins, data = load()
        manifest = json.loads(MANIFEST.read_text())
        rendered = derive_mod.render(data).encode()
        import hashlib
        self.assertEqual(manifest['rendered_sha256'], hashlib.sha256(rendered).hexdigest())
        self.assertEqual(manifest['tables'], {'ao_hwcg': 3, 'gbif': 5, 'protect': 48, 'pwrup': 17})
        self.assertEqual(manifest['gmem_bytes'], 0x300000)
        self.assertEqual(manifest['uche_gmem_alignment'], 0x1000000)
        self.assertFalse(manifest['hardware_translation_verified'])

    def test_every_value_comes_from_the_sources(self):
        """A tampered source must change the output: no fallback values exist."""
        if not VENDOR.exists():
            self.skipTest('vendor sources are fetched by fetch-ios16-gpu-sources.py')
        pins = json.loads((ROOT / 'hardware' / 'sm8650-gpu.json').read_text())
        baseline = derive_mod.render(derive_mod.derive(VENDOR, pins))
        with tempfile.TemporaryDirectory() as folder:
            tmp = Path(folder)
            for name in ('adreno-gpulist.h', 'adreno_gen7.c', 'gen7_reg.h'):
                shutil.copyfile(VENDOR / name, tmp / name)
            gpulist = (tmp / 'adreno-gpulist.h').read_text()
            # 0x27880 is unique to the gen7_9_0 span list the A750 core uses.
            changed = gpulist.replace('{ GEN7_CP_PROTECT_REG + 46, 0x27880, 0x27c01, 0 }',
                                      '{ GEN7_CP_PROTECT_REG + 46, 0x27880, 0x27c02, 0 }', 1)
            self.assertNotEqual(changed, gpulist, 'protect span anchor not found')
            (tmp / 'adreno-gpulist.h').write_text(changed)
            tampered = derive_mod.render(derive_mod.derive(tmp, pins))
            self.assertNotEqual(baseline, tampered)
            self.assertIn('{46,0x27880,0x27c02,0}', tampered)

    def test_unknown_register_symbol_is_refused(self):
        if not VENDOR.exists():
            self.skipTest('vendor sources are fetched by fetch-ios16-gpu-sources.py')
        with tempfile.TemporaryDirectory() as folder:
            tmp = Path(folder)
            for name in ('adreno-gpulist.h', 'adreno_gen7.c', 'gen7_reg.h'):
                shutil.copyfile(VENDOR / name, tmp / name)
            gen7 = (tmp / 'adreno_gen7.c').read_text()
            (tmp / 'adreno_gen7.c').write_text(
                gen7.replace('GEN7_UCHE_TRAP_BASE_LO', 'GEN7_NOT_A_REGISTER', 1))
            pins = json.loads((ROOT / 'hardware' / 'sm8650-gpu.json').read_text())
            with self.assertRaises(ValueError):
                derive_mod.derive(tmp, pins)

    def test_protect_words_match_the_words_the_device_received(self):
        """The r5 capture recorded what the host actually wrote for +0 and +1."""
        if not CAPTURE.exists():
            self.skipTest('r5 capture is a local out/ artifact')
        recorded = {}
        result = json.loads(CAPTURE.read_text())
        for entry in result.get('configured_registers', []):
            if entry['name'].startswith('GEN7_CP_PROTECT_REG+'):
                recorded[entry['name'].split('+')[1]] = int(entry['value'], 0)
        self.assertTrue(recorded)
        pins, data = load()
        for row in data['protect']:
            index, start, end, deny = row
            word = start | ((end - start) << 18) | (deny << 31)
            key = str(index)
            if key in recorded:
                self.assertEqual(word, recorded[key],
                                 f'CP_PROTECT_REG+{key} differs from the r5 capture')


if __name__ == '__main__':
    unittest.main()
