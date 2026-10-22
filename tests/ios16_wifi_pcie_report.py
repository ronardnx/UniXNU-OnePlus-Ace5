import sys
import base64
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from ios16_wifi_pcie import decode_reports, BINARY_FIELDS, BINARY_FORMAT, BINARY_V4_FIELDS, BINARY_V4_FORMAT

success=b'10,0,0,0,7f,fff,0,20110000,400,110717cb,1000a,0,0,100017cb'
line=b'boot-args = giulia_wifi='+success+b'\n'
r=decode_reports(line+line)
assert len(r)==1 and r[0]['endpoint_identified'] and not r[0]['scan_supported']
extended=decode_reports(b'giulia_wifi='+success+b',0,3,20000\n')[0]
assert extended['gpio_after']==3 and extended['pipe_source']==0x20000
v2=decode_reports(b'giulia_wifi2='+success+b',0,3,20000,2010003,0,0,1\n')[0]
assert v2['report_version']==2 and v2['pin_readback']['perst']=={'config':0x201,'io':3}
def binary_token(values):
    return base64.b64encode(BINARY_FORMAT.pack(*(values.get(k,0) for k in BINARY_FIELDS)))
v3_values=dict(v2,version=3,elbi=0x11400,port_link_before=0x30080,
               port_link_after=0x10020,link_cap=0x13,gen2_ctrl=0x2100,clkreq=0x70000,
               ltssm_seen_lo=0x20007,ltssm_seen_hi=1,ltssm_changes=4)
v3_token=binary_token(v3_values)
v3=decode_reports(b'giulia_wifi3='+v3_token+b'\n')[0]
assert v3['report_version']==3 and v3['endpoint_identified'] and v3['core_x1_configured']
assert v3['ltssm_states_seen']==[0,1,2,17,32] and v3['ltssm_changes']==4
assert v3['pin_readback']['clkreq']=={'config':7,'io':0}
x2=decode_reports(b'giulia_wifi3='+binary_token(dict(v3_values,
    port_link_after=0x30020,link_cap=0x23,gen2_ctrl=0x2200)))[0]
assert x2['core_x2_configured'] and not x2['core_x1_configured'] and x2['endpoint_identified']
def v4_token(changes):
    values=dict(v3_values,version=4,pipe_hz=125000000,pipe_measure_status=1)
    values.update(changes)
    return base64.b64encode(BINARY_V4_FORMAT.pack(*(values.get(k,0) for k in BINARY_V4_FIELDS)))
v4=decode_reports(b'giulia_wifi4='+v4_token({}))[0]
assert v4['pipe_frequency_measured'] and v4['pipe_hz']==125000000
v5=decode_reports(b'giulia_wifi5='+v4_token({'version':5,'rails':511}))[0]
assert v5['endpoint_identified'] and v5['rails']==511
try:decode_reports(b'giulia_wifi5='+v4_token({'version':5}))
except ValueError:pass
else:raise AssertionError('Success without host PHY rail votes accepted')
for changes in ({'pipe_measure_status':2},{'pipe_measure_status':7}):
    try:decode_reports(b'giulia_wifi4='+v4_token(changes))
    except ValueError:pass
    else:raise AssertionError('Invalid measured-frequency claim accepted')
bad_v3=[binary_token(dict(v3_values,**change)) for change in
        ({'port_link_after':0x10000},{'link_cap':0x23},{'gen2_ctrl':0x2200},
         {'ltssm_seen_lo':7},{'command':4},{'version':2})]
for token in (*bad_v3,v3_token[:-1],b'!'*216):
    try:decode_reports(b'giulia_wifi3='+token+b'\n')
    except ValueError:pass
    else:raise AssertionError('Invalid v3 configuration/trace accepted')
assert decode_reports(b'ordinary boot log')==[]
bad=(success.replace(b'110717cb',b'ffffffff'),success.replace(b'7f,fff',b'7f,0'),
     success.replace(b'20110000',b'ffffffff'),success.replace(b'1000a,0,0',b'1000a,1,0'),
     b'1,0',success+b',0')
for token in bad:
    try:decode_reports(b'giulia_wifi='+token+b'\n')
    except ValueError:pass
    else:raise AssertionError('Invalid success report accepted')
failure=decode_reports(b'giulia_wifi=9,3,1,8000000000000006,7f,7,0,0,0,0,1000a,8000000000000006,7f,0\n')[0]
assert not failure['endpoint_identified'] and failure['resource']=='gcc_pcie_0_cfg_ahb_clk'
assert len(failure['retained_rail_names'])==7
print('Wi-Fi PCIe reports: v1-v5, replay, truncation, lane/clock/host-power contradictions passed')
