/* Fixed, already configured Lucid-family PLL vote. SPDX-License-Identifier: MIT */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>
#include <oskal/common.h>
typedef struct {
  UINTN Base;
  UINT32 Size, VoteRegister, VoteMask, ModeRegister, UserRegister;
  UINT32 LRegister, AlphaRegister, FsmMask, LockMask, CalibrationPendingMask;
  /* Mask/value zero selects the MAIN output alone. A divided-output consumer
     supplies its exact USER divider dependency; the library never writes it. */
  UINT32 PostDividerMask, PostDividerValue, ExpectedL, ExpectedAlpha;
  UINT64 ParentHz, FrequencyHz;
  /* LUCID/TRION FSM is in MODE; zero retains the LUCID EVO USER contract.
     LockMask describes the required MODE acknowledgement (ACTIVE for LUCID,
     LOCK for EVO). Neither acknowledgement grants analogue ownership. */
  BOOLEAN FsmInModeRegister;
} CR_FIXED_PLL_DESCRIPTION;
typedef struct {
  CR_FIXED_PLL_DESCRIPTION Description;
  BOOLEAN Initialized, Owned, Ready, RestoreVote;
} CR_FIXED_PLL_VOTE;
/* Caller serializes all operations and retains the assigned mapping through
 * Owned, including failed enable/restore. No analogue configuration is written.
 * Only the exact local vote bit is modified; its preexisting value is restored.
 * Init refuses a live or previously initialized object. */
CR_STATUS CrFixedPllInit(CR_FIXED_PLL_VOTE *, CONST CR_FIXED_PLL_DESCRIPTION *);
CR_STATUS CrFixedPllAcquire(CR_FIXED_PLL_VOTE *);
CR_STATUS CrFixedPllCheck(CR_FIXED_PLL_VOTE *);
CR_STATUS CrFixedPllRelease(CR_FIXED_PLL_VOTE *);
/* A limited inherited direct Lucid MAIN output, with no vote or analogue writes.
   USER MAIN enable is required; unused EVEN/ODD output gates are preserved
   and grant no divided-output contract.
   ParentHz is immutable target XO authority. The resource owner must first
   obtain acknowledged supply/access floors outside the ClockLib spinlock.
   This checker grants no cold-start or firmware ownership authority. */
typedef struct {
  UINTN Base;
  UINT32 Size, ExpectedL, ExpectedAlpha;
  UINT64 ParentHz, FrequencyHz;
  BOOLEAN FsmInModeRegister;
} CR_INHERITED_PLL_DESCRIPTION;
CR_STATUS CrInheritedPllCheck(CONST CR_INHERITED_PLL_DESCRIPTION *,UINT64 RateHz);
/* Explicit direct cold owner, separate from the pure inherited qualifier.
 * Only the public SM8350 DISPCC PLL1 Lucid5LPE profile is qualified. Caller
 * serializes at PASSIVE level, holds the assigned mapping and a real retained
 * acknowledged MMCX>=192 supply floor throughout configure/run/stop. Ready
 * requalifies those suppliers before each IO; Quiet additionally proves every
 * dependent branch/source stopped. No supplier callback runs under a spinlock.
 * The callback/context must survive failed prepare/release until final drain. */
#define CR_COLD_PLL_SM8350_DISP1 0x835001U
typedef CR_STATUS (*CR_COLD_PLL_READY)(VOID *Context,BOOLEAN Quiet);
typedef CR_STATUS (*CR_CLOCK_PLL_SUPPLY_CHECK)(VOID *Context);
typedef struct {
  UINTN Base;
  UINT32 Size,Profile;
  CR_COLD_PLL_READY ReadyCheck;
  VOID *Context;
  BOOLEAN Initialized,ColdClaimed,Touched,Ready,Pending,CalibrationDone;
} CR_COLD_PLL;
CR_STATUS CrColdPllInit(CR_COLD_PLL *,UINTN Base,UINT32 Size,UINT32 Profile,
    CR_COLD_PLL_READY,VOID *Context);
CR_STATUS CrColdPllPrepare(CR_COLD_PLL *);
CR_STATUS CrColdPllCheck(CR_COLD_PLL *);
CR_STATUS CrColdPllRelease(CR_COLD_PLL *);
STATIC inline BOOLEAN CrColdPllNeedsAccess(CONST CR_COLD_PLL *P) {
  return P && (P->Touched || P->Ready || P->Pending);
}
