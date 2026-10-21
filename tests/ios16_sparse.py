import hashlib
from pathlib import Path
import runpy
import struct
import subprocess
import tempfile

verify=runpy.run_path(str(Path(__file__).resolve().parents[1]/'scripts/prepare-ios16-userdata.py'))['sparse_digest']
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory);raw=root/'raw.img';sparse=root/'sparse.img'
    data=bytes(range(256))*32+bytes(8192)+b'ABCD'*2048
    raw.write_bytes(data)
    subprocess.run(['img2simg',str(raw),str(sparse),'4096'],check=True)
    assert verify(sparse)==(len(data),hashlib.sha256(data).hexdigest())
    blob=sparse.read_bytes()
    for name,bad in [('truncated',blob[:-1]),('trailing',blob+b'bad')]:
        path=root/name;path.write_bytes(bad)
        try:verify(path)
        except ValueError:pass
        else:raise AssertionError(name)
    # Exercise a don't-care extent with a defined zero-expanded digest.
    skip=struct.pack('<IHHHHIIII',0xed26ff3a,1,0,28,12,4096,2,1,0)+struct.pack('<HHII',0xcac3,0,2,12)
    sparse.write_bytes(skip)
    assert verify(sparse)==(8192,hashlib.sha256(bytes(8192)).hexdigest())
print('Android sparse round-trip, skip, truncation and trailing-byte checks passed')
