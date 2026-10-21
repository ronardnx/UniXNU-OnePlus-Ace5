"""Download a bounded Actions artifact using resumable, validated HTTPS ranges."""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import subprocess
import time
import threading
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
    p.add_argument('--workers',type=int,default=8)
    a=p.parse_args()
    if not 1<=a.workers<=64: p.error('Workers must be between 1 and 64')
    base=f'https://api.github.com/repos/tycykp/UniXNU-giulia/actions/artifacts/{a.artifact}'
    token=subprocess.check_output(['gh','auth','token'],text=True).strip()
    headers={'Authorization':'Bearer '+token,'Accept':'application/vnd.github+json'}
    with urllib.request.urlopen(urllib.request.Request(base,headers=headers),timeout=30) as r:
        metadata=json.load(r)
    size=metadata['size_in_bytes']
    if metadata['expired'] or size>24*1024**3: raise ValueError('Expired or oversized artifact')
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
    identity={'artifact':a.artifact,'bytes':size,'digest':metadata.get('digest')}
    identity_path=parts/'artifact.json'
    if identity_path.exists():
        if json.loads(identity_path.read_text())!=identity: raise ValueError('Resume identity mismatch')
    elif any(parts.iterdir()):
        raise ValueError('Resume directory lacks artifact identity')
    else: identity_path.write_text(json.dumps(identity))
    chunk=4*1024**2 if size>128*1024**2 else 512*1024
    lock=threading.Lock(); completed=0; last_report=0
    def progress(length):
        nonlocal completed,last_report
        with lock:
            completed+=length
            if completed-last_report>=64*1024**2 or completed==size:
                print(f'Artifact {a.artifact}: {completed}/{size} bytes',flush=True)
                last_report=completed
    def fetch(i):
        start=i*chunk;end=min(size,start+chunk)-1;dest=parts/str(i)
        if dest.exists() and dest.stat().st_size==end-start+1:
            progress(end-start+1);return
        for attempt in range(4):
            try:
                req=urllib.request.Request(url,headers={'Range':f'bytes={start}-{end}'})
                with urllib.request.urlopen(req,timeout=90) as r:
                    if r.status!=206 or r.headers.get('Content-Range')!=f'bytes {start}-{end}/{size}':
                        raise ValueError('Range mismatch')
                    b=r.read(end-start+2)
                if len(b)!=end-start+1: raise ValueError('Range length mismatch')
                dest.write_bytes(b);progress(len(b));return
            except Exception:
                if attempt==3: raise RuntimeError(f'Artifact part {i} failed') from None
                time.sleep(1)
    n=(size+chunk-1)//chunk
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.workers) as pool:list(pool.map(fetch,range(n)))
    with a.out.open('xb') as f:
        for i in range(n):f.write((parts/str(i)).read_bytes())
    if metadata.get('digest','').startswith('sha256:'):
        with a.out.open('rb') as f:actual='sha256:'+hashlib.file_digest(f,'sha256').hexdigest()
        if actual!=metadata['digest']: raise ValueError('GitHub artifact SHA256 mismatch')
    with zipfile.ZipFile(a.out) as z:
        if z.testzip() is not None:raise ValueError('ZIP CRC mismatch')
    a.out.with_suffix('.metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
    print(f'Artifact {a.artifact}: ZIP CRC verified, {size} bytes')

if __name__=='__main__':
    try:main()
    except Exception as e:
        # URL-bearing network exception details must not expose signed URLs.
        raise SystemExit('Artifact download failed: '+type(e).__name__) from None
