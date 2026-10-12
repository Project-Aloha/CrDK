/* Shared native resources + portable command core + real deferred IRQ.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "MdpCommandNative.h"
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#define TRY(X) do{EFI_STATUS S_ = Exact(X);if(S_!=EFI_SUCCESS)return S_;}while(0)
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS||EFI_ERROR(S)?S:EFI_NOT_READY;}
STATIC BOOLEAN Valid(CONST MDP_COMMAND_NATIVE *N){return N&&N->Self==N&&N->Initialized;}
STATIC EFI_STATUS Enter(MDP_COMMAND_NATIVE *N,BOOLEAN NotifySafe,EFI_TPL *Old){
  if(!Valid(N)||!Old||!gBS||!gBS->RaiseTPL||!gBS->RestoreTPL)return EFI_INVALID_PARAMETER;
  EFI_TPL T=EfiGetCurrentTpl();
  if(T>(NotifySafe?TPL_NOTIFY:TPL_CALLBACK))return EFI_ACCESS_DENIED;
  *Old=gBS->RaiseTPL(NotifySafe?TPL_NOTIFY:TPL_CALLBACK);
  EFI_TPL Guard=gBS->RaiseTPL(TPL_NOTIFY);
  EFI_STATUS S=N->Busy?EFI_NOT_READY:EFI_SUCCESS;if(S==EFI_SUCCESS)N->Busy=TRUE;
  gBS->RestoreTPL(Guard);if(S!=EFI_SUCCESS)gBS->RestoreTPL(*Old);return S;
}
STATIC EFI_STATUS Leave(MDP_COMMAND_NATIVE *N,EFI_TPL Old,EFI_STATUS S){
  EFI_TPL Guard=gBS->RaiseTPL(TPL_NOTIFY);N->Busy=FALSE;gBS->RestoreTPL(Guard);
  gBS->RestoreTPL(Old);return Exact(S);
}
STATIC EFI_STATUS Access(MDP_COMMAND_NATIVE *N){
  if(!Valid(N)||!N->Generation||N->Generation!=N->Resources.OwnerGeneration)return EFI_NOT_READY;
  return Exact(MdpNativeResourcesReady(&N->Resources));
}
STATIC CR_MDSS_RETIRE_WINDOW Window(CR_MDSS_CMD_WINDOW W){
  switch(W){case CrMdssCmdDpu:return CrMdssRetireDpu;
    case CrMdssCmdHost:return CrMdssRetireDsi0;case CrMdssCmdPhy:return CrMdssRetirePhy0;
    default:return CrMdssRetireVbif;}
}
STATIC CR_STATUS Ready(VOID *C,CR_MDSS_CMD_WINDOW W){
  return W<CrMdssCmdWindows?Access(C):EFI_INVALID_PARAMETER;
}
STATIC CR_STATUS Read(VOID *C,CR_MDSS_CMD_WINDOW W,UINT32 O,UINT32 *V){
  MDP_COMMAND_NATIVE *N=C;if(W>=CrMdssCmdWindows)return EFI_INVALID_PARAMETER;TRY(Access(N));
  return MdpNativeResourcesRead(&N->Resources,Window(W),O,V);
}
STATIC CR_STATUS Write(VOID *C,CR_MDSS_CMD_WINDOW W,UINT32 O,UINT32 V){
  MDP_COMMAND_NATIVE *N=C;if(W>=CrMdssCmdWindows)return EFI_INVALID_PARAMETER;TRY(Access(N));
  return MdpNativeResourcesWrite(&N->Resources,Window(W),O,V);
}
STATIC CR_STATUS Fence(VOID *C){MDP_COMMAND_NATIVE *N=C;TRY(Access(N));return MdpNativeResourcesFence(&N->Resources);}
STATIC CR_STATUS Delay(VOID *C,UINT32 Us){MDP_COMMAND_NATIVE *N=C;TRY(Access(N));return MdpNativeResourcesDelay(&N->Resources,Us);}
STATIC EFI_STATUS PowerReady(VOID *C){
  MDP_COMMAND_NATIVE *N=C;
  if(!Valid(N)||N->Frozen)return EFI_NOT_READY;
  return MdpNativeResourcesSuppliersReady(&N->Resources);
}
STATIC EFI_STATUS PowerDelay(VOID *C,UINT32 Us){
  TRY(PowerReady(C));
  if(!Us||Us>20000)return EFI_INVALID_PARAMETER;
  MicroSecondDelay(Us);return PowerReady(C);
}
STATIC EFI_STATUS BlankOutput(VOID *C){
  MDP_COMMAND_NATIVE *N=C;TRY(PowerReady(N));
  if(!N->Panel.Initialized)TRY(MdpPanelPowerInit(&N->Panel,N->Providers.Rpmh,N->Config.Gpio,
    &N->Config.Panel,N,PowerReady,PowerDelay));
  BOOLEAN Owned=FALSE;TRY(MdpPanelPowerBindPins(&N->Panel,&Owned));
  if(!Owned)return EFI_NOT_READY;
  TRY(MdpPanelPowerReset(&N->Panel,FALSE));
  return PowerDelay(N,20000);
}
STATIC CR_STATUS FrameReady(VOID *C,CR_MDSS_CMD_WINDOW W){return W==CrMdssCmdDpu?Access(C):EFI_ACCESS_DENIED;}
STATIC CR_STATUS CommandReady(VOID *C,CR_MDSS_CMD_WINDOW W){return W==CrMdssCmdHost?Access(C):EFI_ACCESS_DENIED;}
STATIC CR_STATUS PublishFrame(VOID *C,VOID *Cpu,UINTN Bytes){
  MDP_COMMAND_NATIVE *N=C;TRY(Access(N));return MdpNativeResourcesPublishFrame(&N->Resources,Cpu,Bytes);
}
STATIC CR_STATUS PublishCommand(VOID *C,VOID *Cpu,UINTN Bytes){
  MDP_COMMAND_NATIVE *N=C;TRY(Access(N));return MdpNativeResourcesPublishCommand(&N->Resources,Cpu,Bytes);
}
STATIC CR_STATUS Power(VOID *C,BOOLEAN On,BOOLEAN *Owned){return MdpPanelPowerSet(&((MDP_COMMAND_NATIVE*)C)->Panel,On,Owned);}
STATIC CR_STATUS Reset(VOID *C,BOOLEAN High){
  MDP_COMMAND_NATIVE *N=C;EFI_STATUS S=MdpPanelPowerReset(&N->Panel,High);
  DEBUG((DEBUG_WARN,"MDSSCr: panel reset GPIO%u level=%u status=%r\n",
    N->Panel.Config.Reset.PinNumber,High,S));
  return S;
}
STATIC CR_STATUS Clocks(VOID *C,CONST CR_MDSS_CMD_PLAN *P,BOOLEAN On,BOOLEAN *Owned){
  MDP_COMMAND_NATIVE *N=C;if(!P||!P->ByteInterfaceHz||P->ByteHz%P->ByteInterfaceHz)return EFI_INVALID_PARAMETER;
  CR_CLOCK_DSI_PLAN Plan={.Revision=CR_CLOCK_DSI_PLAN_REVISION,.Controller=P->Config.Controller,
    .PixelHz=P->PixelHz,.ByteHz=P->ByteHz,.PixelSourceHz=P->PixelHz,.PixelM=1,.PixelN=1,
    .ByteInterfaceDivider=(UINT32)(P->ByteHz/P->ByteInterfaceHz)};
  return MdpNativeResourcesSetDsiPlan(&N->Resources,P->Config.Controller,On?&Plan:NULL,On,Owned);
}
STATIC VOID Notify(MDP_COMMAND_NATIVE *N,EFI_STATUS S){
  if(N->CompletionNotified||N->Closing||N->Frozen)return;
  N->CompletionNotified=TRUE;N->StartupStatus=Exact(S);
  if(N->Config.StartupComplete)N->Config.StartupComplete(N->Config.NotifyContext,N->StartupStatus);
}
STATIC EFI_STATUS Service(VOID *C,UINT64 Generation){
  MDP_COMMAND_NATIVE *N=C;if(!Valid(N)||Generation!=N->Generation)return EFI_ACCESS_DENIED;
  EFI_TPL Old;TRY(Enter(N,FALSE,&Old));
  EFI_STATUS S;
  if(N->Closing||N->Frozen)S=EFI_SUCCESS;
  else {
    S=Exact(Access(N));UINT32 Events=0;
    if(S==EFI_SUCCESS)S=Exact(CrMdssCmdHandleIrq(&N->Core,&Events));
    if(S==EFI_SUCCESS&&N->Core.Starting&&N->Core.FirstFrameDone){
      S=Exact(MdpNativeResourcesCheck(&N->Resources));
      if(S==EFI_SUCCESS)S=Exact(CrMdssCmdStart(&N->Core));
    }
  }
  BOOLEAN Completed=N->Core.Active;
  S=Leave(N,Old,S);if(Completed||S!=EFI_SUCCESS)Notify(N,S);return S;
}
STATIC CR_STATUS Events(VOID *C,BOOLEAN On,BOOLEAN *Owned){
  MDP_COMMAND_NATIVE *N=C;if(!Owned||On>1)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=On?MdpNativeEventsOpen(&N->Events,N,N->Generation,N->Config.Resources.Target->Interrupt,Service):
    MdpNativeEventsSeal(&N->Events);
  *Owned=MdpNativeEventsOwned(&N->Events);return Exact(S);
}
STATIC CR_STATUS FreezeEvents(VOID *C,BOOLEAN *Frozen){
  MDP_COMMAND_NATIVE *N=C;if(!Frozen)return EFI_INVALID_PARAMETER;*Frozen=FALSE;
  EFI_STATUS S=Exact(MdpNativeEventsFreeze(&N->Events));
  if(S==EFI_SUCCESS)*Frozen=!N->Events.Initialized||N->Events.Frozen;return S;
}
STATIC EFI_STATUS SetupCore(MDP_COMMAND_NATIVE *N){
  CONST CR_MDSS_HW_PROFILE *H=N->Resources.Hardware;
  if(!H||H->Soc!=CR_MDSS_CMD_SM8250||H->DpuBytes<0x83208||H->DsiBytes<0x400||H->PhyBytes<0x7c0||
     N->Config.Resources.Target->VbifBytes<0x2008)return EFI_UNSUPPORTED;
  CR_MDSS_CMD_IO Io={.Context=N,.Physical={H->DpuBase,H->DsiBase[0],H->PhyBase[0],N->Config.Resources.Target->VbifBase},
    .Bytes={0x83208,0x400,0x7c0,0x2008},.Ready=Ready,.Read32=Read,.Write32=Write,.Fence=Fence,.DelayUs=Delay,
    .ExternalClocks=Clocks,.PanelPower=Power,.PanelReset=Reset,.Events=Events,.FreezeEvents=FreezeEvents};
  CR_MDSS_CMD_MEMORY F={N,N->Resources.FrameCpu,N->Resources.FrameBytes,N->Resources.FrameIova,FrameReady,PublishFrame};
  CR_MDSS_CMD_MEMORY B={N,N->Resources.CommandCpu,EFI_PAGE_SIZE,N->Resources.CommandIova,CommandReady,PublishCommand};
  return Exact(CrMdssCmdInitProfile(&N->Core,&Io,&N->Profile,&F,&B));
}
EFI_STATUS MdpCommandNativeOpen(MDP_COMMAND_NATIVE *N,CONST MDP_NATIVE_PROVIDERS *P,
  CONST MDP_COMMAND_NATIVE_CONFIG *C,CONST CR_MDSS_CMD_PROFILE *F,EFI_PHYSICAL_ADDRESS Physical,UINTN Bytes){
  if(!N||!P||!C||!F||!C->Resources.Target||!C->Gpio||!gBS||!gBS->RaiseTPL||!gBS->RestoreTPL)
    return EFI_INVALID_PARAMETER;
  if(EfiGetCurrentTpl()>TPL_CALLBACK)return EFI_ACCESS_DENIED;
  CR_MDSS_CMD_PLAN Plan;TRY(CrMdssCmdBuildPlanProfile(F,&Plan));
  if(F->Hardware.Soc!=C->Resources.Target->Soc||F->Hardware.Soc!=CR_MDSS_CMD_SM8250||F->Config.Controller||
     !Physical||(Physical&(EFI_PAGE_SIZE-1))||Bytes<(UINTN)F->Config.Stride*F->Config.Height)
    return EFI_UNSUPPORTED;
  if(!N->Initialized){
    if(N->Self)return EFI_INVALID_PARAMETER;
    ZeroMem(N,sizeof(*N));N->Self=N;N->Config=*C;N->Providers=*P;N->Profile=*F;
    N->FramePhysical=Physical;N->FrameBytes=Bytes;N->StartupStatus=EFI_NOT_READY;N->FreezeStatus=EFI_NOT_READY;N->Initialized=TRUE;
  }else if(!Valid(N)||N->FramePhysical!=Physical||N->FrameBytes!=Bytes||
    CompareMem(&N->Config,C,sizeof(*C))||CompareMem(&N->Providers,P,sizeof(*P))||CompareMem(&N->Profile,F,sizeof(*F)))
    return EFI_ACCESS_DENIED;
  if(N->Closing||N->Frozen)return EFI_NOT_READY;
  EFI_TPL Old;TRY(Enter(N,FALSE,&Old));EFI_STATUS S=EFI_SUCCESS;
  if(N->Stage<1){
    MDP_NATIVE_RESOURCE_CONFIG Resources=C->Resources;
    Resources.BlankOutput=BlankOutput;Resources.OutputContext=N;
    S=Exact(MdpNativeResourcesOpen(&N->Resources,P,&Resources,1,Physical,Bytes));
    if(S==EFI_SUCCESS){N->Generation=N->Resources.OwnerGeneration;N->Stage=1;}
  }
  if(S==EFI_SUCCESS&&N->Stage<2){
    if(!N->Panel.Initialized)S=Exact(MdpPanelPowerInit(&N->Panel,P->Rpmh,C->Gpio,&C->Panel,N,PowerReady,PowerDelay));
    if(S==EFI_SUCCESS){BOOLEAN Owned=FALSE;S=Exact(MdpPanelPowerBindPins(&N->Panel,&Owned));}
    if(S==EFI_SUCCESS)N->Stage=2;
  }
  if(S==EFI_SUCCESS&&N->Stage<3){S=SetupCore(N);if(S==EFI_SUCCESS)N->Stage=3;}
  if(S==EFI_SUCCESS&&!N->ObserverBound){
    if(!P->Smmu||!P->Smmu->SetExitPrepareObserver)S=EFI_UNSUPPORTED;
    else S=Exact(P->Smmu->SetExitPrepareObserver(P->Smmu,N,MdpCommandNativeFreeze,N));
    if(S==EFI_SUCCESS)N->ObserverBound=TRUE;
  }
  return Leave(N,Old,S);
}
EFI_STATUS MdpCommandNativeReady(MDP_COMMAND_NATIVE *N){
  if(!Valid(N))return EFI_INVALID_PARAMETER;
  if(N->Closing||N->Frozen||!N->ObserverBound||!N->Core.Active||N->Core.CleanupPending||N->Core.Faulted||
     !N->Events.Registered||N->Events.Faulted||N->Events.Incarnation!=N->Generation)return EFI_NOT_READY;
  TRY(Access(N));return MdpPanelPowerReady(&N->Panel);
}
EFI_STATUS MdpCommandNativeStart(MDP_COMMAND_NATIVE *N){
  EFI_TPL Old;TRY(Enter(N,FALSE,&Old));EFI_STATUS S;
  if(N->Closing||N->Frozen||!N->ObserverBound||N->Stage!=3)S=EFI_NOT_READY;
  else if(N->Core.Active)S=MdpCommandNativeReady(N);
  else {S=Exact(MdpNativeResourcesCheck(&N->Resources));if(S==EFI_SUCCESS)S=Exact(CrMdssCmdStart(&N->Core));}
  BOOLEAN Pending=S==EFI_NOT_READY&&N->Core.Starting&&!N->Core.CleanupPending;
  S=Leave(N,Old,S);if(!Pending&&(S==EFI_SUCCESS||N->Core.CleanupPending))Notify(N,S);return S;
}
EFI_STATUS MdpCommandNativePublish(MDP_COMMAND_NATIVE *N,UINTN Offset,UINTN Bytes){
  /* Auto-refresh already owns scanout. Publish only validates the retained
   * mapping and cleans framebuffer cache; it must work for notify-level GOP
   * callers without admitting Start, brightness commands or teardown there. */
  EFI_TPL Old;TRY(Enter(N,TRUE,&Old));EFI_STATUS S=MdpCommandNativeReady(N);
  if(S==EFI_SUCCESS)S=Exact(CrMdssCmdPublish(&N->Core,Offset,Bytes));return Leave(N,Old,S);
}
EFI_STATUS MdpCommandNativeBrightness(MDP_COMMAND_NATIVE *N,UINT32 Level){
  EFI_TPL Old;TRY(Enter(N,FALSE,&Old));EFI_STATUS S=MdpCommandNativeReady(N);
  if(S==EFI_SUCCESS)S=Exact(CrMdssCmdSetBrightness(&N->Core,Level));return Leave(N,Old,S);
}
EFI_STATUS MdpCommandNativeClose(MDP_COMMAND_NATIVE *N){
  EFI_TPL Old;TRY(Enter(N,FALSE,&Old));N->Closing=TRUE;EFI_STATUS S;
  if(N->Frozen)S=EFI_ACCESS_DENIED;
  else {
    S=N->Core.Initialized?Exact(CrMdssCmdStop(&N->Core)):Exact(MdpNativeEventsSeal(&N->Events));
    if(S==EFI_SUCCESS&&N->Panel.Initialized)S=Exact(MdpPanelPowerClose(&N->Panel));
    if(S==EFI_SUCCESS)S=Exact(MdpNativeResourcesClose(&N->Resources));
  }
  /* Typed group/retained panel prevent disposal. Never zero an owned native
   * object or withdraw its registered EBS observer after a refused Close. */
  return Leave(N,Old,S);
}
EFI_STATUS EFIAPI MdpCommandNativeFreeze(VOID *Context){
  MDP_COMMAND_NATIVE *N=Context;EFI_TPL Old;TRY(Enter(N,TRUE,&Old));N->Frozen=TRUE;
  EFI_STATUS S=Exact(MdpNativeResourcesCheck(&N->Resources));
  if(S==EFI_SUCCESS)S=N->Core.Initialized?Exact(CrMdssCmdFreeze(&N->Core)):EFI_NOT_READY;
  N->FreezeStatus=S;return Leave(N,Old,S);
}
