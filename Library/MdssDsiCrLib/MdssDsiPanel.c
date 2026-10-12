/* Android dsi_panel reset/command lifecycle, retained owner integration.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <Library/MdssDsiPanelCrLib.h>
#include "MdssDsiInternal.h"
#include <oskal/cr_string.h>
STATIC CR_STATUS Commands(CONST CR_MDSS_PANEL_COMMAND *C,UINT32 N,UINT32 Mask) {
  UINT64 Delay=0;
  if(N>CR_MDSS_PANEL_MAX_COMMANDS || (N&&!C) ||
     (N&&(UINTN)C>(UINTN)-1-(UINTN)N*sizeof(*C)))return CR_INVALID_PARAMETER;
  for(UINT32 I=0;I<N;++I) {
    CONST CR_MDSS_PANEL_COMMAND *P=&C[I];
    if(P->DataType==0xff) {
      if(P->Reserved||P->ControllerMask||P->LowPower>1||!P->DelayMs||P->DelayMs>1000||
         P->Payload||P->Bytes)return CR_INVALID_PARAMETER;
      Delay+=P->DelayMs;continue;
    }
    if(P->Reserved || P->LowPower>1 || !P->ControllerMask ||
       (P->ControllerMask&~Mask) || P->DelayMs>1000 ||
       !P->Payload || !P->Bytes || P->Bytes>252 ||
       (UINTN)P->Payload>(UINTN)-1-P->Bytes)return CR_INVALID_PARAMETER;
    if(P->DataType!=0x05 && P->DataType!=0x15 && P->DataType!=0x13 &&
       P->DataType!=0x23 && P->DataType!=0x29 && P->DataType!=0x39)return CR_UNSUPPORTED;
    if(((P->DataType==0x05||P->DataType==0x13)&&P->Bytes!=1) ||
       ((P->DataType==0x15||P->DataType==0x23)&&P->Bytes!=2))return CR_INVALID_PARAMETER;
    Delay+=P->DelayMs;
  }
  return Delay<=30000?CR_SUCCESS:CR_INVALID_PARAMETER;
}
CR_STATUS CrMdssPanelProfileValidate(CONST CR_MDSS_PANEL_PROFILE *P) {
  CR_MDSS_DSI_PLAN Plan;CR_STATUS S;UINT64 Delay=0;
  if(!P || P->Revision!=CR_MDSS_PANEL_PROFILE_REVISION || P->OffResetHigh>1 ||
     P->ResetCount>CR_MDSS_PANEL_MAX_RESETS || (P->ResetCount&&!P->Reset) ||
     (P->ResetCount&&(UINTN)P->Reset>(UINTN)-1-(UINTN)P->ResetCount*sizeof(*P->Reset)))return CR_INVALID_PARAMETER;
  S=CrMdssDsiBuildPlan(&P->Mode,&Plan);if(S!=CR_SUCCESS)return S;
  UINT32 Mask=((1U<<P->Mode.Controllers)-1U)<<P->Mode.FirstController;
  S=Commands(P->On,P->OnCount,Mask);if(S!=CR_SUCCESS)return S;
  S=Commands(P->Off,P->OffCount,Mask);if(S!=CR_SUCCESS)return S;
  for(UINT32 I=0;I<P->ResetCount;++I) {
    CONST CR_MDSS_PANEL_RESET *R=&P->Reset[I];
    if(R->High>1 || R->Reserved[0] || R->Reserved[1] || R->Reserved[2] || R->DelayUs>1000000)
      return CR_INVALID_PARAMETER;
    Delay+=R->DelayUs;
  }
  return Delay<=5000000?CR_SUCCESS:CR_INVALID_PARAMETER;
}
STATIC CR_STATUS Ready(CR_MDSS_DSI_PANEL *P) {return XExact(P->Io.Ready(P->Io.Context));}
STATIC CR_STATUS Wait(CR_MDSS_DSI_PANEL *P,UINT32 Us) {
  /* No service work at elevated TPL/IRQL; native/WDM caller controls it.
     Split waits keep live dependency checks bounded to at most20ms. */
  while(Us) {UINT32 Chunk=Us>20000?20000:Us;CR_STATUS S=Ready(P);if(S!=CR_SUCCESS)return S;
    S=XExact(P->Io.DelayUs(P->Io.Context,Chunk));if(S!=CR_SUCCESS)return S;Us-=Chunk;}
  return CR_SUCCESS;
}
STATIC CR_STATUS Run(CR_MDSS_DSI_PANEL *P,CONST CR_MDSS_PANEL_COMMAND *C,UINT32 N) {
  for(UINT32 I=0;I<N;++I) {
    if(C[I].DataType==0xff) {CR_STATUS S=Wait(P,C[I].DelayMs*1000);if(S!=CR_SUCCESS)return S;continue;}
    for(UINT32 J=P->Profile.Mode.FirstController;
        J<P->Profile.Mode.FirstController+P->Profile.Mode.Controllers;++J)if(C[I].ControllerMask&(1U<<J)) {
      CR_STATUS S=Ready(P);if(S!=CR_SUCCESS)return S;
      S=CrMdssDsiWritePacket(P->Dsi,J,C[I].DataType,C[I].Payload,C[I].Bytes,C[I].LowPower);
      if(S!=CR_SUCCESS)return S;
    }
    CR_STATUS S=Wait(P,C[I].DelayMs*1000);if(S!=CR_SUCCESS)return S;
  }
  return CR_SUCCESS;
}
CR_STATUS CrMdssDsiPanelInit(CR_MDSS_DSI_PANEL *P,CONST CR_MDSS_PANEL_PROFILE *F,CONST CR_MDSS_PANEL_IO *Io) {
  if(!P||!Io||!Io->Context||!Io->Ready||!Io->Acquire||!Io->Release||!Io->Reset||
     !Io->DelayUs||!Io->PublishPixels)return CR_INVALID_PARAMETER;
  if(P->Initialized)return CR_BUSY;
  CR_STATUS S=CrMdssPanelProfileValidate(F);if(S!=CR_SUCCESS)return S;
  CR_MDSS_DSI_PANEL V={0};V.Profile=*F;V.Io=*Io;V.Initialized=TRUE;*P=V;return CR_SUCCESS;
}
STATIC BOOLEAN SameMode(CONST CR_MDSS_DSI_MODE *A,CONST CR_MDSS_DSI_MODE *B) {
  /* Plans are zero-built canonical representations, avoiding caller padding. */
  CR_MDSS_DSI_PLAN X,Y;
  if(CrMdssDsiBuildPlan(A,&X)!=CR_SUCCESS || CrMdssDsiBuildPlan(B,&Y)!=CR_SUCCESS)return FALSE;
  /* BuildPlan copies Mode, which can contain padding. Compare the actual
     pixel/payload/clock/timing plans plus each scalar raster field. */
  CONST CR_MDSS_MODE *L=&A->Raster,*R=&B->Raster;
  if(A->Controllers!=B->Controllers||A->FirstController!=B->FirstController||A->PhyType!=B->PhyType||
     A->PixelHz!=B->PixelHz||A->ContinuousClock!=B->ContinuousClock||A->Dsc!=B->Dsc||
     A->SlicesPerPacket!=B->SlicesPerPacket||cr_memcmp(A->LaneMap,B->LaneMap,4))return FALSE;
#define EQ(n) if(L->n!=R->n)return FALSE
  EQ(Soc);EQ(Controller);EQ(Width);EQ(Height);EQ(Stride);EQ(HSync);EQ(HBackPorch);EQ(HFrontPorch);
  EQ(VSync);EQ(VBackPorch);EQ(VFrontPorch);EQ(RefreshQ16);EQ(Lanes);EQ(VirtualChannel);EQ(TrafficMode);
  EQ(VideoFlags);EQ(BorderColor);EQ(InvertHsync);EQ(InvertVsync);EQ(HsaHseAfterVsVe);EQ(DisableEot);
#undef EQ
  if(A->Dsc) {UINT8 U[128],V[128];
    if(CrMdssDscPackPps(&A->Compression,0,U,128)!=CR_SUCCESS ||
       CrMdssDscPackPps(&B->Compression,0,V,128)!=CR_SUCCESS||cr_memcmp(U,V,128))return FALSE;}
  return TRUE;
}
CR_STATUS CrMdssDsiPanelPrepare(VOID *Context,CR_MDSS_DSI *D,BOOLEAN *Owned) {
  CR_MDSS_DSI_PANEL *P=Context;
  if(!P||!P->Initialized||!D||!D->Initialized||!Owned)return CR_INVALID_PARAMETER;
  *Owned=P->PowerOwned||P->ResetTouched;
  if(P->Dsi&&P->Dsi!=D)return CR_BUSY;
  if(!SameMode(&P->Profile.Mode,&D->Plan.Mode))return CR_UNSUPPORTED;
  if(!D->Prepared||D->Active||D->CleanupPending||P->CleanupPending)return CR_NOT_READY;
  CR_STATUS S=Ready(P);if(S!=CR_SUCCESS)return S;
  if(P->Prepared){*Owned=TRUE;return CR_SUCCESS;}
  S=CrMdssPanelProfileValidate(&P->Profile);if(S!=CR_SUCCESS)return S;
  P->Dsi=D;P->CleanupPending=TRUE;
  BOOLEAN Held=TRUE;P->PowerOwned=TRUE;*Owned=TRUE;
  S=XExact(P->Io.Acquire(P->Io.Context,&Held));P->PowerOwned=Held!=FALSE;
  if(S!=CR_SUCCESS)return S;if(Held!=TRUE)return CR_NOT_READY;
  for(UINT32 I=0;I<P->Profile.ResetCount;++I) {
    S=Ready(P);if(S!=CR_SUCCESS)return S;P->ResetTouched=TRUE;
    S=XExact(P->Io.Reset(P->Io.Context,P->Profile.Reset[I].High));if(S!=CR_SUCCESS)return S;
    S=Wait(P,P->Profile.Reset[I].DelayUs);if(S!=CR_SUCCESS)return S;
  }
  S=Run(P,P->Profile.On,P->Profile.OnCount);if(S!=CR_SUCCESS)return S;
  P->Prepared=TRUE;P->CleanupPending=FALSE;return CR_SUCCESS;
}
CR_STATUS CrMdssDsiPanelUnprepare(VOID *Context,CR_MDSS_DSI *D,BOOLEAN *Owned) {
  CR_MDSS_DSI_PANEL *P=Context;
  if(!P||!P->Initialized||!D||!D->Initialized||!Owned)return CR_INVALID_PARAMETER;
  *Owned=P->PowerOwned||P->ResetTouched;
  if(P->Dsi&&P->Dsi!=D)return CR_BUSY;
  if(!*Owned){P->Prepared=P->CleanupPending=FALSE;P->Dsi=NULL;return CR_SUCCESS;}
  if(D->Active)return CR_NOT_READY;
  CR_STATUS S=Ready(P);if(S!=CR_SUCCESS)return S;P->CleanupPending=TRUE;
  /* Failure after an off packet may poison transport. Physical reset/power
     below are still attempted, and certify panel off without packet replay. */
  if(P->Prepared&&!D->CleanupPending&&D->Prepared) {
    S=Run(P,P->Profile.Off,P->Profile.OffCount);
    P->Prepared=FALSE;
    if(S!=CR_SUCCESS&&Ready(P)!=CR_SUCCESS)return S;
  }
  S=Ready(P);if(S!=CR_SUCCESS)return S;P->ResetTouched=TRUE;
  S=XExact(P->Io.Reset(P->Io.Context,P->Profile.OffResetHigh));if(S!=CR_SUCCESS)return S;
  if(P->PowerOwned) {BOOLEAN Held=TRUE;S=XExact(P->Io.Release(P->Io.Context,&Held));
    if(S!=CR_SUCCESS)return S;if(Held!=FALSE)return CR_NOT_READY;P->PowerOwned=FALSE;}
  P->ResetTouched=P->Prepared=P->CleanupPending=FALSE;P->Dsi=NULL;*Owned=FALSE;return CR_SUCCESS;
}
CR_STATUS CrMdssDsiPanelPublish(VOID *Context,VOID *Cpu,UINTN Bytes) {
  CR_MDSS_DSI_PANEL *P=Context;
  if(!P||!P->Initialized||!P->Prepared||P->CleanupPending||!Cpu||
     Bytes!=(UINTN)P->Profile.Mode.Raster.Stride*P->Profile.Mode.Raster.Height ||
     (UINTN)Cpu>(UINTN)-1-Bytes)return CR_INVALID_PARAMETER;
  CR_STATUS S=Ready(P);if(S!=CR_SUCCESS)return S;return XExact(P->Io.PublishPixels(P->Io.Context,Cpu,Bytes));
}
CR_STATUS CrMdssDsiPanelUninit(CR_MDSS_DSI_PANEL *P) {
  if(!P||!P->Initialized)return CR_INVALID_PARAMETER;
  if(P->PowerOwned||P->ResetTouched||P->CleanupPending)return CR_BUSY;
  cr_memset(P,0,sizeof(*P));return CR_SUCCESS;
}
