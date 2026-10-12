/* Native Clock/RPMh/ICB/SMMU adapter for the shared display cores.
 * Immutable target data supplies resource identities; providers own access.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "MdpExtendedNative.h"
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiLib.h>
STATIC UINT64 Base(CONST MDP_EXTENDED_NATIVE *N,CR_MDSS_EXT_WINDOW W) {
  CONST CR_MDSS_HW_PROFILE *H=N->Hardware;
  if(W==CrMdssExtDpu)return H->DpuBase;
  if(W==CrMdssExtDsi0||W==CrMdssExtDsi1)return H->DsiBase[W==CrMdssExtDsi1];
  return H->PhyBase[W==CrMdssExtPhy1];
}
STATIC UINTN Size(CONST MDP_EXTENDED_NATIVE *N,CR_MDSS_EXT_WINDOW W) {
  return W==CrMdssExtDpu?N->Hardware->DpuBytes:
    (W==CrMdssExtDsi0||W==CrMdssExtDsi1)?N->Hardware->DsiBytes:N->Hardware->PhyBytes;
}
STATIC BOOLEAN TargetValid(CONST CR_MDSS_TARGET *T,UINT32 Soc) {
  if(!T||T->Revision!=CR_MDSS_TARGET_REVISION||T->Size!=sizeof(*T)||T->Soc!=Soc||
    !T->SmmuBase||(T->SmmuBase&0xfff)||T->RequiresStoppedDisplay>1)return FALSE;
  for(UINT32 I=0;I<CR_MDSS_TARGET_STREAMS;++I)
    for(UINT32 J=0;J<I;++J)if(T->Streams[I]==T->Streams[J])return FALSE;
  for(UINT32 I=0;I<CR_MDSS_TARGET_RAILS;++I)
    if(!T->Rails[I].Name||!T->Rails[I].Name[0]||!T->Rails[I].Millivolts)return FALSE;
  for(UINT32 I=0;I<CR_MDSS_TARGET_PATHS;++I)
    if(!T->Paths[I].Provider||!T->Paths[I].Provider[0]||T->Paths[I].Source==T->Paths[I].Destination)return FALSE;
  for(UINT32 I=0;I<CR_MDSS_TARGET_CLOCKS;++I)
    if(!T->Clocks[I].Controller||!T->Clocks[I].Controller[0]||!T->Clocks[I].Name||!T->Clocks[I].Name[0])return FALSE;
  return TRUE;
}
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_NOT_READY;}
STATIC EFI_STATUS Level(VOID){return EfiGetCurrentTpl()<=TPL_CALLBACK?EFI_SUCCESS:EFI_ACCESS_DENIED;}
STATIC BOOLEAN HasController(MDP_EXTENDED_NATIVE *N,UINT32 I){return I>=N->Plan.Mode.FirstController&&
  I<N->Plan.Mode.FirstController+N->Plan.Mode.Controllers;}
STATIC EFI_STATUS Gcd(MDP_EXTENDED_NATIVE *N,UINT64 Base,UINTN Bytes,EFI_GCD_MEMORY_TYPE Type) {
  if(!Bytes||Base>MAX_UINT64-Bytes)return EFI_INVALID_PARAMETER;
  UINT64 At=Base,End=Base+Bytes;
  while(At<End){EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;
    EFI_STATUS S=Exact(N->Providers.Dxe->GetMemorySpaceDescriptor(At,&D));if(S!=EFI_SUCCESS)return S;
    if(D.GcdMemoryType!=Type||D.BaseAddress>At||!D.Length||D.BaseAddress>MAX_UINT64-D.Length||
       D.BaseAddress+D.Length<=At)return EFI_UNSUPPORTED;
    if(Type==EfiGcdMemoryTypeReserved&&(D.Attributes&(EFI_MEMORY_RP|EFI_MEMORY_WP|EFI_MEMORY_RO)))
      return EFI_ACCESS_DENIED;
    At=MIN(End,D.BaseAddress+D.Length);
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Current(MDP_EXTENDED_NATIVE *N) {
  if(N&&N->Frozen&&!N->FreezeAccess)return EFI_ACCESS_DENIED;
  EFI_STATUS S=(N&&N->FreezeAccess&&EfiGetCurrentTpl()<=TPL_NOTIFY)?EFI_SUCCESS:Level();
  if(S!=EFI_SUCCESS)return S;
  if(N&&N->Initialized&&N->Config.Target->RequiresStoppedDisplay)
    return N->AccessReady?MdpNativeResourcesReady(&N->Resources):EFI_NOT_READY;
  if(!N||!N->Initialized||!N->AccessReady||!N->ExternalOwned||!N->FrameMap)return EFI_NOT_READY;
  for(UINT32 I=0;I<2;++I)if(!N->RailOwned[I]||!N->Paths[I])return EFI_NOT_READY;
  for(UINT32 I=0;I<7;++I)if((I<5||HasController(N,I-5))&&!N->ClockOwned[I])return EFI_NOT_READY;
  /* Reserve is idempotent for this exact owner and proves the private service's
     current access/domain lifetime independently of a pending group restore. */
  BOOLEAN Owned=TRUE;
  S=Exact(N->Providers.External->Reserve(N->Providers.External,N,&Owned));
  if(S!=EFI_SUCCESS)return S;if(!Owned)return EFI_NOT_READY;
  return EFI_SUCCESS;
}
STATIC CR_STATUS Ready(VOID *C,CR_MDSS_EXT_WINDOW W) {
  MDP_EXTENDED_NATIVE *N=C;if(W>=CrMdssExtWindows)return EFI_INVALID_PARAMETER;
  if(W!=CrMdssExtDpu&&!HasController(N,(W==CrMdssExtDsi0||W==CrMdssExtPhy0)?0:1))return EFI_UNSUPPORTED;
  return Current(N);
}
STATIC CR_STATUS Read(VOID *C,CR_MDSS_EXT_WINDOW W,UINT32 O,UINT32 *V){
  MDP_EXTENDED_NATIVE *N=C;
  if(!N||!N->Hardware||!V||W>=CrMdssExtWindows||(O&3)||O>Size(N,W)-4)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=Ready(C,W);if(S!=EFI_SUCCESS)return S;
  *V=MmioRead32((UINTN)Base(N,W)+O);return EFI_SUCCESS;
}
STATIC CR_STATUS Write(VOID *C,CR_MDSS_EXT_WINDOW W,UINT32 O,UINT32 V){
  MDP_EXTENDED_NATIVE *N=C;
  if(!N||!N->Hardware||W>=CrMdssExtWindows||(O&3)||O>Size(N,W)-4)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=Ready(C,W);if(S!=EFI_SUCCESS)return S;
  MmioWrite32((UINTN)Base(N,W)+O,V);return EFI_SUCCESS;
}
STATIC CR_STATUS Fence(VOID *C){EFI_STATUS S=Current(C);if(S==EFI_SUCCESS)MemoryFence();return S;}
STATIC CR_STATUS Delay(VOID *C,UINT32 Us){EFI_STATUS S=Current(C);if(S!=EFI_SUCCESS)return S;
  if(Us>20000)return EFI_INVALID_PARAMETER;MicroSecondDelay(Us);return Current(C);}
STATIC CR_STATUS Clocks(VOID *C,UINT32 I,CONST CR_MDSS_DSI_PLAN *P,BOOLEAN On,BOOLEAN *Owned) {
  MDP_EXTENDED_NATIVE *N=C;if(!Owned||!HasController(N,I))return EFI_INVALID_PARAMETER;
  /* A failure before SetPlan retains the caller's existing ownership output. */
  EFI_STATUS S=Current(N);if(S!=EFI_SUCCESS)return S;
  CR_CLOCK_DSI_PLAN V;if(On){S=CrMdssDsiClockPlan(P,I,&V);if(S!=EFI_SUCCESS)return S;}
  if(N->Config.Target->RequiresStoppedDisplay)
    return MdpNativeResourcesSetDsiPlan(&N->Resources,I,On?&V:NULL,On,Owned);
  S=Exact(N->Providers.External->SetPlan(N->Providers.External,N,I,On?&V:NULL,On,Owned));
  if(S!=EFI_SUCCESS)return S;
  if(*Owned!=On)return EFI_NOT_READY;
  if(On){CR_CLOCK_DSI_PLAN Actual;BOOLEAN Enabled,Pending;UINT64 Incarnation;
    S=Exact(N->Providers.External->GetPlan(N->Providers.External,N,I,&Actual,&Enabled,&Pending,&Incarnation));
    if(S!=EFI_SUCCESS)return S;
    if(!Enabled||Pending||!Incarnation||(N->Incarnation&&Incarnation!=N->Incarnation)||
       !CrClockDsiPlanEqual(&V,&Actual))return EFI_NOT_READY;
    N->Incarnation=Incarnation;
  }
  return EFI_SUCCESS;
}
STATIC CR_STATUS PanelReady(VOID *C){MDP_EXTENDED_NATIVE *N=C;EFI_STATUS S=Current(N);
  return S==EFI_SUCCESS?Exact(N->Config.Panel.Ready(N->Config.Panel.Context)):S;}
STATIC CR_STATUS PanelAcquire(VOID *C,BOOLEAN *Owned){MDP_EXTENDED_NATIVE *N=C;
  EFI_STATUS S=PanelReady(N);
  /* The inherited-output fence already acquired this exact panel owner.
     Reacquiring a powered bridge would demand reset HIGH too early. */
  if(S==EFI_SUCCESS&&!(N->PanelLeaseOwned&&N->OutputPanelAcquired))
    S=Exact(N->Config.Panel.Acquire(N->Config.Panel.Context,&N->PanelLeaseOwned));
  *Owned=N->PanelLeaseOwned;return S;}
STATIC CR_STATUS PanelRelease(VOID *C,BOOLEAN *Owned){MDP_EXTENDED_NATIVE *N=C;
  EFI_STATUS S=PanelReady(N);
  if(S==EFI_SUCCESS)S=Exact(N->Config.Panel.Release(N->Config.Panel.Context,&N->PanelLeaseOwned));
  if(!N->PanelLeaseOwned)N->OutputPanelAcquired=FALSE;
  *Owned=N->PanelLeaseOwned;return S;}
STATIC CR_STATUS Reset(VOID *C,BOOLEAN High){MDP_EXTENDED_NATIVE *N=C;EFI_STATUS S=PanelReady(N);
  return S==EFI_SUCCESS?Exact(N->Config.Panel.Reset(N->Config.Panel.Context,High)):S;}
STATIC EFI_STATUS BlankOutput(VOID *C){
  MDP_EXTENDED_NATIVE *N=C;
  EFI_STATUS S=MdpNativeResourcesSuppliersReady(&N->Resources);if(S!=EFI_SUCCESS)return S;
  S=Exact(N->Config.Panel.Ready(N->Config.Panel.Context));if(S!=EFI_SUCCESS)return S;
  /* A successful early acquisition is distinct from a partial owned claim.
     Reset-low retries must not call Acquire, which validates reset HIGH. */
  if(!N->OutputPanelAcquired){
    S=Exact(N->Config.Panel.Acquire(N->Config.Panel.Context,&N->PanelLeaseOwned));if(S!=EFI_SUCCESS)return S;
    if(!N->PanelLeaseOwned)return EFI_NOT_READY;
    N->OutputPanelAcquired=TRUE;
  }
  if(!N->PanelLeaseOwned)return EFI_NOT_READY;
  S=Exact(N->Config.Panel.Reset(N->Config.Panel.Context,FALSE));if(S!=EFI_SUCCESS)return S;
  MicroSecondDelay(20000);
  return MdpNativeResourcesSuppliersReady(&N->Resources);
}
STATIC CR_STATUS Publish(VOID *C,VOID *Cpu,UINTN Bytes){MDP_EXTENDED_NATIVE *N=C;
  EFI_STATUS S=Current(N);if(S!=EFI_SUCCESS)return S;
  if(Cpu!=N->FrameCpu||Bytes!=N->FrameBytes)return EFI_INVALID_PARAMETER;
  if(N->Config.Target->RequiresStoppedDisplay){
    S=MdpNativeResourcesPublishFrame(&N->Resources,Cpu,Bytes);if(S!=EFI_SUCCESS)return S;
  }
  WriteBackDataCacheRange(Cpu,Bytes);MemoryFence();
  /* A real bridge owner may finish post-reset FIFO/video preparation here,
     after panel reset/commands/PPS and before DPU/DSI timing is enabled. */
  if(N->Config.Panel.PublishPixels){S=PanelReady(N);if(S!=EFI_SUCCESS)return S;
    S=Exact(N->Config.Panel.PublishPixels(N->Config.Panel.Context,Cpu,Bytes));if(S!=EFI_SUCCESS)return S;}
  return Current(N);}
STATIC CR_STATUS CommandReady(VOID *C,UINT32 I){MDP_EXTENDED_NATIVE *N=C;
  if(!HasController(N,I)||!N->CommandMap||!N->CommandCpu)return EFI_NOT_READY;return Current(N);}
STATIC CR_STATUS CommandPublish(VOID *C,VOID *Cpu,UINTN Bytes){MDP_EXTENDED_NATIVE *N=C;
  EFI_STATUS S=Current(N);if(S!=EFI_SUCCESS)return S;
  if(!N->CommandMap||Cpu!=N->CommandCpu||!Bytes||Bytes>EFI_PAGE_SIZE)return EFI_INVALID_PARAMETER;
  if(N->Config.Target->RequiresStoppedDisplay)
    return MdpNativeResourcesPublishCommand(&N->Resources,Cpu,Bytes);
  WriteBackDataCacheRange(Cpu,Bytes);MemoryFence();return EFI_SUCCESS;}
STATIC EFI_STATUS Map(MDP_EXTENDED_NATIVE *N,BOOLEAN Command) {
  EDKII_IOMMU_PROTOCOL *I=N->Providers.Iommu;
  VOID **Token=Command?&N->CommandMap:&N->FrameMap;
  UINT64 *Address=Command?&N->CommandIova:&N->FrameIova;
  UINTN Bytes=Command?EFI_PAGE_SIZE:N->FrameBytes;
  EFI_STATUS S;
  if(Command){S=Exact(I->AllocateBuffer(I,AllocateAnyPages,EfiBootServicesData,1,&N->CommandCpu,0));
    if(S!=EFI_SUCCESS)return S;if(!N->CommandCpu||((UINTN)N->CommandCpu&EFI_PAGE_MASK))return EFI_DEVICE_ERROR;
    ZeroMem(N->CommandCpu,EFI_PAGE_SIZE);}
  S=Exact(I->Map(I,Command?EdkiiIoMmuOperationBusMasterCommonBuffer:EdkiiIoMmuOperationBusMasterRead64,
    Command?N->CommandCpu:N->FrameCpu,&Bytes,Address,Token));
  if(S!=EFI_SUCCESS)return S;
  if(!*Token||Bytes!=(Command?EFI_PAGE_SIZE:N->FrameBytes)||!*Address||
     *Address>MAX_UINT32||Bytes-1>MAX_UINT32-*Address||(*Address&EFI_PAGE_MASK))return EFI_UNSUPPORTED;
  for(UINT32 J=0;J<CR_MDSS_TARGET_STREAMS;++J){S=Exact(I->SetAttributeById(I,N->Config.Target->SmmuBase,N->Config.Target->Streams[J],*Token,EDKII_IOMMU_ACCESS_READ));
    if(S!=EFI_SUCCESS)return S;}
  return EFI_SUCCESS;
}
EFI_STATUS MdpExtendedNativeOpen(MDP_EXTENDED_NATIVE *N,CONST MDP_EXTENDED_PROVIDERS *P,
    CONST MDP_EXTENDED_NATIVE_CONFIG *C,CONST CR_MDSS_PANEL_PROFILE *F,EFI_PHYSICAL_ADDRESS Physical,UINTN Bytes) {
  EFI_STATUS S=Level();if(S!=EFI_SUCCESS)return S;
  if(!N||!P||!C||!F||!Physical||(Physical&EFI_PAGE_MASK)||!Bytes||Bytes>MAX_UINTN-(UINTN)Physical||
    !C->MdpHz||!C->Panel.Context||!C->Panel.Ready||!C->Panel.Acquire||!C->Panel.Release||!C->Panel.Reset||
    !P->Clock||P->Clock->Revision<EFI_CLOCK_CR_PROTOCOL_REVISION||!P->Clock->AcquireClock||!P->Clock->SetClock||
    !P->External||P->External->Revision!=EFI_CLOCK_EXTERNAL_CR_PROTOCOL_REVISION||!P->External->Reserve||
    !P->External->SetPlan||!P->External->GetPlan||!P->External->ReleaseOwner||
    !P->Rpmh||P->Rpmh->Revision<EFI_RPMH_CR_PROTOCOL_REVISION||!P->Rpmh->AcquireVreg||!P->Rpmh->RpmhEnableVreg||
    !P->Rpmh->EnsureBootFloor||!P->Icb||P->Icb->Revision<EFI_INTERCONNECT_CR_PROTOCOL_REVISION||
    !P->Icb->AcquirePath||!P->Icb->SetBandwidth||!P->Icb->ReleasePath||!P->Smmu||!P->Smmu->Attach||
    !P->Iommu||!P->Iommu->Map||!P->Iommu->Unmap||!P->Iommu->AllocateBuffer||!P->Iommu->FreeBuffer||
    !P->Iommu->SetAttributeById||!P->Dxe||!P->Dxe->GetMemorySpaceDescriptor)return EFI_INVALID_PARAMETER;
  if(N->Initialized)return EFI_ALREADY_STARTED;
  if(!TargetValid(C->Target,F->Mode.Raster.Soc))return EFI_INVALID_PARAMETER;
  CONST CR_MDSS_HW_PROFILE *Hardware=CrMdssGetHardwareProfile(C->Target->Soc);
  if(!Hardware||!(Hardware->Capabilities&CR_MDSS_HW_VIDEO))return EFI_UNSUPPORTED;
  S=CrMdssPanelProfileValidate(F);if(S!=EFI_SUCCESS)return S;
  UINT64 Required=(UINT64)F->Mode.Raster.Stride*F->Mode.Raster.Height;
  if(Required!=Bytes||!C->AverageKBps[0]||!C->AverageKBps[1]||
     C->PeakKBps[0]<C->AverageKBps[0]||C->PeakKBps[1]<C->AverageKBps[1])return EFI_INVALID_PARAMETER;
  ZeroMem(N,sizeof(*N));N->Providers=*P;N->Config=*C;N->FrameCpu=(VOID *)(UINTN)Physical;N->FrameBytes=Bytes;
  N->Hardware=Hardware;N->Initialized=TRUE;
  S=CrMdssDsiBuildPlan(&F->Mode,&N->Plan);if(S!=EFI_SUCCESS)goto Failed;
  if(C->Target->RequiresStoppedDisplay){
    MDP_NATIVE_RESOURCE_CONFIG ResourceConfig={.Target=C->Target,.MdpHz=C->MdpHz,
      .AverageKBps={C->AverageKBps[0],C->AverageKBps[1]},.PeakKBps={C->PeakKBps[0],C->PeakKBps[1]},
      .BlankOutput=BlankOutput,.OutputContext=N};
    UINT32 Mask=((1U<<N->Plan.Mode.Controllers)-1U)<<N->Plan.Mode.FirstController;
    S=MdpNativeResourcesOpen(&N->Resources,P,&ResourceConfig,Mask,Physical,Bytes);
    if(S!=EFI_SUCCESS)goto Failed;
    N->FrameIova=N->Resources.FrameIova;N->CommandIova=N->Resources.CommandIova;
    N->CommandCpu=N->Resources.CommandCpu;
    N->FrameMap=N->Resources.FrameLease.Token;N->CommandMap=N->Resources.CommandLease.Token;
    goto CoreInit;
  }
  S=Gcd(N,Physical,Bytes,EfiGcdMemoryTypeReserved);if(S!=EFI_SUCCESS)goto Failed;
  for(UINT32 W=0;W<CrMdssExtWindows;++W){if(W!=CrMdssExtDpu&&!HasController(N,(W==CrMdssExtDsi0||W==CrMdssExtPhy0)?0:1))continue;
    S=Gcd(N,Base(N,(CR_MDSS_EXT_WINDOW)W),Size(N,(CR_MDSS_EXT_WINDOW)W),EfiGcdMemoryTypeMemoryMappedIo);if(S!=EFI_SUCCESS)goto Failed;}
  S=Exact(P->Rpmh->EnsureBootFloor(P->Rpmh));if(S!=EFI_SUCCESS)goto Failed;
  for(UINT32 J=0;J<2;++J){S=Exact(P->Rpmh->AcquireVreg(P->Rpmh,C->Target->Rails[J].Name,C->Target->Rails[J].Millivolts,C->Target->Rails[J].Mode,&N->RailOwned[J]));
    if(S!=EFI_SUCCESS||!N->RailOwned[J]){if(S==EFI_SUCCESS)S=EFI_NOT_READY;goto Failed;}}
  for(UINT32 J=0;J<2;++J){S=Exact(P->Icb->AcquirePath(P->Icb,C->Target->Paths[J].Provider,C->Target->Paths[J].Source,C->Target->Paths[J].Destination,&N->Paths[J]));
    if(S!=EFI_SUCCESS||!N->Paths[J]){if(S==EFI_SUCCESS)S=EFI_NOT_READY;goto Failed;}
    S=Exact(P->Icb->SetBandwidth(P->Icb,N->Paths[J],C->AverageKBps[J],C->PeakKBps[J]));if(S!=EFI_SUCCESS)goto Failed;}
  S=Exact(P->External->Reserve(P->External,N,&N->ExternalOwned));
  if(S!=EFI_SUCCESS||!N->ExternalOwned){if(S==EFI_SUCCESS)S=EFI_NOT_READY;goto Failed;}
  for(UINT32 J=0;J<7;++J){if(J>=5&&!HasController(N,J-5))continue;
    UINT64 Rate=(J==0||J==3)?C->MdpHz:(J>=4?19200000:0);
    S=Exact(P->Clock->AcquireClock(P->Clock,N->Config.Target->Clocks[J].Controller,N->Config.Target->Clocks[J].Name,Rate,&N->ClockOwned[J]));
    if(S!=EFI_SUCCESS||!N->ClockOwned[J]){if(S==EFI_SUCCESS)S=EFI_NOT_READY;goto Failed;}}
  for(UINT32 J=0;J<CR_MDSS_TARGET_STREAMS;++J){S=Exact(P->Smmu->Attach(P->Smmu,0,0xffff,N->Config.Target->Streams[J]));if(S!=EFI_SUCCESS)goto Failed;}
  S=Map(N,FALSE);if(S!=EFI_SUCCESS)goto Failed;
  S=Map(N,TRUE);if(S!=EFI_SUCCESS)goto Failed;
CoreInit:
  N->AccessReady=TRUE;
  CR_MDSS_PANEL_IO PanelIo={N,PanelReady,PanelAcquire,PanelRelease,Reset,Delay,Publish};
  S=CrMdssDsiPanelInit(&N->Panel,F,&PanelIo);if(S!=EFI_SUCCESS)goto Failed;
  CR_MDSS_EXT_IO Io={.Context=N,.Ready=Ready,.Read32=Read,.Write32=Write,.Fence=Fence,.DelayUs=Delay,.ExternalClocks=Clocks};
  for(UINT32 W=0;W<CrMdssExtWindows;++W){Io.Physical[W]=Base(N,(CR_MDSS_EXT_WINDOW)W);Io.Bytes[W]=Size(N,(CR_MDSS_EXT_WINDOW)W);}
  CR_MDSS_EXT_PANEL Panel={&N->Panel,CrMdssDsiPanelPrepare,CrMdssDsiPanelUnprepare,CrMdssDsiPanelPublish};
  CR_MDSS_DSI_COMMAND_MEMORY Command={N,N->CommandCpu,EFI_PAGE_SIZE,N->CommandIova,CommandReady,CommandPublish};
  S=CrMdssExtendedInit(&N->Display,&Io,&Panel,&F->Mode,N->FrameCpu,N->FrameIova,Bytes,&Command);
  if(S==EFI_SUCCESS)return S;
Failed:
  /* Exact retained journals are preserved even if cleanup itself fails. */
  (VOID)MdpExtendedNativeClose(N);return S;
}
EFI_STATUS MdpExtendedNativeStart(MDP_EXTENDED_NATIVE *N){EFI_STATUS S=Current(N);if(S!=EFI_SUCCESS)return S;
  if(N->Closing)return EFI_NOT_READY;
  if(N->Config.Target->RequiresStoppedDisplay){S=MdpNativeResourcesCheck(&N->Resources);if(S!=EFI_SUCCESS)return S;}
  return CrMdssExtendedStart(&N->Display);}
EFI_STATUS MdpExtendedNativeClose(MDP_EXTENDED_NATIVE *N) {
  EFI_STATUS S=Level();if(S!=EFI_SUCCESS)return S;if(!N)return EFI_INVALID_PARAMETER;
  if(N->Frozen)return EFI_ACCESS_DENIED;
  if(!N->Initialized)return EFI_SUCCESS;N->Closing=TRUE;
  if(N->Display.Initialized){S=CrMdssExtendedUninit(&N->Display);if(S!=EFI_SUCCESS)return S;}
  if(N->Panel.Initialized){S=CrMdssDsiPanelUninit(&N->Panel);if(S!=EFI_SUCCESS)return S;}
  if(N->Config.Target->RequiresStoppedDisplay){
    S=MdpNativeResourcesClose(&N->Resources);if(S!=EFI_SUCCESS)return S;
    ZeroMem(N,sizeof(*N));return EFI_SUCCESS;
  }
  N->AccessReady=FALSE;
  if(N->CommandMap){S=Exact(N->Providers.Iommu->Unmap(N->Providers.Iommu,N->CommandMap));if(S!=EFI_SUCCESS)return S;N->CommandMap=NULL;}
  if(N->CommandCpu){S=Exact(N->Providers.Iommu->FreeBuffer(N->Providers.Iommu,1,N->CommandCpu));if(S!=EFI_SUCCESS)return S;N->CommandCpu=NULL;}
  if(N->FrameMap){S=Exact(N->Providers.Iommu->Unmap(N->Providers.Iommu,N->FrameMap));if(S!=EFI_SUCCESS)return S;N->FrameMap=NULL;}
  for(UINT32 J=7;J-->0;)if(N->ClockOwned[J]){S=Exact(N->Providers.Clock->SetClock(N->Providers.Clock,N->Config.Target->Clocks[J].Controller,N->Config.Target->Clocks[J].Name,0,FALSE));
    if(S!=EFI_SUCCESS)return S;N->ClockOwned[J]=FALSE;}
  if(N->ExternalOwned){S=Exact(N->Providers.External->ReleaseOwner(N->Providers.External,N));if(S!=EFI_SUCCESS)return S;N->ExternalOwned=FALSE;}
  for(UINT32 J=2;J-->0;)if(N->Paths[J]){S=Exact(N->Providers.Icb->ReleasePath(N->Providers.Icb,N->Paths[J]));if(S!=EFI_SUCCESS)return S;N->Paths[J]=0;}
  for(UINT32 J=2;J-->0;)if(N->RailOwned[J]){S=Exact(N->Providers.Rpmh->RpmhEnableVreg(N->Providers.Rpmh,N->Config.Target->Rails[J].Name,FALSE));
    if(S!=EFI_SUCCESS)return S;N->RailOwned[J]=FALSE;}
  ZeroMem(N,sizeof(*N));return EFI_SUCCESS;
}
EFI_STATUS MdpExtendedNativeFreeze(MDP_EXTENDED_NATIVE *N) {
  EFI_STATUS S;
  if(!N||!N->Initialized)return EFI_INVALID_PARAMETER;
  if(EfiGetCurrentTpl()>TPL_NOTIFY)return EFI_ACCESS_DENIED;
  if(N->FreezeAccess)return EFI_NOT_READY;
  N->Frozen=TRUE;
  if(N->FreezeComplete)return N->FreezeStatus;
  N->FreezeAccess=TRUE;
  S=Current(N);if(S!=EFI_SUCCESS)goto Done;
  if(N->Config.Target->RequiresStoppedDisplay){S=MdpNativeResourcesCheck(&N->Resources);if(S!=EFI_SUCCESS)goto Done;}
  if(N->Display.Dpu.Initialized){
    S=CrMdssDpuExtendedStop(&N->Display.Dpu);if(S!=EFI_SUCCESS)goto Done;
  }
  /* Do not run panel-off packets, drop clocks, or unmap at NOTIFY. This is
     Linux's host-disable/reset portion only: a completed software reset and
     idle readback fence any FIFO/common-buffer command DMA as well as video. */
  for(UINT32 I=N->Plan.Mode.Controllers;I>0;--I){
    UINT32 Controller=N->Plan.Link[I-1].Controller,V;
    CR_MDSS_EXT_WINDOW W=Controller?CrMdssExtDsi1:CrMdssExtDsi0;
    S=Read(N,W,4,&V);if(S!=EFI_SUCCESS)goto Done;
    S=Write(N,W,4,V&~7U);if(S!=EFI_SUCCESS)goto Done;
    S=Write(N,W,0x118,1);if(S!=EFI_SUCCESS)goto Done;
    S=Delay(N,20000);if(S!=EFI_SUCCESS)goto Done;
    S=Write(N,W,0x118,0);if(S!=EFI_SUCCESS)goto Done;
    S=Fence(N);if(S!=EFI_SUCCESS)goto Done;
    for(UINT32 Elapsed=0;;Elapsed+=50){
      UINT32 Busy;
      S=Read(N,W,4,&V);if(S!=EFI_SUCCESS)goto Done;
      S=Read(N,W,8,&Busy);if(S!=EFI_SUCCESS)goto Done;
      if(!(V&7)&&!(Busy&2))break;
      if(Elapsed==5000){S=EFI_TIMEOUT;goto Done;}
      S=Delay(N,50);if(S!=EFI_SUCCESS)goto Done;
    }
  }
  N->Display.Active=FALSE;N->Display.Dsi.Active=FALSE;
  N->FreezeComplete=TRUE;S=EFI_SUCCESS;
Done:
  N->FreezeStatus=S;N->FreezeAccess=FALSE;return S;
}
