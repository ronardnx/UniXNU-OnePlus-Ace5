"""Check native cache instructions and execute its range loop with trace hooks.

Host hooks test address coverage/order only, never cache coherence on hardware.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = ROOT / 'ios16/giulia_gpu_cache.c'
with tempfile.TemporaryDirectory() as directory:
    folder = Path(directory)
    assembly = folder / 'cache.s'
    subprocess.run(['clang', '--target=aarch64-none-elf', '-ffreestanding', '-O2',
                    '-Wall', '-Wextra', '-Werror', '-DGG_CACHE_STANDALONE',
                    '-S', str(source), '-o', str(assembly)], check=True)
    asm = assembly.read_text().lower()
    assert re.search(r'\bmrs\s+x\d+,\s*ctr_el0', asm)
    assert re.search(r'\bdc\s+cvac,\s*x\d+', asm)
    assert re.search(r'\bdsb\s+sy\b', asm)
    assert not re.search(r'\bdc\s+(?:zva|ivac|civac)\b', asm)
    text = source.read_text()
    substitutions = {
        '#if !defined(__aarch64__)': '#if 0',
        '__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));': 'ctr=test_ctr;',
        '__asm__ volatile("dc cvac, %0" :: "r"(at) : "memory");': 'record(at);',
        '__asm__ volatile("dsb sy" ::: "memory");': '++barriers;',
    }
    for old, new in substitutions.items():
        assert text.count(old) == 1
        text = text.replace(old, new)
    fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static uint64_t test_ctr;
static uintptr_t addresses[1024];static unsigned count,barriers;
static void record(uintptr_t at){assert(count<1024 && !barriers);addresses[count++]=at;}
'''+text+r'''
static void reset(unsigned shift){test_ctr=(uint64_t)shift<<16;count=barriers=0;}
int main(void) {
    unsigned cases=0;
    for(unsigned shift=0;shift<=10;++shift) {
        size_t line=(size_t)4<<shift;
        for(unsigned offset=0;offset<line;offset+=line/4) {
            for(unsigned n=1;n<=3*line;n+=line/2) {
                uintptr_t at=0x100000+offset;reset(shift);
                assert(giulia_gpu_cache_publish((void *)at,n));
                assert(barriers==1 && count==(offset+n+line-1)/line);
                for(unsigned i=0;i<count;++i)assert(addresses[i]==0x100000+i*line);
                ++cases;
            }
        }
    }
    reset(4);assert(!giulia_gpu_cache_publish(NULL,4096) && !count && !barriers);
    assert(!giulia_gpu_cache_publish((void *)0x1000,0) && !count && !barriers);
    assert(!giulia_gpu_cache_publish((void *)(UINTPTR_MAX-63),65) && !count && !barriers);
    for(unsigned shift=11;shift<16;++shift) {
        reset(shift);assert(!giulia_gpu_cache_publish((void *)0x1000,4096) && !count && !barriers);
    }
    reset(4);assert(giulia_gpu_cache_publish((void *)(UINTPTR_MAX-63),64));
    assert(count==1 && barriers==1 && addresses[0]==UINTPTR_MAX-63);
    printf("GPU cache publication: %u range cases, invalid/overflow edges and barrier ordering passed; native AArch64 instructions checked\n",cases);
}
'''
    (folder / 'cache.c').write_text(fixture)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-DGG_CACHE_STANDALONE', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-I', str(ROOT / 'ios16'),
                    str(folder / 'cache.c'), '-o', str(folder / 'cache')], check=True)
    subprocess.run([str(folder / 'cache')], check=True)
