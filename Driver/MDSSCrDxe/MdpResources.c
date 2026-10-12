/** @file
  SM8450 MDSS DSI0 resource and 5nm DPHY initialization.

  PHY programming and timing arithmetic are adapted from Linux
  drivers/gpu/drm/msm/dsi/phy/dsi_phy_7nm.c and dsi_phy.c. Register offsets:
  drivers/gpu/drm/msm/registers/display/dsi_phy_7nm.xml. Clock topology and
  supplies: drivers/clk/qcom/dispcc-sm8450.c and sm8450{,-hdk}.dts[i].
  Copyright (c) 2015, 2018, The Linux Foundation.
  SPDX-License-Identifier: GPL-2.0-only

  Shared Cr providers retain ownership of their PLL, MMCX, RPMh and
  interconnect votes. This driver owns only DSI0 PHY/PLL and its DMA mapping.
  No stock executable is used as an implementation reference.
**/
#include "MdpResources.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/EFIClockCrProtocol.h>
#include <Protocol/EFIRpmhCrProtocol.h>
#include <Protocol/EFIInterconnectCrProtocol.h>
#include <Protocol/EFISmmuCrProtocol.h>
#include <Protocol/IoMmu.h>

#define DISP_CC_BASE  0x0AF00000U
typedef struct {
  UINTN Phy, Lane, Pll;
  CONST CHAR8 *EscapeClock, *ByteClock, *ByteInterfaceClock, *PixelClock;
  UINT32 ByteSource, PixelSource;
  UINT32 ByteConfig, PixelConfig, ByteDivider;
} MDP_DSI_RESOURCE_TARGET;
/* Registers: public Linux sm8450.dtsi; parent muxes: public
   dispcc-sm8450.c map_2. Supplies for both hosts are described by the public
   Android Waipio device trees (waipio-sde.dtsi and
   waipio-sde-common.dtsi; L6B/1200mV and L5B/880mV). The URLs and revision
   are recorded in Documentation/WaipioDisplaySourceReview.md. No local
   snapshot, firmware image, or OEM executable is an implementation source. */
STATIC CONST MDP_DSI_RESOURCE_TARGET mDsiTargets[] = {
  { 0x0AE94400U, 0x0AE94600U, 0x0AE94900U,
    "disp_cc_mdss_esc0_clk", "disp_cc_mdss_byte0_clk",
    "disp_cc_mdss_byte0_intf_clk", "disp_cc_mdss_pclk0_clk",
    2, 1, 0x8138, 0x80C0, 0x814C },
  { 0x0AE96400U, 0x0AE96600U, 0x0AE96900U,
    "disp_cc_mdss_esc1_clk", "disp_cc_mdss_byte1_clk",
    "disp_cc_mdss_byte1_intf_clk", "disp_cc_mdss_pclk1_clk",
    4, 3, 0x8154, 0x80D8, 0x8168 },
};
STATIC UINT32 mActiveController = MAX_UINT32;
/* Default address is used only by isolated PHY host tests. Production
   selects ownership before any resource access and retains it on failure. */
STATIC CONST MDP_DSI_RESOURCE_TARGET *mDsiTarget = &mDsiTargets[0];
#define PHY_BASE  (mDsiTarget->Phy)
#define LANE_BASE (mDsiTarget->Lane)
#define PLL_BASE  (mDsiTarget->Pll)
#define SMMU_BASE     0x15000000U

/* Offsets within the DPHY common, lane and PLL windows, respectively. */
#define PHY_CLK_CFG0        0x010
#define PHY_CLK_CFG1        0x014
#define PHY_RBUF_CTRL       0x01C
#define PHY_VREG_CTRL0      0x020
#define PHY_CTRL0           0x024
#define PHY_CTRL2           0x02C
#define PHY_CTRL3           0x030
#define PHY_LANE_CFG0       0x034
#define PHY_LANE_CFG1       0x038
#define PHY_PLL_CTRL        0x03C
#define PHY_LANE_CTRL0      0x0A0
#define PHY_LANE_CTRL1      0x0A4
#define PHY_TIMING0         0x0B4
#define PHY_HSTX_STR        0x0EC
#define PHY_RESCODE_TOP     0x0F4
#define PHY_RESCODE_BOTTOM  0x0F8
#define PHY_LPTX_STR        0x100
#define PHY_PEMPH_CTRL      0x104
#define PHY_CAL_SEL         0x10C
#define PHY_VREG_CTRL1      0x110
#define PHY_CTRL4           0x114
#define PHY_DIG_RESET       0x128
#define PHY_STATUS         0x140
#define PHY_REFGEN_VOTE     0x1AC
#define PLL_SYSTEM_MUXES    0x028

typedef struct {
  CONST CHAR8 *Controller;
  CONST CHAR8 *Name;
  UINT32 Rate;
  BOOLEAN Enabled;
} MDP_CLOCK;

/* Enable order follows the Linux MDSS/DSI runtime-PM dependencies. Rates
   are selected by the native provider, including its MMCX performance vote. */
STATIC MDP_CLOCK mClocks[] = {
  { "gcc", "gcc_disp_ahb_clk", 0, FALSE },
  { "gcc", "gcc_disp_xo_clk", 0, FALSE },
  /* Mainline SM8450's MDSS node requests the GDSC-gated AHB branch.  The
     non-GDSC branch is not in the DTS clock list; requesting both leaves
     would create two independent rate votes on their shared AHB RCG. */
  { "dispcc", "disp_cc_mdss_ahb_clk", 19200000, FALSE },
  { "dispcc", "disp_cc_mdss_mdp_clk", 200000000, FALSE },
  { "gcc", "gcc_disp_hf_axi_clk", 0, FALSE },
  { "gcc", "gcc_disp_sf_axi_clk", 0, FALSE },
  /* mdss_mdp_lut_clk shares disp_cc_mdss_mdp_clk_src with mdss_mdp_clk. */
  { "dispcc", "disp_cc_mdss_mdp_lut_clk", 200000000, FALSE },
  { "dispcc", "disp_cc_mdss_vsync_clk", 19200000, FALSE },
  { "dispcc", "disp_cc_mdss_esc0_clk", 19200000, FALSE },
  { "dispcc", "disp_cc_mdss_byte0_clk", 0, FALSE },
  { "dispcc", "disp_cc_mdss_byte0_intf_clk", 0, FALSE },
  { "dispcc", "disp_cc_mdss_pclk0_clk", 0, FALSE },
};
#define ACCESS_CLOCK_COUNT  2U
#define CORE_CLOCK_COUNT  9U
#define BYTE_CLOCK       9U
#define BYTE_INTF_CLOCK  10U
#define PIXEL_CLOCK      11U

typedef struct {
  CONST CHAR8 *Name;
  UINT32 Millivolts;
  BOOLEAN Requested;
} MDP_RAIL_VOTE;

/* Waipio Android explicitly wires both DSI hosts to L6B/1200 mV and
   both PHYs to L5B/880 mV; Linux HDK8450 confirms the DSI0 assignments.
   The RPMh provider owns the shared rail references with PCIe. */
STATIC MDP_RAIL_VOTE mRailVotes[] = {
  { "ldob6", 1200, FALSE },
  { "ldob5", 880, FALSE },
};

/* Target route aliases; the native ICB ABI belongs to CrInterconnectTarget. */
typedef struct { UINT32 Source; UINT32 Destination; } MDP_ICB_PATH;
STATIC CONST MDP_ICB_PATH mIcbPaths[] = {
  { 0x4002, 0x4003 }, /* APPSS -> display configuration */
  { 0x4000, 0x4001 }, /* MDP -> memory */
};
STATIC INTERCONNECT_PATH_HANDLE mIcbHandles[ARRAY_SIZE (mIcbPaths)];
STATIC UINT64 mIcbAverage[ARRAY_SIZE (mIcbPaths)], mIcbPeak[ARRAY_SIZE (mIcbPaths)];
STATIC UINT32 mByteRate, mByteSource, mPixelSource, mByteDivider, mPixelDivider, mInterfaceDivider;
STATIC EFI_CLOCK_CR_PROTOCOL *mClock;
STATIC EFI_RPMH_CR_PROTOCOL *mRpmh;
STATIC EFI_INTERCONNECT_CR_PROTOCOL *mIcb;
STATIC CONST CHAR8 *mGdscName;
STATIC BOOLEAN mGdscOn;
STATIC BOOLEAN mPhyTouched;
STATIC BOOLEAN mReady;
STATIC BOOLEAN mRetained;
STATIC UINT64 mPixelRate;
STATIC UINT32 mHostClockTiming;
STATIC EFI_SMMU_CR_PROTOCOL *mSmmu;
STATIC EDKII_IOMMU_PROTOCOL *mIommu;
STATIC VOID *mFrameBufferMapping;
STATIC VOID *mCommandCpu;
STATIC VOID *mCommandMapping;
STATIC EFI_PHYSICAL_ADDRESS mCommandDevice;
STATIC BOOLEAN mCommandRetained;
STATIC EFI_PHYSICAL_ADDRESS mMappedPhysical;
STATIC EFI_PHYSICAL_ADDRESS mMappedDeviceAddress;
STATIC UINTN mMappedSize;

STATIC EFI_STATUS
Poll (UINTN Address, UINT32 Mask, UINT32 Expected, UINTN TimeoutUs)
{
  UINTN Elapsed;
  UINT32 Value = 0;
  for (Elapsed = 0; Elapsed < TimeoutUs; Elapsed += 5) {
    Value = MmioRead32 (Address);
    if ((Value & Mask) == Expected) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay (5);
  }
  DEBUG ((DEBUG_ERROR, "MDP resource poll %lx: value=%x mask=%x expected=%x\n",
          (UINT64)Address, Value, Mask, Expected));
  return EFI_TIMEOUT;
}

STATIC INT32
RoundUp (INT32 Value, INT32 Divisor)
{
  return Value >= 0 ? (Value + Divisor - 1) / Divisor :
                      (Value - Divisor + 1) / Divisor;
}

STATIC UINT32
Interpolate (INT32 Maximum, INT32 Minimum, INT32 Percent)
{
  INT32 Result = RoundUp ((Maximum - Minimum) * Percent, 100) + Minimum;
  return (UINT32)MAX (Result, 0);
}

/* Linux msm_dsi_dphy_timing_calc_v4, with 1000x nanosecond precision. */
STATIC EFI_STATUS
CalculateTimings (UINT64 BitRate, UINT32 *Values)
{
  INT32 Ui, Ui8, Min, Max;
  UINT32 Index;
  if ((BitRate < 80000000ULL) || (BitRate > 2500000000ULL)) {
    return EFI_UNSUPPORTED;
  }
  Ui = (INT32)(1000000000ULL / (BitRate / 1000));
  Ui8 = Ui * 8;
  ZeroMem (Values, 14 * sizeof (*Values));
  Values[2] = Interpolate (95000 / Ui8, RoundUp (38000, Ui8), 50);
  Min = RoundUp (300000 - (INT32)Values[2] * Ui8, Ui8) - 1;
  Max = Min > 255 ? 511 : 255;
  Values[1] = Interpolate (Max, Min, 2);
  Values[3] = Interpolate ((85000 + 15 * Ui) / Ui8,
                          RoundUp (60000 + 3 * Ui, Ui8), 30);
  Values[6] = Interpolate ((85000 + 6 * Ui) / Ui8,
                          RoundUp (40000 + 4 * Ui, Ui8), 50);
  Min = RoundUp (145000 + 10 * Ui - (INT32)Values[6] * Ui8, Ui8) - 1;
  Values[5] = Interpolate (255, Min, 10);
  Values[7] = Interpolate ((85000 + 12 * Ui) / Ui8 - 1,
                          RoundUp (60000 + 4 * Ui, Ui8) - 1, 30);
  Values[8] = (UINT32)RoundUp (50000 - 8 * Ui, Ui8);
  Values[4] = Interpolate (255, RoundUp (100000, Ui8) - 1, 10);
  Min = RoundUp (60000 + 52 * Ui + ((INT32)Values[7] + 1) * Ui8, 16 * Ui) - 1;
  Values[13] = Interpolate (255, Min, 5);
  Min = RoundUp (106000 + ((INT32)Values[2] + (INT32)Values[1] + 1) * Ui8,
                 16 * Ui) - 1;
  Values[12] = (UINT32)(RoundUp ((255 - Min) * 125, 10000) + Min);
  Values[9] = 2;
  Values[10] = 4;
  for (Index = 0; Index < 14; Index++) {
    if (Values[Index] > 255) {
      return EFI_UNSUPPORTED;
    }
  }
  return EFI_SUCCESS;
}

typedef struct { UINT16 Offset; UINT8 Value; } PLL_VALUE;
STATIC CONST PLL_VALUE mPllFixed[] = {
  { 0x018, 0x01 }, { 0x004, 0x03 }, { 0x010, 0x00 }, { 0x020, 0x00 },
  { 0x024, 0x4E }, { 0x044, 0x40 }, { 0x068, 0xBA }, { 0x078, 0x0C },
  { 0x0A8, 0x00 }, { 0x0B8, 0x00 }, { 0x0C8, 0x08 }, { 0x160, 0x0A },
  { 0x168, 0xC0 }, { 0x170, 0x84 }, { 0x170, 0x82 }, { 0x178, 0x4C },
  { 0x190, 0x80 }, { 0x090, 0x29 }, { 0x090, 0x2F }, { 0x094, 0x2A },
  { 0x094, 0x3F }, { 0x260, 0x22 },
};

STATIC EFI_STATUS
EnablePhy (UINT64 PixelRate, UINT32 Lanes)
{
  UINT64 BitRate, Vco, Divider;
  UINT32 OutDivider, PixelDivider, PixelMux, Inverter, Index, Timings[14];
  UINTN Lane;
  EFI_STATUS Status;
  STATIC CONST UINT8 LaneDrive[] = { 0x40, 0x40, 0x40, 0x46, 0x41 };

  BitRate = PixelRate * 24 / Lanes;
  Status = CalculateTimings (BitRate, Timings);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  /* Linux dsi_ctrl_enable packs the shared v4 PHY timings into the host's
     six-bit fields. PHY TIMING_CTRL_12/13 retain the full eight-bit values. */
  mHostClockTiming = (Timings[12] & 0x3F) | ((Timings[13] & 0x3F) << 8);
  Vco = BitRate;
  for (OutDivider = 0; (Vco < 600000000ULL) && (OutDivider < 3); OutDivider++) {
    Vco *= 2;
  }
  if ((Vco < 600000000ULL) || (Vco > 5000000000ULL)) {
    return EFI_UNSUPPORTED;
  }
  PixelDivider = 24 / Lanes;
  PixelMux = 0;
  if (PixelDivider > 15) {
    PixelDivider /= 2;
    PixelMux = 1;
  }

  mPhyTouched = TRUE;
  /* This read is intentionally after rails, interconnect, GDSC and AHB.
     A running foreign PLL is not a cold-init ownership handoff. */
  if ((MmioRead32 (PHY_BASE + PHY_PLL_CTRL) & BIT0) != 0) {
    mPhyTouched = FALSE;
    DEBUG ((DEBUG_ERROR, "MDP: DSI0 PLL already running; refusing to take ownership\n"));
    return EFI_ACCESS_DENIED;
  }
  MmioWrite32 (PHY_BASE + PHY_REFGEN_VOTE, 1);
  MicroSecondDelay (500);
  Status = Poll (PHY_BASE + PHY_STATUS, BIT0, BIT0, 1000);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  MmioWrite32 (PHY_BASE + PHY_CTRL0, BIT6 | BIT5);
  MmioWrite32 (PHY_BASE + PHY_PLL_CTRL, 0);
  MmioWrite32 (PHY_BASE + PHY_RBUF_CTRL, 0);
  if ((MmioRead32 (PHY_BASE) & 0xF0) == 0x20) {
    MmioWrite32 (PHY_BASE + PHY_CTRL4, 4);
  }
  MmioWrite32 (PHY_BASE + PHY_LANE_CFG0, 0x21);
  MmioWrite32 (PHY_BASE + PHY_LANE_CFG1, 0x84);
  MmioWrite32 (PHY_BASE + PHY_VREG_CTRL0, BitRate <= 1500000000ULL ? 0x53 : 0x52);
  MmioWrite32 (PHY_BASE + PHY_VREG_CTRL1, 0x5C);
  MmioWrite32 (PHY_BASE + PHY_CTRL3, 0);
  MmioWrite32 (PHY_BASE + PHY_CAL_SEL, 0);
  MmioWrite32 (PHY_BASE + PHY_HSTX_STR, 0x88);
  MmioWrite32 (PHY_BASE + PHY_PEMPH_CTRL, 0);
  MmioWrite32 (PHY_BASE + PHY_RESCODE_TOP, BitRate <= 1500000000ULL ? 0x3D : 0x01);
  MmioWrite32 (PHY_BASE + PHY_RESCODE_BOTTOM, BitRate <= 1500000000ULL ? 0x38 : 0x39);
  MmioWrite32 (PHY_BASE + PHY_LPTX_STR, 0x55);
  MmioWrite32 (PHY_BASE + PHY_CTRL0, 0x7F);
  MmioWrite32 (PHY_BASE + PHY_LANE_CTRL0, 0x1F);
  MmioWrite32 (PHY_BASE + PHY_CTRL2, 0x40);
  MmioWrite32 (PHY_BASE + PHY_CLK_CFG1, PixelMux); /* standalone internal PLL */
  for (Index = 0; Index < ARRAY_SIZE (Timings); Index++) {
    MmioWrite32 (PHY_BASE + PHY_TIMING0 + 4 * Index, Timings[Index]);
  }
  for (Index = 0; Index < 5; Index++) {
    Lane = LANE_BASE + Index * 0x80;
    MmioWrite32 (Lane + 0x14, Index == 0 ? 3 : 0);
    MmioWrite32 (Lane + 0x10, 0);
    MmioWrite32 (Lane + 0x00, 0);
    MmioWrite32 (Lane + 0x04, 0);
    MmioWrite32 (Lane + 0x08, Index == 4 ? 0x8A : 0x0A);
    MmioWrite32 (Lane + 0x18, LaneDrive[Index]);
  }

  /* Linux dsi_pll_enable_pll_bias(): the PHY power bits alone do not
     enable the PLL bias. Set SYSTEM_MUXES and allow at least 250 ns
     before programming the PLL, including after analog rail collapse. */
  MmioOr32 (PHY_BASE + PHY_CTRL0, BIT5);
  MmioWrite32 (PLL_BASE + PLL_SYSTEM_MUXES, 0xC0);
  NanoSecondDelay (250);
  Divider = (Vco << 18) / 38400000ULL;
  Inverter = Vco <= 1000000000ULL ? 0xA0 :
             Vco <= 2500000000ULL ? 0x20 :
             Vco <= 3500000000ULL ? 0 : 0x40;
  MmioWrite32 (PLL_BASE + 0x0BC, 0x12); /* CORE_INPUT_OVERRIDE */
  MmioWrite32 (PLL_BASE + 0x0E0, (UINT32)(Divider >> 18));
  MmioWrite32 (PLL_BASE + 0x0E4, (UINT32)Divider & 0xFF);
  MmioWrite32 (PLL_BASE + 0x0E8, ((UINT32)Divider >> 8) & 0xFF);
  MmioWrite32 (PLL_BASE + 0x0EC, ((UINT32)Divider >> 16) & 3);
  MmioWrite32 (PLL_BASE + 0x158, 0x40); /* LOCKDET_RATE_1 */
  MmioWrite32 (PLL_BASE + 0x194, 6);    /* LOCK_DELAY */
  MmioWrite32 (PLL_BASE + 0x250, 0x10); /* CMODE_1: DPHY */
  MmioWrite32 (PLL_BASE + 0x248, Inverter);
  MmioWrite32 (PLL_BASE + 0x258, Vco >= 3100000000ULL ? 3 : 1);
  MmioWrite32 (PLL_BASE + 0x240, Vco < 1520000000ULL ? 8 :
                                 Vco >= 2990000000ULL ? 1 : 0);
  for (Index = 0; Index < ARRAY_SIZE (mPllFixed); Index++) {
    MmioWrite32 (PLL_BASE + mPllFixed[Index].Offset, mPllFixed[Index].Value);
  }
  MmioWrite32 (PLL_BASE + 0x150, 0); /* SSC disabled */
  MmioWrite32 (PLL_BASE + 0x154, OutDivider);
  MmioWrite32 (PHY_BASE + PHY_CLK_CFG0, (PixelDivider << 4) | 1);
  MemoryFence ();
  MmioWrite32 (PHY_BASE + PHY_PLL_CTRL, 1);
  Status = Poll (PLL_BASE + 0x1B0, BIT0, BIT0, 5000);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  MmioWrite32 (PHY_BASE + PHY_DIG_RESET, 1);
  MemoryFence ();
  MmioWrite32 (PHY_BASE + PHY_DIG_RESET, 0);
  MemoryFence ();
  MmioWrite32 (PHY_BASE + PHY_CTRL3, 4);
  MmioOr32 (PHY_BASE + PHY_CLK_CFG1, BIT5 | BIT4);
  MmioWrite32 (PHY_BASE + PHY_RBUF_CTRL, 1);
  /* dsi_7nm_set_continuous_clock(): LT9611 uses continuous clock mode. */
  MmioOr32 (PHY_BASE + PHY_LANE_CTRL1, BIT5 | BIT6);
  DEBUG ((DEBUG_INFO, "MDP: DSI0 PHY locked pixel=%Lu bit=%Lu VCO=%Lu pre=%u post=%u\n",
          PixelRate, BitRate, Vco, Timings[12], Timings[13]));
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
EnableClock (UINTN Index)
{
  EFI_STATUS Status;
  MDP_CLOCK *Clock = &mClocks[Index];
  Status = CrAcquireClock (mClock, Clock->Controller, Clock->Name,
                            Clock->Rate, &Clock->Enabled);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MDP: clock %a failed: %r\n", Clock->Name, Status));
  }
  return Status;
}

STATIC EFI_STATUS
RequestRails (VOID)
{
  UINTN Index;
  EFI_STATUS Status;
  for (Index = 0; Index < ARRAY_SIZE (mRailVotes); Index++) {
    Status = mRpmh->AcquireVreg (mRpmh, mRailVotes[Index].Name,
                                  mRailVotes[Index].Millivolts, 7,
                                  &mRailVotes[Index].Requested);
    if (EFI_ERROR (Status)) { return Status; }
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
RequestInterconnect (UINT64 PixelRate)
{
  EFI_STATUS Status;
  UINTN Index;
  UINT64 Average;
  for (Index = 0; Index < ARRAY_SIZE (mIcbPaths); Index++) {
    Status = mIcb->AcquirePath (mIcb, "qcom,sm8450-mdp",
                                mIcbPaths[Index].Source,
                                mIcbPaths[Index].Destination,
                                &mIcbHandles[Index]);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "MDP: ICB route %u -> %u unavailable: %r\n",
              mIcbPaths[Index].Source, mIcbPaths[Index].Destination, Status));
      return Status;
    }
    Average = Index == 0 ? 1000 : (PixelRate * 4 + 999) / 1000;
    Status = mIcb->SetBandwidth (mIcb, mIcbHandles[Index], Average,
                                 Index == 0 ? Average : Average * 2);
    if (EFI_ERROR (Status)) { return Status; }
    mIcbAverage[Index] = Average;
    mIcbPeak[Index] = Index == 0 ? Average : Average * 2;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
MdpInitializeResources (IN CONST EFI_MDP_DSI_TIMING *Timing)
{
  EFI_STATUS Status, Cleanup;
  UINT64 Htotal, Vtotal, PixelRate, ByteRate;
  UINTN Index;
  CONST CHAR8 *Stage = "validation";
  if (Timing == NULL || Timing->DsiController >= ARRAY_SIZE (mDsiTargets) || Timing->DsiColorFormat != 36 ||
      Timing->DsiLanes == 0 || Timing->DsiLanes > 4 || Timing->DsiRefreshRate == 0) {
    return EFI_UNSUPPORTED;
  }
  Htotal = (UINT64)Timing->Width + Timing->HFrontPorch + Timing->HBackPorch + Timing->HSync;
  Vtotal = (UINT64)Timing->Height + Timing->VFrontPorch + Timing->VBackPorch + Timing->VSync;
  if (Htotal == 0 || Vtotal == 0 || Htotal > 65535 || Vtotal > 65535 ||
      Timing->DsiRefreshRate > (240U << 16)) {
    return EFI_INVALID_PARAMETER;
  }
  PixelRate = (Htotal * Vtotal * Timing->DsiRefreshRate + 0x8000) >> 16;
  if (PixelRate == 0 || PixelRate > 200000000) {
    return EFI_UNSUPPORTED;
  }
  if (mRetained || mReady) {
    return EFI_ALREADY_STARTED;
  }
  mActiveController = Timing->DsiController;
  mDsiTarget = &mDsiTargets[mActiveController];
  mClocks[CORE_CLOCK_COUNT - 1].Name = mDsiTarget->EscapeClock;
  mClocks[BYTE_CLOCK].Name = mDsiTarget->ByteClock;
  mClocks[BYTE_INTF_CLOCK].Name = mDsiTarget->ByteInterfaceClock;
  mClocks[PIXEL_CLOCK].Name = mDsiTarget->PixelClock;
  Stage = "clock-protocol";
  Status = gBS->LocateProtocol (&gEfiClockCrProtocolGuid, NULL, (VOID **)&mClock);
  if (EFI_ERROR (Status)) { goto Failed; }
  if (mClock == NULL || mClock->Revision < EFI_CLOCK_CR_PROTOCOL_REVISION ||
      mClock->SetClock == NULL || mClock->SetGdsc == NULL ||
      mClock->ConfigureExternalSource == NULL || mClock->SetDivider == NULL) {
    Status = EFI_UNSUPPORTED;
    goto Failed;
  }
  Stage = "rpmh-protocol";
  Status = gBS->LocateProtocol (&gEfiRpmhCrProtocolGuid, NULL, (VOID **)&mRpmh);
  if (EFI_ERROR (Status)) { goto Failed; }
  if (mRpmh == NULL || mRpmh->Revision < EFI_RPMH_CR_PROTOCOL_REVISION ||
      mRpmh->RpmhSetVregVoltage == NULL || mRpmh->RpmhSetVregMode == NULL ||
      mRpmh->RpmhEnableVreg == NULL || mRpmh->AcquireVreg == NULL) {
    Status = EFI_UNSUPPORTED;
    goto Failed;
  }
  Stage = "interconnect-protocol";
  Status = gBS->LocateProtocol (&gEfiInterconnectCrProtocolGuid, NULL, (VOID **)&mIcb);
  if (EFI_ERROR (Status)) { goto Failed; }
  if (mIcb == NULL || mIcb->Revision < EFI_INTERCONNECT_CR_PROTOCOL_REVISION ||
      mIcb->AcquirePath == NULL || mIcb->SetBandwidth == NULL ||
      mIcb->ReleasePath == NULL) {
    Status = EFI_UNSUPPORTED;
    goto Failed;
  }
  Stage = "regulators";
  Status = RequestRails ();
  if (EFI_ERROR (Status)) { goto Failed; }
  Stage = "interconnect";
  Status = RequestInterconnect (PixelRate);
  if (EFI_ERROR (Status)) { goto Failed; }
  Stage = "dispcc-access";
  for (Index = 0; Index < ACCESS_CLOCK_COUNT; Index++) {
    Status = EnableClock (Index);
    if (EFI_ERROR (Status)) { goto Failed; }
  }
  Stage = "mdss-gdsc";
  mGdscName = "disp_cc_mdss_core_gdsc";
  Status = CrAcquireGdsc (mClock, "dispcc", mGdscName, &mGdscOn);
  if (EFI_ERROR (Status)) { goto Failed; }
  Stage = "mdss-clocks";
  for (Index = ACCESS_CLOCK_COUNT; Index < CORE_CLOCK_COUNT; Index++) {
    Status = EnableClock (Index);
    if (EFI_ERROR (Status)) { goto Failed; }
  }
  /* Confirm the native GDSC provider brought MDSS up before any PHY access. */
  Stage = "gdsc-power-on";
  Status = Poll (DISP_CC_BASE + 0x9000, BIT31, BIT31, 5000);
  if (EFI_ERROR (Status)) { goto Failed; }
  Stage = "dsi-phy";
  Status = EnablePhy (PixelRate, Timing->DsiLanes);
  if (EFI_ERROR (Status)) { goto Failed; }
  Stage = "dsi-clocks";
  ByteRate = PixelRate * 24 / Timing->DsiLanes / 8;
  /* Linux dispcc parent map: byte0/pclk0=2/1, byte1/pclk1=4/3. Native clock
     divider is twice the integer divisor: 2 selects an undivided source. */
  Status = mClock->ConfigureExternalSource (
             mClock, mClocks[BYTE_CLOCK].Controller, mClocks[BYTE_CLOCK].Name,
             (UINT32)ByteRate, mDsiTarget->ByteSource, 2, 0, 0, 0);
  if (EFI_ERROR (Status)) { goto Failed; }
  mByteRate = (UINT32)ByteRate; mByteSource = mDsiTarget->ByteSource; mByteDivider = 2;
  Status = mClock->ConfigureExternalSource (
             mClock, mClocks[PIXEL_CLOCK].Controller, mClocks[PIXEL_CLOCK].Name,
             (UINT32)PixelRate, mDsiTarget->PixelSource, 2, 0, 0, 0);
  if (EFI_ERROR (Status)) { goto Failed; }
  mPixelSource = mDsiTarget->PixelSource; mPixelDivider = 2;
  Status = mClock->SetDivider (
             mClock, mClocks[BYTE_INTF_CLOCK].Controller,
             mClocks[BYTE_INTF_CLOCK].Name, 2);
  if (EFI_ERROR (Status)) { goto Failed; }
  mInterfaceDivider = 2;
  for (Index = CORE_CLOCK_COUNT; Index < ARRAY_SIZE (mClocks); Index++) {
    Status = EnableClock (Index);
    if (EFI_ERROR (Status)) { goto Failed; }
  }
  /* Fail closed if the provider interprets source/divider fields differently. */
  if ((MmioRead32 (DISP_CC_BASE + mDsiTarget->ByteConfig) & 0x371F) !=
        ((mDsiTarget->ByteSource << 8) | 1) ||
      (MmioRead32 (DISP_CC_BASE + mDsiTarget->PixelConfig) & 0x371F) !=
        ((mDsiTarget->PixelSource << 8) | 1) ||
      (MmioRead32 (DISP_CC_BASE + mDsiTarget->ByteDivider) & 0xF) != 1) {
    DEBUG ((DEBUG_ERROR, "MDP: DSI external clock mux/divider mismatch\n"));
    Status = EFI_PROTOCOL_ERROR;
    goto Failed;
  }
  mPixelRate = PixelRate;
  mReady = TRUE;
  DEBUG ((DEBUG_INFO, "MDP: display resources ready; pixel=%Lu byte=%Lu\n", PixelRate, ByteRate));
  return EFI_SUCCESS;

Failed:
  DEBUG ((DEBUG_ERROR, "MDP: resource stage %a failed: %r\n", Stage, Status));
  Cleanup = MdpReleaseResources ();
  if (EFI_ERROR (Cleanup)) {
    DEBUG ((DEBUG_ERROR, "MDP: resource cleanup retained dependencies: %r\n", Cleanup));
  }
  return Status;
}

EFI_STATUS
MdpGetActiveController (OUT UINT32 *Controller)
{
  if (Controller == NULL) return EFI_INVALID_PARAMETER;
  *Controller = MAX_UINT32;
  if (mActiveController == MAX_UINT32) return EFI_NOT_READY;
  *Controller = mActiveController;
  return EFI_SUCCESS;
}

EFI_STATUS
MdpGetHostClockTiming (OUT UINT32 *Value)
{
  if (Value == NULL) { return EFI_INVALID_PARAMETER; }
  if (!mReady || mRetained || mCommandRetained) { return EFI_NOT_READY; }
  *Value = mHostClockTiming;
  return EFI_SUCCESS;
}

EFI_STATUS
MdpMapFrameBuffer (
  IN EFI_PHYSICAL_ADDRESS Physical,
  IN UINTN Size,
  OUT EFI_PHYSICAL_ADDRESS *DeviceAddress
  )
{
  /* sm8450.dtsi iommus=<apps_smmu 0x2800 0x402>. Each bit set in the
     stream-match mask is ignored: expand the set into four exact domains.
     Never reconfigure an existing wildcard or an enabled context bank. */
  STATIC CONST UINT16 Sids[] = { 0x2800, 0x2802, 0x2C00, 0x2C02 };
  EFI_STATUS Status;
  EFI_STATUS Cleanup;
  VOID *Mapping;
  UINTN Index;
  UINTN NumberOfBytes;
  if (DeviceAddress == NULL || Size == 0 || Size > MAX_UINTN - EFI_PAGE_MASK ||
      (Physical & EFI_PAGE_MASK) != 0 ||
      Physical > MAX_UINT64 - EFI_SIZE_TO_PAGES (Size) * EFI_PAGE_SIZE) {
    return EFI_INVALID_PARAMETER;
  }
  *DeviceAddress = 0;
  if (!mReady || mRetained || mCommandRetained) { return EFI_NOT_READY; }
  if (mMappedSize != 0) {
    if (Physical != mMappedPhysical || Size != mMappedSize) { return EFI_ACCESS_DENIED; }
    *DeviceAddress = mMappedDeviceAddress;
    return EFI_SUCCESS;
  }
  Status = gBS->LocateProtocol (&gEfiSmmuCrProtocolGuid, NULL, (VOID **)&mSmmu);
  if (EFI_ERROR (Status) || mSmmu == NULL || mSmmu->Attach == NULL ||
      mSmmu->Revision < EFI_SMMU_CR_PRESERVE_MAPPING_REVISION ||
      mSmmu->PreserveReadOnlyMapping == NULL) {
    return EFI_NOT_READY;
  }
  Status = gBS->LocateProtocol (&gEdkiiIoMmuProtocolGuid, NULL, (VOID **)&mIommu);
  if (EFI_ERROR (Status) || mIommu == NULL || mIommu->Map == NULL ||
      mIommu->Unmap == NULL || mIommu->SetAttributeById == NULL) {
    return EFI_NOT_READY;
  }
  for (Index = 0; Index < ARRAY_SIZE (Sids); Index++) {
    Status = mSmmu->Attach (mSmmu, 0, 0xFFFF, Sids[Index]);
    if (EFI_ERROR (Status)) { break; }
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MDP: framebuffer SID=0x%x attach failed: %r\n",
            Sids[Index], Status));
    return Status;
  }
  NumberOfBytes = Size;
  Mapping = NULL;
  Status = mIommu->Map (mIommu, EdkiiIoMmuOperationBusMasterRead64,
                        (VOID *)(UINTN)Physical, &NumberOfBytes,
                        DeviceAddress, &Mapping);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  /* Map may succeed with fewer bytes than requested. DPU fetches the
     complete surface using a 32-bit source address, so validate the last
     byte too before granting any stream access to this mapping. */
  if (NumberOfBytes != Size || *DeviceAddress > MAX_UINT32 ||
      Size - 1 > MAX_UINT32 - *DeviceAddress) {
    Status = NumberOfBytes != Size ? EFI_BAD_BUFFER_SIZE : EFI_UNSUPPORTED;
    Cleanup = mIommu->Unmap (mIommu, Mapping);
    if (EFI_ERROR (Cleanup)) {
      mFrameBufferMapping = Mapping;
      mRetained = TRUE;
    }
    *DeviceAddress = 0;
    return Status;
  }
  for (Index = 0; Index < ARRAY_SIZE (Sids); Index++) {
    Status = mIommu->SetAttributeById (
                       mIommu, SMMU_BASE, Sids[Index], Mapping,
                       EDKII_IOMMU_ACCESS_READ);
    if (EFI_ERROR (Status)) {
      break;
    }
  }
  if (!EFI_ERROR (Status)) {
    /* GOP scanout and its reserved backing memory outlive boot services.
       Preserve this read-only mapping, not command buffers or other DMA. */
    Status = mSmmu->PreserveReadOnlyMapping (mSmmu, Mapping);
  }
  if (EFI_ERROR (Status)) {
    Cleanup = mIommu->Unmap (mIommu, Mapping);
    if (EFI_ERROR (Cleanup)) {
      mFrameBufferMapping = Mapping;
      mRetained = TRUE;
      DEBUG ((DEBUG_ERROR, "MDP: failed map revoke failed: %r\n", Cleanup));
    }
    *DeviceAddress = 0;
    return Status;
  }
  mFrameBufferMapping = Mapping;
  mMappedPhysical = Physical;
  mMappedDeviceAddress = *DeviceAddress;
  mMappedSize = Size;
  return EFI_SUCCESS;
}

/** Caller has stopped command DMA before revoking this common buffer. */
EFI_STATUS
MdpReleaseCommandBuffer (VOID)
{
  EFI_STATUS Status;
  if (mCommandMapping != NULL) {
    Status = mIommu->Unmap (mIommu, mCommandMapping);
    if (EFI_ERROR (Status)) { mCommandRetained = TRUE; return Status; }
    mCommandMapping = NULL;
  }
  if (mCommandCpu != NULL) {
    Status = mIommu->FreeBuffer (mIommu, 1, mCommandCpu);
    if (EFI_ERROR (Status)) { mCommandRetained = TRUE; return Status; }
    mCommandCpu = NULL;
  }
  mCommandDevice = 0;
  mCommandRetained = FALSE;
  return EFI_SUCCESS;
}

EFI_STATUS
MdpAcquireCommandBuffer (
  OUT VOID **Cpu,
  OUT EFI_PHYSICAL_ADDRESS *Device,
  OUT UINTN *Capacity
  )
{
  STATIC CONST UINT16 Sids[] = { 0x2800, 0x2802, 0x2C00, 0x2C02 };
  EFI_STATUS Status;
  UINTN Index;
  UINTN Bytes = EFI_PAGE_SIZE;
  if (Cpu == NULL || Device == NULL || Capacity == NULL) return EFI_INVALID_PARAMETER;
  *Cpu = NULL; *Device = 0; *Capacity = 0;
  if (!mReady || mRetained || mCommandRetained) return EFI_NOT_READY;
  if (mCommandCpu != NULL) return EFI_ALREADY_STARTED;
  Status = gBS->LocateProtocol (&gEfiSmmuCrProtocolGuid, NULL, (VOID **)&mSmmu);
  if (EFI_ERROR (Status) || mSmmu == NULL || mSmmu->Attach == NULL) return EFI_NOT_READY;
  Status = gBS->LocateProtocol (&gEdkiiIoMmuProtocolGuid, NULL, (VOID **)&mIommu);
  if (EFI_ERROR (Status) || mIommu == NULL || mIommu->AllocateBuffer == NULL ||
      mIommu->FreeBuffer == NULL || mIommu->Map == NULL ||
      mIommu->Unmap == NULL || mIommu->SetAttributeById == NULL) return EFI_NOT_READY;
  for (Index = 0; Index < ARRAY_SIZE (Sids); ++Index) {
    Status = mSmmu->Attach (mSmmu, 0, 0xFFFF, Sids[Index]);
    if (EFI_ERROR (Status)) return Status;
  }
  /* Common-buffer storage is mandatory: ordinary BusMasterRead mappings
     may snapshot/bounce the contents before the caller formats its packet. */
  Status = mIommu->AllocateBuffer (mIommu, AllocateAnyPages, EfiBootServicesData,
                                  1, &mCommandCpu, 0);
  if (EFI_ERROR (Status)) { mCommandCpu = NULL; return Status; }
  if (((UINTN)mCommandCpu & EFI_PAGE_MASK) != 0 || mCommandCpu == NULL) {
    Status = EFI_DEVICE_ERROR;
    goto Error;
  }
  ZeroMem (mCommandCpu, EFI_PAGE_SIZE);
  Status = mIommu->Map (mIommu, EdkiiIoMmuOperationBusMasterCommonBuffer,
                       mCommandCpu, &Bytes, &mCommandDevice, &mCommandMapping);
  if (EFI_ERROR (Status)) { mCommandMapping = NULL; goto Error; }
  if (Bytes != EFI_PAGE_SIZE || (mCommandDevice & EFI_PAGE_MASK) != 0 ||
      mCommandDevice > MAX_UINT32 - (EFI_PAGE_SIZE - 1)) {
    Status = Bytes != EFI_PAGE_SIZE ? EFI_BAD_BUFFER_SIZE : EFI_UNSUPPORTED;
    goto Error;
  }
  for (Index = 0; Index < ARRAY_SIZE (Sids); ++Index) {
    Status = mIommu->SetAttributeById (mIommu, SMMU_BASE, Sids[Index],
                                      mCommandMapping, EDKII_IOMMU_ACCESS_READ);
    if (EFI_ERROR (Status)) goto Error;
  }
  *Cpu = mCommandCpu; *Device = mCommandDevice; *Capacity = EFI_PAGE_SIZE;
  return EFI_SUCCESS;
Error:
  /* No command was submitted. A failed revoke/free pins the allocation
     and forbids reuse; resource teardown retries before dropping clocks. */
  MdpReleaseCommandBuffer ();
  return Status;
}

EFI_STATUS
MdpReleaseResources (VOID)
{
  EFI_STATUS Status;
  UINTN Index;
  Status = MdpReleaseCommandBuffer ();
  if (EFI_ERROR (Status)) return Status;
  /* Stop at the first uncertain DMA state. SmmuCrDxe retains ownership of
     attached streams and page tables; this consumer only revokes its map. */
  if (mFrameBufferMapping != NULL) {
    Status = mIommu->Unmap (mIommu, mFrameBufferMapping);
    if (EFI_ERROR (Status)) {
      mRetained = TRUE;
      return Status;
    }
    mFrameBufferMapping = NULL;
  }
  mMappedSize = 0;
  mMappedPhysical = 0;
  mMappedDeviceAddress = 0;
  for (Index = ARRAY_SIZE (mClocks); Index-- > CORE_CLOCK_COUNT;) {
    if (mClocks[Index].Enabled) {
      Status = mClock->SetClock (mClock, mClocks[Index].Controller,
                                 mClocks[Index].Name, 0, FALSE);
      if (EFI_ERROR (Status)) { mRetained = TRUE; return Status; }
      mClocks[Index].Enabled = FALSE;
    }
  }
  if (mPhyTouched) {
    MmioAnd32 (PHY_BASE + PHY_LANE_CTRL1, (UINT32)~(BIT5 | BIT6));
    MmioAnd32 (PHY_BASE + PHY_CLK_CFG1, (UINT32)~BIT5);
    MmioWrite32 (PHY_BASE + PHY_PLL_CTRL, 0);
    MmioWrite32 (PHY_BASE + PHY_RBUF_CTRL, 0);
    /* Pair the bias vote with Linux dsi_pll_disable_pll_bias(). */
    MmioWrite32 (PLL_BASE + PLL_SYSTEM_MUXES, 0);
    MmioAnd32 (PHY_BASE + PHY_CTRL0, (UINT32)~BIT5);
    NanoSecondDelay (250);
    MmioWrite32 (LANE_BASE + 0x14, 0);
    MmioWrite32 (PHY_BASE + PHY_REFGEN_VOTE, 0);
    MemoryFence ();
    MicroSecondDelay (2);
    MmioWrite32 (PHY_BASE + PHY_LANE_CTRL0, 0);
    MmioWrite32 (PHY_BASE + PHY_CTRL0, 0);
    MemoryFence ();
    mPhyTouched = FALSE;
  }
  for (Index = CORE_CLOCK_COUNT; Index-- > ACCESS_CLOCK_COUNT;) {
    if (mClocks[Index].Enabled) {
      Status = mClock->SetClock (mClock, mClocks[Index].Controller,
                                 mClocks[Index].Name, 0, FALSE);
      if (EFI_ERROR (Status)) { mRetained = TRUE; return Status; }
      mClocks[Index].Enabled = FALSE;
    }
  }
  if (mGdscOn) {
    Status = mClock->SetGdsc (mClock, "dispcc", mGdscName, FALSE);
    if (EFI_ERROR (Status)) { mRetained = TRUE; return Status; }
    mGdscOn = FALSE;
  }
  for (Index = ACCESS_CLOCK_COUNT; Index-- > 0;) {
    if (mClocks[Index].Enabled) {
      Status = mClock->SetClock (mClock, mClocks[Index].Controller,
                                 mClocks[Index].Name, 0, FALSE);
      if (EFI_ERROR (Status)) { mRetained = TRUE; return Status; }
      mClocks[Index].Enabled = FALSE;
    }
  }
  for (Index = ARRAY_SIZE (mIcbHandles); Index-- > 0;) {
    if (mIcbHandles[Index] != INTERCONNECT_INVALID_PATH) {
      Status = mIcb->ReleasePath (mIcb, mIcbHandles[Index]);
      if (EFI_ERROR (Status)) { mRetained = TRUE; return Status; }
      mIcbHandles[Index] = INTERCONNECT_INVALID_PATH;
      mIcbAverage[Index] = mIcbPeak[Index] = 0;
    }
  }
  for (Index = ARRAY_SIZE (mRailVotes); Index-- > 0;) {
    if (mRailVotes[Index].Requested) {
      Status = mRpmh->RpmhEnableVreg (mRpmh, mRailVotes[Index].Name, FALSE);
      if (EFI_ERROR (Status)) { mRetained = TRUE; return Status; }
      mRailVotes[Index].Requested = FALSE;
    }
  }
  mReady = FALSE;
  mRetained = FALSE;
  mActiveController = MAX_UINT32;
  mPixelRate = 0;
  mHostClockTiming = 0;
  mByteRate = mByteSource = mPixelSource = mByteDivider = mPixelDivider = mInterfaceDivider = 0;
  return EFI_SUCCESS;
}

EFI_STATUS
MdpSnapshotResources (CR_MDSS_HANDOFF_PARTICIPANT *P, VOID **Map)
{
  UINT32 Flags = CR_MDSS_NATIVE_OWNED;
  if (!P || !Map || P->Owner != CR_MDSS_NATIVE_DSI || P->ClockCount ||
      P->RailCount || P->PathCount || P->DomainCount) return EFI_INVALID_PARAMETER;
  if (mRetained || mCommandRetained) Flags |= CR_MDSS_NATIVE_RETAINED_ERROR;
  *Map = mFrameBufferMapping;
  for (UINTN I=0; I<ARRAY_SIZE(mClocks); ++I) if (mClocks[I].Enabled) {
    CR_MDSS_NATIVE_CLOCK *C=&P->Clocks[P->ClockCount++];
    C->Owner=P->Owner; C->Flags=Flags; C->RateHz=mClocks[I].Rate;
    AsciiStrCpyS(C->Controller,sizeof(C->Controller),mClocks[I].Controller);
    AsciiStrCpyS(C->Name,sizeof(C->Name),mClocks[I].Name);
  }
  for (UINTN I=0; I<ARRAY_SIZE(mRailVotes); ++I) if (mRailVotes[I].Requested) {
    CR_MDSS_NATIVE_RAIL *R=&P->Rails[P->RailCount++];
    R->Owner=P->Owner; R->Flags=Flags; R->Millivolts=mRailVotes[I].Millivolts; R->Mode=7;
    AsciiStrCpyS(R->Name,sizeof(R->Name),mRailVotes[I].Name);
  }
  for (UINTN I=0; I<ARRAY_SIZE(mIcbHandles); ++I) if (mIcbHandles[I]!=INTERCONNECT_INVALID_PATH) {
    CR_MDSS_NATIVE_PATH *B=&P->Paths[P->PathCount++];
    B->Owner=P->Owner; B->Flags=Flags; B->Source=mIcbPaths[I].Source; B->Destination=mIcbPaths[I].Destination;
    B->Average=mIcbAverage[I]; B->Peak=mIcbPeak[I];
    AsciiStrCpyS(B->Provider,sizeof(B->Provider),"qcom,sm8450-mdp");
  }
  if (mGdscOn) {
    CR_MDSS_NATIVE_DOMAIN *D=&P->Domains[P->DomainCount++];
    D->Owner=P->Owner; D->Flags=Flags;
    AsciiStrCpyS(D->Controller,sizeof(D->Controller),"dispcc");
    AsciiStrCpyS(D->Name,sizeof(D->Name),mGdscName);
  }
  P->Route.SourceRate=mByteRate; P->Route.SourceSelect=mByteSource; P->Route.SourceDivider=mByteDivider;
  P->Route.PixelSource=mPixelSource; P->Route.PixelDivider=mPixelDivider; P->Route.InterfaceDivider=mInterfaceDivider;
  return mReady && !mRetained && !mCommandRetained ? EFI_SUCCESS : EFI_NOT_READY;
}
