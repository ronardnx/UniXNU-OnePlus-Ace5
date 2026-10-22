"""Reproduce the captured PA truncation using the actual Darwin 22 function.

Apply the real patch to a pinned source copy, compile both function bodies,
and check the ARM64 assembly. No device or physical memory is accessed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import urllib.request

ROOT=Path(__file__).resolve().parents[1]


def body(source):
    match=re.search(r'void\npmap_sync_page_attributes_phys\(\s*ppnum_t pp\)\s*\{[^}]+\}',source)
    if not match:raise ValueError('Expected pinned cache-sync function')
    return match[0]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path)
    p.add_argument('--fetch',action='store_true')
    p.add_argument('--out',type=Path,required=True)
    a=p.parse_args()
    pin=json.loads((ROOT/'hardware/sm8650-high-physical-cache.json').read_text())['source']
    source=a.source or ROOT/'.cache/gpu-memory-source/pmap.c'
    if not source.exists() and a.fetch:
        with urllib.request.urlopen(pin['url'],timeout=60) as response:data=response.read()
        if hashlib.sha256(data).hexdigest()!=pin['sha256']:raise ValueError('Downloaded source hash mismatch')
        source.parent.mkdir(parents=True,exist_ok=True);source.write_bytes(data)
    data=source.read_bytes()
    if hashlib.sha256(data).hexdigest()!=pin['sha256']:raise ValueError('Unqualified XNU pmap source')
    old=body(data.decode())
    patch=ROOT/'ios16/0081-giulia-pmap-high-physical-cache-sync.patch'
    a.out.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        tmp=Path(tmp);file=tmp/'osfmk/arm/pmap/pmap.c';file.parent.mkdir(parents=True);file.write_bytes(data)
        subprocess.run(['git','apply','--check',str(patch.resolve())],cwd=tmp,check=True)
        subprocess.run(['git','apply',str(patch.resolve())],cwd=tmp,check=True)
        new=body(file.read_text())
        harness='''#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
typedef uint32_t ppnum_t;
typedef uint64_t vm_offset_t;
#ifndef PAGE_SHIFT
#define PAGE_SHIFT 14
#endif
#define PAGE_SIZE (1u << PAGE_SHIFT)
#define TRUE 1
static uint64_t observed;
static void flush_dcache(vm_offset_t pa,unsigned bytes,int physical) {
 assert(bytes==PAGE_SIZE && physical==TRUE);observed=pa;
}
'''+old.replace('pmap_sync_page_attributes_phys','original_sync')+'\n'+new+'''
int main(void) {
 const uint64_t addresses[]={0,0xffffc000ULL,0x100000000ULL,0x8c0000000ULL,
   0x8c4a00000ULL,0x93fffc000ULL,0xffffffc000ULL};
 unsigned i;assert(sizeof(ppnum_t)==4 && sizeof(vm_offset_t)==8);
 for(i=0;i<sizeof(addresses)/sizeof(addresses[0]);++i) {
   uint64_t pa=addresses[i]&~((uint64_t)PAGE_SIZE-1);
   assert((pa>>PAGE_SHIFT)<=UINT32_MAX);
   pmap_sync_page_attributes_phys((ppnum_t)(pa>>PAGE_SHIFT));assert(observed==pa);
 }
 original_sync((ppnum_t)(0x8c4a00000ULL>>PAGE_SHIFT));
 assert(observed==0xc4a00000ULL);
 pmap_sync_page_attributes_phys((ppnum_t)(0x8c4a00000ULL>>PAGE_SHIFT));
 assert(observed==0x8c4a00000ULL);
 printf("PAGE_SHIFT=%u: old=0xc4a00000, fixed=0x8c4a00000; high-PA cases passed\\n",PAGE_SHIFT);
 return 0;
}
'''
        test=tmp/'cache.c';test.write_text(harness)
        output=[]
        for shift in (12,14,16):
            binary=tmp/('test-'+str(shift))
            subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                            '-DPAGE_SHIFT='+str(shift),str(test),'-o',str(binary)],check=True)
            result=subprocess.check_output([str(binary)],text=True);output.append(result.strip());print(result,end='')
        prelude='typedef unsigned int ppnum_t;\ntypedef unsigned long long vm_offset_t;\n#define PAGE_SHIFT 14\n#define PAGE_SIZE 16384\n#define TRUE 1\nextern void flush_dcache(vm_offset_t,unsigned,int);\n'
        for label,function in [('old',old),('fixed',new)]:
            c=tmp/(label+'.c');c.write_text(prelude+function)
            asm=a.out/(label+'-aarch64.s')
            subprocess.run(['clang','--target=aarch64-none-elf','-O2','-ffreestanding','-S',str(c),'-o',str(asm)],check=True)
        oldasm=(a.out/'old-aarch64.s').read_text();fixedasm=(a.out/'fixed-aarch64.s').read_text()
        assert re.search(r'lsl\s+w0,\s*w0,\s*#14',oldasm)
        assert not re.search(r'lsl\s+w0,\s*w0,\s*#14',fixedasm)
        assert re.search(r'(?:ubfiz\s+x0,\s*x0,\s*#14,\s*#32|lsl\s+x0,\s*x0,\s*#14)',fixedasm)
        report={'passed':True,'source_sha256':pin['sha256'],
            'patch_sha256':hashlib.sha256(patch.read_bytes()).hexdigest(),'page_shifts':[12,14,16],
            'cases':21,'captured_low_pa_reproduced':True,'aarch64_widen_before_shift':True,
            'hardware_tested':False,'output':output}
        (a.out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
        print('Real source patch and AArch64 instruction-width check passed; no phone test')


if __name__=='__main__':main()
