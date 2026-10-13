/* Data-only native display ownership publication. SPDX-License-Identifier: MIT */
#pragma once
#include <Interface/CrMdssHandoff.h>
#include <oskal/cr_status.h>

typedef struct {
  UINT32 Owner, State, MappingId, StreamCount;
  UINT64 Status;
  SmmuMappingEvidence Streams[CR_MDSS_NATIVE_STREAMS];
} CR_MDSS_NATIVE_MAPPING;

/* Exactly one producer owns this snapshot. DSI/DP own their route and its
   mode/resource/demand metadata; only SMMU owns MappingId/State/Streams/Status.
   Bridge and PHY own their corresponding fixed states. Every array element
   has this exact Owner. Unused fields/array suffixes are zero. Append copies
   into disjoint global slices and commits the frozen owner only on success.
   A retained error is immutable diagnostic evidence, not cleanup success. */
typedef struct {
  UINT32 Owner, ClockCount, RailCount, PathCount;
  UINT32 DomainCount, Reserved0, Reserved1, Reserved2;
  CR_MDSS_NATIVE_ROUTE Route;
  CR_MDSS_NATIVE_MAPPING Mappings[CR_MDSS_NATIVE_ROUTES];
  CR_MDSS_NATIVE_CLOCK Clocks[CR_MDSS_NATIVE_CLOCKS];
  CR_MDSS_NATIVE_RAIL Rails[CR_MDSS_NATIVE_RAILS];
  CR_MDSS_NATIVE_PATH Paths[CR_MDSS_NATIVE_PATHS];
  CR_MDSS_NATIVE_DOMAIN Domains[CR_MDSS_NATIVE_DOMAINS];
  CR_MDSS_NATIVE_BRIDGE_STATE Bridge;
  CR_MDSS_NATIVE_PHY_STATE Phy;
} CR_MDSS_HANDOFF_PARTICIPANT;

/* Zero-initialized caller-owned reserved normal memory. No allocations,
   firmware calls, pointer publication or hardware accesses occur here.
   The coordinator owns actual registration/quiescence and final memory-map
   exclusion. Sequence remains odd until the caller cache-publishes the final
   complete record. Reinitializing an existing record is refused. */
CR_STATUS CrMdssHandoffInit(CR_MDSS_NATIVE_HANDOFF *,UINT64 Generation,
    UINT64 TransferId,UINT64 RecordPhysical,UINT64 RecordBytes,
    UINT32 RequiredOwners,UINT32 RegisteredOwners);
CR_STATUS CrMdssHandoffAppend(CR_MDSS_NATIVE_HANDOFF *,
    CONST CR_MDSS_HANDOFF_PARTICIPANT *);
/* Finalize writes the intended nonzero even Sequence and CRC into caller
   storage only after strict validation. For shared publication the caller
   keeps its external/shared Sequence odd while preparing this local record,
   copies all other bytes, cache-publishes/fences, then stores the even value. */
CR_STATUS CrMdssHandoffFinalize(CR_MDSS_NATIVE_HANDOFF *,UINT32 Sequence);
/* Snapshot requires distinct nonoverlapping output, copies volatile bytes
   between stable even sequence observations, then validates CRC/schema.
   Failure leaves Output unchanged. Scratch is caller-owned record storage. */
CR_STATUS CrMdssHandoffSnapshot(CONST volatile VOID *Record,UINTN Bytes,
    CR_MDSS_NATIVE_HANDOFF *Scratch,CR_MDSS_NATIVE_HANDOFF *Output);
CR_STATUS CrMdssHandoffValidate(CONST CR_MDSS_NATIVE_HANDOFF *);
/* Pure native-evidence qualification only. No OS DMA, reserved-memory,
   dependency D0, serial or exclusive-owner grant follows from success. */
CR_STATUS CrMdssHandoffCanClaim(CONST CR_MDSS_NATIVE_HANDOFF *);
UINT32 CrMdssHandoffCrc32(CONST CR_MDSS_NATIVE_HANDOFF *);
