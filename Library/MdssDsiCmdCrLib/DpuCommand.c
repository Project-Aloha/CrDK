/* SM8250 DPU6 command DMA0/LM0/CTL0/PP0 -> DSC0 -> INTF1.
 * Derived from Qualcomm cad09e061ef6 sde_hw_{ctl,intf,vbif}.c,
 * sde_encoder_phys_cmd.c and Linux dpu_6_0_sm8250.h/dpu_hw_sspp.c.
 * Copyright (c) 2015-2020 The Linux Foundation.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "Internal.h"

#define CTL 0x1000U
#define DMA 0x24000U
#define LM  0x44000U
#define INTF 0x6a800U
#define TE_IRQ 0x6d800U
#define FORCE 0x2acU
#define FORCE_DMA0 CMDBIT(8)
#define XIN_DMA0 CMDBIT(1)
#define DONE CMDBIT(8)
#define UNDERRUN CMDBIT(26)
#define TOP_OWNED (DONE | UNDERRUN)
#define TE_OWNED CMDBIT(2)
#define FLUSH (CMDBIT(11) | CMDBIT(6) | CMDBIT(17) | CMDBIT(22) | CMDBIT(31))
#define TRY(Expression) do { S = (Expression); if (S != CR_SUCCESS) return S; } while (0)

static CR_STATUS Selected(CR_MDSS_CMD *D)
{
  CR_MDSS_CMD_PLAN P;
  if (!D || !D->Initialized) return CR_INVALID_PARAMETER;
  CR_STATUS S = CrMdssCmdBuildPlanProfile(&D->Profile, &P);
  if (S != CR_SUCCESS) return S;
  if (cr_memcmp(&P, &D->Plan, sizeof(P))) return CR_INVALID_PARAMETER;
  UINT64 Bytes = (UINT64)P.Config.Stride * P.Config.Height;
  if (!D->Frame.Cpu || !D->Frame.DeviceAddress || (D->Frame.DeviceAddress & 3) ||
      Bytes > D->Frame.Bytes || D->Frame.DeviceAddress > CR_MAX_UINT32 ||
      Bytes - 1U > CR_MAX_UINT32 - D->Frame.DeviceAddress)
    return CR_BAD_BUFFER_SIZE;
  return CmdDmaReady(D, CrMdssCmdDpu);
}

static CR_STATUS UpdateChecked(CR_MDSS_CMD *D, CR_MDSS_CMD_WINDOW W,
                               UINT32 O, UINT32 Mask, UINT32 Value)
{
  UINT32 V;
  CR_STATUS S;
  TRY(CmdRead(D, W, O, &V));
  return CmdChecked(D, W, O, (V & ~Mask) | (Value & Mask), Mask);
}

static CR_STATUS ForceClock(CR_MDSS_CMD *D)
{
  UINT32 V;
  CR_STATUS S;
  TRY(CmdRead(D, CrMdssCmdDpu, FORCE, &V));
  if (!D->DpuForceTouched) {
    D->DpuOriginalForce = V;
    D->DpuForceTouched = TRUE;
  }
  return CmdChecked(D, CrMdssCmdDpu, FORCE, V | FORCE_DMA0, FORCE_DMA0);
}

static CR_STATUS RestoreClock(CR_MDSS_CMD *D)
{
  if (!D->DpuForceTouched) return CR_SUCCESS;
  CR_STATUS S = UpdateChecked(D, CrMdssCmdDpu, FORCE, FORCE_DMA0, D->DpuOriginalForce);
  if (S == CR_SUCCESS) D->DpuForceTouched = FALSE;
  return S;
}

static CR_STATUS Unhalt(CR_MDSS_CMD *D)
{
  CR_STATUS S;
  D->DpuHaltTouched = TRUE;
  TRY(UpdateChecked(D, CrMdssCmdVbif, 0x200, XIN_DMA0, 0));
  /* SM8250 target Android ACK is the LOW XIN field, unlike SM8450. Both
   * edges are witnessed while DMA0's forced clock is still asserted. */
  return CmdPoll(D, CrMdssCmdVbif, 0x204, XIN_DMA0, 0, 2000);
}

static CR_STATUS FreshHalt(CR_MDSS_CMD *D)
{
  CR_STATUS S;
  TRY(Unhalt(D));
  TRY(UpdateChecked(D, CrMdssCmdVbif, 0x200, XIN_DMA0, XIN_DMA0));
  return CmdPoll(D, CrMdssCmdVbif, 0x204, XIN_DMA0, XIN_DMA0, 2000);
}

static CR_STATUS Ack(CR_MDSS_CMD *D, UINT32 Clear, UINT32 Status, UINT32 Bits)
{
  if (!Bits) return CR_SUCCESS;
  UINT32 V;
  CR_STATUS S;
  TRY(CmdWrite(D, CrMdssCmdDpu, Clear, Bits));
  TRY(CmdFence(D, CrMdssCmdDpu));
  TRY(CmdRead(D, CrMdssCmdDpu, Status, &V));
  return (V & Bits) ? CR_DEVICE_ERROR : CR_SUCCESS;
}

static CR_STATUS IrqMask(CR_MDSS_CMD *D, BOOLEAN On)
{
  CR_STATUS S;
  TRY(UpdateChecked(D, CrMdssCmdDpu, 0x10, TOP_OWNED, On ? TOP_OWNED : 0));
  return UpdateChecked(D, CrMdssCmdDpu, TE_IRQ, TE_OWNED, On ? TE_OWNED : 0);
}

static CR_STATUS ClearOwned(CR_MDSS_CMD *D)
{
  CR_STATUS S;
  TRY(Ack(D, 0x18, 0x14, TOP_OWNED));
  return Ack(D, TE_IRQ + 8, TE_IRQ + 4, TE_OWNED);
}

static CR_STATUS Qos(CR_MDSS_CMD *D)
{
  static const UINT32 Levels[8] = {3,3,4,4,5,5,6,6};
  CR_STATUS S;
  /* The supplied target's eight RT levels. XIN1-only updates preserve other
   * engines. Do not clear global VBIF shareability registers as Android's
   * whole-device initialization does. */
  for (UINT32 I = 0; I < 8; ++I) {
    TRY(UpdateChecked(D, CrMdssCmdVbif, 0x550 + I * 8, 7U << 4, Levels[I] << 4));
    TRY(UpdateChecked(D, CrMdssCmdVbif, 0x590 + I * 8, 7U << 4, Levels[I] << 4));
  }
  TRY(UpdateChecked(D, CrMdssCmdVbif, 0x160, 7U << 4, 3U << 4));
  TRY(UpdateChecked(D, CrMdssCmdVbif, 0x170, XIN_DMA0, 0));
  TRY(UpdateChecked(D, CrMdssCmdVbif, 0x174, XIN_DMA0, 0));
  TRY(CmdChecked(D, CrMdssCmdDpu, DMA + 0x60, 0xffff, 0xffffffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, DMA + 0x64, 0xfff0, 0xffffffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, DMA + 0x74, 0x22335777, 0xffffffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, DMA + 0x78, 0x00112222, 0xffffffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, DMA + 0x6c, 1, 0x00330031U));
  return CmdChecked(D, CrMdssCmdDpu, DMA + 0x134, 9, 0xfU);
}

static CR_STATUS Program(CR_MDSS_CMD *D)
{
  const CR_MDSS_CMD_CONFIG *C = &D->Plan.Config;
  UINT32 Size = (C->Height << 16) | C->Width;
  UINT32 Div = (UINT32)(C->VsyncHz / ((UINT64)D->Plan.Vtotal * C->RefreshHz));
  CR_STATUS S;
  static const UINT32 Zeros[] = {
    DMA + 0x38, DMA + 0x08, DMA + 0x10,
    DMA + 0x18, DMA + 0x1c, DMA + 0x20, DMA + 0x28, DMA + 0xb4,
    DMA + 0x100, DMA + 0x104, DMA + 0x110, DMA + 0x114,
    DMA + 0x120, DMA + 0x124, DMA + 0x170,
    CTL + 0x40, CTL + 0x70, CTL + 0xe4, CTL + 0xec,
    CTL + 0xf0, CTL + 0xf8, CTL + 0x134,
    INTF, INTF + 0x280, INTF + 0x284, INTF + 0x2b4
  };
  for (UINT32 I = 0; I < sizeof(Zeros) / sizeof(Zeros[0]); ++I)
    TRY(CmdChecked(D, CrMdssCmdDpu, Zeros[I], 0, 0xffffffffU));
  TRY(Qos(D));
  TRY(CmdChecked(D, CrMdssCmdDpu, DMA, Size, 0xffffffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, DMA + 0x0c, Size, 0xffffffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, DMA + 0x14, (UINT32)D->Frame.DeviceAddress, 0xffffffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, DMA + 0x24, C->Stride, 0xffffffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, DMA + 0x30, 0x236ff, 0xffffffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, DMA + 0x34, 0x03020001, 0xffffffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, DMA + 0x108, Size, 0xffffffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, DMA + 0x118, Size, 0xffffffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, DMA + 0x128, Size, 0xffffffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, DMA + 0x38, CMDBIT(31), 0xffffffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, LM, 0, 0xffffffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, LM + 4, Size, 0xffffffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, CTL, CMDBIT(18), 0xffffffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, CTL + 0x14, CMDBIT(17), 0xffffffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, CTL + 0xe8, 1, 0xffffffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, CTL + 0xf4, 2, 0xffffffffU));
  TRY(UpdateChecked(D, CrMdssCmdDpu, INTF + 0x25c, 0xf, 0));
  TRY(CmdChecked(D, CrMdssCmdDpu, INTF + 0x288, CMDBIT(19) | Div, 0x001fffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, INTF + 0x28c, 0xfff0, 0xffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, INTF + 0x290, C->Height + 5, 0xffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, INTF + 0x294, C->Height, 0xffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, INTF + 0x29c, 0x00040004, 0xffffffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, INTF + 0x2a0, C->Height, 0xffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, INTF + 0x2a4, C->Height + 1, 0xffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, INTF + 0x2a8, 1, 0xffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, INTF + 0x284, 1, 1));
  return CR_SUCCESS;
}

static CR_STATUS Start(CR_MDSS_CMD *D)
{
  CR_STATUS S;
  TRY(IrqMask(D, FALSE));
  TRY(ClearOwned(D));
  TRY(ForceClock(D));
  TRY(FreshHalt(D));
  TRY(Program(D));
  TRY(Unhalt(D));
  TRY(RestoreClock(D));
  TRY(IrqMask(D, TRUE));
  TRY(CmdChecked(D, CrMdssCmdDpu, CTL + 0x104, 1, 1));
  TRY(CmdChecked(D, CrMdssCmdDpu, CTL + 0x110, 2, 2));
  /* Autorefresh triggers the transfer; CTL_START is intentionally untouched.
   * The combined flush is queued while autorefresh is still disabled. */
  TRY(CmdChecked(D, CrMdssCmdDpu, CTL + 0x18, FLUSH, FLUSH));
  D->FrameInFlight = TRUE;
  D->AutoRefresh = TRUE;
  TRY(CmdChecked(D, CrMdssCmdDpu, INTF + 0x2b4, CMDBIT(31) | 1, CMDBIT(31) | 0xffffU));
  D->Starting = TRUE;
  return CR_SUCCESS;
}

CR_STATUS CmdDpuStart(CR_MDSS_CMD *D)
{
  CR_STATUS S = Selected(D);
  if (S != CR_SUCCESS) return S;
  if (!D->IrqOwned || !D->ExternalOwned || !D->PanelOwned || !D->HostTouched ||
      !D->PhyTouched || !D->DscTouched || D->CleanupPending || D->Faulted)
    return CR_NOT_READY;
  if (D->DpuTouched)
    return D->Starting && D->FirstFrameDone ? CR_SUCCESS : CR_NOT_READY;
  S = D->Frame.Publish(D->Frame.Context, D->Frame.Cpu,
      (UINTN)D->Plan.Config.Stride * D->Plan.Config.Height);
  if (S != CR_SUCCESS) return CR_ERROR(S) ? S : CR_NOT_READY;
  D->FirstFrameDone = FALSE;
  D->DpuTouched = TRUE;
  S = Start(D);
  if (S != CR_SUCCESS) {
    D->CleanupPending = TRUE;
    return S;
  }
  return CR_NOT_READY;
}

static CR_STATUS FrameIdle(CR_MDSS_CMD *D)
{
  UINT32 Limit = D->Plan.FrameUs * 2U;
  for (UINT32 I = 0; I < Limit; I += 2000U) {
    UINT32 V;
    /* Android seq1 delays before each observation. Keep tearcheck enabled
     * until this count settles; disabling it can hide an ongoing transfer. */
    CR_STATUS S = CmdDelay(D, CrMdssCmdDpu, 2000);
    if (S != CR_SUCCESS) return S;
    S = CmdRead(D, CrMdssCmdDpu, INTF + 0x2b0, &V);
    if (S != CR_SUCCESS) return S;
    V &= 0xffffU;
    if (!V || V >= D->Plan.Config.Height) return CR_SUCCESS;
  }
  return CR_TIMEOUT;
}

static CR_STATUS AutoStatus(CR_MDSS_CMD *D, UINT32 *Status)
{
  CR_STATUS S;
  /* Exact Android INTF1 debugbus selector: block64, test point2. */
  TRY(CmdChecked(D, CrMdssCmdDpu, 0x418, 0x405, 0xffffffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, 0x348, 0x7001, 0xffffffffU));
  TRY(CmdRead(D, CrMdssCmdDpu, 0x34c, Status));
  return CmdChecked(D, CrMdssCmdDpu, 0x418, 0, 0xffffffffU);
}

static CR_STATUS AutoRefreshIdle(CR_MDSS_CMD *D)
{
  CR_STATUS S;
  if (!D->DpuDebugTouched) {
    UINT32 Periph, Dspp;
    TRY(CmdRead(D, CrMdssCmdDpu, 0x418, &Periph));
    TRY(CmdRead(D, CrMdssCmdDpu, 0x348, &Dspp));
    D->DpuOriginalPeriphDebug = Periph;
    D->DpuOriginalDsppDebug = Dspp;
    D->DpuDebugTouched = TRUE;
  }
  UINT32 Status;
  TRY(AutoStatus(D, &Status));
  for (UINT32 Trial = 0; Status & CMDBIT(7); ++Trial) {
    if (Trial == 40) return CR_TIMEOUT;
    if (!Trial)
      TRY(UpdateChecked(D, CrMdssCmdDpu, INTF + 0x2b4, CMDBIT(31), 0));
    TRY(CmdDelay(D, CrMdssCmdDpu, 25000));
    TRY(AutoStatus(D, &Status));
    /* Android check_and_reset_tearcheck: sampled status may already be idle,
     * but this iteration still repairs the write count from START_POS. */
    UINT32 ReadCount, Start, Count;
    TRY(CmdRead(D, CrMdssCmdDpu, INTF + 0x298, &ReadCount));
    TRY(CmdRead(D, CrMdssCmdDpu, INTF + 0x2a0, &Start));
    TRY(CmdRead(D, CrMdssCmdDpu, INTF + 0x290, &Count));
    if (Start & 0xffff0000U) return CR_UNSUPPORTED;
    TRY(CmdChecked(D, CrMdssCmdDpu, INTF + 0x290,
        (Count & 0xffff0000U) | Start, 0xffffffffU));
  }
  TRY(CmdChecked(D, CrMdssCmdDpu, 0x418, D->DpuOriginalPeriphDebug, 0xffffffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, 0x348, D->DpuOriginalDsppDebug, 0xffffffffU));
  D->DpuDebugTouched = FALSE;
  return CR_SUCCESS;
}

static CR_STATUS Stop(CR_MDSS_CMD *D)
{
  CR_STATUS S;
  TRY(IrqMask(D, FALSE));
  TRY(UpdateChecked(D, CrMdssCmdDpu, INTF + 0x288, CMDBIT(20), 0));
  TRY(UpdateChecked(D, CrMdssCmdDpu, INTF + 0x2b4, CMDBIT(31), 0));
  TRY(FrameIdle(D));
  TRY(AutoRefreshIdle(D));
  TRY(CmdChecked(D, CrMdssCmdDpu, INTF + 0x284, 0, 1));
  TRY(CmdChecked(D, CrMdssCmdDpu, CTL, 0, 0xffffffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, CTL + 0x40, 0, 0xffffffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, CTL + 0x70, 0, 0xffffffffU));
  TRY(CmdChecked(D, CrMdssCmdDpu, CTL + 0xe8, 0, 1));
  TRY(CmdChecked(D, CrMdssCmdDpu, CTL + 0xf4, 0, 2));
  /* SSPP+0x38 bit31 is pixel-extension override, not a DMA enable. The
   * actual source fence is retired CTL routing plus the fresh VBIF halt. */
  TRY(CmdChecked(D, CrMdssCmdDpu, INTF, 0, 1));
  /* Reset is a command engine fence, not a write/readback completion bit. */
  TRY(CmdWrite(D, CrMdssCmdDpu, CTL + 0x30, 1));
  TRY(CmdFence(D, CrMdssCmdDpu));
  TRY(CmdPoll(D, CrMdssCmdDpu, CTL + 0x30, 1, 0, 2000));
  TRY(CmdPoll(D, CrMdssCmdDpu, CTL + 0x18, FLUSH, 0, 2000));
  TRY(ForceClock(D));
  TRY(FreshHalt(D));
  TRY(ClearOwned(D));
  TRY(RestoreClock(D));
  return CR_SUCCESS;
}

CR_STATUS CmdDpuStop(CR_MDSS_CMD *D)
{
  if (!D || !D->Initialized) return CR_INVALID_PARAMETER;
  if (!D->DpuTouched && !D->FrameInFlight && !D->AutoRefresh &&
      !D->DpuForceTouched && !D->DpuHaltTouched && !D->DpuDebugTouched) return CR_SUCCESS;
  if (D->IrqOwned && !D->IrqFrozen) return CR_NOT_READY; /* actual ISR/DPC drain first */
  CR_STATUS S = CmdDmaReady(D, CrMdssCmdDpu);
  if (S == CR_SUCCESS) S = Stop(D);
  if (S != CR_SUCCESS) {
    D->CleanupPending = TRUE;
    return S;
  }
  D->DpuTouched = D->FrameInFlight = D->AutoRefresh = FALSE;
  D->DpuHaltTouched = FALSE; /* physical request remains asserted */
  D->Starting = D->FirstFrameDone = D->Active = FALSE;
  return CR_SUCCESS;
}

CR_STATUS CmdDpuIrq(CR_MDSS_CMD *D, UINT32 *Events)
{
  if (!D || !D->Initialized || !Events) return CR_INVALID_PARAMETER;
  UINTN E = (UINTN)Events, Base = (UINTN)D;
  if (E > (UINTN)-1 - sizeof(*Events) || Base > (UINTN)-1 - sizeof(*D) ||
      (E < Base + sizeof(*D) && Base < E + sizeof(*Events)))
    return CR_INVALID_PARAMETER;
  if (!D->IrqOwned || !D->DpuTouched || !D->FrameInFlight || !D->AutoRefresh ||
      D->CleanupPending || D->Faulted) return CR_NOT_READY;
  UINT32 Top, TopEnable, Te, TeEnable, Pending, TePending, Out = 0;
  CR_STATUS S = CmdDmaReady(D, CrMdssCmdDpu);
  if (S == CR_SUCCESS) S = CmdRead(D, CrMdssCmdDpu, 0x14, &Top);
  if (S == CR_SUCCESS) S = CmdRead(D, CrMdssCmdDpu, 0x10, &TopEnable);
  if (S == CR_SUCCESS) S = CmdRead(D, CrMdssCmdDpu, TE_IRQ + 4, &Te);
  if (S == CR_SUCCESS) S = CmdRead(D, CrMdssCmdDpu, TE_IRQ, &TeEnable);
  if (S != CR_SUCCESS) goto Fault;
  Pending = Top & TopEnable & TOP_OWNED;
  TePending = Te & TeEnable & TE_OWNED;
  if ((Pending & DONE) && D->FrameSerial == CR_MAX_UINT64) { S = CR_BAD_BUFFER_SIZE; goto Fault; }
  if (TePending && D->TeSerial == CR_MAX_UINT64) { S = CR_BAD_BUFFER_SIZE; goto Fault; }
  S = Ack(D, 0x18, 0x14, Pending);
  if (S == CR_SUCCESS) S = Ack(D, TE_IRQ + 8, TE_IRQ + 4, TePending);
  if (S != CR_SUCCESS) goto Fault;
  if (Pending & UNDERRUN) {
    D->Faulted = D->CleanupPending = TRUE;
    D->Active = FALSE;
    *Events = CR_MDSS_CMD_UNDERRUN;
    return CR_DEVICE_ERROR;
  }
  if (Pending & DONE) {
    UINT32 Flush;
    S = CmdRead(D, CrMdssCmdDpu, CTL + 0x18, &Flush);
    if (S != CR_SUCCESS) goto Fault;
    if (Flush & FLUSH) { S = CR_NOT_READY; goto Fault; }
    if (!D->FirstFrameDone) {
      /* Android defers external TE until after the first kickoff. This
       * first actual done interrupt proves the software-triggered transfer. */
      S = UpdateChecked(D, CrMdssCmdDpu, INTF + 0x288, CMDBIT(20), CMDBIT(20));
      if (S != CR_SUCCESS) goto Fault;
      D->FirstFrameDone = TRUE;
    }
    ++D->FrameSerial;
    Out |= CR_MDSS_CMD_FRAME_DONE;
  }
  if (TePending) { ++D->TeSerial; Out |= CR_MDSS_CMD_TE; }
  *Events = Out;
  return CR_SUCCESS;
Fault:
  D->Faulted = D->CleanupPending = TRUE;
  D->Active = FALSE;
  return S;
}
