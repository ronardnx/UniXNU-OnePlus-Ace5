import importlib.util
from pathlib import Path
import struct
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('entry_probe',ROOT/'scripts/patch-xnu-entry-probe.py')
p=importlib.util.module_from_spec(spec);spec.loader.exec_module(p)

class ProbePadding(unittest.TestCase):
    def setUp(self):
        p.ENTRY_VA=0x1000
        p.SEGMENTS[:]=[(0x1000,4096,0,4096)]
    def test_zeros_and_architectural_nops(self):
        data=bytes(128)+struct.pack('<I',0xd503201f)*992
        self.assertTrue(p.is_padding(data,0x1040,256))
    def test_instructions_and_page_boundary_rejected(self):
        data=bytearray(4096);struct.pack_into('<I',data,128,0x14000000)
        self.assertFalse(p.is_padding(data,0x1080,96))
        self.assertFalse(p.is_padding(data,0x1000,96))
        self.assertFalse(p.is_padding(data,0x1fe0,96))

if __name__=='__main__':unittest.main()
