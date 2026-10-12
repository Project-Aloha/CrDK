/* Operative native extended display -> retained physical BGRX GOP.
 * Shared Crane cores own hardware; this file owns UEFI publication/lifetime.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "MdpExtendedOutput.h"
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/DebugLib.h>
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_NOT_READY;}
STATIC MDP_EXTENDED_OUTPUT *Output(EFI_GRAPHICS_OUTPUT_PROTOCOL *G){
  return BASE_CR(G,MDP_EXTENDED_OUTPUT,Gop);
}
STATIC EFI_STATUS EnterAtLevel(MDP_EXTENDED_OUTPUT *O,EFI_TPL Limit){
  if(!O||!O->Initialized)return EFI_INVALID_PARAMETER;
  if(EfiGetCurrentTpl()>Limit)return EFI_ACCESS_DENIED;
  EFI_TPL T=gBS->RaiseTPL(TPL_NOTIFY);
  EFI_STATUS S=O->Exiting?EFI_ACCESS_DENIED:O->Busy?EFI_NOT_READY:EFI_SUCCESS;
  if(S==EFI_SUCCESS)O->Busy=TRUE;gBS->RestoreTPL(T);return S;
}
/* Power, construction and teardown remain callback-level operations.
 * GOP mode metadata and framebuffer BLTs also serve GraphicsConsole/SRE at
 * TPL_NOTIFY. Their existing owner/mapping checks and cache publication are
 * notify-safe; this does not admit clock, panel or allocation lifecycle RPCs. */
STATIC EFI_STATUS Enter(MDP_EXTENDED_OUTPUT *O){return EnterAtLevel(O,TPL_CALLBACK);}
STATIC EFI_STATUS Leave(MDP_EXTENDED_OUTPUT *O,EFI_STATUS S){
  EFI_TPL T=gBS->RaiseTPL(TPL_NOTIFY);O->Busy=FALSE;gBS->RestoreTPL(T);return S;
}
STATIC BOOLEAN Rectangle(MDP_EXTENDED_OUTPUT *O,UINTN X,UINTN Y,UINTN W,UINTN H){
  return W&&H&&X<O->Info.HorizontalResolution&&Y<O->Info.VerticalResolution&&
    W<=O->Info.HorizontalResolution-X&&H<=O->Info.VerticalResolution-Y;
}
STATIC BOOLEAN BufferRectangle(VOID *P,UINTN X,UINTN Y,UINTN W,UINTN H,UINTN Delta){
  if(!P||!W||!H||W>MAX_UINTN/4)return FALSE;
  UINTN Row=Delta?Delta:W*4,End;
  if((Row&3)||X>Row/4||W>Row/4-X||Y>MAX_UINTN-(H-1))return FALSE;
  End=Y+H-1;if(End>MAX_UINTN/Row)return FALSE;End*=Row;
  if(X+W>MAX_UINTN/4||End>MAX_UINTN-(X+W)*4)return FALSE;End+=(X+W)*4;
  return (UINTN)P<=MAX_UINTN-End;
}
STATIC EFI_STATUS BltLocked(MDP_EXTENDED_OUTPUT *O,EFI_GRAPHICS_OUTPUT_BLT_PIXEL *B,
  EFI_GRAPHICS_OUTPUT_BLT_OPERATION Op,UINTN Sx,UINTN Sy,UINTN Dx,UINTN Dy,UINTN W,UINTN H,UINTN Delta){
  if(O->Exiting||!O->BltConfig||!O->CacheReady)return EFI_ACCESS_DENIED;
  if(Op>=EfiGraphicsOutputBltOperationMax||!W||!H)return EFI_INVALID_PARAMETER;
  if(Op==EfiBltVideoToBltBuffer){
    if(!Rectangle(O,Sx,Sy,W,H)||!BufferRectangle(B,Dx,Dy,W,H,Delta))return EFI_INVALID_PARAMETER;
  }else{
    if(!Rectangle(O,Dx,Dy,W,H))return EFI_INVALID_PARAMETER;
    if(Op==EfiBltVideoToVideo&&!Rectangle(O,Sx,Sy,W,H))return EFI_INVALID_PARAMETER;
    if(Op==EfiBltBufferToVideo&&!BufferRectangle(B,Sx,Sy,W,H,Delta))return EFI_INVALID_PARAMETER;
    if(Op==EfiBltVideoFill&&!B)return EFI_INVALID_PARAMETER;
  }
  EFI_STATUS Admission=EFI_SUCCESS;
  if(O->PipelineContext)Admission=Exact(O->Pipeline.Ready(O->PipelineContext));
  else if(O->Typed)Admission=MdpNativeResourcesCheck(&O->Native.Resources);
  if(Admission!=EFI_SUCCESS)return Admission;
  RETURN_STATUS R=FrameBufferBlt(O->BltConfig,B,Op,Sx,Sy,Dx,Dy,W,H,Delta);
  if(R!=RETURN_SUCCESS)return EFI_INVALID_PARAMETER;
  if(Op!=EfiBltVideoToBltBuffer){
    for(UINTN Y=0;Y<H;++Y){
      if(O->Exiting)return EFI_ACCESS_DENIED;
      WriteBackDataCacheRange((VOID *)(UINTN)(O->Mode.FrameBufferBase+
        (Dy+Y)*O->Info.PixelsPerScanLine*4+Dx*4),W*4);
    }
    MemoryFence();
    if(O->PipelineContext){
      UINTN Offset=Dy*O->Info.PixelsPerScanLine*4+Dx*4;
      UINTN Span=(H-1)*O->Info.PixelsPerScanLine*4+W*4;
      return Exact(O->Pipeline.Publish(O->PipelineContext,Offset,Span));
    }
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Query(EFI_GRAPHICS_OUTPUT_PROTOCOL *G,UINT32 Mode,UINTN *Size,
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info){
  if(!G||!Size||!Info)return EFI_INVALID_PARAMETER;
  MDP_EXTENDED_OUTPUT *O=Output(G);if(G!=&O->Gop)return EFI_INVALID_PARAMETER;
  if(Mode)return EFI_UNSUPPORTED;EFI_STATUS S=EnterAtLevel(O,TPL_NOTIFY);if(S!=EFI_SUCCESS)return S;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *V=AllocateCopyPool(sizeof(O->Info),&O->Info);
  if(O->Exiting){if(V)FreePool(V);return Leave(O,EFI_ACCESS_DENIED);}
  if(!V)return Leave(O,EFI_OUT_OF_RESOURCES);*Info=V;*Size=sizeof(O->Info);return Leave(O,EFI_SUCCESS);
}
STATIC EFI_STATUS EFIAPI Blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *G,EFI_GRAPHICS_OUTPUT_BLT_PIXEL *B,
  EFI_GRAPHICS_OUTPUT_BLT_OPERATION Op,UINTN Sx,UINTN Sy,UINTN Dx,UINTN Dy,UINTN W,UINTN H,UINTN Delta){
  if(!G)return EFI_INVALID_PARAMETER;MDP_EXTENDED_OUTPUT *O=Output(G);
  EFI_STATUS S=EnterAtLevel(O,TPL_NOTIFY);if(S!=EFI_SUCCESS)return S;
  return Leave(O,BltLocked(O,B,Op,Sx,Sy,Dx,Dy,W,H,Delta));
}
STATIC EFI_STATUS EFIAPI SetMode(EFI_GRAPHICS_OUTPUT_PROTOCOL *G,UINT32 Mode){
  if(!G)return EFI_INVALID_PARAMETER;if(Mode)return EFI_UNSUPPORTED;
  MDP_EXTENDED_OUTPUT *O=Output(G);EFI_STATUS S=EnterAtLevel(O,TPL_NOTIFY);if(S!=EFI_SUCCESS)return S;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Black={0};
  return Leave(O,BltLocked(O,&Black,EfiBltVideoFill,0,0,0,0,
    O->Info.HorizontalResolution,O->Info.VerticalResolution,0));
}
STATIC EFI_STATUS EFIAPI SetPower(EFI_DISPLAY_POWER_PROTOCOL *P,EFI_DISPLAY_POWER_STATE State){
  if(!P||(State!=EfiDisplayPowerStateOn&&State!=EfiDisplayPowerStateOff))return EFI_INVALID_PARAMETER;
  MDP_EXTENDED_OUTPUT *O=BASE_CR(P,MDP_EXTENDED_OUTPUT,Power);
  EFI_STATUS S=Enter(O);if(S!=EFI_SUCCESS)return S;
  if(O->PipelineContext)return Leave(O,Exact(O->Pipeline.SetPower(O->PipelineContext,State)));
  S=State==EfiDisplayPowerStateOn?MdpExtendedNativeStart(&O->Native):CrMdssExtendedStop(&O->Native.Display);
  return Leave(O,S);
}
STATIC EFI_STATUS EFIAPI GetPower(EFI_DISPLAY_POWER_PROTOCOL *P,EFI_DISPLAY_POWER_STATE *State){
  if(!P||!State)return EFI_INVALID_PARAMETER;MDP_EXTENDED_OUTPUT *O=BASE_CR(P,MDP_EXTENDED_OUTPUT,Power);
  EFI_STATUS S=Enter(O);if(S!=EFI_SUCCESS)return S;
  if(O->PipelineContext)return Leave(O,Exact(O->Pipeline.GetPower(O->PipelineContext,State)));
  if(O->Native.Display.CleanupPending)S=EFI_NOT_READY;
  else *State=O->Native.Display.Active?EfiDisplayPowerStateOn:EfiDisplayPowerStateOff;
  return Leave(O,S);
}
STATIC EFI_STATUS Descriptor(MDP_EXTENDED_OUTPUT *O,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){
  if(O->Exiting)return EFI_ACCESS_DENIED;
  EFI_STATUS S=Exact(O->Dxe->GetMemorySpaceDescriptor(O->Mode.FrameBufferBase,D));
  if(S!=EFI_SUCCESS)return S;
  if(D->GcdMemoryType!=EfiGcdMemoryTypeReserved||D->BaseAddress>O->Mode.FrameBufferBase||
    D->Length>MAX_UINT64-D->BaseAddress||O->Mode.FrameBufferBase>MAX_UINT64-O->CacheBytes||
    D->Length<O->CacheBytes||O->Mode.FrameBufferBase-D->BaseAddress>D->Length-O->CacheBytes||
    (D->Attributes&(EFI_MEMORY_RP|EFI_MEMORY_WP|EFI_MEMORY_RO)))return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Cache(MDP_EXTENDED_OUTPUT *O){
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;EFI_STATUS S=Descriptor(O,&D);if(S!=EFI_SUCCESS)return S;
  if(!(D.Capabilities&EFI_MEMORY_WC))return EFI_UNSUPPORTED;
  O->OriginalAttributes=D.Attributes;O->AttributesSaved=TRUE;O->AttributesTouched=TRUE;
  UINT64 Desired=(D.Attributes&~EFI_CACHE_ATTRIBUTE_MASK)|EFI_MEMORY_WC;
  WriteBackInvalidateDataCacheRange((VOID *)(UINTN)O->Mode.FrameBufferBase,O->CacheBytes);
  if(O->Exiting)return EFI_ACCESS_DENIED;
  S=Exact(O->Dxe->SetMemorySpaceAttributes(O->Mode.FrameBufferBase,O->CacheBytes,Desired));
  if(S!=EFI_SUCCESS)return S;
  S=Descriptor(O,&D);if(S!=EFI_SUCCESS)return S;
  if(D.Attributes!=Desired)return EFI_DEVICE_ERROR;O->CacheReady=TRUE;return EFI_SUCCESS;
}
EFI_STATUS EFIAPI MdpExtendedOutputFreeze(VOID *Context){
  MDP_EXTENDED_OUTPUT *O=Context;if(!O||!O->Initialized)return EFI_INVALID_PARAMETER;
  if(EfiGetCurrentTpl()>TPL_NOTIFY)return EFI_ACCESS_DENIED;
  EFI_TPL T=gBS->RaiseTPL(TPL_NOTIFY);O->Exiting=TRUE;
  if(O->Busy){if(O->Native.Initialized)O->Native.Frozen=TRUE;gBS->RestoreTPL(T);return EFI_NOT_READY;}
  if(!O->Frozen||!(O->PipelineContext?O->PipelineFrozen:O->Native.FreezeComplete)){
    O->FreezeStatus=O->PipelineContext?Exact(O->Pipeline.Freeze(O->PipelineContext)):
      O->Native.Initialized?MdpExtendedNativeFreeze(&O->Native):EFI_NOT_READY;
    if(O->PipelineContext&&O->FreezeStatus==EFI_SUCCESS)O->PipelineFrozen=TRUE;
    O->Frozen=TRUE;
  }
  gBS->RestoreTPL(T);return O->Typed?O->FreezeStatus:EFI_SUCCESS;
}
STATIC VOID EFIAPI Exit(EFI_EVENT Event,VOID *Context){(VOID)Event;(VOID)MdpExtendedOutputFreeze(Context);}
EFI_STATUS EFIAPI MdpExtendedOutputSnapshot(VOID *Context,CR_MDSS_HANDOFF_PARTICIPANT *P,VOID **Map){
  MDP_EXTENDED_OUTPUT *O=Context;
  if(!O||!P||!Map)return EFI_INVALID_PARAMETER;
  if(!O->Frozen||O->Busy)return EFI_NOT_READY;
  if(O->PipelineContext)return EFI_UNSUPPORTED;
  ZeroMem(P,sizeof(*P));P->Owner=CR_MDSS_NATIVE_DSI;P->Route.Owner=P->Owner;
  CR_MDSS_NATIVE_ROUTE *R=&P->Route;
  /* A single legacy route cannot encode two PHY source/config records or DSC
     state. Keep observed dimensions/mapping for diagnostics, explicitly deny
     current Windows CanClaim rather than turn absence into a stopped grant. */
  R->State=CR_MDSS_NATIVE_UNCERTAIN;R->Flags=CR_MDSS_NATIVE_OWNED|CR_MDSS_NATIVE_RETAINED_ERROR;
  R->Controller=O->SelectedMode.FirstController;R->Dpu=0xae01000;R->DpuBytes=0x8f000;
  R->InterfaceBase=0xae01000+0x35000+R->Controller*0x1000;R->InterfaceBytes=0x400;
  R->PipeBase=0xae01000+0x24000;R->PipeBytes=0x1000;
  R->CtlBase=0xae01000+0x15000;R->CtlBytes=0x1000;
  R->MixerBase=0xae01000+0x44000;R->MixerBytes=0x1000;
  R->LinkBase=R->Controller?0xae96000:0xae94000;R->LinkBytes=0x400;
  R->PhyBase=R->LinkBase+0x400;R->PhyBytes=0x500;R->PllBase=R->PhyBase+0x500;R->PllBytes=0x300;
  R->Mode=(CR_MDSS_NATIVE_MODE){O->Info.HorizontalResolution,O->Info.VerticalResolution,
    O->Info.PixelsPerScanLine*4,CR_MDSS_NATIVE_FORMAT_BGRX8888,
    O->SelectedMode.Raster.HFrontPorch,O->SelectedMode.Raster.HSync,O->SelectedMode.Raster.HBackPorch,
    O->SelectedMode.Raster.VFrontPorch,O->SelectedMode.Raster.VSync,O->SelectedMode.Raster.VBackPorch,
    O->SelectedMode.PixelHz};
  R->FrameBuffer=O->Mode.FrameBufferBase;R->FrameBufferBytes=O->Mode.FrameBufferSize;
  R->DeviceAddress=O->Native.FrameIova;*Map=O->Native.FrameMap;return EFI_SUCCESS;
}
EFI_STATUS MdpExtendedOutputOpen(MDP_EXTENDED_OUTPUT *O,CONST MDP_EXTENDED_PROVIDERS *P,
  CONST MDP_EXTENDED_NATIVE_CONFIG *C,CONST CR_MDSS_PANEL_PROFILE *F,EFI_PHYSICAL_ADDRESS Frame,
  UINTN Bytes,EFI_MDSS_HANDOFF_PROTOCOL *H){
  if(!O||!P||!C||!F||!Frame||!Bytes||Bytes>MAX_UINTN-EFI_PAGE_MASK||
    !P->Dxe||!P->Dxe->GetMemorySpaceDescriptor||!P->Dxe->SetMemorySpaceAttributes||
    !C->Target||
    (!C->Target->RequiresStoppedDisplay&&(!H||H->Revision!=EFI_MDSS_HANDOFF_PROTOCOL_REVISION||!H->SetParticipant))||
    (C->Target->RequiresStoppedDisplay&&(!P->Smmu||!P->Smmu->SetExitPrepareObserver))||
    EfiGetCurrentTpl()>TPL_CALLBACK)return EFI_INVALID_PARAMETER;
  if(O->Initialized)return EFI_ALREADY_STARTED;
  EFI_STATUS S=CrMdssPanelProfileValidate(F);if(S!=EFI_SUCCESS)return S;
  if((UINT64)F->Mode.Raster.Stride*F->Mode.Raster.Height!=Bytes)return EFI_INVALID_PARAMETER;
  ZeroMem(O,sizeof(*O));O->Initialized=TRUE;O->Busy=TRUE;O->Handoff=H;O->Dxe=P->Dxe;O->Typed=C->Target->RequiresStoppedDisplay;
  /* Native Close clears its journal after exact retirement. The selected
     immutable mode remains independently available to diagnose a later
     attribute/participant cleanup failure without inventing controller0. */
  O->SelectedMode=F->Mode;
  O->Native.Providers=*P;
  O->Info=(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION){.HorizontalResolution=F->Mode.Raster.Width,
    .VerticalResolution=F->Mode.Raster.Height,.PixelFormat=PixelBlueGreenRedReserved8BitPerColor,
    .PixelsPerScanLine=F->Mode.Raster.Stride/4};
  O->Mode=(EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE){.MaxMode=1,.Info=&O->Info,.SizeOfInfo=sizeof(O->Info),
    .FrameBufferBase=Frame,.FrameBufferSize=Bytes};O->CacheBytes=ALIGN_VALUE(Bytes,EFI_PAGE_SIZE);
  O->Gop=(EFI_GRAPHICS_OUTPUT_PROTOCOL){Query,SetMode,Blt,&O->Mode};
  O->Power=(EFI_DISPLAY_POWER_PROTOCOL){EFI_DISPLAY_POWER_PROTOCOL_REVISION,SetPower,GetPower};
  O->Path=(EFI_DISPLAY_DEVICE_PATH)ANDROMEDA_DISPLAY_DEVICE_PATH_INIT;
  if(!O->Typed){
    S=Exact(H->SetParticipant(H,CR_MDSS_NATIVE_DSI,O,MdpExtendedOutputFreeze,MdpExtendedOutputSnapshot));
    if(S!=EFI_SUCCESS)goto Failed;O->Registered=TRUE;
  }
  S=Exact(gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Exit,O,&gEfiEventExitBootServicesGuid,&O->ExitEvent));
  if(S!=EFI_SUCCESS||!O->ExitEvent){if(S==EFI_SUCCESS)S=EFI_DEVICE_ERROR;goto Failed;}
  if(!O->Typed){S=Cache(O);if(S!=EFI_SUCCESS)goto Failed;}
  S=MdpExtendedNativeOpen(&O->Native,P,C,F,Frame,Bytes);if(S!=EFI_SUCCESS)goto Failed;
  if(O->Typed){
    /* Producer owns the WC transition after the inherited route is stopped.
       The frontend only observes that completed cache contract. */
    EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;S=Descriptor(O,&D);if(S!=EFI_SUCCESS)goto Failed;
    if((D.Attributes&EFI_CACHE_ATTRIBUTE_MASK)!=EFI_MEMORY_WC){S=EFI_UNSUPPORTED;goto Failed;}
    O->CacheReady=TRUE;
    S=MdpExtendedOutputBindPrepare(O,P->Smmu);if(S!=EFI_SUCCESS)goto Failed;
  }
  RETURN_STATUS R=FrameBufferBltConfigure((VOID *)(UINTN)Frame,&O->Info,NULL,&O->BltBytes);
  if(R!=RETURN_BUFFER_TOO_SMALL){S=EFI_UNSUPPORTED;goto Failed;}
  O->BltConfig=AllocatePool(O->BltBytes);if(!O->BltConfig){S=EFI_OUT_OF_RESOURCES;goto Failed;}
  R=FrameBufferBltConfigure((VOID *)(UINTN)Frame,&O->Info,O->BltConfig,&O->BltBytes);
  if(R!=RETURN_SUCCESS){S=EFI_UNSUPPORTED;goto Failed;}
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Black={0};S=BltLocked(O,&Black,EfiBltVideoFill,0,0,0,0,
    O->Info.HorizontalResolution,O->Info.VerticalResolution,0);if(S!=EFI_SUCCESS)goto Failed;
  S=MdpExtendedNativeStart(&O->Native);if(S!=EFI_SUCCESS)goto Failed;
  if(O->Exiting){S=EFI_ACCESS_DENIED;goto Failed;}
  S=Exact(gBS->InstallMultipleProtocolInterfaces(&O->Handle,&gEfiDevicePathProtocolGuid,&O->Path,
    &gEfiGraphicsOutputProtocolGuid,&O->Gop,&gEfiDisplayPowerStateProtocolGuid,&O->Power,NULL));
  if(S!=EFI_SUCCESS||!O->Handle){
    if(O->Handle||S==EFI_SUCCESS)O->PublicationPending=TRUE;
    if(S==EFI_SUCCESS)S=EFI_DEVICE_ERROR;goto Failed;
  }
  O->Published=TRUE;Leave(O,EFI_SUCCESS);
  DEBUG((DEBUG_WARN,"MDSSCr: extended physical GOP %ux%u CPHY=%u controllers=%u DSC=%u CPU=%lx IOVA=%lx\n",
    O->Info.HorizontalResolution,O->Info.VerticalResolution,F->Mode.PhyType==CrMdssCphy,F->Mode.Controllers,
    F->Mode.Dsc,Frame,O->Native.FrameIova));
  return EFI_SUCCESS;
Failed:
  Leave(O,S);(VOID)MdpExtendedOutputClose(O);return S;
}
EFI_STATUS MdpExtendedOutputClose(MDP_EXTENDED_OUTPUT *O){
  if(!O)return EFI_INVALID_PARAMETER;if(!O->Initialized)return EFI_SUCCESS;
  if(O->Published||O->PublicationPending||O->Exiting)return EFI_ACCESS_DENIED;
  EFI_STATUS S=Enter(O);if(S!=EFI_SUCCESS)return S;
  if(O->ExitEvent){S=Exact(gBS->CloseEvent(O->ExitEvent));if(S!=EFI_SUCCESS)return Leave(O,S);O->ExitEvent=NULL;}
  if(O->PipelineContext){S=Exact(O->Pipeline.Close(O->PipelineContext));if(S!=EFI_SUCCESS)return Leave(O,S);}
  if(O->Native.Initialized){S=MdpExtendedNativeClose(&O->Native);if(S!=EFI_SUCCESS)return Leave(O,S);}
  /* Native Close clears its object only after actual DMA/resource retirement;
     retain the exact DxeServices pointer independently until attribute restore. */
  if(O->AttributesTouched){
    EFI_DXE_SERVICES *Dxe=O->Dxe;
    if(!Dxe)return Leave(O,EFI_NOT_READY);
    S=Exact(Dxe->SetMemorySpaceAttributes(O->Mode.FrameBufferBase,O->CacheBytes,O->OriginalAttributes));
    if(S!=EFI_SUCCESS)return Leave(O,S);
    EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;S=Descriptor(O,&D);if(S!=EFI_SUCCESS)return Leave(O,S);
    if(D.Attributes!=O->OriginalAttributes)return Leave(O,EFI_DEVICE_ERROR);O->AttributesTouched=FALSE;
  }
  if(O->PrepareBound){S=Exact(O->PrepareSmmu->SetExitPrepareObserver(O->PrepareSmmu,O,NULL,NULL));
    if(S!=EFI_SUCCESS)return Leave(O,S);O->PrepareBound=FALSE;}
  if(O->Registered){S=Exact(O->Handoff->SetParticipant(O->Handoff,CR_MDSS_NATIVE_DSI,O,NULL,NULL));
    if(S!=EFI_SUCCESS)return Leave(O,S);O->Registered=FALSE;}
  if(O->BltConfig)FreePool(O->BltConfig);ZeroMem(O,sizeof(*O));return EFI_SUCCESS;
}

EFI_STATUS MdpExtendedOutputBindPrepare(MDP_EXTENDED_OUTPUT *O,EFI_SMMU_CR_PROTOCOL *Smmu){
  if(!O||!O->Initialized||!Smmu||!Smmu->SetExitPrepareObserver||O->Exiting)return EFI_INVALID_PARAMETER;
  if(O->PrepareBound)return O->PrepareSmmu==Smmu?EFI_SUCCESS:EFI_ACCESS_DENIED;
  EFI_STATUS S=Exact(Smmu->SetExitPrepareObserver(Smmu,O,MdpExtendedOutputFreeze,O));
  if(S==EFI_SUCCESS){O->PrepareSmmu=Smmu;O->PrepareBound=TRUE;}return S;
}
EFI_STATUS MdpExtendedOutputInitPipeline(MDP_EXTENDED_OUTPUT *O,EFI_DXE_SERVICES *Dxe,
  CONST MDP_OUTPUT_PIPELINE *P,VOID *Context,EFI_PHYSICAL_ADDRESS Frame,UINT32 W,UINT32 H,UINT32 Stride){
  if(!O||!Dxe||!Dxe->GetMemorySpaceDescriptor||!P||!P->Ready||!P->Publish||!P->Freeze||
     !P->Close||!P->SetPower||!P->GetPower||!Context||!Frame||(Frame&EFI_PAGE_MASK)||
     !W||!H||W>MAX_UINT32/4||Stride<W*4||(Stride&3)||
     (UINT64)Stride*H>MAX_UINTN-EFI_PAGE_MASK||EfiGetCurrentTpl()>TPL_CALLBACK)return EFI_INVALID_PARAMETER;
  if(O->Initialized)return EFI_ALREADY_STARTED;
  ZeroMem(O,sizeof(*O));O->Initialized=TRUE;O->Busy=TRUE;O->Typed=TRUE;O->Dxe=Dxe;
  O->Pipeline=*P;O->PipelineContext=Context;
  O->Info=(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION){.HorizontalResolution=W,.VerticalResolution=H,
    .PixelFormat=PixelBlueGreenRedReserved8BitPerColor,.PixelsPerScanLine=Stride/4};
  O->Mode=(EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE){.MaxMode=1,.Info=&O->Info,.SizeOfInfo=sizeof(O->Info),
    .FrameBufferBase=Frame,.FrameBufferSize=(UINTN)Stride*H};
  O->CacheBytes=ALIGN_VALUE(O->Mode.FrameBufferSize,EFI_PAGE_SIZE);
  O->Gop=(EFI_GRAPHICS_OUTPUT_PROTOCOL){Query,SetMode,Blt,&O->Mode};
  O->Power=(EFI_DISPLAY_POWER_PROTOCOL){EFI_DISPLAY_POWER_PROTOCOL_REVISION,SetPower,GetPower};
  O->Path=(EFI_DISPLAY_DEVICE_PATH)ANDROMEDA_DISPLAY_DEVICE_PATH_INIT;
  EFI_STATUS S=Exact(gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Exit,O,&gEfiEventExitBootServicesGuid,&O->ExitEvent));
  if(S==EFI_SUCCESS&&!O->ExitEvent)S=EFI_DEVICE_ERROR;
  return Leave(O,S);
}
EFI_STATUS MdpExtendedOutputConfigureFramebuffer(MDP_EXTENDED_OUTPUT *O){
  EFI_STATUS S=Enter(O);if(S!=EFI_SUCCESS)return S;
  if(!O->PipelineContext||O->BltConfig)return Leave(O,EFI_ALREADY_STARTED);
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;S=Descriptor(O,&D);if(S!=EFI_SUCCESS)return Leave(O,S);
  if((D.Attributes&EFI_CACHE_ATTRIBUTE_MASK)!=EFI_MEMORY_WC)return Leave(O,EFI_UNSUPPORTED);
  O->CacheReady=TRUE;
  RETURN_STATUS R=FrameBufferBltConfigure((VOID *)(UINTN)O->Mode.FrameBufferBase,&O->Info,NULL,&O->BltBytes);
  if(R!=RETURN_BUFFER_TOO_SMALL)return Leave(O,EFI_UNSUPPORTED);
  O->BltConfig=AllocatePool(O->BltBytes);if(!O->BltConfig)return Leave(O,EFI_OUT_OF_RESOURCES);
  R=FrameBufferBltConfigure((VOID *)(UINTN)O->Mode.FrameBufferBase,&O->Info,O->BltConfig,&O->BltBytes);
  if(R!=RETURN_SUCCESS)return Leave(O,EFI_UNSUPPORTED);
  /* Before first kickoff, Start owns the actual whole-frame DMA publish. */
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Black={0};
  R=FrameBufferBlt(O->BltConfig,&Black,EfiBltVideoFill,0,0,0,0,O->Info.HorizontalResolution,O->Info.VerticalResolution,0);
  return Leave(O,R==RETURN_SUCCESS?EFI_SUCCESS:EFI_DEVICE_ERROR);
}
EFI_STATUS MdpExtendedOutputPublishPipeline(MDP_EXTENDED_OUTPUT *O){
  EFI_STATUS S=Enter(O);if(S!=EFI_SUCCESS)return S;
  if(!O->PipelineContext||!O->BltConfig||!O->CacheReady||!O->PrepareBound)return Leave(O,EFI_NOT_READY);
  if(O->Published)return Leave(O,EFI_SUCCESS);
  if(O->PublicationPending)return Leave(O,EFI_ACCESS_DENIED);
  S=Exact(O->Pipeline.Ready(O->PipelineContext));if(S!=EFI_SUCCESS)return Leave(O,S);
  S=Exact(gBS->InstallMultipleProtocolInterfaces(&O->Handle,&gEfiDevicePathProtocolGuid,&O->Path,
    &gEfiGraphicsOutputProtocolGuid,&O->Gop,&gEfiDisplayPowerStateProtocolGuid,&O->Power,NULL));
  if(O->Handle||S==EFI_SUCCESS)O->PublicationPending=TRUE;
  if(S==EFI_SUCCESS&&!O->Handle)S=EFI_DEVICE_ERROR;
  if(O->Exiting&&S==EFI_SUCCESS)S=EFI_ACCESS_DENIED;
  if(S==EFI_SUCCESS){O->Published=TRUE;O->PublicationPending=FALSE;}
  return Leave(O,S);
}
