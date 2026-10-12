/* Exclusive mapped ClockLib DSI external-clock owner. SPDX-License-Identifier: MIT */
#pragma once
#include <oskal/cr_types.h>
#include <Library/clock.h>
#include <Library/MdssHandoffCrLib.h>
#include <Library/ClockExternalPlan.h>
#define CR_CLOCK_EXTERNAL_SOC_SM8250 0x8250U
#define CR_CLOCK_EXTERNAL_SOC_SM8350 0x8350U
#define CR_CLOCK_EXTERNAL_SOC_SM8450 0x8450U
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
  /* Explicit provider-selected immutable layout. Admission validates the
     actual mapped graph; this value never grants native/DMA ownership. */
  UINT32 Soc;
  /* Stopped inherited DSI0/1 baseline. A partial attempt retains the exact
     owner and generation; completed retirement never writes the group again. */
  VOID *InheritedDsiOwner;
  UINT64 InheritedDsiGeneration;
  UINT32 InheritedDsiTouched, InheritedDsiOff;
  BOOLEAN InheritedDsiComplete;
  UINT64 BootGeneration;
  BOOLEAN CoreResetTouched, CoreResetComplete, FunctionalTouched, FunctionalComplete;
  /* Permanent caller supply qualifier and direct source lifetime; never
     fabricate a ClockLib reference for inherited or cold analogue state. */
  CR_COLD_PLL FunctionalPll;
  CR_CLOCK_PLL_SUPPLY_CHECK FunctionalPllSupply;
  VOID *FunctionalPllSupplyContext;
  BOOLEAN FunctionalPllInherited;
} CR_CLOCK_EXTERNAL;
/* Caller holds the provider's PASSIVE outer serialization around every API,
 * including Guard and all legacy/owner clock mutations. Init/Reserve/Get
 * are pure. No generic entry point may mutate a claimed leaf or its parents,
 * even when it presents the same owner token; Set is the sole producer.
 * Root native handoff must make the display path cold before Set. */
CR_STATUS CrClockExternalInit(CR_CLOCK_EXTERNAL *, ClockDriverContext *);
/* Pure graph admission for a known SoC, without reads, writes or allocation.
   Legacy Init remains the exact SM8450 entry point. Missing/partial resources
   are rejected; no target graph is manufactured by this service. */
CR_STATUS CrClockExternalInitForSoc(CR_CLOCK_EXTERNAL *,ClockDriverContext *,UINT32 Soc);
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
/* Profile-whitelisted GDSC, byte/pixel config and byte-divider readback only.
 * Requires actual access/domain refs before any read. */
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
/* SM8250/SM8350 only. Caller owns the exclusive group plus supply/access
   floors and has actually stopped both hosts/PHYs and all inherited MDSS
   writers. This retires only six DSI gates and four private RCG force-on
   bits; no reset, DMA or SMMU fence is inferred. Generation is immutable
   across partial retries. Both physical groups must be admitted. */
CR_STATUS CrClockExternalRetireInheritedDsi(CR_CLOCK_EXTERNAL *,VOID *Owner,UINT64 Generation);
/* Read-only proof used before SMMU transfer/cold initialization. Completed
   Retire itself is a one-shot result and cannot stop a new owned pipeline. */
CR_STATUS CrClockExternalCheckInheritedDsiOff(CR_CLOCK_EXTERNAL *,VOID *Owner,UINT64 Generation);
CR_STATUS CrClockExternalResetCore(CR_CLOCK_EXTERNAL *,VOID *Owner,UINT64 Generation,BOOLEAN Assert);
CR_STATUS CrClockExternalCheckCoreResetReleased(CR_CLOCK_EXTERNAL *,VOID *Owner,UINT64 Generation);
CR_STATUS CrClockExternalRetireInheritedFunctional(CR_CLOCK_EXTERNAL *,VOID *Owner,UINT64 Generation);
/* SupplyCheck proves retained acknowledged floors and caller ICC/rails;
   cold writes require MMCX>=192 with its MX/CX/XO dependencies. It may neither mutate
   ClockLib nor call this service; the provider already holds serialization.
   Context survives failed cleanup. Live sources are qualified read-only;
   only exact SM8350 cold direct PLL1 may be configured after physical quiet. */
CR_STATUS CrClockExternalPrepareFunctionalPll(CR_CLOCK_EXTERNAL *,VOID *Owner,UINT64 Generation,
    CR_CLOCK_PLL_SUPPLY_CHECK,VOID *SupplyContext);
CR_STATUS CrClockExternalCheckFunctionalPll(CR_CLOCK_EXTERNAL *,VOID *Owner,UINT64 Generation);
CR_STATUS CrClockExternalReleaseFunctionalPll(CR_CLOCK_EXTERNAL *,VOID *Owner);
STATIC inline BOOLEAN CrClockExternalNativePending(CONST CR_CLOCK_EXTERNAL *E) {
  return (E->NativeOwner && !E->NativeComplete) ||
    (E->InheritedDsiOwner && !E->InheritedDsiComplete) ||
    (E->CoreResetTouched && !E->CoreResetComplete) ||
    (E->FunctionalTouched && !E->FunctionalComplete);
}

STATIC inline BOOLEAN CrClockExternalNeedsAccess(CONST CR_CLOCK_EXTERNAL *E) {
  return CrClockExternalNativePending(E) || E->Enabled || E->RecoveryPending ||
    CrColdPllNeedsAccess(&E->FunctionalPll) ||
    E->DividerSaved || E->SourceSaved[0] || E->SourceSaved[1] ||
    E->Held[0] || E->Held[1] || E->Held[2] || E->SecondEnabled ||
    E->SecondRecoveryPending || E->SecondDividerSaved ||
    E->SecondSourceSaved[0] || E->SecondSourceSaved[1] ||
    E->SecondHeld[0] || E->SecondHeld[1] || E->SecondHeld[2];
}
/* Legacy calls have no owner proof and cannot alter the access/domain votes
   sustaining this group. Absolute-owner votes remain independently shared. */
CR_STATUS CrClockExternalLegacyGuard(CR_CLOCK_EXTERNAL *, ClockNode *);
