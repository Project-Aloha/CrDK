/* SM8250 DPHY 4.1, single controller, selected HDK8245 command panel.
 * Copyright (c) 2016-2019, The Linux Foundation. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Hardware sequence derived from Android cad09e061ef6cf689a4a1e54d27562e0f042236a
 * techpack/display/{msm/dsi/dsi_phy_hw_v4_0.c,pll/dsi_pll_7nm.c}, cross-checked
 * against Linux a53261b60834ce617cc4f3c7723dda8285c59789 dsi_phy_7nm.c.
 * The native owner supplies the qualified PHY4.1 identity, exclusive mapping,
 * retired boot display, rails/refgen/core clocks and incarnation checks. No
 * guessed revision-ID encoding, rail or DISPCC programming belongs here.
 */
#include "Internal.h"

/* Supplied DT: PHY0 0xae94400/0x7c0; PLL0 0xae94900. The PHY mapping also
 * covers PERF_OPTIMIZE at +0x760, beyond the PLL node's short 0x260 window. */
#define PHY_BASE 0x0ae94400ULL
#define PHY_BYTES 0x7c0U
#define PLL(O) (0x500U + (O))
#define PHY_CALL(E) do { CR_STATUS Result = (E); if (Result != CR_SUCCESS) return Result; } while (0)
#define PHY_W(O,V) PHY_CALL(CmdChecked(D,CrMdssCmdPhy,(O),(V),0xff))

typedef struct { UINT32 Offset, Value; } PHY_SETTING;

STATIC CR_STATUS PhyAccess(CR_MDSS_CMD *D)
{
  if (!D || !D->Initialized || !D->Io.Ready || !D->Io.Read32 ||
      !D->Io.Write32 || !D->Io.Fence || !D->Io.DelayUs)
    return CR_INVALID_PARAMETER;
  if (D->Io.Physical[CrMdssCmdPhy] != PHY_BASE ||
      D->Io.Bytes[CrMdssCmdPhy] < PHY_BYTES)
    return CR_UNSUPPORTED;
  return CmdReady(D,CrMdssCmdPhy);
}

STATIC CR_STATUS PhyPlan(CR_MDSS_CMD *D)
{
  CR_MDSS_CMD_PLAN P;
  PHY_CALL(CrMdssCmdBuildPlanProfile(&D->Profile,&P));
  /* A forged or stale derived plan must not silently program another rate.
   * The 18-bit PLL fraction floors 654193088 Hz to 654193066.40625 Hz:
   * DEC=17, FRAC=0x2526, lane=327096533.203125 Hz (0.0331 ppm below request).
   * Plan clock fields remain nominal requests for the native clock provider. */
  if (D->Plan.VcoHz != P.VcoHz || D->Plan.LaneBitHz != P.LaneBitHz ||
      D->Plan.ByteHz != P.ByteHz || D->Plan.ByteInterfaceHz != P.ByteInterfaceHz ||
      D->Plan.PixelHz != P.PixelHz || D->Plan.PllPostDivider != 2 ||
      D->Plan.PhyPostDivider != 1 || D->Plan.PixelDivider != 3)
    return CR_UNSUPPORTED;
  for (UINT32 I=0; I<14; ++I)
    if (D->Plan.PhyTimings[I] != P.PhyTimings[I]) return CR_UNSUPPORTED;
  if (cr_memcmp(&D->Plan,&P,sizeof(P))) return CR_UNSUPPORTED;
  return CR_SUCCESS;
}

CR_STATUS CmdPhyStart(CR_MDSS_CMD *D)
{
  UINT32 Revision, Control;
  UINT64 Fraction;
  STATIC CONST UINT8 LaneControl[5]={0x40,0x40,0x40,0x46,0x41};
  /* Only the writable, rate-independent programming shared by mainline and
   * Android 4.1 is needed. Android's bulk reset table also writes status
   * registers; those are deliberately not treated as programmable state. */
  STATIC CONST PHY_SETTING Analog[]={
    {0x258,0x01},{0x240,0x08},{0x018,0x01},{0x004,0x03},
    {0x010,0x00},{0x020,0x00},{0x024,0x4e},{0x044,0x40},
    {0x068,0xba},{0x078,0x0c},{0x0a8,0x00},{0x0b8,0x00},
    {0x0c8,0x08},{0x160,0x0a},{0x168,0xc0},{0x170,0x84},
    {0x170,0x82},{0x178,0x4c},{0x190,0x80},{0x090,0x29},
    {0x090,0x2f},{0x094,0x2a},{0x094,0x3f},{0x260,0x22}
  };

  PHY_CALL(PhyAccess(D));
  if (D->PhyTouched || D->CleanupPending || D->HostTouched || D->DpuTouched ||
      D->ExternalOwned || D->CommandInFlight || D->FrameInFlight)
    return CR_NOT_READY;
  PHY_CALL(PhyPlan(D));
  PHY_CALL(CmdRead(D,CrMdssCmdPhy,0x03c,&Control));
  /* The owner must retire inherited scanout before entering this library. */
  if (Control & 1) return CR_NOT_READY;
  PHY_CALL(CmdPoll(D,CrMdssCmdPhy,0x140,1,1,1000));
  PHY_CALL(CmdRead(D,CrMdssCmdPhy,0x000,&Revision));

  /* Set before even the first callback: a failing write may have committed. */
  D->PhyTouched=TRUE;
  PHY_W(0x024,0x60);                 /* digital and PLL power-down deassert */
  PHY_W(0x03c,0);                    /* PLL core reset */
  /* A retained lock-high is not evidence that this new rate locked. Require
   * an unlocked observation under core reset before the later lock-high.
   * If this silicon retains the indication, cold admission must fail rather
   * than invent a successful PLL incarnation; Stop uses control fences. */
  PHY_CALL(CmdPoll(D,CrMdssCmdPhy,PLL(0x1b0),1,0,1000));
  PHY_W(0x01c,0);                    /* resync FIFO off */
  if ((Revision & 0xf0)==0x20) PHY_W(0x114,4);
  PHY_W(0x034,0x21); PHY_W(0x038,0x84); /* logical lanes 0,1,2,3 */
  PHY_W(0x020,0x53); PHY_W(0x110,0x5c);
  PHY_W(0x030,0); PHY_W(0x10c,0); PHY_W(0x0ec,0x88);
  PHY_W(0x104,0); PHY_W(0x0f4,0x3d); PHY_W(0x0f8,0x39);
  PHY_W(0x100,0x55);
  PHY_W(0x024,0x7f); PHY_W(0x0a0,0x1f);
  PHY_W(0x02c,0x40);                /* DPHY full-rate mode */
  PHY_W(0x014,0);                   /* standalone internal PLL */
  for (UINT32 I=0; I<14; ++I) PHY_W(0x0b4+4*I,D->Plan.PhyTimings[I]);
  for (UINT32 I=0; I<5; ++I) {
    PHY_W(0x214+0x80*I,0); PHY_W(0x210+0x80*I,0);
  }
  PHY_W(0x214,3);                   /* LPRX/CDRX only on logical lane 0 */
  for (UINT32 I=0; I<5; ++I) {
    PHY_W(0x200+0x80*I,0); PHY_W(0x204+0x80*I,0);
    PHY_W(0x208+0x80*I,I==4 ? 0x8a : 0x0a);
    PHY_W(0x218+0x80*I,LaneControl[I]);
  }

  /* PLL bias and prescaler: the board's real reference is 19.2 MHz. */
  PHY_W(PLL(0x028),0xc0);
  PHY_CALL(CmdDelay(D,CrMdssCmdPhy,1)); /* >= Android's 250 ns bias delay */
  Fraction=D->Plan.VcoHz*262144ULL/38400000ULL;
  PHY_W(PLL(0x0bc),0x12);
  PHY_W(PLL(0x0e0),(UINT32)(Fraction>>18));
  PHY_W(PLL(0x0e4),(UINT32)(Fraction&0xff));
  PHY_W(PLL(0x0e8),(UINT32)((Fraction>>8)&0xff));
  PHY_W(PLL(0x0ec),(UINT32)((Fraction>>16)&3));
  PHY_W(PLL(0x158),0x40); PHY_W(PLL(0x194),6);
  PHY_W(PLL(0x250),0x10); PHY_W(PLL(0x248),0xa0);
  for (UINT32 I=0; I<sizeof(Analog)/sizeof(Analog[0]); ++I)
    PHY_W(PLL(Analog[I].Offset),Analog[I].Value);
  /* DT has ssc-mode but no ssc-en. Explicit off prevents inherited spread. */
  PHY_W(PLL(0x150),0);
  PHY_W(PLL(0x154),1);               /* output divider encoding log2(2) */
  PHY_W(0x010,0x31);                /* bit divider 1, pixel divider 3 */
  PHY_W(0x014,1);                   /* pixel parent bit clock / 2 */
  PHY_CALL(CmdFence(D,CrMdssCmdPhy)); /* mux must precede PLL start */
  PHY_W(0x03c,1);
  PHY_CALL(CmdPoll(D,CrMdssCmdPhy,PLL(0x1b0),1,1,5000));
  PHY_W(0x128,1); PHY_W(0x128,0);   /* digital reset before global clocks */
  PHY_W(0x030,4); PHY_W(0x014,0x31);
  PHY_W(0x01c,1);
  /* Android clears CLK_EN_SEL after the retiming FIFO is synchronized. */
  PHY_W(0x014,0x21);
  return CR_SUCCESS;
}

STATIC CR_STATUS PhyOff(CR_MDSS_CMD *D,UINT32 Offset,UINT32 Mask)
{
  PHY_CALL(CmdUpdate(D,CrMdssCmdPhy,Offset,Mask,0));
  PHY_CALL(CmdFence(D,CrMdssCmdPhy));
  return CmdPoll(D,CrMdssCmdPhy,Offset,Mask,0,1000);
}

CR_STATUS CmdPhyStop(CR_MDSS_CMD *D)
{
  UINT32 Value;
  STATIC CONST PHY_SETTING Off[]={
    {0x0a4,0x60},{0x014,0x20},{0x03c,1},{0x01c,0xff},
    {PLL(0x028),0xff},{0x024,0x20},{0x214,0xff},
    {0x024,0x1f},{0x0a0,0xff},{0x024,0xff}
  };
  if (!D || !D->Initialized) return CR_INVALID_PARAMETER;
  if (!D->PhyTouched) return CR_SUCCESS;
  /* Dependencies cannot be released from a start-failure shortcut. */
  if (D->HostTouched || D->DpuTouched || D->DscTouched || D->ExternalOwned ||
      D->CommandInFlight || D->FrameInFlight || D->AutoRefresh)
    return CR_NOT_READY;
  PHY_CALL(PhyAccess(D));
  for (UINT32 I=0; I<sizeof(Off)/sizeof(Off[0]); ++I) {
    PHY_CALL(PhyOff(D,Off[I].Offset,Off[I].Value));
    if (Off[I].Offset==0x024 && Off[I].Value==0x20)
      PHY_CALL(CmdDelay(D,CrMdssCmdPhy,1));
  }
  /* A later write must not have reactivated an earlier gate. Read controls,
   * never infer physical off from the possibly latched PLL lock indicator. */
  PHY_CALL(CmdFence(D,CrMdssCmdPhy));
  for (UINT32 I=0; I<sizeof(Off)/sizeof(Off[0]); ++I) {
    PHY_CALL(CmdRead(D,CrMdssCmdPhy,Off[I].Offset,&Value));
    if (Value & Off[I].Value) return CR_DEVICE_ERROR;
  }
  D->PhyTouched=FALSE;
  return CR_SUCCESS;
}
