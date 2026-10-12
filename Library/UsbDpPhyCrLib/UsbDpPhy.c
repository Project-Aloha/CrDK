/** SM8450-HDK QMP combo/HS PHY and FSA4480 sequencing.
 * Register policy adapted from Linux phy-qcom-qmp-combo.c,
 * phy-qcom-snps-femto-v2.c and typec/mux/fsa4480.c, plus the Android
 * Waipio HDK param-override-seq. Copyright (c) 2017-2021, The Linux Foundation.
 * Copyright (C) 2021-2022 Linaro Ltd. SPDX-License-Identifier: GPL-2.0-only
 */
#include <Library/UsbDpPhyCrLib.h>
#include <oskal/cr_atomic.h>
#include <oskal/cr_memory.h>
#include <oskal/cr_string.h>
#include <oskal/cr_time.h>
#include "PhyTables.h"

#define B(N) (1U << (N))
#define QMP 0U
#define HS 0x10000U /* Internal window tag, never an address. */
#define DP 0x2a00U
#define PLL 0x2000U
#define PCS 0x1c00U
#define TX0 0x2200U
#define TX1 0x2600U
#define COUNT(A) (sizeof(A) / sizeof((A)[0]))

STATIC CR_STATUS Exact(CR_STATUS S) {
  return S == CR_SUCCESS ? S : CR_ERROR(S) ? S : CR_NOT_READY;
}
STATIC CR_STATUS Remember(CR_USB_DP_PHY *P, CR_STATUS S) {
  S = Exact(S);
  if (P->IoStatus == CR_SUCCESS && S != CR_SUCCESS) P->IoStatus = S;
  return S;
}
STATIC CR_STATUS Gate(CR_USB_DP_PHY *P) {
  if (P->IoStatus != CR_SUCCESS) return P->IoStatus;
  if (CrAtomicLoad32(&P->Frozen)) return Remember(P, CR_BUSY);
  return Remember(P, P->Io.Ready(P->Io.Context));
}
STATIC BOOLEAN Fits(CONST CR_USB_DP_PHY_VIEW *V, UINT32 Size) {
  return V->Address && !(V->Address & 3U) && V->Bytes >= Size &&
    V->Address <= (UINTN)-1 - V->Bytes;
}
STATIC CR_STATUS Begin(CR_USB_DP_PHY *P) {
  if (!P || !P->Initialized) return CR_INVALID_PARAMETER;
  if (CrAtomicLoad32(&P->Frozen)) return CR_BUSY;
  CR_STATUS S = CrMutexAcquire(&P->Lock);
  if (S != CR_SUCCESS) return Exact(S);
  CrAtomicOr32(&P->Active, 1);
  P->IoStatus = CR_SUCCESS;
  if (!P->Initialized || CrAtomicLoad32(&P->Frozen)) {
    CrAtomicAnd32(&P->Active, 0);
    CrMutexRelease(&P->Lock);
    return CR_BUSY;
  }
  return CR_SUCCESS;
}
STATIC CR_STATUS End(CR_USB_DP_PHY *P, CR_STATUS S) {
  S = Exact(S);
  if (S == CR_SUCCESS && P->IoStatus != CR_SUCCESS) S = P->IoStatus;
  CrMemoryFence();
  CrAtomicAnd32(&P->Active, 0);
  CrMutexRelease(&P->Lock);
  return S;
}
STATIC CR_USB_DP_PHY_WINDOW Window(UINT32 A) {
  return A >= HS ? CrUsbDpPhyHs : CrUsbDpPhyQmp;
}
STATIC UINT32 Offset(UINT32 A) { return A >= HS ? A - HS : A; }
STATIC UINT32 Read(CR_USB_DP_PHY *P, UINT32 A) {
  UINT32 V = 0, O = Offset(A);
  CR_USB_DP_PHY_WINDOW W = Window(A);
  if (Gate(P) != CR_SUCCESS) return 0;
  if ((O & 3U) || O > P->Config.View[W].Bytes - 4U) {
    Remember(P, CR_INVALID_PARAMETER); return 0;
  }
  P->LastWindow = W; P->LastOffset = O;
  Remember(P, P->Io.Read32(P->Io.Context, W, O, &V));
  P->LastValue = V;
  return V;
}
STATIC VOID Write(CR_USB_DP_PHY *P, UINT32 A, UINT32 V) {
  UINT32 O = Offset(A); CR_USB_DP_PHY_WINDOW W = Window(A);
  if (Gate(P) != CR_SUCCESS) return;
  if ((O & 3U) || O > P->Config.View[W].Bytes - 4U) {
    Remember(P, CR_INVALID_PARAMETER); return;
  }
  P->LastWindow = W; P->LastOffset = O; P->LastValue = V;
  Remember(P, P->Io.Write32(P->Io.Context, W, O, V));
}
STATIC VOID Update(CR_USB_DP_PHY *P, UINT32 A, UINT32 Mask, UINT32 V) {
  UINT32 Old = Read(P, A);
  Write(P, A, (Old & ~Mask) | (V & Mask));
}
STATIC VOID Or(CR_USB_DP_PHY *P, UINT32 A, UINT32 V) { Update(P, A, V, V); }
STATIC VOID And(CR_USB_DP_PHY *P, UINT32 A, UINT32 V) { Update(P, A, ~V, 0); }
STATIC CR_STATUS Wait(CR_USB_DP_PHY *P, UINT32 A, UINT32 Mask, UINT32 Want) {
  for (UINT32 I = 0; I < 100; ++I) {
    UINT32 V = Read(P, A);
    if (P->IoStatus != CR_SUCCESS) return P->IoStatus;
    if ((V & Mask) == Want) return CR_SUCCESS;
    cr_sleep(100);
  }
  return Remember(P, CR_TIMEOUT);
}
STATIC VOID Table(CR_USB_DP_PHY *P, UINT32 Base, CONST CR_PHY_REG *R,
    UINTN N, UINT8 Lane) {
  for (UINTN I = 0; I < N && P->IoStatus == CR_SUCCESS; ++I)
    if (R[I].LaneMask & Lane) Write(P, Base + R[I].Offset, R[I].Value);
}
#define LOAD(Base, Name, Lane) Table(P, Base, Name, COUNT(Name), Lane)
STATIC CR_STATUS Reset(CR_USB_DP_PHY *P, CONST CHAR8 *Name, BOOLEAN Assert) {
  if (Gate(P) != CR_SUCCESS) return P->IoStatus;
  return Remember(P, P->Io.Reset(P->Io.Context, Name, Assert));
}
STATIC CR_STATUS Rails(CR_USB_DP_PHY *P, UINT32 First, UINT32 N, BOOLEAN On) {
  for (UINT32 I = 0; I < N; ++I) {
    CR_USB_DP_PHY_RAIL *R = &P->Rails[First + (On ? I : N - 1U - I)];
    if ((On && R->Owned) || (!On && !R->Owned)) continue;
    if (Gate(P) != CR_SUCCESS) return P->IoStatus;
    BOOLEAN Owned = R->Owned;
    CR_STATUS S = Exact(P->Io.Rail(P->Io.Context, R->Name, R->Millivolts,
      R->Mode, On, &Owned));
    /* A failed final release cannot discard the pre-existing lease even
       if a faulty callback has cleared its output. Partial acquire owns
       exactly the callback's committed token. */
    if (On) R->Owned = Owned || S == CR_SUCCESS;
    else if (S == CR_SUCCESS && !Owned) R->Owned = FALSE;
    if (S == CR_SUCCESS && Owned != On) S = CR_NOT_READY;
    R->Status = S;
    if (Remember(P, S) != CR_SUCCESS) return S;
  }
  return CR_SUCCESS;
}
STATIC CR_STATUS Clock(CR_USB_DP_PHY *P, UINT32 I, BOOLEAN On) {
  CR_USB_DP_PHY_CLOCK *C = &P->Clocks[I];
  if ((On && C->Owned) || (!On && !C->Owned)) return CR_SUCCESS;
  if (Gate(P) != CR_SUCCESS) return P->IoStatus;
  BOOLEAN Owned = C->Owned;
  CR_STATUS S = Exact(P->Io.Clock(P->Io.Context, C->Name, C->Rate, On, &Owned));
  if (On) C->Owned = Owned || S == CR_SUCCESS;
  else if (S == CR_SUCCESS && !Owned) C->Owned = FALSE;
  if (S == CR_SUCCESS && Owned != On) S = CR_NOT_READY;
  C->Status = S;
  return Remember(P, S);
}
STATIC CR_STATUS SwitchRead(CR_USB_DP_PHY *P, UINT8 R, UINT8 *V) {
  if (Gate(P) != CR_SUCCESS) return P->IoStatus;
  return Remember(P, P->Io.SwitchRead(P->Io.Context, R, V));
}
STATIC CR_STATUS SwitchWrite(CR_USB_DP_PHY *P, UINT8 R, UINT8 V) {
  if (Gate(P) != CR_SUCCESS) return P->IoStatus;
  /* Local bus preparation owns no switch write. Once submitted, even an
     error can have committed and must retain the switch-safe journal. */
  BOOLEAN Dispatched = FALSE;
  CR_STATUS S = Exact(P->Io.SwitchWrite(P->Io.Context, R, V, &Dispatched));
  if (Dispatched) { P->SwitchTouched = TRUE; P->SwitchKnown = FALSE; }
  if (S == CR_SUCCESS && !Dispatched) S = CR_NOT_READY;
  return Remember(P, S);
}
STATIC CR_STATUS SwitchSet(CR_USB_DP_PHY *P, UINT8 Enable, UINT8 Select) {
  UINT8 E = 0, S = 0;
  if (P->SwitchKnown && P->SwitchEnable == Enable && P->SwitchSelect == Select)
    return Gate(P);
  if (SwitchWrite(P, 4, 0x98) != CR_SUCCESS) return P->IoStatus;
  cr_sleep(35);
  if (SwitchWrite(P, 5, Select) != CR_SUCCESS ||
      SwitchWrite(P, 4, Enable) != CR_SUCCESS) return P->IoStatus;
  if (Enable & 0x60U) cr_sleep(15);
  if (SwitchRead(P, 4, &E) != CR_SUCCESS || SwitchRead(P, 5, &S) != CR_SUCCESS)
    return P->IoStatus;
  if (E != Enable || S != Select) return Remember(P, CR_DEVICE_ERROR);
  P->SwitchEnable = Enable; P->SwitchSelect = Select; P->SwitchKnown = TRUE;
  return CR_SUCCESS;
}
STATIC CR_STATUS SwitchInit(CR_USB_DP_PHY *P) {
  UINT8 Id = 0;
  if (SwitchRead(P, 0, &Id) != CR_SUCCESS) return P->IoStatus;
  if (Id == 0xffU) return Remember(P, CR_NOT_FOUND);
  P->SwitchKnown = FALSE;
  for (UINT8 R = 8; R <= 0x10; ++R)
    if (SwitchWrite(P, R, R == 0x10 ? 9 : 0) != CR_SUCCESS) return P->IoStatus;
  return SwitchSet(P, 0x98, 0x18);
}
STATIC CR_STATUS SwitchRoute(CR_USB_DP_PHY *P, CONST CR_USB_DP_PHY_ROUTE *R) {
  UINT8 Enable = 0x98, Select = 0x18;
  if (R->DpActive) {
    Enable = (R->PinAssignment == 3 || R->PinAssignment == 5) ? 0xe0 : 0xf8;
    Select = (R->PinAssignment == 3 || R->PinAssignment == 5) ? 0 : 0x18;
    if (R->Orientation == 2) Select |= 0x60;
  }
  return SwitchSet(P, Enable, Select);
}
STATIC BOOLEAN SameRoute(CONST CR_USB_DP_PHY_ROUTE *A, CONST CR_USB_DP_PHY_ROUTE *B) {
  return A->Orientation == B->Orientation && A->DpActive == B->DpActive &&
    A->DpLanes == B->DpLanes && A->PinAssignment == B->PinAssignment && A->Usb3 == B->Usb3;
}
STATIC CR_STATUS Common(CR_USB_DP_PHY *P, CONST CR_USB_DP_PHY_ROUTE *R) {
  if (Rails(P, 0, 3, TRUE) != CR_SUCCESS || SwitchInit(P) != CR_SUCCESS ||
      SwitchRoute(P, R) != CR_SUCCESS) return P->IoStatus;
  P->CommonTouched = TRUE;
  if (Reset(P, "gcc_usb3_dp_phy_prim_bcr", TRUE) != CR_SUCCESS ||
      Reset(P, "gcc_usb3_phy_prim_bcr", TRUE) != CR_SUCCESS) return P->IoStatus;
  cr_sleep(10);
  if (Reset(P, "gcc_usb3_dp_phy_prim_bcr", FALSE) != CR_SUCCESS ||
      Reset(P, "gcc_usb3_phy_prim_bcr", FALSE) != CR_SUCCESS) return P->IoStatus;
  for (UINT32 I = 0; I < CR_USB_DP_PHY_CLOCKS; ++I)
    if (Clock(P, I, TRUE) != CR_SUCCESS) return P->IoStatus;
  Or(P, QMP + 8, B(0)); Or(P, QMP + 0x1c, 0xf);
  Write(P, QMP + 0x10, R->Orientation == 2 ? 3 : 2);
  UINT32 Mode = R->DpActive ? (R->DpLanes == 4 ? 2 : 3) : 1;
  Write(P, QMP, Mode); Write(P, QMP + 0x1c, Mode == 2 ? 0xc : Mode == 1 ? 3 : 0);
  And(P, QMP + 0xc, ~3U); And(P, QMP + 4, ~B(0));
  Or(P, PCS + QPHY_V4_PCS_POWER_DOWN_CONTROL, B(0)); CrMemoryFence();
  if (P->IoStatus != CR_SUCCESS) return P->IoStatus;
  P->Route = *R; P->CommonReady = TRUE;
  return CR_SUCCESS;
}
STATIC CR_STATUS Usb(CR_USB_DP_PHY *P, BOOLEAN SuperSpeed) {
  STATIC CONST CR_PHY_REG HdkHsTune[] = {{0x6c,0xe6,3},{0x70,0x0c,3},{0x74,0x17,3}};
  if (Rails(P, 3, 3, TRUE) != CR_SUCCESS) return P->IoStatus;
  P->UsbTouched = TRUE;
  if (Reset(P, "gcc_qusb2phy_prim_bcr", TRUE) != CR_SUCCESS) return P->IoStatus;
  cr_sleep(100);
  if (Reset(P, "gcc_qusb2phy_prim_bcr", FALSE) != CR_SUCCESS) return P->IoStatus;
  Or(P, HS + 0x94, B(1)); Or(P, HS + 0x50, B(1)); And(P, HS + 0x54, ~0x70U);
  Or(P, HS + 0x58, B(5)|B(4)); Update(P, HS + 0xa0, 3, 2); Or(P, HS + 0x60, B(0));
  for (UINT32 I = 0; I < COUNT(HdkHsTune); ++I) {
    Update(P, HS + HdkHsTune[I].Offset, 0xff, HdkHsTune[I].Value);
    if ((Read(P, HS + HdkHsTune[I].Offset) & 0xffU) != HdkHsTune[I].Value)
      Remember(P, CR_DEVICE_ERROR);
    if (P->IoStatus != CR_SUCCESS) return P->IoStatus;
  }
  Or(P, HS + 0x5c, B(0)); Or(P, HS + 0x64, B(2)|B(3)); Or(P, HS + 0x3c, B(0));
  And(P, HS + 0x54, ~B(2)); And(P, HS + 0x50, ~B(1));
  And(P, HS + 0x64, ~B(3)); And(P, HS + 0x94, ~B(1));
  if (P->IoStatus != CR_SUCCESS) return P->IoStatus;
  P->UsbSs = SuperSpeed;
  if (!SuperSpeed) return CR_SUCCESS;
  LOAD(0x1000, sm8150_usb3_serdes_tbl, 3);
  LOAD(0x1200, sm8350_usb3_tx_tbl, 1); LOAD(0x1600, sm8350_usb3_tx_tbl, 2);
  LOAD(0x1400, sm8350_usb3_rx_tbl, 1); LOAD(0x1800, sm8350_usb3_rx_tbl, 2);
  LOAD(PCS, sm8350_usb3_pcs_tbl, 3); LOAD(0x1f00, sm8350_usb3_pcs_usb_tbl, 3);
  Or(P, PCS + QPHY_V4_PCS_POWER_DOWN_CONTROL, B(0)); cr_sleep(10);
  And(P, PCS + QPHY_V4_PCS_SW_RESET, ~B(0)); Or(P, PCS + QPHY_V4_PCS_START_CONTROL, 3);
  return Wait(P, PCS + QPHY_V4_PCS_PCS_STATUS1, B(6), 0);
}
STATIC CR_STATUS Aux(CR_USB_DP_PHY *P) {
  STATIC CONST UINT8 Config[] = {0,0x13,0xa4,0,0xa,0x26,0xa,3,0xb7,3};
  P->DpTouched = TRUE;
  Write(P, DP + 0x18, 0x67); Write(P, PLL + QSERDES_V4_COM_BIAS_EN_CLKBUFLR_EN, 0x17);
  for (UINT32 I = 0; I < COUNT(Config); ++I) Write(P, DP + 0x20 + I * 4, Config[I]);
  Write(P, DP + 0x54, 0x1f);
  return P->IoStatus;
}

CR_STATUS CrUsbDpPhyInit(CR_USB_DP_PHY *P, CONST CR_USB_DP_PHY_CONFIG *C,
    CONST CR_USB_DP_PHY_IO *Io) {
  if (CrMutexCheckLevel() != CR_SUCCESS || !P || !C || !Io || C->Revision != CR_USB_DP_PHY_REVISION ||
      C->Soc != CR_USB_DP_PHY_SOC || C->Board != CR_USB_DP_PHY_HDK ||
      !Fits(&C->View[CrUsbDpPhyQmp], CR_USB_DP_PHY_QMP_BYTES) ||
      !Fits(&C->View[CrUsbDpPhyHs], CR_USB_DP_PHY_HS_BYTES) ||
      (C->View[0].Address < C->View[1].Address + C->View[1].Bytes &&
       C->View[1].Address < C->View[0].Address + C->View[0].Bytes) ||
      !Io->Ready || !Io->Read32 || !Io->Write32 || !Io->Clock || !Io->Rail ||
      !Io->Reset || !Io->SwitchRead || !Io->SwitchWrite)
    return CR_INVALID_PARAMETER;
  if (P->Initialized) return CR_ALREADY_STARTED;
  /* Preserve caller-owned inputs even when supplied from this fresh
     zero-initialized object's metadata slots. */
  CR_USB_DP_PHY_CONFIG SavedConfig = *C;
  CR_USB_DP_PHY_IO SavedIo = *Io;
  cr_memset(P, 0, sizeof(*P)); P->Config = SavedConfig; P->Io = SavedIo;
  STATIC CONST CR_USB_DP_PHY_CLOCK Clocks[CR_USB_DP_PHY_CLOCKS] = {
    {"gcc_usb3_0_clkref_en",0,FALSE,CR_SUCCESS},
    {"gcc_usb3_prim_phy_aux_clk",19200000,FALSE,CR_SUCCESS},
    {"gcc_usb3_prim_phy_com_aux_clk",19200000,FALSE,CR_SUCCESS},
    {"gcc_usb3_prim_phy_pipe_clk",0,FALSE,CR_SUCCESS}};
  STATIC CONST CR_USB_DP_PHY_RAIL Rails[CR_USB_DP_PHY_RAILS] = {
    {"bobc1",3296,6,FALSE,CR_SUCCESS},{"ldob1",912,7,FALSE,CR_SUCCESS},
    {"ldob6",1200,7,FALSE,CR_SUCCESS},{"ldob5",880,7,FALSE,CR_SUCCESS},
    {"ldoc1",1800,7,FALSE,CR_SUCCESS},{"ldob2",3072,7,FALSE,CR_SUCCESS}};
  for (UINT32 I = 0; I < CR_USB_DP_PHY_CLOCKS; ++I) P->Clocks[I] = Clocks[I];
  for (UINT32 I = 0; I < CR_USB_DP_PHY_RAILS; ++I) P->Rails[I] = Rails[I];
  CrMutexInit(&P->Lock); P->Initialized = TRUE;
  return CR_SUCCESS;
}
CR_STATUS CrUsbDpPhyAcquire(CR_USB_DP_PHY *P, VOID *Owner, CR_USB_DP_PHY_CLIENT Client,
    CONST CR_USB_DP_PHY_ROUTE *R, BOOLEAN SuperSpeed, BOOLEAN *Owned) {
  if (Owned) *Owned = FALSE;
  if (!P || !Owner || (UINT32)Client >= CrUsbDpPhyClients || !R || !Owned ||
      SuperSpeed > 1 || R->PdReady > 1 || R->Connected > 1 || R->DpActive > 1 || R->Usb3 > 1)
    return CR_INVALID_PARAMETER;
  if (!R->PdReady || !R->Connected || R->LastError != CR_SUCCESS ||
      (R->Orientation != 1 && R->Orientation != 2)) return CR_NOT_READY;
  if (R->DpActive && ((R->DpLanes != 2 && R->DpLanes != 4) ||
      R->PinAssignment < 3 || R->PinAssignment > 6 ||
      ((R->PinAssignment == 3 || R->PinAssignment == 5) != (R->DpLanes == 4))))
    return CR_UNSUPPORTED;
  if (Client == CrUsbDpPhyDp && !R->DpActive) return CR_NOT_READY;
  if (Client == CrUsbDpPhyUsb && SuperSpeed && !R->Usb3) return CR_UNSUPPORTED;
  CR_STATUS S = Begin(P);
  if (S != CR_SUCCESS) return S;
  if (P->Owner[Client]) return End(P, CR_ALREADY_STARTED);
  if (P->Owner[1-Client] && (!P->CommonReady || !P->Ready[1-Client] || !SameRoute(R,&P->Route)))
    return End(P, CR_NOT_READY);
  P->Owner[Client] = Owner; *Owned = TRUE;
  if (!P->CommonReady) {
    S = Common(P, R); P->CommonStatus = S;
    if (S != CR_SUCCESS) goto Out;
  }
  S = Client == CrUsbDpPhyUsb ? Usb(P, SuperSpeed) : Aux(P);
  if (S == CR_SUCCESS) P->Ready[Client] = TRUE;
Out:
  P->ClientStatus[Client] = S;
  return End(P, S);
}
CR_STATUS CrUsbDpPhyPowerDp(CR_USB_DP_PHY *P, VOID *Owner, UINT32 Rate, UINT8 Lanes) {
  if (!Owner || (Lanes != 1 && Lanes != 2 && Lanes != 4) ||
      (Rate != 1620 && Rate != 2700 && Rate != 5400 && Rate != 8100)) return CR_INVALID_PARAMETER;
  CR_STATUS S = Begin(P);
  if (S != CR_SUCCESS) return S;
  if (P->Owner[CrUsbDpPhyDp] != Owner) return End(P, CR_BUSY);
  if (!P->Ready[CrUsbDpPhyDp] || Lanes > P->Route.DpLanes) return End(P, CR_NOT_READY);
  BOOLEAN Reverse = P->Route.Orientation == 2;
  UINT32 Bias0, Bias1, Driver0 = 0x10, Driver1 = 0x10, VcoDiv;
  Write(P, DP + 0x18, 2); LOAD(PLL, qmp_v4_dp_serdes_tbl, 3);
  switch (Rate) {
  case 1620: LOAD(PLL, qmp_v4_dp_serdes_tbl_rbr, 3); VcoDiv = 1; break;
  case 2700: LOAD(PLL, qmp_v4_dp_serdes_tbl_hbr, 3); VcoDiv = 1; break;
  case 5400: LOAD(PLL, qmp_v4_dp_serdes_tbl_hbr2, 3); VcoDiv = 2; break;
  default: LOAD(PLL, qmp_v4_dp_serdes_tbl_hbr3, 3); VcoDiv = 0; break;
  }
  LOAD(TX0, qmp_v5_dp_tx_tbl, 1); LOAD(TX1, qmp_v5_dp_tx_tbl, 2);
  Write(P, DP + 0x14, 0xf);
  Write(P, DP + 0x18, 0x65 | ((Lanes == 4 || Reverse) ? 8 : 0) | ((Lanes == 4 || !Reverse) ? 0x10 : 0));
  Write(P, DP + 0x1c, Reverse ? 0x4c : 0x5c);
  Write(P, DP + 0x24, 0x13); Write(P, DP + 0x28, 0xa4);
  Write(P, DP + 0x78, 5); Write(P, DP + 0x9c, 5); Write(P, DP + 0x70, VcoDiv);
  Write(P, DP + 0x10, 1); Write(P, DP + 0x10, 5); Write(P, DP + 0x10, 1); Write(P, DP + 0x10, 9);
  Write(P, PLL + QSERDES_V4_COM_RESETSM_CNTRL, 0x20);
  S = Wait(P, PLL + QSERDES_V4_COM_C_READY_STATUS, B(0), B(0));
  if (S != CR_SUCCESS) goto Out;
  S = Wait(P, PLL + QSERDES_V4_COM_CMN_STATUS, 3, 3);
  if (S != CR_SUCCESS) goto Out;
  Write(P, DP + 0x10, 0x19); S = Wait(P, DP + 0xdc, 3, 3);
  if (S != CR_SUCCESS) goto Out;
  if (Lanes == 4) Bias0 = Bias1 = 0x3f;
  else {
    Bias0 = Reverse ? (Lanes == 1 ? 0x3e : 0x3f) : 0x15;
    Bias1 = Reverse ? 0x15 : (Lanes == 1 ? 0x3e : 0x3f);
    if (Lanes == 1) { Driver0 = Reverse ? 0x13 : 0x10; Driver1 = Reverse ? 0x10 : 0x13; }
  }
  Write(P, TX0 + QSERDES_V4_TX_HIGHZ_DRVR_EN, Driver0); Write(P, TX1 + QSERDES_V4_TX_HIGHZ_DRVR_EN, Driver1);
  Write(P, TX0 + QSERDES_V4_TX_TRANSCEIVER_BIAS_EN, Bias0); Write(P, TX1 + QSERDES_V4_TX_TRANSCEIVER_BIAS_EN, Bias1);
  Write(P, DP + 0x10, 0x18); cr_sleep(2000); Write(P, DP + 0x10, 0x19);
  S = Wait(P, DP + 0xdc, B(1), B(1));
  if (S != CR_SUCCESS) goto Out;
  Write(P, TX0 + QSERDES_V4_TX_TX_POL_INV, 0xa); Write(P, TX1 + QSERDES_V4_TX_TX_POL_INV, 0xa);
  S = P->IoStatus;
  if (S == CR_SUCCESS) { P->DpRate = Rate; P->DpLanes = Lanes; }
Out:
  if (S != CR_SUCCESS) { P->DpRate = 0; P->DpLanes = 0; }
  P->ClientStatus[CrUsbDpPhyDp] = S;
  return End(P, S);
}
CR_STATUS CrUsbDpPhySetDpLevels(CR_USB_DP_PHY *P, VOID *Owner, UINT8 V, UINT8 Pre) {
  STATIC CONST UINT8 LowV[4][4] = {{8,15,22,31},{17,30,31,255},{22,31,255,255},{31,255,255,255}};
  STATIC CONST UINT8 HighV[4][4] = {{2,18,22,26},{9,25,31,255},{16,31,255,255},{31,255,255,255}};
  STATIC CONST UINT8 LowP[4][4] = {{0,13,20,26},{0,14,21,255},{0,14,255,255},{3,255,255,255}};
  STATIC CONST UINT8 HighP[4][4] = {{0,12,21,27},{2,14,22,255},{2,17,255,255},{4,255,255,255}};
  if (!Owner || V > 3 || Pre > 3 || V + Pre > 3) return CR_INVALID_PARAMETER;
  CR_STATUS S = Begin(P);
  if (S != CR_SUCCESS) return S;
  if (P->Owner[CrUsbDpPhyDp] != Owner) return End(P, CR_BUSY);
  if (!P->Ready[CrUsbDpPhyDp] || !P->DpRate) return End(P, CR_NOT_READY);
  UINT8 Voltage = (P->DpRate <= 2700 ? LowV[V][Pre] : HighV[V][Pre]) | 0x20;
  UINT8 Emphasis = (P->DpRate <= 2700 ? LowP[V][Pre] : HighP[V][Pre]) | 0x20;
  Write(P, TX0 + QSERDES_V4_TX_TX_DRV_LVL, Voltage); Write(P, TX1 + QSERDES_V4_TX_TX_DRV_LVL, Voltage);
  Write(P, TX0 + QSERDES_V4_TX_TX_EMP_POST1_LVL, Emphasis); Write(P, TX1 + QSERDES_V4_TX_TX_EMP_POST1_LVL, Emphasis);
  P->ClientStatus[CrUsbDpPhyDp] = P->IoStatus;
  return End(P, P->IoStatus);
}
CR_STATUS CrUsbDpPhyClearDpAuxInterrupts(CR_USB_DP_PHY *P, VOID *Owner) {
  if (!Owner) return CR_INVALID_PARAMETER;
  CR_STATUS S = Begin(P);
  if (S != CR_SUCCESS) return S;
  if (P->Owner[CrUsbDpPhyDp] != Owner || !P->Ready[CrUsbDpPhyDp] ||
      !P->CommonReady || !P->DpTouched) return End(P, CR_NOT_READY);
  /* Android dp_catalog_aux_clear_hw_int_v420 uses the actual four-byte PHY
     revision to select d8/e0. SM8450 shares the QMP v4/v5 layout with 8350;
     reject absent/malformed/unknown major IDs before touching the latch. */
  UINT32 Version = 0;
  for (UINT32 I = 0; I < 4; ++I) {
    UINT32 Id = Read(P, DP + I * 4);
    if (P->IoStatus != CR_SUCCESS) return End(P, P->IoStatus);
    if (Id > 0xff) return End(P, CR_UNSUPPORTED);
    Version |= Id << (I * 8);
  }
  if (Version < 0x40000000U || Version >= 0x70000000U) return End(P, CR_UNSUPPORTED);
  UINT32 Status = DP + (Version >= 0x60000000U ? 0xe0 : 0xd8);
  (VOID)Read(P, Status);
  if (P->IoStatus != CR_SUCCESS) return End(P, P->IoStatus);
  STATIC CONST UINT32 Values[] = {0x1f, 0x9f, 0};
  for (UINT32 I = 0; I < COUNT(Values); ++I) {
    Write(P, DP + 0x58, Values[I]); CrMemoryFence();
    if (P->IoStatus != CR_SUCCESS) return End(P, P->IoStatus);
  }
  if (Read(P, Status) & 0x1fU) return End(P, P->IoStatus != CR_SUCCESS ? P->IoStatus : CR_NOT_READY);
  return End(P, P->IoStatus);
}

CR_STATUS CrUsbDpPhyRelease(CR_USB_DP_PHY *P, VOID *Owner, CR_USB_DP_PHY_CLIENT Client) {
  if (!Owner || (UINT32)Client >= CrUsbDpPhyClients) return CR_INVALID_PARAMETER;
  CR_STATUS S = Begin(P);
  if (S != CR_SUCCESS) return S;
  if (!P->Owner[Client]) return End(P, CR_SUCCESS);
  if (P->Owner[Client] != Owner) return End(P, CR_BUSY);
  P->Ready[Client] = FALSE;
  if (Client == CrUsbDpPhyDp && P->DpTouched) {
    Write(P, DP + 0x18, 2); CrMemoryFence();
    if (P->IoStatus != CR_SUCCESS) { S = P->IoStatus; goto Out; }
    P->DpRate = 0; P->DpLanes = 0; P->DpTouched = FALSE;
  }
  if (Client == CrUsbDpPhyUsb) {
    if (P->UsbTouched) {
      S = Reset(P, "gcc_qusb2phy_prim_bcr", TRUE);
      if (S != CR_SUCCESS) goto Out;
      if (P->UsbSs) {
        Or(P, PCS + QPHY_V4_PCS_SW_RESET, B(0));
        And(P, PCS + QPHY_V4_PCS_START_CONTROL, ~3U);
        And(P, PCS + QPHY_V4_PCS_POWER_DOWN_CONTROL, ~B(0));
        if (P->IoStatus != CR_SUCCESS) { S = P->IoStatus; goto Out; }
      }
      P->UsbTouched = P->UsbSs = FALSE;
    }
    S = Rails(P, 3, 3, FALSE); if (S != CR_SUCCESS) goto Out;
  }
  if (!P->Owner[1-Client]) {
    if (P->CommonTouched) {
      P->CommonReady = FALSE;
      S = Reset(P, "gcc_usb3_dp_phy_prim_bcr", TRUE); if (S != CR_SUCCESS) goto Out;
      S = Reset(P, "gcc_usb3_phy_prim_bcr", TRUE); if (S != CR_SUCCESS) goto Out;
      P->CommonTouched = FALSE;
    }
    if (P->SwitchTouched) {
      S = SwitchSet(P, 0x98, 0x18); if (S != CR_SUCCESS) goto Out;
      P->SwitchTouched = FALSE;
    }
    for (UINT32 I = CR_USB_DP_PHY_CLOCKS; I-- > 0;) {
      S = Clock(P, I, FALSE); if (S != CR_SUCCESS) goto Out;
    }
    S = Rails(P, 0, 3, FALSE); if (S != CR_SUCCESS) goto Out;
    P->CommonReady = FALSE; P->SwitchKnown = FALSE; P->CommonStatus = CR_SUCCESS;
  }
  P->Owner[Client] = NULL;
Out:
  P->ClientStatus[Client] = S;
  return End(P, S);
}
CR_STATUS CrUsbDpPhyFreeze(CR_USB_DP_PHY *P) {
  if (!P || !P->Initialized) return CR_INVALID_PARAMETER;
  CrAtomicOr32(&P->Frozen, 1); CrMemoryFence();
  return CrAtomicLoad32(&P->Active) ? CR_BUSY : CR_SUCCESS;
}
CR_STATUS CrUsbDpPhyUninit(CR_USB_DP_PHY *P) {
  if (!P || !P->Initialized) return CR_INVALID_PARAMETER;
  CR_STATUS S = Begin(P); if (S != CR_SUCCESS) return S;
  BOOLEAN Owned = P->Owner[0] || P->Owner[1] || P->CommonTouched || P->UsbTouched ||
    P->DpTouched || P->SwitchTouched;
  for (UINT32 I = 0; I < CR_USB_DP_PHY_CLOCKS; ++I) Owned |= P->Clocks[I].Owned;
  for (UINT32 I = 0; I < CR_USB_DP_PHY_RAILS; ++I) Owned |= P->Rails[I].Owned;
  if (Owned) return End(P, CR_BUSY);
  P->Initialized = FALSE;
  return End(P, CR_SUCCESS);
}
