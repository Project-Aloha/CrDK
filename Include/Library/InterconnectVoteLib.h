/* Reserved absolute per-owner Linux-unit interconnect votes. MIT */
#pragma once
#include <Library/interconnect.h>

typedef struct {
  CONST CHAR8 *Provider;
  UINT32 SourceId, DestinationId;
} CR_ICB_PATH;
typedef struct {
  UINT64 Average, Peak; /* kB/s, exactly 1000 bytes/sec per unit */
  BOOLEAN Present;
} CR_ICB_VOTE_VALUE;
typedef struct {
  VOID *Owner;
  CR_ICB_PATH Path;
  INTERCONNECT_PATH_HANDLE Handle;
  CR_ICB_VOTE_VALUE Value;
} CR_ICB_VOTE_ENTRY;
typedef struct {
  InterconnectDeviceContext *Core;
  CR_ICB_VOTE_ENTRY *Entries;
  UINT32 Capacity;
  CR_MUTEX Lock;
  BOOLEAN Initialized;
} CR_ICB_VOTER;

/* PASSIVE; caller-owned nonpaged fixed storage, no allocations. Owner is a
   stable opaque identity; immutable provider strings and core/storage outlive
   all reservations and recovery. Lifecycle changes are externally serialized.
   Internal lock nests before the core's lock. Handles belong exclusively to
   this bank; legacy consumers may use independent handles on the same core.
   Reserve EVERY owner/path before permanent PEP publication: shared core has
   32 handles, and Resolve/Get/Validate never allocate or repair hardware.
   Failed initialization does not acquire handles. Failed release retains its
   reservation until retry. An absent vote is zero demand on a RESERVED path,
   so journal Close never prevents a later pure resource qualification. */
CR_STATUS CrIcbVotesInit(CR_ICB_VOTER *, InterconnectDeviceContext *,
                         CR_ICB_VOTE_ENTRY *, UINT32 Capacity);
CR_STATUS CrIcbVotesReserve(CR_ICB_VOTER *, VOID *Owner, CONST CR_ICB_PATH *);
CR_STATUS CrIcbVotesValidate(CR_ICB_VOTER *, VOID *Owner, CONST CR_ICB_PATH *);
/* Get copies this owner's logical value even during global rollback. */
CR_STATUS CrIcbVotesGet(CR_ICB_VOTER *, VOID *Owner, CONST CR_ICB_PATH *,
                        CR_ICB_VOTE_VALUE *, BOOLEAN *RecoveryPending);
CR_STATUS CrIcbVotesSet(CR_ICB_VOTER *, VOID *Owner, CONST CR_ICB_PATH *,
                        CONST CR_ICB_VOTE_VALUE *);
/* Only after the owner's journals have closed and callbacks drained. */
CR_STATUS CrIcbVotesReleaseOwner(CR_ICB_VOTER *, VOID *Owner);
CR_STATUS CrIcbVotesDrain(CR_ICB_VOTER *);
CR_STATUS CrIcbVotesUninit(CR_ICB_VOTER *);
/* Adapter teardown: after ALL requests drain and core Deinit clears all BCMs,
   forget now-invalid handles. Refuses initialized/pending hardware. Permanent
   broker keeps desired votes separately and re-reserves a new incarnation. */
CR_STATUS CrIcbVotesRetireAfterDeinit(CR_ICB_VOTER *);
