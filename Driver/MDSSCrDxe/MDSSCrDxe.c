/**
  Open SM8450 MDSS/DSI framebuffer driver.

  The register layout and timing equations in this file are taken from the
  Linux msm DPU and DSI drivers.  The panel input is the same XML format used
  by the HDK firmware.  No Qualcomm DisplayDxe ABI is used here.
**/
#include <PiDxe.h>
#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/GraphicsOutput.h>
#include <Protocol/EFIDisplayPwr.h>
#include <Protocol/EFIMdpDsiProtocol.h>
#include <Protocol/EFILt9611CrProtocol.h>
#include <Protocol/EFIMdssHandoffProtocol.h>
#include <Configuration/DisplayDevicePath.h>
#include "MdpPanel.h"
#include "MdpResources.h"
#include "MdpScanout.h"
#include "MdpDsiCommand.h"
#include "MdpHdkExtendedProfile.h"
#include "MdpSelectedPanel.h"

/*
 * Mainline sm8450.dtsi maps the DPU "mdp" resource at 0x0ae01000.
 * dpu_8_1_sm8450.h offsets (DMA0 0x24000, CTL0 0x15000,
 * INTF1 0x35000) are relative to that resource. Android maps the larger
 * MDSS window at 0x0ae00000 and uses offsets 0x1000 higher. Keep the DPU base
 * separate from the DSI controller resources below, which are at 0xae94000
 * and 0xae96000 in sm8450.dtsi.
 */
#define MDP_BASE                  0x0AE01000ULL
#define DSI0_BASE                 0x0AE94000ULL
#define DSI1_BASE                 0x0AE96000ULL
#define MDP_BIT(_n)               (1u << (_n))

/* Linux dpu_8_1_sm8450.h catalog offsets. */
#define DPU_SSPP_DMA0             0x24000
#define DPU_CTL0                  0x15000
#define DPU_INTF1                 0x35000
#define DPU_INTF2                 0x36000

#define SSPP_SRC_SIZE             0x00
#define SSPP_SRC_XY               0x08
#define SSPP_OUT_SIZE             0x0C
#define SSPP_OUT_XY               0x10
#define SSPP_SRC0_ADDR            0x14
#define SSPP_SRC1_ADDR            0x18
#define SSPP_SRC_YSTRIDE0         0x24
#define SSPP_SRC_FORMAT           0x30
#define SSPP_SRC_UNPACK_PATTERN   0x34
#define SSPP_SRC_OP_MODE          0x38

/* top_0 clock control from sm8450_mdp.clk_ctrls[DPU_CLK_CTRL_DMA0]. */
#define DPU_DMA0_CLK_CTRL         0x2AC
#define DPU_DMA0_CLK_BIT          8

#define CTL_TOP                   0x014
#define CTL_FLUSH                 0x018
#define CTL_START                 0x01C
#define CTL_INTF_ACTIVE           0x0F4
#define CTL_FETCH_PIPE_ACTIVE     0x0FC
#define CTL_PIPE_ACTIVE           0x12C
#define CTL_LAYER_ACTIVE          0x130
#define CTL_INTF_FLUSH            0x110

#define INTF_TIMING_ENGINE_EN     0x000
#define INTF_CONFIG                0x004
#define INTF_HSYNC_CTL             0x008
#define INTF_VSYNC_PERIOD_F0       0x00C
#define INTF_VSYNC_PULSE_WIDTH_F0  0x014
#define INTF_DISPLAY_V_START_F0    0x01C
#define INTF_DISPLAY_V_END_F0      0x024
#define INTF_DISPLAY_HCTL          0x03C
#define INTF_ACTIVE_HCTL           0x040
#define INTF_BORDER_COLOR          0x044
#define INTF_UNDERFLOW_COLOR       0x048
#define INTF_HSYNC_SKEW            0x04C
#define INTF_POLARITY_CTL          0x050
#define INTF_CONFIG2               0x060
#define INTF_DISPLAY_DATA_HCTL     0x064
#define INTF_ACTIVE_DATA_HCTL      0x068
#define INTF_PANEL_FORMAT          0x090
#define INTF_FRAME_LINE_COUNT_EN   0x0A8
#define INTF_PROG_FETCH_START      0x170
#define INTF_MUX                   0x25C

/* Qualcomm DSI 6G offsets used by Linux dsi_host.c. */
#define DSI_VERSION                0x1F0
#define DSI_6G_HW_VERSION          0x000
#define DSI_6G_REG_SHIFT           0x004
#define DSI_CTRL                   0x000
#define DSI_ACTIVE_H               0x020
#define DSI_ACTIVE_V               0x024
#define DSI_TOTAL                  0x028
#define DSI_ACTIVE_HSYNC           0x02C
#define DSI_ACTIVE_VSYNC_HPOS      0x030
#define DSI_ACTIVE_VSYNC_VPOS      0x034
#define DSI_CLK_CTRL               0x118
#define DSI_RESET                  0x114

#define DSI_CTRL_ENABLE            MDP_BIT (0)
#define DSI_CTRL_VID_MODE_EN       MDP_BIT (1)
#define DSI_CTRL_CLK_EN            MDP_BIT (8)
#define DSI_CTRL_LANE0             MDP_BIT (4)
#define DSI_VID_CFG0                0x00C
#define DSI_VID_CFG1                0x01C
#define DSI_LANE_SWAP_CTRL          0x0AC
#define DSI_LANE_CTRL               0x0A8
#define DSI_EOT_PACKET_CTRL         0x0C8
#define DSI_CLKOUT_TIMING_CTRL      0x0C0
#define DSI_T_CLK_PRE_EXTEND         0x17C
#define DSI_VID_CFG0_VC(_v)         ((_v) & 0x3)
#define DSI_VID_CFG0_FORMAT(_f)      (((_f) & 0x7) << 4)
#define DSI_VID_CFG0_TRAFFIC(_m)    (((_m) & 0x3) << 8)
#define INTF_CFG2_DATA_HCTL_EN     MDP_BIT (4)
#define INTF_CFG_ACTIVE_H_EN       MDP_BIT (29)
#define INTF_CFG_ACTIVE_V_EN       MDP_BIT (30)

#define PANEL_XML_FILE_GUID \
  { 0x7321f393, 0x1df3, 0x4a58, { 0x90, 0x40, 0xdc, 0x1e, 0xef, 0x15, 0xeb, 0x35 } }

typedef struct {
  EFI_GRAPHICS_OUTPUT_PROTOCOL        Gop;
  EFI_MDP_DSI_PROTOCOL                Dsi;
  EFI_DISPLAY_POWER_PROTOCOL          Power;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION Info;
  EFI_PHYSICAL_ADDRESS                FrameBuffer;
  EFI_PHYSICAL_ADDRESS                DeviceFrameBuffer;
  UINT32                              FrameBufferStride;
  MDP_PANEL_CONFIG                    Panel;
  FRAME_BUFFER_CONFIGURE              *BltConfig;
  UINTN                               BltConfigSize;
  BOOLEAN                             ScanoutActive;
  BOOLEAN                             ResourcesReady;
  BOOLEAN                             HostConfigured;
  BOOLEAN                             PowerPending;
  BOOLEAN                             PanelCommandsStarted;
  BOOLEAN                             GopPublished;
  EFI_HANDLE                          Handle;
  BOOLEAN                             Busy;
  BOOLEAN                             Frozen;
  BOOLEAN                             FreezeComplete;
  EFI_STATUS                          FreezeStatus;
} MDP_DISPLAY;

STATIC MDP_DISPLAY mDisplay;
STATIC EFI_MDSS_HANDOFF_PROTOCOL *mHandoff;
STATIC BOOLEAN mHandoffRegistered;
STATIC EFI_DISPLAY_DEVICE_PATH mDisplayDevicePath = ANDROMEDA_DISPLAY_DEVICE_PATH_INIT;

STATIC EFI_STATUS EFIAPI MdpBridgeReady (IN EFI_MDP_DSI_PROTOCOL *This);

STATIC EFI_STATUS BeginDisplay (VOID)
{
  EFI_STATUS Status;
  EFI_TPL Tpl = gBS->RaiseTPL(TPL_NOTIFY);
  Status = mDisplay.Frozen ? EFI_ACCESS_DENIED : (mDisplay.Busy ? EFI_NOT_READY : EFI_SUCCESS);
  if (Status == EFI_SUCCESS) mDisplay.Busy = TRUE;
  gBS->RestoreTPL(Tpl);
  return Status;
}
STATIC EFI_STATUS EndDisplay (EFI_STATUS Status)
{
  EFI_TPL Tpl = gBS->RaiseTPL(TPL_NOTIFY);
  mDisplay.Busy = FALSE;
  gBS->RestoreTPL(Tpl);
  return Status;
}

STATIC EFI_STATUS EFIAPI FreezeDisplay (VOID *Context)
{
  EFI_TPL Tpl;
  if (Context != &mDisplay) return EFI_INVALID_PARAMETER;
  Tpl = gBS->RaiseTPL(TPL_NOTIFY);
  if (!mDisplay.Frozen) {
    mDisplay.Frozen = TRUE;
    mDisplay.FreezeStatus = mDisplay.Busy ? EFI_NOT_READY : MdpDsiCommandFreeze();
    mDisplay.FreezeComplete = !mDisplay.Busy;
  }
  EFI_STATUS Status = mDisplay.FreezeComplete ? EFI_SUCCESS : EFI_NOT_READY;
  gBS->RestoreTPL(Tpl);
  return Status;
}

STATIC EFI_STATUS EFIAPI SnapshotDisplay (VOID *Context, CR_MDSS_HANDOFF_PARTICIPANT *P, VOID **Map)
{
  EFI_STATUS Resources;
  EFI_MDP_DSI_TIMING *T=&mDisplay.Panel.Timing;
  CR_MDSS_NATIVE_ROUTE *R;
  if (Context != &mDisplay || !P || !Map) return EFI_INVALID_PARAMETER;
  if (!mDisplay.Frozen || !mDisplay.FreezeComplete || mDisplay.Busy) return EFI_NOT_READY;
  ZeroMem(P,sizeof(*P)); P->Owner=CR_MDSS_NATIVE_DSI; R=&P->Route; R->Owner=P->Owner;
  Resources=MdpSnapshotResources(P,Map);
  R->State=CR_MDSS_NATIVE_UNCERTAIN;
  R->Flags=CR_MDSS_NATIVE_OWNED;
  if (Resources!=EFI_SUCCESS || mDisplay.FreezeStatus!=EFI_SUCCESS || mDisplay.PowerPending) R->Flags|=CR_MDSS_NATIVE_RETAINED_ERROR;
  else R->State=mDisplay.ScanoutActive ? CR_MDSS_NATIVE_LIVE : CR_MDSS_NATIVE_STOPPED;
  /* This locator targets HDK8450's DSI0 only. A different selected host is
     preserved as a nontransferable diagnostic, never described as DSI0. */
  R->Controller=T->DsiController;
  R->Dpu=MDP_BASE; R->DpuBytes=0x8f000;
  R->InterfaceBase=MDP_BASE+(T->DsiController?DPU_INTF2:DPU_INTF1); R->InterfaceBytes=0x400;
  R->PipeBase=MDP_BASE+DPU_SSPP_DMA0; R->PipeBytes=0x1000;
  R->CtlBase=MDP_BASE+DPU_CTL0; R->CtlBytes=0x1000;
  R->MixerBase=MDP_BASE+0x44000; R->MixerBytes=0x1000;
  R->LinkBase=T->DsiController?DSI1_BASE:DSI0_BASE; R->LinkBytes=0x400;
  R->PhyBase=R->LinkBase+0x400; R->PhyBytes=0x500;
  R->PllBase=R->LinkBase+0x900; R->PllBytes=0x300;
  R->FrameBuffer=mDisplay.FrameBuffer;
  R->FrameBufferBytes=(UINT64)mDisplay.FrameBufferStride*T->Height;
  R->DeviceAddress=mDisplay.DeviceFrameBuffer;
  if (R->FrameBufferBytes && T->Width && T->Height && T->DsiController==0) {
    R->Mode.Width=T->Width; R->Mode.Height=T->Height; R->Mode.Stride=mDisplay.FrameBufferStride;
    R->Mode.Format=CR_MDSS_NATIVE_FORMAT_BGRX8888;
    R->Mode.HFront=T->HFrontPorch; R->Mode.HSync=T->HSync; R->Mode.HBack=T->HBackPorch;
    R->Mode.VFront=T->VFrontPorch; R->Mode.VSync=T->VSync; R->Mode.VBack=T->VBackPorch;
    R->Mode.PixelHz=(((UINT64)T->Width+T->HFrontPorch+T->HSync+T->HBackPorch)*
      ((UINT64)T->Height+T->VFrontPorch+T->VSync+T->VBackPorch)*T->DsiRefreshRate+0x8000)>>16;
  } else {
    R->State=CR_MDSS_NATIVE_UNCERTAIN; R->Flags|=CR_MDSS_NATIVE_RETAINED_ERROR;
    if (!R->FrameBufferBytes) R->FrameBuffer=0;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS UnregisterHandoff (VOID)
{
  EFI_STATUS Status;
  if (!mHandoffRegistered) return EFI_SUCCESS;
  Status=mHandoff->SetParticipant(mHandoff,CR_MDSS_NATIVE_DSI,&mDisplay,NULL,NULL);
  if (Status==EFI_SUCCESS) mHandoffRegistered=FALSE;
  return Status;
}

STATIC EFI_STATUS
RunPanelSequence (IN CONST MDP_DSI_SEQUENCE *Sequence)
{
  EFI_STATUS Status;
  UINTN Index;

  if (Sequence->Unsupported) return EFI_UNSUPPORTED;
  for (Index = 0; Index < Sequence->Count; ++Index) {
    CONST MDP_DSI_COMMAND *Command = &Sequence->Commands[Index];
    if (Command->DataType != 0xFF) {
      Status = MdpDsiCommandTransfer (
                 mDisplay.Panel.Timing.DsiController,
                 (UINT8)mDisplay.Panel.Timing.DsiVirtualChannel,
                 Command->DataType, Command->Payload, Command->Length, TRUE);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "MDSSCrDxe: panel command %u type=0x%x failed: %r\n",
                (UINT32)Index, Command->DataType, Status));
        return Status;
      }
    }
    if (Command->DelayMs != 0) {
      MicroSecondDelay ((UINTN)Command->DelayMs * 1000);
    }
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS LoadPanelXml (OUT MDP_PANEL_CONFIG *Panel)
{
  EFI_STATUS Status;
  VOID *Buffer;
  UINTN Size;
  EFI_GUID FileGuid = PANEL_XML_FILE_GUID;
  Buffer = NULL; Size = 0;
  Status = GetSectionFromAnyFv (&FileGuid, EFI_SECTION_RAW, 0, &Buffer, &Size);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MDSSCrDxe: panel XML unavailable: %r\n", Status));
    return Status;
  }
  Status = MdpParsePanelXml ((CONST CHAR8 *)Buffer, Size, Panel);
  FreePool (Buffer);
  if (EFI_ERROR (Status)) return Status;
  return MdpValidateVideoPanel (Panel);
}

STATIC
VOID DpuWriteTiming (IN CONST MDP_PANEL_CONFIG *Panel, IN EFI_PHYSICAL_ADDRESS Fb,
                     IN UINT32 Stride, IN UINT32 FetchStart)
{
  CONST EFI_MDP_DSI_TIMING *P = &Panel->Timing;
  UINT32 HTotal = P->HSync + P->HBackPorch + P->Width + P->HFrontPorch;
  UINT32 VTotal = P->VSync + P->VBackPorch + P->Height + P->VFrontPorch;
  UINT32 HSyncStart = P->HBackPorch + P->HSync;
  UINT32 HSyncEnd = HTotal - P->HFrontPorch - 1;
  UINT32 DisplayVStart = (P->VSync + P->VBackPorch) * HTotal;
  UINT32 DisplayVEnd = (VTotal - P->VFrontPorch) * HTotal - 1;
  EFI_PHYSICAL_ADDRESS Dpu = MDP_BASE;
  EFI_PHYSICAL_ADDRESS Sspp = Dpu + DPU_SSPP_DMA0;
  EFI_PHYSICAL_ADDRESS Ctl = Dpu + DPU_CTL0;
  EFI_PHYSICAL_ADDRESS Intf = Dpu + ((P->DsiController == 1) ? DPU_INTF2 : DPU_INTF1);
  UINT32 IntfBit = P->DsiController + 1;
  DEBUG ((DEBUG_INIT,
          "MDSSCrDxe: DPU program DMA0=0x%lx CTL0=0x%lx INTF=0x%lx IOVA=0x%lx\n",
          Sspp, Ctl, Intf, Fb));
  /* Linux DPU DMA0 linear XRGB8888 setup. */
  MmioWrite32 ((UINTN)(Sspp + SSPP_SRC0_ADDR), (UINT32)Fb);
  /* SRC1_ADDR selects the second image plane; this linear RGBX path has
     one plane.  The DPU 8.1 fetch address is 32-bit on this platform. */
  MmioWrite32 ((UINTN)(Sspp + SSPP_SRC1_ADDR), 0);
  MmioWrite32 ((UINTN)(Sspp + SSPP_SRC_YSTRIDE0), Stride);
  MmioWrite32 ((UINTN)(Sspp + SSPP_SRC_SIZE), P->Width | (P->Height << 16));
  MmioWrite32 ((UINTN)(Sspp + SSPP_OUT_SIZE), P->Width | (P->Height << 16));
  MmioWrite32 ((UINTN)(Sspp + SSPP_SRC_XY), 0);
  MmioWrite32 ((UINTN)(Sspp + SSPP_OUT_XY), 0);
  /* FrameBufferBltConfig exposes B,G,R,X bytes.  This is Linux's
     XRGB8888 format: element[0..3] = B,G,R,X. */
  MmioWrite32 ((UINTN)(Sspp + SSPP_SRC_UNPACK_PATTERN), 0x03020001);
  /* mdp_common.xml encodes BPC8/BPC8A as value 3 (the fields are 2 bits).
     Writing the literal bit width (8) truncates to zero and produces an
     invalid fetch format on DPU 8.1. */
  MmioWrite32 ((UINTN)(Sspp + SSPP_SRC_FORMAT),
               (3u << 0) | (3u << 2) | (3u << 4) | (3u << 6) |
               (3u << 12) | (3u << 9) | MDP_BIT (17));
  MdpSetupDmaPixelExtension (P->Width, P->Height);
  MmioWrite32 ((UINTN)(Dpu + DPU_DMA0_CLK_CTRL),
               MmioRead32 ((UINTN)(Dpu + DPU_DMA0_CLK_CTRL)) |
               MDP_BIT (DPU_DMA0_CLK_BIT));
  MdpSetupDmaRouting ();
  MdpSetupDmaQos ();
  MmioWrite32 ((UINTN)(Ctl + CTL_INTF_ACTIVE), MDP_BIT (IntfBit));
  /* DPU's active masks use the fetch_tbl mapping from dpu_hw_ctl.c:
     SSPP_DMA0 is bit 0; VIG0 starts at bit 16. */
  MmioWrite32 ((UINTN)(Ctl + CTL_FETCH_PIPE_ACTIVE), MDP_BIT (0));
  MmioWrite32 ((UINTN)(Ctl + CTL_PIPE_ACTIVE), MDP_BIT (0));
  MmioWrite32 ((UINTN)(Ctl + CTL_LAYER_ACTIVE), MDP_BIT (0));
  MdpSetupSingleMixer (P->Width, P->Height);
  /* DPU 7+ requires the default group id in CTL_TOP[31:28]. */
  MmioWrite32 ((UINTN)(Ctl + CTL_TOP), 0xFu << 28);
  MmioWrite32 ((UINTN)(Intf + INTF_HSYNC_CTL), (HTotal << 16) | P->HSync);
  MmioWrite32 ((UINTN)(Intf + INTF_VSYNC_PERIOD_F0), VTotal * HTotal);
  MmioWrite32 ((UINTN)(Intf + INTF_VSYNC_PULSE_WIDTH_F0), P->VSync * HTotal);
  MmioWrite32 ((UINTN)(Intf + INTF_DISPLAY_HCTL), (HSyncEnd << 16) | HSyncStart);
  MmioWrite32 ((UINTN)(Intf + INTF_DISPLAY_V_START_F0), DisplayVStart);
  MmioWrite32 ((UINTN)(Intf + INTF_DISPLAY_V_END_F0), DisplayVEnd);
  MmioWrite32 ((UINTN)(Intf + INTF_ACTIVE_HCTL), 0);
  MmioWrite32 ((UINTN)(Intf + INTF_BORDER_COLOR), Panel->BorderColor);
  MmioWrite32 ((UINTN)(Intf + INTF_UNDERFLOW_COLOR), 0xff);
  MmioWrite32 ((UINTN)(Intf + INTF_HSYNC_SKEW), 0);
  MmioWrite32 ((UINTN)(Intf + INTF_POLARITY_CTL),
               (Panel->InvertHsyncPolarity ? MDP_BIT (0) : 0) |
               (Panel->InvertVsyncPolarity ? MDP_BIT (1) : 0));
  /* Linux BPC8 encoding is 3; 0x21 selects RGB888 packing. */
  MmioWrite32 ((UINTN)(Intf + INTF_PANEL_FORMAT), 0x213f);
  /* Linux dpu_hw_intf_setup_timing_engine enables both counters. */
  MmioWrite32 ((UINTN)(Intf + INTF_FRAME_LINE_COUNT_EN), 3);
  /* INTF1/2 are paired with ping-pong 0 for the single-display path. */
  MmioWrite32 ((UINTN)(Intf + INTF_MUX), 0);
  MmioWrite32 ((UINTN)(Intf + INTF_CONFIG2), INTF_CFG2_DATA_HCTL_EN);
  /* Linux programs the data window from hsync_start_x through the active
     width.  A zero start sends the first pixels during the porch on DPU 8.1. */
  MmioWrite32 ((UINTN)(Intf + INTF_DISPLAY_DATA_HCTL),
               ((HSyncStart + P->Width - 1) << 16) | HSyncStart);
  MmioWrite32 ((UINTN)(Intf + INTF_ACTIVE_DATA_HCTL), 0);
  /* Android sde_hw_intf_setup_prg_fetch: program the counter before enabling
     prefetch. This must be part of the CTL flush preceding timing enable. */
  if (FetchStart != 0)
    MmioWrite32 ((UINTN)(Intf + INTF_PROG_FETCH_START), FetchStart);
  MmioWrite32 ((UINTN)(Intf + INTF_CONFIG), FetchStart != 0 ? MDP_BIT (31) : 0);
  MmioWrite32 ((UINTN)(Ctl + CTL_INTF_FLUSH), MDP_BIT (IntfBit));
  DEBUG ((DEBUG_INIT, "MDSSCrDxe: DPU timing ready; flush/start scanout\n"));
  /* DMA0, LM0, interface and CTL pending bits (dpu_hw_ctl.c). */
  MmioWrite32 ((UINTN)(Ctl + CTL_FLUSH), MDP_BIT (11) | MDP_BIT (6) |
               MDP_BIT (31) | MDP_BIT (17));
  MmioWrite32 ((UINTN)(Ctl + CTL_START), 1);
  MmioWrite32 ((UINTN)(Intf + INTF_TIMING_ENGINE_EN), 1);
}

STATIC
EFI_STATUS EFIAPI ConfigureVideoInternal (IN EFI_MDP_DSI_PROTOCOL *This, IN CONST EFI_MDP_DSI_TIMING *Timing, IN EFI_PHYSICAL_ADDRESS Fb, IN UINT32 Stride)
{
  EFI_PHYSICAL_ADDRESS Base;
  EFI_PHYSICAL_ADDRESS DsiRegs;
  UINT32 Ctrl;
  UINT32 Version;
  UINT32 HostClockTiming;
  UINT32 FetchStart;
  EFI_STATUS Status;
  UINT64 HTotal;
  UINT64 VTotal;
  (VOID)This;
  if (Timing == NULL || Fb == 0 || Fb != mDisplay.FrameBuffer ||
      mDisplay.DeviceFrameBuffer == 0 ||
      Timing->DsiController > 1 ||
      Timing->Width == 0 || Timing->Height == 0 ||
      Timing->DsiLanes == 0 || Timing->DsiLanes > 4 ||
      (UINT64)Stride < (UINT64)Timing->Width * 4) return EFI_INVALID_PARAMETER;
  HTotal = (UINT64)Timing->HSync + Timing->HBackPorch + Timing->Width + Timing->HFrontPorch;
  VTotal = (UINT64)Timing->VSync + Timing->VBackPorch + Timing->Height + Timing->VFrontPorch;
  if (HTotal < Timing->Width || VTotal < Timing->Height || HTotal > 0x10000 ||
      VTotal > 0x10000 || HTotal * VTotal > MAX_UINT32)
    return EFI_BAD_BUFFER_SIZE;
  /* Resource rates and the DMA mapping were prepared for this fixed mode.
     The handoff protocol must not switch hosts or exceed that mapping. */
  if (CompareMem (Timing, &mDisplay.Panel.Timing, sizeof (*Timing)) != 0 ||
      Stride != mDisplay.FrameBufferStride) return EFI_UNSUPPORTED;
  if (mDisplay.ScanoutActive) return EFI_SUCCESS;
  Status = MdpGetVideoFetchStart (Timing, &FetchStart);
  if (EFI_ERROR (Status)) return Status;
  Status = MdpGetHostClockTiming (&HostClockTiming);
  if (EFI_ERROR (Status)) return Status;
  Base = (Timing->DsiController == 1) ? DSI1_BASE : DSI0_BASE;
  DEBUG ((DEBUG_INIT, "MDSSCrDxe: DSI probe base=0x%lx\n", Base));
  /* Linux probes the unshifted VERSION scratch location first.  A zero
     value identifies DSI6G; older DSIv2 hosts are outside this driver. */
  Version = MmioRead32 ((UINTN)(Base + DSI_VERSION));
  if (Version != 0 && Version != 0xffffffff) {
    DEBUG ((DEBUG_WARN, "MDSSCrDxe: unsupported unshifted DSI version=0x%08x\n", Version));
    return EFI_UNSUPPORTED;
  }
  /* SM8450 uses the Linux DSI6G(v3) layout: offset 0 is the hardware
     version and every operational register is shifted by four bytes. */
  DsiRegs = Base + DSI_6G_REG_SHIFT;
  Version = MmioRead32 ((UINTN)(DsiRegs + DSI_VERSION));
  DEBUG ((DEBUG_INIT, "MDSSCrDxe: DSI6G version=0x%08x; enable clocks/reset\n", Version));
  if (Version == 0 || Version == 0xffffffff) {
    DEBUG ((DEBUG_WARN, "MDSSCrDxe: DSI6G version unavailable (version=0x%08x hw=0x%08x)\n",
            Version, MmioRead32 ((UINTN)(Base + DSI_6G_HW_VERSION))));
    return EFI_NOT_READY;
  }
  /* Follow dsi_ctrl_enable()/dsi_sw_reset() ordering from Linux.  The
     controller's functional clocks must be ungated before programming the
     timing registers. MdpResources owns the PHY and configures byte/pixel
     source rates through the platform clock provider. */
  Ctrl = MmioRead32 ((UINTN)(DsiRegs + DSI_CTRL));
  if (Ctrl & DSI_CTRL_ENABLE) {
    MmioWrite32 ((UINTN)(DsiRegs + DSI_CTRL), Ctrl & ~DSI_CTRL_ENABLE);
    MemoryFence ();
  }
  MmioWrite32 ((UINTN)(DsiRegs + DSI_CLK_CTRL),
               MDP_BIT (0) | MDP_BIT (1) | MDP_BIT (2) |
               MDP_BIT (3) | MDP_BIT (4) | MDP_BIT (5) |
               MDP_BIT (9));
  MemoryFence ();
  MmioWrite32 ((UINTN)(DsiRegs + DSI_RESET), 1);
  MicroSecondDelay (20000);
  MmioWrite32 ((UINTN)(DsiRegs + DSI_RESET), 0);
  MemoryFence ();
  DEBUG ((DEBUG_INIT, "MDSSCrDxe: DSI reset complete; program host timing\n"));
  /* Linux dsi_host video timing registers. PHY and source-clock setup was
     completed by MdpResources before entering this host configuration. */
  {
    UINT32 HTotal = Timing->HSync + Timing->HBackPorch + Timing->Width + Timing->HFrontPorch;
    UINT32 VTotal = Timing->VSync + Timing->VBackPorch + Timing->Height + Timing->VFrontPorch;
    UINT32 HaStart = Timing->HSync + Timing->HBackPorch;
    UINT32 VaStart = Timing->VSync + Timing->VBackPorch;
    MmioWrite32 ((UINTN)(DsiRegs + DSI_ACTIVE_H), (HaStart & 0xffff) | ((HaStart + Timing->Width) << 16));
    MmioWrite32 ((UINTN)(DsiRegs + DSI_ACTIVE_V), (VaStart & 0xffff) | ((VaStart + Timing->Height) << 16));
    MmioWrite32 ((UINTN)(DsiRegs + DSI_TOTAL), ((HTotal - 1) & 0xffff) | ((VTotal - 1) << 16));
    MmioWrite32 ((UINTN)(DsiRegs + DSI_ACTIVE_HSYNC), 0 | (Timing->HSync << 16));
    MmioWrite32 ((UINTN)(DsiRegs + DSI_ACTIVE_VSYNC_HPOS), 0);
    /* DSI ACTIVE_VSYNC_VPOS is relative to the frame start.  Linux uses
       vs_start=0 and vs_end=vsync pulse width; VaStart is the active window
       origin and belongs only in ACTIVE_V. */
    MmioWrite32 ((UINTN)(DsiRegs + DSI_ACTIVE_VSYNC_VPOS),
                 (Timing->VSync << 16));
    /* Qualcomm XML uses 36 for packed RGB888; Linux's DSI register enum
       uses 3 for VID_DST_FORMAT_RGB888. */
    UINT32 DsiFormat = (Timing->DsiColorFormat == 36 || Timing->DsiColorFormat == 24) ? 3 : Timing->DsiColorFormat;
    MmioWrite32 ((UINTN)(DsiRegs + DSI_VID_CFG0),
                 DSI_VID_CFG0_VC (Timing->DsiVirtualChannel) |
                 DSI_VID_CFG0_FORMAT (DsiFormat) |
                 DSI_VID_CFG0_TRAFFIC (Timing->DsiTrafficMode) |
                 (mDisplay.Panel.HsaHseAfterVsVe ? MDP_BIT (28) : 0) |
                 (Timing->DsiVideoFlags & (MDP_DSI_VIDEO_BLLP_LP |
                                           MDP_DSI_VIDEO_EOF_BLLP_LP |
                                           MDP_DSI_VIDEO_HSA_LP |
                                           MDP_DSI_VIDEO_HBP_LP |
                                           MDP_DSI_VIDEO_HFP_LP)));
  }
  /* Linux dsi_ctrl_enable(): RGB order, identity data-lane mapping and
     explicit EoT policy must not depend on reset/previous-boot values. */
  MmioWrite32 ((UINTN)(DsiRegs + DSI_VID_CFG1), 0);
  /* Share the actual PHY timing calculation, as Linux dsi_ctrl_enable does.
     v4 timings use no legacy double-byte-clock pre extension. */
  MmioWrite32 ((UINTN)(DsiRegs + DSI_CLKOUT_TIMING_CTRL), HostClockTiming);
  MmioWrite32 ((UINTN)(DsiRegs + DSI_T_CLK_PRE_EXTEND), 0);
  MmioWrite32 ((UINTN)(DsiRegs + DSI_LANE_SWAP_CTRL), 0);
  MmioWrite32 ((UINTN)(DsiRegs + DSI_EOT_PACKET_CTRL),
               mDisplay.Panel.DisableEot ? 0 : MDP_BIT (0));
  /* The 5nm PHY owns continuous clock generation (LANE_CTRL1[6:5]).
     Match Linux's host request while clearing HS_REQ_SEL_PHY. */
  MmioWrite32 ((UINTN)(DsiRegs + DSI_LANE_CTRL),
               (MmioRead32 ((UINTN)(DsiRegs + DSI_LANE_CTRL)) & ~MDP_BIT (24)) |
               MDP_BIT (28));
  Ctrl = DSI_CTRL_CLK_EN | DSI_CTRL_ENABLE |
         ((Timing->DsiLanes >= 1) ? DSI_CTRL_LANE0 : 0) |
         ((Timing->DsiLanes >= 2) ? MDP_BIT (5) : 0) |
         ((Timing->DsiLanes >= 3) ? MDP_BIT (6) : 0) |
         ((Timing->DsiLanes >= 4) ? MDP_BIT (7) : 0);
  MmioWrite32 ((UINTN)(DsiRegs + DSI_CTRL), Ctrl);
  MemoryFence ();
  mDisplay.HostConfigured = TRUE;
  /* Linux powers/configures the host before panel preparation, and enables
     video afterwards. Init commands must not compete with active scanout. */
  if (!mDisplay.PanelCommandsStarted &&
      (mDisplay.Panel.InitSequence.Count != 0 ||
       mDisplay.Panel.TermSequence.Count != 0)) {
    mDisplay.PanelCommandsStarted = TRUE;
    Status = RunPanelSequence (&mDisplay.Panel.InitSequence);
    if (EFI_ERROR (Status)) return Status;
  }
  MmioWrite32 ((UINTN)(DsiRegs + DSI_CTRL), Ctrl | DSI_CTRL_VID_MODE_EN);
  DEBUG ((DEBUG_INIT, "MDSSCrDxe: DSI video enabled; clean framebuffer 0x%lx\n", Fb));
  /* The DPU is a bus master.  Ensure GOP writes are visible before the
     timing engine is started; the reserved framebuffer is normally WB cache. */
  WriteBackDataCacheRange ((VOID *)(UINTN)Fb, (UINTN)Stride * Timing->Height);
  DpuWriteTiming (&mDisplay.Panel, mDisplay.DeviceFrameBuffer, Stride, FetchStart);
  mDisplay.ScanoutActive = TRUE;
  /* Retain ownership on timeout: cleanup must stop/fence the bus master. */
  DEBUG ((DEBUG_INIT, "MDSSCrDxe: scanout started; wait for frame/flush progress\n"));
  Status = MdpWaitScanoutProgress (Timing->DsiController);
  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INIT,
          "MDSSCrDxe: scanout progress: %r\n", Status));
  return Status;
}

/* Display idle stops the real output, not just its pixels. Keep the original
   clocks, PHY, bridge, IOVA and GOP storage owned: a wake resumes exactly the
   same mode without allocating/freeing or replaying panel preparation. HDK's
   LT9611 output loses DSI video; an external monitor owns its own backlight. */
STATIC EFI_STATUS EFIAPI DisplayPowerSet(EFI_DISPLAY_POWER_PROTOCOL *This,
                                        EFI_DISPLAY_POWER_STATE State)
{
  EFI_STATUS S;
  if (This!=&mDisplay.Power ||
      (State!=EfiDisplayPowerStateOn && State!=EfiDisplayPowerStateOff))
    return EFI_INVALID_PARAMETER;
  EFI_TPL Caller=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(Caller);
  if(Caller>TPL_CALLBACK)return EFI_UNSUPPORTED;
  S=BeginDisplay();if(S!=EFI_SUCCESS)return S;
  if (!mDisplay.GopPublished || !mDisplay.ResourcesReady || !mDisplay.HostConfigured)
    return EndDisplay(EFI_NOT_READY);
  /* A partial stop/start must first reach a verified off state, including
     retries of a failed On. Software ScanoutActive alone is not that proof. */
  if (mDisplay.ScanoutActive || mDisplay.PowerPending) {
    if (State==EfiDisplayPowerStateOn && !mDisplay.PowerPending)
      return EndDisplay(EFI_SUCCESS);
    mDisplay.PowerPending=TRUE;
    S=MdpStopScanout(mDisplay.Panel.Timing.DsiController);
    if(S!=EFI_SUCCESS)return EndDisplay(EFI_ERROR(S)?S:EFI_NOT_READY);
    mDisplay.ScanoutActive=FALSE;mDisplay.PowerPending=FALSE;
  }
  if(State==EfiDisplayPowerStateOn) {
    mDisplay.PowerPending=TRUE;
    S=ConfigureVideoInternal(&mDisplay.Dsi,&mDisplay.Panel.Timing,
                            mDisplay.FrameBuffer,mDisplay.FrameBufferStride);
    if(S!=EFI_SUCCESS)return EndDisplay(EFI_ERROR(S)?S:EFI_NOT_READY);
    mDisplay.PowerPending=FALSE;
  }
  DEBUG((DEBUG_WARN,"MDSSCrDxe: display output %a; framebuffer and leases retained\n",
         State==EfiDisplayPowerStateOn?"on":"off"));
  return EndDisplay(EFI_SUCCESS);
}
STATIC EFI_STATUS EFIAPI DisplayPowerGet(EFI_DISPLAY_POWER_PROTOCOL *This,
                                        EFI_DISPLAY_POWER_STATE *State)
{
  EFI_STATUS S;
  if(This!=&mDisplay.Power || !State)return EFI_INVALID_PARAMETER;
  S=BeginDisplay();if(S!=EFI_SUCCESS)return S;
  if(!mDisplay.GopPublished || !mDisplay.ResourcesReady || mDisplay.PowerPending)
    return EndDisplay(EFI_NOT_READY);
  *State=mDisplay.ScanoutActive?EfiDisplayPowerStateOn:EfiDisplayPowerStateOff;
  return EndDisplay(EFI_SUCCESS);
}

STATIC EFI_STATUS EFIAPI GetFramebuffer (IN EFI_MDP_DSI_PROTOCOL *This, OUT EFI_PHYSICAL_ADDRESS *Fb, OUT UINT32 *Width, OUT UINT32 *Height, OUT UINT32 *Stride)
{
  MDP_DISPLAY *Display = &mDisplay;
  EFI_STATUS Status;
  if (This != &mDisplay.Dsi || Fb == NULL || Width == NULL || Height == NULL || Stride == NULL) return EFI_INVALID_PARAMETER;
  Status=BeginDisplay(); if (Status!=EFI_SUCCESS) return Status;
  *Fb = Display->FrameBuffer; *Width = Display->Panel.Timing.Width; *Height = Display->Panel.Timing.Height; *Stride = Display->FrameBufferStride;
  return EndDisplay(EFI_SUCCESS);
}

STATIC EFI_STATUS EFIAPI ConfigureVideo (EFI_MDP_DSI_PROTOCOL *This, CONST EFI_MDP_DSI_TIMING *Timing, EFI_PHYSICAL_ADDRESS Fb, UINT32 Stride)
{
  EFI_STATUS Status;
  if (This != &mDisplay.Dsi) return EFI_INVALID_PARAMETER;
  Status=BeginDisplay(); if (Status!=EFI_SUCCESS) return Status;
  if(mDisplay.PowerPending)return EndDisplay(EFI_NOT_READY);
  return EndDisplay(ConfigureVideoInternal(This,Timing,Fb,Stride));
}

STATIC EFI_STATUS EFIAPI GopQueryMode (IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This, IN UINT32 Mode, OUT UINTN *Size, OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info)
{
  EFI_STATUS Status;
  if (This!=&mDisplay.Gop || Mode!=0 || !Size || !Info) return EFI_INVALID_PARAMETER;
  Status=BeginDisplay(); if (Status!=EFI_SUCCESS) return Status;
  *Info=AllocateCopyPool(sizeof(mDisplay.Info),&mDisplay.Info);
  if (!*Info) return EndDisplay(EFI_OUT_OF_RESOURCES);
  *Size=sizeof(mDisplay.Info); return EndDisplay(EFI_SUCCESS);
}
STATIC EFI_STATUS EFIAPI GopSetMode (IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This, IN UINT32 Mode)
{
  MDP_DISPLAY *D;
  EFI_TPL Tpl;
  EFI_STATUS Status;

  if (This != &mDisplay.Gop) return EFI_INVALID_PARAMETER;
  if (Mode != 0) return EFI_UNSUPPORTED;
  D = BASE_CR (This, MDP_DISPLAY, Gop);
  if (D->Gop.Mode == NULL) return EFI_NOT_READY;
  Status=BeginDisplay(); if (Status!=EFI_SUCCESS) return Status;
  Tpl = gBS->RaiseTPL (TPL_NOTIFY);
  Status = MdpClearFrameBuffer ((VOID *)(UINTN)D->FrameBuffer,
                               D->Gop.Mode->FrameBufferSize,
                               D->Panel.Timing.Width,
                               D->Panel.Timing.Height,
                               D->FrameBufferStride);
  if (!EFI_ERROR (Status)) D->Gop.Mode->Mode = 0;
  gBS->RestoreTPL (Tpl);
  return EndDisplay(Status);
}
STATIC EFI_STATUS EFIAPI GopBlt (IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This, IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Blt, IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION Op, IN UINTN Sx, IN UINTN Sy, IN UINTN Dx, IN UINTN Dy, IN UINTN W, IN UINTN H, IN UINTN Delta OPTIONAL)
{
  EFI_STATUS Status; RETURN_STATUS St;
  if (This!=&mDisplay.Gop) return EFI_INVALID_PARAMETER;
  Status=BeginDisplay(); if (Status!=EFI_SUCCESS) return Status;
  St=FrameBufferBlt(mDisplay.BltConfig,Blt,Op,Sx,Sy,Dx,Dy,W,H,Delta);
  if (!RETURN_ERROR(St)) WriteBackDataCacheRange((VOID*)(UINTN)mDisplay.FrameBuffer,(UINTN)mDisplay.FrameBufferStride*mDisplay.Panel.Timing.Height);
  return EndDisplay(RETURN_ERROR(St)?EFI_INVALID_PARAMETER:EFI_SUCCESS);
}

STATIC EFI_STATUS
ReleaseDisplay (VOID)
{
  EFI_STATUS Status;
  EFI_STATUS TermStatus = EFI_SUCCESS;
  UINTN Dsi;

  if (mDisplay.ScanoutActive) {
    Status = MdpStopScanout (mDisplay.Panel.Timing.DsiController);
    if (EFI_ERROR (Status)) return Status;
    mDisplay.ScanoutActive = FALSE;
  }
  if (mDisplay.HostConfigured) {
    Status = MdpDsiCommandQuiesce (mDisplay.Panel.Timing.DsiController);
    if (EFI_ERROR (Status)) return Status;
    if (mDisplay.PanelCommandsStarted) {
      /* Linux disables/resets the video engine before sending off commands.
         Do not replay a partially executed off sequence on an unload retry. */
      mDisplay.PanelCommandsStarted = FALSE;
      TermStatus = RunPanelSequence (&mDisplay.Panel.TermSequence);
      if (EFI_ERROR (TermStatus)) {
        DEBUG ((DEBUG_ERROR, "MDSSCrDxe: panel termination failed: %r\n", TermStatus));
      }
      Status = MdpDsiCommandQuiesce (mDisplay.Panel.Timing.DsiController);
      if (EFI_ERROR (Status)) return Status;
    }
    Dsi = (UINTN)((mDisplay.Panel.Timing.DsiController == 1) ? DSI1_BASE : DSI0_BASE) + DSI_6G_REG_SHIFT;
    MmioWrite32 (Dsi + DSI_CTRL, MmioRead32 (Dsi + DSI_CTRL) & ~(BIT0 | BIT1 | BIT2));
    MemoryFence ();
    if ((MmioRead32 (Dsi + DSI_CTRL) & (BIT0 | BIT1 | BIT2)) != 0) return EFI_DEVICE_ERROR;
    mDisplay.HostConfigured = FALSE;
  }
  if (mDisplay.ResourcesReady) {
    Status = MdpReleaseResources ();
    if (EFI_ERROR (Status)) return Status;
    mDisplay.ResourcesReady = FALSE;
    mDisplay.DeviceFrameBuffer = 0;
  }
  if (mDisplay.BltConfig != NULL) {
    FreePool (mDisplay.BltConfig);
    mDisplay.BltConfig = NULL;
  }
  if (mDisplay.Gop.Mode != NULL) {
    FreePool (mDisplay.Gop.Mode);
    mDisplay.Gop.Mode = NULL;
  }
  MdpFreePanelConfig (&mDisplay.Panel);
  return TermStatus;
}

EFI_STATUS EFIAPI
MDSSCrDxeUnloadInternal (IN EFI_HANDLE ImageHandle)
{
  EFI_STATUS Status;
  (VOID)ImageHandle;

  /* Refuse unload while a console driver still owns these protocols. Do not
     stop its display before UninstallMultiple has accepted the disconnect.
     The prepare stage publishes only MDP/DSI; the final stage adds GOP. */
  if (mDisplay.Handle != NULL) {
    if (mDisplay.GopPublished) {
      Status = gBS->UninstallMultipleProtocolInterfaces (
                      mDisplay.Handle,
                      &gEfiDevicePathProtocolGuid, &mDisplayDevicePath,
                      &gEfiGraphicsOutputProtocolGuid, &mDisplay.Gop,
                      &gEfiDisplayPowerStateProtocolGuid, &mDisplay.Power,
                      &gEfiMdpDsiProtocolGuid, &mDisplay.Dsi,
                      NULL);
    } else {
      Status = gBS->UninstallMultipleProtocolInterfaces (
                      mDisplay.Handle,
                      &gEfiMdpDsiProtocolGuid, &mDisplay.Dsi,
                      NULL);
    }
    if (EFI_ERROR (Status)) return Status;
    mDisplay.Handle = NULL;
    mDisplay.GopPublished = FALSE;
  }
  /* ExitBootServices deliberately does not call this: an OS bootloader
     continues to use the GOP framebuffer after boot-services exit. */
  Status=ReleaseDisplay();
  return Status==EFI_SUCCESS ? UnregisterHandoff() : Status;
}

EFI_STATUS EFIAPI MDSSCrDxeUnload (EFI_HANDLE ImageHandle)
{
  if (FixedPcdGet32(PcdMdssPanelProfile)) return MdpSelectedPanelUnload();
  if (FixedPcdGetBool(PcdMdssHdkExtendedProfileEnable)) return MdpHdkExtendedProfileUnload();
  EFI_STATUS Status=BeginDisplay(); if (Status!=EFI_SUCCESS) return Status;
  return EndDisplay(MDSSCrDxeUnloadInternal(ImageHandle));
}

/* BridgeReady is called after the prepare protocol has been published.  A
   failed final handoff must revoke that protocol together with any DSI/DPU
   ownership before returning the error.  If the hardware cannot be fenced,
   retain the image and ownership so a later explicit unload can retry. */
STATIC EFI_STATUS
MdpBridgeFailure (IN EFI_STATUS Failure)
{
  EFI_STATUS Cleanup;
  Cleanup = ReleaseDisplay ();
  if (!EFI_ERROR (Cleanup) && mDisplay.Handle != NULL &&
      !mDisplay.GopPublished) {
    Cleanup = gBS->UninstallMultipleProtocolInterfaces (
                 mDisplay.Handle, &gEfiMdpDsiProtocolGuid, &mDisplay.Dsi,
                 NULL);
    if (!EFI_ERROR (Cleanup)) {
      mDisplay.Handle = NULL;
    }
  }
  if (EFI_ERROR (Cleanup)) {
    DEBUG ((DEBUG_ERROR,
            "MDSSCrDxe: bridge handoff %r; cleanup retained: %r\n",
            Failure, Cleanup));
    return EFI_SUCCESS;
  }
  return Failure;
}

/*
 * Complete the second half of the display handoff.  MDSS deliberately does
 * not consume the LT9611 protocol in its DEPEX: it prepares clocks, PHY,
 * framebuffer and DMA first.  LT9611 calls this method after publishing its
 * bridge protocol; this callback then performs bridge configure, DSI host
 * setup, scanout start, and final GOP publication.
 */
STATIC EFI_STATUS EFIAPI
MdpBridgeReadyInternal (IN EFI_MDP_DSI_PROTOCOL *This)
{
  EFI_STATUS              Status;
  EFI_LT9611_CR_PROTOCOL *Bridge;
  EFI_MDP_DSI_TIMING      Timing;

  if (This != &mDisplay.Dsi) {
    return EFI_INVALID_PARAMETER;
  }
  if (mDisplay.Handle == NULL || !mDisplay.ResourcesReady) {
    return EFI_NOT_READY;
  }
  if (mDisplay.GopPublished) {
    return EFI_ALREADY_STARTED;
  }
  if (mDisplay.ScanoutActive) {
    return EFI_NOT_READY;
  }

  Bridge = NULL;
  Status = gBS->LocateProtocol (
                   &gEfiLt9611CrProtocolGuid, NULL, (VOID **)&Bridge);
  if (EFI_ERROR (Status) || Bridge == NULL || Bridge->ConfigureVideo == NULL) {
    DEBUG ((DEBUG_ERROR, "MDSSCrDxe: bridge notify without LT9611: %r\n",
            EFI_NOT_FOUND));
    return MdpBridgeFailure (EFI_NOT_READY);
  }

  CopyMem (&Timing, &mDisplay.Panel.Timing, sizeof (Timing));
  DEBUG ((DEBUG_INIT,
          "MDSSCrDxe: bridge notify; configuring LT9611 before DSI scanout\n"));
  Status = Bridge->ConfigureVideo (Bridge, &Timing);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MDSSCrDxe: LT9611 bridge configure failed: %r\n",
            Status));
    return MdpBridgeFailure (Status);
  }
  DEBUG ((DEBUG_INIT, "MDSSCrDxe: LT9611 timing ready; configure DSI/DPU\n"));
  Status = ConfigureVideoInternal (
             &mDisplay.Dsi, &Timing, mDisplay.FrameBuffer,
             mDisplay.FrameBufferStride);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MDSSCrDxe: DSI video handoff failed: %r\n", Status));
    return MdpBridgeFailure (Status);
  }
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mDisplay.Handle,
                  &gEfiDevicePathProtocolGuid, &mDisplayDevicePath,
                  &gEfiGraphicsOutputProtocolGuid, &mDisplay.Gop,
                      &gEfiDisplayPowerStateProtocolGuid, &mDisplay.Power,
                  NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
            "MDSSCrDxe: GOP publication after bridge notify failed: %r\n",
            Status));
    return MdpBridgeFailure (Status);
  }
  mDisplay.GopPublished = TRUE;
  DEBUG ((DEBUG_INIT,
          "MDSSCrDxe: bridge notify complete; GOP published %ux%u\n",
          mDisplay.Panel.Timing.Width, mDisplay.Panel.Timing.Height));
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI MdpBridgeReady (EFI_MDP_DSI_PROTOCOL *This)
{
  EFI_STATUS Status=BeginDisplay(); if (Status!=EFI_SUCCESS) return Status;
  return EndDisplay(MdpBridgeReadyInternal(This));
}

EFI_STATUS EFIAPI MDSSCrDxeEntryPoint (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  EFI_STATUS CleanupStatus;
  ARM_MEMORY_REGION_DESCRIPTOR_EX Region;
  UINT32 Size;
  EFI_MDP_DSI_TIMING Timing;
  (VOID)ImageHandle; (VOID)SystemTable;
  if (FixedPcdGet32(PcdMdssPanelProfile)) {
    Status=MdpSelectedPanelStart(FixedPcdGet32(PcdMdssPanelProfile));
    if(Status!=EFI_SUCCESS&&Status!=EFI_NOT_READY)DEBUG((DEBUG_ERROR,"MDSSCr: selected native panel startup %r retained=%u\n",Status,MdpSelectedPanelRetained()));
    return MdpSelectedPanelRetained()?EFI_SUCCESS:Status;
  }
  if (FixedPcdGetBool(PcdMdssHdkExtendedProfileEnable)) {
    Status = MdpHdkExtendedProfileStart();
    if (Status != EFI_SUCCESS) DEBUG((DEBUG_ERROR,"MDSSCr: selected profile startup %r retained=%u\n",
                                    Status,MdpHdkExtendedProfileRetained()));
    /* Keep actual partial event/protocol ownership resident on an error. */
    return MdpHdkExtendedProfileRetained() ? EFI_SUCCESS : Status;
  }
  if (mHandoffRegistered || mDisplay.Handle != NULL || mDisplay.ResourcesReady || mDisplay.HostConfigured) {
    return EFI_ALREADY_STARTED;
  }
  ZeroMem (&mDisplay, sizeof (mDisplay));
  if (FixedPcdGetBool(PcdMdssHandoffEnable)) {
    Status=gBS->LocateProtocol(&gEfiMdssHandoffProtocolGuid,NULL,(VOID**)&mHandoff);
    if (Status!=EFI_SUCCESS) return EFI_ERROR(Status)?Status:EFI_NOT_READY;
    if (!mHandoff || mHandoff->Revision<EFI_MDSS_HANDOFF_PROTOCOL_REVISION || !mHandoff->SetParticipant) return EFI_UNSUPPORTED;
    Status=mHandoff->SetParticipant(mHandoff,CR_MDSS_NATIVE_DSI,&mDisplay,FreezeDisplay,SnapshotDisplay);
    if (Status!=EFI_SUCCESS) return EFI_ERROR(Status)?Status:EFI_NOT_READY;
    mHandoffRegistered=TRUE;
  }
  Status = LoadPanelXml (&mDisplay.Panel);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MDSSCrDxe: unsupported or malformed panel XML: %r\n", Status));
    goto Fail;
  }
  Status = LocateMemoryMapAreaByName ("Display Reserved", &Region);
  if (EFI_ERROR (Status)) goto Fail;
  if (mDisplay.Panel.Timing.Width > (MAX_UINT32 / 4) ||
      ((UINT64)mDisplay.Panel.Timing.Width * mDisplay.Panel.Timing.Height * 4) > Region.Length) {
    DEBUG ((DEBUG_ERROR, "MDSSCrDxe: panel %ux%u does not fit Display Reserved (%lu bytes)\n",
            mDisplay.Panel.Timing.Width, mDisplay.Panel.Timing.Height, Region.Length));
    Status = EFI_BAD_BUFFER_SIZE;
    goto Fail;
  }
  mDisplay.FrameBuffer = Region.Address;
  Size = mDisplay.Panel.Timing.Width * 4;
  mDisplay.FrameBufferStride = Size;
  mDisplay.Info.Version = 0;
  mDisplay.Info.HorizontalResolution = mDisplay.Panel.Timing.Width;
  mDisplay.Info.VerticalResolution = mDisplay.Panel.Timing.Height;
  mDisplay.Info.PixelFormat = PixelBlueGreenRedReserved8BitPerColor;
  mDisplay.Info.PixelsPerScanLine = mDisplay.Panel.Timing.Width;
  mDisplay.Power.Revision=EFI_DISPLAY_POWER_PROTOCOL_REVISION;
  mDisplay.Power.SetDisplayPowerState=DisplayPowerSet;
  mDisplay.Power.GetDisplayPowerState=DisplayPowerGet;
  mDisplay.Gop.QueryMode = GopQueryMode; mDisplay.Gop.SetMode = GopSetMode; mDisplay.Gop.Blt = GopBlt;
  mDisplay.Gop.Mode = AllocateZeroPool (sizeof (*mDisplay.Gop.Mode));
  if (mDisplay.Gop.Mode == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Fail; }
  mDisplay.Gop.Mode->MaxMode = 1; mDisplay.Gop.Mode->Mode = 0; mDisplay.Gop.Mode->Info = &mDisplay.Info;
  mDisplay.Gop.Mode->SizeOfInfo = sizeof (mDisplay.Info); mDisplay.Gop.Mode->FrameBufferBase = mDisplay.FrameBuffer;
  mDisplay.Gop.Mode->FrameBufferSize = (UINTN)mDisplay.FrameBufferStride * mDisplay.Panel.Timing.Height;
  Status = FrameBufferBltConfigure ((VOID *)(UINTN)mDisplay.FrameBuffer, &mDisplay.Info, NULL, &mDisplay.BltConfigSize);
  if (Status == RETURN_BUFFER_TOO_SMALL) { mDisplay.BltConfig = AllocatePool (mDisplay.BltConfigSize); if (mDisplay.BltConfig == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Fail; } Status = FrameBufferBltConfigure ((VOID *)(UINTN)mDisplay.FrameBuffer, &mDisplay.Info, mDisplay.BltConfig, &mDisplay.BltConfigSize); }
  if (RETURN_ERROR (Status)) { Status = EFI_DEVICE_ERROR; goto Fail; }
  Status = GopSetMode (&mDisplay.Gop, 0);
  if (EFI_ERROR (Status)) goto Fail;
  mDisplay.Dsi.BridgeReady = MdpBridgeReady;
  mDisplay.Dsi.ConfigureVideo = ConfigureVideo;
  mDisplay.Dsi.GetFramebuffer = GetFramebuffer;
  CopyMem (&Timing, &mDisplay.Panel.Timing, sizeof (Timing));
  /* Initialization can itself retain a partially acquired resource. Keep
     the owner resident if its rollback cannot yet revoke that ownership. */
  mDisplay.ResourcesReady = TRUE;
  Status = MdpInitializeResources (&Timing);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }
  Status = MdpMapFrameBuffer (
             mDisplay.FrameBuffer,
             mDisplay.Gop.Mode->FrameBufferSize,
             &mDisplay.DeviceFrameBuffer
             );
  if (EFI_ERROR (Status)) {
    goto Fail;
  }
  DEBUG ((DEBUG_INFO,
          "MDSSCrDxe: prepare panel %ux%u DSI controller %u lanes=%u\n",
          Timing.Width, Timing.Height, Timing.DsiController, Timing.DsiLanes));
  /* Publish only the prepared DSI protocol.  LT9611 consumes it, publishes
     its bridge, and calls BridgeReady to perform the final DSI/scanout/GOP
     stage. */
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mDisplay.Handle,
                  &gEfiMdpDsiProtocolGuid, &mDisplay.Dsi,
                  NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
            "MDSSCrDxe: prepared DSI protocol publication failed: %r\n",
            Status));
    goto Fail;
  }
  DEBUG ((DEBUG_INFO,
          "MDSSCrDxe: prepare complete; waiting for LT9611 bridge\n"));
  return Status;

Fail:
  mDisplay.Handle = NULL;
  CleanupStatus = ReleaseDisplay ();
  if (EFI_ERROR (CleanupStatus)) {
    DEBUG ((DEBUG_ERROR, "MDSSCrDxe: initialization %r; cleanup %r\n", Status, CleanupStatus));
    if (mDisplay.ResourcesReady || mDisplay.HostConfigured) {
      /* A failing DXE entry is unloaded by the core. Keep the image and its
         DMA ownership state alive for an explicit unload/cleanup retry. */
      DEBUG ((DEBUG_ERROR, "MDSSCrDxe: retaining image and DMA resources; GOP unavailable\n"));
      return EFI_SUCCESS;
    }
  }
  CleanupStatus=UnregisterHandoff();
  if (CleanupStatus!=EFI_SUCCESS) return EFI_SUCCESS;
  return Status;
}
