import sys,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from gpu_terminal_native import NativeHfi,start_hwsched
class NativeProtocol(unittest.TestCase):
 def test_minimal_cp_init_removes_restore_bit_and_ordinals(self):
  from gpu_terminal_cp_secure import init_commands
  self.assertEqual(init_commands(0x61400000),[0x70970001,0x08000000,0x70c80007,0x94b,3,0x20000000,2,0x61400000,0,0x80000000])
  self.assertEqual(init_commands(0x61400000,True),[0x70970001,0x08000000,0x70c80004,0x84b,3,0x20000000,2])
 def test_host_raw_dispatch_records_queue_and_doorbell_separately(self):
  import tempfile
  from types import SimpleNamespace
  for fail in (False,True):
   with tempfile.TemporaryDirectory() as folder:
    n=NativeHfi.__new__(NativeHfi);n.sequence=0x900;n.report={};n.platform_enabled=True;n.host_raw_dispatch=True
    events=[]
    def dev(op,*args):
     events.append(op)
     if op==6 and fail:raise RuntimeError('publication not confirmed')
    n.dev=dev
    n.l=SimpleNamespace(capture=SimpleNamespace(root=Path(folder)),write=lambda *a,**k:events.append('doorbell'))
    if fail:
     with self.assertRaisesRegex(RuntimeError,'publication'):n.packet(131,[1,2])
     self.assertNotIn('doorbell',events)
     self.assertIsNone(n.report['raw_cp_submitted'])
    else:
     n.packet(131,[1,2])
     self.assertEqual(events,[10,5,5,5,6,'doorbell'])
     self.assertTrue(n.report['raw_submission_attempts'][0]['doorbell_confirmed'])
    self.assertNotIn(13,events)
 def test_aqe_negotiated_between_core_start_and_start(self):
  class T:
   def __init__(self):self.calls=[]
   def send(self,msg,payload):
    self.calls.append((msg,payload))
    return dict(id=msg,value=1 if msg==12 else 0,status=None)
  t=T();r={};start_hwsched(t,r)
  self.assertEqual(t.calls,[(14,[0]),(12,[121,0]),(11,[2,0,0]),(11,[29,1,0]),(10,[])])
  self.assertTrue(r['aqe_feature_enabled'] and r['hwsched_start_accepted'])
 def test_rejected_aqe_does_not_start(self):
  class T:
   def __init__(self):self.calls=[]
   def send(self,msg,payload):
    self.calls.append(msg)
    return dict(id=msg,value=0xffffffff if payload==[29,1,0] else 0,status=8 if payload==[29,1,0] else 0)
  t=T();r={}
  with self.assertRaisesRegex(RuntimeError,'rejected'):start_hwsched(t,r)
  self.assertEqual(t.calls,[14,12,11,11])
  self.assertNotIn('hwsched_start_accepted',r)
  self.assertNotIn('aqe_feature_enabled',r)
 def fixture(self):
  n=NativeHfi.__new__(NativeHfi);n.report={};n.allocations={};n.writes=[];n.sent=[]
  n.allocate=lambda size,align,flags,kind:dict(handle=0,offset=0x400000,bytes=(size+4095)&~4095,flags=flags,kind=kind,gpu=0x61400000,gmu=0x61400000)
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
  with self.assertRaisesRegex(RuntimeError,'Secure GPU'):
   n.request([0xf00b14,1,0,0,0x1007,5,0,5,0,0x37d000,0])
  self.assertFalse(n.sent)
  self.assertFalse(n.writes)
  self.assertEqual(n.report['blocked_memory_request']['bytes'],0x37d000)
 def test_subpage_reply_keeps_requested_size_and_gmu_only_address(self):
  n=self.fixture();words=[0xd00b14,1,0,0,0x17f,3,0,3,0,24,0]
  n.request(words)
  self.assertEqual(n.writes,[(4,0x400000,4096)])
  self.assertEqual(n.sent[0][1][-2:], [24,0])
  n=self.fixture();n.request([0xb00b14,1,0,0,0x178,16,0,16,0,0x8000,0])
  self.assertEqual(n.sent[0][1][1:3],[0,0])
  self.assertEqual(n.sent[0][1][7],0x61400000)
 def test_secure_reply_only_after_owned_mapping_without_cpu_access(self):
  n=self.fixture();n.platform_enabled=True
  n.secure_allocate=lambda *args:dict(handle=0,bytes=0x37d000,gpu=0xf0000000,gmu=0,secure=True)
  n.request([0xf00b14,1,0,0,0x1007,5,0,5,0,0x37d000,0])
  self.assertEqual(n.sent,[(20,[0xf00b14,0xf0000000,0,0x1007,5,5,5,0,0x37d000,0],1)])
  self.assertFalse(n.writes)
  n=self.fixture();n.platform_enabled=True
  def rejected(*args):raise RuntimeError('ownership unknown')
  n.secure_allocate=rejected
  with self.assertRaisesRegex(RuntimeError,'ownership unknown'):
   n.request([0xf00b14,1,0,0,0x1007,5,0,5,0,0x37d000,0])
  self.assertFalse(n.sent)
 def test_pending_scm_stops_further_host_io(self):
  import re
  n=self.fixture();n.token=0
  class L:
   def command(self,line,pattern,**kw):
    return re.search(pattern,'GIULIA GPU SCM: return a0=0x2 a1=0 a2=0x42 a3=0 a6=0\nGIULIA GPU DEV: done token=1 op=11 code=8')
  n.l=L();n.dev=NativeHfi.dev.__get__(n)
  with self.assertRaises(RuntimeError):n.dev(11,4)
  self.assertTrue(n.l.io_stopped)
  self.assertTrue(n.report['scm_pending_quarantined'])
 def test_allocator_accepts_absent_master_address(self):
  n=self.fixture()
  n.dev=lambda *args:'GIULIA GPU DEV: allocation=3 offset=0x400000 bytes=0x8000 flags=0x178 kind=16 gpu=0 gmu=0x61400000\n'
  value=NativeHfi.allocate(n,0x8000,4096,0x178,16)
  self.assertEqual(value['gpu'],0)
  self.assertEqual(value['gmu'],0x61400000)
 def test_raw_error_stops_automatic_followup_io(self):
  import re
  n=self.fixture();n.token=0
  class L:
   def command(self,line,pattern,**kw):
    return re.search(pattern,'GIULIA GPU WATCH: outcome=timeout quarantined=1\nGIULIA GPU DEV: done token=1 op=13 code=8')
  n.l=L();n.dev=NativeHfi.dev.__get__(n)
  with self.assertRaises(RuntimeError):n.dev(13,11,1000)
  self.assertTrue(n.l.io_stopped)
  self.assertTrue(n.report['raw_watch_quarantined'])
 def test_ack_poll_does_not_touch_mmio(self):
  import re
  n=self.fixture();n.pending=[]
  class L:
   def command(self,*a,**k):return re.search('poll queue=2 code=1','GIULIA GPU HFI: poll queue=1 code=1\nGIULIA GPU HFI: poll queue=2 code=1')
   def write(self,*a,**k):raise AssertionError('Unsolicited MMIO while waiting for GPU')
  n.l=L();n.poll();self.assertFalse(n.pending)
 def test_alignment_refusal_and_counter(self):
  n=self.fixture()
  with self.assertRaises(RuntimeError):n.request([0x700b14,1,0,0,0x33f,2,0,2,0,0x1000,23])
  n.request([0x50036e,3,7]);self.assertEqual(n.sent,[(110,[0x50036e,3,7,0,0],1)])
 def test_repeated_request_reuses_mapping_without_zeroing(self):
  n=self.fixture();calls=[];allocate=n.allocate
  n.allocate=lambda *args:(calls.append(args) or allocate(*args))
  first=[0x700b14,1,0,0,0x33f,2,0,2,0,0x1000,0]
  n.request(first);n.writes.clear()
  again=first.copy();again[0]=0x800b14;n.request(again)
  self.assertEqual(len(calls),1);self.assertFalse(n.writes)
  self.assertEqual(n.sent[-1][1][0],again[0])
  self.assertTrue(n.report['memory_requests'][-1]['reused'])
  self.assertIn((2,2),n.allocations)
  conflict=again.copy();conflict[-2]=8192
  with self.assertRaisesRegex(RuntimeError,'Conflicting'):n.request(conflict)
  self.assertEqual(len(calls),1);self.assertEqual(len(n.sent),2)
 def test_same_handle_different_kind_gets_distinct_allocation(self):
  n=self.fixture()
  for kind in (2,3):n.request([0x700b14,1,0,0,0x33f,kind,0,2,0,0x1000,0])
  self.assertEqual(set(n.allocations),{(2,2),(3,2)})
  self.assertEqual(len(n.writes),2)
 def test_repeated_secure_request_never_reassigns_or_touches_cpu(self):
  n=self.fixture();n.platform_enabled=True;calls=[]
  def secure(*args):
   calls.append(args);return dict(handle=0,bytes=4096,gpu=0xf0000000,gmu=0,secure=True)
  n.secure_allocate=secure
  request=[0xf00b14,1,0,0,0x1007,5,0,5,0,4096,0]
  n.request(request);n.request(request)
  self.assertEqual(len(calls),1);self.assertFalse(n.writes);self.assertEqual(len(n.sent),2)
 def test_all_special_resources_refuse_ram_substitution(self):
  for kind in (11,12,13,20,22,23):
   n=self.fixture();n.allocate=lambda *args:self.fail('Allocated substitute RAM')
   with self.assertRaises(RuntimeError):n.request([0x700b14,1,0,0,0x338,kind,0,2,0,4096,0])
   self.assertFalse(n.sent);self.assertFalse(n.writes)
 def ipc_fixture(self):
  n=self.fixture()
  n.info='GIULIA GPU DEV: ipc_core=1 ipc_host_arbitration=1 ipc_retained_until_reboot=1\n'
  n.l=type('Live',(),{'io_stopped':False})()
  def dev(op,*args):
   n.writes.append((op,*args))
   if op!=19:self.fail('IPC_CORE used generic memory operation '+str(op))
   flags=args[2];reused=int(len(n.writes)>1)
   return f'GIULIA GPU IPC: bytes=0x140000 gpu=0 gmu=0x67000000 flags={hex(flags)} kind=19 reused={reused} cpu_offset=none mapped=1 host_excluded=1\n'
  n.dev=dev
  n.allocate=lambda *args:self.fail('IPC_CORE used ordinary RAM allocation')
  n.read=lambda *args:self.fail('IPC_CORE read through CPU pool offset')
  n.write=lambda *args:self.fail('IPC_CORE written through CPU pool offset')
  n.physical=lambda *args:self.fail('IPC_CORE exposed CPU pool address')
  return n
 def test_ipc_request_is_gmu_only_and_preserves_logical_size(self):
  n=self.ipc_fixture();words=[0x700b14,1,0,0,0x38,19,0,7,0,24,0x1216]
  n.request(words)
  self.assertEqual(n.writes,[(19,24,0x1216,0x38,19)])
  self.assertEqual(n.sent,[(20,[words[0],0,0,0x38,19,7,7,0x67000000,24,0x1216],1)])
  allocation=n.allocations[(19,7)]
  self.assertEqual(allocation['bytes'],0x140000);self.assertNotIn('offset',allocation)
  self.assertIsNone(allocation['cpu_offset'])
  self.assertTrue(allocation['ipc_core'] and allocation['mapped'] and allocation['host_excluded'])
  self.assertEqual(n.report['ipc_allocations'],[dict(allocation,gmu_handle=7)])
 def test_ipc_all_accepted_permissions_have_no_host_or_gpu_access(self):
  for flags in (8,0x18,0x28,0x38,0x48,0x58,0x68,0x78):
   n=self.ipc_fixture();n.request([0x700b14,1,0,0,flags,19,0,7,0,0x140000,0])
   self.assertEqual(n.writes,[(19,0x140000,0,flags,19)])
   self.assertEqual(n.sent[0][1][1:3],[0,0]);self.assertEqual(n.sent[0][1][7],0x67000000)
 def test_ipc_repeated_and_different_handles_use_fixed_singleton(self):
  n=self.ipc_fixture();words=[0x700b14,1,0,0,0x38,19,0,7,0,4096,0]
  n.request(words);again=words.copy();again[0]=0x800b14;n.request(again)
  self.assertEqual(n.writes,[(19,4096,0,0x38,19)])
  self.assertEqual(n.sent[-1][1][0],again[0])
  self.assertTrue(n.report['memory_requests'][-1]['reused'])
  another=again.copy();another[7]=8;another[9]=0x140000;n.request(another)
  self.assertEqual(n.writes,[(19,4096,0,0x38,19),(19,0x140000,0,0x38,19)])
  self.assertEqual(n.allocations[(19,7)]['gmu'],n.allocations[(19,8)]['gmu'])
  self.assertTrue(n.allocations[(19,8)]['reused'])
  for index,value in ((4,8),(9,8192),(10,12)):
   changed=words.copy();changed[index]=value
   with self.assertRaisesRegex(RuntimeError,'Conflicting repeated'):n.request(changed)
  changed=another.copy();changed[7]=9;changed[4]=8
  with self.assertRaisesRegex(RuntimeError,'singleton IPC_CORE permissions'):n.request(changed)
  self.assertEqual(len(n.writes),2);self.assertEqual(len(n.sent),3)
 def test_ipc_requires_explicit_backend_host_arbitration_and_retention(self):
  good='GIULIA GPU DEV: ipc_core=1 ipc_host_arbitration=1 ipc_retained_until_reboot=1\n'
  for info in ('','GIULIA GPU DEV: resources_abi=1 memstore=1\n',
      'GIULIA GPU DEV: ipc_core=1\n',good+good,good.replace('ipc_core=1','ipc_core=0'),
      good.replace('ipc_host_arbitration=1','ipc_host_arbitration=0'),
      good.replace('ipc_retained_until_reboot=1','ipc_retained_until_reboot=0')):
   n=self.ipc_fixture();n.info=info
   with self.assertRaisesRegex(RuntimeError,'resource ABI and host arbitration'):
    n.request([0x700b14,1,0,0,0x38,19,0,7,0,4096,0])
   self.assertFalse(n.writes);self.assertFalse(n.sent);self.assertFalse(n.allocations)
   self.assertFalse(n.l.io_stopped)
 def test_ipc_invalid_descriptors_never_reach_kernel(self):
  base=(4096,0,0x38,19)
  for index,value in ((0,0),(0,0x140001),(0,-1),(0,True),(0,1.0),(0,0x100000000),
      (1,23),(1,0x1300),(1,0x10000),(1,0x100000000),(2,0),(2,0x30),(2,0x39),
      (2,0x3a),(2,0x3c),(2,0xb8),(2,0x138),(2,0x238),(2,0x438),(2,0x1038),(3,20)):
   n=self.ipc_fixture();args=list(base);args[index]=value
   with self.assertRaisesRegex(RuntimeError,'Invalid IPC_CORE'):n.ipc_allocate(*args)
   self.assertFalse(n.writes);self.assertFalse(n.l.io_stopped)
 def test_ipc_rejects_bad_mapping_records_and_stops_io(self):
  good='GIULIA GPU IPC: bytes=0x140000 gpu=0 gmu=0x67000000 flags=0x38 kind=19 reused=0 cpu_offset=none mapped=1 host_excluded=1\n'
  bad=[good.replace(before,after) for before,after in (
   ('bytes=0x140000','bytes=0x1000'),('gpu=0','gpu=0x67000000'),
   ('gmu=0x67000000','gmu=0x67001000'),('flags=0x38','flags=0x8'),('kind=19','kind=2'),
   ('reused=0','reused=2'),('cpu_offset=none','cpu_offset=0'),('mapped=1','mapped=0'),
   ('host_excluded=1','host_excluded=0'))]
  bad.extend(('',good+good,good.rstrip()+' extra=1\n'))
  for reply in bad:
   n=self.ipc_fixture();n.dev=lambda *args:reply
   with self.assertRaises(RuntimeError):n.request([0x700b14,1,0,0,0x38,19,0,7,0,4096,0])
   self.assertFalse(n.sent);self.assertFalse(n.allocations);self.assertFalse(hasattr(n,'ipc_resource'))
   self.assertTrue(n.l.io_stopped);self.assertTrue(n.report['mapping_completion_unconfirmed'])
 def test_ipc_existing_resource_requires_reuse_confirmation(self):
  n=self.ipc_fixture();n.ipc_allocate(4096,0,0x38)
  n.dev=lambda *args:'GIULIA GPU IPC: bytes=0x140000 gpu=0 gmu=0x67000000 flags=0x38 kind=19 reused=0 cpu_offset=none mapped=1 host_excluded=1\n'
  with self.assertRaisesRegex(RuntimeError,'dedicated mapping'):n.ipc_allocate(8192,0,0x38)
  self.assertTrue(n.l.io_stopped);self.assertTrue(n.report['mapping_completion_unconfirmed'])
 def test_ipc_transport_retains_records_and_uses_operation_19(self):
  import re
  n=self.ipc_fixture();n.token=0
  record='GIULIA GPU IPC: bytes=0x140000 gpu=0 gmu=0x67000000 flags=0x38 kind=19 reused=0 cpu_offset=none mapped=1 host_excluded=1'
  class L:
   def command(self,line,pattern,**kw):
    n.writes.append(line)
    return re.search(pattern,'unrelated output\n'+record+'\nGIULIA GPU DEV: done token=1 op=19 code=0')
  n.l=L();n.dev=NativeHfi.dev.__get__(n)
  self.assertEqual(n.ipc_allocate(24,0x1216,0x38)['gmu'],0x67000000)
  self.assertEqual(n.writes,['gpu.dev 0x1 0x13 0x18 0x1216 0x38 0x13'])
 def test_ipc_incomplete_publication_stops_host_io(self):
  import re
  n=self.ipc_fixture();n.token=0
  class L:
   def command(self,line,pattern,**kw):return re.search(pattern,'GIULIA GPU DEV: done token=1 op=19 code=8')
  n.l=L();n.dev=NativeHfi.dev.__get__(n)
  with self.assertRaises(RuntimeError):n.ipc_allocate(4096,0,0x38)
  self.assertTrue(n.l.io_stopped);self.assertTrue(n.report['mapping_completion_unconfirmed'])
 def fence_fixture(self):
  n=self.fixture()
  n.info='GIULIA GPU DEV: fence_import_abi=1 fence_completion=0\n'
  n.l=type('Live',(),{'io_stopped':False})()
  n.fence_bytes=0x1a000;n.fence_epoch=0x123456789abcdef0
  def dev(op,*args):
   n.writes.append((op,*args))
   if op!=20:self.fail('Hardware fence used generic memory operation '+str(op))
   flags=args[2];reused=int(len(n.writes)>1)
   return f'GIULIA GPU FENCE: bytes={hex(n.fence_bytes)} gpu=0 gmu=0x68000000 flags={hex(flags)} kind=23 epoch={hex(n.fence_epoch)} reused={reused} cpu_offset=none mapped=1 completion_verified=0\n'
  n.dev=dev
  n.allocate=lambda *args:self.fail('Hardware fence used ordinary RAM allocation')
  n.read=lambda *args:self.fail('Hardware fence read through CPU pool offset')
  n.write=lambda *args:self.fail('Hardware fence written through CPU pool offset')
  n.physical=lambda *args:self.fail('Hardware fence exposed CPU pool address')
  return n
 def test_fence_request_imports_provider_without_cpu_access_or_completion(self):
  for size in (0x1a000,0x1c000,0x20000):
   n=self.fence_fixture();n.fence_bytes=size
   words=[0x700b14,1,0,0,0x138,23,0,7,0,24,0x1016]
   n.request(words)
   self.assertEqual(n.writes,[(20,24,0x1016,0x138,23)])
   self.assertEqual(n.sent,[(20,[words[0],0,0,0x138,23,7,7,0x68000000,24,0x1016],1)])
   allocation=n.allocations[(23,7)]
   self.assertEqual(allocation['bytes'],size);self.assertNotIn('offset',allocation)
   self.assertIsNone(allocation['cpu_offset']);self.assertEqual(allocation['epoch'],0x123456789abcdef0)
   self.assertTrue(allocation['hardware_fence'] and allocation['mapped'])
   self.assertFalse(allocation['completion_verified']);self.assertFalse(n.l.io_stopped)
   self.assertEqual(n.report['fence_allocations'],[dict(allocation,gmu_handle=7)])
 def test_fence_all_supported_flags_keep_gmu_only_mapping(self):
  for flags in range(0x200):
   n=self.fence_fixture()
   if flags&~0x178 or not flags&8:
    with self.assertRaisesRegex(RuntimeError,'Invalid hardware fence'):n.fence_allocate(4096,0,flags)
    self.assertFalse(n.writes)
   else:
    n.request([0x700b14,1,0,0,flags,23,0,7,0,4096,0])
    self.assertEqual(n.writes,[(20,4096,0,flags,23)])
    self.assertEqual(n.sent[0][1][1:3],[0,0]);self.assertEqual(n.sent[0][1][7],0x68000000)
 def test_fence_requires_exact_import_abi_without_claiming_completion(self):
  good='GIULIA GPU DEV: fence_import_abi=1 fence_completion=0\n'
  for info in ('','GIULIA GPU DEV: resources_abi=1 memstore=1\n',
      'GIULIA GPU DEV: fence_import_abi=1\n',good+good,
      good.replace('fence_import_abi=1','fence_import_abi=2'),
      good.replace('fence_completion=0','fence_completion=1'),good.rstrip()+' provider_ready=1\n'):
   n=self.fence_fixture();n.info=info
   with self.assertRaisesRegex(RuntimeError,'import ABI'):
    n.request([0x700b14,1,0,0,0x138,23,0,7,0,4096,0])
   self.assertFalse(n.writes);self.assertFalse(n.sent);self.assertFalse(n.allocations)
   self.assertFalse(n.l.io_stopped)
 def test_fence_invalid_descriptors_never_reach_kernel(self):
  base=(4096,0,0x138,23)
  for index,value in ((0,0),(0,-1),(0,True),(0,1.0),(0,0x100000000),
      (1,23),(1,0x1700),(1,0x10000),(1,0x100000000),(1,True),(2,0),(2,0x130),
      (2,0x139),(2,0x13a),(2,0x13c),(2,0x1b8),(2,0x338),(2,0x538),(2,0x1138),(3,22)):
   n=self.fence_fixture();args=list(base);args[index]=value
   with self.assertRaisesRegex(RuntimeError,'Invalid hardware fence'):n.fence_allocate(*args)
   self.assertFalse(n.writes);self.assertFalse(n.l.io_stopped)
 def test_fence_kernel_decides_actual_descriptor_capacity(self):
  import re
  for size,alignment in ((0xffffffff,0),(1,0x1616)):
   n=self.fence_fixture();n.token=0
   class L:
    io_stopped=False
    def command(self,line,pattern,**kw):
     n.writes.append(line)
     return re.search(pattern,'GIULIA GPU DEV: done token=1 op=20 code=1')
   n.l=L();n.dev=NativeHfi.dev.__get__(n)
   with self.assertRaisesRegex(RuntimeError,'command failed'):n.fence_allocate(size,alignment,0x138)
   self.assertEqual(n.writes,[f'gpu.dev 0x1 0x14 {hex(size)} {hex(alignment)} 0x138 0x17'])
   self.assertFalse(n.l.io_stopped);self.assertFalse(hasattr(n,'fence_resource'))
 def test_fence_same_key_always_revalidates_live_provider(self):
  n=self.fence_fixture();words=[0x700b14,1,0,0,0x138,23,0,7,0,4096,0]
  n.request(words);again=words.copy();again[0]=0x800b14;n.request(again)
  self.assertEqual(n.writes,[(20,4096,0,0x138,23)]*2)
  self.assertEqual(n.sent[-1][1][0],again[0]);self.assertTrue(n.allocations[(23,7)]['reused'])
  self.assertTrue(n.report['memory_requests'][-1]['reused'])
  for index,value in ((4,8),(9,8192),(10,12)):
   changed=again.copy();changed[index]=value
   with self.assertRaisesRegex(RuntimeError,'Conflicting repeated'):n.request(changed)
  self.assertEqual(len(n.writes),2);self.assertEqual(len(n.sent),2)
 def test_fence_new_handles_share_immutable_singleton(self):
  n=self.fence_fixture();words=[0x700b14,1,0,0,0x138,23,0,7,0,4096,0]
  n.request(words);another=words.copy();another[7]=8;another[9]=0x1a000;n.request(another)
  self.assertEqual(n.writes,[(20,4096,0,0x138,23),(20,0x1a000,0,0x138,23)])
  for field in ('epoch','bytes','flags','gmu'):
   self.assertEqual(n.allocations[(23,7)][field],n.allocations[(23,8)][field])
  self.assertTrue(n.allocations[(23,8)]['reused'])
  another[7]=9;another[4]=8
  with self.assertRaisesRegex(RuntimeError,'singleton hardware fence permissions'):n.request(another)
  self.assertEqual(len(n.writes),2);self.assertEqual(len(n.sent),2)
 def test_fence_cached_key_revalidates_even_at_allocation_limit(self):
  n=self.fence_fixture();words=[0x700b14,1,0,0,0x138,23,0,7,0,4096,0]
  n.request(words)
  for handle in range(63):n.allocations[(2,handle)]={}
  n.request(words)
  self.assertEqual(len(n.allocations),64);self.assertEqual(len(n.writes),2)
  words[7]=8
  with self.assertRaisesRegex(RuntimeError,'allocation limit'):n.request(words)
  self.assertEqual(len(n.writes),2);self.assertEqual(len(n.sent),2)
 def test_fence_no_provider_or_refusal_does_not_ack(self):
  import re
  for code in (1,7):
   n=self.fence_fixture();n.token=0
   class L:
    io_stopped=False
    def command(self,line,pattern,**kw):
     n.writes.append(line)
     return re.search(pattern,f'GIULIA GPU DEV: done token=1 op=20 code={code}')
   n.l=L();n.dev=NativeHfi.dev.__get__(n)
   with self.assertRaisesRegex(RuntimeError,'command failed'):
    n.request([0x700b14,1,0,0,0x138,23,0,7,0,4096,0])
   self.assertEqual(n.writes,['gpu.dev 0x1 0x14 0x1000 0x0 0x138 0x17'])
   self.assertFalse(n.sent);self.assertFalse(n.allocations);self.assertFalse(hasattr(n,'fence_resource'))
   self.assertFalse(n.l.io_stopped);self.assertNotIn('mapping_completion_unconfirmed',n.report)
 def test_fence_cached_key_cannot_hide_provider_loss(self):
  import re
  n=self.fence_fixture();words=[0x700b14,1,0,0,0x138,23,0,7,0,4096,0]
  n.request(words);before=dict(n.allocations[(23,7)]);n.token=0
  class L:
   io_stopped=False
   def command(self,line,pattern,**kw):return re.search(pattern,'GIULIA GPU DEV: done token=1 op=20 code=8')
  n.l=L();n.dev=NativeHfi.dev.__get__(n)
  with self.assertRaisesRegex(RuntimeError,'command failed'):n.request(words)
  self.assertEqual(len(n.sent),1);self.assertEqual(n.allocations[(23,7)],before)
  self.assertTrue(n.l.io_stopped);self.assertTrue(n.report['mapping_completion_unconfirmed'])
 def test_fence_rejects_bad_mapping_records_and_stops_io(self):
  good='GIULIA GPU FENCE: bytes=0x1a000 gpu=0 gmu=0x68000000 flags=0x138 kind=23 epoch=0x123456789abcdef0 reused=0 cpu_offset=none mapped=1 completion_verified=0\n'
  bad=[good.replace(before,after) for before,after in (
   ('bytes=0x1a000','bytes=0x1000'),('bytes=0x1a000','bytes=0x1b000'),
   ('gpu=0','gpu=0x68000000'),('gmu=0x68000000','gmu=0x68001000'),
   ('flags=0x138','flags=0x38'),('kind=23','kind=22'),
   ('epoch=0x123456789abcdef0','epoch=0x0'),('epoch=0x123456789abcdef0','epoch=0x10000000000000000'),
   ('reused=0','reused=2'),('cpu_offset=none','cpu_offset=0'),('mapped=1','mapped=0'),
   ('completion_verified=0','completion_verified=1'))]
  bad.extend(('',good+good,good.rstrip()+' extra=1\n'))
  for reply in bad:
   n=self.fence_fixture();n.dev=lambda *args:reply
   with self.assertRaises(RuntimeError):n.request([0x700b14,1,0,0,0x138,23,0,7,0,4096,0])
   self.assertFalse(n.sent);self.assertFalse(n.allocations);self.assertFalse(hasattr(n,'fence_resource'))
   self.assertTrue(n.l.io_stopped);self.assertTrue(n.report['mapping_completion_unconfirmed'])
  for size,align in ((0x1a001,0),(1,0x1200)):
   n=self.fence_fixture();n.dev=lambda *args:good
   with self.assertRaisesRegex(RuntimeError,'dedicated mapping'):n.fence_allocate(size,align,0x138)
   self.assertTrue(n.l.io_stopped);self.assertFalse(hasattr(n,'fence_resource'))
 def test_fence_epoch_size_and_reuse_cannot_change_on_reimport(self):
  for mutation in ('epoch','bytes','reused'):
   for same_handle in (False,True):
    n=self.fence_fixture();words=[0x700b14,1,0,0,0x138,23,0,7,0,4096,0]
    n.request(words);previous=dict(n.fence_resource)
    if mutation=='epoch':n.fence_epoch+=1
    elif mutation=='bytes':n.fence_bytes=0x1c000
    else:
     original=n.dev
     n.dev=lambda *args:original(*args).replace('reused=1','reused=0')
    if not same_handle:words[7]=8
    with self.assertRaisesRegex(RuntimeError,'provider epoch'):n.request(words)
    self.assertEqual(len(n.writes),2);self.assertEqual(len(n.sent),1)
    self.assertEqual(n.fence_resource,previous);self.assertEqual(set(n.allocations),{(23,7)})
    self.assertTrue(n.l.io_stopped);self.assertTrue(n.report['mapping_completion_unconfirmed'])
 def test_fence_transport_retains_records_and_uses_operation_20(self):
  import re
  n=self.fence_fixture();n.token=0
  record='GIULIA GPU FENCE: bytes=0x1c000 gpu=0 gmu=0x68000000 flags=0x138 kind=23 epoch=0xffffffffffffffff reused=1 cpu_offset=none mapped=1 completion_verified=0'
  class L:
   def command(self,line,pattern,**kw):
    n.writes.append(line)
    return re.search(pattern,'unrelated output\n'+record+'\nGIULIA GPU DEV: done token=1 op=20 code=0')
  n.l=L();n.dev=NativeHfi.dev.__get__(n)
  imported=n.fence_allocate(24,0x1016,0x138)
  self.assertEqual(imported['epoch'],0xffffffffffffffff);self.assertTrue(imported['reused'])
  self.assertEqual(n.writes,['gpu.dev 0x1 0x14 0x18 0x1016 0x138 0x17'])
 def test_invalid_memory_requests_never_allocate_or_ack(self):
  base=[0x700b14,1,0,0,0x33f,2,0,2,0,4096,0]
  for index,value in ((0,0x700a14),(0,0x710b14),(4,0x80000000),(5,26),(9,0),(9,0xffffffff),(10,23)):
   n=self.fixture();n.allocate=lambda *args:self.fail('Allocated malformed request')
   request=base.copy();request[index]=value
   with self.assertRaises(RuntimeError):n.request(request)
   self.assertFalse(n.sent);self.assertFalse(n.writes)
 def test_kernel_allocation_reply_must_match_mapping(self):
  n=self.fixture();n.report['per_allocation_mapping_enabled']=True
  fields=dict(offset='0x400000',bytes='0x1000',flags='0x178',kind='16',gpu='0',gmu='0x61400000')
  def reply(values):
   return 'GIULIA GPU DEV: allocation=3 '+' '.join(f'{k}={v}' for k,v in values.items())+'\n'
  n.dev=lambda *args:reply(fields)
  self.assertEqual(NativeHfi.allocate(n,4096,4096,0x178,16)['gmu'],0x61400000)
  for key,value in (('offset','0'),('bytes','0x2000'),('flags','0x33f'),('kind','2'),('gpu','0x61400000'),('gmu','0x61401000')):
   invalid=dict(fields);invalid[key]=value;n.dev=lambda *args:reply(invalid)
   with self.assertRaisesRegex(RuntimeError,'owned mapping'):NativeHfi.allocate(n,4096,4096,0x178,16)
 def memstore_fixture(self):
  n=self.fixture()
  n.info='GIULIA GPU DEV: resources_abi=1 memstore=1 secure_allocation_mappings=1\n'
  def dev(op,*args):
   n.writes.append((op,*args))
   if op==17:
    flags=args[2];reused=int(sum(call[0]==17 for call in n.writes)>1)
    return f'GIULIA GPU MEMSTORE: bytes=0x8000 gpu=0x66000000 gmu={hex(0x66000000) if flags&8 else "0"} flags={hex(flags)} kind=22 reused={reused} cpu_offset=none mapped=1\n'
   if op==18:return f'GIULIA GPU MEMSTORE: context={args[0]} sop=0xffffffff eop=0xffffffff stable=1 completion_verified=0\n'
   self.fail('Memstore used generic memory operation '+str(op))
  n.dev=dev
  n.allocate=lambda *args:self.fail('Memstore used ordinary RAM allocation')
  n.read=lambda *args:self.fail('Memstore read through generic pool offset')
  return n
 def test_memstore_request_preserves_logical_size_and_wire_alignment(self):
  n=self.memstore_fixture();words=[0x700b14,1,0,0,0x33f,22,0,7,0,24,0xf16]
  n.request(words)
  self.assertEqual(n.writes,[(17,24,0xf16,0x33f,22)])
  self.assertEqual(n.sent,[(20,[words[0],0x66000000,0,0x33f,22,7,7,0x66000000,24,0xf16],1)])
  a=n.allocations[(22,7)]
  self.assertEqual(a['bytes'],0x8000);self.assertNotIn('offset',a)
  self.assertIsNone(a['cpu_offset']);self.assertTrue(a['memstore'])
 def test_memstore_repeat_and_new_handles_share_singleton_without_clearing(self):
  n=self.memstore_fixture();words=[0x700b14,1,0,0,0x33f,22,0,7,0,4096,0]
  n.request(words);n.request(words)
  self.assertEqual(len(n.writes),1)
  again=words.copy();again[7]=8;again[-2]=32768;n.request(again)
  self.assertEqual(n.writes,[(17,4096,0,0x33f,22),(17,32768,0,0x33f,22)])
  self.assertEqual(n.allocations[(22,7)]['gpu'],n.allocations[(22,8)]['gpu'])
  self.assertTrue(n.report['memory_requests'][-1]['reused'])
  for index,value in ((4,7),(9,8192),(10,12)):
   changed=words.copy();changed[index]=value
   with self.assertRaisesRegex(RuntimeError,'Conflicting repeated'):n.request(changed)
  changed=again.copy();changed[7]=9;changed[4]=7
  with self.assertRaisesRegex(RuntimeError,'singleton memstore permissions'):n.request(changed)
  self.assertEqual(len(n.writes),2);self.assertEqual(len(n.sent),3)
 def test_memstore_gmu_master_is_independent(self):
  for flags in (7,0x337,0x30f,0x37f):
   n=self.memstore_fixture();n.request([0x700b14,1,0,0,flags,22,0,7,0,32768,0])
   self.assertEqual(n.sent[0][1][1],0x66000000)
   self.assertEqual(n.sent[0][1][7],0x66000000 if flags&8 else 0)
   self.assertEqual(n.allocations[(22,7)]['flags'],flags)
 def test_memstore_requires_explicit_kernel_resource_abi(self):
  for info in ('','GIULIA GPU DEV: abi=1 gmu_root=0x1000 gpu_root=0x2000\n',
    'GIULIA GPU DEV: resources_abi=2 memstore=1\n','GIULIA GPU DEV: resources_abi=1 memstore=0\n'):
   n=self.memstore_fixture();n.info=info
   with self.assertRaisesRegex(RuntimeError,'resource ABI'):n.memstore_allocate(4096,0,7)
   with self.assertRaisesRegex(RuntimeError,'resource ABI'):n.memstore_read(1)
   self.assertFalse(n.writes);self.assertFalse(n.sent)
 def test_memstore_invalid_descriptors_never_reach_kernel(self):
  base=(4096,0,0x33f,22)
  for index,value in ((0,0),(0,32769),(0,-1),(0,True),(1,23),(1,0x1000),(1,0x10000),
    (2,0x33e),(2,0x33d),(2,0x33b),(2,0x3bf),(2,0x133f),(2,0x73f),(3,23)):
   n=self.memstore_fixture();args=list(base);args[index]=value
   with self.assertRaisesRegex(RuntimeError,'Invalid memstore'):n.memstore_allocate(*args)
   self.assertFalse(n.writes)
 def test_memstore_rejects_bad_mapping_records_without_ack(self):
  good='GIULIA GPU MEMSTORE: bytes=0x8000 gpu=0x66000000 gmu=0x66000000 flags=0x33f kind=22 reused=0 cpu_offset=none mapped=1\n'
  bad=[good.replace(before,after) for before,after in (
   ('bytes=0x8000','bytes=0x1000'),('gpu=0x66000000','gpu=0x61000000'),
   ('gmu=0x66000000','gmu=0'),('flags=0x33f','flags=0x307'),('kind=22','kind=2'),
   ('reused=0','reused=2'),('cpu_offset=none','cpu_offset=0'),('mapped=1','mapped=0'))]
  bad.extend(('',good+good,good.rstrip()+' extra=1\n'))
  for reply in bad:
   n=self.memstore_fixture();n.dev=lambda *args:reply
   with self.assertRaises(RuntimeError):n.request([0x700b14,1,0,0,0x33f,22,0,7,0,4096,0])
   self.assertFalse(n.sent);self.assertFalse(n.allocations)
   self.assertFalse(hasattr(n,'memstore_resource'))
 def test_memstore_existing_resource_requires_reuse_confirmation(self):
  n=self.memstore_fixture();n.memstore_allocate(4096,0,0x33f)
  n.dev=lambda *args:'GIULIA GPU MEMSTORE: bytes=0x8000 gpu=0x66000000 gmu=0x66000000 flags=0x33f kind=22 reused=0 cpu_offset=none mapped=1\n'
  with self.assertRaisesRegex(RuntimeError,'dedicated mapping'):n.memstore_allocate(8192,0,0x33f)
 def test_memstore_observation_never_verifies_completion(self):
  for context in (0,1,812):
   n=self.memstore_fixture();observed=n.memstore_read(context)
   self.assertEqual(n.writes,[(18,context,0,0,0)])
   self.assertEqual(observed,dict(context=context,sop=0xffffffff,eop=0xffffffff,stable=True,completion_verified=False))
   self.assertEqual(n.report['memstore_observations'],[observed])
  n=self.memstore_fixture()
  n.dev=lambda *args:'GIULIA GPU MEMSTORE: context=1 sop=0x8 eop=0x7 stable=0 completion_verified=0\n'
  observed=n.memstore_read(1)
  self.assertFalse(observed['stable']);self.assertFalse(observed['completion_verified'])
 def test_memstore_rejects_invalid_context_without_read(self):
  for context in (-1,813,819,0x100000000,True,1.0):
   n=self.memstore_fixture()
   with self.assertRaisesRegex(RuntimeError,'Invalid memstore context'):n.memstore_read(context)
   self.assertFalse(n.writes)
 def test_memstore_rejects_invalid_timestamp_observations(self):
  good='GIULIA GPU MEMSTORE: context=1 sop=0x8 eop=0x7 stable=1 completion_verified=0\n'
  bad=[good.replace(before,after) for before,after in (
   ('context=1','context=2'),('sop=0x8','sop=0x100000000'),('eop=0x7','eop=0x100000000'),
   ('stable=1','stable=2'),('completion_verified=0','completion_verified=1'))]
  for reply in (*bad,'',good+good):
   n=self.memstore_fixture();n.dev=lambda *args:reply
   with self.assertRaises(RuntimeError):n.memstore_read(1)
   self.assertNotIn('memstore_observations',n.report)
 def test_memstore_transport_retains_resource_records(self):
  import re
  n=self.fixture();n.token=0
  record='GIULIA GPU MEMSTORE: context=1 sop=0 eop=0 stable=1 completion_verified=0'
  class L:
   def command(self,line,pattern,**kw):return re.search(pattern,record+'\nGIULIA GPU DEV: done token=1 op=18 code=0')
  n.l=L();n.dev=NativeHfi.dev.__get__(n)
  self.assertIn(record,n.dev(18,1))
 def test_memstore_incomplete_mapping_stops_host_io(self):
  import re
  n=self.fixture();n.token=0
  class L:
   def command(self,line,pattern,**kw):return re.search(pattern,'GIULIA GPU DEV: done token=1 op=17 code=8')
  n.l=L();n.dev=NativeHfi.dev.__get__(n)
  with self.assertRaises(RuntimeError):n.dev(17,4096,0,7,22)
  self.assertTrue(n.l.io_stopped);self.assertTrue(n.report['mapping_completion_unconfirmed'])
 def test_secure_pte_publication_failure_stops_host_cleanup(self):
  import re
  n=self.fixture();n.token=0
  class L:
   def command(self,line,pattern,**kw):return re.search(pattern,'GIULIA GPU DEV: done token=1 op=12 code=8')
  n.l=L();n.dev=NativeHfi.dev.__get__(n)
  with self.assertRaises(RuntimeError):n.dev(12,65537,4096,0x1007,5)
  self.assertTrue(n.l.io_stopped);self.assertTrue(n.report['mapping_completion_unconfirmed'])
 def test_secure_reply_binds_full_size_permissions_and_slot(self):
  n=self.fixture()
  valid='GIULIA GPU SECURE: allocation=2 bytes=0x1001 gpu=0xf0800000 gmu=0 flags=0x1007 kind=5 owned=1 mapped=1 cpu_access=0\n'
  n.dev=lambda *args:valid
  self.assertEqual(NativeHfi.secure_allocate(n,4097,4096,0x1007,5)['gpu'],0xf0800000)
  for before,after in (('allocation=2','allocation=8'),('bytes=0x1001','bytes=0x2000'),
       ('gpu=0xf0800000','gpu=0xf0801000'),('flags=0x1007','flags=0x1003'),('kind=5','kind=2')):
   n.dev=lambda *args:valid.replace(before,after)
   with self.assertRaisesRegex(RuntimeError,'owned slot'):NativeHfi.secure_allocate(n,4097,4096,0x1007,5)
  n.dev=lambda *args:valid+valid
  with self.assertRaises(RuntimeError):NativeHfi.secure_allocate(n,4097,4096,0x1007,5)
 def test_incomplete_success_record_stops_device_io(self):
  for operation in ('normal','secure','memstore'):
   n=self.fixture();n.l=type('Live',(),{'io_stopped':False})()
   n.info='GIULIA GPU DEV: resources_abi=1 memstore=1 secure_allocation_mappings=1\n'
   n.dev=lambda *args:'GIULIA GPU DEV: done token=1 code=0\n'
   with self.assertRaises(RuntimeError):
    if operation=='normal':NativeHfi.allocate(n,4096,4096,0x33f,2)
    elif operation=='secure':NativeHfi.secure_allocate(n,4096,4096,0x1007,5)
    else:NativeHfi.memstore_allocate(n,4096,0,0x33f)
   self.assertTrue(n.l.io_stopped);self.assertTrue(n.report['mapping_completion_unconfirmed'])
 def test_any_kernel_state_failure_stops_raw_followup(self):
  import re
  for op in (1,3,4,5,6,7,10,11,12,13,14,16,17,19,20,21):
   n=self.fixture();n.token=0
   class L:
    def command(self,line,pattern,**kw):
     return re.search(pattern,f'GIULIA GPU DEV: done token=1 op={op} code=8')
   n.l=L();n.dev=NativeHfi.dev.__get__(n)
   with self.assertRaises(RuntimeError):n.dev(op)
   self.assertTrue(n.l.io_stopped);self.assertTrue(n.report['kernel_state_unconfirmed'])
if __name__=='__main__':unittest.main()
