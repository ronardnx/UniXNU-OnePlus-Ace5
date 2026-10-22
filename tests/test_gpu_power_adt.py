import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from ios16_gpu_power import PROPERTY, add_request, check_request, placeholder
from ios16_platform import read_nodes


def node(name, properties=(), children=()):
    values = [('name', name.encode() + b'\0'), *properties]
    records = b''.join(struct.pack('<32sI', key.encode(), len(value)) + value +
                       bytes(-len(value) % 4) for key, value in values)
    return struct.pack('<II', len(values), len(children)) + records + b''.join(children)


class GPUPowerADT(unittest.TestCase):
    def fixture(self, properties=(), extra=()):
        return node('device-tree', [('physical-model', b'OnePlus Ace 5\0')],
                    [node('chosen', [('dram-base', bytes(8)), *properties],
                          [node('memory-map', [('TrustCache', bytes(16))])]), *extra])

    def test_only_chosen_request_changes(self):
        original = self.fixture()
        out = add_request(original)
        before, after = read_nodes(original), read_nodes(out)
        self.assertEqual(after['/device-tree/chosen'].pop(PROPERTY), placeholder())
        self.assertEqual(before, after)
        self.assertEqual(len(out) - len(original), 36 + 256)
        self.assertFalse(check_request(out, 3)['acceleration'])

    def test_request_requires_explicit_mode_and_new_consumer(self):
        out = add_request(self.fixture())
        for mode in (None, 1, 2):
            with self.assertRaises(ValueError):
                check_request(out, mode)
        with self.assertRaises(ValueError):
            check_request(out, 3, b'old GPU kernel')
        check_request(out, 3, b'GIULIA GPU: UEFI power handoff')

    def test_corruption_and_old_result_are_never_reused(self):
        for at in (0, 4, 8, 12, 16, 255):
            request = bytearray(placeholder()); request[at] ^= 1
            out = self.fixture([(PROPERTY, bytes(request))])
            with self.subTest(at=at), self.assertRaises(ValueError):
                check_request(out, 3)
            with self.assertRaises(ValueError):
                add_request(out)

    def test_shadowed_and_duplicate_requests_rejected(self):
        out = self.fixture(extra=[node('other', [(PROPERTY, placeholder())])])
        with self.assertRaises(ValueError):
            add_request(out)
        with self.assertRaises(ValueError):
            check_request(out, 3)
        out = self.fixture([(PROPERTY, placeholder()), (PROPERTY, placeholder())])
        with self.assertRaises(ValueError):
            check_request(out, 3)

    def test_foreign_identity_missing_chosen_and_bad_bounds(self):
        for blob in (node('device-tree', [('physical-model', b'foreign\0')], [node('chosen')]),
                     node('device-tree', [('physical-model', b'OnePlus Ace 5\0')]),
                     self.fixture()[:-1], self.fixture() + b'garbage'):
            with self.assertRaises(ValueError):
                add_request(blob)


if __name__ == '__main__':
    unittest.main()
