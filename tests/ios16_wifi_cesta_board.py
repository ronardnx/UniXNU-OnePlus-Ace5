"""Bind CESTA instruction data and MMIO ranges to the captured Ace 5 board."""
import argparse
import hashlib
import re
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--dts',type=Path,required=True)
a=p.parse_args()
data=a.dts.read_bytes()
if hashlib.sha256(data).hexdigest()!='a23d858906208fcd7f9567aa6d6d7847e9e03fd5da448dd8e3b9a298a81de0a1':
    raise ValueError('Unverified captured board')
text=data.decode()
pcie=text.split('qcom,pcie@1c00000 {',1)[1].split('pcie0_rp {',1)[0]
crm=text.split('crm@1d01000 {',1)[1].split('};',1)[0]
driver=ROOT/'uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe'
header=(driver/'WifiPcieCesta.h').read_text()
def cells(node,key):
    m=re.search(re.escape(key)+r'\s*=\s*<([^>]+)>;',node)
    if not m:raise ValueError('Missing board property: '+key)
    return [int(v,0) for v in m[1].split()]
def table(name):
    m=re.search(r'\b'+name+r'\[\d+\]\s*=\s*\{([^}]+)\}',header,re.S)
    return [int(v.strip(),0) for v in m[1].split(',')]
assert cells(pcie,'qcom,pcie-sm-seq')==table('gce_code')
assert cells(pcie,'qcom,pcie-sm-branch-seq')==table('gce_branch')
assert cells(pcie,'qcom,pcie-sm-branch-offset')==[0x1000]
assert cells(pcie,'qcom,pcie-sm-start-offset')==[0x1090]
assert cells(pcie,'reg')[-2:]==[0x1d07000,0x7000]
assert cells(crm,'reg')==[0x1d01000,0x3000,0x1d04800,0x800,0x1d05000,0x2000,0x1d04000,0x800]
assert cells(crm,'qcom,hw-drv-ids')==[0,1]
assert cells(crm,'qcom,sw-drv-ids')==[0]
stock=(driver/'WifiPcieStockConfig.h').read_text()
def setting(name):
    return int(re.search(r'#define '+name+r'\s+(0x[0-9a-f]+|[0-9]+)u',stock)[1],0)
assert setting('GW_STOCK_D0_PERF')==len(cells(pcie,'qcom,bw-scale'))//3+1==4
assert setting('GW_STOCK_EP_DELAY_US')==cells(pcie,'qcom,ep-latency')[0]*1000
for absent in ('qcom,target-link-speed','qcom,target-link-width','qcom,core-preset',
               'qcom,eq-pset-req-vec','qcom,eq-fmdc-t-min-phase23','qcom,no-l1-supported',
               'qcom,perst-delay-us-min','qcom,perst-delay-us-max','qcom,tlp-rd-size',
               'qcom,pcie-halt-feature-dis'):
    assert absent not in pcie,absent
source=(driver/'WifiPcie.c').read_text()
for pair in ('{0x1d01000,0x3000}','{0x1d05000,0x2000}','{0x1d07000,0x7000}'):
    assert pair in source
print('CESTA board: 13 instruction words, 7 branch words, instance 0 and all MMIO ranges match the pinned Ace 5 DT')
print('Stock PCIe profile: max-speed D0 level, endpoint delay and absent DT overrides verified')
