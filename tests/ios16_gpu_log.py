"""Assert that the actual common log formatter preserves diagnostic evidence."""
from pathlib import Path
import sys

lines=Path(sys.argv[1]).read_text().splitlines()
assert any('HOST FIXTURE:' in line and 'not hardware evidence' in line for line in lines)
events=[]
for line in lines:
    if not line.startswith('GIULIA GPU EVENT: '):continue
    row=dict(part.split('=',1) for part in line.split(': ',1)[1].split())
    assert set(row)=={'seq','subsystem','phase','op','address_space','address','expected','observed',
                      'mask','code','code_name','native_code','cleanup','cause'}
    assert int(row['code']) and row['cause']=='unconfirmed'
    assert row['code_name']!='unknown'
    events.append(row)

def one(**criteria):
    found=[r for r in events if all(r.get(k)==v for k,v in criteria.items())]
    assert len(found)==1,(criteria,found)
    return found[0]

tcr=one(subsystem='smmu',code_name='readback-mismatch')
assert int(tcr['address'],0)==0x03dc2030 and tcr['address_space']=='physical'
assert int(tcr['expected'],0)^int(tcr['observed'],0)==1
assert int(tcr['mask'],0)==0xffffffff and tcr['cleanup']=='0' and tcr['native_code']=='0'
for phase,address,expected,mask in [('firmware-ready(4)',0x03d7e070,0x100,0x1ff),
                                    ('hfi-ready(6)',0x03d7e600,1,1)]:
    row=one(subsystem='gmu',phase=phase,code_name='timeout')
    assert int(row['address'],0)==address and int(row['expected'],0)==expected
    assert int(row['observed'],0)==0 and int(row['mask'],0)==mask and row['cleanup']=='0'
cleanups=[r for r in events if r['subsystem']=='gmu' and r['cleanup']=='1']
assert len(cleanups)==2
assert all(r['code_name']=='io' and r['native_code']=='89' and
           r['observed']=='unavailable' and int(r['address'],0)==0x03d7e000 for r in cleanups)
power=one(subsystem='power',cleanup='1')
assert power['native_code']=='91' and power['code_name']=='release-failed'
assert power['address']=='unavailable' and power['phase']=='hub-clock(8)'
print('Diagnostic log contract passed: physical addresses, expected/observed/masks, native codes and separate cleanup failures')
