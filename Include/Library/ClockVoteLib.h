/* Absolute per-owner clock/GDSC votes around the shared clock graph.
 * SPDX-License-Identifier: MIT */
#pragma once
#include <Library/clock.h>

typedef struct {
  UINT64 RateHz;
  BOOLEAN Present, Enabled;
} CR_CLOCK_VOTE_VALUE;
typedef struct {
  VOID *Owner;
  ClockNode *Node;
  CR_CLOCK_VOTE_VALUE Value;
  BOOLEAN Held, RecoveryPending, Reserved;
} CR_CLOCK_VOTE_ENTRY;
typedef struct {
  ClockDriverContext *Core;
  CR_CLOCK_VOTE_ENTRY *Entries;
  UINT32 Capacity;
  BOOLEAN Initialized;
} CR_CLOCK_VOTER;

/* PASSIVE; zero-initialize Voter. Storage is caller-owned and allocation-free.
   Serialize every voter operation AND direct graph operation under the same
   outer lock. Core, graph, storage and stable opaque owner identities outlive
   all votes and retained recovery. Init/Get/Validate perform no MMIO or votes.
   Validate checks the immutable graph/rate contract, not current availability.
   Rates must exactly match RCG tables. GDSCs require RateHz=0. Conflicting live
   rates fail safely; this layer does not infer a frequency aggregation policy. */
CR_STATUS CrClockVotesInit(CR_CLOCK_VOTER *, ClockDriverContext *,
                          CR_CLOCK_VOTE_ENTRY *, UINT32 Capacity);
CR_STATUS CrClockVotesValidate(CR_CLOCK_VOTER *, ClockNode *, UINT64 RateHz);
/* Reserve every owner/node in the complete policy before publication. These
   calls only validate metadata and claim fixed storage; they perform no IO.
   Repeated Reserve is idempotent. Absence retains a reservation until explicit
   ReleaseOwner/Drain, so journal Close cannot lose its future capacity proof.
   RequireReservation checks that proof without allocating or recovering. */
CR_STATUS CrClockVotesReserve(CR_CLOCK_VOTER *, VOID *Owner, ClockNode *, UINT64 RateHz);
CR_STATUS CrClockVotesRequireReservation(CR_CLOCK_VOTER *, VOID *Owner, ClockNode *);
/* Copies this owner's logical vote even while recovery is pending. Pending
   means hardware is uncertain; consumers must not access it as if ready. */
CR_STATUS CrClockVotesGet(CR_CLOCK_VOTER *, VOID *Owner, ClockNode *,
                         CR_CLOCK_VOTE_VALUE *, BOOLEAN *RecoveryPending);
/* Absolute and idempotent, including Present=FALSE to remove the logical vote.
   Unreserved users may allocate a slot on Set; permanent users must reserve
   and require the complete owner/node inventory before publication. A
   failed acquisition/release retains exactly its own cleanup reference.
   Recovery always withdraws that reference before retrying a new demand.
   Rate changes withdraw the old owner vote first and may fail if another
   owner has a different rate. Get retains the old logical vote on failure,
   allowing a journal to restore it through Set. */
CR_STATUS CrClockVotesSet(CR_CLOCK_VOTER *, VOID *Owner, ClockNode *,
                         CONST CR_CLOCK_VOTE_VALUE *);
CR_STATUS CrClockVotesReleaseOwner(CR_CLOCK_VOTER *, VOID *Owner);
/* Called after users drain. Failed release retains its entry and blocks
   Uninit; retry never withdraws another owner's already released reference. */
CR_STATUS CrClockVotesDrain(CR_CLOCK_VOTER *);
CR_STATUS CrClockVotesUninit(CR_CLOCK_VOTER *);
