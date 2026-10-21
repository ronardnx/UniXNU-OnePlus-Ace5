"""Check generated GUI patches, physical geometry gate and ADT pairing."""
import argparse
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from ios16_platform import read_nodes,check_platform


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--profile',type=Path,required=True)
    p.add_argument('--adt',type=Path,required=True)
    p.add_argument('--base-kernel',type=Path,required=True)
    a=p.parse_args()
    patches=sorted(a.profile.glob('*.patch'))
    assert len(patches)>55
    for patch in patches:
        subprocess.run(['git','apply','--numstat',str(patch.resolve())],check=True,stdout=subprocess.DEVNULL)
    text='\n'.join(p.read_text(errors='surrogateescape') for p in patches)
    for forbidden in ['0xb8000000', '1440x3200', 'sweet_touch.c', 'sweet_keys.c']:
        assert forbidden not in text,forbidden
    for required in ['SweetSurface.cpp','SweetFramebuffer.cpp','SweetHID.cpp',
                     'oneplus,giulia','GIULIA FB: geometry guard PASS']:
        assert required in text,required
    gate=(a.profile/'0060-vermeer-gui-guards.patch').read_text()
    expr=re.search(r'\+\s*if \((fPhys != .*?)\) \{',gate).group(1)
    program='''#include <stdint.h>
#include <assert.h>
static int accept(uint64_t fPhys,uint32_t fWidth,uint32_t fHeight,uint32_t fRowBytes) {
 return !('''+expr+''');
}
int main(void) {
 assert(accept(0xd5100000,1264,2780,5056));
 assert(!accept(0xb8000000,1440,3200,5760));
 assert(!accept(0xd5100001,1264,2780,5056));
 assert(!accept(0xd5100000,1440,2780,5056));
 assert(!accept(0xd5100000,1264,3200,5056));
 assert(!accept(0xd5100000,1264,2780,5760));
 assert(!accept(0xd5100000,0,0,0));
 assert(!accept(UINT64_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX));
 return 0;
}
'''
    with tempfile.TemporaryDirectory() as td:
        source=Path(td)/'gate.c';exe=Path(td)/'gate';source.write_text(program)
        subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(source),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
    blob=a.adt.read_bytes();nodes=read_nodes(blob)
    root=nodes['/device-tree'];chosen=nodes['/device-tree/chosen']
    assert root['model']==b'iPhone10,3\0' and root['physical-model']==b'OnePlus Ace 5\0'
    assert b'oneplus,giulia\0' in root['compatible'] and b'qcom,sm8650\0' in root['compatible']
    assert '/device-tree/product' in nodes
    assert chosen['dram-base']==chosen['dram-size']==bytes(8)
    assert struct.unpack('<I',chosen['display-scale'])[0]==3
    fb=nodes['/device-tree/sweet-fbcon']
    assert struct.unpack('<QQ',fb['reg'])==(0xd5100000,5056*2780)
    assert [struct.unpack('<I',fb[k])[0] for k in ['width','height','stride']]==[1264,2780,5056]
    disp=nodes['/device-tree/arm-io/disp0']
    assert disp['device_type']==b'display-subsystem\0'
    assert [struct.unpack('<I',disp[k])[0] for k in ['canvas-width','canvas-height']]==[1125,2436]
    try:check_platform(blob,a.base_kernel.read_bytes())
    except ValueError as e:assert 'GUI ADT requires compiled service' in str(e)
    else:raise AssertionError('Base kernel was accepted with a GUI ADT')
    print(f'GUI preparation verified: {len(patches)} patch syntaxes, 8 compiled geometry cases, real board ADT and rejection of the base kernel')
    print('This does not verify a linked GUI kernel or on-device graphics')


if __name__=='__main__':main()
