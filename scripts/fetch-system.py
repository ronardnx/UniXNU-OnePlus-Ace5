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
p.add_argument('--components',nargs='+',default=['OS','Cryptex1,SystemOS','StaticTrustCache','Cryptex1,SystemTrustCache'])
args = p.parse_args()
root = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(args.unixnu.resolve()/'scripts'))
from unixnu import ipsw
from unixnu.cli import parser
from unixnu.config import Context

class BoundedRangeFile(ipsw.RangeFile):
    def __init__(self,url):
        super().__init__(url,block=1<<20)
        self.fetched=0

    def _block(self,index):
        missed=index not in self.cache
        data = super()._block(index)
        if missed:
            self.fetched+=len(data)
            if self.fetched//(32<<20)!=(self.fetched-len(data))//(32<<20):
                print(f'Fetched {self.fetched//(1<<20)} MiB from Apple CDN',flush=True)
        self.cache.pop(index,None)
        self.cache[index] = data
        while len(self.cache) > 8:
            self.cache.pop(next(iter(self.cache)))
        return data

ipsw.RangeFile = BoundedRangeFile
ctx = Context(parser().parse_args(['ipsw','info','-D',str(root),'--out',str(args.out.resolve())]))
source = ipsw.Ipsw(ctx)
records = []
for component in args.components:
    path = source.extract(component)
    records.append(json.loads(path.with_suffix(path.suffix+'.json').read_text()))
    print(f'Completed and hash-checked: {component}',flush=True)
args.out.mkdir(parents=True,exist_ok=True)
(args.out/'system-inputs.json').write_text(json.dumps(records,indent=2)+'\n')
