/* Entire actual charging app + state/raster, real boot-service call shapes.
   SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../Application/ChargerCr/ChargerCr.c"
EFI_GUID gEfiGraphicsOutputProtocolGuid,gEfiDisplayPowerStateProtocolGuid;
EFI_GUID gEfiCrButtonStateProtocolGuid,gEfiBatteryChargerCrProtocolGuid;
EFI_BOOT_SERVICES Services;EFI_BOOT_SERVICES *gBS=&Services;
EFI_RUNTIME_SERVICES Runtime;EFI_RUNTIME_SERVICES *gRT=&Runtime;
STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION Info={.HorizontalResolution=1920,.VerticalResolution=1080};
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE Mode={.Info=&Info};
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL Gop={.Mode=&Mode};
STATIC EFI_DISPLAY_POWER_PROTOCOL Pwr;
STATIC EFI_CR_BUTTON_STATE_PROTOCOL Buttons;
STATIC EFI_BATTERY_CHARGER_CR_PROTOCOL Battery;
STATIC UINT64 Now;STATIC UINT32 Phase,FailAt,Waits,PowerOn,PowerOff,Draws,Resets,Allocs,Closes;
STATIC EFI_STATUS FailStatus;STATIC BOOLEAN Online,PowerState,NoButtons,Duplicate,UnknownBattery,MalformedBattery;
STATIC BOOLEAN TapOnly;
STATIC UINT8 Saved[CR_CHARGER_CANVAS_WIDTH*CR_CHARGER_CANVAS_HEIGHT*4];
STATIC EFI_STATUS Effect(VOID){return ++Phase==FailAt?FailStatus:EFI_SUCCESS;}
VOID *EFIAPI AllocatePool(UINTN N){Allocs++;return malloc(N);}
VOID EFIAPI FreePool(VOID *P){assert(P);Allocs--;free(P);}
VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN L){(VOID)L;return FALSE;}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){(VOID)L;(VOID)F;}
UINT64 EFIAPI GetPerformanceCounter(VOID){return Now;}
UINT64 EFIAPI GetTimeInNanoSecond(UINT64 T){return T*1000;}
STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *R,VOID **Out){(VOID)R;
 if(G==&gEfiCrButtonStateProtocolGuid){if(NoButtons)return EFI_NOT_FOUND;*Out=&Buttons;return EFI_SUCCESS;}
 assert(G==&gEfiBatteryChargerCrProtocolGuid);if(!Online)return EFI_NOT_FOUND;*Out=&Battery;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Handles(EFI_LOCATE_SEARCH_TYPE T,EFI_GUID *G,VOID *R,UINTN *N,EFI_HANDLE **Out){
 assert(T==ByProtocol && G==&gEfiDisplayPowerStateProtocolGuid && !R);*N=Duplicate?2:1;
 *Out=AllocatePool(*N*sizeof(**Out));(*Out)[0]=(VOID*)1;if(Duplicate)(*Out)[1]=(VOID*)2;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Handle(EFI_HANDLE H,EFI_GUID *G,VOID **Out){assert(H==(VOID*)1 || H==(VOID*)2);
 if(G==&gEfiDisplayPowerStateProtocolGuid)*Out=&Pwr;else{assert(G==&gEfiGraphicsOutputProtocolGuid);*Out=&Gop;}return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Watchdog(UINTN Timeout,UINT64 Code,UINTN Size,CHAR16 *Data){assert(!Timeout && !Code && !Size && !Data);return Effect();}
STATIC EFI_STATUS EFIAPI Create(UINT32 T,EFI_TPL L,EFI_EVENT_NOTIFY N,VOID *C,EFI_EVENT *Out){
 assert(T==EVT_TIMER && L==TPL_APPLICATION && !N && !C);EFI_STATUS S=Effect();if(S==EFI_SUCCESS)*Out=(VOID*)3;return S;}
STATIC EFI_STATUS EFIAPI Timer(EFI_EVENT E,EFI_TIMER_DELAY T,UINT64 D){assert(E==(VOID*)3);
 assert((T==TimerPeriodic && D==1000000) || (T==TimerCancel && D==0));return Effect();}
STATIC EFI_STATUS EFIAPI Wait(UINTN N,EFI_EVENT *E,UINTN *I){assert(N==1 && *E==(VOID*)3);assert(++Waits<1000);
 EFI_STATUS S=Effect();if(S!=EFI_SUCCESS)return S;if(TapOnly && Now>=32000000)return EFI_DEVICE_ERROR;Now+=100000;if(Now>=1000000)Online=TRUE;*I=0;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Close(EFI_EVENT E){assert(E==(VOID*)3);Closes++;return Effect();}
STATIC EFI_STATUS EFIAPI Raw(EFI_CR_BUTTON_STATE_PROTOCOL *B,UINT32 *Out){assert(B==&Buttons);*Out=Now>=31000000?CR_BUTTON_STATE_POWER:0;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI RawSnapshotTest(EFI_CR_BUTTON_STATE_PROTOCOL *B,EFI_CR_BUTTON_SNAPSHOT *Out){
 assert(B==&Buttons);memset(Out,0,sizeof(*Out));Out->Size=sizeof(*Out);Out->Revision=EFI_CR_BUTTON_SNAPSHOT_REVISION;
 Out->Pressed=!TapOnly && Now>=31000000?CR_BUTTON_STATE_POWER:0;
 Out->PowerPressSequence=TapOnly?(Now>=30500000?1:0):(Now>=31000000?1:0);return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Snapshot(EFI_BATTERY_CHARGER_CR_PROTOCOL *B,CR_BATTERY_CHARGER_SNAPSHOT *Out){assert(B==&Battery);
 memset(Out,0,sizeof(*Out));Out->Size=MalformedBattery?1:sizeof(*Out);Out->Revision=CR_BATTERY_SNAPSHOT_REVISION;
 Out->PeerReady=TRUE;Out->Present=TRUE;Out->ValidMask=CR_BATTERY_VALID_PRESENT|CR_BATTERY_VALID_STATUS;
 Out->Status=CrBatteryCharging;Out->SocHundredthsPercent=3700;
 if(!UnknownBattery)Out->ValidMask|=CR_BATTERY_VALID_SOC;return UnknownBattery?EFI_NOT_READY:EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Power(EFI_DISPLAY_POWER_PROTOCOL *P,EFI_DISPLAY_POWER_STATE S){assert(P==&Pwr);
 assert(S==EfiDisplayPowerStateOn || S==EfiDisplayPowerStateOff);PowerState=S==EfiDisplayPowerStateOn;
 if(PowerState)PowerOn++;else PowerOff++;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI GetPower(EFI_DISPLAY_POWER_PROTOCOL *P,EFI_DISPLAY_POWER_STATE *S){assert(P==&Pwr);*S=PowerState?EfiDisplayPowerStateOn:EfiDisplayPowerStateOff;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *G,EFI_GRAPHICS_OUTPUT_BLT_PIXEL *P,
 EFI_GRAPHICS_OUTPUT_BLT_OPERATION Op,UINTN Sx,UINTN Sy,UINTN Dx,UINTN Dy,UINTN W,UINTN H,UINTN Delta){
 assert(G==&Gop && PowerState && !Sx && !Sy);
 if(Op==EfiBltVideoFill){assert(!Dx && !Dy && W==1920 && H==1080 && !Delta && !P->Blue && !P->Green && !P->Red);}
 else{assert(Op==EfiBltBufferToVideo && Dx==864 && Dy==396 && W==192 && H==288 && Delta==192*4);memcpy(Saved,P,sizeof(Saved));Draws++;}
 return EFI_SUCCESS;}
STATIC VOID EFIAPI Reset(EFI_RESET_TYPE T,EFI_STATUS S,UINTN Size,VOID *Data){assert(T==EfiResetCold && S==EFI_SUCCESS && !Size && !Data);Resets++;}
STATIC VOID Setup(VOID){Now=0;Phase=FailAt=Waits=PowerOn=PowerOff=Draws=Resets=Allocs=Closes=0;FailStatus=EFI_DEVICE_ERROR;
 Online=PowerState=NoButtons=Duplicate=UnknownBattery=MalformedBattery=TapOnly=FALSE;
 Services=(EFI_BOOT_SERVICES){.LocateProtocol=Locate,.LocateHandleBuffer=Handles,.HandleProtocol=Handle,
 .SetWatchdogTimer=Watchdog,.CreateEvent=Create,.SetTimer=Timer,.WaitForEvent=Wait,.CloseEvent=Close};Runtime.ResetSystem=Reset;
 Gop.Blt=Blt;Buttons=(EFI_CR_BUTTON_STATE_PROTOCOL){EFI_CR_BUTTON_STATE_PROTOCOL_REVISION,Raw,RawSnapshotTest};
 Battery=(EFI_BATTERY_CHARGER_CR_PROTOCOL){EFI_BATTERY_CHARGER_CR_PROTOCOL_REVISION,Snapshot};
 Pwr=(EFI_DISPLAY_POWER_PROTOCOL){EFI_DISPLAY_POWER_PROTOCOL_REVISION,Power,GetPower};}
int main(void){
 Setup();assert(ChargerCrEntryPoint(NULL,NULL)==EFI_DEVICE_ERROR);
 assert(Resets==1 && PowerOff==1 && PowerOn==3 && Draws>0 && Closes==1 && Allocs==0 && Now==34100000);
 CR_CHARGER_UI_INPUT Expected={.SocValid=TRUE,.SocHundredths=3700,.Charging=TRUE};
 EFI_GRAPHICS_OUTPUT_BLT_PIXEL Raster[CR_CHARGER_CANVAS_WIDTH*CR_CHARGER_CANVAS_HEIGHT];
 assert(CrChargerUiRender(Raster,sizeof(Raster)/sizeof(*Raster),&Expected,(UINT32)((34000000/CR_CHARGER_ANIMATION_US)%5))==EFI_SUCCESS);
 assert(!memcmp(Saved,Raster,sizeof(Saved)));
 Setup();TapOnly=TRUE;assert(ChargerCrEntryPoint(NULL,NULL)==EFI_DEVICE_ERROR);
 assert(!Resets && PowerOff==1 && PowerOn==3 && Allocs==0 && Now==32000000);
 Setup();NoButtons=TRUE;assert(ChargerCrEntryPoint(NULL,NULL)==EFI_NOT_FOUND && !Phase && !Allocs);
 Setup();Duplicate=TRUE;assert(ChargerCrEntryPoint(NULL,NULL)==EFI_UNSUPPORTED && !Phase && !Allocs);
 Setup();UnknownBattery=TRUE;assert(ChargerCrEntryPoint(NULL,NULL)==EFI_DEVICE_ERROR && Resets==1);
 Expected.SocValid=FALSE;assert(CrChargerUiRender(Raster,sizeof(Raster)/sizeof(*Raster),&Expected,3)==EFI_SUCCESS);
 assert(!memcmp(Saved,Raster,sizeof(Saved)));
 Setup();MalformedBattery=TRUE;assert(ChargerCrEntryPoint(NULL,NULL)==EFI_DEVICE_ERROR && Resets==1);
 Expected.Charging=FALSE;assert(CrChargerUiRender(Raster,sizeof(Raster)/sizeof(*Raster),&Expected,0)==EFI_SUCCESS);
 assert(!memcmp(Saved,Raster,sizeof(Saved)));
 for(UINT32 Fail=1;Fail<=12;++Fail)for(UINT32 Warn=0;Warn<2;++Warn){
  Setup();FailAt=Fail;FailStatus=Warn?EFI_WARN_UNKNOWN_GLYPH:EFI_DEVICE_ERROR;
  EFI_STATUS S=ChargerCrEntryPoint(NULL,NULL);assert(S==EFI_DEVICE_ERROR || S==EFI_NOT_READY);
  assert(Allocs==0 && (PowerState || Fail==1)); /* no callback uses freed app storage */
 }
 puts("PASS: entire actual charging app waits at APPLICATION, late battery/SOC validity, physical power GOP, idle/wake/hold cold-reset, exact timer failures and allocation cleanup");return 0;
}
