"""Exercise the generated EFI helper at Darwin22's real command-line limit."""
from pathlib import Path
import base64
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from ios16_wifi_pcie import decode_reports

with tempfile.TemporaryDirectory() as folder:
    root=Path(folder)
    source=root/'DarwinBoot.c'
    source.write_text('#include <Uefi.h>\nVOID Original(VOID) {\n'
        '  CopyMem (Args->CommandLine, CommandLine, sizeof (CommandLine));\n}\n')
    command=[sys.executable,str(ROOT/'scripts/enable-wifi-diagnostics.py'),str(root)]
    subprocess.run(command,check=True,capture_output=True)
    first=source.read_bytes()
    subprocess.run(command,check=True,capture_output=True)
    assert source.read_bytes()==first,'Helper installation is not idempotent'
    text=source.read_text()
    helper=text[text.index('#include "GiuliaWifiReport.h"'):text.index('VOID Original')]
    stub=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "GiuliaWifiReport.h"
typedef unsigned long long UINT64;
typedef unsigned long UINTN;
typedef unsigned long long EFI_STATUS;
typedef char CHAR8;
typedef struct { uint32_t a; uint16_t b,c; uint8_t d[8]; } EFI_GUID;
#define STATIC static
#define CONST const
#define VOID void
#define EFIAPI
#define EFI_ERROR(x) ((x)!=0)
#define STATIC_ASSERT _Static_assert
#define DEBUG(x) ((void)0)
#define AsciiStrLen strlen
#define CopyMem memcpy
#define ZeroMem(p,n) memset(p,0,n)
static unsigned calls;
static EFI_STATUS prepare(struct gw_report *r,UINTN size) {
  assert(size==168);calls++;
  r->version=5;r->stage=13;r->error=7;r->detail=256;
  r->rails=127;r->clocks=4095;r->firmware_status=0x8000000000000021ULL;
  r->bar1=0x89abcdef;r->ltssm_seen_lo=3;r->ltssm_seen_hi=1;
  r->pipe_hz=125000000;r->pipe_measure_status=1;
  return 1;
}
static struct {UINT64 version; EFI_STATUS (*prepare)(struct gw_report *,UINTN);}
  protocol={5,prepare};
static EFI_STATUS locate(EFI_GUID *g,VOID *registration,VOID **out) {
  (void)g;(void)registration;*out=&protocol;return 0;
}
static struct {EFI_STATUS (*LocateProtocol)(EFI_GUID *,VOID *,VOID **);}
  services={locate},*gBS=&services;
'''
    main=r'''
int main(void) {
  struct {char before[8],line[608],after[8];} b;
  for(unsigned capacity=0;capacity<=608;capacity++) {
    memset(&b,'!',sizeof(b));memset(b.line,'x',360);b.line[360]=0;calls=0;
    GiuliaAppendWifiDiagnostic(b.line,capacity);
    if(capacity<=600) assert(!calls && strlen(b.line)==360);
    else assert(calls==1 && strlen(b.line)<capacity);
    assert(b.before[0]=='!' && b.after[0]=='!');
  }
  puts(b.line+360);
  struct gw_report raw;
  unsigned char *bytes=(unsigned char *)&raw;
  char encoded[226];
  for(unsigned i=0;i<sizeof(raw);i++) bytes[i]=(unsigned char)(i^0xa5);
  memset(encoded,'!',sizeof(encoded));
  assert(!gw_report_base64(encoded,224,&raw) && encoded[0]=='!');
  assert(gw_report_base64(encoded,225,&raw) && encoded[224]==0 && encoded[225]=='!');
  puts(encoded);
  return 0;
}
'''
    harness=root/'helper.c'
    harness.write_text(stub+helper+main)
    binary=root/'helper'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined',str(harness),'-o',str(binary)],check=True)
    result=subprocess.run([str(binary)],check=True,capture_output=True).stdout.splitlines()
    report=decode_reports(result[0])[0]
    assert report['report_version']==5 and report['firmware_status']==0x8000000000000021
    assert report['bar1']==0x89abcdef and report['ltssm_states_seen']==[0,1,32]
    assert report['pipe_hz']==125000000 and report['pipe_frequency_measured']
    assert base64.b64decode(result[1],validate=True)==bytes(i^0xa5 for i in range(168))
    print('Wi-Fi handoff: 609 capacity cases, no writes before reservation, ABI round-trip and idempotence passed')
