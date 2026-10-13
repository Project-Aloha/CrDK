/* SM8450 5nm DPHY, adapted from native MdpResources.c and Linux
 * drivers/gpu/drm/msm/dsi/phy/dsi_phy_7nm.c and dsi_phy.c.
 * Copyright (c) 2015, 2018, The Linux Foundation.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "MdssPlatformInternal.h"
#define PHY_BASE 0U
#define LANE_BASE 0x200U
#define PLL_BASE 0x500U
STATIC UINT32 Read(CR_MDSS_PLATFORM *P, UINT32 O) {
  return P->Io.Read32(P->Io.Context, CrMdssPlatformPhy, O);
}
STATIC VOID Write(CR_MDSS_PLATFORM *P, UINT32 O, UINT32 V) {
  P->Io.Write32(P->Io.Context, CrMdssPlatformPhy, O, V);
}
STATIC VOID Or(CR_MDSS_PLATFORM *P, UINT32 O, UINT32 V) { Write(P,O,Read(P,O)|V); }
STATIC CR_STATUS Poll(CR_MDSS_PLATFORM *P, UINT32 O, UINT32 M, UINT32 V, UINT32 Us) {
  return CrMdssPlatformPoll(P,CrMdssPlatformPhy,O,M,V,Us);
}
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
  return (UINT32)(Result > 0 ? Result : 0);
}

/* Linux msm_dsi_dphy_timing_calc_v4, with 1000x nanosecond precision. */
STATIC CR_STATUS
CalculateTimings (UINT64 BitRate, UINT32 *Values)
{
  INT32 Ui, Ui8, Min, Max;
  UINT32 Index;
  if ((BitRate < 80000000ULL) || (BitRate > 2500000000ULL)) {
    return CR_UNSUPPORTED;
  }
  Ui = (INT32)(1000000000ULL / (BitRate / 1000));
  Ui8 = Ui * 8;
  for (Index = 0; Index < 14; ++Index) Values[Index] = 0;
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
      return CR_UNSUPPORTED;
    }
  }
  return CR_SUCCESS;
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

CR_STATUS
CrMdssPlatformEnablePhy (CR_MDSS_PLATFORM *P)
{
  UINT64 PixelRate = P->PixelRate;
  UINT32 Lanes = P->Config.Mode.Lanes;
  UINT64 BitRate, Vco, Divider;
  UINT32 OutDivider, PixelDivider, PixelMux, Inverter, Index, Timings[14];
  UINT32 Lane;
  CR_STATUS Status;
  STATIC CONST UINT8 LaneDrive[] = { 0x40, 0x40, 0x40, 0x46, 0x41 };

  BitRate = PixelRate * 24 / Lanes;
  Status = CalculateTimings (BitRate, Timings);
  if (CR_ERROR (Status)) {
    return Status;
  }
  /* Linux dsi_ctrl_enable packs the shared v4 PHY timings into the host's
     six-bit fields. PHY TIMING_CTRL_12/13 retain the full eight-bit values. */
  P->HostClockTiming = (Timings[12] & 0x3F) | ((Timings[13] & 0x3F) << 8);
  Vco = BitRate;
  for (OutDivider = 0; (Vco < 600000000ULL) && (OutDivider < 3); OutDivider++) {
    Vco *= 2;
  }
  if ((Vco < 600000000ULL) || (Vco > 5000000000ULL)) {
    return CR_UNSUPPORTED;
  }
  PixelDivider = 24 / Lanes;
  PixelMux = 0;
  if (PixelDivider > 15) {
    PixelDivider /= 2;
    PixelMux = 1;
  }

  P->PhyTouched = TRUE;
  /* This read is intentionally after rails, interconnect, GDSC and AHB.
     A running foreign PLL is not a cold-init ownership handoff. */
  if ((Read (P, PHY_BASE + PHY_PLL_CTRL) & BIT_(0)) != 0) {
    P->PhyTouched = FALSE;
      return CR_BUSY;
  }
  Write (P, PHY_BASE + PHY_REFGEN_VOTE, 1);
  P->Io.DelayUs (P->Io.Context, 500);
  Status = Poll (P, PHY_BASE + PHY_STATUS, BIT_(0), BIT_(0), 1000);
  if (CR_ERROR (Status)) {
    return Status;
  }
  Write (P, PHY_BASE + PHY_CTRL0, BIT_(6) | BIT_(5));
  Write (P, PHY_BASE + PHY_PLL_CTRL, 0);
  Write (P, PHY_BASE + PHY_RBUF_CTRL, 0);
  if ((Read (P, PHY_BASE) & 0xF0) == 0x20) {
    Write (P, PHY_BASE + PHY_CTRL4, 4);
  }
  Write (P, PHY_BASE + PHY_LANE_CFG0, 0x21);
  Write (P, PHY_BASE + PHY_LANE_CFG1, 0x84);
  Write (P, PHY_BASE + PHY_VREG_CTRL0, BitRate <= 1500000000ULL ? 0x53 : 0x52);
  Write (P, PHY_BASE + PHY_VREG_CTRL1, 0x5C);
  Write (P, PHY_BASE + PHY_CTRL3, 0);
  Write (P, PHY_BASE + PHY_CAL_SEL, 0);
  Write (P, PHY_BASE + PHY_HSTX_STR, 0x88);
  Write (P, PHY_BASE + PHY_PEMPH_CTRL, 0);
  Write (P, PHY_BASE + PHY_RESCODE_TOP, BitRate <= 1500000000ULL ? 0x3D : 0x01);
  Write (P, PHY_BASE + PHY_RESCODE_BOTTOM, BitRate <= 1500000000ULL ? 0x38 : 0x39);
  Write (P, PHY_BASE + PHY_LPTX_STR, 0x55);
  Write (P, PHY_BASE + PHY_CTRL0, 0x7F);
  Write (P, PHY_BASE + PHY_LANE_CTRL0, 0x1F);
  Write (P, PHY_BASE + PHY_CTRL2, 0x40);
  Write (P, PHY_BASE + PHY_CLK_CFG1, PixelMux); /* standalone internal PLL */
  for (Index = 0; Index < ARRAY_COUNT (Timings); Index++) {
    Write (P, PHY_BASE + PHY_TIMING0 + 4 * Index, Timings[Index]);
  }
  for (Index = 0; Index < 5; Index++) {
    Lane = LANE_BASE + Index * 0x80;
    Write (P, Lane + 0x14, Index == 0 ? 3 : 0);
    Write (P, Lane + 0x10, 0);
    Write (P, Lane + 0x00, 0);
    Write (P, Lane + 0x04, 0);
    Write (P, Lane + 0x08, Index == 4 ? 0x8A : 0x0A);
    Write (P, Lane + 0x18, LaneDrive[Index]);
  }

  /* Linux dsi_pll_enable_pll_bias(): the PHY power bits alone do not
     enable the PLL bias. Set SYSTEM_MUXES and allow at least 250 ns
     before programming the PLL, including after analog rail collapse. */
  Or (P, PHY_BASE + PHY_CTRL0, BIT_(5));
  Write (P, PLL_BASE + PLL_SYSTEM_MUXES, 0xC0);
  P->Io.DelayUs (P->Io.Context, 1); /* at least 250ns */
  Divider = (Vco << 18) / 38400000ULL;
  Inverter = Vco <= 1000000000ULL ? 0xA0 :
             Vco <= 2500000000ULL ? 0x20 :
             Vco <= 3500000000ULL ? 0 : 0x40;
  Write (P, PLL_BASE + 0x0BC, 0x12); /* CORE_INPUT_OVERRIDE */
  Write (P, PLL_BASE + 0x0E0, (UINT32)(Divider >> 18));
  Write (P, PLL_BASE + 0x0E4, (UINT32)Divider & 0xFF);
  Write (P, PLL_BASE + 0x0E8, ((UINT32)Divider >> 8) & 0xFF);
  Write (P, PLL_BASE + 0x0EC, ((UINT32)Divider >> 16) & 3);
  Write (P, PLL_BASE + 0x158, 0x40); /* LOCKDET_RATE_1 */
  Write (P, PLL_BASE + 0x194, 6);    /* LOCK_DELAY */
  Write (P, PLL_BASE + 0x250, 0x10); /* CMODE_1: DPHY */
  Write (P, PLL_BASE + 0x248, Inverter);
  Write (P, PLL_BASE + 0x258, Vco >= 3100000000ULL ? 3 : 1);
  Write (P, PLL_BASE + 0x240, Vco < 1520000000ULL ? 8 :
                                 Vco >= 2990000000ULL ? 1 : 0);
  for (Index = 0; Index < ARRAY_COUNT (mPllFixed); Index++) {
    Write (P, PLL_BASE + mPllFixed[Index].Offset, mPllFixed[Index].Value);
  }
  Write (P, PLL_BASE + 0x150, 0); /* SSC disabled */
  Write (P, PLL_BASE + 0x154, OutDivider);
  Write (P, PHY_BASE + PHY_CLK_CFG0, (PixelDivider << 4) | 1);
  P->Io.Fence (P->Io.Context);
  Write (P, PHY_BASE + PHY_PLL_CTRL, 1);
  Status = Poll (P, PLL_BASE + 0x1B0, BIT_(0), BIT_(0), 5000);
  if (CR_ERROR (Status)) {
    return Status;
  }
  Write (P, PHY_BASE + PHY_DIG_RESET, 1);
  P->Io.Fence (P->Io.Context);
  Write (P, PHY_BASE + PHY_DIG_RESET, 0);
  P->Io.Fence (P->Io.Context);
  Write (P, PHY_BASE + PHY_CTRL3, 4);
  Or (P, PHY_BASE + PHY_CLK_CFG1, BIT_(5) | BIT_(4));
  Write (P, PHY_BASE + PHY_RBUF_CTRL, 1);
  /* dsi_7nm_set_continuous_clock(): LT9611 uses continuous clock mode. */
  Or (P, PHY_BASE + PHY_LANE_CTRL1, BIT_(5) | BIT_(6));
  return CR_SUCCESS;
}


/* Linux dsi_pll_7nm_vco_unprepare + dsi_7nm_phy_disable, corroborated by
 * Qualcomm dsi_pll_5nm_disable/dsi_phy_hw_v4_0_disable. Gate the global clock
 * before PLL shutdown, retire bias before REFGEN/lanes, and honor both delays.
 * Register readbacks establish the disabled controls. No undocumented meaning
 * is assigned to PLL lock status after the analog block has been powered down. */
STATIC CR_STATUS FenceStatus(CR_STATUS S) {
  return S==CR_SUCCESS?S:CR_ERROR(S)?S:CR_NOT_READY;
}
STATIC CR_STATUS FenceRead(CONST CR_MDSS_DSI_PHY_FENCE_IO *I,UINT32 O,UINT32 *V) {
  CR_STATUS S=FenceStatus(I->Ready(I->Context));if(S!=CR_SUCCESS)return S;
  return FenceStatus(I->Read32(I->Context,O,V));
}
STATIC CR_STATUS FenceWait(CONST CR_MDSS_DSI_PHY_FENCE_IO *I,UINT32 O,UINT32 Mask) {
  for(UINT32 T=0;T<=1000;T+=10){
    UINT32 V=0;CR_STATUS S=FenceRead(I,O,&V);if(S!=CR_SUCCESS)return S;
    if(!(V&Mask))return CR_SUCCESS;
    if(T==1000)break;
    S=FenceStatus(I->Ready(I->Context));if(S!=CR_SUCCESS)return S;
    S=FenceStatus(I->DelayUs(I->Context,10));if(S!=CR_SUCCESS)return S;
  }
  return CR_TIMEOUT;
}
STATIC CR_STATUS FenceClear(CONST CR_MDSS_DSI_PHY_FENCE_IO *I,UINT32 O,UINT32 Mask) {
  UINT32 V=0;CR_STATUS S=FenceRead(I,O,&V);if(S!=CR_SUCCESS)return S;
  S=FenceStatus(I->Ready(I->Context));if(S!=CR_SUCCESS)return S;
  S=FenceStatus(I->Write32(I->Context,O,V&~Mask));if(S!=CR_SUCCESS)return S;
  S=FenceStatus(I->Ready(I->Context));if(S!=CR_SUCCESS)return S;
  S=FenceStatus(I->Fence(I->Context));if(S!=CR_SUCCESS)return S;
  return FenceWait(I,O,Mask);
}
CR_STATUS CrMdssPlatformFenceDsiPhy(CONST CR_MDSS_DSI_PHY_FENCE_IO *I) {
  if(!I||!I->Context||!I->Ready||!I->Read32||!I->Write32||!I->Fence||!I->DelayUs)
    return CR_INVALID_PARAMETER;
  if(I->PhyPhysical!=0xae94400ULL)return CR_UNSUPPORTED;
  if(I->PhyBytes<CR_MDSS_PHY_BYTES || I->PhyBytes>(UINT64)-1-I->PhyPhysical)
    return CR_BAD_BUFFER_SIZE;
  STATIC CONST struct {UINT32 Offset,Mask,Delay;} Steps[]={
    {PHY_LANE_CTRL1,BIT_(5)|BIT_(6),0},
    {PHY_CLK_CFG1,BIT_(5),0},
    {PHY_PLL_CTRL,BIT_(0),0},
    {PHY_RBUF_CTRL,0xff,0},
    {PLL_BASE+PLL_SYSTEM_MUXES,0xff,0},
    {PHY_CTRL0,BIT_(5),1},
    {LANE_BASE+0x14,0xff,0},
    {PHY_REFGEN_VOTE,0xff,2},
    {PHY_CTRL0,0x1f,0},
    {PHY_LANE_CTRL0,0xff,0},
    {PHY_CTRL0,0xff,0}
  };
  for(UINT32 N=0;N<ARRAY_COUNT(Steps);++N){
    CR_STATUS S=FenceClear(I,Steps[N].Offset,Steps[N].Mask);if(S!=CR_SUCCESS)return S;
    if(Steps[N].Delay){
      S=FenceStatus(I->Ready(I->Context));if(S!=CR_SUCCESS)return S;
      S=FenceStatus(I->DelayUs(I->Context,Steps[N].Delay));if(S!=CR_SUCCESS)return S;
    }
  }
  /* Recheck earlier controls too; dropped writes or a revoked/external owner
   * must not be mistaken for a completed transfer by the upper journal. */
  for(UINT32 N=0;N<ARRAY_COUNT(Steps);++N){
    CR_STATUS S=FenceWait(I,Steps[N].Offset,Steps[N].Mask);if(S!=CR_SUCCESS)return S;
  }
  return FenceStatus(I->Ready(I->Context));
}
STATIC CR_STATUS PlatformPhyReady(VOID *Context) {
  CR_MDSS_PLATFORM *P=Context;return P->Io.ResourceReady(P->Io.Context,P);
}
STATIC CR_STATUS PlatformPhyRead(VOID *Context,UINT32 O,UINT32 *V) {
  *V=Read(Context,O);return CR_SUCCESS;
}
STATIC CR_STATUS PlatformPhyWrite(VOID *Context,UINT32 O,UINT32 V) {
  Write(Context,O,V);return CR_SUCCESS;
}
STATIC CR_STATUS PlatformPhyFence(VOID *Context) {
  CR_MDSS_PLATFORM *P=Context;P->Io.Fence(P->Io.Context);return CR_SUCCESS;
}
STATIC CR_STATUS PlatformPhyDelay(VOID *Context,UINT32 Us) {
  CR_MDSS_PLATFORM *P=Context;P->Io.DelayUs(P->Io.Context,Us);return CR_SUCCESS;
}
CR_STATUS CrMdssPlatformDisablePhy(CR_MDSS_PLATFORM *P) {
  CR_MDSS_DSI_PHY_FENCE_IO I={P,0xae94400ULL,P->Io.PhyBytes,
    PlatformPhyReady,PlatformPhyRead,PlatformPhyWrite,PlatformPhyFence,PlatformPhyDelay};
  CR_STATUS S=CrMdssPlatformFenceDsiPhy(&I);
  if(S==CR_SUCCESS)P->PhyTouched=FALSE;
  return S;
}
