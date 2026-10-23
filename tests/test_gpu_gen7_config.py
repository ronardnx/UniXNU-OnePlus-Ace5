"""Offline register-serialization and failure-boundary checks; no phone access."""
import sys,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from gpu_terminal_gen7_config import Gen7Config
class Memory:
 def __init__(self):self.values={};self.writes=[]
 def read(self,a):return self.values.get(a,0)
 def write(self,a,v,verify=True):
  assert 0<=v<=0xffffffff
  assert 0x3d00000<=a<0x3d40000 or 0x3d6c000<=a<0x3d98000
  if a==0x3d03834:v&=~4095
  self.values[a]=v;self.writes.append((a,v))
class Pool:
 def __init__(self):self.words={};self.allocs=0
 def allocate(self,*args):self.allocs+=1;return dict(offset=0x400000,gpu=0x61400000,bytes=4096)
 def dev(self,*args):pass
 def write(self,a,words):self.words.update({a+4*i:v for i,v in enumerate(words)})
 def read(self,a,n):return [self.words.get(a+4*i,0) for i in range(n)]
class ConfigTests(unittest.TestCase):
 def fixture(self):
  l,t,r=Memory(),Pool(),{};return l,t,r,Gen7Config(l,t,r)
 def test_exact_powerup_header_and_protect_span(self):
  l,t,r,c=self.fixture();l.values[0x3d00000+4*c.word('GEN7_RBBM_CGC_P2S_STATUS')]=1;c.configure()
  self.assertEqual(t.read(0x400000,4),[0,0,0,0x113a])
  self.assertEqual(r['powerup_list']['words'],154)
  self.assertEqual(c.rd('GEN7_CP_PROTECT_REG'),0x13fc0000)
  self.assertEqual(c.rd('GEN7_CP_PROTECT_REG+47'),0x80027c02)
  self.assertEqual(c.rd('GEN7_RBBM_INT_0_MASK'),0)
  self.assertEqual(c.rd('GEN7_RBBM_INTERFACE_HANG_INT_CNTL'),0x40cfffff)
 def test_missing_clock_ack_does_not_publish_powerup_list(self):
  l,t,r,c=self.fixture()
  with self.assertRaisesRegex(RuntimeError,'handshake'):c.configure()
  self.assertEqual(t.allocs,0)
  self.assertNotIn('gen7_nonsecure_registers_verified',r)
if __name__=='__main__':unittest.main()
