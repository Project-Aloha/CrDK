/* Native adapter for the shared physical retirement and typed DMA owners.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "MdpNativeResources.h"
#include <Library/BaseMemoryLib.h>
#include <Library/CrDalLib.h>
#include <Library/BaseLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/GraphicsOutput.h>

STATIC MDP_NATIVE_RESOURCES *mOwner;
STATIC UINT64 mGeneration;
#define TRY(E) do {EFI_STATUS ResourceStatus=Exact(E);if(ResourceStatus!=EFI_SUCCESS)return ResourceStatus;} while(0)
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS||EFI_ERROR(S)?S:EFI_NOT_READY;}
STATIC EFI_STATUS Level(VOID){return EfiGetCurrentTpl()<=TPL_CALLBACK?EFI_SUCCESS:EFI_ACCESS_DENIED;}
STATIC UINT64 Base(CONST MDP_NATIVE_RESOURCES *R,CR_MDSS_RETIRE_WINDOW W){
  if(W==CrMdssRetireDpu)return R->Hardware->DpuBase;
  if(W==CrMdssRetireDsi0||W==CrMdssRetireDsi1)return R->Hardware->DsiBase[W==CrMdssRetireDsi1];
  if(W==CrMdssRetirePhy0||W==CrMdssRetirePhy1)return R->Hardware->PhyBase[W==CrMdssRetirePhy1];
  return R->Config.Target->VbifBase;
}
STATIC UINTN Size(CONST MDP_NATIVE_RESOURCES *R,CR_MDSS_RETIRE_WINDOW W){
  if(W==CrMdssRetireDpu)return R->Hardware->DpuBytes;
  if(W==CrMdssRetireDsi0||W==CrMdssRetireDsi1)return R->Hardware->DsiBytes;
  if(W==CrMdssRetirePhy0||W==CrMdssRetirePhy1)return R->Hardware->PhyBytes;
  return R->Config.Target->VbifBytes;
}
STATIC VOID RetirementDiagnostic(CONST MDP_NATIVE_RESOURCES *R){
  CONST CR_MDSS_RETIRE *Q=&R->Retirement;
  STATIC CONST CHAR8 *Names[]={"DPU","DSI0","DSI1","PHY0","PHY1","VBIF"};
  if(!Q->ReadbackFailed||Q->FailureWindow>=CrMdssRetireWindows)return;
  DEBUG((DEBUG_ERROR,"MDSSCr: retirement readback %a reg=%Lx mask=%08x expected=%08x actual=%08x stage=%u resume=%u\n",
    Names[Q->FailureWindow],Base(R,Q->FailureWindow)+Q->FailureOffset,
    Q->FailureMask,Q->FailureExpected,Q->FailureActual,Q->Stage,Q->ResumeStage));
}
STATIC EFI_STATUS Gcd(MDP_NATIVE_RESOURCES *R,UINT64 At,UINTN Bytes,EFI_GCD_MEMORY_TYPE Type){
  if(!Bytes||At>MAX_UINT64-Bytes)return EFI_INVALID_PARAMETER;
  UINT64 End=At+Bytes;
  while(At<End){EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;
    TRY(R->Providers.Dxe->GetMemorySpaceDescriptor(At,&D));
    if(D.GcdMemoryType!=Type||D.BaseAddress>At||!D.Length||D.BaseAddress>MAX_UINT64-D.Length||
       D.BaseAddress+D.Length<=At)return EFI_UNSUPPORTED;
    if(Type==EfiGcdMemoryTypeReserved&&(D.Attributes&(EFI_MEMORY_RP|EFI_MEMORY_WP|EFI_MEMORY_RO)))
      return EFI_ACCESS_DENIED;
    At=MIN(End,D.BaseAddress+D.Length);
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Access(MDP_NATIVE_RESOURCES *R){
  if(!R||mOwner!=R||!R->Initialized||!R->OwnerGeneration||R->Stage<2||!R->ExternalOwned)
    return EFI_NOT_READY;
  if(EfiGetCurrentTpl()>TPL_NOTIFY)return EFI_ACCESS_DENIED;
  for(UINT32 I=0;I<2;++I)if(!R->RailOwned[I]||!R->Paths[I])return EFI_NOT_READY;
  BOOLEAN Owned=TRUE;
  TRY(R->Providers.External->Reserve(R->Providers.External,R,&Owned));
  return Owned?EFI_SUCCESS:EFI_NOT_READY;
}
STATIC EFI_STATUS Range(MDP_NATIVE_RESOURCES *R,CR_MDSS_RETIRE_WINDOW W,UINT32 O){
  if(!R||!R->Hardware||W>=CrMdssRetireWindows||(O&3)||O>Size(R,W)-4)
    return EFI_INVALID_PARAMETER;
  TRY(Access(R));return R->FunctionalReady?EFI_SUCCESS:EFI_NOT_READY;
}
STATIC CR_STATUS RawReady(VOID *C,CR_MDSS_RETIRE_WINDOW W){
  if(W>=CrMdssRetireWindows)return EFI_INVALID_PARAMETER;
  MDP_NATIVE_RESOURCES *R=C;TRY(Access(R));return R->FunctionalReady?EFI_SUCCESS:EFI_NOT_READY;
}
STATIC CR_STATUS RawRead(VOID *C,CR_MDSS_RETIRE_WINDOW W,UINT32 O,UINT32 *V){
  MDP_NATIVE_RESOURCES *R=C;if(!V)return EFI_INVALID_PARAMETER;
  TRY(Range(R,W,O));*V=MmioRead32((UINTN)Base(R,W)+O);return EFI_SUCCESS;
}
STATIC CR_STATUS RawWrite(VOID *C,CR_MDSS_RETIRE_WINDOW W,UINT32 O,UINT32 V){
  MDP_NATIVE_RESOURCES *R=C;TRY(Range(R,W,O));
  MmioWrite32((UINTN)Base(R,W)+O,V);return EFI_SUCCESS;
}
STATIC CR_STATUS RawFence(VOID *C){TRY(Access(C));MemoryFence();return EFI_SUCCESS;}
STATIC CR_STATUS RawDelay(VOID *C,UINT32 Us){
  if(!Us||Us>20000)return EFI_INVALID_PARAMETER;
  TRY(Access(C));MicroSecondDelay(Us);return Access(C);
}
STATIC CR_STATUS Seal(VOID *C){
  MDP_NATIVE_RESOURCES *R=C;TRY(Access(R));
  /* This singleton has no published pixel/IRQ/pipeline interfaces until Open
     completes. The containing frontend cannot admit them before Ready. ABL
     has left the CPU; MU owns the fresh GIC registration lifecycle. */
  if(R->AccessReady||R->Stage!=2)return EFI_ACCESS_DENIED;
  R->WritersSealed=TRUE;MemoryFence();return EFI_SUCCESS;
}
STATIC CR_STATUS Reset(VOID *C,BOOLEAN Assert){
  MDP_NATIVE_RESOURCES *R=C;TRY(Access(R));
  if(!R->WritersSealed||!R->OutputBlanked)return EFI_ACCESS_DENIED;
  return Exact(R->Providers.External->ResetCore(R->Providers.External,R,R->OwnerGeneration,Assert));
}
STATIC EFI_STATUS PllFloorPolicy(VOID) {
  CONST CR_DAL_DEVICE_INFO *Info=NULL;
  TRY(CrDalGetDeviceInfo(CrDalDeviceRpmhArcInventory,0,CR_DAL_DATA_REVISION_1,
      sizeof(CR_DAL_RPMH_ARC_INVENTORY),&Info));
  if(!Info || !Info->Data || Info->DataSize!=sizeof(CR_DAL_RPMH_ARC_INVENTORY) ||
      Info->DataRevision!=CR_DAL_DATA_REVISION_1)return EFI_COMPROMISED_DATA;
  CONST CR_DAL_RPMH_ARC_INVENTORY *V=Info->Data;
  if(V->Revision!=CR_DAL_RPMH_ARC_REVISION || V->Size!=sizeof(*V) ||
      !V->Entries || !V->Count || V->Count>CR_RPMH_ARC_MAX_RESOURCES)return EFI_COMPROMISED_DATA;
  UINT32 Mm=MAX_UINT32,Mx=MAX_UINT32,Cx=MAX_UINT32,Xo=MAX_UINT32;
  for(UINT32 I=0;I<V->Count;++I){
    CONST CR_DAL_RPMH_ARC_ENTRY *Q=&V->Entries[I];
    if(Q->Name[sizeof(Q->Name)-1] ||
        (Q->ParentIndex!=CR_DAL_RPMH_ARC_NO_PARENT&&Q->ParentIndex>=I))return EFI_COMPROMISED_DATA;
    if(!AsciiStrCmp(Q->Name,"mmcx.lvl"))Mm=I;
    if(!AsciiStrCmp(Q->Name,"mx.lvl"))Mx=I;
    if(!AsciiStrCmp(Q->Name,"cx.lvl"))Cx=I;
    if(!AsciiStrCmp(Q->Name,"xo.lvl"))Xo=I;
  }
  if(Mm==MAX_UINT32 || Mx==MAX_UINT32 || Cx==MAX_UINT32 || Xo==MAX_UINT32 ||
      V->Entries[Mm].ParentIndex!=Mx || V->Entries[Mm].Kind!=CR_DAL_RPMH_ARC_POWER ||
      V->Entries[Mx].Kind!=CR_DAL_RPMH_ARC_POWER || V->Entries[Cx].Kind!=CR_DAL_RPMH_ARC_POWER ||
      V->Entries[Xo].Kind!=CR_DAL_RPMH_ARC_XO || V->Entries[Mm].MinLevel<192 ||
      V->Entries[Mx].MinLevel<192 || V->Entries[Cx].MinLevel<192)return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}
STATIC CR_STATUS PllSuppliers(VOID *C) {
  MDP_NATIVE_RESOURCES *R=C;
  /* ClockExternal already holds its provider lock. Do not call Access() or
     any Clock/External API here. The permanent boot floor and private ICC/
     rail owners cannot be withdrawn while this native journal survives. */
  if(!R || mOwner!=R || !R->Initialized || !R->OwnerGeneration || R->Stage<2 ||
      !R->Qualified || !R->WritersSealed || !R->ExternalOwned || !R->ExternalReady ||
      R->IccResetStage!=4)return EFI_NOT_READY;
  for(UINT32 I=0;I<2;++I)
    if(!R->RailOwned[I] || !R->RailReady[I] || !R->Paths[I] || !R->PathReady[I])return EFI_NOT_READY;
  /* Only SM8350 may write a cold analogue source. Kona keeps the legacy
     acknowledged boot-floor contract for its inherited read-only source;
     its catalog does not publish an explicit calibration inventory. */
  if(R->Config.Target->Soc==CR_MDSS_SOC_SM8350)TRY(PllFloorPolicy());
  /* Real provider qualifies known/nonpending/applied floor ACKs and no TCS
     in flight; minimum level authority above rejects inadequate CMDDB data. */
  return Exact(R->Providers.Rpmh->EnsureBootFloor(R->Providers.Rpmh));
}
STATIC CR_STATUS ResetReleased(VOID *C){
  MDP_NATIVE_RESOURCES *R=C;TRY(Access(R));
  TRY(R->Providers.External->CheckCoreResetReleased(R->Providers.External,R,R->OwnerGeneration));
  if(R->FunctionalReady)return Exact(R->Providers.External->CheckFunctionalPll(
    R->Providers.External,R,R->OwnerGeneration));
  TRY(R->Providers.External->RetireInheritedFunctional(R->Providers.External,R,R->OwnerGeneration));
  /* Reset this display client's absolute demands, never the shared provider.
     Two private handles preserve other display, USB and storage aggregates.
     Record each completed transition; uncertain setters retry idempotently. */
  while(R->IccResetStage<4){
    UINT32 I=R->IccResetStage/2;BOOLEAN On=R->IccResetStage&1;
    TRY(R->Providers.Icb->SetBandwidth(R->Providers.Icb,R->Paths[I],
      On?R->Config.AverageKBps[I]:0,On?R->Config.PeakKBps[I]:0));
    ++R->IccResetStage;
  }
  TRY(R->Providers.External->PrepareFunctionalPll(R->Providers.External,R,R->OwnerGeneration,PllSuppliers,R));
  for(UINT32 I=0;I<7;++I){
    if(R->ClockReady[I])continue;
    CONST CR_MDSS_TARGET_CLOCK *Q=&R->Config.Target->Clocks[I];
    if(R->ClockOwned[I]){
      TRY(R->Providers.Clock->SetClock(R->Providers.Clock,Q->Controller,Q->Name,0,FALSE));
      R->ClockOwned[I]=FALSE;
    }
    UINT64 Rate=(I==0||I==3)?R->Config.MdpHz:(I>=4?19200000:0);
    TRY(R->Providers.Clock->AcquireClock(R->Providers.Clock,Q->Controller,Q->Name,Rate,&R->ClockOwned[I]));
    if(!R->ClockOwned[I])return EFI_NOT_READY;
    R->ClockReady[I]=TRUE;
  }
  TRY(R->Providers.External->CheckFunctionalPll(R->Providers.External,R,R->OwnerGeneration));
  R->FunctionalReady=TRUE;return EFI_SUCCESS;
}
STATIC CR_STATUS ExternalOff(VOID *C){
  MDP_NATIVE_RESOURCES *R=C;TRY(Access(R));
  return Exact(R->Providers.External->RetireInheritedDsi(R->Providers.External,R,R->OwnerGeneration));
}
STATIC CR_STATUS ExternalOffCheck(VOID *C){
  MDP_NATIVE_RESOURCES *R=C;TRY(Access(R));
  return Exact(R->Providers.External->CheckInheritedDsiOff(R->Providers.External,R,R->OwnerGeneration));
}
STATIC EFI_STATUS EFIAPI VerifyStopped(VOID *C,UINT64 Generation){
  MDP_NATIVE_RESOURCES *R=C;TRY(Access(R));
  if(Generation!=R->OwnerGeneration||!R->WritersSealed||R->AccessReady)
    return EFI_ACCESS_DENIED;
  return CrMdssRetireCheck(&R->Retirement);
}
STATIC BOOLEAN TargetValid(CONST CR_MDSS_TARGET *T){
  if(!T||T->Revision!=CR_MDSS_TARGET_REVISION||T->Size!=sizeof(*T)||!T->RequiresStoppedDisplay||
     (T->Soc!=CR_MDSS_SOC_SM8250&&T->Soc!=CR_MDSS_SOC_SM8350)||
     T->TopBase!=0xae00000||T->TopBytes!=0x1000||T->VbifBase!=0xaeb0000||
     T->VbifBytes!=0x3000||T->SmmuBase!=0x15000000||T->Interrupt!=115)return FALSE;
  STATIC CONST UINT16 Sids[]={0x820,0x822,0xc20,0xc22};
  for(UINT32 I=0;I<4;++I)if(T->Streams[I]!=Sids[I])return FALSE;
  for(UINT32 I=0;I<2;++I){
    if(!T->Rails[I].Name||!T->Rails[I].Name[0]||!T->Rails[I].Millivolts||
       !T->Paths[I].Provider||!T->Paths[I].Provider[0]||T->Paths[I].Source==T->Paths[I].Destination)return FALSE;
  }
  for(UINT32 I=0;I<7;++I)
    if(!T->Clocks[I].Controller||!T->Clocks[I].Name||!T->Clocks[I].Controller[0]||!T->Clocks[I].Name[0])return FALSE;
  return TRUE;
}
STATIC BOOLEAN ProvidersValid(CONST MDP_NATIVE_PROVIDERS *P){
  return P&&P->Dxe&&P->Dxe->GetMemorySpaceDescriptor&&P->Clock&&
    P->Clock->Revision>=EFI_CLOCK_CR_PROTOCOL_REVISION&&P->Clock->AcquireClock&&
    P->Clock->SetClock&&P->Clock->SetReset&&P->Rpmh&&
    P->Rpmh->Revision>=EFI_RPMH_CR_PROTOCOL_REVISION&&P->Rpmh->AcquireVreg&&
    P->Rpmh->RpmhEnableVreg&&P->Rpmh->EnsureBootFloor&&P->Icb&&
    P->Icb->Revision>=EFI_INTERCONNECT_CR_PROTOCOL_REVISION&&P->Icb->AcquirePath&&
    P->Icb->SetBandwidth&&P->Icb->ReleasePath&&P->External&&
    P->External->Revision>=EFI_CLOCK_EXTERNAL_CR_PROTOCOL_REVISION&&P->External->Reserve&&
    P->External->SetPlan&&P->External->GetPlan&&P->External->ReleaseOwner&&
    P->External->RetireInheritedDsi&&P->External->CheckInheritedDsiOff&&P->Smmu&&
    P->External->ResetCore&&P->External->CheckCoreResetReleased&&P->External->RetireInheritedFunctional&&
    P->External->PrepareFunctionalPll&&P->External->CheckFunctionalPll&&
    P->Smmu->Revision>=EFI_SMMU_CR_DISPLAY_OWNERSHIP_REVISION&&P->Smmu->AcquireDisplayGroup&&
    P->Smmu->BindDisplayFrame&&P->Smmu->AllocateDisplayCommand&&P->Smmu->PrepareDisplayBuffer&&
    P->Smmu->CheckDisplayBuffer&&P->Smmu->QueryDisplayBuffer&&P->Smmu->PreserveDisplayFrame;
}
STATIC EFI_STATUS Query(MDP_NATIVE_RESOURCES *R,EFI_SMMU_CR_DISPLAY_LEASE *L,BOOLEAN Command){
  EFI_SMMU_CR_DISPLAY_BUFFER_INFO V={0};
  TRY(R->Providers.Smmu->QueryDisplayBuffer(R->Providers.Smmu,&R->Group,L,&V));
  UINTN Bytes=Command?EFI_PAGE_SIZE:R->FrameBytes;
  UINT32 RequiredFlags=CR_SMMU_DISPLAY_READY|CR_SMMU_DISPLAY_OWNER_READY|CR_SMMU_DISPLAY_CPU_READY|CR_SMMU_DISPLAY_MAPPING|
    (Command?CR_SMMU_DISPLAY_ALLOCATED:CR_SMMU_DISPLAY_BORROWED_FRAME);
  if(V.Size!=sizeof(V)||V.Revision!=EFI_SMMU_CR_DISPLAY_BUFFER_INFO_REVISION||
     V.Kind!=(Command?CrSmmuDisplayCommand:CrSmmuDisplayFrame)||
     V.OwnerGeneration!=R->OwnerGeneration||V.Generation!=L->Generation||
     (V.Flags&RequiredFlags)!=RequiredFlags||(V.Flags&CR_SMMU_DISPLAY_CLEANUP_ONLY)||
     V.LastStatus!=EFI_SUCCESS||V.PageBytes!=ALIGN_VALUE(Bytes,EFI_PAGE_SIZE)||
     V.GrantedMask!=0xf||V.Bytes!=Bytes||!V.HostAddress||!V.DeviceAddress||
     (V.HostAddress&EFI_PAGE_MASK)||(V.DeviceAddress&EFI_PAGE_MASK)||
     V.DeviceAddress>MAX_UINT32||Bytes-1>MAX_UINT32-V.DeviceAddress)return EFI_COMPROMISED_DATA;
  if(Command){R->CommandCpu=(VOID *)(UINTN)V.HostAddress;R->CommandIova=V.DeviceAddress;}
  else{
    if(V.HostAddress!=(UINTN)R->FrameCpu||!(V.Flags&CR_SMMU_DISPLAY_BORROWED_FRAME))return EFI_COMPROMISED_DATA;
    R->FrameIova=V.DeviceAddress;
  }
  return EFI_SUCCESS;
}
EFI_STATUS MdpNativeResourcesOpen(MDP_NATIVE_RESOURCES *R,CONST MDP_NATIVE_PROVIDERS *P,
  CONST MDP_NATIVE_RESOURCE_CONFIG *C,UINT32 Controllers,EFI_PHYSICAL_ADDRESS Physical,UINTN Bytes){
  TRY(Level());
  if(!R||!C||!TargetValid(C->Target)||!ProvidersValid(P)||!Controllers||(Controllers&~3U)||
     !Physical||(Physical&EFI_PAGE_MASK)||!Bytes||Bytes>MAX_UINTN-EFI_PAGE_MASK||Physical>MAX_UINT32||
     ALIGN_VALUE(Bytes,EFI_PAGE_SIZE)>0x100000000ULL-Physical||
     !C->BlankOutput||!C->OutputContext||!C->MdpHz||!C->AverageKBps[0]||!C->AverageKBps[1]||
     C->PeakKBps[0]<C->AverageKBps[0]||C->PeakKBps[1]<C->AverageKBps[1])return EFI_INVALID_PARAMETER;
  if(mOwner&&mOwner!=R)return EFI_ACCESS_DENIED;
  if(!R->Initialized){
    if(!gBS||!gBS->LocateProtocol||!gBS->RaiseTPL||!gBS->RestoreTPL)return EFI_NOT_READY;
    VOID *Gop=NULL;EFI_STATUS S=gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid,NULL,&Gop);
    if(S!=EFI_NOT_FOUND)return S==EFI_SUCCESS?EFI_ACCESS_DENIED:Exact(S);
    EFI_TPL T=gBS->RaiseTPL(TPL_NOTIFY);
    if((mOwner&&mOwner!=R)||mGeneration==MAX_UINT64){gBS->RestoreTPL(T);return EFI_ACCESS_DENIED;}
    mOwner=R;ZeroMem(R,sizeof(*R));R->OwnerGeneration=++mGeneration;
    R->Hardware=CrMdssGetHardwareProfile(C->Target->Soc);R->Providers=*P;R->Config=*C;
    R->FrameCpu=(VOID *)(UINTN)Physical;R->FrameBytes=Bytes;R->ControllerMask=Controllers;
    R->Initialized=TRUE;R->Stage=1;gBS->RestoreTPL(T);
  }else if(mOwner!=R||R->Config.Target!=C->Target||R->Config.MdpHz!=C->MdpHz||
           R->Config.AverageKBps[0]!=C->AverageKBps[0]||R->Config.AverageKBps[1]!=C->AverageKBps[1]||
           R->Config.PeakKBps[0]!=C->PeakKBps[0]||R->Config.PeakKBps[1]!=C->PeakKBps[1]||
           R->Config.BlankOutput!=C->BlankOutput||R->Config.OutputContext!=C->OutputContext||
           R->FrameCpu!=(VOID *)(UINTN)Physical||R->FrameBytes!=Bytes||
           R->ControllerMask!=Controllers||R->Closing)return EFI_ACCESS_DENIED;
  if(R->Stage==1){
    if(!R->Qualified){
      TRY(Gcd(R,Physical,ALIGN_VALUE(Bytes,EFI_PAGE_SIZE),EfiGcdMemoryTypeReserved));
      for(UINT32 W=0;W<CrMdssRetireWindows;++W)
        TRY(Gcd(R,Base(R,(CR_MDSS_RETIRE_WINDOW)W),Size(R,(CR_MDSS_RETIRE_WINDOW)W),EfiGcdMemoryTypeMemoryMappedIo));
      TRY(Gcd(R,0xaf02000,4,EfiGcdMemoryTypeMemoryMappedIo));
      TRY(P->Rpmh->EnsureBootFloor(P->Rpmh));
      R->Qualified=TRUE;
    }
    for(UINT32 I=0;I<2;++I){
      if(R->RailReady[I])continue;
      if(R->RailOwned[I]){
        TRY(P->Rpmh->RpmhEnableVreg(P->Rpmh,C->Target->Rails[I].Name,FALSE));
        R->RailOwned[I]=FALSE;
      }
      TRY(P->Rpmh->AcquireVreg(P->Rpmh,C->Target->Rails[I].Name,C->Target->Rails[I].Millivolts,
                             C->Target->Rails[I].Mode,&R->RailOwned[I]));
      if(!R->RailOwned[I])return EFI_NOT_READY;
      R->RailReady[I]=TRUE;
    }
    for(UINT32 I=0;I<2;++I){
      if(R->PathReady[I])continue;
      if(!R->Paths[I]){
        TRY(P->Icb->AcquirePath(P->Icb,C->Target->Paths[I].Provider,C->Target->Paths[I].Source,
                              C->Target->Paths[I].Destination,&R->Paths[I]));
        if(!R->Paths[I])return EFI_NOT_READY;
      }
      TRY(P->Icb->SetBandwidth(P->Icb,R->Paths[I],C->AverageKBps[I],C->PeakKBps[I]));
      R->PathReady[I]=TRUE;
    }
    /* A failed prerequisite lease can be drained before any destructive
       retirement. Never reacquire atop its partially retained own votes. */
    if(R->ExternalOwned&&!R->ExternalReady){
      TRY(P->External->ReleaseOwner(P->External,R));R->ExternalOwned=FALSE;
    }
    TRY(P->External->Reserve(P->External,R,&R->ExternalOwned));
    if(!R->ExternalOwned)return EFI_NOT_READY;
    R->ExternalReady=TRUE;
    /* Access/GDSC only: early BCR precedes inherited functional retirement
       and all new MDP/LUT/VSYNC/ESC source programming. */
    R->Stage=2;
    CR_MDSS_RETIRE_IO Io={R,C->Target->Soc,RawReady,Seal,RawRead,RawWrite,RawFence,
      RawDelay,Reset,ResetReleased,ExternalOff,ExternalOffCheck};
    TRY(CrMdssRetireInit(&R->Retirement,&Io));
  }
  if(R->Stage<2)return EFI_ALREADY_STARTED;
  if(R->Stage==2){
    if(!R->OutputBlanked){
      /* The panel/bridge still receives inherited scanout. Hold its reset
       * before the core BCR, source retirement or ICC demand withdrawal.
       * A failed GPIO acknowledgement retains ownership and blocks reset. */
      R->OutputTouched=TRUE;
      EFI_STATUS Blank=Exact(C->BlankOutput(C->OutputContext));
      DEBUG((DEBUG_WARN,"MDSSCr: output reset before core/clock/ICB retirement: %r\n",Blank));
      if(Blank!=EFI_SUCCESS)return Blank;
      R->OutputBlanked=TRUE;
    }
    EFI_STATUS Status=Exact(CrMdssRetireStart(&R->Retirement));
    STATIC CONST UINT32 ForceOffsets[]={0x2ac,0x2b4,0x2bc,0x2c4};
    for(UINT32 I=0;I<4;++I)if(R->Retirement.ForceMissing[I])
      DEBUG((DEBUG_WARN,"MDSSCr: clock force reg=%Lx missing=%08x actual=%08x; VBIF retirement=%r stage=%u\n",
        R->Hardware->DpuBase+ForceOffsets[I],R->Retirement.ForceMissing[I],
        R->Retirement.ForceReadback[I],Status,R->Retirement.Stage));
    if(Status!=EFI_SUCCESS){
      DEBUG((DEBUG_ERROR,"MDSSCr: inherited retirement %r stage=%u functional=%u icc=%u\n",
        Status,R->Retirement.Stage,R->FunctionalReady,R->IccResetStage));
      RetirementDiagnostic(R);
      return Status;
    }
    DEBUG((DEBUG_INFO,"MDSSCr: early core reset and display resource rebuild complete\n"));
    R->Stage=3;
  }
  EFI_SMMU_CR_PROTOCOL *S=R->Providers.Smmu;
  if(R->Stage==3){TRY(S->AcquireDisplayGroup(S,C->Target->Soc,R->OwnerGeneration,VerifyStopped,R,&R->Group));R->Stage=4;}
  if(R->Stage==4){TRY(S->BindDisplayFrame(S,&R->Group,R->FrameCpu,Physical,Bytes,&R->FrameLease));R->Stage=5;}
  if(R->Stage==5){TRY(S->AllocateDisplayCommand(S,&R->Group,EFI_PAGE_SIZE,&R->CommandLease));R->Stage=6;}
  if(R->Stage==6){TRY(S->PrepareDisplayBuffer(S,&R->Group,&R->FrameLease));R->Stage=7;}
  if(R->Stage==7){TRY(S->PrepareDisplayBuffer(S,&R->Group,&R->CommandLease));R->Stage=8;}
  if(R->Stage==8){
    TRY(Query(R,&R->FrameLease,FALSE));TRY(Query(R,&R->CommandLease,TRUE));
    TRY(S->PreserveDisplayFrame(S,&R->Group,&R->FrameLease));R->Stage=9;
  }
  if(R->Stage==9){
    EFI_STATUS Status=Exact(CrMdssRetireResume(&R->Retirement));
    if(Status!=EFI_SUCCESS){RetirementDiagnostic(R);return Status;}
    R->Stage=10;
  }
  R->AccessReady=TRUE;
  EFI_STATUS Admission=MdpNativeResourcesCheck(R);
  if(Admission!=EFI_SUCCESS)R->AccessReady=FALSE;
  return Admission;
}
EFI_STATUS MdpNativeResourcesReady(MDP_NATIVE_RESOURCES *R){
  TRY(Access(R));if(!R->AccessReady||R->Closing||!R->FunctionalReady)return EFI_NOT_READY;
  for(UINT32 I=0;I<7;++I)if(!R->ClockOwned[I]||!R->ClockReady[I])return EFI_NOT_READY;
  TRY(R->Providers.External->CheckFunctionalPll(R->Providers.External,R,R->OwnerGeneration));
  TRY(Query(R,&R->FrameLease,FALSE));return Query(R,&R->CommandLease,TRUE);
}
EFI_STATUS MdpNativeResourcesSuppliersReady(MDP_NATIVE_RESOURCES *R){
  /* Read-only lease proof, also used by a live command GOP at TPL_NOTIFY.
   * Access retains its level/owner checks. Open/Close still require Level(). */
  TRY(Access(R));
  return R->Closing||!R->Qualified||!R->ExternalReady?EFI_NOT_READY:EFI_SUCCESS;
}
EFI_STATUS MdpNativeResourcesCheck(MDP_NATIVE_RESOURCES *R){
  TRY(MdpNativeResourcesReady(R));
  TRY(R->Providers.Smmu->CheckDisplayBuffer(R->Providers.Smmu,&R->Group,&R->FrameLease));
  TRY(R->Providers.Smmu->CheckDisplayBuffer(R->Providers.Smmu,&R->Group,&R->CommandLease));
  return MdpNativeResourcesReady(R);
}
EFI_STATUS MdpNativeResourcesRead(MDP_NATIVE_RESOURCES *R,CR_MDSS_RETIRE_WINDOW W,UINT32 O,UINT32 *V){
  TRY(MdpNativeResourcesReady(R));return RawRead(R,W,O,V);
}
EFI_STATUS MdpNativeResourcesWrite(MDP_NATIVE_RESOURCES *R,CR_MDSS_RETIRE_WINDOW W,UINT32 O,UINT32 V){
  TRY(MdpNativeResourcesReady(R));return RawWrite(R,W,O,V);
}
EFI_STATUS MdpNativeResourcesFence(MDP_NATIVE_RESOURCES *R){TRY(MdpNativeResourcesReady(R));return RawFence(R);}
EFI_STATUS MdpNativeResourcesDelay(MDP_NATIVE_RESOURCES *R,UINT32 Us){TRY(MdpNativeResourcesReady(R));return RawDelay(R,Us);}
EFI_STATUS MdpNativeResourcesSetDsiPlan(MDP_NATIVE_RESOURCES *R,UINT32 I,CONST CR_CLOCK_DSI_PLAN *P,
  BOOLEAN On,BOOLEAN *Owned){
  if(!Owned||I>1||!R||!(R->ControllerMask&(1U<<I))||(On&&!P))return EFI_INVALID_PARAMETER;
  TRY(MdpNativeResourcesReady(R));
  TRY(R->Providers.External->SetPlan(R->Providers.External,R,I,On?P:NULL,On,Owned));
  if(*Owned!=On)return EFI_NOT_READY;
  if(On){CR_CLOCK_DSI_PLAN Actual;BOOLEAN Enabled,Pending;UINT64 Generation;
    TRY(R->Providers.External->GetPlan(R->Providers.External,R,I,&Actual,&Enabled,&Pending,&Generation));
    if(!Enabled||Pending||!Generation||(R->Incarnation&&R->Incarnation!=Generation)||
       !CrClockDsiPlanEqual(P,&Actual))return EFI_NOT_READY;
    R->Incarnation=Generation;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Publish(MDP_NATIVE_RESOURCES *R,VOID *Cpu,UINTN Bytes,BOOLEAN Command){
  if(!R||!Cpu||!Bytes)return EFI_INVALID_PARAMETER;
  UINTN Start=(UINTN)Cpu,BaseAddress=(UINTN)(Command?R->CommandCpu:R->FrameCpu);
  UINTN Limit=Command?EFI_PAGE_SIZE:R->FrameBytes;
  if(Start<BaseAddress||Start-BaseAddress>=Limit||Bytes>Limit-(Start-BaseAddress))return EFI_INVALID_PARAMETER;
  TRY(MdpNativeResourcesCheck(R));WriteBackDataCacheRange(Cpu,Bytes);MemoryFence();
  return MdpNativeResourcesReady(R);
}
EFI_STATUS MdpNativeResourcesPublishFrame(MDP_NATIVE_RESOURCES *R,VOID *Cpu,UINTN Bytes){return Publish(R,Cpu,Bytes,FALSE);}
EFI_STATUS MdpNativeResourcesPublishCommand(MDP_NATIVE_RESOURCES *R,VOID *Cpu,UINTN Bytes){return Publish(R,Cpu,Bytes,TRUE);}
EFI_STATUS MdpNativeResourcesClose(MDP_NATIVE_RESOURCES *R){
  TRY(Level());if(!R)return EFI_INVALID_PARAMETER;if(!R->Initialized)return EFI_SUCCESS;
  if(mOwner!=R)return EFI_ACCESS_DENIED;
  /* The typed producer permanently retains this stop callback and group
     identity. A touched reset/retirement must also retain supplier access.
     Never discard this storage or release suppliers on a partial fence. */
  if(R->OutputTouched||R->Retirement.Touched||R->Group.Token)return EFI_UNSUPPORTED;
  R->Closing=TRUE;
  for(UINT32 I=7;I-->0;)if(R->ClockOwned[I]){
    TRY(R->Providers.Clock->SetClock(R->Providers.Clock,R->Config.Target->Clocks[I].Controller,
                                     R->Config.Target->Clocks[I].Name,0,FALSE));R->ClockOwned[I]=FALSE;
  }
  if(R->ExternalOwned){TRY(R->Providers.External->ReleaseOwner(R->Providers.External,R));R->ExternalOwned=FALSE;}
  for(UINT32 I=2;I-->0;)if(R->Paths[I]){TRY(R->Providers.Icb->ReleasePath(R->Providers.Icb,R->Paths[I]));R->Paths[I]=0;}
  for(UINT32 I=2;I-->0;)if(R->RailOwned[I]){
    TRY(R->Providers.Rpmh->RpmhEnableVreg(R->Providers.Rpmh,R->Config.Target->Rails[I].Name,FALSE));R->RailOwned[I]=FALSE;
  }
  mOwner=NULL;ZeroMem(R,sizeof(*R));return EFI_SUCCESS;
}
