import plistlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from test_adt import decode, ROOT

class RootMatching(unittest.TestCase):
    def test_volume_uuid_selects_iomedia(self):
        volume='04769d26-2ca0-4825-b443-e2e5f88300f2'
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'root.adt'
            subprocess.run([sys.executable,str(ROOT/'scripts/make-giulia-adt.py'),
                '--output',str(path),'--dram-size','0x10000000',
                '--root-match-volume-uuid',volume],check=True)
            chosen=decode(path.read_bytes())['/device-tree/chosen']
        match=plistlib.loads(b'<plist version="1.0">'+chosen['root-matching'].rstrip(b'\0')+b'</plist>')
        self.assertEqual(match,{'IOProviderClass':'IOMedia',
                               'IOPropertyMatch':{'UUID':volume.upper()}})

    def test_invalid_identity_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'invalid.adt'
            result=subprocess.run([sys.executable,str(ROOT/'scripts/make-giulia-adt.py'),
                '--output',str(path),'--dram-size','0x10000000',
                '--root-match-volume-uuid','</string>'],capture_output=True)
            self.assertNotEqual(result.returncode,0)
            self.assertFalse(path.exists())
