/* Firmware charging screen. SPDX-License-Identifier: MIT */
#include "ChargerUi.h"
#include <Protocol/EFICrButtonState.h>
#include <Protocol/EFIBatteryChargerCrProtocol.h>
#include <Protocol/EFIDisplayPwr.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
typedef struct {
 EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
 EFI_DISPLAY_POWER_PROTOCOL *Power;
 EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Canvas;
 BOOLEAN Clear;
} CHARGER_DISPLAY;
STATIC EFI_STATUS Exact(EFI_STATUS S) { return S==EFI_SUCCESS?S:(EFI_ERROR(S)?S:EFI_NOT_READY); }
STATIC EFI_STATUS ScreenPower(VOID *Context,BOOLEAN On) {
 CHARGER_DISPLAY *D=Context;
 EFI_STATUS S=Exact(D->Power->SetDisplayPowerState(D->Power,
                    On?EfiDisplayPowerStateOn:EfiDisplayPowerStateOff));
 if(S==EFI_SUCCESS && On)D->Clear=TRUE;
 return S;
}
STATIC EFI_STATUS Draw(VOID *Context,CONST CR_CHARGER_UI_INPUT *I,UINT32 Frame) {
 CHARGER_DISPLAY *D=Context;EFI_STATUS S;UINT32 W,H;
 EFI_GRAPHICS_OUTPUT_BLT_PIXEL Black={0};
 if(!D->Gop->Mode || !D->Gop->Mode->Info)return EFI_NOT_READY;
 W=D->Gop->Mode->Info->HorizontalResolution;H=D->Gop->Mode->Info->VerticalResolution;
 if(W<CR_CHARGER_CANVAS_WIDTH || H<CR_CHARGER_CANVAS_HEIGHT)return EFI_UNSUPPORTED;
 if(D->Clear) {
  S=Exact(D->Gop->Blt(D->Gop,&Black,EfiBltVideoFill,0,0,0,0,W,H,0));
  if(S!=EFI_SUCCESS)return S;D->Clear=FALSE;
 }
 S=CrChargerUiRender(D->Canvas,CR_CHARGER_CANVAS_WIDTH*CR_CHARGER_CANVAS_HEIGHT,I,Frame);
 if(S!=EFI_SUCCESS)return S;
 return Exact(D->Gop->Blt(D->Gop,D->Canvas,EfiBltBufferToVideo,0,0,
   (W-CR_CHARGER_CANVAS_WIDTH)/2,(H-CR_CHARGER_CANVAS_HEIGHT)/2,
   CR_CHARGER_CANVAS_WIDTH,CR_CHARGER_CANVAS_HEIGHT,
   CR_CHARGER_CANVAS_WIDTH*sizeof(*D->Canvas)));
}
STATIC EFI_STATUS Reboot(VOID *Context) {
 (VOID)Context;DEBUG((DEBUG_WARN,"ChargerCr: continuous power hold >3 seconds; reboot\n"));
 gRT->ResetSystem(EfiResetCold,EFI_SUCCESS,0,NULL);
 return EFI_DEVICE_ERROR; /* ResetSystem must not return. */
}
STATIC EFI_STATUS FindDisplay(CHARGER_DISPLAY *D) {
 EFI_STATUS S;EFI_HANDLE *Handles=NULL;UINTN Count=0,I;
 S=gBS->LocateHandleBuffer(ByProtocol,&gEfiDisplayPowerStateProtocolGuid,NULL,&Count,&Handles);
 if(S!=EFI_SUCCESS)return Exact(S);
 S=EFI_NOT_FOUND;
 /* The power protocol is the physical display owner's qualification. The
    virtual console's GOP does not advertise it. DP stays an independent GOP;
    never infer another output's power state from this primary owner. */
 for(I=0;I<Count;++I) {
  EFI_DISPLAY_POWER_PROTOCOL *Power=NULL;EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop=NULL;
  EFI_STATUS P=gBS->HandleProtocol(Handles[I],&gEfiDisplayPowerStateProtocolGuid,(VOID**)&Power);
  if(P!=EFI_SUCCESS || !Power || Power->Revision<EFI_DISPLAY_POWER_PROTOCOL_REVISION ||
     !Power->SetDisplayPowerState || !Power->GetDisplayPowerState)continue;
  P=gBS->HandleProtocol(Handles[I],&gEfiGraphicsOutputProtocolGuid,(VOID**)&Gop);
  if(P!=EFI_SUCCESS || !Gop || !Gop->Blt || !Gop->Mode || !Gop->Mode->Info)continue;
  if(Gop->Mode->Info->HorizontalResolution<CR_CHARGER_CANVAS_WIDTH ||
     Gop->Mode->Info->VerticalResolution<CR_CHARGER_CANVAS_HEIGHT)continue;
  if(D->Gop){S=EFI_UNSUPPORTED;D->Gop=NULL;D->Power=NULL;break;}
  D->Gop=Gop;D->Power=Power;S=EFI_SUCCESS;
 }
 if(Handles)FreePool(Handles);return S;
}
STATIC VOID ReadBattery(EFI_BATTERY_CHARGER_CR_PROTOCOL *B,CR_CHARGER_UI_INPUT *I) {
 CR_BATTERY_CHARGER_SNAPSHOT S={0};EFI_STATUS Status;
 if(!B || !B->GetSnapshot || B->Revision<EFI_BATTERY_CHARGER_CR_PROTOCOL_REVISION)return;
 Status=B->GetSnapshot(B,&S);
 if((Status!=EFI_SUCCESS && Status!=EFI_NOT_READY) || S.Size!=sizeof(S) ||
    S.Revision!=CR_BATTERY_SNAPSHOT_REVISION || !S.PeerReady)return;
 if((S.ValidMask&CR_BATTERY_VALID_PRESENT) && !S.Present)return;
 if((S.ValidMask&CR_BATTERY_VALID_SOC) && S.SocHundredthsPercent<=10000) {
  I->SocValid=TRUE;I->SocHundredths=S.SocHundredthsPercent;
 }
 I->Charging=(S.ValidMask&CR_BATTERY_VALID_STATUS) && S.Status==CrBatteryCharging;
}
EFI_STATUS EFIAPI ChargerCrEntryPoint(EFI_HANDLE Image,EFI_SYSTEM_TABLE *SystemTable) {
 CHARGER_DISPLAY D={0};CR_CHARGER_UI Ui;CR_CHARGER_UI_IO Io={&D,ScreenPower,Draw,Reboot};
 EFI_CR_BUTTON_STATE_PROTOCOL *Buttons=NULL;EFI_BATTERY_CHARGER_CR_PROTOCOL *Battery=NULL;
 EFI_EVENT Tick=NULL;EFI_STATUS S,Cleanup;UINTN Index;UINT64 NextDiscovery=0;
 (VOID)Image;(VOID)SystemTable;
 S=gBS->LocateProtocol(&gEfiCrButtonStateProtocolGuid,NULL,(VOID**)&Buttons);
 if(S!=EFI_SUCCESS)return Exact(S);
 if(!Buttons || Buttons->Revision<EFI_CR_BUTTON_STATE_PROTOCOL_REVISION || !Buttons->GetSnapshot)
  return EFI_UNSUPPORTED;
 S=FindDisplay(&D);if(S!=EFI_SUCCESS)return S;
 /* Charging deliberately remains in boot services for an unlimited time. */
 S=gBS->SetWatchdogTimer(0,0,0,NULL);if(S!=EFI_SUCCESS)return Exact(S);
 D.Canvas=AllocatePool(CR_CHARGER_CANVAS_WIDTH*CR_CHARGER_CANVAS_HEIGHT*sizeof(*D.Canvas));
 if(!D.Canvas)return EFI_OUT_OF_RESOURCES;
 S=gBS->CreateEvent(EVT_TIMER,TPL_APPLICATION,NULL,NULL,&Tick);
 if(S!=EFI_SUCCESS){S=Exact(S);goto Done;}
 S=gBS->SetTimer(Tick,TimerPeriodic,1000000); /* 100 ms, no notify callback. */
 if(S!=EFI_SUCCESS){S=Exact(S);goto Done;}
 S=CrChargerUiInit(&Ui,&Io,GetTimeInNanoSecond(GetPerformanceCounter())/1000);
 if(S!=EFI_SUCCESS)goto Done;
 DEBUG((DEBUG_WARN,"ChargerCr: physical GOP %ux%u; idle30s, power-hold>3s\n",
   D.Gop->Mode->Info->HorizontalResolution,D.Gop->Mode->Info->VerticalResolution));
 for(;;) {
  EFI_CR_BUTTON_SNAPSHOT ButtonSample={0};UINT64 Now=GetTimeInNanoSecond(GetPerformanceCounter())/1000;
  CR_CHARGER_UI_INPUT Input={0};EFI_STATUS Step;
  if(!Battery && Now>=NextDiscovery) {
   VOID *Found=NULL;
   if(gBS->LocateProtocol(&gEfiBatteryChargerCrProtocolGuid,NULL,&Found)==EFI_SUCCESS)Battery=Found;
   NextDiscovery=Now+1000000;
  }
  Input.ButtonsValid=Buttons->GetSnapshot(Buttons,&ButtonSample)==EFI_SUCCESS &&
   ButtonSample.Size==sizeof(ButtonSample) && ButtonSample.Revision==EFI_CR_BUTTON_SNAPSHOT_REVISION;
  Input.PowerPressed=(ButtonSample.Pressed&CR_BUTTON_STATE_POWER)!=0;
  Input.OtherPressed=(ButtonSample.Pressed&(CR_BUTTON_STATE_VOLUME_UP|CR_BUTTON_STATE_VOLUME_DOWN))!=0;
  Input.PowerPressSequence=ButtonSample.PowerPressSequence;
  ReadBattery(Battery,&Input);
  Step=CrChargerUiStep(&Ui,&Input,Now);
  if(Ui.ResetIssued){S=Step;break;}
  /* Missing peer data is drawn as --%. Hardware power/draw errors remain
     retriable, and the UI never performs battery RPC from this path. */
  if(Step!=EFI_SUCCESS && Step!=EFI_NOT_READY)
   DEBUG((DEBUG_ERROR,"ChargerCr: UI operation failed: %r\n",Step));
  S=gBS->WaitForEvent(1,&Tick,&Index);if(S!=EFI_SUCCESS){S=Exact(S);break;}
 }
Done:
 if(Tick) {
  Cleanup=gBS->SetTimer(Tick,TimerCancel,0);
  if(Cleanup!=EFI_SUCCESS && S==EFI_SUCCESS)S=Exact(Cleanup);
  Cleanup=gBS->CloseEvent(Tick);
  if(Cleanup!=EFI_SUCCESS && S==EFI_SUCCESS)S=Exact(Cleanup);
 }
 /* No event callback references this stack/storage. Always wake the retained
    owner before returning to BDS/manual shell, even after a failed UI op. */
 Cleanup=ScreenPower(&D,TRUE);if(Cleanup!=EFI_SUCCESS && S==EFI_SUCCESS)S=Cleanup;
 if(D.Canvas)FreePool(D.Canvas);
 return S;
}
