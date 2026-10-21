"""Exercise loader pointer conversion and rejection of malformed chains."""
import contextlib
import io
from pathlib import Path
import runpy
import struct
import sys
import tempfile

reference=Path(sys.argv[1]).resolve()
script=Path(__file__).resolve().parents[1]/'scripts/ios16-rebase.py'

def fixture():
    b=bytearray(0x8000)
    struct.pack_into('<8I',b,0,0xfeedfacf,0x100000c,0,2,3,376,0,0)
    struct.pack_into('<II16sQQQQIIII',b,32,0x19,72,b'__TEXT',0xfffffff007004000,0x8000,0,0x8000,7,5,0,0)
    struct.pack_into('<4I',b,104,5,288,6,68)
    struct.pack_into('<Q',b,104+272,0xfffffff007004800)
    struct.pack_into('<4I',b,392,0x80000034,16,0x7000,0x100)
    struct.pack_into('<7I',b,0x7000,0,0x20,0,0,0,0,0)
    struct.pack_into('<II',b,0x7020,1,8)
    struct.pack_into('<IHHQIH',b,0x7028,24,0x4000,6,0,0,1)
    struct.pack_into('<H',b,0x703e,0x400)
    struct.pack_into('<Q',b,0x400,0x100)
    return b

with tempfile.TemporaryDirectory() as d:
    d=Path(d)
    cases=[('good',None,False),('bind',(0x400,'<Q',1<<63),True),
           ('escaped-page',(0x400,'<Q',0xfff<<51),True),
           ('arm64e',(8,'<I',2),True),('imports',(0x7010,'<I',1),True),
           ('bad-command',(36,'<I',0),True)]
    for name,change,fail in cases:
        b=fixture()
        if change: struct.pack_into(change[1],b,change[0],change[2])
        source=d/(name+'.input'); output=d/(name+'.macho'); source.write_bytes(b)
        sys.argv=[str(script),'--reference',str(reference),'--kernel',str(source),'--out',str(output)]
        try:
            with contextlib.redirect_stdout(io.StringIO()): runpy.run_path(str(script),run_name='__main__')
        except (ValueError,AssertionError):
            assert fail and not output.exists(),name
        else:
            assert not fail,name
            assert struct.unpack_from('<Q',output.read_bytes(),0x400)[0]==0xfffffff007004100
print('Six ARM64 rebase/rejection cases passed')
