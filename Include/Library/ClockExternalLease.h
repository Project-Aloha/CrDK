/* Display external clocks with private prerequisite votes. SPDX-License-Identifier: MIT */
#pragma once
#include <Library/ClockExternalLib.h>

typedef struct {
  CR_CLOCK_EXTERNAL External;
  UINT64 Incarnation;
  BOOLEAN AccessOwned[3], DomainOwned, Ready, CleanupPending;
} CR_CLOCK_EXTERNAL_LEASE;

/* Caller serializes these APIs and all legacy clock mutations together.
   Rail/interconnect and PHY lifetime remain the display caller's responsibility.
   Owned includes an exclusive reservation retained after a failed acquisition. */
CR_STATUS CrClockExternalLeaseInit(CR_CLOCK_EXTERNAL_LEASE *, ClockDriverContext *);
CR_STATUS CrClockExternalLeaseReserve(CR_CLOCK_EXTERNAL_LEASE *, VOID *, BOOLEAN *Owned);
CR_STATUS CrClockExternalLeaseSetPlan(CR_CLOCK_EXTERNAL_LEASE *, VOID *, UINT32,
    CONST CR_CLOCK_DSI_PLAN *, BOOLEAN, BOOLEAN *Owned);
CR_STATUS CrClockExternalLeaseGetPlan(CR_CLOCK_EXTERNAL_LEASE *, VOID *, UINT32,
    CR_CLOCK_DSI_PLAN *, BOOLEAN *, BOOLEAN *, UINT64 *Incarnation);
CR_STATUS CrClockExternalLeaseRelease(CR_CLOCK_EXTERNAL_LEASE *, VOID *);
CR_STATUS CrClockExternalLeaseGuard(CR_CLOCK_EXTERNAL_LEASE *, ClockNode *);
