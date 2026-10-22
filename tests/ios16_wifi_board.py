"""Check power requests against the captured board's regulator constraints."""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--dts',type=Path,required=True)
a=p.parse_args()
data=a.dts.read_bytes()
record=json.loads((ROOT/'hardware/sm8650-wifi.json').read_text())
assert hashlib.sha256(data).hexdigest()==record['android_device_tree']['decoded_sha256']
text=data.decode()
code=(ROOT/'uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe/WifiPcie.c').read_text()
def initializer(name):
    return re.search(r'\b'+name+r'\[GW_RAILS\]=\{([^}]+)\}',code)[1]
names=re.findall(r'"([^"]+)"',initializer('mRailNames'))
volts=[int(x) for x in initializer('mMillivolts').split(',')]
limits=[int(x) for x in initializer('mMaximumMv').split(',')]
assert len(names)==len(volts)==len(limits)==9 and len(set(names))==9
for name,mv,maximum in zip(names,volts,limits):
    match=re.search(r'\brpmh-regulator-'+re.escape(name)+r'\s*\{',text)
    assert match,'Missing captured regulator '+name
    at=match.end();depth=1;end=at
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    block=text[at:end]
    low=int(re.search(r'regulator-min-microvolt\s*=\s*<(0x[0-9a-f]+)>',block)[1],16)
    high=int(re.search(r'regulator-max-microvolt\s*=\s*<(0x[0-9a-f]+)>',block)[1],16)
    assert low<=mv*1000<=maximum*1000<=high,(name,mv,maximum,low,high)
print('Wi-Fi power: all 9 requested voltages and ceilings match the pinned Ace 5 DT')
