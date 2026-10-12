/* Real external-clock adaptation shared by native DSI caller and Windows.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <Library/MdssDsiCrLib.h>
CR_STATUS CrMdssDsiClockPlan(CONST CR_MDSS_DSI_PLAN *P,UINT32 I,CR_CLOCK_DSI_PLAN *Out) {
  if(!P || !Out || I>1)return CR_INVALID_PARAMETER;
  CR_MDSS_DSI_PLAN Fresh;
  CR_STATUS S=CrMdssDsiBuildPlan(&P->Mode,&Fresh);if(S!=CR_SUCCESS)return S;
  if(I<P->Mode.FirstController || I>=P->Mode.FirstController+P->Mode.Controllers ||
      P->PixelHz!=Fresh.PixelHz || P->ByteHz!=Fresh.ByteHz || P->SymbolHz!=Fresh.SymbolHz ||
      P->VcoHz!=Fresh.VcoHz || P->PixelSourceHz!=Fresh.PixelSourceHz ||
      P->PllPostDivider!=Fresh.PllPostDivider || P->PhyPostDivider!=Fresh.PhyPostDivider ||
      P->PixelDivider!=Fresh.PixelDivider || P->PixelMux!=Fresh.PixelMux ||
      P->PixelM!=Fresh.PixelM || P->PixelN!=Fresh.PixelN ||
      P->ByteInterfaceDivider!=Fresh.ByteInterfaceDivider)return CR_INVALID_PARAMETER;
  CR_CLOCK_DSI_PLAN C={CR_CLOCK_DSI_PLAN_REVISION,I,P->PixelHz,P->ByteHz,P->PixelSourceHz,
      P->PixelM,P->PixelN,P->ByteInterfaceDivider,0};
  S=CrClockDsiPlanValidate(&C);if(S!=CR_SUCCESS)return S;
  *Out=C;return CR_SUCCESS;
}
CR_STATUS CrMdssDsiClockOwnerSet(VOID *Context,UINT32 I,
    CONST CR_MDSS_DSI_PLAN *Plan,BOOLEAN On,BOOLEAN *Owned) {
  CR_MDSS_DSI_CLOCK_OWNER *C=Context;
  if(!C || !C->Provider || !C->Owner || !C->Ready || !Owned || I>1 || On>1)return CR_INVALID_PARAMETER;
  CR_CLOCK_EXTERNAL *E=C->Provider;
  /* Observe retained mutation before a fallible admission check. A failed
     off never licenses withdrawal of the resources sustaining that journal. */
  *Owned=E->Owner==C->Owner && E->PlanValid[I];
  CR_STATUS S=C->Ready(C->Context);if(S!=CR_SUCCESS)return CR_ERROR(S)?S:CR_NOT_READY;
  CR_CLOCK_DSI_PLAN P;
  if(On){S=CrMdssDsiClockPlan(Plan,I,&P);if(S!=CR_SUCCESS)return S;}
  S=CrClockExternalSetPlan(E,C->Owner,I,On?&P:NULL,On);
  *Owned=E->Owner==C->Owner && E->PlanValid[I];
  return S;
}
