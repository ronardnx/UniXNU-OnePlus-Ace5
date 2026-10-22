"""Verify unaligned DMA wire operations and their optimized AArch64 accesses."""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
source=r'''
#include "giulia_wifi_mhi.h"
#include "giulia_wifi_qmi.h"
#define FN __attribute__((noinline))
FN void mhi_copy(void *d,const void *s,size_t n){gwm_copy(d,s,n);}
FN void mhi_zero(void *d,size_t n){gwm_zero(d,n);}
FN void mhi_put32(unsigned char *p,uint32_t n){gwm_put32(p,n);}
FN void mhi_put64(unsigned char *p,uint64_t n){gwm_put64(p,n);}
FN void mhi_length(unsigned char *p){gwm_put64(p,0x80000);}
FN uint64_t mhi_read(const unsigned char *p){return gwm_u64(p);}
FN void qmi_copy(void *d,const void *s,size_t n){gwq_copy(d,s,n);}
FN void qmi_put16(void *p,uint16_t n){gwq_w16(p,n);}
FN void qmi_put32(void *p,uint32_t n){gwq_w32(p,n);}
FN void qmi_put64(void *p,uint64_t n){gwq_w64(p,n);}
FN uint16_t qmi_read16(const void *p){return gwq_u16(p);}
FN uint64_t qmi_read64(const void *p){return gwq_u64(p);}
#ifdef HOST_TEST
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void){
 unsigned char d[96],s[96];for(unsigned i=0;i<96;i++)s[i]=(unsigned char)(i*7);
 for(unsigned off=0;off<32;off++){
  memset(d,0xa5,sizeof(d));mhi_put64(d+off,0xfedcba9876543210ull);
  assert(mhi_read(d+off)==0xfedcba9876543210ull);
  for(unsigned i=0;i<96;i++)if(i<off || i>=off+8)assert(d[i]==0xa5);
  mhi_length(d+off);assert(mhi_read(d+off)==0x80000);
  qmi_put64(d+off,0x0123456789abcdefull);assert(qmi_read64(d+off)==0x0123456789abcdefull);
  qmi_put16(d+off,0xa55a);assert(qmi_read16(d+off)==0xa55a);
  mhi_copy(d+off,s+31-off,33);assert(!memcmp(d+off,s+31-off,33));
  qmi_copy(d+off,s+31-off,33);assert(!memcmp(d+off,s+31-off,33));
  mhi_zero(d+off,33);for(unsigned i=0;i<33;i++)assert(d[off+i]==0);
 }
 puts("DMA byte helpers: 32 unaligned offsets, exact lengths and guard bytes passed");
}
#endif
'''
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory);c=root/'dma.c';c.write_text(source)
    native=root/'native'
    subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-O2',
        '-fsanitize=address,undefined','-DHOST_TEST','-I',str(ROOT/'ios16'),str(c),'-o',str(native)],check=True)
    subprocess.run([str(native)],check=True)
    for level in ('-O2','-O3'):
        asm=root/(level+'.s')
        subprocess.run(['clang','--target=aarch64-none-elf','-std=c11',level,'-ffreestanding',
            '-I',str(ROOT/'ios16'),'-S',str(c),'-o',str(asm)],check=True)
        text=asm.read_text();count=0
        for line in text.splitlines():
            match=re.match(r'\s+([a-z][a-z0-9]*)\s',line)
            if match and match[1].startswith(('ld','st')):
                assert match[1] in ('ldrb','ldurb','strb','sturb'),line
                count+=1
        assert count>32 and 'mhi_length:' in text
        print('AArch64 '+level+': all DMA helper loads/stores remain byte-width; no merged unaligned word/vector access')
