"""Compile and exercise the real UEFI hall adapters with fake native firmware."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / 'uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe'

UEFI = r'''
#ifndef MOCK_UEFI_H
#define MOCK_UEFI_H
#include <stdint.h>
#include <stddef.h>
typedef uint8_t BOOLEAN;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef uintptr_t UINTN;
typedef uint64_t EFI_STATUS;
typedef void VOID;
typedef char CHAR8;
typedef void *EFI_EVENT;
typedef struct { uint32_t Data1; uint16_t Data2,Data3;uint8_t Data4[8]; } EFI_GUID;
#define STATIC static
#define CONST const
#define EFIAPI
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS UINT64_C(0)
#define EFI_UNSUPPORTED UINT64_C(0x8000000000000003)
#define EFI_NOT_READY UINT64_C(0x8000000000000006)
#define EFI_DEVICE_ERROR UINT64_C(0x8000000000000007)
#define EFI_ERROR(s) (((s)>>63)!=0)
#define EVT_NOTIFY_SIGNAL 1
#define TPL_CALLBACK 8
#define EFI_VARIABLE_BOOTSERVICE_ACCESS 2
#define EFI_VARIABLE_RUNTIME_ACCESS 4
#define DEBUG_ERROR 1
#define DEBUG(x) ((void)0)
typedef void (*NOTIFY)(EFI_EVENT,void *);
typedef struct {
 EFI_STATUS (*LocateProtocol)(EFI_GUID *,void *,void **);
 EFI_STATUS (*CloseEvent)(EFI_EVENT);
 EFI_STATUS (*CreateEvent)(UINT32,UINTN,NOTIFY,void *,EFI_EVENT *);
 EFI_STATUS (*RegisterProtocolNotify)(EFI_GUID *,EFI_EVENT,void **);
} MOCK_BOOT;
typedef struct { EFI_STATUS (*SetVariable)(const void *,EFI_GUID *,UINT32,UINTN,void *); } MOCK_RUNTIME;
extern MOCK_BOOT *gBS;
extern MOCK_RUNTIME *gRT;
#endif
'''
CLOCK = r'''
#include <Uefi.h>
typedef struct CLOCK EFI_CLOCK_PROTOCOL;
struct CLOCK {
 UINT64 Version;
 EFI_STATUS (*GetClockID)(EFI_CLOCK_PROTOCOL *,const CHAR8 *,UINTN *);
 EFI_STATUS (*EnableClock)(EFI_CLOCK_PROTOCOL *,UINTN);
 void *DisableClock,*IsClockEnabled;
 EFI_STATUS (*IsClockOn)(EFI_CLOCK_PROTOCOL *,UINTN,BOOLEAN *);
 EFI_STATUS (*SetClockFreqHz)(EFI_CLOCK_PROTOCOL *,UINTN,UINT32,unsigned,UINT32 *);
 EFI_STATUS (*GetClockFreqHz)(EFI_CLOCK_PROTOCOL *,UINTN,UINT32 *);
};
#define EFI_CLOCK_FREQUENCY_HZ_EXACT 3
extern EFI_GUID gEfiClockProtocolGuid;
'''
FIXTURE = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "HallPower.c"
#include "HallClock.c"
static HALL_RPMH rpmh;
static HALL_CMDDB db;
static EFI_CLOCK_PROTOCOL clocks;
EFI_GUID gEfiClockProtocolGuid={0x9876,0,0,{0}};
static unsigned create_count,issue_count,wait_count,id_count,rate_count,get_count,enable_count,on_count;
static unsigned bad_slave,no_providers;
static UINT32 power_report[4],clock_report[4];
static EFI_STATUS locate(EFI_GUID *guid,void *registration,void **out)
{
 (void)registration;
 if(no_providers)return EFI_NOT_READY;
 if(guid->Data1==mHallRpmhGuid.Data1)*out=&rpmh;
 else if(guid->Data1==mHallCmdGuid.Data1)*out=&db;
 else {assert(guid->Data1==gEfiClockProtocolGuid.Data1);*out=&clocks;}
 return 0;
}
static EFI_STATUS close_event(EFI_EVENT event){assert(event);return 0;}
static EFI_STATUS create_event(UINT32 type,UINTN tpl,NOTIFY fn,void *ctx,EFI_EVENT *event)
{(void)ctx;assert(type==EVT_NOTIFY_SIGNAL && tpl==TPL_CALLBACK && fn);*event=(void *)1;return 0;}
static EFI_STATUS register_notify(EFI_GUID *guid,EFI_EVENT event,void **registration)
{assert(guid && event);*registration=(void *)2;return 0;}
static EFI_STATUS set_variable(const void *name,EFI_GUID *guid,UINT32 attrs,UINTN bytes,void *data)
{
 const uint16_t *n=name;
 assert(guid->Data1==mHallPowerStatusGuid.Data1 && attrs==6 && bytes==16);
 assert(n[10]=='P' || n[10]=='C');memcpy(n[10]=='P' ? power_report : clock_report,data,16);return 0;
}
static MOCK_BOOT bs={locate,close_event,create_event,register_notify};
static MOCK_RUNTIME rt={set_variable};
MOCK_BOOT *gBS=&bs;
MOCK_RUNTIME *gRT=&rt;
static EFI_STATUS query_address(HALL_CMDDB *this,const CHAR8 *name,UINT32 *value)
{assert(this==&db && !strcmp(name,"ldob1"));*value=0x40020;return 0;}
static EFI_STATUS query_slave(HALL_CMDDB *this,const CHAR8 *name,UINT32 *value)
{assert(this==&db && !strcmp(name,"ldob1"));*value=bad_slave ? 3 : 4;return 0;}
static EFI_STATUS valid(UINT32 drv,BOOLEAN *yes){assert(drv==2);*yes=1;return 0;}
static EFI_STATUS create(UINT32 drv,const CHAR8 *name,void **client)
{assert(drv==2 && !strcmp(name,"giulia-hall-vcc"));create_count++;*client=&rpmh;return 0;}
static EFI_STATUS issue(void *client,UINT32 set,BOOLEAN completion,UINT32 address,UINT32 value,UINT32 *request)
{
 static const UINT32 offsets[]={0,8,4},values[]={1800,7,1};
 assert(client==&rpmh && set==0 && completion && issue_count<3);
 assert(address==0x40020+offsets[issue_count] && value==values[issue_count]);
 *request=++issue_count;return 0;
}
static EFI_STATUS wait_request(void *client,UINT32 request)
{assert(client==&rpmh && request==issue_count);wait_count++;return 0;}
static EFI_STATUS get_id(EFI_CLOCK_PROTOCOL *this,const CHAR8 *name,UINTN *id)
{assert(this==&clocks && id_count<3 && !strcmp(name,mHallClockNames[id_count]));*id=100+id_count++;return 0;}
static EFI_STATUS rate(EFI_CLOCK_PROTOCOL *this,UINTN id,UINT32 value,unsigned match,UINT32 *actual)
{assert(this==&clocks && id==102 && value==19200000 && match==3 && id_count==3);rate_count++;*actual=value;return 0;}
static EFI_STATUS get_rate(EFI_CLOCK_PROTOCOL *this,UINTN id,UINT32 *actual)
{assert(this==&clocks && id==102 && rate_count==1);get_count++;*actual=19200000;return 0;}
static EFI_STATUS enable(EFI_CLOCK_PROTOCOL *this,UINTN id)
{assert(this==&clocks && get_count==1 && id==100+enable_count);enable_count++;return 0;}
static EFI_STATUS on(EFI_CLOCK_PROTOCOL *this,UINTN id,BOOLEAN *yes)
{assert(this==&clocks && id==100+on_count && enable_count==on_count+1);on_count++;*yes=1;return 0;}
static void reset(void)
{
 memset(&rpmh,0,sizeof(rpmh));memset(&db,0,sizeof(db));memset(&clocks,0,sizeof(clocks));
 memset(&mHallPowerResult,0,sizeof(mHallPowerResult));memset(&mHallClockResult,0,sizeof(mHallClockResult));
 mHallPowerAttempted=mHallPowerScheduled=mHallClockAttempted=mHallClockScheduled=0;
 mHallPowerClient=NULL;mHallPowerEvent[0]=mHallPowerEvent[1]=mHallClockEvent=NULL;
 create_count=issue_count=wait_count=id_count=rate_count=get_count=enable_count=on_count=bad_slave=no_providers=0;
 rpmh.Version=0x10001;db.Version=0x10000;clocks.Version=0x1000a;
 rpmh.Methods[1]=(void *)valid;rpmh.Methods[5]=(void *)create;
 rpmh.Methods[18]=(void *)issue;rpmh.Methods[11]=(void *)wait_request;
 db.Methods[1]=(void *)query_address;db.Methods[7]=(void *)query_slave;
 clocks.GetClockID=get_id;clocks.SetClockFreqHz=rate;clocks.GetClockFreqHz=get_rate;
 clocks.EnableClock=enable;clocks.IsClockOn=on;
}
int main(void)
{
 reset();GiuliaHallPowerSchedule();GiuliaHallClockSchedule();
 assert(GiuliaHallPowerIsReady() && GiuliaHallClockIsReady());
 assert(create_count==1 && issue_count==3 && wait_count==3 && enable_count==3 && on_count==3);
 assert(power_report[0]==10 && clock_report[0]==10 && !power_report[2] && !clock_report[2]);
 GiuliaHallPowerSchedule();GiuliaHallClockSchedule();HallPowerNotify(NULL,NULL);HallClockNotify(NULL,NULL);
 assert(issue_count==3 && enable_count==3);
 reset();bad_slave=1;GiuliaHallPowerSchedule();assert(!GiuliaHallPowerIsReady() && !create_count && !issue_count);
 reset();rpmh.Version=0x10002;GiuliaHallPowerSchedule();assert(!issue_count && !create_count);
 reset();db.Methods[7]=NULL;GiuliaHallPowerSchedule();assert(!issue_count && !create_count);
 reset();clocks.Version=0x10009;GiuliaHallClockSchedule();assert(!id_count && !rate_count && !enable_count);
 reset();clocks.GetClockFreqHz=NULL;GiuliaHallClockSchedule();assert(!id_count && !rate_count && !enable_count);
 reset();no_providers=1;GiuliaHallPowerSchedule();GiuliaHallClockSchedule();assert(!mHallPowerAttempted && !mHallClockAttempted);
 no_providers=0;HallPowerNotify(NULL,NULL);HallClockNotify(NULL,NULL);assert(GiuliaHallPowerIsReady() && GiuliaHallClockIsReady());
 puts("Hall real adapters: pinned ABI, exact resource/names, native slots, delayed protocols, diagnostics and one-shot handoff passed");return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='giulia-hall-uefi-') as directory:
    temp = Path(directory)
    (temp / 'Library').mkdir()
    (temp / 'Protocol').mkdir()
    (temp / 'Uefi.h').write_text(UEFI)
    for name in ('DebugLib.h', 'UefiBootServicesTableLib.h', 'UefiRuntimeServicesTableLib.h'):
        (temp / 'Library' / name).write_text('#include <Uefi.h>\n')
    (temp / 'Protocol/EFIClock.h').write_text(CLOCK)
    (temp / 'PayloadIdentity.h').write_text('#define GIULIA_HALL_PREPARE 1\n')
    fixture = temp / 'fixture.c'
    fixture.write_text(FIXTURE)
    binary = temp / 'test'
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fshort-wchar',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I', str(temp), '-I', str(DRIVER), str(fixture), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    (temp / 'PayloadIdentity.h').write_text('#define GIULIA_HALL_PREPARE 0\n')
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fshort-wchar',
                    '-fsyntax-only', '-I', str(temp), '-I', str(DRIVER),
                    str(DRIVER / 'HallPower.c'), str(DRIVER / 'HallClock.c')], check=True)
