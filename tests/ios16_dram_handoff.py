"""Run the generated DarwinBoot ADT parser and DRAM writes under ASan/UBSan."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile


def node(name, props=(), children=()):
    props = [('name', name.encode() + b'\0'), *props]
    data = struct.pack('<II', len(props), len(children))
    for key, value in props:
        data += key.encode().ljust(32, b'\0') + struct.pack('<I', len(value))
        data += value + bytes(-len(value) % 4)
    return data + b''.join(children)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--driver', type=Path, required=True)
    p.add_argument('--adt', type=Path)
    a = p.parse_args()
    code = (a.driver / 'DarwinBoot.c').read_text()
    parser = code[code.index('STATIC\nBOOLEAN\nAdtNode ('):code.index('STATIC\nBOOLEAN\nValidTrustCaches (')]
    guard = code[code.index('  Offset = 0;'):code.index('  if (KeepPacked &&')]
    guard = guard.replace('Status = EFI_COMPROMISED_DATA; goto Fail;', 'free (Data); return 2;')
    writes = '\n'.join(line for line in code.splitlines()
                       if 'WriteUnaligned64' in line and ('DramBaseOffset' in line or 'DramSizeOffset' in line))
    assert len(writes.splitlines()) == 2
    stub = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t UINT8;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef size_t UINTN;
typedef char CHAR8;
typedef int BOOLEAN;
#define STATIC static
#define CONST const
#define FALSE 0
#define TRUE 1
#define VOID void
#define MAX_UINTN SIZE_MAX
#define ALIGN_VALUE(x,a) (((x)+(a)-1)&~((size_t)(a)-1))
#define ScanMem8(p,n,v) memchr((p),(v),(n))
#define CompareMem memcmp
#define AsciiStrCmp strcmp
#include "WindowSize.h"
static UINT32 U32(const void *p) { UINT32 x; memcpy(&x,p,4); return x; }
static UINT64 U64(const void *p) { UINT64 x; memcpy(&x,p,8); return x; }
static void WriteUnaligned64(UINT64 *p, UINT64 x) { memcpy(p,&x,8); }
'''
    runner = r'''
int main(int argc,char **argv) {
  if(argc!=3) return 3;
  FILE *f=fopen(argv[1],"rb"); if(!f) return 3;
  fseek(f,0,SEEK_END); UINTN TreeSize=(UINTN)ftell(f); rewind(f);
  UINT8 *Data=malloc(TreeSize ? TreeSize : 1); if(!Data) return 3;
  if(fread(Data,1,TreeSize,f)!=TreeSize) return 3; fclose(f);
  const void *Tree=Data;
  UINTN Offset,RamdiskOffset,TrustCacheOffset,DramBaseOffset,DramSizeOffset;
  UINT64 WindowSize;
  BOOLEAN KeepPacked;
  /*GUARD*/
  struct { UINT64 PhysicalBase,MemorySize; } BootArgs={0x980000000ULL,WindowSize};
  const typeof(BootArgs) *Args=&BootArgs;
  UINT64 TreeAddress=(UINT64)(uintptr_t)Data;
  /*WRITES*/
  f=fopen(argv[2],"wb"); if(!f) return 3;
  if(fwrite(Data,1,TreeSize,f)!=TreeSize) return 3;
  fclose(f); free(Data); return 0;
}
'''.replace('/*GUARD*/', guard).replace('/*WRITES*/', writes)
    zero = bytes(8)
    props = [('dram-base', zero), ('dram-size', zero),
             ('darwinboot-memory-size', struct.pack('<Q', 0x80000000))]
    def tree(properties=props, children=None):
        return node('device-tree', children=[node('chosen', properties)] if children is None else children)
    valid = tree()
    cases = [
        ('valid', valid, True),
        ('missing-base', tree(props[1:]), False),
        ('missing-size', tree([props[0], props[2]]), False),
        ('duplicate-base', tree([*props, props[0]]), False),
        ('duplicate-size', tree([*props, props[1]]), False),
        ('short-base', tree([('dram-base', bytes(4)), *props[1:]]), False),
        ('long-size', tree([props[0], ('dram-size', bytes(16)), props[2]]), False),
        ('stale-base', tree([('dram-base', struct.pack('<Q',0xc0000000)), *props[1:]]), False),
        ('stale-size', tree([props[0], ('dram-size', struct.pack('<Q',0x80000000)), props[2]]), False),
        ('wrong-node', tree(children=[node('other', props)]), False),
        ('duplicate-chosen', tree(children=[node('chosen', props),node('chosen',props)]), False),
        ('nested-chosen', tree(children=[node('other',children=[node('chosen',props)])]), False),
        ('unrelated-children', tree(children=[node('chosen',props),node('cpus',children=[node('cpu0')])]), True),
        ('oversized-window', tree([*props[:2],('darwinboot-memory-size',struct.pack('<Q',0x400000000))]), False),
        ('trailing-bytes', valid+b'bad!', False),
    ]
    cases += [(f'truncated-{n}', valid[:n], False) for n in (0,1,7,8,39,len(valid)-1)]
    if a.adt:
        cases.append(('generated-ace5-adt', a.adt.read_bytes(), True))
    with tempfile.TemporaryDirectory() as td:
        work = Path(td)
        src, exe = work/'handoff.c', work/'handoff'
        src.write_text(stub+parser+runner)
        subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                        '-I',str(a.driver.resolve()),str(src),'-o',str(exe)],check=True)
        for name, blob, ok in cases:
            inp, out = work/(name+'.adt'), work/(name+'.patched')
            inp.write_bytes(blob)
            r = subprocess.run([str(exe),str(inp),str(out)],capture_output=True)
            assert r.returncode == (0 if ok else 2), (name,r.returncode,r.stderr.decode())
            if ok:
                expected = bytearray(blob)
                # Check only the two property payloads changed, and boot_args
                # values were used verbatim, including the >4 GiB base.
                for key, value in [('dram-base',0x980000000),('dram-size',0x80000000)]:
                    at = blob.index(key.encode().ljust(32,b'\0'))+36
                    expected[at:at+8] = struct.pack('<Q',value)
                assert out.read_bytes()==expected, name
            else:
                assert not out.exists(), name
    print(f'Darwin22 DRAM handoff: {len(cases)} parser, validation and physical-window cases passed')


if __name__ == '__main__':
    main()
