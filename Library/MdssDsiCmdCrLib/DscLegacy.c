/* SM8250 DSC 1.1 encoder (the pre-DPU8 register layout).
 * Derived from Qualcomm cad09e061ef6 sde_hw_dsc.c/sde_encoder.c and
 * Linux dpu_hw_dsc.c. Copyright (c) 2015-2020 The Linux Foundation.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "Internal.h"

#define DSC 0x80000U
#define PP  0x70000U
#define DSC_MUX (DSC + 0x1800U)

static CR_STATUS Selected(CR_MDSS_CMD *D)
{
  CR_MDSS_CMD_PLAN P;
  CR_STATUS S;
  if (!D || !D->Initialized) return CR_INVALID_PARAMETER;
  S = CrMdssCmdBuildPlanProfile(&D->Profile, &P);
  if (S != CR_SUCCESS) return S;
  if (cr_memcmp(&P, &D->Plan, sizeof(P))) return CR_INVALID_PARAMETER;
  return CmdReady(D, CrMdssCmdDpu);
}

static CR_STATUS Fail(CR_MDSS_CMD *D, CR_STATUS S)
{
  D->CleanupPending = TRUE;
  return S;
}

CR_STATUS CmdDscStart(CR_MDSS_CMD *D)
{
  CR_STATUS S = Selected(D);
  if (S != CR_SUCCESS) return S;
  if (D->DscTouched || D->DpuTouched || D->CleanupPending)
    return CR_NOT_READY;

  const CR_MDSS_DSC_CONFIG *C = &D->Plan.Dsc;
  const CR_MDSS_DSC_PARAMS *P = &C->Params;
  /* The target Android Hardent latency calculation: 44+48-1 SSM delay,
   * 28 pipeline cycles, 64 output rate, plus one soft-slice chunk latency.
   * This exact two-soft-slice/single-encoder command profile yields 3. */
  UINT32 InputLatency = 28U + 3U * (91U + 2U);
  UINT32 OutputLatency = (9U * 64U + 48U + 7U) / 8U + 1U;
  UINT32 MultiLatency = (8U * C->ChunkBytes + 7U) / 8U;
  UINT32 Lines = (C->InitialTransmitDelay + InputLatency + OutputLatency +
                  MultiLatency + P->SliceWidth - 1U) / P->SliceWidth;
  UINT32 LastGroup = P->SliceWidth % 3U;
  LastGroup = LastGroup ? LastGroup - 1U : 2U;
  UINT32 Enc = (Lines << 20) | (LastGroup << 18) | (P->BppQ4 << 8) |
      (P->BlockPrediction ? CMDBIT(7) : 0U) | (C->LineBufferDepth << 3) | CMDBIT(1);
  static const UINT32 Offsets[] = {
    0x00,0x04,0x08,0x0c,0x10,0x14,0x18,0x1c,0x20,0x24,0x28,0x2c,0x30,0x34,0x38
  };
  UINT32 Values[] = {
    0, Enc, (P->PictureWidth << 16) | P->PictureHeight,
    (P->SliceWidth << 16) | P->SliceHeight, C->ChunkBytes << 16,
    (C->InitialDecodeDelay << 16) | C->InitialTransmitDelay,
    C->InitialScale, C->ScaleDecrement, C->ScaleIncrement, C->FirstLineBpg,
    (C->NflBpg << 16) | C->SliceBpg, (C->InitialOffset << 16) | C->FinalOffset,
    (2U << 10) | (C->FlatnessMaxQp << 5) | C->FlatnessMinQp, C->RcModelSize,
    (3U << 18) | (3U << 14) | (C->QuantLimit << 9) | (C->QuantLimit << 4) | 6U
  };

  D->DscTouched = TRUE; /* precedes every possibly committed MMIO effect */
  for (UINT32 I = 0; I < sizeof(Offsets) / sizeof(Offsets[0]); ++I) {
    S = CmdChecked(D, CrMdssCmdDpu, DSC + Offsets[I], Values[I], 0xffffffffU);
    if (S != CR_SUCCESS) return Fail(D, S);
  }
  for (UINT32 I = 0; I < 14; ++I) {
    S = CmdChecked(D, CrMdssCmdDpu, DSC + 0x3c + 4U * I, C->Thresholds[I], 0xffU);
    if (S != CR_SUCCESS) return Fail(D, S);
  }
  for (UINT32 I = 0; I < CR_MDSS_DSC_RANGES; ++I) {
    S = CmdChecked(D, CrMdssCmdDpu, DSC + 0x74 + 4U * I, C->Ranges[I].MinQp, 0x1fU);
    if (S != CR_SUCCESS) return Fail(D, S);
  }
  for (UINT32 I = 0; I < CR_MDSS_DSC_RANGES; ++I) {
    S = CmdChecked(D, CrMdssCmdDpu, DSC + 0xb0 + 4U * I, C->Ranges[I].MaxQp, 0x1fU);
    if (S != CR_SUCCESS) return Fail(D, S);
  }
  for (UINT32 I = 0; I < CR_MDSS_DSC_RANGES; ++I) {
    /* Target Android reads its plain char table through char*. AArch64's
     * unsigned plain char writes the byte representation. The hardware
     * field is signed six-bit; reserved readback bits are ignored. */
    S = CmdChecked(D, CrMdssCmdDpu, DSC + 0xec + 4U * I,
        (UINT32)(UINT8)C->Ranges[I].BpgOffset, 0x3fU);
    if (S != CR_SUCCESS) return Fail(D, S);
  }
  S = CmdChecked(D, CrMdssCmdDpu, DSC_MUX, 0, 0xfU);
  if (S != CR_SUCCESS) return Fail(D, S);
  UINT32 V;
  S = CmdRead(D, CrMdssCmdDpu, PP + 0xc8, &V);
  if (S != CR_SUCCESS) return Fail(D, S);
  S = CmdChecked(D, CrMdssCmdDpu, PP + 0xc8, V | CMDBIT(18), CMDBIT(18));
  if (S != CR_SUCCESS) return Fail(D, S);
  S = CmdChecked(D, CrMdssCmdDpu, PP + 0xa0, 1, 1);
  return S == CR_SUCCESS ? S : Fail(D, S);
}

CR_STATUS CmdDscStop(CR_MDSS_CMD *D)
{
  if (!D || !D->Initialized) return CR_INVALID_PARAMETER;
  if (!D->DscTouched) return CR_SUCCESS;
  /* COMMON_MODE=0 also means the enabled single command encoder. It is
   * not an off proof. The actual fetch/route fence must precede this phase. */
  if (D->DpuTouched || D->FrameInFlight || D->AutoRefresh || D->IrqOwned)
    return CR_NOT_READY;
  CR_STATUS S = CmdChecked(D, CrMdssCmdDpu, PP + 0xa0, 0, 1);
  if (S != CR_SUCCESS) return Fail(D, S);
  S = CmdChecked(D, CrMdssCmdDpu, DSC_MUX, 0xf, 0xf);
  if (S != CR_SUCCESS) return Fail(D, S);
  S = CmdChecked(D, CrMdssCmdDpu, DSC, 0, 0xffffffffU);
  if (S != CR_SUCCESS) return Fail(D, S);
  D->DscTouched = FALSE;
  return CR_SUCCESS;
}
