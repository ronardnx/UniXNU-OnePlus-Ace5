"""Run the complete offline Wi-Fi bring-up checks in one Linux/WSL command."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--dts',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--runtime',action='store_true',help='Also check the complete kernel transport and full-panel renderer')
a=p.parse_args()
checks=[]
def check(args):
    result=subprocess.run([str(x) for x in args],cwd=ROOT,check=True,capture_output=True,text=True)
    print(result.stdout,end='')
    if result.stderr:print(result.stderr,file=sys.stderr,end='')
    checks.append({'command':[str(x) for x in args],'result':'passed','output':result.stdout.strip()})
with tempfile.TemporaryDirectory(prefix='giulia-wifi-') as directory:
    for name in ('ios16_wifi_pcie','ios16_wifi_power_cache'):
        binary=Path(directory)/name
        check(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
               ROOT/'tests'/f'{name}.c','-o',binary])
        check([binary])
    check([sys.executable,ROOT/'tests/ios16_wifi_pcie_report.py'])
    check([sys.executable,ROOT/'tests/ios16_wifi_handoff.py'])
    check([sys.executable,ROOT/'tests/ios16_wifi_board.py','--dts',a.dts.resolve()])
    if a.runtime:
        check(['bash',ROOT/'scripts/test-ios16-wifi-runtime.sh'])
        check([sys.executable,ROOT/'tests/ios16_wifi_embed.py'])
a.out.parent.mkdir(parents=True,exist_ok=True)
with a.out.open('x') as f:
    json.dump({'status':'offline-checks-passed','hardware_verified':False,'checks':checks},f,indent=2)
    f.write('\n')
