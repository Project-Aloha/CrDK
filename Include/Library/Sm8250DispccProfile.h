/* Unused, immutable HDK8245 DSI0 clock description.
 * SPDX-License-Identifier: GPL-2.0-only
 * Facts from Qualcomm Android Kona and Linux SM8250 clock drivers.
 * This is not a ClockNode ABI, a hardware lease, or a native retirement proof.
 */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>

#define CR_SM8250_DISPCC_HDK8245_DSI0 0x82450001U
#define CR_SM8250_DISPCC_CONTROLLER_COUNT 2U
#define CR_SM8250_DISPCC_RCG_COUNT 6U
#define CR_SM8250_DISPCC_BRANCH_COUNT 12U

/* Local description indices only; never cast to a ClockLib controller ID. */
typedef enum {
  CrSm8250DispccGcc = 0,
  CrSm8250DispccDisp = 1
} CR_SM8250_DISPCC_CONTROLLER_ID;

typedef enum {
  CrSm8250DispccNoParent = 0,
  CrSm8250DispccXo,
  CrSm8250DispccPll1,
  CrSm8250DispccPhy0Byte,
  CrSm8250DispccPhy0Pixel,
  CrSm8250DispccAhbSource,
  CrSm8250DispccMdpSource,
  CrSm8250DispccVsyncSource,
  CrSm8250DispccEsc0Source,
  CrSm8250DispccByte0Source,
  CrSm8250DispccPixel0Source,
  CrSm8250DispccByte0Divider
} CR_SM8250_DISPCC_SOURCE_ID;

/* These describe source behavior, not values to program into registers. */
typedef enum {
  CrSm8250DispccHaltPoll = 0,
  CrSm8250DispccHaltDelay = 1,
  CrSm8250DispccHaltVoted = 2,
  CrSm8250DispccProbeEnable = 3
} CR_SM8250_DISPCC_HALT;

typedef struct {
  CONST CHAR8 *Name;
  CONST CHAR8 *Compatible;
  UINT64 Physical;
  UINT32 Bytes;
} CR_SM8250_DISPCC_WINDOW;

typedef struct {
  CONST CHAR8 *Name;
  CR_SM8250_DISPCC_SOURCE_ID Id;
  CR_SM8250_DISPCC_SOURCE_ID Parent;
  CONST CHAR8 *ParentName;
  UINT32 CommandOffset;
  UINT8 HidWidth, MndWidth, Selector;
  /* HID stores 2 * divider - 1. External clocks use unity HID/M/N;
   * a zero FixedHz requires a separately validated live PHY clock plan. */
  UINT8 HidValue;
  UINT16 M, N;
  UINT64 FixedHz;
  BOOLEAN AndroidSafeConfig, AndroidHardwareControl;
} CR_SM8250_DISPCC_RCG;

typedef struct {
  CONST CHAR8 *Name;
  CR_SM8250_DISPCC_CONTROLLER_ID Controller;
  CR_SM8250_DISPCC_SOURCE_ID Parent;
  UINT32 EnableOffset, EnableMask, HaltOffset;
  UINT32 HwcgOffset, HwcgMask;
  CR_SM8250_DISPCC_HALT AndroidHalt, MainlineHalt;
  BOOLEAN AndroidCritical;
} CR_SM8250_DISPCC_BRANCH;

typedef struct {
  CONST CHAR8 *Name;
  CR_SM8250_DISPCC_SOURCE_ID Id, Parent;
  UINT32 Offset;
  UINT8 Shift, Width, Divisor;
  BOOLEAN ReadOnly;
} CR_SM8250_DISPCC_DIVIDER;

typedef struct {
  CONST CHAR8 *Name;
  UINT32 Offset;
  BOOLEAN FdtHardwareTrigger, FdtRetainRegisters;
  BOOLEAN MainlineHardwareControl, MainlineRetainFlipFlops;
  UINT8 MainlineEnableRestWait, MainlineEnableFewWait, MainlineClockDisableWait;
} CR_SM8250_DISPCC_GDSC;

typedef struct {
  CONST CHAR8 *Name, *ParentName;
  UINT32 Offset, DirectOwnerProfile;
  UINT64 ReferenceHz, OutputHz;
} CR_SM8250_DISPCC_PLL;

typedef struct {
  CONST CHAR8 *Name, *ParentName, *RpmhResource;
  UINT64 BoardHz, OutputHz;
  UINT32 Divider, EnableOffset, OnValue;
  /* Exact Kona clk-rpmh clock vote, distinct from regulator common levels.
   * Resolve its address in CMDDB; retain and aggregate the real XO lease. */
  BOOLEAN ActiveSet, WakeSet, SleepSet;
} CR_SM8250_DISPCC_XO;

typedef struct {
  CONST CHAR8 *SupplyName, *MmcxResource, *MxResource, *CxResource;
  UINT32 MmcxBoardMinimum, MmcxCalibrationMinimum, MmcxOperatingMinimum;
  INT32 MxDropoutFromSelectedMmcx;
  UINT32 CxParentMinimum;
  /* Common voltage levels, not raw ARC selectors or microvolts. Both sets
   * match FDT qcom,set=3; the provider must resolve CMDDB and retain parents.
   * MX minimum uses the actual selected MMCX level, not the requested floor. */
  BOOLEAN ActiveSet, SleepSet;
} CR_SM8250_DISPCC_RAILS;

typedef struct {
  UINT32 Id, Soc, Board, DsiControllerMask;
  CONST CHAR8 *AndroidCommit, *MainlineCommit, *FdtSha256;
  CR_SM8250_DISPCC_WINDOW Controllers[CR_SM8250_DISPCC_CONTROLLER_COUNT];
  CR_SM8250_DISPCC_RCG Rcgs[CR_SM8250_DISPCC_RCG_COUNT];
  CR_SM8250_DISPCC_BRANCH Branches[CR_SM8250_DISPCC_BRANCH_COUNT];
  CR_SM8250_DISPCC_DIVIDER Byte0Divider;
  CR_SM8250_DISPCC_GDSC Gdsc;
  CR_SM8250_DISPCC_PLL Pll1;
  CR_SM8250_DISPCC_XO Xo;
  CR_SM8250_DISPCC_RAILS Rails;
} CR_SM8250_DISPCC_PROFILE;

/* Pure lookup. Unknown IDs return CR_UNSUPPORTED and clear Out. No target
 * registration, MMIO, source adoption, rail voting or firmware handoff occurs.
 * This selected runtime subset excludes DSI1/DP/eDP/rotator/RSCC/PLL0. It is
 * insufficient to retire a stock route that may still use those resources.
 * Branch AndroidHalt is the pinned Android policy; MainlineHalt records real
 * differences for review, and does not silently override that policy. */
CR_STATUS CrSm8250DispccGetProfile(
  UINT32 Id, CONST CR_SM8250_DISPCC_PROFILE **Out);
