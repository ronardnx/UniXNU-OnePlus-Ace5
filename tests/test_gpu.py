import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
import ios16_gpu as gpu


class GpuInputs(unittest.TestCase):
    def test_child_property_cannot_replace_parent(self):
        text = '\nparent {\n child {\n reg = <0x1000 0x100>;\n };\n};'
        with self.assertRaises(ValueError):
            gpu.cells(gpu.block(text, 'parent'), 'reg')

    def test_parent_properties_survive_nested_children(self):
        text = '\nreg = <0x1000 0x100>;\n child {\n nested {\n reg = <1 2>;\n };\n };'
        self.assertEqual(gpu.cells(text, 'reg'), [4096, 256])

    def test_duplicate_node_rejected(self):
        with self.assertRaises(ValueError):
            gpu.block('\ngpu {\n};\ngpu {\n};', 'gpu')

    def test_unterminated_node_rejected(self):
        with self.assertRaises(ValueError):
            gpu.block('\ngpu {\n', 'gpu')

    def test_duplicate_property_rejected(self):
        with self.assertRaises(ValueError):
            gpu.cells('\nreg = <1 2>;\nreg = <3 4>;', 'reg')

    def test_unresolved_and_wide_cells_rejected(self):
        for value in ('<&gpu 1>', '<0x100000000>', '<(1 << 2)>', '<-1>', '<>'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                gpu.cells('\nreg = ' + value + ';', 'reg')

    def test_register_pair_validation(self):
        for reg, names in [('<1>', '"a"'), ('<1 2 3 4>', '"a", "a"'),
                           ('<1 0>', '"a"'), ('<0xfffffff0 0x20>', '"a"')]:
            with self.subTest(reg=reg), self.assertRaises(ValueError):
                gpu.windows('\nreg = ' + reg + ';\nreg-names = ' + names + ';')

    def test_named_registers_do_not_depend_on_order(self):
        self.assertEqual(gpu.windows('\nreg = <4096 256 8192 512>;\nreg-names = "cx", "gpu";'),
                         {'cx': {'base': 4096, 'size': 256}, 'gpu': {'base': 8192, 'size': 512}})

    def test_pin_inventory(self):
        pins = json.loads((ROOT/'hardware/sm8650-gpu.json').read_text())
        self.assertEqual({p['name'] for p in pins['firmware']}, set(gpu.FIRMWARE))
        self.assertEqual(len(pins['firmware']), 4)
        self.assertEqual(pins['board']['chip_id'], gpu.CHIP_ID)
        self.assertEqual(pins['gmem_bytes'], 3*1024*1024)
        self.assertNotEqual(pins['board']['gmu_windows']['gmu']['base'], 0x3d69000)
        self.assertEqual(pins['board']['stream_ids']['secure'], 2)
        for entry in pins['firmware'] + pins['vendor_sources']:
            self.assertRegex(entry['sha256'], r'^[0-9a-f]{64}$')


if __name__ == '__main__':
    unittest.main()
