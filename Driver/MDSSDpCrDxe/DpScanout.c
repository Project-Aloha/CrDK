/** Independent DMA1/CTL1/LM2/PP2/INTF0 scanout from Linux DPU 8.1.
    SPDX-License-Identifier: GPL-2.0-only */
#include "DpLink.h"
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#define DPU  0x0AE01000U
#define DMA1 (DPU + 0x26000)
#define CTL1 (DPU + 0x16000)
#define LM2  (DPU + 0x46000)
#define INTF0 (DPU + 0x34000)
#define VBIF 0x0AEB0000U /* actual sm8450.dtsi mdp "vbif" resource */
#define INTF0_UNDERRUN BIT24

/* These are the DPU 8.1 catalogue's INTF0 status registers and the DP
   controller's ordinary stream registers. Read while their owners are live,
   before cleanup; do not infer valid pixel fetch from a frame counter alone. */
STATIC VOID Snapshot(EFI_STATUS Status)
{
  DEBUG((EFI_ERROR(Status) ? DEBUG_ERROR : DEBUG_INIT,
    "MDSSDpCr: scanout %r INTF0 status=%08x frame=%u line=%u irq=%08x config=%08x fetch=%08x mux=%08x\n",
    Status, MmioRead32(INTF0 + 0x26C), MmioRead32(INTF0 + 0xAC),
    MmioRead32(INTF0 + 0xB0), MmioRead32(DPU + 0x14),
    MmioRead32(INTF0 + 4), MmioRead32(INTF0 + 0x170), MmioRead32(INTF0 + 0x25C)));
  DEBUG((EFI_ERROR(Status) ? DEBUG_ERROR : DEBUG_INIT,
    "MDSSDpCr: DMA1 address=%08x stride=%08x format=%08x op=%08x security=%08x CTL1 flush=%08x route=%08x fetch=%08x dp-select=%08x\n",
    MmioRead32(DMA1 + 0x14), MmioRead32(DMA1 + 0x24), MmioRead32(DMA1 + 0x30),
    MmioRead32(DMA1 + 0x38), MmioRead32(DMA1 + 0x70), MmioRead32(CTL1 + 0x18),
    MmioRead32(CTL1 + 8), MmioRead32(CTL1 + 0xFC), MmioRead32(DPU + 0x460)));
  DEBUG((EFI_ERROR(Status) ? DEBUG_ERROR : DEBUG_INIT,
    "MDSSDpCr: stream mainlink=%08x state=%08x config=%08x ready=%08x irq2=%08x levels=%08x MVID=%u NVID=%u total=%08x active=%08x misc=%08x\n",
    MmioRead32(CR_DP_LINK), MmioRead32(CR_DP_LINK + 4), MmioRead32(CR_DP_LINK + 8),
    MmioRead32(CR_DP_LINK + 0x40), MmioRead32(CR_DP_AHB + 0x24), MmioRead32(CR_DP_LINK + 0x44),
    MmioRead32(CR_DP_LINK + 0x10), MmioRead32(CR_DP_LINK + 0x18),
    MmioRead32(CR_DP_LINK + 0x1C), MmioRead32(CR_DP_LINK + 0x28), MmioRead32(CR_DP_LINK + 0x2C)));
  DEBUG((EFI_ERROR(Status) ? DEBUG_ERROR : DEBUG_INIT,
    "MDSSDpCr: INTF0 timing hsync=%08x vsync=%08x display-v=%08x/%08x active-v=%08x/%08x hctl=%08x/%08x config2=%08x data-hctl=%08x P0-intf=%08x\n",
    MmioRead32(INTF0 + 8), MmioRead32(INTF0 + 0xC),
    MmioRead32(INTF0 + 0x1C), MmioRead32(INTF0 + 0x24),
    MmioRead32(INTF0 + 0x2C), MmioRead32(INTF0 + 0x34),
    MmioRead32(INTF0 + 0x3C), MmioRead32(INTF0 + 0x40),
    MmioRead32(INTF0 + 0x60), MmioRead32(INTF0 + 0x64), MmioRead32(CR_DP_P0 + 0x14)));
  DEBUG((EFI_ERROR(Status) ? DEBUG_ERROR : DEBUG_INIT,
    "MDSSDpCr: LM2 op=%08x size=%08x CTL1 intf=%08x merge=%08x dsc=%08x wb=%08x cwb=%08x master=%08x\n",
    MmioRead32(LM2), MmioRead32(LM2 + 4), MmioRead32(CTL1 + 0xF4),
    MmioRead32(CTL1 + 0xE4), MmioRead32(CTL1 + 0xE8), MmioRead32(CTL1 + 0xEC),
    MmioRead32(CTL1 + 0xF0), MmioRead32(CTL1 + 0x134)));
  /* DISPCC SM8450's DPTX0 source/branch registers, read only while the
     frontend holds these clocks. Clock labels/rate requests alone cannot
     establish that the pixel mux and M/N latched the intended ratio. */
  DEBUG((EFI_ERROR(Status) ? DEBUG_ERROR : DEBUG_INIT,
    "MDSSDpCr: DISPCC DP0 link-cmd/cfg=%08x/%08x divider=%08x pixel-cmd/cfg=%08x/%08x M/N/D=%08x/%08x/%08x branches=%08x/%08x/%08x\n",
    MmioRead32(0x0AF0819C), MmioRead32(0x0AF081A0), MmioRead32(0x0AF081B4),
    MmioRead32(0x0AF081BC), MmioRead32(0x0AF081C0), MmioRead32(0x0AF081C4),
    MmioRead32(0x0AF081C8), MmioRead32(0x0AF081CC), MmioRead32(0x0AF08040),
    MmioRead32(0x0AF08048), MmioRead32(0x0AF08050)));
  DEBUG((EFI_ERROR(Status) ? DEBUG_ERROR : DEBUG_INIT,
    "MDSSDpCr: pixel-source P0 BIST=%08x timing=%08x TPG=%08x/%08x DSC-DTO=%08x INTF0 test=%08x TPG=%08x DMA1 danger/safe=%u/%u clock=%08x\n",
    MmioRead32(CR_DP_P0), MmioRead32(CR_DP_P0 + 0x10),
    MmioRead32(CR_DP_P0 + 0x60), MmioRead32(CR_DP_P0 + 0x64), MmioRead32(CR_DP_P0 + 0x7C),
    MmioRead32(INTF0 + 0x54), MmioRead32(INTF0 + 0x100),
    (MmioRead32(DPU + 0x360) >> 22) & 3, (MmioRead32(DPU + 0x364) >> 22) & 1,
    MmioRead32(DPU + 0x2B4)));
  /* Pure observation of the catalogue's DMA1 XIN5 and shared VBIF error
     latches. Never clear another display owner's requests/errors or change
     halt/OT/QoS policy based on a speculative white-screen diagnosis. */
  DEBUG((EFI_ERROR(Status) ? DEBUG_ERROR : DEBUG_INIT,
    "MDSSDpCr: VBIF DMA1/XIN5 halt-request/ack=%u/%u read-limit=%u memtype=%u pending/src-errors=%08x/%08x\n",
    (MmioRead32(VBIF + 0x200) >> 5) & 1, (MmioRead32(VBIF + 0x204) >> 5) & 1,
    (MmioRead32(VBIF + 0xB4) >> 8) & 0xFF, (MmioRead32(VBIF + 0x160) >> 20) & 7,
    MmioRead32(VBIF + 0x190), MmioRead32(VBIF + 0x194)));
}

VOID CrDpSnapshotScanout(VOID)
{
  EFI_STATUS Status = (MmioRead32(DPU + 0x14) & INTF0_UNDERRUN) != 0 ? EFI_DEVICE_ERROR :
    (MmioRead32(INTF0 + 0x26C) & BIT0) == 0 ? EFI_NOT_READY : EFI_SUCCESS;
  Snapshot(Status);
}

VOID CrDpStartScanout(CONST CR_DP_TIMING *T, EFI_PHYSICAL_ADDRESS Iova, UINT32 Stride)
{
  UINT32 HTotal = T->Width + T->HFront + T->HSync + T->HBack;
  UINT32 VTotal = T->Height + T->VFront + T->VSync + T->VBack;
  UINT32 HStart = T->HSync + T->HBack;
  UINT32 HWindow = HStart | ((HStart + T->Width - 1) << 16);
  UINT32 VStart = (T->VSync + T->VBack) * HTotal + HStart;
  UINT32 VEnd = (VTotal - T->VFront) * HTotal - 1 - T->HFront;
  UINT32 StartLines = T->VSync + T->VBack;
  UINT32 FetchLines = StartLines >= 24 ? 0 : 24 - StartLines;
  UINT32 I, MixerOp;
  if (FetchLines > T->VFront) FetchLines = T->VFront;
  /* Android sde_hw_sspp_setup_secure clears all four address-security
     bits for a nonsecure RECT_SOLO. Our framebuffer uses the nonsecure
     SMMU mapping, so never inherit a secure boot-display address route. */
  DEBUG((DEBUG_INIT, "MDSSDpCr: DMA1 inherited address-security=%08x\n",
         MmioRead32(DMA1 + 0x70)));
  MmioAnd32(DMA1 + 0x70, ~0xFU);
  MmioWrite32(DMA1, T->Width | ((UINT32)T->Height << 16));
  MmioWrite32(DMA1 + 8, 0);
  MmioWrite32(DMA1 + 0xC, T->Width | ((UINT32)T->Height << 16));
  MmioWrite32(DMA1 + 0x10, 0);
  MmioWrite32(DMA1 + 0x14, (UINT32)Iova);
  MmioWrite32(DMA1 + 0x18, 0); MmioWrite32(DMA1 + 0x1C, 0); MmioWrite32(DMA1 + 0x20, 0);
  MmioWrite32(DMA1 + 0x24, Stride);
  MmioWrite32(DMA1 + 0x30, 0xFF | (3U << 9) | (3U << 12) | BIT17);
  MmioWrite32(DMA1 + 0x34, 0x03020001);
  MmioWrite32(DMA1 + 0x38, BIT31); MmioWrite32(DMA1 + 0x170, 0);
  for (I = 0; I < 3; ++I) {
    MmioWrite32(DMA1 + 0x100 + I * 0x10, 0);
    MmioWrite32(DMA1 + 0x104 + I * 0x10, 0);
    MmioWrite32(DMA1 + 0x108 + I * 0x10, T->Width | ((UINT32)T->Height << 16));
  }
  MmioWrite32(DMA1 + 0x60, 0x3FFFF); MmioWrite32(DMA1 + 0x64, 0xFE00);
  MmioWrite32(DMA1 + 0x74, 0x22335777); MmioWrite32(DMA1 + 0x78, 0x00112222);
  MmioWrite32(DMA1 + 0x6C, BIT0);
  MmioOr32(DPU + 0x2B4, BIT8);
  /* DPU 8.1 routes INTF0 to DP PHY0 in both directions. Preserve INTF1. */
  MmioAndThenOr32(DPU + 0x460, (UINT32)~(7U | (7U << 6)), 1U | (1U << 6));
  MixerOp = MmioRead32(LM2);
  DEBUG((DEBUG_INIT, "MDSSDpCr: LM2 inherited op=%08x\n", MixerOp));
  /* Linux dpu_crtc selects the solo BASE stage and setup_alpha_out replaces
     the low operation bits with BIT(BASE). Preserve bit30 as upstream does,
     disable split output, and discard stale blending-stage selections. */
  MmioWrite32(LM2, (MixerOp & BIT30) | BIT0);
  MmioWrite32(LM2 + 4, T->Width | ((UINT32)T->Height << 16));
  MmioWrite32(CTL1 + 8, BIT24 | (1U << 21)); /* LM2 base = DMA1 RECT0 */
  MmioWrite32(CTL1 + 0x48, 0); MmioWrite32(CTL1 + 0x78, 0);
  MmioWrite32(CTL1 + 0xA8, 0);
  MmioWrite32(CTL1 + 0x14, 0xFU << 28);
  MmioWrite32(CTL1 + 0xF4, BIT0);
  /* DMA1 has SSPP inventory id10 but fetch_tbl maps it to hardware bit1.
     DPU8.1 stages LM2 through CTL_LAYER+8; PIPE_ACTIVE/LAYER_ACTIVE and
     LAYER_EXT4 are newer hardware, not enabled by the SM8450 catalogue. */
  MmioWrite32(CTL1 + 0xFC, BIT1);
  MmioWrite32(CTL1 + 0x128, BIT0); /* DP peripheral flush */
  MmioWrite32(INTF0 + 8, (HTotal << 16) | T->HSync);
  MmioWrite32(INTF0 + 0xC, VTotal * HTotal);
  MmioWrite32(INTF0 + 0x14, T->VSync * HTotal);
  MmioWrite32(INTF0 + 0x1C, VStart); MmioWrite32(INTF0 + 0x24, VEnd);
  MmioWrite32(INTF0 + 0x2C, VStart); MmioWrite32(INTF0 + 0x34, VStart + T->Height * HTotal - 1);
  MmioWrite32(INTF0 + 0x3C, HWindow); MmioWrite32(INTF0 + 0x40, HWindow);
  MmioWrite32(INTF0 + 0x44, 0); MmioWrite32(INTF0 + 0x48, 0xFF);
  MmioWrite32(INTF0 + 0x4C, 0);
  MmioWrite32(INTF0 + 0x50, (T->HNegative ? BIT0 : 0) | (T->VNegative ? BIT1 : 0));
  MmioWrite32(INTF0 + 0x60, BIT4);
  MmioWrite32(INTF0 + 0x64, HWindow); MmioWrite32(INTF0 + 0x68, 0);
  MmioWrite32(INTF0 + 0x90, 0x213F);
  MmioWrite32(INTF0 + 0xA8, 3); MmioWrite32(INTF0 + 0x25C, 2); /* PP2 */
  /* Linux programmable_fetch_get_num_lines uses SM8450's 24-line
     worst-case latency minus VSYNC + VBP, bounded by VFP. In particular,
     1080p60 already has 41 start-of-frame lines and needs no VFP fetch. */
  MmioWrite32(INTF0 + 0x170, FetchLines ? (VTotal - FetchLines) * HTotal + 1 : 0);
  MmioWrite32(INTF0 + 4, BIT29 | BIT30 | (FetchLines ? BIT31 : 0));
  DEBUG((DEBUG_INIT, "MDSSDpCr: prepared INTF0 config=%08x config2=%08x hctl=%08x data-hctl=%08x CTL1 top=%08x\n",
    MmioRead32(INTF0 + 4), MmioRead32(INTF0 + 0x60), MmioRead32(INTF0 + 0x3C),
    MmioRead32(INTF0 + 0x64), MmioRead32(CTL1 + 0x14)));
  MmioWrite32(CTL1 + 0x110, BIT0);
  MmioWrite32(CTL1 + 0x18, BIT12 | BIT8 | BIT17 | BIT30 | BIT31);
  /* Clear only our old INTF0 underrun latch. Leave the live DSI owner's
     interrupt status and enables untouched. */
  MmioWrite32(DPU + 0x18, INTF0_UNDERRUN);
  MemoryFence();
  MmioWrite32(CTL1 + 0x1C, 1); MmioWrite32(INTF0, 1);
}
EFI_STATUS CrDpStopScanout(VOID)
{
  EFI_STATUS Status;
  MmioWrite32(INTF0, 0); MemoryFence();
  Status = CrDpWait(INTF0 + 0x26C, BIT0, 0, 100000);
  if (EFI_ERROR(Status)) return Status;
  MmioWrite32(CTL1 + 0x30, 1); MemoryFence();
  return CrDpWait(CTL1 + 0x30, BIT0, 0, 2000);
}
EFI_STATUS CrDpWaitScanout(VOID)
{
  UINT32 I, First = 0;
  BOOLEAN HaveFrame = FALSE;
  EFI_STATUS Status;
  for (I = 0; I < 250000; I += 50) {
    if ((MmioRead32(DPU + 0x14) & INTF0_UNDERRUN) != 0) {
      Snapshot(EFI_DEVICE_ERROR);
      return EFI_DEVICE_ERROR;
    }
    if ((MmioRead32(INTF0 + 0x26C) & BIT0) != 0) {
      UINT32 Frame = MmioRead32(INTF0 + 0xAC);
      if (!HaveFrame) { First = Frame; HaveFrame = TRUE; }
      else if (Frame != First && MmioRead32(CTL1 + 0x18) == 0) {
        UINT8 Sink[6] = {0};
        UINT8 Lanes = (UINT8)(((MmioRead32(CR_DP_LINK + 8) >> 4) & 3) + 1);
        Status = CrDpAuxTransfer(TRUE, TRUE, 0x202, Sink, sizeof(Sink));
        if (!EFI_ERROR(Status) && !CrDpLaneStatus(Sink, Lanes, TRUE)) Status = EFI_DEVICE_ERROR;
        DEBUG((EFI_ERROR(Status) ? DEBUG_ERROR : DEBUG_INIT,
          "MDSSDpCr: running sink lanes=%u status=%02x/%02x align=%02x: %r\n",
          Lanes, Sink[0], Sink[1], Sink[2], Status));
        Snapshot(Status);
        return Status;
      }
    } else HaveFrame = FALSE;
    MicroSecondDelay(50);
  }
  Snapshot(EFI_TIMEOUT);
  return EFI_TIMEOUT;
}
