/* Selected bridge path: real packaged XML -> selected profile -> extended GOP.
 * LT9611 remains the exact physical GPIO/FIFO owner. SPDX-License-Identifier: MIT */
#include "MdpHdkExtendedProfile.h"
#include "MdpExtendedEntry.h"
#include <Guid/EventGroup.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CrDalLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/EFILt9611PanelCrProtocol.h>
typedef struct {
  EFI_MDSS_PANEL_PROFILE_CR_PROTOCOL Selection;
  EFI_MDP_DSI_PROTOCOL Prepared;
  MDP_PANEL_CONFIG Xml;
  MDP_EXTENDED_PANEL Panel;
  MDP_EXTENDED_ENTRY Entry;
  EFI_LT9611_PANEL_CR_PROTOCOL *Bridge;
  VOID *LeaseOwner;
  EFI_HANDLE Handle;
  EFI_EVENT ExitEvent;
  BOOLEAN Initialized,Busy,Frozen,Published,PublicationPending,PanelOwned;
} HDK_PROFILE;
STATIC HDK_PROFILE mProfile;
STATIC CONST CR_MDSS_PANEL_RESET ResetSteps[]={{TRUE,{0},20000},{FALSE,{0},20000},{TRUE,{0},300000}};
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_NOT_READY;}
STATIC EFI_STATUS Enter(HDK_PROFILE *P){
  if(EfiGetCurrentTpl()>TPL_CALLBACK)return EFI_ACCESS_DENIED;
  EFI_TPL T=gBS->RaiseTPL(TPL_NOTIFY);EFI_STATUS S=P->Frozen?EFI_ACCESS_DENIED:P->Busy?EFI_NOT_READY:EFI_SUCCESS;
  if(S==EFI_SUCCESS)P->Busy=TRUE;gBS->RestoreTPL(T);return S;
}
STATIC EFI_STATUS Leave(HDK_PROFILE *P,EFI_STATUS S){
  EFI_TPL T=gBS->RaiseTPL(TPL_NOTIFY);P->Busy=FALSE;gBS->RestoreTPL(T);return S;
}
STATIC EFI_STATUS Bridge(HDK_PROFILE *P){
  if(!P->Initialized||P->Frozen||!P->LeaseOwner)return EFI_NOT_READY;
  if(!P->Bridge){VOID *B=NULL;EFI_STATUS S=Exact(gBS->LocateProtocol(&gEfiLt9611PanelCrProtocolGuid,NULL,&B));
    if(S!=EFI_SUCCESS)return S;if(!B)return EFI_NOT_READY;
    EFI_LT9611_PANEL_CR_PROTOCOL *L=B;
    if(L->Revision!=EFI_LT9611_PANEL_CR_PROTOCOL_REVISION||!L->Ready||!L->Acquire||!L->Release||!L->Reset||!L->Prepare)
      return EFI_UNSUPPORTED;
    P->Bridge=L;
  }
  return Exact(P->Bridge->Ready(P->Bridge,P));
}
STATIC CR_STATUS PanelReady(VOID *C){if(C!=&mProfile)return EFI_INVALID_PARAMETER;return Bridge(C);}
STATIC CR_STATUS PanelAcquire(VOID *C,BOOLEAN *Owned){HDK_PROFILE *P=C;
  EFI_STATUS S=PanelReady(C);
  if(S==EFI_SUCCESS)S=Exact(P->Bridge->Acquire(P->Bridge,P,&P->PanelOwned));
  *Owned=P->PanelOwned;return S;}
STATIC CR_STATUS PanelRelease(VOID *C,BOOLEAN *Owned){HDK_PROFILE *P=C;
  EFI_STATUS S=PanelReady(C);
  if(S==EFI_SUCCESS)S=Exact(P->Bridge->Release(P->Bridge,P,&P->PanelOwned));
  *Owned=P->PanelOwned;return S;}
STATIC CR_STATUS PanelReset(VOID *C,BOOLEAN High){HDK_PROFILE *P=C;EFI_STATUS S=PanelReady(C);
  return S==EFI_SUCCESS?Exact(P->Bridge->Reset(P->Bridge,P,High)):S;}
STATIC CR_STATUS PanelPrepare(VOID *C,VOID *Cpu,UINTN Bytes){HDK_PROFILE *P=C;
  EFI_STATUS S=PanelReady(C);if(S!=EFI_SUCCESS)return S;
  if(!Cpu||Bytes!=(UINTN)P->Panel.Profile.Mode.Raster.Stride*P->Panel.Profile.Mode.Raster.Height)return EFI_INVALID_PARAMETER;
  return Exact(P->Bridge->Prepare(P->Bridge,P,&P->Xml.Timing));}
STATIC EFI_STATUS EFIAPI SelectionReady(EFI_MDSS_PANEL_PROFILE_CR_PROTOCOL *This,VOID *Owner){
  HDK_PROFILE *P=&mProfile;
  if(This!=&P->Selection||Owner!=P->LeaseOwner)return EFI_ACCESS_DENIED;
  if(EfiGetCurrentTpl()>TPL_CALLBACK)return EFI_ACCESS_DENIED;return Bridge(P);
}
STATIC EFI_STATUS EFIAPI SelectionAcquire(EFI_MDSS_PANEL_PROFILE_CR_PROTOCOL *This,VOID *Owner,
  EFI_MDSS_PANEL_SELECTION *Out,BOOLEAN *Owned){
  HDK_PROFILE *P=&mProfile;if(This!=&P->Selection||!Owner||!Out||!Owned)return EFI_INVALID_PARAMETER;
  if(!P->Initialized||P->Frozen)return EFI_NOT_READY;
  if(EfiGetCurrentTpl()>TPL_CALLBACK)return EFI_ACCESS_DENIED;
  if(P->LeaseOwner&&P->LeaseOwner!=Owner)return EFI_ACCESS_DENIED;
  P->LeaseOwner=Owner;*Owned=TRUE;EFI_STATUS S=Bridge(P);if(S!=EFI_SUCCESS)return S;
  UINT64 Average=(P->Panel.Profile.Mode.PixelHz*4+999)/1000;
  *Out=(EFI_MDSS_PANEL_SELECTION){.Size=sizeof(*Out),.Revision=1,.Profile=&P->Panel.Profile,
    .MdpHz=200000000,.AverageKBps={1000,Average},.PeakKBps={1000,Average*2},
    .Panel={P,PanelReady,PanelAcquire,PanelRelease,PanelReset,NULL,PanelPrepare}};
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI SelectionRelease(EFI_MDSS_PANEL_PROFILE_CR_PROTOCOL *This,VOID *Owner,BOOLEAN *Owned){
  HDK_PROFILE *P=&mProfile;if(This!=&P->Selection||!Owner||!Owned)return EFI_INVALID_PARAMETER;
  if(EfiGetCurrentTpl()>TPL_CALLBACK)return EFI_ACCESS_DENIED;
  if(P->LeaseOwner&&P->LeaseOwner!=Owner)return EFI_ACCESS_DENIED;
  if(P->PanelOwned)return EFI_NOT_READY;
  P->LeaseOwner=NULL;*Owned=FALSE;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Configure(EFI_MDP_DSI_PROTOCOL *This,CONST EFI_MDP_DSI_TIMING *T,EFI_PHYSICAL_ADDRESS F,UINT32 S){
  (VOID)T;(VOID)F;(VOID)S;if(This!=&mProfile.Prepared)return EFI_INVALID_PARAMETER;
  return mProfile.Frozen?EFI_ACCESS_DENIED:EFI_UNSUPPORTED;
}
STATIC EFI_STATUS EFIAPI GetFramebuffer(EFI_MDP_DSI_PROTOCOL *This,EFI_PHYSICAL_ADDRESS *Frame,
  UINT32 *Width,UINT32 *Height,UINT32 *Stride){
  HDK_PROFILE *P=&mProfile;if(This!=&P->Prepared||!Frame||!Width||!Height||!Stride)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=Enter(P);if(S!=EFI_SUCCESS)return S;
  MDP_EXTENDED_OUTPUT *O=&P->Entry.Output;
  if(!O->Published)S=EFI_NOT_READY;
  else{*Frame=O->Mode.FrameBufferBase;*Width=O->Info.HorizontalResolution;*Height=O->Info.VerticalResolution;
    *Stride=O->Info.PixelsPerScanLine*4;}
  return Leave(P,S);
}
STATIC EFI_STATUS EFIAPI BridgeReady(EFI_MDP_DSI_PROTOCOL *This){
  HDK_PROFILE *P=&mProfile;if(This!=&P->Prepared)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=Enter(P);if(S!=EFI_SUCCESS)return S;
  S=P->Entry.Output.Published?SelectionReady(&P->Selection,&P->Entry):
    MdpExtendedEntryStart(&P->Entry,&P->Selection);
  DEBUG((DEBUG_WARN,"MDSSCr: selected bridge profile BridgeReady %r GOP=%u retained=%u\n",
    S,P->Entry.Output.Published,P->Entry.Initialized));return Leave(P,S);
}
STATIC VOID EFIAPI Exit(EFI_EVENT Event,VOID *C){
  HDK_PROFILE *P=C;if(P!=&mProfile||Event!=P->ExitEvent)return;
  P->Frozen=TRUE;MemoryFence();
  if(P->Entry.Output.Initialized)(VOID)MdpExtendedOutputFreeze(&P->Entry.Output);
}
BOOLEAN MdpHdkExtendedProfileRetained(VOID){return mProfile.Initialized;}
EFI_STATUS MdpHdkExtendedProfileStart(VOID){
  HDK_PROFILE *P=&mProfile;if(P->Initialized)return EFI_ALREADY_STARTED;
  if(EfiGetCurrentTpl()>TPL_CALLBACK)return EFI_ACCESS_DENIED;
  CONST CR_DAL_LT9611_CONFIG *Target=CrDalGetLt9611Config();
  CONST CR_MDSS_TARGET *Mdss=CrDalGetMdssContext();
  if(!Target||!Mdss||Target->I2cAddress!=0x2b||Target->PowerPinCount!=3||!Target->PowerPins)
    return EFI_UNSUPPORTED;
  if(Mdss->Soc==CR_MDSS_SOC_SM8450){
    if(Target->I2cInstance!=10||Target->PowerPins[0]!=9||Target->PowerPins[1]!=109||
       Target->PowerPins[2]!=107||Target->InterruptPin!=44)return EFI_UNSUPPORTED;
  }else if(Mdss->Soc==CR_MDSS_SOC_SM8350){
    if(Target->I2cInstance!=15||Target->PowerPins[0]!=47||Target->PowerPins[1]!=49||
       Target->PowerPins[2]!=48||Target->InterruptPin!=50)return EFI_UNSUPPORTED;
  }else return EFI_UNSUPPORTED;
  ZeroMem(P,sizeof(*P));P->Initialized=TRUE;
  EFI_GUID XmlGuid={0x7321f393,0x1df3,0x4a58,{0x90,0x40,0xdc,0x1e,0xef,0x15,0xeb,0x35}};
  VOID *Xml=NULL;UINTN Bytes=0;EFI_STATUS S=Exact(GetSectionFromAnyFv(&XmlGuid,EFI_SECTION_RAW,0,&Xml,&Bytes));
  if(S!=EFI_SUCCESS){if(Xml)FreePool(Xml);goto Failed;}
  if(!Xml||!Bytes){S=EFI_COMPROMISED_DATA;goto Parsed;}
  S=MdpParsePanelXml(Xml,Bytes,&P->Xml);
Parsed:
  if(Xml)FreePool(Xml);if(S!=EFI_SUCCESS)goto Failed;
  /* Exact selected transport mode. Alternative physical panels must publish their
     own explicit profile/power owner; they cannot borrow these HDMI pins. */
  EFI_MDP_DSI_TIMING *T=&P->Xml.Timing;
  /* The reused transport XML does not grant GPIO ownership. Actual reset
     identity comes from the explicit bridge target, never its template pin. */
  T->DisplayResetGpio=Target->PowerPins[2];
  if(MdpValidateVideoPanel(&P->Xml)!=EFI_SUCCESS||T->Width!=1920||T->Height!=1080||
    T->HFrontPorch!=88||T->HBackPorch!=148||T->HSync!=44||T->VFrontPorch!=4||T->VBackPorch!=36||T->VSync!=5||
    T->DsiRefreshRate!=(60U<<16)||T->DsiLanes!=4||T->DsiController!=0||T->DisplayResetGpio!=Target->PowerPins[2]||P->Xml.HasCommands){S=EFI_UNSUPPORTED;goto Failed;}
  CONST UINT8 LaneMap[]={1,2,4,8};
  S=MdpBuildExtendedPanelForSoc(Mdss->Soc,&P->Xml,NULL,LaneMap,FALSE,ResetSteps,ARRAY_SIZE(ResetSteps),FALSE,&P->Panel);
  if(S!=EFI_SUCCESS)goto Failed;
  P->Selection=(EFI_MDSS_PANEL_PROFILE_CR_PROTOCOL){EFI_MDSS_PANEL_PROFILE_CR_PROTOCOL_REVISION,SelectionAcquire,SelectionReady,SelectionRelease};
  P->Prepared=(EFI_MDP_DSI_PROTOCOL){BridgeReady,Configure,GetFramebuffer};
  S=Exact(gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Exit,P,&gEfiEventExitBootServicesGuid,&P->ExitEvent));
  if(S!=EFI_SUCCESS||!P->ExitEvent){if(S==EFI_SUCCESS)S=EFI_DEVICE_ERROR;goto Failed;}
  if(P->Frozen){S=EFI_ACCESS_DENIED;goto Failed;}
  S=Exact(gBS->InstallMultipleProtocolInterfaces(&P->Handle,&gEfiMdssPanelProfileCrProtocolGuid,&P->Selection,
    &gEfiMdpDsiProtocolGuid,&P->Prepared,NULL));
  if(S!=EFI_SUCCESS||!P->Handle){if(P->Handle||S==EFI_SUCCESS)P->PublicationPending=TRUE;if(S==EFI_SUCCESS)S=EFI_DEVICE_ERROR;goto Failed;}
  P->Published=TRUE;return EFI_SUCCESS;
Failed:
  (VOID)MdpHdkExtendedProfileUnload();return S;
}
EFI_STATUS MdpHdkExtendedProfileUnload(VOID){
  HDK_PROFILE *P=&mProfile;if(!P->Initialized)return EFI_SUCCESS;
  EFI_STATUS S=Enter(P);if(S!=EFI_SUCCESS)return S;
  S=MdpExtendedEntryClose(&P->Entry);if(S!=EFI_SUCCESS)return Leave(P,S);
  if(P->PublicationPending)return Leave(P,EFI_ACCESS_DENIED);
  if(P->Published){S=Exact(gBS->UninstallMultipleProtocolInterfaces(P->Handle,&gEfiMdssPanelProfileCrProtocolGuid,&P->Selection,
    &gEfiMdpDsiProtocolGuid,&P->Prepared,NULL));if(S!=EFI_SUCCESS)return Leave(P,S);P->Published=FALSE;P->Handle=NULL;}
  if(P->ExitEvent){S=Exact(gBS->CloseEvent(P->ExitEvent));if(S!=EFI_SUCCESS)return Leave(P,S);P->ExitEvent=NULL;}
  MdpFreePanelConfig(&P->Xml);ZeroMem(P,sizeof(*P));return EFI_SUCCESS;
}
