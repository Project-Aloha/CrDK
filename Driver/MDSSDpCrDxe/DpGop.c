/** Event-driven GOP pixel writes; no periodic framebuffer copy. MIT */
#include "DpGop.h"
#include "DpReservation.h"
#include "DpLink.h"
#include <PiDxe.h>
#include <Configuration/DisplayDevicePath.h>
#include <Guid/ConsoleOutDevice.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/SimpleTextOut.h>
STATIC EFI_HANDLE mGopHandle;
STATIC EFI_DISPLAY_DEVICE_PATH mGopPath=ANDROMEDA_DP_DISPLAY_DEVICE_PATH_INIT;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL mGop;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mGopMode;
STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION mGopInfo;
STATIC FRAME_BUFFER_CONFIGURE *mBltConfig;
STATIC UINTN mGopPages,mBltConfigSize;
STATIC BOOLEAN mGopPublished,mGopPublicationPending,mGopExiting,mGopBltBusy;
STATIC BOOLEAN mGopAttributesSaved,mGopAttributesAttempted,mGopCacheReady;
STATIC BOOLEAN mGopConOutAdded,mGopErrOutAdded;
STATIC UINT64 mGopOriginalAttributes;
STATIC EFI_STATUS GetAllocationDescriptor(EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){
  return CrDpReservationCheck(D);
}
STATIC EFI_STATUS QualifyCache(VOID){
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;
  EFI_STATUS S=GetAllocationDescriptor(&D);if(S!=EFI_SUCCESS)return S;
  if(!(D.Capabilities&EFI_MEMORY_WC))return EFI_UNSUPPORTED;
  if(!mGopAttributesSaved){mGopOriginalAttributes=D.Attributes;mGopAttributesSaved=TRUE;}
  UINT64 Desired=(mGopOriginalAttributes&~EFI_CACHE_ATTRIBUTE_MASK)|EFI_MEMORY_WC|EFI_MEMORY_XP;
  if(mGopCacheReady&&D.Attributes==Desired)return EFI_SUCCESS;
  mGopCacheReady=FALSE;mGopAttributesAttempted=TRUE;
  /* AArch64 CpuDxe maps WC to normal noncacheable memory. Linear GOP clients
   * may write FrameBufferBase without calling Blt, so clean the old WB mapping
   * before changing attributes once, rather than copying/cleaning every frame. */
  WriteBackInvalidateDataCacheRange((VOID *)(UINTN)mGopMode.FrameBufferBase,EFI_PAGES_TO_SIZE(mGopPages));
  S=gDS->SetMemorySpaceAttributes(mGopMode.FrameBufferBase,EFI_PAGES_TO_SIZE(mGopPages),Desired);
  if(S!=EFI_SUCCESS)return EFI_ERROR(S)?S:EFI_NOT_READY;
  S=GetAllocationDescriptor(&D);if(S!=EFI_SUCCESS)return S;
  if(D.Attributes!=Desired)return EFI_DEVICE_ERROR;
  mGopCacheReady=TRUE;
  DEBUG((DEBUG_WARN,"MDSSDpCr: retained GOP framebuffer cache WC verified CPU=%lx bytes=%u attributes=%lx\n",
    mGopMode.FrameBufferBase,(UINT32)EFI_PAGES_TO_SIZE(mGopPages),Desired));
  return EFI_SUCCESS;
}
STATIC BOOLEAN Enter(VOID){EFI_TPL T=gBS->RaiseTPL(TPL_NOTIFY);BOOLEAN Allowed=!mGopExiting&&!mGopBltBusy&&mBltConfig!=NULL&&mGopCacheReady;
  if(Allowed)mGopBltBusy=TRUE;gBS->RestoreTPL(T);return Allowed;}
STATIC VOID Leave(VOID){EFI_TPL T=gBS->RaiseTPL(TPL_NOTIFY);mGopBltBusy=FALSE;gBS->RestoreTPL(T);}
STATIC EFI_STATUS EFIAPI Query(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,UINT32 Mode,UINTN *Size,EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info){
  if(This!=&mGop||!Size||!Info)return EFI_INVALID_PARAMETER;
  if(Mode!=0)return EFI_UNSUPPORTED;if(!Enter())return EFI_NOT_READY;
  *Info=AllocateCopyPool(sizeof(mGopInfo),&mGopInfo);if(*Info)*Size=sizeof(mGopInfo);
  Leave();return *Info?EFI_SUCCESS:EFI_OUT_OF_RESOURCES;
}
STATIC BOOLEAN Rectangle(UINTN X,UINTN Y,UINTN W,UINTN H){return W&&H&&X<CR_DP_GOP_WIDTH&&Y<CR_DP_GOP_HEIGHT&&
  W<=CR_DP_GOP_WIDTH-X&&H<=CR_DP_GOP_HEIGHT-Y;}
STATIC BOOLEAN BufferRectangle(EFI_GRAPHICS_OUTPUT_BLT_PIXEL *P,UINTN X,UINTN Y,UINTN W,UINTN H,UINTN Delta){
  UINTN Row,End;
  if(!P||!W||!H||W>MAX_UINTN/4)return FALSE;
  Row=Delta?Delta:W*4;
  if((Row&3)||X>Row/4||W>Row/4-X||Y>MAX_UINTN-(H-1))return FALSE;
  End=Y+H-1;if(End>MAX_UINTN/Row)return FALSE;End*=Row;
  if(X+W>MAX_UINTN/4||End>MAX_UINTN-(X+W)*4)return FALSE;End+=(X+W)*4;
  return (UINTN)P<=MAX_UINTN-End;
}
STATIC EFI_STATUS EFIAPI Blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Buffer,
  EFI_GRAPHICS_OUTPUT_BLT_OPERATION Op,UINTN Sx,UINTN Sy,UINTN Dx,UINTN Dy,UINTN W,UINTN H,UINTN Delta){
  if(This!=&mGop||Op>=EfiGraphicsOutputBltOperationMax||!W||!H)return EFI_INVALID_PARAMETER;
  if(Op==EfiBltVideoToBltBuffer){if(!Rectangle(Sx,Sy,W,H)||!BufferRectangle(Buffer,Dx,Dy,W,H,Delta))return EFI_INVALID_PARAMETER;}
  else {if(!Rectangle(Dx,Dy,W,H))return EFI_INVALID_PARAMETER;
    if(Op==EfiBltVideoToVideo&&!Rectangle(Sx,Sy,W,H))return EFI_INVALID_PARAMETER;
    if(Op==EfiBltBufferToVideo&&!BufferRectangle(Buffer,Sx,Sy,W,H,Delta))return EFI_INVALID_PARAMETER;
    if(Op==EfiBltVideoFill&&!Buffer)return EFI_INVALID_PARAMETER;}
  if(!Enter())return EFI_NOT_READY;
  RETURN_STATUS S=FrameBufferBlt(mBltConfig,Buffer,Op,Sx,Sy,Dx,Dy,W,H,Delta);
  if(S==RETURN_SUCCESS&&Op!=EfiBltVideoToBltBuffer){
    /* Publish touched rows at the caller's TPL while retaining the entry
     * gate through the final fence. A glyph does not clean the full surface. */
    for(UINTN Y=0;Y<H;++Y)WriteBackDataCacheRange((VOID *)(UINTN)(mGopMode.FrameBufferBase+
      (Dy+Y)*CR_DP_GOP_STRIDE+Dx*4),W*4);
    MemoryFence();
  }
  Leave();return S==RETURN_SUCCESS?EFI_SUCCESS:EFI_INVALID_PARAMETER;
}
STATIC EFI_STATUS EFIAPI SetMode(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,UINT32 Mode){
  if(This!=&mGop)return EFI_INVALID_PARAMETER;if(Mode!=0)return EFI_UNSUPPORTED;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Black={0};return Blt(This,&Black,EfiBltVideoFill,0,0,0,0,CR_DP_GOP_WIDTH,CR_DP_GOP_HEIGHT,0);
}
EFI_STATUS CrDpGopPrepare(VOID){
  if(mGopExiting)return EFI_ACCESS_DENIED;
  if(!mGopPages){EFI_PHYSICAL_ADDRESS Address=0;UINT64 Bytes=0;
    EFI_STATUS S=CrDpReservationAcquire(&Address,&Bytes);if(S!=EFI_SUCCESS)return S;
    mGopPages=EFI_SIZE_TO_PAGES(Bytes);mGopMode.FrameBufferBase=Address;
    mGopMode.FrameBufferSize=(UINTN)CR_DP_GOP_STRIDE*CR_DP_GOP_HEIGHT;
  }
  EFI_STATUS S=QualifyCache();if(S!=EFI_SUCCESS)return S;
  if(mBltConfig)return EFI_SUCCESS;
  mGopInfo=(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION){.Version=0,.HorizontalResolution=CR_DP_GOP_WIDTH,.VerticalResolution=CR_DP_GOP_HEIGHT,
    .PixelFormat=PixelBlueGreenRedReserved8BitPerColor,.PixelsPerScanLine=CR_DP_GOP_WIDTH};
  mGopMode.MaxMode=1;mGopMode.Mode=0;mGopMode.Info=&mGopInfo;mGopMode.SizeOfInfo=sizeof(mGopInfo);
  mBltConfigSize=0;
  RETURN_STATUS R=FrameBufferBltConfigure((VOID *)(UINTN)mGopMode.FrameBufferBase,&mGopInfo,NULL,&mBltConfigSize);
  if(R!=RETURN_BUFFER_TOO_SMALL)return EFI_UNSUPPORTED;
  mBltConfig=AllocatePool(mBltConfigSize);if(!mBltConfig)return EFI_OUT_OF_RESOURCES;
  R=FrameBufferBltConfigure((VOID *)(UINTN)mGopMode.FrameBufferBase,&mGopInfo,mBltConfig,&mBltConfigSize);
  if(R!=RETURN_SUCCESS){FreePool(mBltConfig);mBltConfig=NULL;return EFI_UNSUPPORTED;}
  mGop.QueryMode=Query;mGop.SetMode=SetMode;mGop.Blt=Blt;mGop.Mode=&mGopMode;
  /* This is an independent console surface. GraphicsConsole can retain the
   * current GOP mode and clear only its centered text window, so initialize
   * all pixels before publication instead of leaving DEBUG bars in margins. */
  return SetMode(&mGop,0);
}
EFI_STATUS CrDpGopGetFramebuffer(EFI_PHYSICAL_ADDRESS *Fb,UINT32 *W,UINT32 *H,UINT32 *Stride){
  if(!Fb||!W||!H||!Stride)return EFI_INVALID_PARAMETER;if(!mGopPages||!mBltConfig||!mGopCacheReady)return EFI_NOT_READY;
  *Fb=mGopMode.FrameBufferBase;*W=CR_DP_GOP_WIDTH;*H=CR_DP_GOP_HEIGHT;*Stride=CR_DP_GOP_STRIDE;return EFI_SUCCESS;
}
EFI_STATUS CrDpGopPublish(BOOLEAN *New){
  if(!New)return EFI_INVALID_PARAMETER;*New=FALSE;
  if(mGopExiting||!mBltConfig||!mGopCacheReady)return EFI_NOT_READY;if(mGopPublished)return EFI_SUCCESS;
  if(mGopPublicationPending)return EFI_ACCESS_DENIED;
  EFI_STATUS S=gBS->InstallMultipleProtocolInterfaces(&mGopHandle,&gEfiDevicePathProtocolGuid,&mGopPath,
    &gEfiGraphicsOutputProtocolGuid,&mGop,NULL);
  /* Retain storage/module on an uncertain publication instead of freeing a
   * mode/framebuffer pointer that a protocol callback could have observed. */
  if(S!=EFI_SUCCESS){if(mGopHandle!=NULL)mGopPublicationPending=TRUE;return S;}
  mGopPublished=TRUE;*New=TRUE;return EFI_SUCCESS;
}
EFI_STATUS CrDpGopConnect(VOID){
  if(!mGopPublished||mGopExiting)return EFI_NOT_READY;
  if(!FixedPcdGetBool(PcdPlatformDisplayPortConsoleEnable))return EFI_UNSUPPORTED;
  /* BDS removes a path when its hot-plug GOP is absent. Restore this board's
   * exact selected path before ConPlatform tests ConOut/ErrOut. Each successful
   * append occurs once per published lifetime; driver retries do not poll or
   * rewrite persistent variables, and other console instances are retained. */
  EFI_STATUS S;
  if(!mGopConOutAdded){S=EfiBootManagerUpdateConsoleVariable(ConOut,(VOID *)&mGopPath,NULL);
    if(S!=EFI_SUCCESS)return S;mGopConOutAdded=TRUE;}
  if(!mGopErrOutAdded){S=EfiBootManagerUpdateConsoleVariable(ErrOut,(VOID *)&mGopPath,NULL);
    if(S!=EFI_SUCCESS)return S;mGopErrOutAdded=TRUE;}
  S=gBS->ConnectController(mGopHandle,NULL,NULL,TRUE);
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *Text=NULL;
  EFI_STATUS Q=gBS->HandleProtocol(mGopHandle,&gEfiSimpleTextOutProtocolGuid,(VOID **)&Text);
  if(Q!=EFI_SUCCESS||!Text||!Text->OutputString)return S==EFI_SUCCESS?(Q==EFI_SUCCESS?EFI_NOT_READY:Q):S;
  VOID *Marker=NULL;Q=gBS->HandleProtocol(mGopHandle,&gEfiConsoleOutDeviceGuid,&Marker);
  if(Q!=EFI_SUCCESS)return Q; /* A selected marker has a valid NULL interface. */
  if(!gBS->OpenProtocolInformation)return EFI_NOT_READY;
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *Entries=NULL;UINTN Count=0;
  Q=gBS->OpenProtocolInformation(mGopHandle,&gEfiConsoleOutDeviceGuid,&Entries,&Count);
  BOOLEAN Bound=FALSE;
  /* Shell ConsoleLogger substitutes its image handle in the system table
   * while forwarding to the original ConSplitter. Qualify the actual console
   * driver/child relationship rather than that temporary wrapper handle. */
  if(Q==EFI_SUCCESS&&Entries){for(UINTN I=0;I<Count&&!Bound;++I){
    if(!(Entries[I].Attributes&EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER)||!Entries[I].OpenCount||
      !Entries[I].AgentHandle||!Entries[I].ControllerHandle||Entries[I].ControllerHandle==mGopHandle)continue;
    BOOLEAN Driver=FALSE;
    for(UINTN J=0;J<Count;++J)
      if((Entries[J].Attributes&EFI_OPEN_PROTOCOL_BY_DRIVER)&&Entries[J].OpenCount&&
        Entries[J].AgentHandle==Entries[I].AgentHandle&&Entries[J].ControllerHandle==mGopHandle)Driver=TRUE;
    if(!Driver)continue;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *Child=NULL;
    EFI_STATUS ChildStatus=gBS->HandleProtocol(Entries[I].ControllerHandle,&gEfiSimpleTextOutProtocolGuid,(VOID **)&Child);
    if(ChildStatus==EFI_SUCCESS&&Child&&Child->OutputString)Bound=TRUE;
  }}
  if(Entries)FreePool(Entries);
  return Bound?EFI_SUCCESS:Q==EFI_SUCCESS?EFI_NOT_READY:Q;
}
BOOLEAN CrDpGopPublished(VOID){return mGopPublished||mGopPublicationPending;}
VOID CrDpGopExit(VOID){mGopExiting=TRUE;}
BOOLEAN CrDpGopFrozen(VOID){return mGopExiting&&!mGopBltBusy;}
BOOLEAN CrDpGopAllocation(EFI_PHYSICAL_ADDRESS *Fb,UINT64 *Bytes,BOOLEAN *Prepared){
  if(!Fb||!Bytes||!Prepared)return FALSE;
  UINT64 Capacity=0;BOOLEAN Retained=CrDpReservationAllocation(Fb,&Capacity);
  *Bytes=Retained?(UINTN)CR_DP_GOP_STRIDE*CR_DP_GOP_HEIGHT:0;
  *Prepared=mGopCacheReady&&mBltConfig!=NULL&&!mGopPublicationPending;return Retained;
}
EFI_STATUS CrDpGopRelease(VOID){
  if(mGopExiting||CrDpGopPublished()||mGopBltBusy)return EFI_ACCESS_DENIED;
  if(mGopPages){
    if(mGopAttributesAttempted){EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;mGopCacheReady=FALSE;
      EFI_STATUS S=GetAllocationDescriptor(&D);if(S!=EFI_SUCCESS)return S;
      S=gDS->SetMemorySpaceAttributes(mGopMode.FrameBufferBase,EFI_PAGES_TO_SIZE(mGopPages),mGopOriginalAttributes);
      if(S!=EFI_SUCCESS)return EFI_ERROR(S)?S:EFI_NOT_READY;
      S=GetAllocationDescriptor(&D);if(S!=EFI_SUCCESS)return S;
      if(D.Attributes!=mGopOriginalAttributes)return EFI_DEVICE_ERROR;
      mGopAttributesAttempted=FALSE; /* No attribute IO after a release attempt. */
    }
  }
  EFI_STATUS S=CrDpReservationRelease();if(S!=EFI_SUCCESS)return S;
  mGopPages=0;
  if(mBltConfig){FreePool(mBltConfig);mBltConfig=NULL;}
  mBltConfigSize=0;mGopAttributesSaved=mGopAttributesAttempted=mGopCacheReady=FALSE;mGopOriginalAttributes=0;
  ZeroMem(&mGopMode,sizeof(mGopMode));return EFI_SUCCESS;
}
