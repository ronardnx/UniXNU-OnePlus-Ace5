import sys,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from gpu_terminal_native import NativeHfi
class NativeProtocol(unittest.TestCase):
 def fixture(self):
  n=NativeHfi.__new__(NativeHfi);n.report={};n.allocations={};n.writes=[];n.sent=[]
  n.allocate=lambda size,align,flags,kind:dict(handle=0,offset=0x400000,bytes=size,flags=flags,kind=kind,gpu=0x61400000,gmu=0x61400000)
  n.dev=lambda *args:n.writes.append(args)
  n.read=lambda *args:[0]
  n.packet=lambda *args:n.sent.append(args)
  return n
 def test_recorded_scratch_request(self):
  n=self.fixture();words=[0x700b14,1,0,0,0x33f,2,0,2,0,0x1000,0]
  n.request(words)
  self.assertEqual(n.sent,[(20,[words[0],0x61400000,0,0x33f,2,2,2,0x61400000,0x1000,0],1)])
  self.assertEqual(n.writes,[(4,0x400000,4096)])
 def test_unknown_mmio_and_secure_no_success_reply(self):
  n=self.fixture()
  with self.assertRaises(RuntimeError):n.request([0x700b14,1,0,0,0x338,19,0,2,0,0x1000,0])
  self.assertFalse(n.sent)
 def test_alignment_refusal_and_counter(self):
  n=self.fixture()
  with self.assertRaises(RuntimeError):n.request([0x700b14,1,0,0,0x33f,2,0,2,0,0x1000,23])
  n.request([0x50036e,3,7]);self.assertEqual(n.sent,[(110,[0x50036e,3,7,0,0],1)])
if __name__=='__main__':unittest.main()
