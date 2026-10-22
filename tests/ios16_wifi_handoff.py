"""Exercise the generated EFI helper at Darwin22's real command-line limit."""
from pathlib import Path
import base64
import subprocess
import sys
import tempfile
import os

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
static unsigned snapshot_available,snapshot_calls;
static unsigned snapshot_magic=0x31535047;
static unsigned pending_mode;
static EFI_STATUS prepare(struct gw_report *r,UINTN size) {
  assert(size==168);calls++;
  if(pending_mode) {
    r->version=5;r->stage=18;r->detail=GW_XNU_LINK_TAG;r->error=0;
    r->rails=511;r->clocks=4095;r->phy=0x86;r->root_device=0x11317cb;
    r->gpio_after=3;r->perst=0x2010000;r->ref_clock=1;return 0;
  }
  r->version=5;r->stage=13;r->error=7;r->detail=256;
  r->rails=127;r->clocks=4095;r->firmware_status=0x8000000000000021ULL;
  r->bar1=0x89abcdef;r->ltssm_seen_lo=3;r->ltssm_seen_hi=1;
  r->pipe_hz=125000000;r->pipe_measure_status=1;
  return 1;
}
static struct {UINT64 version; EFI_STATUS (*prepare)(struct gw_report *,UINTN);}
  protocol={5,prepare};
static EFI_STATUS snapshot(struct gw_report *r,UINTN size) {
  assert(size==168);snapshot_calls++;
  memset(r,0,size);r->version=snapshot_magic;r->stage=13;r->detail=7;
  return 0;
}
static struct {UINT64 version; EFI_STATUS (*prepare)(struct gw_report *,UINTN);}
  snapshot_protocol={1,snapshot};
static EFI_STATUS locate(EFI_GUID *g,VOID *registration,VOID **out) {
  (void)registration;
  if(g->d[7]==2) {
    if(!snapshot_available)return 1;
    *out=&snapshot_protocol;return 0;
  }
  *out=&protocol;return 0;
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
#ifdef TEST_SNAPSHOT
  snapshot_available=1;calls=0;snapshot_calls=0;
  memset(b.line,'x',360);b.line[360]=0;
  GiuliaAppendWifiDiagnostic(b.line,600);
  assert(!calls && !snapshot_calls);
  GiuliaAppendWifiDiagnostic(b.line,608);
  assert(calls==1 && snapshot_calls==1 && strlen(b.line)==598);
  assert(!strstr(b.line,"giulia_wifi5="));
  puts(b.line+360);
  snapshot_magic=0x32535047;
  memset(b.line,'x',360);b.line[360]=0;
  GiuliaAppendWifiDiagnostic(b.line,608);
  assert(snapshot_calls==2 && strstr(b.line,"giulia_pcid2="));
  puts(b.line+360);
#endif
  pending_mode=1;memset(b.line,'x',360);b.line[360]=0;
  GiuliaAppendWifiDiagnostic(b.line,608);
  assert(strstr(b.line,"giulia_wifip=") && !strstr(b.line,"giulia_wifi5="));
  puts(b.line+360);
  return 0;
}
'''
    harness=root/'helper.c'
    snapshot_mode=os.environ.get('GIULIA_WIFI_SNAPSHOT')=='1'
    harness.write_text(('#define TEST_SNAPSHOT 1\n' if snapshot_mode else '')+stub+helper+main)
    binary=root/'helper'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined',str(harness),'-o',str(binary)],check=True)
    result=subprocess.run([str(binary)],check=True,capture_output=True).stdout.splitlines()
    report=decode_reports(result[0])[0]
    assert report['report_version']==5 and report['firmware_status']==0x8000000000000021
    assert report['bar1']==0x89abcdef and report['ltssm_states_seen']==[0,1,32]
    assert report['pipe_hz']==125000000 and report['pipe_frequency_measured']
    assert base64.b64decode(result[1],validate=True)==bytes(i^0xa5 for i in range(168))
    prepared=decode_reports(result[-1])[0]
    assert prepared['xnu_link_pending'] and not prepared['endpoint_identified']
    if snapshot_mode:
        assert not decode_reports(result[2]),'Snapshot must never be interpreted as a runtime handoff'
        log=root/'snapshot.log';log.write_bytes(result[2]+b'\n'+result[2]+b'\n')
        decoded=root/'snapshot.json'
        subprocess.run([sys.executable,str(ROOT/'scripts/decode-ios16-pcie-snapshot.py'),
                        str(log),'--out',str(decoded)],check=True,capture_output=True)
        import json
        snapshots=json.loads(decoded.read_text())['snapshots']
        assert len(snapshots)==1 and len(snapshots[0]['registers'])==39
        assert snapshots[0]['error']==7
        assert not decode_reports(result[3])
        log.write_bytes(result[3]+b'\n')
        decoded2=root/'trace-v2.json'
        subprocess.run([sys.executable,str(ROOT/'scripts/decode-ios16-pcie-snapshot.py'),
                        str(log),'--out',str(decoded2)],check=True,capture_output=True)
        assert json.loads(decoded2.read_text())['snapshots'][0]['format']==2
        print('PCIe snapshot: bounded handoff, missing protocol fallback, distinct prefix and replay decoding passed')
    print('Wi-Fi handoff: 609 capacity cases, no writes before reservation, ABI round-trip and idempotence passed')
