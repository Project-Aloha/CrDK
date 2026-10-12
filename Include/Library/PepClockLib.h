/* Qualified CLOCK/FOOTSWITCH journal provider for the Kodiak policy schema.
 * SPDX-License-Identifier: MIT */
#pragma once
#include <Library/PepJournalLib.h>
#include <Library/ClockVoteLib.h>

/* CLOCK opcode3 contains both an AML integer and an opaque selector. Match an
   explicit board-qualified tuple to Hz. No guessed units, rounding or live
   frequency aggregation is implicit in this contract. */
typedef struct { UINT64 PolicyValue, Selector, RateHz; } CR_PEP_CLOCK_RATE;
typedef struct {
  CR_PEP_TEXT Name;
  CONST CHAR8 *HardwareName;
  UINT32 Controller;
  BOOLEAN Gdsc;
  UINT64 DefaultRateHz;
  CONST CR_PEP_CLOCK_RATE *Rates;
  UINT32 RateCount;
  /* Qualify Kodiak's {name, on/off} whole-domain form explicitly. */
  BOOLEAN AllowImplicitGdscMode;
} CR_PEP_CLOCK_BINDING;
typedef struct {
  VOID *Context;
  CR_STATUS (*Validate)(VOID *, CONST CR_PEP_CLOCK_BINDING *, UINT64 RateHz);
  CR_STATUS (*Get)(VOID *, VOID *Owner, CONST CR_PEP_CLOCK_BINDING *,
                   CR_CLOCK_VOTE_VALUE *, BOOLEAN *RecoveryPending);
  CR_STATUS (*Set)(VOID *, VOID *Owner, CONST CR_PEP_CLOCK_BINDING *,
                   CONST CR_CLOCK_VOTE_VALUE *);
} CR_PEP_CLOCK_IO;
typedef struct {
  CR_PEP_CLOCK_IO Io;
  CONST CR_PEP_CLOCK_BINDING *Bindings;
  UINT32 Count;
  BOOLEAN Initialized;
} CR_PEP_CLOCK_PROVIDER;

/* PASSIVE and externally serialized. Immutable bindings, IO context and code
   remain alive through journal Close/recovery. Init does not call IO. Duplicate
   policy names or hardware aliases are rejected so a journal key identifies
   exactly one resource. Empty providers are rejected. Resolve validates complete
   opcode/tuple/rate and current provider health without allocating or voting.
   CLOCK1/2 enable/disable. FOOTSWITCH1/2 accept mode1, or the omitted mode only
   for bindings with AllowImplicitGdscMode. Other retain modes are declined.
   Capture stores only a logical owner
   vote; Restore replays that owner alone, including removing an absent vote. */
CR_STATUS CrPepClockProviderInit(CR_PEP_CLOCK_PROVIDER *, CONST CR_PEP_CLOCK_IO *,
                                 CONST CR_PEP_CLOCK_BINDING *, UINT32 Count);
CR_STATUS CrPepClockProviderGetJournal(CR_PEP_CLOCK_PROVIDER *, CR_PEP_JOURNAL_PROVIDER *);
/* Pure binding/opcode/tuple decoding for pre-publication reservations. No IO
   callback or owner lookup is made. The returned immutable binding is borrowed
   from this provider; Describe alone does not prove hardware availability. */
CR_STATUS CrPepClockProviderDescribe(CR_PEP_CLOCK_PROVIDER *, CONST CR_PEP_RESOURCE_OP *,
                                      CONST CR_PEP_CLOCK_BINDING **, UINT64 *RateHz);
/* External lifetime serialization; valid only after every owner journal/vote
   has closed and no provider callback is running. */
CR_STATUS CrPepClockProviderUninit(CR_PEP_CLOCK_PROVIDER *);
