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
