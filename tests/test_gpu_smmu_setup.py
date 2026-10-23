"""Check PRR physical-address handling and refusal before global writes."""
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from gpu_terminal_smmu import prepare_prr, PRR_LOW, PRR_HIGH


class SetupTests(unittest.TestCase):
    def fixture(self, active_bank=None, physical=0x8ca010000, dirty=False):
        class Live:
            def read(self, address):
                return int(active_bank is not None and address == 0x3dc0000+active_bank*4096)
        class Pool:
            allocations = 0
            def allocate(self,*args):
                self.allocations += 1
                return dict(offset=0x400000, gpu=0x61400000, bytes=4096)
            def dev(self,*args): pass
            def read(self,offset,count): return [1 if dirty else 0]*count
            def physical(self,offset): return physical
        report={}; writes=[];pool=Pool()
        return Live(),pool,report,lambda a,v:writes.append((a,v)),writes

    def test_physical_halves_not_gpu_virtual_or_shifted(self):
        live,pool,report,write,writes=self.fixture()
        prepare_prr(live,pool,report,write)
        self.assertEqual(writes,[(PRR_LOW,0xca010000),(PRR_HIGH,8)])
        self.assertTrue(report['prr_marker']['prepared'])

    def test_any_active_context_refuses_before_allocating(self):
        for bank in (0,1,2,21):
            live,pool,report,write,writes=self.fixture(active_bank=bank)
            with self.assertRaisesRegex(RuntimeError,'disabled'):prepare_prr(live,pool,report,write)
            self.assertEqual(pool.allocations,0)
            self.assertEqual(writes,[])

    def test_bad_page_or_address_never_programs_marker(self):
        for options in ({'dirty':True},{'physical':0},{'physical':0x8ca010001},{'physical':1<<36}):
            live,pool,report,write,writes=self.fixture(**options)
            with self.assertRaises(RuntimeError):prepare_prr(live,pool,report,write)
            self.assertEqual(writes,[])

    def test_partial_write_is_not_reported_prepared(self):
        live,pool,report,_,writes=self.fixture()
        def fail_high(address,value):
            writes.append((address,value))
            if address==PRR_HIGH:raise RuntimeError('readback failure')
        with self.assertRaisesRegex(RuntimeError,'readback'):prepare_prr(live,pool,report,fail_high)
        self.assertFalse(report['prr_marker']['prepared'])


if __name__=='__main__':unittest.main()
