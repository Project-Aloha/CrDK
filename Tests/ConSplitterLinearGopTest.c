/** Actual ConSplitter graphics contract with two independent physical GOPs. */
#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleTextOut.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static EFI_GUID Selected;
#define _PCD_VALUE_PcdConSplitterPrimaryGopGuid (&Selected)
#include "../Override/MU_BASECORE/MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitter.h"
static EFI_BOOT_SERVICES Services;
EFI_BOOT_SERVICES *gBS=&Services;
EFI_GUID gEfiDevicePathProtocolGuid,gEfiGraphicsOutputProtocolGuid;
EFI_GUID gEfiConsoleOutDeviceGuid,gEfiSimpleTextOutProtocolGuid;
TEXT_OUT_SPLITTER_PRIVATE_DATA mConOut;
typedef struct {VENDOR_DEVICE_PATH Vendor;EFI_DEVICE_PATH_PROTOCOL End;} DISPLAY_PATH;
static DISPLAY_PATH Paths[2];
static EFI_STATUS PathStatus,StopStatus;
static BOOLEAN StopMustBeClosed;
static BOOLEAN StopReenter;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Texts[2];
static EFI_SIMPLE_TEXT_OUTPUT_MODE TextModes[2];
static UINTN TextCalls[2];
static UINTN Uninstalls;
static EFI_STATUS QueryStatus,SetStatus,BltStatus;
EFI_STATUS ConSplitterAddGraphicsOutputMode(TEXT_OUT_SPLITTER_PRIVATE_DATA *,EFI_GRAPHICS_OUTPUT_PROTOCOL *,EFI_HANDLE);

VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memmove(D,S,N);}
VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}
VOID *EFIAPI AllocatePool(UINTN N){return malloc(N);}
VOID EFIAPI FreePool(VOID *P){free(P);}
BOOLEAN EFIAPI DebugAssertEnabled(VOID){return TRUE;}
VOID EFIAPI DebugAssert(CONST CHAR8 *F,UINTN L,CONST CHAR8 *D){fprintf(stderr,"%s:%zu %s\n",F,(size_t)L,D);abort();}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){(void)L;return FALSE;}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){(void)L;(void)F;}
BOOLEAN EFIAPI CompareGuid(CONST GUID *A,CONST GUID *B){return memcmp(A,B,sizeof(*A))==0;}
EFI_STATUS ConSplitterAddOutputMode(TEXT_OUT_SPLITTER_PRIVATE_DATA *P,EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *T){(void)T;P->TextOutMode.MaxMode=1;return EFI_SUCCESS;}
VOID ConSplitterSyncOutputMode(TEXT_OUT_SPLITTER_PRIVATE_DATA *P,EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *T){(void)P;(void)T;}
EFI_STATUS ConSplitterGetIntersectionBetweenConOutAndStrErr(VOID){return EFI_SUCCESS;}
VOID TextOutSetMode(TEXT_OUT_SPLITTER_PRIVATE_DATA *P,UINTN M){P->TextOutMode.Mode=(INT32)M;}
static EFI_STATUS EFIAPI HandleProtocol(EFI_HANDLE H,EFI_GUID *G,VOID **Out){
  assert(G==&gEfiDevicePathProtocolGuid);UINTN I=(UINTN)H-1;assert(I<2);
  if(PathStatus!=EFI_SUCCESS)return PathStatus;*Out=&Paths[I];return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Uninstall(EFI_HANDLE H,EFI_GUID *G,VOID *I){
  (void)H;(void)G;(void)I;++Uninstalls;return EFI_SUCCESS;
}
EFI_STATUS ConSplitterStop(EFI_DRIVER_BINDING_PROTOCOL *B,EFI_HANDLE H,EFI_HANDLE V,EFI_GUID *C,EFI_GUID *G,VOID **Out){
  (void)B;(void)V;(void)C;(void)G;UINTN I=(UINTN)H-1;assert(I<2);
  if(StopMustBeClosed)assert(!mConOut.PrimaryGraphicsOutput&&!mConOut.GraphicsOutput.Mode->FrameBufferBase);
  if(StopReenter){assert(mConOut.PrimaryGopController==H);StopReenter=FALSE;
    TEXT_OUT_AND_GOP_DATA *List=mConOut.TextOutList;UINTN N=mConOut.CurrentNumberOfConsoles;
    assert(N<3);List[N]=List[N-1];++mConOut.CurrentNumberOfConsoles;
    assert(ConSplitterAddGraphicsOutputMode(&mConOut,List[N].GraphicsOutput,(EFI_HANDLE)2)==EFI_SUCCESS);
    assert(!mConOut.PrimaryGraphicsOutput&&mConOut.PrimaryGopController==H&&!mConOut.GraphicsOutput.Mode->FrameBufferBase);
  }
  *Out=&Texts[I];return StopStatus;
}
static EFI_STATUS EFIAPI TextAttribute(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *T,UINTN A){(void)T;(void)A;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI OutputString(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *T,CHAR16 *S){
  assert(S&&*S);UINTN I=T==&Texts[0]?0:1;assert(T==&Texts[I]);++TextCalls[I];return EFI_SUCCESS;
}

EFI_STATUS EFIAPI ConSplitterGraphicsOutputQueryMode(EFI_GRAPHICS_OUTPUT_PROTOCOL *,UINT32,UINTN *,EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **);
EFI_STATUS EFIAPI ConSplitterGraphicsOutputSetMode(EFI_GRAPHICS_OUTPUT_PROTOCOL *,UINT32);
EFI_STATUS EFIAPI ConSplitterGraphicsOutputBlt(EFI_GRAPHICS_OUTPUT_PROTOCOL *,EFI_GRAPHICS_OUTPUT_BLT_PIXEL *,EFI_GRAPHICS_OUTPUT_BLT_OPERATION,UINTN,UINTN,UINTN,UINTN,UINTN,UINTN,UINTN);
#include "con_splitter_graphics_actual.inc"

typedef struct {
  EFI_GRAPHICS_OUTPUT_PROTOCOL Gop;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE Mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION Info;
  UINT32 *Pixels;
  UINTN Sets,Blts;
} DISPLAY;
static DISPLAY Displays[2];
static DISPLAY *Display(EFI_GRAPHICS_OUTPUT_PROTOCOL *G){
  for(UINTN I=0;I<2;++I)if(G==&Displays[I].Gop)return &Displays[I];
  assert(!"unexpected physical GOP");return NULL;
}
static EFI_STATUS EFIAPI Query(EFI_GRAPHICS_OUTPUT_PROTOCOL *G,UINT32 M,UINTN *N,EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info){
  DISPLAY *D=Display(G);if(M)return EFI_UNSUPPORTED;
  if(QueryStatus!=EFI_SUCCESS)return QueryStatus;
  *Info=malloc(sizeof(D->Info));assert(*Info);**Info=D->Info;*N=sizeof(D->Info);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Set(EFI_GRAPHICS_OUTPUT_PROTOCOL *G,UINT32 M){
  DISPLAY *D=Display(G);if(M)return EFI_UNSUPPORTED;
  ++D->Sets;D->Mode.FrameBufferBase=(UINTN)D->Pixels;D->Mode.FrameBufferSize=1920*1080*4;
  if(SetStatus!=EFI_SUCCESS){D->Mode.FrameBufferBase+=64;D->Mode.FrameBufferSize-=64;return SetStatus;}
  memset(D->Pixels,0,D->Mode.FrameBufferSize);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *G,EFI_GRAPHICS_OUTPUT_BLT_PIXEL *P,EFI_GRAPHICS_OUTPUT_BLT_OPERATION Op,
  UINTN Sx,UINTN Sy,UINTN Dx,UINTN Dy,UINTN W,UINTN H,UINTN Delta){
  DISPLAY *D=Display(G);(void)Sx;(void)Sy;(void)Delta;
  assert(Op==EfiBltVideoFill&&P&&Dx+W<=1920&&Dy+H<=1080);++D->Blts;
  if(BltStatus!=EFI_SUCCESS)return BltStatus;
  UINT32 V;memcpy(&V,P,4);for(UINTN Y=Dy;Y<Dy+H;++Y)for(UINTN X=Dx;X<Dx+W;++X)D->Pixels[Y*1920+X]=V;
  return EFI_SUCCESS;
}
static VOID Scenario(BOOLEAN RequireLinear,BOOLEAN SecondaryFirst){
  TEXT_OUT_SPLITTER_PRIVATE_DATA P={0};TEXT_OUT_AND_GOP_DATA List[3]={0};
  P.Signature=TEXT_OUT_SPLITTER_PRIVATE_DATA_SIGNATURE;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE Mode={0};EFI_GRAPHICS_OUTPUT_MODE_INFORMATION Info={0};
  TEXT_OUT_SPLITTER_QUERY_DATA TextInfo[2]={0};
  EFI_GUID Primary={0x9042A9DE,0x23DC,0x4A38,{0x96,0xFB,0x7A,0xDE,0xD0,0x80,0x51,0x6A}};
  Selected=RequireLinear?Primary:(EFI_GUID){0};
  P.TextOutQueryData=TextInfo;P.TextOutQueryDataCount=2;
  P.TextOutList=List;P.GraphicsOutput=(EFI_GRAPHICS_OUTPUT_PROTOCOL){ConSplitterGraphicsOutputQueryMode,ConSplitterGraphicsOutputSetMode,ConSplitterGraphicsOutputBlt,&Mode};
  Mode.Info=&Info;
  for(UINTN I=0;I<2;++I){DISPLAY *D=&Displays[I];*D=(DISPLAY){0};D->Pixels=calloc(1920*1080,4);assert(D->Pixels);
    D->Info=(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION){.HorizontalResolution=1920,.VerticalResolution=1080,
      .PixelFormat=PixelBlueGreenRedReserved8BitPerColor,.PixelsPerScanLine=1920};
    D->Mode=(EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE){.MaxMode=1,.Info=&D->Info,.SizeOfInfo=sizeof(D->Info),
      .FrameBufferBase=(UINTN)D->Pixels,.FrameBufferSize=1920*1080*4};
    D->Gop=(EFI_GRAPHICS_OUTPUT_PROTOCOL){Query,Set,Blt,&D->Mode};
    Paths[I]=(DISPLAY_PATH){.Vendor={.Header={HARDWARE_DEVICE_PATH,HW_VENDOR_DP,{sizeof(VENDOR_DEVICE_PATH),0}},.Guid=I?(EFI_GUID){.Data1=7}:Primary},
      .End={END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{sizeof(EFI_DEVICE_PATH_PROTOCOL),0}}};
  }
  PathStatus=EFI_SUCCESS;
  for(UINTN I=0;I<2;++I){Texts[I]=(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL){.OutputString=OutputString,.Mode=&TextModes[I]};TextCalls[I]=0;}
  P.TextOut=(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL){.OutputString=ConSplitterTextOutOutputString,.SetAttribute=TextAttribute,.Mode=&P.TextOutMode};
  for(UINTN N=0;N<2;++N){UINTN I=SecondaryFirst?1-N:N;DISPLAY *D=&Displays[I];
    List[N]=(TEXT_OUT_AND_GOP_DATA){.GraphicsOutput=&D->Gop,.TextOut=&Texts[I],.ControllerHandle=(EFI_HANDLE)(I+1)};P.CurrentNumberOfConsoles=N+1;
    assert(ConSplitterAddGraphicsOutputMode(&P,&D->Gop,(EFI_HANDLE)(I+1))==EFI_SUCCESS);
    if(!N&&!SecondaryFirst)assert(Info.PixelFormat==PixelBlueGreenRedReserved8BitPerColor&&Mode.FrameBufferBase==(UINTN)D->Pixels);
    if(!N&&SecondaryFirst&&RequireLinear)assert(!P.PrimaryGraphicsOutput&&!Mode.MaxMode&&!Mode.FrameBufferBase);
  }
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Out=NULL;UINTN Bytes=0;
  assert(P.GraphicsOutput.QueryMode(&P.GraphicsOutput,0,&Bytes,&Out)==EFI_SUCCESS&&Bytes==sizeof(*Out));
  assert(P.TextOut.OutputString(&P.TextOut,L"boot text")==EFI_SUCCESS&&TextCalls[0]==1&&TextCalls[1]==1);
  printf("actual two-GOP console: format=%u framebuffer=%llx size=%zu\n",Info.PixelFormat,(unsigned long long)Mode.FrameBufferBase,(size_t)Mode.FrameBufferSize);
  if(RequireLinear){assert(Info.PixelFormat==PixelBlueGreenRedReserved8BitPerColor);
    assert(Mode.FrameBufferBase==(UINTN)Displays[0].Pixels&&Mode.FrameBufferSize==1920*1080*4);
    assert(Out->PixelFormat==Info.PixelFormat&&Out->PixelsPerScanLine==1920);
  }else{assert(Info.PixelFormat==PixelBltOnly&&Out->PixelFormat==PixelBltOnly&&!Mode.FrameBufferBase&&!Mode.FrameBufferSize);}
  FreePool(Out);assert(P.GraphicsOutput.SetMode(&P.GraphicsOutput,0)==EFI_SUCCESS);
  if(RequireLinear)assert(Info.PixelFormat==PixelBlueGreenRedReserved8BitPerColor&&Mode.FrameBufferBase==(UINTN)Displays[0].Pixels);
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Pixel={1,2,3,0};
  assert(P.GraphicsOutput.Blt(&P.GraphicsOutput,&Pixel,EfiBltVideoFill,0,0,0,0,1,1,0)==EFI_SUCCESS);
  assert(Displays[0].Pixels[0]==0x00030201);
  if(RequireLinear){
    assert(Displays[1].Pixels[0]==0&&Displays[1].Sets==0&&Displays[1].Blts==0);
    ((UINT32 *)(UINTN)Mode.FrameBufferBase)[1]=0x11223344;assert(Displays[0].Pixels[1]==0x11223344&&Displays[1].Pixels[1]==0);
    assert(Displays[1].Gop.Blt(&Displays[1].Gop,&Pixel,EfiBltVideoFill,0,0,0,0,1,1,0)==EFI_SUCCESS);
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *OwnedInfo=Mode.Info;
    SetStatus=EFI_DEVICE_ERROR;assert(P.GraphicsOutput.SetMode(&P.GraphicsOutput,0)==EFI_DEVICE_ERROR);
    assert(Mode.Info==OwnedInfo&&Mode.Info!=Displays[0].Mode.Info&&Mode.FrameBufferBase==(UINTN)Displays[0].Pixels+64);
    assert(Mode.FrameBufferSize==1920*1080*4-64&&Displays[1].Sets==0);
    SetStatus=EFI_SUCCESS;assert(P.GraphicsOutput.SetMode(&P.GraphicsOutput,0)==EFI_SUCCESS);
    QueryStatus=EFI_DEVICE_ERROR;Out=(VOID *)1;Bytes=11;
    assert(P.GraphicsOutput.QueryMode(&P.GraphicsOutput,0,&Bytes,&Out)==EFI_DEVICE_ERROR&&Out==(VOID *)1&&Bytes==11);QueryStatus=EFI_SUCCESS;
    BltStatus=EFI_DEVICE_ERROR;UINTN Before=Displays[1].Blts;
    assert(P.GraphicsOutput.Blt(&P.GraphicsOutput,&Pixel,EfiBltVideoFill,0,0,0,0,1,1,0)==EFI_DEVICE_ERROR&&Displays[1].Blts==Before);BltStatus=EFI_SUCCESS;
    Displays[0].Mode.MaxMode=0;assert(ConSplitterSyncPrimaryGop(&P)==EFI_NOT_READY&&Mode.Info==OwnedInfo&&!Mode.FrameBufferBase);
    Displays[0].Mode.MaxMode=1;assert(ConSplitterSyncPrimaryGop(&P)==EFI_SUCCESS&&Mode.Info==OwnedInfo&&Mode.FrameBufferBase==(UINTN)Displays[0].Pixels);
    /* Exact selected identity: malformed/extended paths and positive status do not qualify. */
    assert(ConSplitterIsPrimaryGop((EFI_HANDLE)1));Paths[0].End.SubType=END_INSTANCE_DEVICE_PATH_SUBTYPE;
    assert(!ConSplitterIsPrimaryGop((EFI_HANDLE)1));Paths[0].End.SubType=END_ENTIRE_DEVICE_PATH_SUBTYPE;
    PathStatus=EFI_WARN_UNKNOWN_GLYPH;assert(!ConSplitterIsPrimaryGop((EFI_HANDLE)1));PathStatus=EFI_SUCCESS;
    /* Another selected-looking device stays counted without replacing ownership. */
    Paths[1].Vendor.Guid=Primary;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL AliasText=Texts[0];
    List[2]=(TEXT_OUT_AND_GOP_DATA){.GraphicsOutput=&Displays[0].Gop,.TextOut=&AliasText,.ControllerHandle=(EFI_HANDLE)2};
    P.CurrentNumberOfConsoles=3;assert(ConSplitterAddGraphicsOutputMode(&P,&Displays[0].Gop,(EFI_HANDLE)2)==EFI_SUCCESS);
    assert(ConSplitterTextOutDeleteDevice(&P,&AliasText)==EFI_SUCCESS&&P.CurrentNumberOfGraphicsOutput==2);
    assert(P.PrimaryGraphicsOutput==&Displays[0].Gop&&P.PrimaryGopController==(EFI_HANDLE)1&&Mode.FrameBufferBase==(UINTN)Displays[0].Pixels);
    List[2]=(TEXT_OUT_AND_GOP_DATA){.GraphicsOutput=&Displays[1].Gop,.TextOut=&Texts[1],.ControllerHandle=(EFI_HANDLE)2};
    P.CurrentNumberOfConsoles=3;assert(ConSplitterAddGraphicsOutputMode(&P,&Displays[1].Gop,(EFI_HANDLE)2)==EFI_SUCCESS);
    assert(P.CurrentNumberOfGraphicsOutput==3&&P.PrimaryGraphicsOutput==&Displays[0].Gop&&P.PrimaryGopController==(EFI_HANDLE)1);
    assert(ConSplitterTextOutDeleteDevice(&P,&Texts[1])==EFI_SUCCESS&&P.CurrentNumberOfGraphicsOutput==2);
    assert(Mode.FrameBufferBase==(UINTN)Displays[0].Pixels);
    assert(ConSplitterTextOutDeleteDevice(&P,&Texts[1])==EFI_SUCCESS&&P.CurrentNumberOfGraphicsOutput==1);
    assert(Mode.FrameBufferBase==(UINTN)Displays[0].Pixels); /* 2->1 retains exact primary. */
    List[1]=(TEXT_OUT_AND_GOP_DATA){.GraphicsOutput=&Displays[1].Gop,.TextOut=&Texts[1],.ControllerHandle=(EFI_HANDLE)2};P.CurrentNumberOfConsoles=2;
    assert(ConSplitterAddGraphicsOutputMode(&P,&Displays[1].Gop,(EFI_HANDLE)2)==EFI_SUCCESS);
    /* Stop admission closes before the parent marker is released; failed stop restores actual ownership. */
    mConOut=P;mConOut.TextOut.Mode=&mConOut.TextOutMode;StopMustBeClosed=TRUE;StopStatus=EFI_DEVICE_ERROR;StopReenter=TRUE;
    assert(ConSplitterConOutDriverBindingStop(NULL,(EFI_HANDLE)1,1,NULL)==EFI_DEVICE_ERROR);
    assert(mConOut.PrimaryGraphicsOutput==&Displays[0].Gop&&mConOut.PrimaryGopController==(EFI_HANDLE)1&&Mode.FrameBufferBase==(UINTN)Displays[0].Pixels);
    assert(mConOut.CurrentNumberOfGraphicsOutput==3);assert(ConSplitterTextOutDeleteDevice(&mConOut,&Texts[1])==EFI_SUCCESS);
    StopStatus=EFI_SUCCESS;StopReenter=TRUE;assert(ConSplitterConOutDriverBindingStop(NULL,(EFI_HANDLE)1,1,NULL)==EFI_SUCCESS);
    assert(!mConOut.PrimaryGraphicsOutput&&!mConOut.PrimaryGopController&&!Mode.FrameBufferBase&&Info.PixelFormat==PixelBltOnly);
    assert(mConOut.CurrentNumberOfGraphicsOutput==2); /* Both secondary entries remain honest independent outputs. */
    assert(mConOut.GraphicsOutput.Blt(&mConOut.GraphicsOutput,&Pixel,EfiBltVideoFill,0,0,0,0,1,1,0)==EFI_NOT_READY);
  }else assert(Displays[1].Pixels[0]==0x00030201&&Displays[0].Sets==1&&Displays[1].Sets==1);
  FreePool(P.GraphicsOutputModeBuffer);for(UINTN I=0;I<2;++I)free(Displays[I].Pixels);
}
int main(int argc,char **argv){
  Services.HandleProtocol=HandleProtocol;Services.UninstallProtocolInterface=Uninstall;
  BOOLEAN Linear=argc==2&&!strcmp(argv[1],"linear");
  Scenario(Linear,FALSE);if(Linear)Scenario(TRUE,TRUE);
  puts("actual ConSplitter default aggregate / explicit primary modes, linear writes, independent GOP, exact path, counts, removal and stop rollback PASS");return 0;
}
