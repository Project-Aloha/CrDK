/* Read-only SM8350 HDK inherited DSI0 video inventory.
 * SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>

#define CR_HDK8350_DISPLAY_REVISION 1U
#define CR_HDK8350_DISPLAY_MAX_READS 2048U

typedef struct {
  UINT32 Top, Intf, Dsc, Wb, Cwb, Cdm, Merge, Flush, Reset, IntfMaster;
  UINT32 Fetch, Layer[6][4];
} CR_HDK8350_DISPLAY_CTL;
typedef struct {
  UINT32 SourceSize, SourceXy, OutputSize, OutputXy, Address[4], Stride[2];
  UINT32 Format, Unpack, OpMode, Secure, MultiRect, Decimation, Exclusion;
  UINT32 PixelExtension[9], Scaler, Csc;
} CR_HDK8350_DISPLAY_SOURCE;
typedef struct {
  UINT32 Enable, Config, Config2, Status, Hsync, VPeriod, VPulse;
  UINT32 HDisplay, VStart, VEnd, HActive, VActiveStart, VActiveEnd;
  UINT32 DataHDisplay, DataHActive, Polarity, PanelFormat, CountEnable;
  UINT32 FrameCount, LineCount, Mux, TearEnable, AutoRefresh, CommandTrigger;
  UINT32 TestEnable;
} CR_HDK8350_DISPLAY_INTF;
typedef struct {
  UINT32 Version, Control, Status, VideoConfig, ActiveH, ActiveV, Total;
  UINT32 Split, LaneControl, LaneSwap, Cphy, Reset;
} CR_HDK8350_DISPLAY_HOST;
typedef struct {
  UINT32 Control, Pll, PllStatus, Clock, Buffer, Lanes, ClockLane, Status;
  UINT32 Global, LaneMap[2];
} CR_HDK8350_DISPLAY_PHY;
typedef struct {
  UINT32 DpuVersion, InterfaceSelect, SplitEnable, SplitUpper, SplitLower;
  UINT32 PpbControl[4], PpbConfig[2], DceSelect, LmOutput[6], LmMode[6];
  CR_HDK8350_DISPLAY_CTL Ctl[6];
  CR_HDK8350_DISPLAY_SOURCE Source[8]; /* VIG0..3, DMA0..3, DPU7 catalog. */
  CR_HDK8350_DISPLAY_INTF Intf[4];
  CR_HDK8350_DISPLAY_HOST Host[2];
  CR_HDK8350_DISPLAY_PHY Phy[2];
  UINT32 DispGdsc;
  UINT32 GccDisplay[4]; /* DISP_AHB, DISP_XO, DISP_HF_AXI, DISP_SF_AXI. */
  UINT32 DispClock[7]; /* AHB, MDP, LUT, VSYNC, BYTE0, BYTE0_INTF, PCLK0. */
  UINT32 DsiClockConfig[3]; /* BYTE0_CFG, PCLK0_CFG, BYTE0_DIV. */
  UINT32 DsiClockCommand[2]; /* BYTE0_CMD, PCLK0_CMD; pending UPDATE rejects. */
} CR_HDK8350_DISPLAY_RAW;
typedef struct {
  VOID *Context;
  /* Caller has already proved every window mapped/powered and excluded
   * competing software writers. A callback failure immediately stops reads.
   * This library never enables resources, writes MMIO, or acknowledges IRQs. */
  CR_STATUS (*Read32)(VOID *, UINT64 Physical, UINT32 *Value);
} CR_HDK8350_DISPLAY_IO;
typedef struct {
  UINT32 Revision, Reads;
  BOOLEAN Stable;
  CR_HDK8350_DISPLAY_RAW Raw;
  /* Admission clears these before any rejection. Device is an inherited
   * IOVA, never proof of CPU physical mapping. The caller must prove the
   * entire byte range through the current SMMU before framebuffer access. */
  UINT32 Width, Height, Stride, SourceOffset, Format, Unpack;
  UINT32 RedMask, GreenMask, BlueMask, ReservedMask;
  UINT64 Device, Bytes;
} CR_HDK8350_DISPLAY_OBSERVATION;

/* Two finite sweeps; complete unsupported inventory remains diagnostic.
 * Stable compares configuration, excluding explicitly dynamic counters and
 * engine status. Read errors preserve partial raw values but no revision. */
CR_STATUS CrHdk8350DisplayReadInventory(CONST CR_HDK8350_DISPLAY_IO *,
                                     CR_HDK8350_DISPLAY_OBSERVATION *);
/* Strict alpha-disabled base-stage VIG0 or DMA0 -> CTL0/LM0/INTF1 ->
 * DSI0/PHY0, 1080p60. Higher-stage blending/alpha ownership is not qualified.
 * No bridge identity, HDMI pixels, hardware ownership or SMMU proof claimed.
 * Frame/line counters are exposed for the caller's bounded progress check. */
CR_STATUS CrHdk8350DisplayAdmitPreserved(CR_HDK8350_DISPLAY_OBSERVATION *);
