/** @file
  Ace 5 touch VDD: native RPMh ACTIVE vote for the DT's ldob4 at 1800 mV.
  This small interoperability adapter uses version-checked firmware interfaces;
  it contains no raw PMIC/RSC writes and never addresses another resource.
  ABI provenance: hardware/sm8650-touch.json. SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "PayloadIdentity.h"

VOID GiuliaTouchPowerSchedule (VOID);

#if defined(GIULIA_TOUCH_POWER) && GIULIA_TOUCH_POWER
typedef struct { UINT64 Version; VOID *Methods[24]; } GIULIA_RPMH_INTERFACE;
typedef struct { UINT64 Version; VOID *Methods[8]; } GIULIA_CMD_INTERFACE;
typedef EFI_STATUS (EFIAPI *GIULIA_QUERY)(GIULIA_CMD_INTERFACE *, CONST CHAR8 *, UINT32 *);
typedef EFI_STATUS (EFIAPI *GIULIA_DRV_VALID)(UINT32, BOOLEAN *);
typedef EFI_STATUS (EFIAPI *GIULIA_CREATE)(UINT32, CONST CHAR8 *, VOID **);
typedef EFI_STATUS (EFIAPI *GIULIA_ISSUE)(VOID *, UINT32, BOOLEAN, UINT32, UINT32, UINT32 *);
typedef EFI_STATUS (EFIAPI *GIULIA_WAIT)(VOID *, UINT32);
STATIC EFI_GUID mRpmhGuid = {0xe1d0b573,0xf29f,0x495d,{0x93,0x52,0xca,0x0d,0xd4,0x2a,0x0a,0x74}};
STATIC EFI_GUID mCmdGuid = {0x1c34f691,0xd33d,0x4e14,{0xb8,0xf8,0x32,0xc6,0xc0,0x29,0xe9,0x5b}};
STATIC EFI_EVENT mPowerEvent[2];
STATIC VOID *mPowerRegistration[2];
STATIC VOID *mPowerClient;
STATIC BOOLEAN mPowerAttempted;

STATIC VOID EFIAPI TouchPowerReady (EFI_EVENT Event, VOID *Context)
{
  GIULIA_RPMH_INTERFACE *Rpmh;
  GIULIA_CMD_INTERFACE *Db;
  EFI_STATUS Status;
  UINT32 Address = 0, Slave = 0, Request = 0;
  BOOLEAN Valid = FALSE;
  UINTN Index;
  CONST UINT32 Offsets[3] = {0,8,4};
  CONST UINT32 Values[3] = {1800,7,1}; /* mV, PMIC4 LDO HPM, enable */
  if (mPowerAttempted) { return; }
  Status = gBS->LocateProtocol (&mRpmhGuid, NULL, (VOID **)&Rpmh);
  if (EFI_ERROR (Status)) { return; }
  Status = gBS->LocateProtocol (&mCmdGuid, NULL, (VOID **)&Db);
  if (EFI_ERROR (Status)) { return; }
  mPowerAttempted = TRUE;
  for (Index = 0; Index < 2; Index++) {
    if (mPowerEvent[Index] != NULL) { gBS->CloseEvent (mPowerEvent[Index]); mPowerEvent[Index] = NULL; }
  }
  DEBUG ((DEBUG_ERROR, "GIULIA TOUCH POWER: RPMh=%lx CmdDb=%lx\n", Rpmh->Version, Db->Version));
  if (Rpmh->Version != 0x10001 || Db->Version != 0x10000 ||
      Rpmh->Methods[1] == NULL || Rpmh->Methods[5] == NULL ||
      Rpmh->Methods[18] == NULL || Rpmh->Methods[23] == NULL ||
      Db->Methods[1] == NULL || Db->Methods[7] == NULL) { return; }
  Status = ((GIULIA_QUERY)Db->Methods[1])(Db,"ldob4",&Address);
  if (EFI_ERROR (Status)) { return; }
  Status = ((GIULIA_QUERY)Db->Methods[7])(Db,"ldob4",&Slave);
  DEBUG ((DEBUG_ERROR, "GIULIA TOUCH POWER: ldob4 address=%x slave=%u status=%r\n", Address, Slave, Status));
  if (EFI_ERROR (Status) || Slave != 4 || (Address >> 16) != 4 || (Address & 15) != 0) { return; }
  /* Native mapping enum 2 = HLOS; never create a TZ/HYP client. */
  Status = ((GIULIA_DRV_VALID)Rpmh->Methods[1])(2,&Valid);
  if (EFI_ERROR (Status) || !Valid) { return; }
  Status = ((GIULIA_CREATE)Rpmh->Methods[5])(2,"giulia-touch-vdd",&mPowerClient);
  if (EFI_ERROR (Status) || mPowerClient == NULL) { return; }
  for (Index = 0; Index < 3; Index++) {
    Request = 0;
    Status = ((GIULIA_ISSUE)Rpmh->Methods[18])(mPowerClient,0,TRUE,
        Address + Offsets[Index],Values[Index],&Request);
    if (!EFI_ERROR (Status) && Request != 0) {
      Status = ((GIULIA_WAIT)Rpmh->Methods[23])(mPowerClient,Request);
    }
    DEBUG ((DEBUG_ERROR, "GIULIA TOUCH POWER: ldob4+%u=%u req=%u status=%r\n",
        Offsets[Index],Values[Index],Request,Status));
    if (EFI_ERROR (Status)) { return; }
  }
  /* Retain this client and its ACTIVE vote for the kernel handoff. */
  DEBUG ((DEBUG_ERROR, "GIULIA TOUCH POWER: ldob4 1800 mV ACTIVE vote complete\n"));
}
#endif

VOID GiuliaTouchPowerSchedule (VOID)
{
#if defined(GIULIA_TOUCH_POWER) && GIULIA_TOUCH_POWER
  EFI_STATUS Status;
  EFI_GUID *Guids[2] = {&mRpmhGuid,&mCmdGuid};
  UINTN Index;
  for (Index = 0; Index < 2; Index++) {
    Status = gBS->CreateEvent (EVT_NOTIFY_SIGNAL,TPL_CALLBACK,TouchPowerReady,NULL,&mPowerEvent[Index]);
    if (EFI_ERROR (Status)) { break; }
    Status = gBS->RegisterProtocolNotify (Guids[Index],mPowerEvent[Index],&mPowerRegistration[Index]);
    if (EFI_ERROR (Status)) {
      gBS->CloseEvent (mPowerEvent[Index]);mPowerEvent[Index] = NULL;break;
    }
  }
  TouchPowerReady (NULL,NULL);
#endif
}
