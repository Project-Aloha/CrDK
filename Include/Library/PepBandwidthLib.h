/* Qualified BUSARB owner-journal provider. SPDX-License-Identifier: MIT */
#pragma once
#include <Library/PepJournalLib.h>
#include <Library/InterconnectVoteLib.h>

typedef struct {
  CR_PEP_TEXT Master, Slave;
  CR_ICB_PATH Path;
  /* Explicit AML contract: 1000 for bytes/sec, 1 for Linux kB/s.
     Round positive fractional kB/s UP, so a minimum demand is not lost. */
  UINT32 PolicyUnitsPerKbyte;
} CR_PEP_BANDWIDTH_BINDING;
typedef struct {
  VOID *Context;
  CR_STATUS (*Validate)(VOID *, VOID *Owner, CONST CR_ICB_PATH *);
  CR_STATUS (*Get)(VOID *, VOID *Owner, CONST CR_ICB_PATH *, CR_ICB_VOTE_VALUE *, BOOLEAN *Pending);
  CR_STATUS (*Set)(VOID *, VOID *Owner, CONST CR_ICB_PATH *, CONST CR_ICB_VOTE_VALUE *);
} CR_PEP_BANDWIDTH_IO;
typedef struct {
  CR_PEP_BANDWIDTH_IO Io;
  CONST CR_PEP_BANDWIDTH_BINDING *Bindings;
  UINT32 Count;
  BOOLEAN Initialized;
} CR_PEP_BANDWIDTH_PROVIDER;

/* PASSIVE, externally serialized immutable bindings and independent nonpaged
   storage. Init calls no IO. Qualify exact AML master/slave names against board
   provider-local endpoints; no global endpoint aliases or implicit units.
   Reserve all owner paths OUTSIDE pure Resolve, before irreversible publication.
   Resolve checks full BUSARB{3,master,slave,AB,IB}, no handle acquisition, votes
   or recovery. Capture/Restore use only this owner's logical Linux-unit vote;
   global uncertain BCM recovery never makes a foreign path part of this owner.
   Provider/code/bindings/IO outlive all journals, including failed Close retry. */
CR_STATUS CrPepBandwidthProviderInit(CR_PEP_BANDWIDTH_PROVIDER *, CONST CR_PEP_BANDWIDTH_IO *,
                                     CONST CR_PEP_BANDWIDTH_BINDING *, UINT32 Count);
CR_STATUS CrPepBandwidthProviderGetJournal(CR_PEP_BANDWIDTH_PROVIDER *, CR_PEP_JOURNAL_PROVIDER *);
/* Pure full opcode/unit decoding for pre-publication path reservations. Calls
   no IO; the borrowed binding/value do not establish hardware availability. */
CR_STATUS CrPepBandwidthProviderDescribe(CR_PEP_BANDWIDTH_PROVIDER *, CONST CR_PEP_RESOURCE_OP *,
                                         CONST CR_PEP_BANDWIDTH_BINDING **, CR_ICB_VOTE_VALUE *);
/* Only after all journals close, callbacks drain and publication is absent. */
CR_STATUS CrPepBandwidthProviderUninit(CR_PEP_BANDWIDTH_PROVIDER *);
