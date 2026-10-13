/* SPDX-License-Identifier: MIT */
#include <Library/ClockExternalPlan.h>
CR_STATUS CrClockDsiPlanValidate(CONST CR_CLOCK_DSI_PLAN *P) {
  if(!P || P->Revision!=CR_CLOCK_DSI_PLAN_REVISION || P->Controller>1 || P->Reserved ||
      !P->PixelHz || !P->ByteHz || !P->PixelSourceHz || !P->PixelM || !P->PixelN ||
      P->PixelM>P->PixelN || P->PixelN>255 ||
      (P->ByteInterfaceDivider!=1 && P->ByteInterfaceDivider!=2))return CR_INVALID_PARAMETER;
  /* The source-qualified 5nm VCO ceiling is 5GHz. This also bounds every
     multiplication below without relying on machine-width arithmetic. */
  if(P->PixelHz>5000000000ULL || P->ByteHz>5000000000ULL ||
      P->PixelSourceHz>5000000000ULL || P->ByteHz%P->ByteInterfaceDivider)
    return CR_UNSUPPORTED;
  UINT64 Rate=P->PixelSourceHz*P->PixelM/P->PixelN;
  UINT64 Delta=Rate>P->PixelHz?Rate-P->PixelHz:P->PixelHz-Rate;
  /* PHY divider and requested payload clocks each round down to integer Hz.
     Two integer divisions can differ by at most two Hz. */
  return Delta<=2 ? CR_SUCCESS:CR_UNSUPPORTED;
}
BOOLEAN CrClockDsiPlanEqual(CONST CR_CLOCK_DSI_PLAN *A,CONST CR_CLOCK_DSI_PLAN *B) {
  return A && B && A->Revision==B->Revision && A->Controller==B->Controller &&
    A->PixelHz==B->PixelHz && A->ByteHz==B->ByteHz && A->PixelSourceHz==B->PixelSourceHz &&
    A->PixelM==B->PixelM && A->PixelN==B->PixelN &&
    A->ByteInterfaceDivider==B->ByteInterfaceDivider && A->Reserved==B->Reserved;
}
