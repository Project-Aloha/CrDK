/* Actual platform early-graphics adapter; boot-service faults are modeled. MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#include <Library/MsPlatformEarlyGraphicsLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
EFI_GUID gCraneNativeGopProtocolGuid;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION Info;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE Mode;
static EFI_GRAPHICS_OUTPUT_PROTOCOL Gop;
static EFI_STATUS LocateStatus;
static UINTN Allocations,Live,FailAllocation,ModeWrites;
static EFI_TPL Tpl=TPL_APPLICATION;
EFI_TPL EFIAPI EfiGetCurrentTpl(VOID){return Tpl;}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}
VOID *EFIAPI AllocatePool(UINTN N){if(++Allocations==FailAllocation)return NULL;VOID *P=malloc(N);assert(P);++Live;return P;}
VOID *EFIAPI AllocateZeroPool(UINTN N){VOID *P=AllocatePool(N);if(P)memset(P,0,N);return P;}
VOID EFIAPI FreePool(VOID *P){assert(P&&Live);--Live;free(P);}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){(void)L;(void)F;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){(void)L;return FALSE;}
static EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *R,VOID **P){(void)G;(void)R;if(LocateStatus==EFI_SUCCESS)*P=&Gop;return LocateStatus;}
static EFI_STATUS EFIAPI Set(EFI_GRAPHICS_OUTPUT_PROTOCOL *G,UINT32 N){(void)G;(void)N;++ModeWrites;return EFI_DEVICE_ERROR;}
static EFI_STATUS EFIAPI Query(EFI_GRAPHICS_OUTPUT_PROTOCOL *G,UINT32 N,UINTN *B,EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **I){
 (void)G;(void)N;*B=sizeof(Info);*I=AllocatePool(*B);if(!*I)return EFI_OUT_OF_RESOURCES;**I=Info;return EFI_SUCCESS;
}
static EFI_BOOT_SERVICES Boot={.LocateProtocol=Locate};
EFI_BOOT_SERVICES *gBS=&Boot;
static void Drop(EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *P){if(P){if(P->Info)FreePool(P->Info);FreePool(P);}}
int main(int argc,char **argv){
 assert(argc==2);int Case=atoi(argv[1]);
 Info=(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION){.HorizontalResolution=1920,.VerticalResolution=1080,.PixelsPerScanLine=1920,.PixelFormat=PixelBlueGreenRedReserved8BitPerColor};
 Mode=(EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE){.MaxMode=1,.Info=&Info,.SizeOfInfo=sizeof(Info),.FrameBufferBase=0xa0000000,.FrameBufferSize=8294400};
 Gop=(EFI_GRAPHICS_OUTPUT_PROTOCOL){.Mode=&Mode,.SetMode=Set,.QueryMode=Query};
 EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Out=NULL,*Other=NULL;EFI_STATUS S;
 if(Case==0){LocateStatus=EFI_NOT_FOUND;S=MsEarlyGraphicsGetFrameBufferInfo(&Out);assert(S==EFI_NOT_FOUND&&!Out&&!Live);}
 if(Case==1){
  assert(MsEarlyGraphicsGetFrameBufferInfo(&Out)==EFI_SUCCESS&&Out&&Out!=&Mode&&Out->Info!=&Info);
  assert(MsEarlyGraphicsGetFrameBufferInfo(&Other)==EFI_SUCCESS&&Other&&Other!=Out&&Other->Info!=Out->Info);
  UINTN Before=Allocations;Mode.FrameBufferBase+=0x1000000;
  assert(MsEarlyGraphicsGetFrameBufferInfo(&Out)==EFI_SUCCESS&&Out->FrameBufferBase==Mode.FrameBufferBase&&Allocations==Before);
  assert(Other->FrameBufferBase!=Out->FrameBufferBase);
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE Saved=*Out;LocateStatus=EFI_DEVICE_ERROR;
  assert(MsEarlyGraphicsGetFrameBufferInfo(&Out)==EFI_DEVICE_ERROR&&!memcmp(&Saved,Out,sizeof(Saved)));
 }
 if(Case==2||Case==3){FailAllocation=Case-1;S=MsEarlyGraphicsGetFrameBufferInfo(&Out);assert(S==EFI_OUT_OF_RESOURCES&&!Out&&!Live);FailAllocation=0;
  assert(MsEarlyGraphicsGetFrameBufferInfo(&Out)==EFI_SUCCESS&&Out);}
 if(Case==4){Mode.Mode=MAX_UINT32;assert(MsEarlyGraphicsGetFrameBufferInfo(&Out)==EFI_NOT_READY&&!Out&&!Live&&!ModeWrites);}
 if(Case==5){Mode.FrameBufferSize=4;assert(MsEarlyGraphicsGetFrameBufferInfo(&Out)==EFI_UNSUPPORTED&&!Out&&!Live);}
 if(Case==6){Info.PixelFormat=PixelBltOnly;assert(MsEarlyGraphicsGetFrameBufferInfo(&Out)==EFI_UNSUPPORTED&&!Out&&!Live);}
 if(Case==7){Info.PixelsPerScanLine=1;assert(MsEarlyGraphicsGetFrameBufferInfo(&Out)==EFI_UNSUPPORTED&&!Out&&!Live);}
 if(Case==8){Mode.FrameBufferBase=MAX_UINT64-4096;assert(MsEarlyGraphicsGetFrameBufferInfo(&Out)==EFI_UNSUPPORTED&&!Out&&!Live);}
 if(Case==9){Mode.SizeOfInfo=sizeof(Info)-1;assert(MsEarlyGraphicsGetFrameBufferInfo(&Out)==EFI_NOT_READY&&!Out&&!Live);}
 if(Case==10){Info.HorizontalResolution=1080;Info.VerticalResolution=2340;Info.PixelsPerScanLine=1080;Mode.FrameBufferSize=1080*2340*4;
  assert(MsEarlyGraphicsGetFrameBufferInfo(&Out)==EFI_SUCCESS&&Out->Info->VerticalResolution==2340);}
 if(Case==11){assert(MsEarlyGraphicsGetFrameBufferInfo(NULL)==EFI_INVALID_PARAMETER&&!Live);}
 Drop(Out);Drop(Other);assert(!Live&&!ModeWrites);printf("early graphics case %d PASS\n",Case);return 0;
}
