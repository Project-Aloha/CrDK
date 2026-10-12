/** SST link programming adapted from Linux msm/dp/dp_ctrl.c and dp_panel.c.
    Copyright (c) 2012-2020, The Linux Foundation.
    SPDX-License-Identifier: GPL-2.0-only */
#include "DpLink.h"
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

BOOLEAN CrDpLaneStatus(CONST UINT8 *S, UINT8 Lanes, BOOLEAN Equalized)
{
  UINT8 I, Mask = Equalized ? 7 : 1;
  if (S == NULL || (Lanes != 1 && Lanes != 2 && Lanes != 4)) return FALSE;
  for (I = 0; I < Lanes; ++I)
    if (((S[I / 2] >> ((I % 2) * 4)) & Mask) != Mask) return FALSE;
  return !Equalized || (S[2] & BIT0) != 0;
}

EFI_STATUS CrDpCalculateTu(CONST CR_DP_TIMING *T, UINT32 Rate, UINT8 Lanes, CR_DP_TU *Tu)
{
  UINT64 Denominator, Numerator, Error, BestError, Extra, Pixels, LinkHz, Delay;
  UINT32 Size, BestSize = 0, Valid, HBlank;
  if (T == NULL || Tu == NULL || T->PixelHz == 0 || T->Width == 0 ||
      (Lanes != 1 && Lanes != 2 && Lanes != 4) ||
      (Rate != 1620 && Rate != 2700 && Rate != 5400 && Rate != 8100))
    return EFI_INVALID_PARAMETER;
  LinkHz = (UINT64)Rate * 100000;
  Denominator = LinkHz * Lanes;
  Numerator = (UINT64)T->PixelHz * 3;
  if (Numerator >= Denominator) return EFI_UNSUPPORTED;
  /* Linux's legacy RGB TU calculation: minimize the rounded boundary error,
     then buffer its accumulated excess over one scanline. All ratios share
     a denominator, so comparisons need no fixed-point rounding. */
  BestError = Denominator;
  for (Size = 32; Size <= 64; ++Size) {
    Valid = (UINT32)((Numerator * Size + Denominator - 1) / Denominator);
    if (Valid == 0 || Valid >= Size) continue;
    Error = Valid * Denominator - Numerator * Size;
    if (Error < BestError) { BestError = Error; BestSize = Size; }
  }
  if (BestSize == 0) return EFI_UNSUPPORTED;
  Valid = (UINT32)((Numerator * BestSize + Denominator - 1) / Denominator);
  Extra = ((UINT64)T->Width * 3 / Valid + 1) * BestError;
  /* As in Linux, a sub-byte accumulated excess needs no extra pixel. */
  Extra = Extra < Denominator ? 0 : (Extra + Denominator - 1) / Denominator;
  Pixels = (Extra + 2) / 3;
  Delay = (Pixels * LinkHz + T->PixelHz - 1) / T->PixelHz + BestSize - Valid +
          (4 * LinkHz + T->PixelHz - 1) / T->PixelHz;
  HBlank = T->HFront + T->HSync + T->HBack;
  if (Delay > MAX_UINT16 || HBlank <= 4 ||
      Delay * T->PixelHz >= (UINT64)(HBlank - 4) * LinkHz ||
      Delay * T->PixelHz >= (UINT64)T->Width * LinkHz) return EFI_UNSUPPORTED;
  Tu->Size = (UINT8)BestSize; Tu->Valid = (UINT8)Valid; Tu->Delay = (UINT16)Delay;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS SetLevels(EFI_USB_DP_PHY_CR_PROTOCOL *Phy, CONST UINT8 *Status,
                            UINT8 Lanes, UINT8 *Voltage, UINT8 *Pre)
{
  UINT8 I, V = 0, P = 0, Lane[4];
  EFI_STATUS Result;
  if (Status != NULL) for (I = 0; I < Lanes; ++I) {
    UINT8 Request = (Status[4 + I / 2] >> ((I % 2) * 4)) & 15;
    if ((Request & 3) > V) V = Request & 3;
    if ((Request >> 2) > P) P = Request >> 2;
  }
  if (V + P > 3) P = 3 - V;
  *Voltage = V; *Pre = P;
  Result = Phy->SetDpLevels(Phy, V, P);
  if (EFI_ERROR(Result)) return Result;
  for (I = 0; I < Lanes; ++I)
    Lane[I] = V | (P << 3) | (V == 3 ? BIT2 : 0) | (P == 3 - V ? BIT5 : 0);
  return CrDpAuxTransfer(TRUE, FALSE, 0x103, Lane, Lanes);
}

STATIC EFI_STATUS Pattern(UINT8 Pattern)
{
  UINT8 Sink = Pattern == 4 ? 7 : Pattern | BIT5;
  EFI_STATUS Status;
  MmioWrite32(CR_DP_LINK + 4, 0);
  MmioWrite32(CR_DP_LINK + 4, (UINT32)1 << (Pattern - 1));
  Status = CrDpWait(CR_DP_LINK + 0x40, (UINT32)1 << (Pattern + 2),
                    (UINT32)1 << (Pattern + 2), 10000);
  if (EFI_ERROR(Status)) return Status;
  return CrDpAuxTransfer(TRUE, FALSE, 0x102, &Sink, 1);
}

EFI_STATUS CrDpTrain(EFI_USB_DP_PHY_CR_PROTOCOL *Phy, CONST UINT8 *Dpcd,
                    UINT32 Rate, UINT8 Lanes)
{
  UINT8 Config[2], LinkStatus[6] = {0}, V = 0, P = 0, LastV = 0, SameV = 0;
  UINT32 I, Delay;
  EFI_STATUS Status;
  UINT8 Zero = 0;
  if (Phy == NULL || Phy->SetDpLevels == NULL || Dpcd == NULL ||
      (Lanes != 1 && Lanes != 2 && Lanes != 4)) return EFI_INVALID_PARAMETER;
  MmioWrite32(CR_DP_LINK, BIT1);
  MmioWrite32(CR_DP_LINK, 0);
  MmioWrite32(CR_DP_LINK, BIT0 | BIT25 |
              ((MmioRead32(CR_DP_AHB) >= 0x10020000) ? (3U << 23) : BIT23));
  MmioWrite32(CR_DP_LINK + 8, (2U << 13) | ((Lanes - 1) << 4) |
              ((Dpcd[2] & BIT7) ? BIT6 : 0) | BIT8 | BIT2 | BIT1 | BIT0);
  MmioWrite32(CR_DP_LINK + 0x38, 0xE4);
  Config[0] = (UINT8)(Rate / 270); Config[1] = Lanes | (Dpcd[2] & BIT7);
  Status = CrDpAuxTransfer(TRUE, FALSE, 0x100, Config, 2);
  if (EFI_ERROR(Status)) goto Out;
  Config[0] = 0; Config[1] = 1; /* PHY uses SSC disabled; ANSI 8b/10b */
  Status = CrDpAuxTransfer(TRUE, FALSE, 0x107, Config, 2);
  if (EFI_ERROR(Status)) goto Out;
  Status = Pattern(1);
  if (EFI_ERROR(Status)) goto Out;
  Status = SetLevels(Phy, NULL, Lanes, &V, &P);
  if (EFI_ERROR(Status)) goto Out;
  Delay = Dpcd[0] >= 0x14 || (Dpcd[14] & 0x7F) == 0 ? 100 : (Dpcd[14] & 0x7F) * 4000;
  if (Delay > 16000) { Status = EFI_UNSUPPORTED; goto Out; }
  for (I = 0; I < 20; ++I) {
    MicroSecondDelay(Delay);
    Status = CrDpAuxTransfer(TRUE, TRUE, 0x202, LinkStatus, 6);
    if (EFI_ERROR(Status)) goto Out;
    if (CrDpLaneStatus(LinkStatus, Lanes, FALSE)) break;
    if (V == 3 || SameV == 5) { Status = EFI_TIMEOUT; goto Out; }
    LastV = V;
    Status = SetLevels(Phy, LinkStatus, Lanes, &V, &P);
    if (EFI_ERROR(Status)) goto Out;
    SameV = V == LastV ? SameV + 1 : 0;
  }
  if (I == 20) { Status = EFI_TIMEOUT; goto Out; }
  Status = Pattern((Dpcd[3] & BIT7) ? 4 : (Dpcd[2] & BIT6) ? 3 : 2);
  if (EFI_ERROR(Status)) goto Out;
  Delay = (Dpcd[14] & 0x7F) == 0 ? 400 : (Dpcd[14] & 0x7F) * 4000;
  if (Delay > 16000) { Status = EFI_UNSUPPORTED; goto Out; }
  for (I = 0; I < 6; ++I) {
    MicroSecondDelay(Delay);
    Status = CrDpAuxTransfer(TRUE, TRUE, 0x202, LinkStatus, 6);
    if (EFI_ERROR(Status)) goto Out;
    if (!CrDpLaneStatus(LinkStatus, Lanes, FALSE)) { Status = EFI_DEVICE_ERROR; goto Out; }
    if (CrDpLaneStatus(LinkStatus, Lanes, TRUE)) break;
    Status = SetLevels(Phy, LinkStatus, Lanes, &V, &P);
    if (EFI_ERROR(Status)) goto Out;
  }
  if (I == 6) Status = EFI_TIMEOUT;
Out:
  MmioWrite32(CR_DP_LINK + 4, 0);
  {
    EFI_STATUS ClearStatus = CrDpAuxTransfer(TRUE, FALSE, 0x102, &Zero, 1);
    if (!EFI_ERROR(Status)) Status = ClearStatus;
  }
  DEBUG((EFI_ERROR(Status) ? DEBUG_ERROR : DEBUG_INIT,
         "MDSSDpCr: train rate=%u lanes=%u lane-status=%02x/%02x align=%02x V=%u P=%u: %r\n",
         Rate, Lanes, LinkStatus[0], LinkStatus[1], LinkStatus[2], V, P, Status));
  return Status;
}

VOID CrDpConfigureStream(CONST CR_DP_TIMING *T, UINT32 Rate, CONST CR_DP_TU *Tu)
{
  UINT32 HTotal = T->Width + T->HFront + T->HSync + T->HBack;
  UINT32 VTotal = T->Height + T->VFront + T->VSync + T->VBack;
  UINT32 M = T->PixelHz, N = Rate * 100000, A = M, B = N, G, Scale;
  /* A memory-fed stream must not inherit the DP pixel generator. Match
     Linux msm_dp_panel_tpg_disable / Android dp_catalog_panel_tpg_cfg(false)
     on our owned P0 bank, independently of unrelated interface fields. */
  DEBUG((DEBUG_INIT, "MDSSDpCr: P0 inherited BIST=%08x timing=%08x TPG=%08x; select DPU pixels\n",
    MmioRead32(CR_DP_P0), MmioRead32(CR_DP_P0 + 0x10), MmioRead32(CR_DP_P0 + 0x60)));
  MmioWrite32(CR_DP_P0 + 0x60, 0);
  MmioWrite32(CR_DP_P0, 0);
  MmioAnd32(CR_DP_P0 + 0x10, ~(UINT32)BIT0);
  /* Linux clears DSC DTO before every ordinary uncompressed stream. */
  MmioWrite32(CR_DP_P0 + 0x7C, 0);
  MemoryFence();
  while (B != 0) { G = A % B; A = B; B = G; }
  M /= A; N /= A;
  Scale = N < 32768 ? (32768 + N - 1) / N : 1;
  /* Synchronous MSA uses the exact pixel/link ratio, like Linux's M/N
     clock-derived path. Scaling must not round either side separately. */
  MmioWrite32(CR_DP_LINK + 0x10, M * Scale); MmioWrite32(CR_DP_LINK + 0x18, N * Scale);
  MmioWrite32(CR_DP_LINK + 0x1C, HTotal | (VTotal << 16));
  MmioWrite32(CR_DP_LINK + 0x20, (T->HSync + T->HBack) | ((T->VSync + T->VBack) << 16));
  MmioWrite32(CR_DP_LINK + 0x24, T->HSync | (T->HNegative ? BIT15 : 0) |
              ((UINT32)T->VSync << 16) | (T->VNegative ? BIT31 : 0));
  MmioWrite32(CR_DP_LINK + 0x28, T->Width | ((UINT32)T->Height << 16));
  MmioWrite32(CR_DP_LINK + 0x2C, BIT5 | BIT0);
  MmioWrite32(CR_DP_LINK + 0x30, Tu->Valid | ((UINT32)Tu->Delay << 16));
  MmioWrite32(CR_DP_LINK + 0x34, 0); MmioWrite32(CR_DP_LINK + 0x4C, Tu->Size - 1);
  /* Linux dp_panel_timing_cfg and Android dp_catalog_panel_timing_cfg only
     select wide-bus mode here. Preserve the other P0 interface fields. */
  MmioAnd32(CR_DP_P0 + 0x14, ~(UINT32)BIT4); /* single-pixel RGB */
}

EFI_STATUS CrDpObserveLink(VOID)
{
  UINT8 Sink[6] = {0};
  UINT8 Lanes = (UINT8)(((MmioRead32(CR_DP_LINK + 8) >> 4) & 3) + 1);
  EFI_STATUS Status = CrDpAuxTransfer(TRUE, TRUE, 0x202, Sink, sizeof(Sink));
  if (!EFI_ERROR(Status) && !CrDpLaneStatus(Sink, Lanes, TRUE)) Status = EFI_DEVICE_ERROR;
  DEBUG((EFI_ERROR(Status) ? DEBUG_ERROR : DEBUG_WARN,
    "MDSSDpCr: retained sink lanes=%u status=%02x/%02x align=%02x adjust=%02x/%02x: %r\n",
    Lanes, Sink[0], Sink[1], Sink[2], Sink[4], Sink[5], Status));
  return Status;
}
