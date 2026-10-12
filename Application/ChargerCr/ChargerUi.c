/* Charging UI state and bounded native drawing. SPDX-License-Identifier: MIT */
#include "ChargerUi.h"
#include <Library/BaseMemoryLib.h>
STATIC EFI_STATUS Exact(EFI_STATUS S) { return S==EFI_SUCCESS?S:(EFI_ERROR(S)?S:EFI_NOT_READY); }
EFI_STATUS CrChargerUiInit(CR_CHARGER_UI *U,CONST CR_CHARGER_UI_IO *Io,UINT64 Now) {
  if(!U||!Io||!Io->Power||!Io->Draw||!Io->Reboot)return EFI_INVALID_PARAMETER;
  ZeroMem(U,sizeof(*U));U->Io=*Io;U->LastTime=U->LastActivity=Now;U->Initialized=TRUE;
  return EFI_SUCCESS;
}
EFI_STATUS CrChargerUiStep(CR_CHARGER_UI *U,CONST CR_CHARGER_UI_INPUT *I,UINT64 Now) {
  EFI_STATUS S;BOOLEAN WantOn,Changed;
  if(!U||!U->Initialized||!I||I->SocHundredths>10000)return EFI_INVALID_PARAMETER;
  if(Now<U->LastTime)return EFI_INVALID_PARAMETER;
  if(U->ResetIssued)return EFI_ABORTED;
  U->LastTime=Now;
  /* An unknown or missing sample breaks a continuous-hold witness. A delayed
     event loop cannot count three seconds that it did not observe. */
  if(!I->ButtonsValid || (U->PowerHeld && Now-U->LastButtonTime>CR_CHARGER_BUTTON_GAP_US)) {
    U->PowerHeld=FALSE;U->OtherHeld=FALSE;
  }
  if(I->ButtonsValid) {
    /* Sequence only preserves a short observed tap for wake/activity. It
       cannot contribute any time to a continuous held-level witness. */
    if(U->PressSequenceKnown && I->PowerPressSequence!=U->LastPowerPressSequence) {
      /* A new press while currently high proves release/repress between
         samples. Separate holds must never share the old start time. */
      U->PowerHeld=FALSE;
      if(I->PowerPressSequence>U->LastPowerPressSequence)U->LastActivity=Now;
    }
    U->LastPowerPressSequence=I->PowerPressSequence;U->PressSequenceKnown=TRUE;
    if(I->PowerPressed && !U->PowerHeld) {U->PowerSince=Now;U->LastActivity=Now;}
    if(I->OtherPressed && !U->OtherHeld && U->ScreenOn)U->LastActivity=Now;
    U->PowerHeld=I->PowerPressed;U->OtherHeld=I->OtherPressed;U->LastButtonTime=Now;
    if(U->PowerHeld && Now-U->PowerSince>CR_CHARGER_HOLD_US) {
      U->ResetIssued=TRUE;return Exact(U->Io.Reboot(U->Io.Context));
    }
  }
  WantOn=U->PowerHeld || Now-U->LastActivity<CR_CHARGER_IDLE_US;
  if(!U->ScreenKnown || U->ScreenOn!=WantOn) {
    /* Failed/pending power changes remain unknown and are retried. Never
       publish screen-off based only on drawing black pixels. */
    U->ScreenKnown=FALSE;S=Exact(U->Io.Power(U->Io.Context,WantOn));
    if(S!=EFI_SUCCESS)return S;
    U->ScreenOn=WantOn;U->ScreenKnown=TRUE;U->Drawn=FALSE;
  }
  if(!U->ScreenOn)return EFI_SUCCESS;
  Changed=!U->Drawn || U->LastSocValid!=I->SocValid || U->LastCharging!=I->Charging ||
    (I->SocValid && U->LastSoc!=I->SocHundredths);
  if(Changed || (I->Charging && Now-U->LastDraw>=CR_CHARGER_ANIMATION_US)) {
    UINT32 Frame=I->Charging ? (UINT32)((Now/CR_CHARGER_ANIMATION_US)%5) : 0;
    S=Exact(U->Io.Draw(U->Io.Context,I,Frame));if(S!=EFI_SUCCESS)return S;
    U->LastDraw=Now;U->LastSoc=I->SocHundredths;U->LastSocValid=I->SocValid;
    U->LastCharging=I->Charging;U->Animation=Frame;U->Drawn=TRUE;
  }
  return EFI_SUCCESS;
}
STATIC VOID Rectangle(EFI_GRAPHICS_OUTPUT_BLT_PIXEL *P,UINT32 X,UINT32 Y,UINT32 W,UINT32 H,
                      UINT8 R,UINT8 G,UINT8 B) {
  UINT32 I,J;EFI_GRAPHICS_OUTPUT_BLT_PIXEL C={B,G,R,0};
  if(X>=CR_CHARGER_CANVAS_WIDTH || Y>=CR_CHARGER_CANVAS_HEIGHT)return;
  if(W>CR_CHARGER_CANVAS_WIDTH-X)W=CR_CHARGER_CANVAS_WIDTH-X;
  if(H>CR_CHARGER_CANVAS_HEIGHT-Y)H=CR_CHARGER_CANVAS_HEIGHT-Y;
  for(J=0;J<H;++J)for(I=0;I<W;++I)P[(Y+J)*CR_CHARGER_CANVAS_WIDTH+X+I]=C;
}
STATIC CONST UINT8 Digits[10][7]={
 {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
 {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
 {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
 {14,17,17,15,1,1,14}};
STATIC VOID Glyph(EFI_GRAPHICS_OUTPUT_BLT_PIXEL *P,UINT32 X,CONST UINT8 Rows[7]) {
  UINT32 I,J;for(J=0;J<7;++J)for(I=0;I<5;++I)if(Rows[J]&(1U<<(4-I)))
    Rectangle(P,X+I*4,240+J*4,4,4,232,236,242);
}
EFI_STATUS CrChargerUiRender(EFI_GRAPHICS_OUTPUT_BLT_PIXEL *P,UINTN Count,
                            CONST CR_CHARGER_UI_INPUT *I,UINT32 Frame) {
  UINT32 Fill,Percent,DigitsCount,X;UINT8 R,G;
  STATIC CONST UINT8 PercentGlyph[7]={24,25,2,4,8,19,3};
  STATIC CONST UINT8 DashGlyph[7]={0,0,0,31,0,0,0};
  if(!P||!I||Count<CR_CHARGER_CANVAS_WIDTH*CR_CHARGER_CANVAS_HEIGHT||I->SocHundredths>10000)
    return EFI_INVALID_PARAMETER;
  ZeroMem(P,CR_CHARGER_CANVAS_WIDTH*CR_CHARGER_CANVAS_HEIGHT*sizeof(*P));
  Rectangle(P,72,18,48,10,208,216,224);Rectangle(P,40,28,112,192,208,216,224);
  Rectangle(P,46,34,100,180,0,0,0);
  Fill=I->SocValid?(I->SocHundredths*172U/10000):0;
  R=I->SocValid&&I->SocHundredths<1500?234:38;G=R==234?74:190;
  if(Fill)Rectangle(P,50,210-Fill,92,Fill,R,G,96);
  if(I->Charging) {
    /* Five ascending segments, deterministic and independent of key/timer
       activity. They remain visible even with an unknown battery SOC. */
    UINT32 Segment;for(Segment=0;Segment<=Frame%5;++Segment)
      Rectangle(P,82,188-Segment*30,28,16,80,225,240);
  }
  if(!I->SocValid){Glyph(P,60,DashGlyph);Glyph(P,84,DashGlyph);Glyph(P,108,PercentGlyph);}
  else {
    Percent=I->SocHundredths/100;DigitsCount=Percent==100?3:Percent>=10?2:1;
    X=(CR_CHARGER_CANVAS_WIDTH-(DigitsCount+1)*24+4)/2;
    if(Percent==100){Glyph(P,X,Digits[1]);X+=24;Glyph(P,X,Digits[0]);X+=24;}
    else if(Percent>=10){Glyph(P,X,Digits[Percent/10]);X+=24;}
    Glyph(P,X,Digits[Percent%10]);Glyph(P,X+24,PercentGlyph);
  }
  return EFI_SUCCESS;
}
