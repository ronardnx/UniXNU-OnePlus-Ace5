"""Run the production fixed ADT finder and loader hook on host memory only."""
from pathlib import Path
import importlib.util
import os
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT=Path(__file__).resolve().parents[1]
HEADERS=ROOT/'uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe'
sys.path.insert(0,str(ROOT/'scripts'))
from ios16_gpu_power import placeholder
from ios16_platform import read_nodes


def node(name,props=(),children=()):
    values=[('name',name.encode()+b'\0'),*props]
    result=struct.pack('<II',len(values),len(children))
    for key,value in values:
        result+=struct.pack('<32sI',key.encode(),len(value))+value+bytes(-len(value)%4)
    return result+b''.join(children)


def tree(report=None,extra=(),children=None,root_props=None):
    chosen=[] if report is None else [('giulia-gpu-power',report)]
    chosen+=list(extra)
    if children is None:
        children=[node('chosen',chosen),node('cpus',children=[node('cpu0')])]
    if root_props is None:root_props=[('physical-model',b'OnePlus Ace 5\0')]
    return node('device-tree',root_props,children)


STUB=r'''
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
#define DEBUG(x) ((void)0)
#define DEBUG_ERROR 0
#define GPP_FIRMWARE_TYPES 1
#include "GpuPowerReport.h"
static unsigned scenario,locates,prepares;
static EFI_STATUS EFIAPI prepare(struct gpp_report *r,UINTN size) {
    ++prepares;
    if(size!=256 || !gpp_report_requested(r))abort();
    if(scenario==6)return EFI_SUCCESS; /* Provider forgot to populate result. */
    r->phase=GPP_PHASE_DONE;r->flags=GPP_READY;
    r->attempted=3;r->confirmed=3;r->retained=1;
    r->values[0]=0;r->valid_reads=1;
    if(scenario==3 || scenario==8) {
        r->code=GPP_ERR_RESOURCE;r->phase=GPP_PHASE_CLOCKS;
        r->firmware_status=EFI_DEVICE_ERROR;r->failure_phase=GPP_PHASE_CLOCKS;
    }
    gpp_report_seal(r);
    if(scenario==4)r->crc32^=1;
    if(scenario==7) {r->version=77;gpp_report_seal(r);}
    return scenario==3 || scenario==5?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
struct protocol {UINT64 Version;EFI_STATUS(EFIAPI *Prepare)(struct gpp_report *,UINTN);};
static struct protocol provider={1,prepare};
static EFI_STATUS locate(EFI_GUID *guid,VOID *registration,VOID **out) {
    EFI_GUID expected=GPP_PROTOCOL_GUID;
    ++locates;
    if(memcmp(guid,&expected,sizeof(expected)) || registration)abort();
    if(scenario==1)return EFI_NOT_FOUND;
    provider.Version=scenario==2?2:1;
    provider.Prepare=scenario==10?NULL:prepare;
    *out=scenario==9?NULL:&provider;
    return EFI_SUCCESS;
}
static struct {EFI_STATUS(*LocateProtocol)(EFI_GUID *,VOID *,VOID **);} services={locate},*gBS=&services;
#include "GpuPowerHandoff.h"
int main(int argc,char **argv) {
    if(argc==3 && !strcmp(argv[1],"--placeholder")) {
        struct gpp_report r;gpp_report_init(&r);
        if(!gpp_report_requested(&r) || gpp_report_valid(&r))return 9;
        FILE *out=fopen(argv[2],"wb");if(!out)return 3;
        if(fwrite(&r,1,sizeof(r),out)!=sizeof(r))return 3;
        return fclose(out)?3:0;
    }
    if(argc!=5)return 3;
    scenario=(unsigned)strtoul(argv[3],NULL,10);
    FILE *input=fopen(argv[1],"rb");if(!input)return 3;
    fseek(input,0,SEEK_END);UINTN bytes=(UINTN)ftell(input);rewind(input);
    UINT8 *data=malloc(bytes?bytes:1);if(!data)return 3;
    if(fread(data,1,bytes,input)!=bytes)return 3;
    fclose(input);
    UINTN offset=0;int found=gpa_adt_find(data,bytes,256,&offset);
    EFI_STATUS status=GiuliaGpuPowerDiagnostic(data,bytes,argv[4],strlen(argv[4])+1);
    FILE *output=fopen(argv[2],"wb");if(!output)return 3;
    if(fwrite(data,1,bytes,output)!=bytes)return 3;
    fclose(output);free(data);
    printf("status=%llu locates=%u prepares=%u found=%d offset=%zu\n",
        (unsigned long long)status,locates,prepares,found,offset);
    return 0;
}
'''


with tempfile.TemporaryDirectory() as folder:
    work=Path(folder)
    code=work/'handoff.c';code.write_text(STUB)
    binary=work/'handoff'
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                    '-I',str(HEADERS),str(code),'-o',str(binary)],check=True)
    request=placeholder()
    subprocess.run([str(binary),'--placeholder',str(work/'request.bin')],check=True)
    assert (work/'request.bin').read_bytes()==request,'C/Python placeholder ABI drift'
    assert len(request)==256 and request[16:]==bytes(240)
    count=0

    def run(label,blob,scenario=0,command='giulia_gpu=3',calls=1,changed=True,found=1):
        global count
        count+=1
        before=work/(label+'.adt');after=work/(label+'.result')
        before.write_bytes(blob)
        output=subprocess.check_output([str(binary),str(before),str(after),str(scenario),command],text=True)
        fields={key:int(value) for key,value in (part.split('=') for part in output.split())}
        assert fields['found']==found,(label,fields)
        assert fields['prepares']==calls,(label,fields)
        result=after.read_bytes()
        if not changed:
            assert result==blob,label
            if not calls:assert fields['locates']==0,(label,fields)
            return fields,None
        offset=fields['offset'];report=result[offset:offset+256]
        assert len(report)==256 and report[:16]==request[:16],label
        assert report[-4:]==struct.pack('<I',zlib.crc32(report[:-4])),label
        assert result[:offset]==blob[:offset] and result[offset+256:]==blob[offset+256:],label
        return fields,report

    valid=tree(request)
    for scenario in range(11):
        calls=0 if scenario in (1,2,9,10) else 1
        fields,report=run(f'provider-{scenario}',valid,scenario,calls=calls)
        code_value,phase=struct.unpack_from('<II',report,16)
        status=struct.unpack_from('<Q',report,80)[0]
        assert status==fields['status']
        if scenario==0:
            assert code_value==0 and phase==11
            assert struct.unpack_from('<I',report,48)[0]==1
        elif scenario in (3,8):
            assert code_value==6 and phase==8
            assert struct.unpack_from('<I',report,32)[0]==3
        else:
            assert code_value==11 and phase==0x100
            assert not struct.unpack_from('<I',report,52)[0]&2

    commands=['','giulia_gpu=1','giulia_gpu=2','giulia_gpu=03','giulia_gpu=3x',
              'other=giulia_gpu=3','giulia_gpu=3 giulia_gpu=3','giulia_gpu=3 giulia_gpu=2',
              'giulia_gpu=3 giulia_gpu','giulia_gpu=3\n','giulia_gpu=3\t']
    for index,command in enumerate(commands):
        run(f'gate-{index}',valid,command=command,calls=0,changed=False)
    run('surrounded-token',valid,command='-v cpus=1 giulia_gpu=3 serial=3')

    malformed=[('absent',tree(),0),('short-report',tree(request[:-1]),-1),
               ('long-report',tree(request+b'\0'),-1),
               ('duplicate-property',tree(request,extra=[('giulia-gpu-power',request)]),-1),
               ('duplicate-other-property',tree(request,extra=[('x',b'a'),('x',b'b')]),-1),
               ('duplicate-chosen',tree(children=[node('chosen',[('giulia-gpu-power',request)]),node('chosen')]),-1),
               ('wrong-node',tree(children=[node('chosen'),node('other',[('giulia-gpu-power',request)])]),-1),
               ('nested-chosen',tree(children=[node('chosen'),node('other',children=[node('chosen',[('giulia-gpu-power',request)])])]),-1),
               ('missing-identity',tree(request,root_props=[]),-1),
               ('wrong-identity',tree(request,root_props=[('physical-model',b'other\0')]),-1),
               ('duplicate-sibling',tree(children=[node('chosen',[('giulia-gpu-power',request)]),node('cpu'),node('cpu')]),-1),
               ('trailing',valid+b'!',-1)]
    malformed += [(f'truncated-{length}',valid[:length],-1) for length in (0,1,7,8,39,len(valid)-1)]
    deep=node('leaf')
    for index in range(65):deep=node('n'+str(index),children=[deep])
    malformed += [('deep',tree(children=[node('chosen',[('giulia-gpu-power',request)]),deep]),-1),
                  ('many-nodes',tree(children=[node('chosen',[('giulia-gpu-power',request)]),*[node('n'+str(i)) for i in range(256)]]),-1),
                  ('many-properties',tree(request,extra=[('k'+str(i),b'') for i in range(256)]),-1)]
    for label,blob,found in malformed:
        run(label,blob,calls=0,changed=False,found=found)
    stale=bytearray(request);stale[16]=1
    _,rejected=run('used-request',tree(bytes(stale)),calls=0)
    assert struct.unpack_from('<II',rejected,16)==(11,0x100)
    # An additional property changes report alignment by four bytes. The
    # production helper must use CopyMem, never a cast to an aligned struct.
    aligned=tree(children=[node('chosen',[('padding',b''),('giulia-gpu-power',request)])])
    field,_=run('four-byte-report-alignment',aligned)
    if field['offset']%8!=4:
        aligned=tree(children=[node('chosen',[('giulia-gpu-power',request)])])
        field,_=run('four-byte-report-alignment-second',aligned)
    assert field['offset']%8==4

    spec=importlib.util.spec_from_file_location('enable_gpu_power',ROOT/'scripts/enable-gpu-power-diagnostics.py')
    installer=importlib.util.module_from_spec(spec);spec.loader.exec_module(installer)
    loader=work/'loader';loader.mkdir()
    template=('#include <Uefi.h>\nvoid Boot(void) {\n'
              '  CopyMem ((VOID *)(UINTN)TreeAddress, Tree, TreeSize);\n'
              '  Root->Close (Root); Root = NULL;\n'
              '  Status = GetMap (&Map, &Capacity, &MapSize, &Key, &Stride);\n'
              '  gBS->ExitBootServices (mImage, Key);\n}\n')
    (loader/'DarwinBoot.c').write_text(template)
    installer.install(loader);first=(loader/'DarwinBoot.c').read_bytes()
    installer.install(loader);assert (loader/'DarwinBoot.c').read_bytes()==first
    installed=first.decode()
    assert installed.count('GiuliaGpuPowerDiagnostic (')==1
    assert installed.index('Root->Close')<installed.index('GiuliaGpuPowerDiagnostic (')<installed.index('Status = GetMap')
    assert 'giulia_gpu=' not in installed and 'Args->CommandLine' not in installed
    (loader/'DarwinBoot.c').write_text(template.replace('  Root->Close (Root); Root = NULL;\n',''))
    broken=(loader/'DarwinBoot.c').read_bytes()
    try:installer.install(loader)
    except ValueError:pass
    else:raise AssertionError('Changed loader anchors accepted')
    assert (loader/'DarwinBoot.c').read_bytes()==broken
print(f'GPU ADT handoff: {count} host-only parser/gate/provider cases; C/Python ABI, exact writes and idempotent late hook passed')
