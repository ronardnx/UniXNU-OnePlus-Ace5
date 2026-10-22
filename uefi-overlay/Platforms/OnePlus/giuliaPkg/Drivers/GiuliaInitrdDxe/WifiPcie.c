/** @file
  Opt-in WCN7850 power and PCIe0 enumeration through the board's firmware APIs.
  Called explicitly by DarwinBoot after DXE dispatch, before ExitBootServices.
  Register/protocol provenance: hardware/sm8650-wifi.json.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <PiDxe.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/EFIClock.h>
#include "PayloadIdentity.h"

VOID GiuliaWifiInstall (VOID);

#if defined(GIULIA_WIFI_PCIE) && GIULIA_WIFI_PCIE
#include "WifiPciePolicy.h"
#include "WifiPciePhy.h"
#include "WifiPowerCache.h"
typedef struct { UINT64 Version; VOID *Methods[24]; } GW_RPMH;
typedef struct { UINT64 Version; VOID *Methods[8]; } GW_CMD;
typedef EFI_STATUS (EFIAPI *GW_QUERY)(GW_CMD *, CONST CHAR8 *, UINT32 *);
typedef EFI_STATUS (EFIAPI *GW_VALID)(UINT32, BOOLEAN *);
typedef EFI_STATUS (EFIAPI *GW_CREATE)(UINT32, CONST CHAR8 *, VOID **);
typedef EFI_STATUS (EFIAPI *GW_ISSUE)(VOID *, UINT32, BOOLEAN, UINT32, UINT32, UINT32 *);
typedef EFI_STATUS (EFIAPI *GW_WAIT)(VOID *, UINT32);
typedef EFI_STATUS (EFIAPI *GW_FIND_VOTE)(UINT32,GW_COMMAND **);
typedef struct { UINT64 Version; EFI_STATUS (EFIAPI *Prepare)(struct gw_report *,UINTN); } GW_PROTOCOL;
STATIC EFI_GUID mWifiGuid={0xf5b737f6,0x3b39,0x438d,{0x84,0xc0,0x17,0xcb,0x11,0x07,0x00,0x01}};
STATIC EFI_GUID mRpmhGuid={0xe1d0b573,0xf29f,0x495d,{0x93,0x52,0xca,0x0d,0xd4,0x2a,0x0a,0x74}};
STATIC EFI_GUID mCmdGuid={0x1c34f691,0xd33d,0x4e14,{0xb8,0xf8,0x32,0xc6,0xc0,0x29,0xe9,0x5b}};
STATIC GW_RPMH *mRpmh;
STATIC GW_CMD *mDb;
STATIC EFI_CLOCK_PROTOCOL *mClock;
STATIC VOID *mClient;
STATIC BOOLEAN mAttempted;
STATIC struct gw_report mReport;
STATIC UINT32 mAddresses[GW_RAILS];
STATIC UINT32 mOldVotes[GW_RAILS][3]; /* voltage, mode, enable */
STATIC BOOLEAN mOldValid[GW_RAILS][3];
STATIC UINTN mClocks[GW_CLOCKS_COUNT],mDomains[GW_DOMAINS_COUNT];
STATIC UINT32 mOldFreq[2];
STATIC UINT32 mOldPipeSource;
STATIC BOOLEAN mPipeSourceChanged;
/* Seven endpoint rails, then host PHY analog/PLL. The host rails are pinned
   to the captured board's L1I (880..912mV) and L3I (1200mV) constraints;
   SM8650's upstream PCIe0 PHY maps these to vdda-phy and vdda-pll. */
STATIC CONST CHAR8 *mRailNames[GW_RAILS]={"ldob15","ldoc3","smpi4","smpc2","smpc3","smpc6","smpc1","ldoi1","ldoi3"};
STATIC CONST UINT32 mMillivolts[GW_RAILS]={1800,1200,940,976,916,1864,1316,880,1200};
STATIC CONST UINT32 mMaximumMv[GW_RAILS]={1800,1200,1004,1036,1064,2000,1348,912,1200};
STATIC CONST CHAR8 *mClockNames[GW_CLOCKS_COUNT]={
  "gcc_aggre_noc_pcie_axi_clk","gcc_cnoc_pcie_sf_axi_clk","gcc_ddrss_pcie_sf_qtb_clk",
  "gcc_pcie_0_cfg_ahb_clk","gcc_pcie_0_mstr_axi_clk","gcc_pcie_0_slv_axi_clk",
  "gcc_pcie_0_slv_q2a_axi_clk","gcc_pcie_0_aux_clk","gcc_pcie_0_phy_rchng_clk",
  "gcc_cfg_noc_pcie_anoc_ahb_clk","tcsr_pcie_0_clkref_en","gcc_pcie_0_pipe_clk"
};
STATIC CONST UINTN mBases[]={0,0x100000,0x1c00000,0x1c06000,0x60000000,0x60001000,0x60100000,0x1ff1100,0x162000};
STATIC CONST UINTN mGpioBases[]={0xf110000,0xf15e000,0xf15f000,0xf160000,0xf111000,0xf112000,0xf113000};

STATIC unsigned Read (VOID *Context,unsigned Region,unsigned Offset)
{
  if(Region==GW_GPIO) return MmioRead32(mGpioBases[Offset/0x1000]+Offset%0x1000);
  return MmioRead32(mBases[Region]+Offset);
}
STATIC VOID Write (VOID *Context,unsigned Region,unsigned Offset,unsigned Value)
{
  if(Region==GW_GPIO) MmioWrite32(mGpioBases[Offset/0x1000]+Offset%0x1000,Value);
  else MmioWrite32(mBases[Region]+Offset,Value);
}
STATIC VOID WaitUs (VOID *Context,unsigned Us) { gBS->Stall(Us); }
STATIC VOID Step (VOID *Context,struct gw_report *R)
{
  DEBUG((DEBUG_ERROR,"GIULIA WIFI: stage=%u detail=%u err=%u status=%lx rails=%x clocks=%x phy=%x link=%x id=%x\n",
    R->stage,R->detail,R->error,R->firmware_status,R->rails,R->clocks,R->phy,R->link,R->device));
}

STATIC EFI_STATUS Vote (UINT32 Address,UINT32 Value)
{
  UINT32 Request=0;
  EFI_STATUS Status=((GW_ISSUE)mRpmh->Methods[18])(mClient,0,TRUE,Address,Value,&Request);
  if(!EFI_ERROR(Status) && Request) Status=((GW_WAIT)mRpmh->Methods[11])(mClient,Request);
  return Status;
}
STATIC VOID ReleaseRail (unsigned Index)
{
  EFI_STATUS Status;
  UINTN J;
  CONST UINT32 Offsets[3]={0,8,4};
  /* Restore the observed floor; do not assume RPMh aggregates software clients. */
  for(J=3;J>0;J--) {
    if(!mOldValid[Index][J-1]) {
      /* An absent firmware cache is unknown, never an inferred zero vote. */
      mReport.retained_rails|=1u<<Index;
      if(!EFI_ERROR(mReport.cleanup_status)) mReport.cleanup_status=EFI_NOT_READY;
      continue;
    }
    Status=Vote(mAddresses[Index]+Offsets[J-1],mOldVotes[Index][J-1]);
    if(EFI_ERROR(Status)) mReport.cleanup_status=Status;
  }
}
STATIC int Resource (VOID *Context,unsigned Kind,unsigned Index,int Enable)
{
  EFI_STATUS Status=EFI_SUCCESS;
  UINT32 Actual=0,Hz;
  BOOLEAN On=FALSE;
  if(Kind==GW_RAIL) {
    if(!Enable) { ReleaseRail(Index); return 0; }
    Status=Vote(mAddresses[Index],MAX(mMillivolts[Index],mOldVotes[Index][0]));
    if(!EFI_ERROR(Status)) Status=Vote(mAddresses[Index]+8,7); /* normal/high-power mode */
    if(!EFI_ERROR(Status)) Status=Vote(mAddresses[Index]+4,1);
    if(EFI_ERROR(Status)) ReleaseRail(Index);
  } else if(Kind==GW_DOMAIN) {
    Status=Enable ? mClock->EnableClockPowerDomain(mClock,mDomains[Index]) :
                    mClock->DisableClockPowerDomain(mClock,mDomains[Index]);
  } else if(Kind==GW_CLOCK) {
    if(!Enable) {
      Status=mClock->DisableClock(mClock,mClocks[Index]);
      if(Index==GW_PIPE_CLOCK && mPipeSourceChanged) {
        EFI_STATUS Restore=mClock->SelectExternalSource(mClock,mClocks[Index],0,mOldPipeSource,2,0,0,0);
        if(EFI_ERROR(Restore)) mReport.cleanup_status=Restore;
        mPipeSourceChanged=FALSE;
      }
      if((Index==7 || Index==8) && mOldFreq[Index-7])
        (VOID)mClock->SetClockFreqHz(mClock,mClocks[Index],mOldFreq[Index-7],EFI_CLOCK_FREQUENCY_HZ_EXACT,&Actual);
      if(EFI_ERROR(Status)) mReport.cleanup_status=Status;
      return EFI_ERROR(Status);
    }
    if(Index==GW_PIPE_CLOCK) {
      /* Vendor GCC: 0x6b070[1:0], PHY PIPE=0, safe XO=2. EnableClock alone
         does not select the external PHY source. The native clock node has
         this mux and its 125 MHz voltage table; this mux has no divider. */
      UINT32 Before=MmioRead32(0x16b070),After;
      if(Before==MAX_UINT32 || ((Before&3)!=0 && (Before&3)!=2)) {
        mReport.firmware_status=EFI_COMPROMISED_DATA;
        return 1;
      }
      mOldPipeSource=Before&3;
      Status=mClock->SelectExternalSource(mClock,mClocks[Index],125000000,0,2,0,0,0);
      mPipeSourceChanged=TRUE;
      After=MmioRead32(0x16b070);
      mReport.pipe_source=(mOldPipeSource<<16)|(After&0xffff);
      if(!EFI_ERROR(Status) && (After==MAX_UINT32 || (After&3)!=0)) Status=EFI_DEVICE_ERROR;
    }
    if(Index==7 || Index==8) {
      Hz=Index==7?19200000:100000000;
      (VOID)mClock->GetClockFreqHz(mClock,mClocks[Index],&mOldFreq[Index-7]);
      Status=mClock->SetClockFreqHz(mClock,mClocks[Index],Hz,EFI_CLOCK_FREQUENCY_HZ_EXACT,&Actual);
      if(!EFI_ERROR(Status) && Actual!=Hz) Status=EFI_UNSUPPORTED;
    }
    if(!EFI_ERROR(Status)) {
      Status=mClock->EnableClock(mClock,mClocks[Index]);
      if(!EFI_ERROR(Status)) {
        /* PIPE and AXI are hardware gated before training; CFG/AUX must run. */
        Status=(Index==3 || Index==7) ? mClock->IsClockOn(mClock,mClocks[Index],&On) :
                                     mClock->IsClockEnabled(mClock,mClocks[Index],&On);
        if(!EFI_ERROR(Status) && !On) Status=EFI_NOT_READY;
        if(EFI_ERROR(Status)) (VOID)mClock->DisableClock(mClock,mClocks[Index]);
      }
    }
    if(EFI_ERROR(Status) && (Index==7 || Index==8) && mOldFreq[Index-7])
      (VOID)mClock->SetClockFreqHz(mClock,mClocks[Index],mOldFreq[Index-7],EFI_CLOCK_FREQUENCY_HZ_EXACT,&Actual);
    if(EFI_ERROR(Status) && Index==GW_PIPE_CLOCK && mPipeSourceChanged) {
      EFI_STATUS Restore=mClock->SelectExternalSource(mClock,mClocks[Index],0,mOldPipeSource,2,0,0,0);
      if(EFI_ERROR(Restore)) mReport.cleanup_status=Restore;
      mPipeSourceChanged=FALSE;
    }
  } else Status=EFI_INVALID_PARAMETER;
  if(Enable && EFI_ERROR(Status)) mReport.firmware_status=Status;
  if(!Enable && EFI_ERROR(Status)) mReport.cleanup_status=Status;
  return EFI_ERROR(Status);
}

STATIC EFI_STATUS MapMmio (UINT64 Base,UINT64 Length)
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Desc;
  EFI_STATUS Status=gDS->GetMemorySpaceDescriptor(Base,&Desc);
  if(EFI_ERROR(Status)) return Status;
  if(Base<Desc.BaseAddress || Length>Desc.Length || Base-Desc.BaseAddress>Desc.Length-Length)
    return EFI_UNSUPPORTED;
  if(Desc.GcdMemoryType==EfiGcdMemoryTypeNonExistent) {
    Status=gDS->AddMemorySpace(EfiGcdMemoryTypeMemoryMappedIo,Base,Length,EFI_MEMORY_UC);
    if(EFI_ERROR(Status)) return Status;
  } else if(Desc.GcdMemoryType!=EfiGcdMemoryTypeMemoryMappedIo) return EFI_ACCESS_DENIED;
  return gDS->SetMemorySpaceAttributes(Base,Length,EFI_MEMORY_UC);
}

STATIC EFI_STATUS PrepareResources (VOID)
{
  EFI_STATUS Status;
  UINT32 Slave;
  UINTN I,J;
  BOOLEAN Valid=FALSE;
  CONST CHAR8 *Domains[2]={"gcc_pcie_0_gdsc","gcc_pcie_0_phy_gdsc"};
  CONST UINT64 Maps[][2]={{0x16b000,0x1000},{0x16c000,0x1000},{0x1c00000,0x3000},{0x1c06000,0x2000},
    {0x60000000,0x2000},{0x60100000,0x1000},{0x1ff1000,0x1000},{0x162000,0x1000}};
  mReport.stage=GW_ABI;
  Status=gBS->LocateProtocol(&mRpmhGuid,NULL,(VOID **)&mRpmh); if(EFI_ERROR(Status)) return Status;
  Status=gBS->LocateProtocol(&mCmdGuid,NULL,(VOID **)&mDb); if(EFI_ERROR(Status)) return Status;
  Status=gBS->LocateProtocol(&gEfiClockProtocolGuid,NULL,(VOID **)&mClock); if(EFI_ERROR(Status)) return Status;
  mReport.clock_version=mClock->Version;
  /* The pinned giulia ClockDxe is 1.0.a; its first 16 slots retain this ABI. */
  if(mRpmh->Version!=0x10001 || mDb->Version!=0x10000 || mClock->Version!=0x1000a ||
    !mRpmh->Methods[1] || !mRpmh->Methods[5] || !mRpmh->Methods[9] || !mRpmh->Methods[11] || !mRpmh->Methods[18] ||
    !mDb->Methods[1] || !mDb->Methods[7] || !mClock->GetClockPowerDomainID || !mClock->EnableClockPowerDomain ||
    !mClock->DisableClockPowerDomain || !mClock->GetClockID || !mClock->EnableClock || !mClock->DisableClock ||
    !mClock->GetClockFreqHz || !mClock->SetClockFreqHz || !mClock->IsClockOn || !mClock->IsClockEnabled ||
    !mClock->SelectExternalSource)
    return EFI_UNSUPPORTED;
  mReport.stage=GW_RESOLVE;
  /* Validate every resource ID before any power, clock, or GPIO mutation. */
  for(I=0;I<GW_RAILS;I++) {
    mReport.detail=(UINT32)I;
    Status=((GW_QUERY)mDb->Methods[1])(mDb,mRailNames[I],&mAddresses[I]); if(EFI_ERROR(Status)) return Status;
    Status=((GW_QUERY)mDb->Methods[7])(mDb,mRailNames[I],&Slave); if(EFI_ERROR(Status)) return Status;
    if(Slave!=4 || (mAddresses[I]>>16)!=4 || (mAddresses[I]&15)) return EFI_UNSUPPORTED;
    for(J=0;J<I;J++) if(mAddresses[I]==mAddresses[J]) return EFI_COMPROMISED_DATA;
  }
  for(I=0;I<GW_CLOCKS_COUNT;I++) {
    mReport.detail=0x100+(UINT32)I;
    Status=mClock->GetClockID(mClock,mClockNames[I],&mClocks[I]); if(EFI_ERROR(Status)) return Status;
  }
  for(I=0;I<GW_DOMAINS_COUNT;I++) {
    mReport.detail=0x200+(UINT32)I;
    Status=mClock->GetClockPowerDomainID(mClock,Domains[I],&mDomains[I]); if(EFI_ERROR(Status)) return Status;
  }
  Status=((GW_VALID)mRpmh->Methods[1])(2,&Valid); if(EFI_ERROR(Status) || !Valid) return EFI_UNSUPPORTED;
  mReport.stage=GW_MAP;
  for(I=0;I<ARRAY_SIZE(Maps);I++) {
    mReport.detail=(UINT32)I; Status=MapMmio(Maps[I][0],Maps[I][1]); if(EFI_ERROR(Status)) return Status;
  }
  Status=((GW_CREATE)mRpmh->Methods[5])(2,"giulia-wifi",&mClient);
  if(!EFI_ERROR(Status) && mClient==NULL) Status=EFI_OUT_OF_RESOURCES;
  if(EFI_ERROR(Status)) return Status;
  /* The hardware-read API (slot 17) is an EFI_UNSUPPORTED stub in the pinned
     binary. Snapshot valid, idle HLOS cache entries through public slot 9.
     There is no software-client aggregation assumption. Unknown prior votes
     are retained, never lowered to an invented zero during failure cleanup. */
  mReport.stage=GW_RESOLVE;
  for(I=0;I<GW_RAILS;I++) {
    CONST UINT32 Offsets[3]={0,8,4};
    for(J=0;J<3;J++) {
      GW_COMMAND *Command=NULL;
      int ValidVote=0,CacheStatus;
      mReport.detail=0x300+(UINT32)(I*3+J);
      Status=((GW_FIND_VOTE)mRpmh->Methods[9])(mAddresses[I]+Offsets[J],&Command);
      if(EFI_ERROR(Status)) return Status;
      CacheStatus=gw_cached_vote(Command,mAddresses[I]+Offsets[J],&mOldVotes[I][J],&ValidVote);
      if(CacheStatus==GW_CACHE_BAD) return EFI_COMPROMISED_DATA;
      if(CacheStatus==GW_CACHE_BUSY) return EFI_NOT_READY;
      mOldValid[I][J]=(BOOLEAN)ValidVote;
    }
    if(mOldVotes[I][0]>mMaximumMv[I] || mOldVotes[I][1]>7 || mOldVotes[I][2]>1)
      return EFI_COMPROMISED_DATA;
  }
  return Status;
}

STATIC EFI_STATUS EFIAPI Prepare (struct gw_report *Result,UINTN Size)
{
  EFI_STATUS Status;
  struct gw_bus Bus={NULL,Read,Write,WaitUs,Resource,Step};
  STATIC_ASSERT(sizeof(GW_VOTE)==12,"RPMh vote ABI");
  STATIC_ASSERT(sizeof(GW_VOTER)==72,"RPMh voter ABI");
  STATIC_ASSERT(sizeof(GW_COMMAND)==24,"RPMh resource ABI");
  if(Result==NULL || Size!=sizeof(*Result)) return EFI_INVALID_PARAMETER;
  if(!mAttempted) {
    mAttempted=TRUE;
    ZeroMem(&mReport,sizeof(mReport)); mReport.version=5; mReport.stage=GW_START;
    Status=PrepareResources();
    if(EFI_ERROR(Status)) { mReport.error=GW_RESOURCE_ERROR; mReport.firmware_status=Status; }
    else (VOID)gw_bringup(&Bus,&mReport,gw_phy_sequence,ARRAY_SIZE(gw_phy_sequence));
    Step(NULL,&mReport);
  }
  CopyMem(Result,&mReport,sizeof(mReport));
  return mReport.error ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
STATIC GW_PROTOCOL mProtocol={5,Prepare};
#endif

VOID GiuliaWifiInstall (VOID)
{
#if defined(GIULIA_WIFI_PCIE) && GIULIA_WIFI_PCIE
  EFI_HANDLE Handle=NULL;
  EFI_STATUS Status=gBS->InstallMultipleProtocolInterfaces(&Handle,&mWifiGuid,&mProtocol,NULL);
  DEBUG((DEBUG_ERROR,"GIULIA WIFI: prepare protocol v5 installed: %r\n",Status));
#endif
}
