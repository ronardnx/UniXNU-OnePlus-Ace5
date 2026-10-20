"""Download pinned Apple OS components locally, with bounded range-cache RAM.

Does not mount or flash images. Uses the upstream extractor and SHA256 pins.
"""
import argparse
import json
from pathlib import Path
import sys

p = argparse.ArgumentParser()
p.add_argument('--unixnu',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
args = p.parse_args()
root = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(args.unixnu.resolve()/'scripts'))
from unixnu import ipsw
from unixnu.cli import parser
from unixnu.config import Context

class BoundedRangeFile(ipsw.RangeFile):
    def _block(self,index):
        data = super()._block(index)
        self.cache.pop(index,None)
        self.cache[index] = data
        while len(self.cache) > 8:
            self.cache.pop(next(iter(self.cache)))
        return data

ipsw.RangeFile = BoundedRangeFile
ctx = Context(parser().parse_args(['ipsw','info','-D',str(root),'--out',str(args.out.resolve())]))
source = ipsw.Ipsw(ctx)
records = []
for component in ['OS','Cryptex1,SystemOS','StaticTrustCache','Cryptex1,SystemTrustCache']:
    path = source.extract(component)
    records.append(json.loads(path.with_suffix(path.suffix+'.json').read_text()))
    print(f'Completed and hash-checked: {component}',flush=True)
args.out.mkdir(parents=True,exist_ok=True)
(args.out/'system-inputs.json').write_text(json.dumps(records,indent=2)+'\n')
