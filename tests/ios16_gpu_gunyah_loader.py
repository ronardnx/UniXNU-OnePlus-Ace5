"""Exercise the actual loader handoff and kernel decoder on host memory only."""
from pathlib import Path as _ModulePath
import sys as _module_sys
_module_sys.path.insert(0, str(_ModulePath(__file__).resolve().parents[1] / "scripts"))
from ios16_sources import include_flags
from pathlib import Path
import importlib.util
import os
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
HEADERS = ROOT / 'uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe'
sys.path.insert(0, str(ROOT / 'scripts'))
from ios16_gpu_gunyah import PROPERTY, add_request, check_request, placeholder
from ios16_platform import read_nodes


def node(name, props=(), children=()):
    values = [('name', name.encode() + b'\0'), *props]
    data = struct.pack('<II', len(values), len(children))
    for key, value in values:
        data += struct.pack('<32sI', key.encode(), len(value)) + value + bytes(-len(value) % 4)
    return data + b''.join(children)


def tree(report=None, extra=(), children=None, root_props=None):
    chosen = [] if report is None else [(PROPERTY, report)]
    chosen += list(extra)
    if children is None:
        children = [node('chosen', chosen), node('cpus', children=[node('cpu0')])]
    if root_props is None:
        root_props = [('physical-model', b'OnePlus Ace 5\0')]
    return node('device-tree', root_props, children)


STUB = r'''
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
typedef void VOID;
typedef UINT64 EFI_STATUS;
typedef struct {UINT32 a;uint16_t b,c;UINT8 d[8];} EFI_GUID;
#define EFIAPI
#define EFI_SUCCESS UINT64_C(0)
#define EFI_NOT_STARTED UINT64_C(0x8000000000000013)
#define EFI_NOT_FOUND UINT64_C(0x800000000000000e)
#define EFI_UNSUPPORTED UINT64_C(0x8000000000000003)
#define EFI_DEVICE_ERROR UINT64_C(0x8000000000000007)
#define EFI_COMPROMISED_DATA UINT64_C(0x8000000000000021)
#define EFI_ERROR(x) (((x)>>63)!=0)
#define CopyMem memcpy
#define ZeroMem(p,n) memset((p),0,(n))
#define DEBUG(x) ((void)0)
#define DEBUG_INFO 0
#define GGR_FIRMWARE_TYPES 1
#include "GpuGunyahReport.h"
static unsigned scenario,locates,captures;
static void populated(struct ggr_report *r) {
    ggr_report_init(r);r->source=GGR_SOURCE_BOOT_FDT;r->fdt_bytes=4096;
    r->fdt_crc32=0x01234567;r->epoch=UINT64_C(0x123456789abcdef0);
    r->tx_cap=UINT64_C(0x09c762828ef31568);r->rx_cap=UINT64_C(0x09c762828ef3445a);
    r->rm_node=256;r->gic_node=128;r->interrupt_parent=1;r->interrupt_cells=3;
    r->tx_irq[0]=r->rx_irq[0]=0;r->tx_irq[1]=0x3a0;r->rx_irq[1]=0x3a1;
    r->tx_irq[2]=r->rx_irq[2]=1;ggr_report_seal(r);
    if(!ggr_report_valid(r))abort();
}
static EFI_STATUS EFIAPI capture(struct ggr_report *r,UINTN size) {
    ++captures;if(size!=256 || !ggr_report_requested(r))abort();
    if(scenario==5)return EFI_SUCCESS; /* Pristine request is not a result. */
    populated(r);
    if(scenario==3)return EFI_DEVICE_ERROR; /* Even CRC-valid data is cleared. */
    if(scenario==4){r->crc32^=1;return EFI_SUCCESS;}
    if(scenario==8)r->version=77;
    if(scenario==9)r->code=GGR_ERR_RESOURCE;
    if(scenario==10)r->epoch=0;
    if(scenario==11)r->rx_cap=r->tx_cap;
    if(scenario==12)r->tx_irq[0]=1;
    if(scenario==13)r->source=0;
    if(scenario==14)r->fdt_bytes=71;
    if(scenario==15)r->bytes=255;
    if(scenario==16)r->rx_cap=UINT64_MAX;
    if(scenario==17)r->rx_irq[1]=988;
    if(scenario==18)r->rx_irq[1]=r->tx_irq[1];
    if(scenario==19)r->reserved[38]=1;
    if(scenario==20)r->rm_node=3;
    ggr_report_seal(r);return EFI_SUCCESS;
}
struct protocol {UINT64 Version;EFI_STATUS(EFIAPI *Capture)(struct ggr_report *,UINTN);};
static struct protocol provider={1,capture};
static EFI_STATUS locate(EFI_GUID *guid,VOID *registration,VOID **out) {
    const EFI_GUID expected=GGR_PROTOCOL_GUID;++locates;
    if(memcmp(guid,&expected,sizeof(expected)) || registration)abort();
    if(scenario==1)return EFI_NOT_FOUND;
    provider.Version=scenario==2?2:1;provider.Capture=scenario==7?NULL:capture;
    *out=scenario==6?NULL:&provider;return EFI_SUCCESS;
}
static struct {EFI_STATUS(*LocateProtocol)(EFI_GUID *,VOID *,VOID **);} services={locate},*gBS=&services;
#include "GpuGunyahHandoff.h"
#include "giulia_gpu_gunyah_boot.h"
static int write_file(const char *path,const void *data,size_t size) {
    FILE *out=fopen(path,"wb");if(!out)return 3;
    if(fwrite(data,1,size,out)!=size)return 3;
    return fclose(out)?3:0;
}
static UINT8 *read_file(const char *path,UINTN *bytes,unsigned shift) {
    FILE *in=fopen(path,"rb");if(!in)return NULL;
    if(fseek(in,0,SEEK_END))return NULL;
    long length=ftell(in);if(length<0)return NULL;*bytes=(UINTN)length;rewind(in);
    UINT8 *data=malloc(*bytes+shift+1);if(!data)return NULL;
    if(fread(data+shift,1,*bytes,in)!=*bytes)return NULL;
    fclose(in);return data;
}
int main(int argc,char **argv) {
    if(!ggr_gpu_enabled("giulia_gpu=1",sizeof("giulia_gpu=1")) ||
       ggr_gpu_enabled(NULL,16) || ggr_gpu_enabled("giulia_gpu=1",0) ||
       ggr_gpu_enabled("giulia_gpu=1",strlen("giulia_gpu=1")))return 9;
    struct ggr_report rejected;memset(&rejected,0xaa,sizeof(rejected));
    if(ggr_boot_decode(NULL,sizeof(rejected),&rejected) || ggr_boot_decode(NULL,0,NULL))return 9;
    for(unsigned i=0;i<sizeof(rejected);i++)if(((UINT8 *)&rejected)[i])return 9;
    if(argc==3 && !strcmp(argv[1],"--placeholder")) {
        struct ggr_report r;ggr_report_init(&r);
        if(!ggr_report_requested(&r) || ggr_report_valid(&r))return 9;
        return write_file(argv[2],&r,sizeof(r));
    }
    if(argc==4 && !strcmp(argv[1],"--decode")) {
        UINTN bytes=0;UINT8 *data=read_file(argv[2],&bytes,1);if(!data)return 3;
        struct ggr_report r;memset(&r,0xaa,sizeof(r));
        int decoded=ggr_boot_decode(data+1,(unsigned)bytes,&r);free(data);
        printf("decoded=%d\n",decoded);return write_file(argv[3],&r,sizeof(r));
    }
    if(argc!=5)return 3;
    scenario=(unsigned)strtoul(argv[3],NULL,10);
    UINTN bytes=0;UINT8 *data=read_file(argv[1],&bytes,0);if(!data)return 3;
    UINTN offset=0;int found=gpa_adt_find_named(data,bytes,256,"giulia-gpu-gunyah",&offset);
    EFI_STATUS status=GiuliaGpuGunyahHandoff(data,bytes,argv[4],strlen(argv[4])+1);
    printf("status=%llu locates=%u captures=%u found=%d offset=%zu\n",
        (unsigned long long)status,locates,captures,found,offset);
    int result=write_file(argv[2],data,bytes);free(data);return result;
}
'''


def raises_value(callback, label):
    try:
        callback()
    except ValueError:
        return
    raise AssertionError(label)


with tempfile.TemporaryDirectory() as folder:
    work = Path(folder)
    code = work / 'gunyah-loader.c'
    code.write_text(STUB)
    binary = work / 'gunyah-loader'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I', str(HEADERS), *include_flags(ROOT), str(code), '-o', str(binary)], check=True)
    request = placeholder()
    subprocess.run([str(binary), '--placeholder', str(work / 'request.bin')], check=True)
    assert (work / 'request.bin').read_bytes() == request, 'C/Python request ABI drift'
    assert len(request) == 256 and request[:16] == struct.pack('<4I', 0x31524747, 1, 256, 1)
    assert request[16:] == bytes(240)
    count = 0

    def run(label, blob, scenario=0, command='giulia_gpu=3', calls=1, changed=True, found=1,
            success=False, locates=None):
        global count
        count += 1
        before, after = work / (label + '.adt'), work / (label + '.result')
        before.write_bytes(blob)
        output = subprocess.check_output([str(binary), str(before), str(after), str(scenario), command], text=True)
        fields = {key: int(value) for key, value in (part.split('=') for part in output.split())}
        assert fields['found'] == found and fields['captures'] == calls, (label, fields)
        if locates is not None:
            assert fields['locates'] == locates, (label, fields)
        result = after.read_bytes()
        if not changed:
            assert result == blob and not fields['locates'], (label, fields)
            return fields, None
        offset = fields['offset']
        report = result[offset:offset + 256]
        assert result[:offset] == blob[:offset] and result[offset + 256:] == blob[offset + 256:], label
        if success:
            assert fields['status'] == 0 and report[:16] == request[:16], (label, fields)
            assert report[-4:] == struct.pack('<I', zlib.crc32(report[:-4])), label
        else:
            assert fields['status'] >> 63 and report == bytes(256), (label, fields)
        return fields, report

    def decode(label, blob, accepted=False):
        global count
        count += 1
        before, after = work / (label + '.wire'), work / (label + '.decoded')
        before.write_bytes(blob)
        output = subprocess.check_output([str(binary), '--decode', str(before), str(after)], text=True)
        assert output.strip() == 'decoded=' + str(int(accepted)), (label, output)
        assert after.read_bytes() == (blob if accepted else bytes(256)), label

    valid = tree(request)
    _, captured = run('provider-success', valid, success=True, locates=1)
    decode('kernel-success', captured, accepted=True)
    for scenario in range(1, 21):
        calls = 0 if scenario in (1, 2, 6, 7) else 1
        run(f'provider-{scenario}', valid, scenario, calls=calls, locates=1)

    # Runtime-valid old data is never treated as a fresh loader request.
    run('stale-crc-valid', tree(captured), calls=0, locates=0)
    stale = bytearray(request)
    stale[16] = 1
    run('used-request', tree(bytes(stale)), calls=0, locates=0)
    for mode in (1, 2, 3):
        run(f'gpu-mode-{mode}', valid, command=f'giulia_gpu={mode}', success=True)
    run('surrounded-token', valid, command='-v cpus=1 giulia_gpu=2 serial=3', success=True)
    bad_commands = ['', 'giulia_gpu=0', 'giulia_gpu=4', 'giulia_gpu=03', 'giulia_gpu=3x',
                    'other=giulia_gpu=3', 'giulia_gpu', 'giulia_gpu=3 giulia_gpu=3',
                    'giulia_gpu=1 giulia_gpu=2', 'giulia_gpu=3 giulia_gpu',
                    'giulia_gpu=3\n', 'giulia_gpu=3\t', 'giulia_gpu=3\x7f']
    for index, command in enumerate(bad_commands):
        run(f'gate-{index}', valid, command=command, calls=0, changed=False)

    malformed = [
        ('absent', tree(), 0),
        ('short-slot', tree(request[:-1]), -1),
        ('long-slot', tree(request + b'\0'), -1),
        ('duplicate-property', tree(request, extra=[(PROPERTY, request)]), -1),
        ('duplicate-other-property', tree(request, extra=[('x', b'a'), ('x', b'b')]), -1),
        ('duplicate-chosen', tree(children=[node('chosen', [(PROPERTY, request)]), node('chosen')]), -1),
        ('wrong-node', tree(children=[node('chosen'), node('other', [(PROPERTY, request)])]), -1),
        ('nested-chosen', tree(children=[node('chosen'), node('other', children=[node('chosen', [(PROPERTY, request)])])]), -1),
        ('missing-identity', tree(request, root_props=[]), -1),
        ('wrong-identity', tree(request, root_props=[('physical-model', b'other\0')]), -1),
        ('duplicate-sibling', tree(children=[node('chosen', [(PROPERTY, request)]), node('cpu'), node('cpu')]), -1),
        ('wrong-root', node('root', [('physical-model', b'OnePlus Ace 5\0')], [node('chosen', [(PROPERTY, request)])]), -1),
        ('trailing', valid + b'!', -1),
    ]
    malformed += [(f'truncated-{length}', valid[:length], -1) for length in (0, 1, 7, 8, 39, len(valid) - 1)]
    deep = node('leaf')
    for index in range(65):
        deep = node('n' + str(index), children=[deep])
    malformed += [
        ('deep', tree(children=[node('chosen', [(PROPERTY, request)]), deep]), -1),
        ('many-nodes', tree(children=[node('chosen', [(PROPERTY, request)]), *[node('n' + str(i)) for i in range(256)]]), -1),
        ('many-properties', tree(request, extra=[('k' + str(i), b'') for i in range(256)]), -1),
    ]
    for label, blob, found in malformed:
        run(label, blob, calls=0, changed=False, found=found)
    four_aligned = False
    for padding in ([], [('padding', b'')]):
        blob = tree(children=[node('chosen', padding + [(PROPERTY, request)])])
        fields, report = run('unaligned-slot-' + str(len(padding)), blob, success=True)
        decode('unaligned-decode-' + str(len(padding)), report, accepted=True)
        four_aligned |= fields['offset'] % 8 == 4
    assert four_aligned, 'Did not exercise a slot lacking struct alignment'

    decode('kernel-placeholder', request)
    decode('kernel-zeroed', bytes(256))
    for length in (0, 1, 15, 255, 257):
        decode(f'kernel-length-{length}', (captured + b'!')[:length])
    for offset in range(256):
        corrupt = bytearray(captured)
        corrupt[offset] ^= 1
        decode(f'crc-byte-{offset}', bytes(corrupt))

    def reseal(blob):
        return bytes(blob[:-4]) + struct.pack('<I', zlib.crc32(blob[:-4]))

    # Recompute CRC after corrupting semantic fields: CRC is not provenance
    # or a grant of hypervisor/capability/IRQ ownership.
    invalid_fields = [(0, 0), (4, 2), (8, 255), (12, 0), (16, 3), (20, 0), (20, 2),
                      (24, 71), (24, 16 * 1024**2 + 1), (56, 4096), (56, 3), (56, 128),
                      (60, 4096), (60, 3), (64, 0), (64, 0xffffffff), (68, 2),
                      (72, 1), (76, 988), (80, 2), (84, 1), (88, 988), (88, 0x3a0), (92, 0)]
    invalid_fields += [(offset, 1) for offset in range(96, 252, 4)]
    for index, (offset, value) in enumerate(invalid_fields):
        corrupt = bytearray(captured)
        struct.pack_into('<I', corrupt, offset, value)
        decode(f'semantic-u32-{index}', reseal(corrupt))
    for index, (offset, value) in enumerate([(32, 0), (40, 0xffffffffffffffff),
                                            (48, 0xffffffffffffffff),
                                            (48, struct.unpack_from('<Q', captured, 40)[0])]):
        corrupt = bytearray(captured)
        struct.pack_into('<Q', corrupt, offset, value)
        decode(f'semantic-u64-{index}', reseal(corrupt))
    # Zero is a valid capability handle when distinct; it still grants no
    # runtime authority. The kernel decoder accepts unaligned input bytes.
    zero_cap = bytearray(captured)
    struct.pack_into('<Q', zero_cap, 40, 0)
    decode('zero-capability', reseal(zero_cap), accepted=True)

    base = tree(extra=[('unrelated', b'preserve me')])
    requested = add_request(base)
    nodes = read_nodes(requested)
    assert nodes['/device-tree/chosen'].pop(PROPERTY) == request
    assert nodes == read_nodes(base), 'Request insertion changed other data'
    consumer = b'prefix GIULIA GPU: boot Gunyah resources suffix'
    for mode in (1, 2, 3):
        metadata = check_request(requested, mode, consumer)
        assert metadata['source'] == 'current-boot-fdt'
        for flag in ('irq_lease', 'runtime_capabilities_verified', 'hypervisor_verified', 'hardware_verified'):
            assert metadata[flag] is False, (mode, flag)
    assert check_request(base, 0) is None
    for mode in (None, 0, 4):
        raises_value(lambda: check_request(requested, mode), f'Disabled GPU accepted request: {mode}')
    for label, blob in [('absent', base), ('old-result', tree(captured)),
                        ('wrong-node', tree(children=[node('chosen'), node('other', [(PROPERTY, request)])])),
                        ('bad-board', tree(request, root_props=[('physical-model', b'other\0')]))]:
        raises_value(lambda blob=blob: check_request(blob, 1, consumer), 'Invalid Python request: ' + label)
    raises_value(lambda: check_request(requested, 1, b'no consumer'), 'Missing kernel consumer accepted')
    for label, blob in [('duplicate', requested),
                        ('no-chosen', tree(children=[node('other')])),
                        ('bad-board', tree(root_props=[('physical-model', b'other\0')]))]:
        raises_value(lambda blob=blob: add_request(blob), 'Invalid request insertion: ' + label)

    spec = importlib.util.spec_from_file_location('enable_gpu_gunyah', ROOT / 'scripts/enable-gpu-gunyah-handoff.py')
    installer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(installer)
    loader = work / 'loader'
    loader.mkdir()
    template = ('#include <Uefi.h>\nvoid Boot(void) {\n'
                '  CopyMem ((VOID *)(UINTN)TreeAddress, Tree, TreeSize);\n'
                '  Root->Close (Root); Root = NULL;\n'
                '  Status = GetMap (&Map, &Capacity, &MapSize, &Key, &Stride);\n'
                '  gBS->ExitBootServices (mImage, Key);\n}\n')
    source = loader / 'DarwinBoot.c'
    source.write_text(template)
    installer.install(loader)
    first = source.read_bytes()
    installer.install(loader)
    assert source.read_bytes() == first, 'Installer is not idempotent'
    installed = first.decode()
    assert installed.count(installer.INCLUDE) == 1 and installed.count(installer.CALL) == 1
    assert installed.index('Root->Close') < installed.index(installer.CALL) < installed.index('Status = GetMap')
    assert 'CommandLine, sizeof (CommandLine)' in installed
    assert 'giulia_gpu=' not in installed and 'Args->CommandLine' not in installed
    for name in ('GpuGunyahReport.h', 'GpuGunyahHandoff.h', 'GpuPowerAdt.h'):
        assert (loader / name).read_bytes() == (HEADERS / name).read_bytes()

    def rejected_install(label, text, missing_header=False):
        destination = work / ('install-failure-' + label)
        destination.mkdir()
        candidate = destination / 'DarwinBoot.c'
        candidate.write_text(text)
        sentinel = destination / 'GpuGunyahReport.h'
        sentinel.write_bytes(b'unchanged header')
        before = {path.name: path.read_bytes() for path in destination.iterdir()}
        original_headers = installer.HEADERS
        try:
            if missing_header:
                installer.HEADERS = work / 'missing-headers'
                installer.HEADERS.mkdir(exist_ok=True)
                # A later missing dependency must be detected before copying
                # even the first header over the destination's sentinel.
                (installer.HEADERS / 'GpuGunyahReport.h').write_bytes((HEADERS / 'GpuGunyahReport.h').read_bytes())
            raises_value(lambda: installer.install(destination), 'Bad installer input: ' + label)
        finally:
            installer.HEADERS = original_headers
        assert {path.name: path.read_bytes() for path in destination.iterdir()} == before, label

    anchors = ['  Root->Close (Root); Root = NULL;\n',
               '  CopyMem ((VOID *)(UINTN)TreeAddress, Tree, TreeSize);\n',
               '  Status = GetMap (&Map, &Capacity, &MapSize, &Key, &Stride);\n',
               '  gBS->ExitBootServices (mImage, Key);\n']
    for index, anchor in enumerate(anchors):
        rejected_install(f'missing-{index}', template.replace(anchor, ''))
        rejected_install(f'duplicate-{index}', template.replace(anchor, anchor + anchor))
    rejected_install('wrong-order', template.replace(anchors[0], '').replace(anchors[2], anchors[2] + anchors[0]))
    rejected_install('ambiguous-call', template.replace(anchors[0], anchors[0] + '  GiuliaGpuGunyahHandoff (changed);\n'))
    rejected_install('duplicate-include', installed.replace(installer.INCLUDE, installer.INCLUDE * 2))
    rejected_install('misplaced-call', installed.replace(installer.CALL, '').replace(anchors[0], installer.CALL + anchors[0]))
    rejected_install('header-preflight', template, missing_header=True)

print(f'GPU Gunyah loader: {count} host-only handoff/decode cases; C/Python ABI, '
      'mode gates, stale-result clearing, no lease upgrade and idempotent installer preflight passed')
