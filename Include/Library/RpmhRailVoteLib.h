/* Absolute fixed-contract regulator leases for per-owner journals. MIT */
#pragma once
#include <Library/RpmhRailLib.h>
typedef struct {
  UINT32 VoltageMv;
  UINT8 Mode;
  BOOLEAN Present, Enabled;
} CR_RPMH_RAIL_VALUE;
typedef struct {
  VOID *Owner;
  CR_RPMH_RAIL *Rail;
  CR_RPMH_RAIL_VALUE Value;
  BOOLEAN Held, RecoveryPending, Reserved;
} CR_RPMH_RAIL_ENTRY;
typedef struct {
  CR_RPMH_RAIL_IO Io;
  CR_RPMH_RAIL *Rails;
  UINT32 RailCount;
  CR_RPMH_RAIL_ENTRY *Entries;
  UINT32 Capacity;
  BOOLEAN Initialized;
} CR_RPMH_RAIL_VOTER;

/* PASSIVE, caller-owned nonpaged fixed storage. ALL raw/graph/bank operations
   share one outer serialization. Immutable qualified rail inventory/profiles,
   IO, entries and stable opaque owners outlive recovery. Init/Validate/Get
   never look up addresses or vote. Qualification requires cached nonzero
   addresses for the entire parent chain; optional unresolved rails decline.
   A live rail has one exact voltage/mode contract shared by compatible owners.
   Conflicts fail rather than inventing a voltage/mode aggregation policy.
   Set is absolute; repeated enable adds no extra lease. Failed writes/cleanup
   keep the old logical value and their own cleanup lease for journal retry. */
CR_STATUS CrRpmhRailVotesInit(CR_RPMH_RAIL_VOTER *, CONST CR_RPMH_RAIL_IO *,
    CR_RPMH_RAIL *Rails, UINT32 RailCount, CR_RPMH_RAIL_ENTRY *, UINT32 Capacity);
CR_STATUS CrRpmhRailVotesValidate(CR_RPMH_RAIL_VOTER *, CR_RPMH_RAIL *, UINT32 VoltageMv, UINT8 Mode);
/* Reserve the whole owner/rail policy before publication. No IO or address
   lookup is performed. Repeated Reserve is idempotent; an absent logical vote
   retains its slot until explicit ReleaseOwner/Drain. RequireReservation is
   pure. Unreserved Set users retain the fallible on-demand storage contract. */
CR_STATUS CrRpmhRailVotesReserve(CR_RPMH_RAIL_VOTER *, VOID *Owner, CR_RPMH_RAIL *, UINT32 VoltageMv, UINT8 Mode);
CR_STATUS CrRpmhRailVotesRequireReservation(CR_RPMH_RAIL_VOTER *, VOID *Owner, CR_RPMH_RAIL *);
CR_STATUS CrRpmhRailVotesGet(CR_RPMH_RAIL_VOTER *, VOID *Owner, CR_RPMH_RAIL *,
                            CR_RPMH_RAIL_VALUE *, BOOLEAN *RecoveryPending);
CR_STATUS CrRpmhRailVotesSet(CR_RPMH_RAIL_VOTER *, VOID *Owner, CR_RPMH_RAIL *, CONST CR_RPMH_RAIL_VALUE *);
CR_STATUS CrRpmhRailVotesReleaseOwner(CR_RPMH_RAIL_VOTER *, VOID *Owner);
CR_STATUS CrRpmhRailVotesDrain(CR_RPMH_RAIL_VOTER *);
CR_STATUS CrRpmhRailVotesUninit(CR_RPMH_RAIL_VOTER *);
