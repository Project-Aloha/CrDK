/* Real GraphicsConsoleHelper entry, with negotiated text/GOP modes. MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#include <Library/GraphicsConsoleHelperLib.h>
#include <Library/UefiBootServicesTableLib.h>
EFI_GUID gEfiGraphicsOutputProtocolGuid={.Data1=1},gEfiSimpleTextOutProtocolGuid={.Data1=2};
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION Info;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE Mode;
static EFI_SIMPLE_TEXT_OUTPUT_MODE TextMode;
static EFI_GRAPHICS_OUTPUT_PROTOCOL Gop;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Text;
static UINTN Cols[4]={80,100,240,0},Rows[4]={25,31,56,0};
static EFI_STATUS SetResult,LookupResult,PcdResult;
static UINT32 Writes[4],PcdCalls,TextSets,HardwareSets;
static EFI_STATUS EFIAPI Handle(EFI_HANDLE H,EFI_GUID *G,VOID **P){
 assert(H==(VOID*)1);if(LookupResult)return LookupResult;
 *P=G==&gEfiGraphicsOutputProtocolGuid?(VOID*)&Gop:(VOID*)&Text;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *K,VOID **P){(void)G;(void)K;(void)P;return EFI_NOT_FOUND;}
static EFI_STATUS EFIAPI Query(EFI_GRAPHICS_OUTPUT_PROTOCOL *G,UINT32 N,UINTN *B,EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **I){
 assert(G==&Gop);if(N)return EFI_UNSUPPORTED;*B=sizeof(Info);*I=malloc(*B);assert(*I);**I=Info;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Set(EFI_GRAPHICS_OUTPUT_PROTOCOL *G,UINT32 N){(void)G;(void)N;++HardwareSets;return EFI_UNSUPPORTED;}
static EFI_STATUS EFIAPI QueryText(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *T,UINTN N,UINTN *C,UINTN *R){
 assert(T==&Text);if(N>=(UINTN)TextMode.MaxMode)return EFI_UNSUPPORTED;*C=Cols[N];*R=Rows[N];return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI SetText(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *T,UINTN N){
 assert(T==&Text&&N<(UINTN)TextMode.MaxMode);++TextSets;if(!SetResult)TextMode.Mode=(INT32)N;return SetResult;
}
RETURN_STATUS EFIAPI LibPcdSet32S(UINTN Token,UINT32 V){assert(Token>=1&&Token<=4);++PcdCalls;if(PcdResult)return PcdResult;Writes[Token-1]=V;return RETURN_SUCCESS;}
UINT32 EFIAPI LibPcdSet32(UINTN Token,UINT32 V){(void)LibPcdSet32S(Token,V);return V;}
VOID EFIAPI FreePool(VOID *P){free(P);}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){(void)L;(void)F;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){(void)L;return FALSE;}
static EFI_BOOT_SERVICES Boot={.HandleProtocol=Handle,.LocateProtocol=Locate};
static EFI_SYSTEM_TABLE Table={.ConsoleOutHandle=(VOID*)1};
EFI_BOOT_SERVICES *gBS=&Boot;EFI_SYSTEM_TABLE *gST=&Table;
int main(int argc,char **argv){
 assert(argc==2);int N=atoi(argv[1]);
 Info=(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION){.HorizontalResolution=1920,.VerticalResolution=1080,.PixelsPerScanLine=1920,.PixelFormat=PixelBlueGreenRedReserved8BitPerColor};
 Mode=(EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE){.MaxMode=1,.Info=&Info,.SizeOfInfo=sizeof(Info)};
 Gop=(EFI_GRAPHICS_OUTPUT_PROTOCOL){.QueryMode=Query,.SetMode=Set,.Mode=&Mode};
 TextMode=(EFI_SIMPLE_TEXT_OUTPUT_MODE){.MaxMode=1};Text=(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL){.QueryMode=QueryText,.SetMode=SetText,.Mode=&TextMode};Table.ConOut=&Text;
 if(N==0)assert(SetGraphicsConsoleMode(GCM_LOW_RES)==EFI_SUCCESS);
 if(N==1)assert(SetGraphicsConsoleMode(GCM_NATIVE_RES)==EFI_SUCCESS);
 if(N==2){Info.HorizontalResolution=1080;Info.VerticalResolution=2340;Info.PixelsPerScanLine=1080;assert(SetGraphicsConsoleMode(GCM_NATIVE_RES)==EFI_SUCCESS);}
 if(N==3){TextMode.MaxMode=3;SetResult=EFI_DEVICE_ERROR;assert(SetGraphicsConsoleMode(GCM_NATIVE_RES)==EFI_DEVICE_ERROR&&!PcdCalls);}
 if(N==4){PcdResult=RETURN_DEVICE_ERROR;assert(SetGraphicsConsoleMode(GCM_NATIVE_RES)==EFI_DEVICE_ERROR&&PcdCalls==1);}
 if(N==5)assert(SetGraphicsConsoleMode(255)==EFI_INVALID_PARAMETER&&!PcdCalls&&!TextSets);
 if(N==6){LookupResult=EFI_NOT_FOUND;assert(SetGraphicsConsoleMode(GCM_NATIVE_RES)==EFI_NOT_FOUND&&!PcdCalls);}
 if(N==7){TextMode.MaxMode=4;assert(SetGraphicsConsoleMode(GCM_NATIVE_RES)==EFI_SUCCESS&&TextMode.Mode==2);}
 if(N==8){TextMode.MaxMode=3;assert(SetGraphicsConsoleMode(GCM_LOW_RES)==EFI_SUCCESS&&TextMode.Mode==0);}
 if(N==9){Mode.MaxMode=0;assert(SetGraphicsConsoleMode(GCM_NATIVE_RES)==EFI_NOT_READY&&!TextSets&&!PcdCalls);}
 if(N==10){Info.PixelFormat=PixelBltOnly;assert(SetGraphicsConsoleMode(GCM_NATIVE_RES)==EFI_SUCCESS);}
 if(N==12){SetResult=EFI_WARN_UNKNOWN_GLYPH;assert(SetGraphicsConsoleMode(GCM_NATIVE_RES)==EFI_NOT_READY&&!PcdCalls);}
 if(N==13){Mode.SizeOfInfo=0;assert(SetGraphicsConsoleMode(GCM_NATIVE_RES)==EFI_NOT_READY&&!TextSets);}
 if(N==11){Cols[0]=Rows[0]=0;assert(SetGraphicsConsoleMode(GCM_NATIVE_RES)==EFI_UNSUPPORTED&&!TextSets&&!PcdCalls);}
 if(N==0||N==1||N==2||N==7||N==8||N==10){assert(Writes[0]==Info.HorizontalResolution&&Writes[1]==Info.VerticalResolution);assert(Writes[2]==Cols[TextMode.Mode]&&Writes[3]==Rows[TextMode.Mode]);}
 assert(!HardwareSets);printf("native console case %d PASS\n",N);return 0;
}
