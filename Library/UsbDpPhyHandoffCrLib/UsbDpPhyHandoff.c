/* Known native policy follows UsbDpPhyCrLib's SM8450-HDK Init journal.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <Library/UsbDpPhyHandoffCrLib.h>
CR_STATIC_ASSERT(CR_USB_DP_PHY_CLOCKS == 4, "native PHY clock policy requires review");
CR_STATIC_ASSERT(CR_USB_DP_PHY_RAILS == 6, "native PHY rail policy requires review");

typedef struct { CONST CHAR8 *Name; UINT64 Rate; } CLOCK_POLICY;
typedef struct { CONST CHAR8 *Name; UINT32 Millivolts, Mode; } RAIL_POLICY;
STATIC CONST CLOCK_POLICY Clocks[CR_USB_DP_PHY_CLOCKS] = {
  {"gcc_usb3_0_clkref_en", 0},
  {"gcc_usb3_prim_phy_aux_clk", 19200000},
  {"gcc_usb3_prim_phy_com_aux_clk", 19200000},
  {"gcc_usb3_prim_phy_pipe_clk", 0}
};
STATIC CONST RAIL_POLICY Rails[CR_USB_DP_PHY_RAILS] = {
  {"bobc1", 3296, 6}, {"ldob1", 912, 7}, {"ldob6", 1200, 7},
  {"ldob5", 880, 7}, {"ldoc1", 1800, 7}, {"ldob2", 3072, 7}
};
STATIC BOOLEAN Range(UINTN P, UINTN N) { return P && N && P <= (UINTN)-1 - N; }
STATIC BOOLEAN Overlap(UINTN A, UINTN An, UINTN B, UINTN Bn) {
  return A < B + Bn && B < A + An;
}
STATIC BOOLEAN Name(CONST CHAR8 *Actual, UINTN N, CONST CHAR8 *Expected) {
  UINTN I = 0;
  while (Expected[I]) {
    if (I >= N || Actual[I] != Expected[I]) return FALSE;
    ++I;
  }
  for (; I < N; ++I) if (Actual[I]) return FALSE;
  return TRUE;
}

CR_STATUS CrUsbDpPhyHandoffQualify(CONST CR_USB_NATIVE_HANDOFF *Usb,
    UINT64 Boot, CONST CR_MDSS_NATIVE_HANDOFF *M,
    CR_USB_DP_PHY_NATIVE_DEMANDS *Out) {
  if (!Range((UINTN)Usb, sizeof(*Usb)) || !Range((UINTN)M, sizeof(*M)) ||
      !Range((UINTN)Out, sizeof(*Out)) ||
      Overlap((UINTN)Usb, sizeof(*Usb), (UINTN)M, sizeof(*M)) ||
      Overlap((UINTN)Usb, sizeof(*Usb), (UINTN)Out, sizeof(*Out)) ||
      Overlap((UINTN)M, sizeof(*M), (UINTN)Out, sizeof(*Out)))
    return CR_INVALID_PARAMETER;
  CR_STATUS S = CrUsbHandoffCanTakeover(Usb, Boot);
  if (S != CR_SUCCESS) return S;
  S = CrMdssHandoffCanClaim(M);
  if (S != CR_SUCCESS) return S;
  if (M->Phase != CR_MDSS_NATIVE_FINAL ||
      M->RequiredOwners != CR_MDSS_NATIVE_ALL_OWNERS ||
      M->RegisteredOwners != M->RequiredOwners || M->FrozenOwners != M->RequiredOwners)
    return CR_NOT_READY;
  CONST CR_MDSS_NATIVE_ROUTE *Dp = &M->Routes[1];
  if (Dp->Owner != CR_MDSS_NATIVE_DP ||
      (Dp->State != CR_MDSS_NATIVE_ABSENT &&
       !(Dp->State == CR_MDSS_NATIVE_STOPPED &&
         Dp->MappingState == CR_MDSS_NATIVE_MAPPING_REVOKED &&
         Dp->MappingOwner == CR_MDSS_NATIVE_SMMU && !Dp->MappingStatus &&
         Dp->StreamCount == CR_MDSS_NATIVE_STREAMS))) return CR_NOT_READY;

  CONST CR_MDSS_NATIVE_PHY_STATE *P = &M->Phy;
  UINT32 WantClocks = 0, WantRails = 0;
  if (P->State != CR_MDSS_NATIVE_ABSENT) {
    /* The actual producer never emits STOPPED for an owned PHY. A clean
       journal needs all successful owner/readiness/touched witnesses, not
       just a ready bit or an arbitrary subset of the known demands. */
    UINT32 Want = CR_MDSS_NATIVE_PHY_COMMON | CR_MDSS_NATIVE_PHY_FSA;
    if (P->UsbOwners) Want |= CR_MDSS_NATIVE_PHY_USB;
    if (P->DpOwners) Want |= CR_MDSS_NATIVE_PHY_DP;
    if (P->State != CR_MDSS_NATIVE_LIVE || P->Flags != CR_MDSS_NATIVE_OWNED ||
        P->Status || P->Base != 0x088e8000ULL || P->Bytes != 0x4000 ||
        (!P->UsbOwners && !P->DpOwners) ||
        (P->ReadyMask & ~CR_MDSS_NATIVE_PHY_USB_SS) != Want ||
        P->TouchedMask != Want) return CR_NOT_READY;
    WantClocks = (1U << CR_USB_DP_PHY_CLOCKS) - 1U;
    WantRails = P->UsbOwners ? (1U << CR_USB_DP_PHY_RAILS) - 1U : 7U;
  }
  CR_USB_DP_PHY_NATIVE_DEMANDS Result = {0};
  Result.UsbBootGeneration = Usb->BootGeneration; Result.UsbTransferId = Usb->TransferId;
  Result.MdssGeneration = M->Generation; Result.MdssTransferId = M->TransferId;
  for (UINT32 I = 0; I < M->ClockCount; ++I) {
    CONST CR_MDSS_NATIVE_CLOCK *C = &M->Clocks[I];
    if (C->Owner != CR_MDSS_NATIVE_PHY) continue;
    UINT32 Slot = CR_USB_DP_PHY_CLOCKS;
    for (UINT32 J = 0; J < CR_USB_DP_PHY_CLOCKS; ++J)
      if (Name(C->Name, sizeof(C->Name), Clocks[J].Name)) { Slot = J; break; }
    if (Slot == CR_USB_DP_PHY_CLOCKS || C->Flags != CR_MDSS_NATIVE_OWNED ||
        !Name(C->Controller, sizeof(C->Controller), "gcc") || C->RateHz != Clocks[Slot].Rate ||
        (Result.ClockMask & (1U << Slot))) return CR_NOT_READY;
    Result.ClockMask |= 1U << Slot; Result.Clocks[Slot] = *C;
  }
  for (UINT32 I = 0; I < M->RailCount; ++I) {
    CONST CR_MDSS_NATIVE_RAIL *R = &M->Rails[I];
    if (R->Owner != CR_MDSS_NATIVE_PHY) continue;
    UINT32 Slot = CR_USB_DP_PHY_RAILS;
    for (UINT32 J = 0; J < CR_USB_DP_PHY_RAILS; ++J)
      if (Name(R->Name, sizeof(R->Name), Rails[J].Name)) { Slot = J; break; }
    if (Slot == CR_USB_DP_PHY_RAILS || R->Flags != CR_MDSS_NATIVE_OWNED ||
        R->Millivolts != Rails[Slot].Millivolts || R->Mode != Rails[Slot].Mode ||
        (Result.RailMask & (1U << Slot))) return CR_NOT_READY;
    Result.RailMask |= 1U << Slot; Result.Rails[Slot] = *R;
  }
  /* No PHY interconnect/domain journal exists in the qualified producer. */
  for (UINT32 I = 0; I < M->PathCount; ++I)
    if (M->Paths[I].Owner == CR_MDSS_NATIVE_PHY) return CR_NOT_READY;
  for (UINT32 I = 0; I < M->DomainCount; ++I)
    if (M->Domains[I].Owner == CR_MDSS_NATIVE_PHY) return CR_NOT_READY;
  if (Result.ClockMask != WantClocks || Result.RailMask != WantRails) return CR_NOT_READY;
  *Out = Result;
  return CR_SUCCESS;
}
