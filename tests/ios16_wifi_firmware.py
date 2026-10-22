"""Original-asset integrity and exact hardware/region selection checks."""
import argparse
import copy
import json
from pathlib import Path
import struct
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from ios16_wifi_firmware import load_bundle,select_files,elf_info,safe_name

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--bundle',type=Path,required=True)
a=p.parse_args()
catalog=load_bundle(a.bundle)
identity=dict(soc_hw_version=0x12000201,board_id=255,chip_id=2,project=23851,region='CN')
v2=select_files(catalog,**identity)
assert v2['main']=='kiwi/amss20.bin' and v2['board']=='kiwi/bdwlan.b0c'
v1=select_files(catalog,**dict(identity,soc_hw_version=0x12000101))
assert v1['main']=='kiwi/amss.bin' and v1['phy']=='kiwi/phy_ucode.elf'
assert select_files(catalog,**dict(identity,board_id=1))['board']=='kiwi/bdwlan.e01'
cases=3
for change in ({'soc_hw_version':0},{'soc_hw_version':0xffffffff},
               {'soc_hw_version':0x12000301},{'chip_id':0x12},
               {'project':23867},{'region':'EU'},{'board_id':0x1234},
               {'board_id':65536},{'board_id':-1},{'soc_hw_version':True}):
    try:select_files(catalog,**dict(identity,**change))
    except ValueError:cases+=1
    else:raise AssertionError('Unmatched hardware selected '+repr(change))
for name in ('../kiwi/amss.bin','kiwi/../amss.bin','/kiwi/amss.bin','kiwi\\amss.bin','C:/amss.bin'):
    try:safe_name(name)
    except ValueError:cases+=1
    else:raise AssertionError('Unsafe path accepted')
data=(a.bundle/v2['main']).read_bytes()
for change in ('truncate','table','segment'):
    bad=bytearray(data)
    if change=='truncate':bad=bad[:30]
    elif change=='table':struct.pack_into('<I',bad,28,0xffffff00)
    else:
        phoff=struct.unpack_from('<I',bad,28)[0]
        struct.pack_into('<I',bad,phoff+4,len(bad)-1)
        struct.pack_into('<I',bad,phoff+16,4096)
    try:elf_info(bad)
    except ValueError:cases+=1
    else:raise AssertionError('Malformed firmware ELF accepted')
with tempfile.TemporaryDirectory() as folder:
    target=Path(folder)
    entry=next(e for e in catalog['files'] if e['path']==v2['main'])
    one=copy.deepcopy(catalog);one['files']=[entry]
    (target/'manifest.json').write_text(json.dumps(one))
    file=target/entry['path'];file.parent.mkdir(parents=True);file.write_bytes(data)
    load_bundle(target)
    damaged=bytearray(data);damaged[-1]^=1;file.write_bytes(damaged)
    try:load_bundle(target)
    except ValueError:cases+=1
    else:raise AssertionError('Firmware byte corruption accepted')
print(f'Wi-Fi firmware: {cases} exact-selection, missing-identity, path and ELF bounds cases passed')
