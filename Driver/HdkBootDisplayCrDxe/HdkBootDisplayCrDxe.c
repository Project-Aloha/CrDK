/* Native HDK boot scanout GOP, based on public Android/Linux DPU/DSI data.
 * Retains an admitted running DSI panel/DSI-to-HDMI pipeline and reserved
 * splash. No foreign clock/rail/PHY/bridge/stream configuration is rewritten.
 * Inactive or unqualified routes emit bounded MDSSCr evidence for cold-init.
 * A software page-table walk is diagnostic only: functional admission here
 * requires current identity/bypass routing for every nonsecure display SID.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/Hdk8350BootDisplayCrLib.h>
#include <Library/IoLib.h>
#include <Library/MdssBoot8250CrLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/PlatformMemoryMapLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/EFISmmuCrProtocol.h>
#include <Guid/EventGroup.h>
#include <Configuration/DisplayDevicePath.h>
typedef struct {
  UINT32 Width,Height,Stride,Source,Unpack,Red,Green,Blue,Reserved,Progress;
  UINT64 Device,Bytes;
} HDK_FRAME;
typedef struct {
  EFI_GRAPHICS_OUTPUT_PROTOCOL Gop;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE Mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION Info;
  FRAME_BUFFER_CONFIGURE *Blt;
  UINTN BltBytes;
  EFI_SMMU_CR_PROTOCOL *Smmu;
  CR_SMMU_DISPLAY_OBSERVATION Dma;
  HDK_FRAME Frame;
  EFI_HANDLE Handle;
  EFI_EVENT ExitEvent;
  BOOLEAN Busy,Frozen,Published;
} HDK_DISPLAY;
STATIC HDK_DISPLAY mDisplay;
STATIC EFI_DISPLAY_DEVICE_PATH mPath=ANDROMEDA_DISPLAY_DEVICE_PATH_INIT;
STATIC UINT32 Board(VOID){return FixedPcdGet32(PcdHdkDisplayBoard);}
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS||EFI_ERROR(S)?S:EFI_NOT_READY;}
STATIC EFI_STATUS Mapped(UINT64 A,UINT64 Bytes,EFI_GCD_MEMORY_TYPE Type){
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;
  EFI_STATUS S=Exact(gDS->GetMemorySpaceDescriptor(A,&D));if(S!=EFI_SUCCESS)return S;
  if(!Bytes||D.GcdMemoryType!=Type||D.BaseAddress>A||D.Length>MAX_UINT64-D.BaseAddress||
     A-D.BaseAddress>=D.Length||Bytes>D.Length-(A-D.BaseAddress)||
     (D.Attributes&(EFI_MEMORY_RP|EFI_MEMORY_WP|EFI_MEMORY_RO)))return EFI_ACCESS_DENIED;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Access(BOOLEAN Diagnostic){
  EFI_STATUS S=Mapped(0xaf00000,0x20000,EfiGcdMemoryTypeMemoryMappedIo);
  if(S!=EFI_SUCCESS)return S;
  S=Mapped(0x100000,0x1f0000,EfiGcdMemoryTypeMemoryMappedIo);if(S!=EFI_SUCCESS)return S;
  UINT32 Gdsc=MmioRead32(0xaf03000),Gcc=MmioRead32(Board()==8245?0x10b00c:0x127004);
  UINT32 Ahb=MmioRead32(Board()==8245?0xaf02080:0xaf0207c);
  if(Diagnostic)DEBUG((DEBUG_ERROR,"MDSSCr board=%u access GDSC=%08x GCC_AHB=%08x DISP_AHB=%08x\n",Board(),Gdsc,Gcc,Ahb));
  if(!(Gdsc&BIT31)||(Gdsc&BIT0)||!(Gcc&BIT0)||!(Ahb&BIT0)||(Ahb&BIT31))return EFI_NOT_READY;
  return Mapped(0xae00000,0x100000,EfiGcdMemoryTypeMemoryMappedIo);
}
STATIC CR_STATUS ReadPhysical(VOID *Context,UINT64 A,UINT32 *V){
  if(Context!=&mDisplay||!V||(A&3))return EFI_INVALID_PARAMETER;
  if(!((A>=0xae00000&&A<0xaf00000)||(A>=0xaf00000&&A<0xaf20000)||
       (A>=0x100000&&A<0x2f0000)))return EFI_ACCESS_DENIED;
  *V=MmioRead32((UINTN)A);return EFI_SUCCESS;
}
STATIC CR_STATUS ReadKona(VOID *C,CR_MDSS_BOOT_WINDOW W,UINT32 O,UINT32 *V){
  CONST UINT64 Bases[]={0xae00000,0xae01000,0xaeb0000,0xae94000,0xae94400,0xae96000,0xae96400,0xaeac000};
  CONST UINT32 Sizes[]={0x1000,0x8f000,0x2008,0x400,0x7c0,0x400,0x7c0,0x214};
  if((UINT32)W>=ARRAY_SIZE(Bases)||O>Sizes[W]-4)return EFI_INVALID_PARAMETER;
  return ReadPhysical(C,Bases[W]+O,V);
}
STATIC EFI_STATUS Probe(HDK_FRAME *F,BOOLEAN Diagnostic){
  ZeroMem(F,sizeof(*F));EFI_STATUS S=Access(Diagnostic);if(S!=EFI_SUCCESS)return S;
  if(Board()==8245){
    CR_MDSS_BOOT_OBSERVATION O;
    S=CrMdssBoot8250ReadInventory(&mDisplay,ReadKona,&O);
    if(S!=EFI_SUCCESS)return Exact(S);
    S=CrMdssBoot8250ProbePreservedRoute(&O);
    if(Diagnostic){
      DEBUG((DEBUG_ERROR,"MDSSCr command route DPU=%08x CTL0=%08x INTF=%08x DSC=%08x host=%08x PHY=%08x autorefresh=%08x line=%u route=%r\n",
        O.DpuVersion,O.Ctl[0].Top,O.Ctl[0].Intf,O.Ctl[0].Dsc,O.HostControl[0],O.PhyControl[0],O.Intf[1].AutoRefresh,O.Intf[1].LineCount,S));
      for(UINT32 I=0;I<8;++I)if(O.Sspp[I].SourceSize||O.Sspp[I].Address[0])
        DEBUG((DEBUG_ERROR,"MDSSCr source%u size=%08x addr=%08x stride=%08x fmt=%08x unpack=%08x op=%08x\n",
          I,O.Sspp[I].SourceSize,O.Sspp[I].Address[0],O.Sspp[I].Stride[0],O.Sspp[I].Format,O.Sspp[I].Unpack,O.Sspp[I].OpMode));
    }
    if(S!=EFI_SUCCESS)return Exact(S);
    *F=(HDK_FRAME){O.Width,O.Height,O.Stride,O.SourceOffset,O.FrameUnpack,
      O.PixelRedMask,O.PixelGreenMask,O.PixelBlueMask,O.PixelReservedMask,
      O.Intf[1].LineCount,O.FrameDevice,O.FrameBytes};
  }else if(Board()==8350){
    CR_video route_DISPLAY_OBSERVATION O;CR_video route_DISPLAY_IO Io={&mDisplay,ReadPhysical};
    S=CrHdk8350DisplayReadInventory(&Io,&O);if(S!=EFI_SUCCESS)return Exact(S);
    S=CrHdk8350DisplayAdmitPreserved(&O);
    if(Diagnostic){
      DEBUG((DEBUG_ERROR,"MDSSCr video route DSI-to-LT9611UXC DPU=%08x CTL0=%08x INTF=%08x host=%08x PHY=%08x video=%08x frame=%u line=%u route=%r\n",
        O.Raw.DpuVersion,O.Raw.Ctl[0].Top,O.Raw.Ctl[0].Intf,O.Raw.Host[0].Control,O.Raw.Phy[0].Control,O.Raw.Intf[1].Enable,O.Raw.Intf[1].FrameCount,O.Raw.Intf[1].LineCount,S));
      for(UINT32 I=0;I<8;++I)if(O.Raw.Source[I].SourceSize||O.Raw.Source[I].Address[0])
        DEBUG((DEBUG_ERROR,"MDSSCr source%u size=%08x addr=%08x stride=%08x fmt=%08x unpack=%08x op=%08x\n",
          I,O.Raw.Source[I].SourceSize,O.Raw.Source[I].Address[0],O.Raw.Source[I].Stride[0],O.Raw.Source[I].Format,O.Raw.Source[I].Unpack,O.Raw.Source[I].OpMode));
    }
    if(S!=EFI_SUCCESS)return Exact(S);
    *F=(HDK_FRAME){O.Width,O.Height,O.Stride,O.SourceOffset,O.Unpack,
      O.RedMask,O.GreenMask,O.BlueMask,O.ReservedMask,O.Raw.Intf[1].FrameCount,O.Device,O.Bytes};
  }else return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}
STATIC BOOLEAN SameFrame(CONST HDK_FRAME *A,CONST HDK_FRAME *B){
  return A->Width==B->Width&&A->Height==B->Height&&A->Stride==B->Stride&&
    A->Source==B->Source&&A->Unpack==B->Unpack&&A->Red==B->Red&&A->Green==B->Green&&
    A->Blue==B->Blue&&A->Reserved==B->Reserved&&A->Device==B->Device&&A->Bytes==B->Bytes;
}
STATIC EFI_STATUS CpuFrame(UINT64 Physical,UINT64 Bytes){
  ARM_MEMORY_REGION_DESCRIPTOR_EX *Map=GetPlatformMemoryMap();
  if(!Map||!Bytes)return EFI_ACCESS_DENIED;
  for(UINTN I=0;I<MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT&&Map[I].Length;++I){
    ARM_MEMORY_REGION_DESCRIPTOR_EX *R=&Map[I];
    if(R->Address<=Physical&&Physical-R->Address<R->Length&&Bytes<=R->Length-(Physical-R->Address)){
      if(R->HobOption!=AddMem||R->MemoryType!=EfiReservedMemoryType||
         (R->ArmAttributes!=UNCACHED_UNBUFFERED_XN&&R->ArmAttributes!=WRITE_THROUGH_XN&&
          R->ArmAttributes!=WRITE_BACK_XN))return EFI_ACCESS_DENIED;
      return Mapped(Physical,Bytes,EfiGcdMemoryTypeReserved);
    }
  }
  return EFI_ACCESS_DENIED;
}
STATIC EFI_STATUS IdentityDma(HDK_FRAME *F,CR_SMMU_DISPLAY_OBSERVATION *O){
  /* No CPU table dereference: translated routes require a separate hardware
   * ATS/cache and table-lifetime contract before becoming a functional GOP. */
  EFI_STATUS S=Exact(mDisplay.Smmu->QueryInheritedDisplay(mDisplay.Smmu,F->Device,F->Bytes,NULL,NULL,O));
  if(S!=EFI_SUCCESS)return S;
  for(UINT32 I=0;I<O->Count;++I)if(((O->Streams[I].S2cr>>16)&3)==0&&(O->Streams[I].Sctlr&1))return EFI_UNSUPPORTED;
  if(O->PhysicalAddress!=F->Device)return EFI_UNSUPPORTED;
  return CpuFrame(O->PhysicalAddress,O->Bytes);
}
STATIC EFI_STATUS Current(VOID){
  HDK_FRAME F;EFI_STATUS S=Probe(&F,FALSE);if(S!=EFI_SUCCESS)return S;
  if(!SameFrame(&F,&mDisplay.Frame))return EFI_NOT_READY;
  CR_SMMU_DISPLAY_OBSERVATION O;S=IdentityDma(&F,&O);if(S!=EFI_SUCCESS)return S;
  return CompareMem(&O,&mDisplay.Dma,sizeof(O))?EFI_NOT_READY:EFI_SUCCESS;
}
STATIC EFI_STATUS Enter(VOID){
  if(mDisplay.Frozen)return EFI_ACCESS_DENIED;
  if(EfiGetCurrentTpl()>TPL_NOTIFY)return EFI_UNSUPPORTED;
  EFI_TPL T=gBS->RaiseTPL(TPL_NOTIFY);EFI_STATUS S=mDisplay.Frozen?EFI_ACCESS_DENIED:mDisplay.Busy?EFI_NOT_READY:EFI_SUCCESS;
  if(S==EFI_SUCCESS)mDisplay.Busy=TRUE;gBS->RestoreTPL(T);return S;
}
STATIC EFI_STATUS Leave(EFI_STATUS S){EFI_TPL T=gBS->RaiseTPL(TPL_NOTIFY);mDisplay.Busy=FALSE;gBS->RestoreTPL(T);return S;}
STATIC EFI_STATUS EFIAPI Query(EFI_GRAPHICS_OUTPUT_PROTOCOL *G,UINT32 M,UINTN *N,EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **I){
  if(G!=&mDisplay.Gop||!N||!I)return EFI_INVALID_PARAMETER;*N=0;*I=NULL;if(M)return EFI_UNSUPPORTED;
  EFI_STATUS S=Enter();if(S!=EFI_SUCCESS)return S;
  *I=AllocateCopyPool(sizeof(mDisplay.Info),&mDisplay.Info);if(!*I)return Leave(EFI_OUT_OF_RESOURCES);
  *N=sizeof(mDisplay.Info);return Leave(EFI_SUCCESS);
}
STATIC EFI_STATUS BltOwned(EFI_GRAPHICS_OUTPUT_BLT_PIXEL *B,EFI_GRAPHICS_OUTPUT_BLT_OPERATION O,
    UINTN Sx,UINTN Sy,UINTN Dx,UINTN Dy,UINTN W,UINTN H,UINTN Delta){
  if((UINT32)O>=EfiGraphicsOutputBltOperationMax||!W||!H)return EFI_INVALID_PARAMETER;
  if(O!=EfiBltVideoToBltBuffer){
    if(Dx>=mDisplay.Frame.Width||Dy>=mDisplay.Frame.Height||W>mDisplay.Frame.Width-Dx||H>mDisplay.Frame.Height-Dy)
      return EFI_INVALID_PARAMETER;
  }
  if(O==EfiBltVideoToBltBuffer||O==EfiBltVideoToVideo){
    if(Sx>=mDisplay.Frame.Width||Sy>=mDisplay.Frame.Height||W>mDisplay.Frame.Width-Sx||H>mDisplay.Frame.Height-Sy)
      return EFI_INVALID_PARAMETER;
  }
  if(O==EfiBltVideoFill&&!B)return EFI_INVALID_PARAMETER;
  if(O==EfiBltBufferToVideo||O==EfiBltVideoToBltBuffer){
    UINTN X=O==EfiBltBufferToVideo?Sx:Dx,Y=O==EfiBltBufferToVideo?Sy:Dy;
    if(!B||W>MAX_UINTN/4||X>MAX_UINTN-W||Y>MAX_UINTN-H)return EFI_INVALID_PARAMETER;
    if(!Delta){if(X)return EFI_INVALID_PARAMETER;Delta=W*4;}
    if((Delta&3)||X+W>Delta/4||Y+H>MAX_UINTN/Delta)return EFI_INVALID_PARAMETER;
    UINTN Span=(Y+H-1)*Delta+(X+W)*4;
    if((UINTN)B>MAX_UINTN-Span)return EFI_INVALID_PARAMETER;
  }
  EFI_STATUS S=Current();if(S!=EFI_SUCCESS)return S;
  RETURN_STATUS R;
  if(O==EfiBltVideoToVideo){
    /* MU's current reverse-row implementation starts one row beyond the
       rectangle. Copy from the last admitted row; CopyMem handles horizontal
       overlap, and row direction preserves vertical overlap. */
    UINT8 *Frame=(UINT8 *)(UINTN)mDisplay.Mode.FrameBufferBase;
    UINTN Source=Sy*mDisplay.Frame.Stride+Sx*4,Destination=Dy*mDisplay.Frame.Stride+Dx*4;
    for(UINTN I=0;I<H;++I){UINTN Row=Destination>Source?H-1-I:I;
      CopyMem(Frame+Destination+Row*mDisplay.Frame.Stride,
          Frame+Source+Row*mDisplay.Frame.Stride,W*4);}
    R=RETURN_SUCCESS;
  }else R=FrameBufferBlt(mDisplay.Blt,B,O,Sx,Sy,Dx,Dy,W,H,Delta);
  if(RETURN_ERROR(R))return EFI_INVALID_PARAMETER;
  if(O!=EfiBltVideoToBltBuffer)
    WriteBackDataCacheRange((VOID *)(UINTN)mDisplay.Mode.FrameBufferBase,mDisplay.Mode.FrameBufferSize);
  MemoryFence();return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *G,EFI_GRAPHICS_OUTPUT_BLT_PIXEL *B,
    EFI_GRAPHICS_OUTPUT_BLT_OPERATION O,UINTN Sx,UINTN Sy,UINTN Dx,UINTN Dy,UINTN W,UINTN H,UINTN Delta){
  if(G!=&mDisplay.Gop)return EFI_INVALID_PARAMETER;EFI_STATUS S=Enter();if(S!=EFI_SUCCESS)return S;
  return Leave(BltOwned(B,O,Sx,Sy,Dx,Dy,W,H,Delta));
}
STATIC EFI_STATUS EFIAPI SetMode(EFI_GRAPHICS_OUTPUT_PROTOCOL *G,UINT32 M){
  if(G!=&mDisplay.Gop)return EFI_INVALID_PARAMETER;if(M)return EFI_UNSUPPORTED;
  EFI_STATUS S=Enter();if(S!=EFI_SUCCESS)return S;EFI_GRAPHICS_OUTPUT_BLT_PIXEL Black={0};
  return Leave(BltOwned(&Black,EfiBltVideoFill,0,0,0,0,mDisplay.Frame.Width,mDisplay.Frame.Height,0));
}
STATIC VOID EFIAPI Exit(EFI_EVENT E,VOID *C){
  if(E!=mDisplay.ExitEvent||C!=&mDisplay)return;
  mDisplay.Frozen=TRUE;MemoryFence();
}
EFI_STATUS EFIAPI HdkBootDisplayEntry(EFI_HANDLE Image,EFI_SYSTEM_TABLE *System){
  (VOID)Image;(VOID)System;EFI_STATUS S;VOID *Existing=NULL;
  if(mDisplay.Published||mDisplay.Busy||mDisplay.ExitEvent||mDisplay.Handle||mDisplay.Blt)return EFI_ALREADY_STARTED;
  if(Board()!=8245&&Board()!=8350)return EFI_UNSUPPORTED;
  if(gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid,NULL,&Existing)==EFI_SUCCESS)return EFI_ALREADY_STARTED;
  ZeroMem(&mDisplay,sizeof(mDisplay));mDisplay.Busy=TRUE;
  S=Exact(gBS->LocateProtocol(&gEfiSmmuCrProtocolGuid,NULL,(VOID **)&mDisplay.Smmu));
  if(S!=EFI_SUCCESS)goto Failed;
  if(!mDisplay.Smmu||mDisplay.Smmu->Revision<EFI_SMMU_CR_DISPLAY_OBSERVATION_REVISION||
      !mDisplay.Smmu->QueryInheritedDisplay){S=EFI_UNSUPPORTED;goto Failed;}
  S=Probe(&mDisplay.Frame,TRUE);if(S!=EFI_SUCCESS)goto Failed;
  S=IdentityDma(&mDisplay.Frame,&mDisplay.Dma);
  DEBUG((DEBUG_ERROR,"MDSSCr inherited framebuffer device=%Lx bytes=%Lx SMMU=%r\n",mDisplay.Frame.Device,mDisplay.Frame.Bytes,S));
  if(S!=EFI_SUCCESS)goto Failed;
  /* Actual pipeline progress, sampled at bounded intervals; time alone is
   * never interpreted as a completed frame or permission to publish GOP. */
  BOOLEAN Progress=FALSE;
  for(UINT32 I=0;I<12;++I){HDK_FRAME F;MicroSecondDelay(3000);S=Probe(&F,FALSE);
    if(S!=EFI_SUCCESS||!SameFrame(&F,&mDisplay.Frame)){if(S==EFI_SUCCESS)S=EFI_NOT_READY;goto Failed;}
    if(F.Progress!=mDisplay.Frame.Progress){Progress=TRUE;break;}}
  if(!Progress){S=EFI_TIMEOUT;goto Failed;}
  S=Current();if(S!=EFI_SUCCESS)goto Failed;
  if(mDisplay.Frozen){S=EFI_ACCESS_DENIED;goto Failed;}
  mDisplay.Info=(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION){.HorizontalResolution=mDisplay.Frame.Width,
    .VerticalResolution=mDisplay.Frame.Height,.PixelFormat=PixelBitMask,.PixelsPerScanLine=mDisplay.Frame.Stride/4,
    .PixelInformation={mDisplay.Frame.Red,mDisplay.Frame.Green,mDisplay.Frame.Blue,mDisplay.Frame.Reserved}};
  mDisplay.Mode=(EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE){.MaxMode=1,.Info=&mDisplay.Info,.SizeOfInfo=sizeof(mDisplay.Info),
    .FrameBufferBase=mDisplay.Dma.PhysicalAddress,.FrameBufferSize=(UINTN)mDisplay.Frame.Bytes};
  mDisplay.Gop=(EFI_GRAPHICS_OUTPUT_PROTOCOL){Query,SetMode,Blt,&mDisplay.Mode};
  RETURN_STATUS R=FrameBufferBltConfigure((VOID *)(UINTN)mDisplay.Mode.FrameBufferBase,&mDisplay.Info,NULL,&mDisplay.BltBytes);
  if(R!=RETURN_BUFFER_TOO_SMALL){S=EFI_DEVICE_ERROR;goto Failed;}
  mDisplay.Blt=AllocatePool(mDisplay.BltBytes);if(!mDisplay.Blt){S=EFI_OUT_OF_RESOURCES;goto Failed;}
  R=FrameBufferBltConfigure((VOID *)(UINTN)mDisplay.Mode.FrameBufferBase,&mDisplay.Info,mDisplay.Blt,&mDisplay.BltBytes);
  if(RETURN_ERROR(R)){S=EFI_DEVICE_ERROR;goto Failed;}
  S=Exact(gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Exit,&mDisplay,&gEfiEventExitBootServicesGuid,&mDisplay.ExitEvent));
  if(S!=EFI_SUCCESS)goto Failed;
  S=Current();if(S!=EFI_SUCCESS)goto Failed;
  if(mDisplay.Frozen){S=EFI_ACCESS_DENIED;goto Failed;}
  S=Exact(gBS->InstallMultipleProtocolInterfaces(&mDisplay.Handle,&gEfiDevicePathProtocolGuid,&mPath,
     &gEfiGraphicsOutputProtocolGuid,&mDisplay.Gop,NULL));
  if(S!=EFI_SUCCESS)goto Failed;
  mDisplay.Published=TRUE;mDisplay.Busy=FALSE;
  DEBUG((DEBUG_ERROR,"MDSSCr board=%u GOP ready %ux%u stride=%u physical=%Lx reserved=%Lx\n",
    Board(),mDisplay.Frame.Width,mDisplay.Frame.Height,mDisplay.Frame.Stride,mDisplay.Mode.FrameBufferBase,mDisplay.Frame.Bytes));
  return EFI_SUCCESS;
Failed:
  DEBUG((DEBUG_ERROR,"MDSSCr board=%u GOP admission=%r; inherited display retained\n",Board(),S));
  if(mDisplay.Handle){
    /* A returned publication handle may still reference our protocols. Keep
       the complete callback owner resident rather than guessing rollback. */
    mDisplay.Frozen=TRUE;return EFI_SUCCESS;
  }
  if(mDisplay.ExitEvent){EFI_STATUS C=Exact(gBS->CloseEvent(mDisplay.ExitEvent));if(C!=EFI_SUCCESS)return EFI_SUCCESS;mDisplay.ExitEvent=NULL;}
  if(mDisplay.Blt){FreePool(mDisplay.Blt);mDisplay.Blt=NULL;}
  mDisplay.Busy=FALSE;return S;
}
EFI_STATUS EFIAPI HdkBootDisplayUnload(EFI_HANDLE Image){
  (VOID)Image;
  /* A published framebuffer and its inherited hardware remain reserved for
   * firmware consumers and OS display takeover; never unload their callbacks. */
  return mDisplay.Published||mDisplay.ExitEvent||mDisplay.Busy?EFI_ACCESS_DENIED:EFI_SUCCESS;
}
