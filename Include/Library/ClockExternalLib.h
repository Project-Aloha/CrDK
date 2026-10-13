/* Exclusive HDK DSI0 external-clock owner. SPDX-License-Identifier: MIT */
#pragma once
#include <oskal/cr_types.h>
#include <Library/clock.h>
#include <Library/MdssHandoffCrLib.h>
#include <Library/ClockExternalPlan.h>
typedef struct {
  ClockDriverContext *Core;
  ClockNode *Leaf[3], *Source[2], *Access[3], *Domain;
  VOID *Owner;
  UINT32 SavedSource[2][4], SavedDivider;
  BOOLEAN Initialized, Held[3], SourceSaved[2], DividerSaved;
  BOOLEAN Enabled, RecoveryPending;
  /* Optional second physical DSI group, reserved by the same permanent owner.
     It shares only actual access/domain leases, never a source/branch vote. */
  ClockNode *SecondLeaf[3], *SecondSource[2];
  UINT32 SavedSecondSource[2][4], SavedSecondDivider;
  BOOLEAN SecondPresent, SecondHeld[3], SecondSourceSaved[2], SecondDividerSaved;
  BOOLEAN SecondEnabled, SecondRecoveryPending;
  CR_CLOCK_DSI_PLAN Plans[2];
  BOOLEAN PlanValid[2];
  /* Native firmware baseline, never a synthetic ClockLib reference. The
     record and owner survive a failed retirement and a completed transfer. */
  ClockNode *NativeNode[12]; /* seven leaves followed by five private RCGs */
  VOID *NativeOwner;
  CR_MDSS_NATIVE_HANDOFF NativeRecord;
  UINT32 NativeMask, NativeTouched, NativeOff;
  BOOLEAN NativeComplete;
} CR_CLOCK_EXTERNAL;
/* Caller holds the provider's PASSIVE outer serialization around every API,
 * including Guard and all legacy/owner clock mutations. Init/Reserve/Get
 * are pure. No generic entry point may mutate a claimed leaf or its parents,
 * even when it presents the same owner token; Set is the sole producer.
 * Root native handoff must make the display path cold before Set. */
CR_STATUS CrClockExternalInit(CR_CLOCK_EXTERNAL *, ClockDriverContext *);
CR_STATUS CrClockExternalReserve(CR_CLOCK_EXTERNAL *, VOID *Owner);
CR_STATUS CrClockExternalGuard(CR_CLOCK_EXTERNAL *, ClockNode *Node);
CR_STATUS CrClockExternalGet(CR_CLOCK_EXTERNAL *, VOID *Owner, BOOLEAN *Enabled, BOOLEAN *Pending);
CR_STATUS CrClockExternalSet(CR_CLOCK_EXTERNAL *, VOID *Owner, UINT64 PixelHz, UINT64 ByteHz, BOOLEAN Enable);
/* Same exclusive reservation as the legacy fixed-HDK producer. Exact plan
   is retained before the first mutation; changing it while enabled/pending
   fails without IO. Off may pass NULL and always drains the retained plan. */
CR_STATUS CrClockExternalSetPlan(CR_CLOCK_EXTERNAL *, VOID *Owner, UINT32 Controller,
    CONST CR_CLOCK_DSI_PLAN *, BOOLEAN Enable);
CR_STATUS CrClockExternalGetPlan(CR_CLOCK_EXTERNAL *, VOID *Owner, UINT32 Controller,
    CR_CLOCK_DSI_PLAN *, BOOLEAN *Enabled, BOOLEAN *Pending);
/* Pure reservation conflict, including DSI1 when the exact graph exists. */
CR_STATUS CrClockExternalGroupConflict(CR_CLOCK_EXTERNAL *,ClockNode *);
CR_STATUS CrClockExternalReleaseOwner(CR_CLOCK_EXTERNAL *, VOID *Owner);
CR_STATUS CrClockExternalDrain(CR_CLOCK_EXTERNAL *);
CR_STATUS CrClockExternalUninit(CR_CLOCK_EXTERNAL *);
/* Whitelisted read-only DISPCC offsets9000/8138/80c0/814c, never a raw
 * register interface. Requires actual access/domain refs before any read. */
CR_STATUS CrClockExternalRead(CR_CLOCK_EXTERNAL *, VOID *Owner, UINT32 Offset, UINT32 *Value);
/* Pure conflict query, also used before accepting inactive generic votes. */
CR_STATUS CrClockExternalNativeConflict(CR_CLOCK_EXTERNAL *, ClockNode *);
/* Caller has the actual live native claim, exclusive group, D0 and its own
 * access/domain votes, and has stopped inherited DPU/DSI/DP/PHY/bridge users.
 * This only retires the named private clock gates and RCG force-on baseline;
 * it does not certify any other hardware fence. Same immutable record and
 * owner are required on retries. Failure retains all touched state and must
 * block access withdrawal. Completed retirement never restores firmware ON. */
CR_STATUS CrClockExternalNativeRetire(CR_CLOCK_EXTERNAL *, VOID *Owner,
    UINT64 TransferId, CONST CR_MDSS_NATIVE_HANDOFF *);
STATIC inline BOOLEAN CrClockExternalNativePending(CONST CR_CLOCK_EXTERNAL *E) {
  return E->NativeOwner && !E->NativeComplete;
}

STATIC inline BOOLEAN CrClockExternalNeedsAccess(CONST CR_CLOCK_EXTERNAL *E) {
  return CrClockExternalNativePending(E) || E->Enabled || E->RecoveryPending ||
    E->DividerSaved || E->SourceSaved[0] || E->SourceSaved[1] ||
    E->Held[0] || E->Held[1] || E->Held[2] || E->SecondEnabled ||
    E->SecondRecoveryPending || E->SecondDividerSaved ||
    E->SecondSourceSaved[0] || E->SecondSourceSaved[1] ||
    E->SecondHeld[0] || E->SecondHeld[1] || E->SecondHeld[2];
}
/* Legacy calls have no owner proof and cannot alter the access/domain votes
   sustaining this group. Absolute-owner votes remain independently shared. */
CR_STATUS CrClockExternalLegacyGuard(CR_CLOCK_EXTERNAL *, ClockNode *);
