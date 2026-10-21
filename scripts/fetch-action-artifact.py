"""Download a bounded Actions artifact using resumable, validated HTTPS ranges."""
import argparse
import concurrent.futures
import json
from pathlib import Path
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self,*args,**kwargs): return None

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--artifact',type=int,required=True)
    p.add_argument('--out',type=Path,required=True)
    a=p.parse_args()
    base=f'https://api.github.com/repos/tycykp/UniXNU-giulia/actions/artifacts/{a.artifact}'
    token=subprocess.check_output(['gh','auth','token'],text=True).strip()
    headers={'Authorization':'Bearer '+token,'Accept':'application/vnd.github+json'}
    with urllib.request.urlopen(urllib.request.Request(base,headers=headers),timeout=30) as r:
        metadata=json.load(r)
    size=metadata['size_in_bytes']
    if metadata['expired'] or size>128*1024**2: raise ValueError('Expired or oversized artifact')
    try:
        urllib.request.build_opener(NoRedirect()).open(urllib.request.Request(base+'/zip',headers=headers),timeout=30)
        raise ValueError('Missing artifact redirect')
    except urllib.error.HTTPError as e:
        if e.code!=302: raise ValueError('Artifact redirect failed') from None
        url=e.headers['Location']
    del token,headers
    if urllib.parse.urlsplit(url).scheme!='https': raise ValueError('Non-HTTPS artifact')
    if a.out.exists(): raise ValueError('Output exists')
    parts=a.out.with_suffix('.parts');parts.mkdir(parents=True,exist_ok=True)
    chunk=512*1024
    def fetch(i):
        start=i*chunk;end=min(size,start+chunk)-1;dest=parts/str(i)
        if dest.exists() and dest.stat().st_size==end-start+1:return
        for attempt in range(4):
            try:
                req=urllib.request.Request(url,headers={'Range':f'bytes={start}-{end}'})
                with urllib.request.urlopen(req,timeout=45) as r:
                    if r.status!=206 or r.headers.get('Content-Range')!=f'bytes {start}-{end}/{size}':
                        raise ValueError('Range mismatch')
                    b=r.read(end-start+2)
                if len(b)!=end-start+1: raise ValueError('Range length mismatch')
                dest.write_bytes(b);print(f'Artifact {a.artifact}: part {i+1}',flush=True);return
            except Exception:
                if attempt==3: raise RuntimeError(f'Artifact part {i} failed') from None
                time.sleep(1)
    n=(size+chunk-1)//chunk
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:list(pool.map(fetch,range(n)))
    with a.out.open('xb') as f:
        for i in range(n):f.write((parts/str(i)).read_bytes())
    with zipfile.ZipFile(a.out) as z:
        if z.testzip() is not None:raise ValueError('ZIP CRC mismatch')
    a.out.with_suffix('.metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
    print(f'Artifact {a.artifact}: ZIP CRC verified, {size} bytes')

if __name__=='__main__':
    try:main()
    except Exception as e:
        # URL-bearing network exception details must not expose signed URLs.
        raise SystemExit('Artifact download failed: '+type(e).__name__) from None
