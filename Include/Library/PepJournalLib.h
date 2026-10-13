/* Durable per-owner resource undo records for PepCrLib.
 * SPDX-License-Identifier: MIT */
#pragma once
#include <Library/PepCrLib.h>

/* Provider-defined logical vote, never a raw shared-register snapshot or a
   borrowed pointer. Capture/Restore affect this owner only. The provider must
   recompute shared aggregates without losing votes from other owners. */
typedef struct { UINT64 Words[8]; } CR_PEP_JOURNAL_VALUE;
/* Only a qualified CrPepDelay operation may Resolve to this identity. Its
   fallible Apply executes in sequence but never captures/owns/restores a vote.
   A failed delay still fails the transaction and rolls prior votes back. */
#define CR_PEP_JOURNAL_TRANSIENT_RESOURCE (~(UINT64)0)
typedef struct {
  UINT64 Resource;
  CR_PEP_JOURNAL_VALUE Before;
} CR_PEP_JOURNAL_ENTRY;
typedef struct {
  VOID *Context;
  /* Pure complete-operation preflight and stable resource identity. Resolve
     must not allocate a hardware handle, add a vote or change a register. */
  CR_STATUS (*Resolve)(VOID *, VOID *Owner, CONST CR_PEP_RESOURCE_OP *, UINT64 *Resource);
  /* Pure copy of the owner's current logical vote; no resource mutation. */
  CR_STATUS (*Capture)(VOID *, VOID *Owner, UINT64 Resource, CR_PEP_JOURNAL_VALUE *);
  /* Absolute idempotent per-owner operation. Failure may have changed hardware
     or the logical vote; the caller will restore the saved Before value. */
  CR_STATUS (*Apply)(VOID *, VOID *Owner, UINT64 Resource, CONST CR_PEP_RESOURCE_OP *);
  /* Absolute idempotent restore, including removing a formerly absent vote.
     Retain bindings/handles and uncertain writes on failure until retry. */
  CR_STATUS (*Restore)(VOID *, VOID *Owner, UINT64 Resource, CONST CR_PEP_JOURNAL_VALUE *);
} CR_PEP_JOURNAL_PROVIDER;
typedef struct {
  CR_PEP_JOURNAL_PROVIDER Provider;
  VOID *Owner;
  CR_PEP_JOURNAL_ENTRY *Transactions, *Owned;
  UINT32 TransactionCapacity, OwnedCapacity;
  UINT32 TransactionCount, OwnedCount, OriginalOwnedCount;
  BOOLEAN Initialized, Active, Failed, Closing;
} CR_PEP_JOURNAL;

/* All mutating functions require passive context and caller serialization.
   Zero-initialize Journal. Storage is caller-owned, nonoverlapping and must
   outlive every retained transaction/owner. No allocation or provider calls
   occur during Init/Begin/Commit. Providers and their resource identities stay
   valid through recovery and Close, independently of removable OS contexts. */
CR_STATUS CrPepJournalInit(CR_PEP_JOURNAL *, VOID *Owner, CONST CR_PEP_JOURNAL_PROVIDER *,
                           CR_PEP_JOURNAL_ENTRY *Transactions, UINT32 TransactionCapacity,
                           CR_PEP_JOURNAL_ENTRY *Owned, UINT32 OwnedCapacity);
CR_STATUS CrPepJournalBegin(CR_PEP_JOURNAL *);
CR_STATUS CrPepJournalValidate(CR_PEP_JOURNAL *, CONST CR_PEP_RESOURCE_OP *);
CR_STATUS CrPepJournalApply(CR_PEP_JOURNAL *, CONST CR_PEP_RESOURCE_OP *);
CR_STATUS CrPepJournalCommit(CR_PEP_JOURNAL *);
CR_STATUS CrPepJournalRollback(CR_PEP_JOURNAL *);
/* Roll back a pending transaction, then remove committed ownership in reverse
   acquisition order. Failed restore retains its entry and gates new Begin.
   Successful Close is idempotent and allows this owner to prepare again. */
CR_STATUS CrPepJournalClose(CR_PEP_JOURNAL *);
CR_STATUS CrPepJournalUninit(CR_PEP_JOURNAL *);
/* This adapter accepts only the owner supplied at Init. A broker with several
   owners dispatches to their permanent journals under its serialization. */
CR_STATUS CrPepJournalGetResourceIo(CR_PEP_JOURNAL *, CR_PEP_RESOURCE_IO *);
