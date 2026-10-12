/** @file
  SM8450 scanout setup/shutdown, based on Linux msm dpu_plane.c,
  dpu_hw_sspp.c, dpu_hw_intf.c, dpu_hw_ctl.c and dsi_host.c.
  SPDX-License-Identifier: GPL-2.0-only
**/
#include "MdpScanout.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/MdssCrLib.h>

/* Mainline sm8450.dtsi "mdp" resource, matching DPU catalog offsets.
   Android's MDSS resource starts 0x1000 earlier and uses larger offsets. */
#define MDP_BASE 0x0AE01000U

EFI_STATUS
MdpGetVideoFetchStart (
  IN CONST EFI_MDP_DSI_TIMING *Timing, OUT UINT32 *FetchStart
  )
{
  CR_MDSS_MODE Mode = { 0 };

  if (Timing == NULL || FetchStart == NULL) return EFI_INVALID_PARAMETER;
  Mode.Width = Timing->Width; Mode.Height = Timing->Height;
  Mode.HSync = Timing->HSync; Mode.HBackPorch = Timing->HBackPorch;
  Mode.HFrontPorch = Timing->HFrontPorch;
  Mode.VSync = Timing->VSync; Mode.VBackPorch = Timing->VBackPorch;
  Mode.VFrontPorch = Timing->VFrontPorch;
  Mode.RefreshQ16 = Timing->DsiRefreshRate;
  return CrMdssGetFetchStart (&Mode, FetchStart);
}

EFI_STATUS
MdpClearFrameBuffer (
  IN VOID *FrameBuffer, IN UINTN BufferSize,
  IN UINT32 Width, IN UINT32 Height, IN UINT32 Stride
  )
{
  UINT64 Size = (UINT64)Stride * Height;

  if (FrameBuffer == NULL || Width == 0 || Height == 0 ||
      (UINT64)Width * 4 > Stride || Size > BufferSize ||
      Size > MAX_UINTN - (UINTN)FrameBuffer) {
    return EFI_INVALID_PARAMETER;
  }
  /* GOP SetMode requires black output, including setting the current mode.
     Zero BGRX pixels (and line padding), then publish CPU writes to DPU. */
  ZeroMem (FrameBuffer, (UINTN)Size);
  WriteBackDataCacheRange (FrameBuffer, (UINTN)Size);
  return EFI_SUCCESS;
}

VOID
MdpSetupDmaQos (VOID)
{
  UINTN Sspp = MDP_BASE + 0x24000;

  /* Linux sm8450_perf_data selects sc7180_qos_linear for linear realtime
     scanout. DPU 8.1 uses the eight-level (64-bit) CREQ register pair.
     The upstream catalog still labels these LUT tables FIXME; matching
     them is a source baseline, not board-validated QoS tuning. */
  MmioWrite32 (Sspp + 0x060, 0x0003FFFF);
  MmioWrite32 (Sspp + 0x064, 0x0000FE00);
  MmioWrite32 (Sspp + 0x074, 0x22335777);
  MmioWrite32 (Sspp + 0x078, 0x00112222);
  /* Enable danger/safe only after all LUT entries are initialized, as
     _dpu_hw_setup_qos_lut does for a realtime non-cursor pipe. */
  MmioWrite32 (Sspp + 0x06C, BIT0);
}

VOID
MdpSetupSingleMixer (IN UINT32 Width, IN UINT32 Height)
{
  UINTN Mixer = MDP_BASE + 0x44000;
  UINT32 Mode = MmioRead32 (Mixer);

  /* Linux dpu_hw_lm_setup_out explicitly clears SPLIT_LEFT_RIGHT for
     a single/left mixer. Do not inherit right-half selection from a
     previous split-display configuration. Preserve other mixer controls. */
  MmioWrite32 (Mixer + 4, Width | (Height << 16));
  MmioWrite32 (Mixer, Mode & ~(UINT32)BIT31);
}

VOID
MdpSetupDmaRouting (VOID)
{
  UINTN Ctl = MDP_BASE + 0x15000;

  /* Linux dpu_hw_ctl_setup_blendstage writes all four routing words on
     DPU 8.1. DMA0 RECT0 is base stage (encoded as 1) in bits 20:18;
     EXT bit 16 is its high stage bit. Clear inherited cursor, DMA RECT1,
     DMA2/3 and VIG RECT1 routes rather than depending on reset values. */
  MmioWrite32 (Ctl + 0x040, 0);
  MmioWrite32 (Ctl + 0x070, 0);
  MmioWrite32 (Ctl + 0x0A0, 0);
  MmioWrite32 (Ctl, BIT24 | (1U << 18));
}

VOID
MdpSetupDmaPixelExtension (IN UINT32 Width, IN UINT32 Height)
{
  UINTN Sspp = MDP_BASE + 0x24000;
  UINT32 Component;
  /* DPU_MAX_PLANES is four, but plane 2 shares the C1C2 register bank.
     Linux dpu_hw_sspp_setup_pe_config() therefore emits C0, C1C2 (using
     component 1), and C3; treating loop index 2 as a fourth register would
     accidentally program C3 from the nonexistent C2 bank. */
  /* Register banks are C0, C1C2, and C3 at offsets 0x100, 0x110, and
     0x120; the final bank is therefore offset index 2 even though it
     represents logical component 3. */
  STATIC CONST UINT32 ComponentBank[] = { 0, 1, 2 };

  /* Linux dpu_plane.c:_dpu_plane_setup_pixel_ext and
     dpu_hw_sspp.c:setup_pe_config use the full source ROI for each RGB
     component, with no fetched/repeated edge pixels for 1:1 scanout.
     C1 and C2 share one bank; C0 and C3 have their own banks. */
  for (Component = 0; Component < ARRAY_SIZE (ComponentBank); ++Component) {
    UINTN Extension = Sspp + 0x100 + ComponentBank[Component] * 0x10;
    MmioWrite32 (Extension, 0);
    MmioWrite32 (Extension + 4, 0);
    MmioWrite32 (Extension + 8, Width | (Height << 16));
  }
  /* dpu_hw_setup_multirect_impl disables multirect for RECT_SOLO.
     setup_format enables the software pixel-extension override even for
     linear RGB DMA pipes, which have no scaler to supply these values. */
  MmioWrite32 (Sspp + 0x170, 0);
  MmioWrite32 (Sspp + 0x038, BIT31);
}

STATIC BOOLEAN
WaitClear (IN UINTN Address, IN UINT32 Mask, IN UINT32 TimeoutUs)
{
  UINT32 Elapsed;
  for (Elapsed = 0; Elapsed < TimeoutUs; Elapsed += 50) {
    if ((MmioRead32 (Address) & Mask) == 0) return TRUE;
    MicroSecondDelay (50);
  }
  return (MmioRead32 (Address) & Mask) == 0;
}

EFI_STATUS
MdpWaitScanoutProgress (IN UINT32 Controller)
{
  UINTN Intf;
  UINT32 InitialFrame = 0;
  UINT32 Elapsed;
  BOOLEAN HaveFrame = FALSE;

  if (Controller > 1) return EFI_INVALID_PARAMETER;
  Intf = MDP_BASE + ((Controller == 1) ? 0x36000 : 0x35000);
  /* Linux dpu_hw_intf_get_status uses INTF_STATUS[0] on DPU >= 5
     and reads the full 32-bit INTF_FRAME_COUNT. Video commit completion
     (dpu_encoder_phys_vid_wait_for_commit_done) requires CTL_FLUSH == 0.
     Poll those conditions instead of installing an IRQ handler in DXE.
     Allow 250 ms for startup, including slower normal video modes. */
  MemoryFence ();
  for (Elapsed = 0; Elapsed <= 250000; Elapsed += 50) {
    if ((MmioRead32 (Intf + 0x26C) & BIT0) != 0) {
      UINT32 Frame = MmioRead32 (Intf + 0x0AC);
      if (!HaveFrame) {
        InitialFrame = Frame;
        HaveFrame = TRUE;
      } else if (Frame != InitialFrame &&
                 MmioRead32 (MDP_BASE + 0x15000 + 0x018) == 0) {
        /* Inequality also accepts the 0xffffffff -> 0 counter wrap. */
        return EFI_SUCCESS;
      }
    } else {
      HaveFrame = FALSE;
    }
    if (Elapsed != 250000) MicroSecondDelay (50);
  }
  return EFI_TIMEOUT;
}

EFI_STATUS
MdpStopScanout (IN UINT32 Controller)
{
  UINTN Intf;
  UINTN Ctl = MDP_BASE + 0x15000;
  UINTN Dsi;
  UINT32 Ctrl;
  if (Controller > 1) return EFI_INVALID_PARAMETER;
  Intf = MDP_BASE + ((Controller == 1) ? 0x36000 : 0x35000);
  Dsi = ((Controller == 1) ? 0x0AE96000 : 0x0AE94000) + 4;

  /* The enable write latches at a frame boundary. DPU 5+ provides the
     actual enable state in INTF_STATUS, unlike TIMING_ENGINE_EN readback.
     Keep DSI clocks running until that disable has latched. */
  MmioWrite32 (Intf, 0);
  MemoryFence ();
  if (!WaitClear (Intf + 0x26C, BIT0, 100000)) return EFI_TIMEOUT;

  /* Linux CTL reset drains/resets the owned control path; do not release
     its framebuffer mapping on a missing reset acknowledgement. */
  MmioWrite32 (Ctl + 0x030, 1);
  MemoryFence ();
  if (!WaitClear (Ctl + 0x030, BIT0, 2000)) return EFI_TIMEOUT;

  /* msm_dsi_host_disable resets the video engine after INTF is disabled.
     Use the DSI6G four-byte operational-register shift. */
  Ctrl = MmioRead32 (Dsi);
  MmioWrite32 (Dsi, Ctrl & ~(BIT0 | BIT1));
  MemoryFence ();
  MmioWrite32 (Dsi + 0x114, 1);
  MicroSecondDelay (20000);
  MmioWrite32 (Dsi + 0x114, 0);
  MemoryFence ();
  if ((MmioRead32 (Dsi) & (BIT0 | BIT1)) != 0) return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}
