/* Actual charging UI/timing, bounded writes and retained power retries. MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../Application/ChargerCr/ChargerUi.h"
VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}
STATIC UINT32 PowerCalls,DrawCalls,Reboots,LastFrame;
STATIC BOOLEAN ActualOn;
STATIC EFI_STATUS PowerStatus,DrawStatus,ResetStatus;
STATIC EFI_STATUS Power(VOID *C,BOOLEAN On){assert(C==(VOID*)1);PowerCalls++;if(PowerStatus==EFI_SUCCESS)ActualOn=On;return PowerStatus;}
STATIC EFI_STATUS Draw(VOID *C,CONST CR_CHARGER_UI_INPUT *I,UINT32 Frame){assert(C==(VOID*)1 && ActualOn);DrawCalls++;LastFrame=Frame;return DrawStatus;}
STATIC EFI_STATUS Reboot(VOID *C){assert(C==(VOID*)1);Reboots++;return ResetStatus;}
STATIC VOID Init(CR_CHARGER_UI *U,UINT64 Now){CR_CHARGER_UI_IO Io={(VOID*)1,Power,Draw,Reboot};PowerCalls=DrawCalls=Reboots=0;ActualOn=FALSE;PowerStatus=DrawStatus=ResetStatus=EFI_SUCCESS;assert(CrChargerUiInit(U,&Io,Now)==EFI_SUCCESS);}
int main(void){
 CR_CHARGER_UI U;CR_CHARGER_UI_INPUT I={.ButtonsValid=TRUE,.SocValid=TRUE,.SocHundredths=5000,.Charging=TRUE};
 Init(&U,0);assert(CrChargerUiStep(&U,&I,0)==EFI_SUCCESS && ActualOn && DrawCalls==1);
 for(UINT64 T=100000;T<CR_CHARGER_IDLE_US;T+=100000)assert(CrChargerUiStep(&U,&I,T)==EFI_SUCCESS);
 assert(DrawCalls==60 && PowerCalls==1); /* battery animation is not activity */
 assert(CrChargerUiStep(&U,&I,CR_CHARGER_IDLE_US)==EFI_SUCCESS && !ActualOn && !U.ScreenOn);
 UINT32 Drawn=DrawCalls;I.SocHundredths=9900;
 assert(CrChargerUiStep(&U,&I,CR_CHARGER_IDLE_US+100000)==EFI_SUCCESS && DrawCalls==Drawn);
 I.PowerPressed=TRUE;assert(CrChargerUiStep(&U,&I,CR_CHARGER_IDLE_US+200000)==EFI_SUCCESS && ActualOn && DrawCalls==Drawn+1);
 I.PowerPressed=FALSE;assert(CrChargerUiStep(&U,&I,CR_CHARGER_IDLE_US+300000)==EFI_SUCCESS && Reboots==0);
 Init(&U,0);I.PowerPressed=TRUE;
 for(UINT64 T=0;T<=CR_CHARGER_HOLD_US;T+=100000)assert(CrChargerUiStep(&U,&I,T)==EFI_SUCCESS);
 assert(Reboots==0);assert(CrChargerUiStep(&U,&I,CR_CHARGER_HOLD_US+100000)==EFI_SUCCESS && Reboots==1);
 assert(CrChargerUiStep(&U,&I,CR_CHARGER_HOLD_US+200000)==EFI_ABORTED && Reboots==1);
 /* Failed/unknown read, even during a held key, destroys continuity. */
 Init(&U,0);I.ButtonsValid=TRUE;
 for(UINT64 T=0;T<2000000;T+=100000)assert(CrChargerUiStep(&U,&I,T)==EFI_SUCCESS);
 I.ButtonsValid=FALSE;assert(CrChargerUiStep(&U,&I,2000000)==EFI_SUCCESS);
 I.ButtonsValid=TRUE;for(UINT64 T=2100000;T<=5000000;T+=100000)assert(CrChargerUiStep(&U,&I,T)==EFI_SUCCESS);
 assert(Reboots==0);assert(CrChargerUiStep(&U,&I,5300000)==EFI_SUCCESS && Reboots==0); /* missed polling gap */
 for(UINT64 T=5400000;T<=8300000;T+=100000)assert(CrChargerUiStep(&U,&I,T)==EFI_SUCCESS);
 assert(Reboots==0);assert(CrChargerUiStep(&U,&I,8400000)==EFI_SUCCESS && Reboots==1);
 /* Nonzero warnings/pending cannot certify power/draw. Retried exact On/Off. */
 Init(&U,0);I.PowerPressed=FALSE;PowerStatus=EFI_WARN_UNKNOWN_GLYPH;
 assert(CrChargerUiStep(&U,&I,0)==EFI_NOT_READY && !U.ScreenKnown && DrawCalls==0);
 PowerStatus=EFI_SUCCESS;DrawStatus=EFI_DEVICE_ERROR;
 assert(CrChargerUiStep(&U,&I,100000)==EFI_DEVICE_ERROR && !U.Drawn);
 DrawStatus=EFI_SUCCESS;assert(CrChargerUiStep(&U,&I,200000)==EFI_SUCCESS && U.Drawn);
 PowerStatus=EFI_TIMEOUT;assert(CrChargerUiStep(&U,&I,CR_CHARGER_IDLE_US)==EFI_TIMEOUT && !U.ScreenKnown && ActualOn);
 PowerStatus=EFI_SUCCESS;assert(CrChargerUiStep(&U,&I,CR_CHARGER_IDLE_US+100000)==EFI_SUCCESS && !ActualOn);
 assert(CrChargerUiStep(&U,&I,1)==EFI_INVALID_PARAMETER);
 /* IRQ-observed press/release between polls wakes without creating a held
    level. Reusing the sequence or advancing it never certifies long hold. */
 Init(&U,0);I=(CR_CHARGER_UI_INPUT){.ButtonsValid=TRUE};
 assert(CrChargerUiStep(&U,&I,0)==EFI_SUCCESS);
 assert(CrChargerUiStep(&U,&I,CR_CHARGER_IDLE_US)==EFI_SUCCESS && !ActualOn);
 I.PowerPressSequence=1;
 assert(CrChargerUiStep(&U,&I,CR_CHARGER_IDLE_US+100000)==EFI_SUCCESS && ActualOn && !U.PowerHeld);
 for(UINT64 T=CR_CHARGER_IDLE_US+200000;T<=CR_CHARGER_IDLE_US+5000000;T+=100000){
  I.PowerPressSequence++;assert(CrChargerUiStep(&U,&I,T)==EFI_SUCCESS && !Reboots && !U.PowerHeld);
 }
 UINT64 IdleAt=U.LastActivity+CR_CHARGER_IDLE_US;
 assert(CrChargerUiStep(&U,&I,IdleAt)==EFI_SUCCESS && !ActualOn && !Reboots);
 /* Invalid current sample cannot turn a retained sequence into activity. */
 I.ButtonsValid=FALSE;I.PowerPressSequence++;
 assert(CrChargerUiStep(&U,&I,IdleAt+100000)==EFI_SUCCESS && !ActualOn && !Reboots);
 /* Sequence identifies a release/repress hidden between high-level samples.
    A near-three-second old hold cannot be added to the newly pressed hold. */
 Init(&U,0);I=(CR_CHARGER_UI_INPUT){.ButtonsValid=TRUE,.PowerPressed=TRUE};
 for(UINT64 T=0;T<3000000;T+=100000)assert(CrChargerUiStep(&U,&I,T)==EFI_SUCCESS && !Reboots);
 I.PowerPressSequence=1;
 for(UINT64 T=3000000;T<=6000000;T+=100000)assert(CrChargerUiStep(&U,&I,T)==EFI_SUCCESS && !Reboots);
 assert(U.PowerSince==3000000);
 assert(CrChargerUiStep(&U,&I,6100000)==EFI_SUCCESS && Reboots==1);
 /* Pixel guard and SOC/animation changes. Unknown values are visibly distinct. */
 UINTN N=CR_CHARGER_CANVAS_WIDTH*CR_CHARGER_CANVAS_HEIGHT;
 EFI_GRAPHICS_OUTPUT_BLT_PIXEL *P=malloc((N+2)*sizeof(*P));assert(P);memset(P,0x5a,(N+2)*sizeof(*P));
 assert(CrChargerUiRender(P+1,N,&I,4)==EFI_SUCCESS);
 for(UINTN J=0;J<sizeof(*P);++J){assert(((UINT8*)P)[J]==0x5a);assert(((UINT8*)(P+N+1))[J]==0x5a);}
 assert((P+1)[28*CR_CHARGER_CANVAS_WIDTH+40].Red==208);
 I.SocValid=FALSE;I.Charging=FALSE;assert(CrChargerUiRender(P+1,N,&I,0)==EFI_SUCCESS);
 assert((P+1)[252*CR_CHARGER_CANVAS_WIDTH+60].Red==232); /* --% dash */
 assert(CrChargerUiRender(P+1,N-1,&I,0)==EFI_INVALID_PARAMETER);
 I.SocHundredths=10001;assert(CrChargerUiRender(P+1,N,&I,0)==EFI_INVALID_PARAMETER);
 free(P);puts("PASS: actual charging timing strict>3s, short wake, 30s idle, unknown/gap hold reset, exact-success retries, animation/no idle extension, bounded BGRX raster");return 0;
}
