import copy
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('firmware_cache',ROOT/'scripts/firmware-cache.py')
cache=importlib.util.module_from_spec(spec)
spec.loader.exec_module(cache)

class FirmwareCache(unittest.TestCase):
    def board(self):
        return {'ipsw':{'build':'21H461','version':'17.7.11','product':'iPad7,1','sha256':{'os.dmg':'abc'}},
                'kernel':{'prelink':{'kernelcache_sha256':hashlib.sha256(b'kernel').hexdigest()}},
                'userdata':{'first_lba':1}}

    def test_hardware_edits_keep_cache_firmware_changes_invalidate(self):
        a=self.board(); b=copy.deepcopy(a)
        b['userdata']['first_lba']=99; b['firmware']={'defines':{'DEBUG':1}}
        for group in ['os','kernel']:
            self.assertEqual(cache.cache_key(a,group),cache.cache_key(b,group))
            b['ipsw']['build']='different'
            self.assertNotEqual(cache.cache_key(a,group),cache.cache_key(b,group))
            b['ipsw']['build']=a['ipsw']['build']

    def test_kernel_pin_and_corruption(self):
        a=self.board(); b=copy.deepcopy(a)
        b['kernel']['prelink']['kernelcache_sha256']='changed'
        self.assertNotEqual(cache.cache_key(a,'kernel'),cache.cache_key(b,'kernel'))
        self.assertEqual(cache.cache_key(a,'os'),cache.cache_key(b,'os'))
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); path=root/'21H461/kernelcache.decompressed'
            self.assertFalse(cache.valid_kernel(a,root))
            path.parent.mkdir(); path.write_bytes(b'kernel')
            self.assertTrue(cache.valid_kernel(a,root))
            path.write_bytes(b'corrupt')
            self.assertFalse(cache.valid_kernel(a,root))

if __name__=='__main__': unittest.main()
