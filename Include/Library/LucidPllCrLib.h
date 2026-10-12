/* Retained direct Lucid source owner, distinct from an FSM vote or Lucid EVO.
 * SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>

#define CR_LUCID_PLL_SM8250_DISP1 0x825001U
#define CR_LUCID_PLL_MMCX_CAL_LEVEL 192U
#define CR_LUCID_PLL_MMCX_RUN_LEVEL 64U
#define CR_LUCID_PLL_READY_MAPPING 1U
#define CR_LUCID_PLL_READY_RETIRED 2U
#define CR_LUCID_PLL_READY_CAL_RAIL 4U
#define CR_LUCID_PLL_READY_RUN_RAIL 8U
#define CR_LUCID_PLL_READY_CHILDREN_QUIET 16U

typedef struct {
  VOID *Context;
  UINT64 Physical;
  UINTN Bytes;
  /* Every call checks exclusive native incarnation, assigned MMIO, core bus
   * access and actual retained dependencies. RETIRED means the genuine native
   * display/clock claim completed, not a fabricated SM8450 record. QUIET means
   * no dependent RCG/branch can consume this source during configure/stop.
   * CAL_RAIL requires an applied retained vote meeting level192. RUN_RAIL
   * requires level64, which an applied calibration vote also satisfies;
   * merely retaining a failed provider token proves neither condition. */
  CR_STATUS (*Ready)(VOID *,UINT32 Requirements);
  CR_STATUS (*Read32)(VOID *,UINT32 Offset,UINT32 *Value);
  CR_STATUS (*Write32)(VOID *,UINT32 Offset,UINT32 Value);
  CR_STATUS (*Fence)(VOID *);
  CR_STATUS (*DelayUs)(VOID *,UINT32 Us);
  /* Two independently retained real MMCX votes. Level is a common voltage
   * level, NEVER a raw ARC selector. Provider translates through CMDDB and
   * holds the supplied board's parent dependencies: MX minimum is the actual
   * selected MMCX level minus1, rounded by MX CMDDB, with its CX enable vote.
   * On success Owned must equal On. On failure it reports actual retained
   * ownership; caller keeps provider/token storage alive through retries. */
  CR_STATUS (*MmcxVote)(VOID *,UINT32 MinimumLevel,BOOLEAN On,BOOLEAN *Owned);
} CR_LUCID_PLL_IO;

typedef struct {
  CR_LUCID_PLL_IO Io;
  UINT32 Profile;
  BOOLEAN Initialized, Touched, Ready, RecoveryPending;
  BOOLEAN CalibrationOwned, OperatingOwned, CalibrationCycleDone;
} CR_LUCID_PLL;

/* Init is pure. Only exact SM8250 DISPCC PLL1 at0xaf01000 is admitted.
 * The native provider owns serialization at PASSIVE_LEVEL; this library
 * owns one physical lifetime. Shared ClockLib integration must retain a
 * passive source transition outside its existing CR_LOCK: that spin lock
 * must never surround real MMCX/RPMh provider callbacks. */
CR_STATUS CrLucidPllInit(CR_LUCID_PLL *,CONST CR_LUCID_PLL_IO *,UINT32 Profile);
CR_STATUS CrLucidPllAcquire(CR_LUCID_PLL *);
CR_STATUS CrLucidPllCheck(CR_LUCID_PLL *);
CR_STATUS CrLucidPllRelease(CR_LUCID_PLL *);
CR_STATUS CrLucidPllUninit(CR_LUCID_PLL *);
STATIC inline BOOLEAN CrLucidPllNeedsAccess(CONST CR_LUCID_PLL *P) {
  return P && (P->Touched || P->CalibrationOwned || P->OperatingOwned ||
               P->RecoveryPending || P->Ready);
}
