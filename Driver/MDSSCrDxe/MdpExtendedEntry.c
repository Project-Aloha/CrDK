/* Production selected-profile -> native providers -> physical GOP caller.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "MdpExtendedEntry.h"
#include <Library/BaseMemoryLib.h>
#include <Library/CrDalLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_NOT_READY;}
STATIC EFI_STATUS Enter(MDP_EXTENDED_ENTRY *E){
  if(EfiGetCurrentTpl()>TPL_CALLBACK)return EFI_ACCESS_DENIED;
  EFI_TPL T=gBS->RaiseTPL(TPL_NOTIFY);EFI_STATUS S=E->Busy?EFI_NOT_READY:EFI_SUCCESS;
  if(S==EFI_SUCCESS)E->Busy=TRUE;gBS->RestoreTPL(T);return S;
}
STATIC EFI_STATUS Leave(MDP_EXTENDED_ENTRY *E,EFI_STATUS S){
  EFI_TPL T=gBS->RaiseTPL(TPL_NOTIFY);E->Busy=FALSE;gBS->RestoreTPL(T);return S;
}
STATIC EFI_STATUS Locate(CONST EFI_GUID *G,VOID **Out){
  VOID *P=NULL;EFI_STATUS S=Exact(gBS->LocateProtocol((EFI_GUID *)G,NULL,&P));
  if(S!=EFI_SUCCESS)return S;if(!P)return EFI_NOT_READY;*Out=P;return EFI_SUCCESS;
}
STATIC CR_STATUS Current(VOID *C){MDP_EXTENDED_ENTRY *E=C;
  if(!E->Initialized||!E->SourceOwned||!E->Source)return CR_NOT_READY;
  return Exact(E->Source->Ready(E->Source,E));
}
STATIC CR_STATUS PanelReady(VOID *C){MDP_EXTENDED_ENTRY *E=C;CR_STATUS S=Current(C);
  return S==CR_SUCCESS?Exact(E->Selection.Panel.Ready(E->Selection.Panel.Context)):S;}
STATIC CR_STATUS PanelAcquire(VOID *C,BOOLEAN *Owned){MDP_EXTENDED_ENTRY *E=C;CR_STATUS S=PanelReady(C);
  if(S==CR_SUCCESS)S=Exact(E->Selection.Panel.Acquire(E->Selection.Panel.Context,&E->PanelOwned));
  *Owned=E->PanelOwned;return S;}
STATIC CR_STATUS PanelRelease(VOID *C,BOOLEAN *Owned){MDP_EXTENDED_ENTRY *E=C;CR_STATUS S=PanelReady(C);
  if(S==CR_SUCCESS)S=Exact(E->Selection.Panel.Release(E->Selection.Panel.Context,&E->PanelOwned));
  *Owned=E->PanelOwned;return S;}
STATIC CR_STATUS PanelReset(VOID *C,BOOLEAN High){MDP_EXTENDED_ENTRY *E=C;CR_STATUS S=PanelReady(C);
  return S==CR_SUCCESS?Exact(E->Selection.Panel.Reset(E->Selection.Panel.Context,High)):S;}
STATIC CR_STATUS PanelPrepare(VOID *C,VOID *Cpu,UINTN Bytes){MDP_EXTENDED_ENTRY *E=C;CR_STATUS S=PanelReady(C);
  if(S!=CR_SUCCESS)return S;
  return E->Selection.Panel.PublishPixels?Exact(E->Selection.Panel.PublishPixels(E->Selection.Panel.Context,Cpu,Bytes)):CR_SUCCESS;}
EFI_STATUS MdpExtendedEntryStart(MDP_EXTENDED_ENTRY *E,EFI_MDSS_PANEL_PROFILE_CR_PROTOCOL *Source){
  if(!E||!Source||Source->Revision!=EFI_MDSS_PANEL_PROFILE_CR_PROTOCOL_REVISION||
    !Source->Acquire||!Source->Ready||!Source->Release)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=Enter(E);if(S!=EFI_SUCCESS)return S;
  if(E->Initialized){S=E->Source==Source?EFI_ALREADY_STARTED:EFI_ACCESS_DENIED;return Leave(E,S);}
  E->Initialized=TRUE;E->Source=Source;
  S=Exact(Source->Acquire(Source,E,&E->Selection,&E->SourceOwned));if(S!=EFI_SUCCESS)goto Failed;
  EFI_MDSS_PANEL_SELECTION *V=&E->Selection;
  if(!E->SourceOwned||V->Size!=sizeof(*V)||V->Revision!=1||!V->Profile||!V->Panel.Context||
    !V->Panel.Ready||!V->Panel.Acquire||!V->Panel.Release||!V->Panel.Reset){S=EFI_COMPROMISED_DATA;goto Failed;}
  S=CrMdssPanelProfileValidate(V->Profile);if(S!=EFI_SUCCESS)goto Failed;
  S=Current(E);if(S!=EFI_SUCCESS)goto Failed;
  ARM_MEMORY_REGION_DESCRIPTOR_EX Region;S=Exact(LocateMemoryMapAreaByName("Display Reserved",&Region));
  if(S!=EFI_SUCCESS)goto Failed;
  UINT64 Size=(UINT64)V->Profile->Mode.Raster.Stride*V->Profile->Mode.Raster.Height;
  if(!Region.Address||!Size||Size>MAX_UINTN||Size>Region.Length){S=EFI_BAD_BUFFER_SIZE;goto Failed;}
  MDP_EXTENDED_PROVIDERS P={.Dxe=gDS};EFI_MDSS_HANDOFF_PROTOCOL *H=NULL;
#define GET(Name,Guid) do{S=Locate(&(Guid),(VOID **)&P.Name);if(S!=EFI_SUCCESS)goto Failed;}while(0)
  GET(Clock,gEfiClockCrProtocolGuid);GET(External,gEfiClockExternalCrProtocolGuid);
  GET(Rpmh,gEfiRpmhCrProtocolGuid);GET(Icb,gEfiInterconnectCrProtocolGuid);
  GET(Smmu,gEfiSmmuCrProtocolGuid);GET(Iommu,gEdkiiIoMmuProtocolGuid);
#undef GET
  CONST CR_MDSS_TARGET *Target=CrDalGetMdssContext();
  if(!Target){S=EFI_UNSUPPORTED;goto Failed;}
  if(!Target->RequiresStoppedDisplay){
    S=Locate(&gEfiMdssHandoffProtocolGuid,(VOID **)&H);if(S!=EFI_SUCCESS)goto Failed;
  }
  MDP_EXTENDED_NATIVE_CONFIG Config={.Target=Target,.MdpHz=V->MdpHz,
    .AverageKBps={V->AverageKBps[0],V->AverageKBps[1]},.PeakKBps={V->PeakKBps[0],V->PeakKBps[1]},
    .Panel={E,PanelReady,PanelAcquire,PanelRelease,PanelReset,NULL,PanelPrepare}};
  S=MdpExtendedOutputOpen(&E->Output,&P,&Config,V->Profile,Region.Address,(UINTN)Size,H);
  if(S==EFI_SUCCESS)return Leave(E,S);
Failed:
  Leave(E,S);(VOID)MdpExtendedEntryClose(E);return S;
}
EFI_STATUS MdpExtendedEntryClose(MDP_EXTENDED_ENTRY *E){
  if(!E)return EFI_INVALID_PARAMETER;if(!E->Initialized)return EFI_SUCCESS;
  EFI_STATUS S=Enter(E);if(S!=EFI_SUCCESS)return S;
  S=MdpExtendedOutputClose(&E->Output);if(S!=EFI_SUCCESS)return Leave(E,S);
  if(E->SourceOwned){S=Exact(E->Source->Release(E->Source,E,&E->SourceOwned));
    if(S!=EFI_SUCCESS)return Leave(E,S);if(E->SourceOwned)return Leave(E,EFI_NOT_READY);}
  ZeroMem(E,sizeof(*E));return EFI_SUCCESS;
}
