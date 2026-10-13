/* Portable retained resource journal. SPDX-License-Identifier: MIT */
#include <Library/PepJournalLib.h>
#include <oskal/cr_mutex.h>

STATIC CR_STATUS Check(CONST CR_PEP_JOURNAL *Journal) {
  CR_STATUS Status=CrMutexCheckLevel();
  if (CR_ERROR(Status)) return Status;
  return Journal!=NULL && Journal->Initialized ? CR_SUCCESS : CR_INVALID_PARAMETER;
}
STATIC BOOLEAN Range(UINTN Start,UINTN Size) { return Start<=(~(UINTN)0)-Size; }
STATIC BOOLEAN Overlap(UINTN A,UINTN As,UINTN B,UINTN Bs) {
  return A<B+Bs && B<A+As;
}
CR_STATUS CrPepJournalInit(CR_PEP_JOURNAL *Journal,VOID *Owner,
    CONST CR_PEP_JOURNAL_PROVIDER *Provider,CR_PEP_JOURNAL_ENTRY *Transactions,
    UINT32 TransactionCapacity,CR_PEP_JOURNAL_ENTRY *Owned,UINT32 OwnedCapacity) {
  CR_STATUS Status=CrMutexCheckLevel();
  if (CR_ERROR(Status)) return Status;
  if (Journal==NULL || Owner==NULL || Provider==NULL || Transactions==NULL || Owned==NULL ||
      TransactionCapacity==0 || OwnedCapacity==0 || Provider->Resolve==NULL ||
      Provider->Capture==NULL || Provider->Apply==NULL || Provider->Restore==NULL)
    return CR_INVALID_PARAMETER;
  if (Journal->Initialized) return CR_ALREADY_STARTED;
  if ((UINTN)TransactionCapacity>(~(UINTN)0)/sizeof(*Transactions) ||
      (UINTN)OwnedCapacity>(~(UINTN)0)/sizeof(*Owned)) return CR_BAD_BUFFER_SIZE;
  UINTN T=(UINTN)Transactions,O=(UINTN)Owned,J=(UINTN)Journal;
  UINTN Ts=(UINTN)TransactionCapacity*sizeof(*Transactions),Os=(UINTN)OwnedCapacity*sizeof(*Owned);
  if (T%_Alignof(CR_PEP_JOURNAL_ENTRY) || O%_Alignof(CR_PEP_JOURNAL_ENTRY) ||
      !Range(T,Ts) || !Range(O,Os) || !Range(J,sizeof(*Journal)) ||
      Overlap(T,Ts,O,Os) || Overlap(T,Ts,J,sizeof(*Journal)) || Overlap(O,Os,J,sizeof(*Journal)))
    return CR_INVALID_PARAMETER;
  CR_PEP_JOURNAL New={0};
  New.Provider=*Provider;New.Owner=Owner;New.Transactions=Transactions;New.Owned=Owned;
  New.TransactionCapacity=TransactionCapacity;New.OwnedCapacity=OwnedCapacity;New.Initialized=TRUE;
  *Journal=New;return CR_SUCCESS;
}
CR_STATUS CrPepJournalBegin(CR_PEP_JOURNAL *Journal) {
  CR_STATUS Status=Check(Journal);if (CR_ERROR(Status)) return Status;
  if (Journal->Active || Journal->Closing) return CR_BUSY;
  Journal->OriginalOwnedCount=Journal->OwnedCount;Journal->Active=TRUE;
  return CR_SUCCESS;
}
CR_STATUS CrPepJournalValidate(CR_PEP_JOURNAL *Journal,CONST CR_PEP_RESOURCE_OP *Operation) {
  CR_STATUS Status=Check(Journal);if (CR_ERROR(Status)) return Status;
  if (Operation==NULL) return CR_INVALID_PARAMETER;
  UINT64 Resource;
  Status=Journal->Provider.Resolve(Journal->Provider.Context,Journal->Owner,Operation,&Resource);
  if (CR_ERROR(Status)) return Status;
  return (Operation->Kind==CrPepDelay)==(Resource==CR_PEP_JOURNAL_TRANSIENT_RESOURCE) ? CR_SUCCESS : CR_COMPROMISED_DATA;
}
CR_STATUS CrPepJournalApply(CR_PEP_JOURNAL *Journal,CONST CR_PEP_RESOURCE_OP *Operation) {
  CR_STATUS Status=Check(Journal);if (CR_ERROR(Status)) return Status;
  if (!Journal->Active || Journal->Failed || Journal->Closing) return CR_NOT_READY;
  if (Operation==NULL) {Journal->Failed=TRUE;return CR_INVALID_PARAMETER;}
  UINT64 Resource;
  Status=Journal->Provider.Resolve(Journal->Provider.Context,Journal->Owner,Operation,&Resource);
  if (CR_ERROR(Status)) {Journal->Failed=TRUE;return Status;}
  if (Operation->Kind==CrPepDelay) {
    if (Resource!=CR_PEP_JOURNAL_TRANSIENT_RESOURCE) {Journal->Failed=TRUE;return CR_COMPROMISED_DATA;}
    Status=Journal->Provider.Apply(Journal->Provider.Context,Journal->Owner,Resource,Operation);
    if (CR_ERROR(Status)) Journal->Failed=TRUE;
    return Status;
  }
  if (Resource==CR_PEP_JOURNAL_TRANSIENT_RESOURCE) {Journal->Failed=TRUE;return CR_COMPROMISED_DATA;}
  if (Journal->TransactionCount==Journal->TransactionCapacity) {
    Journal->Failed=TRUE;return CR_OUT_OF_RESOURCES;
  }
  UINT32 Index=0;
  while (Index<Journal->OwnedCount && Journal->Owned[Index].Resource!=Resource) ++Index;
  if (Index==Journal->OwnedCount && Index==Journal->OwnedCapacity) {
    Journal->Failed=TRUE;return CR_OUT_OF_RESOURCES;
  }
  CR_PEP_JOURNAL_ENTRY Entry={0};Entry.Resource=Resource;
  Status=Journal->Provider.Capture(Journal->Provider.Context,Journal->Owner,Resource,&Entry.Before);
  if (CR_ERROR(Status)) {Journal->Failed=TRUE;return Status;}
  if (Index==Journal->OwnedCount) Journal->Owned[Journal->OwnedCount++]=Entry;
  // Record EVERY attempted write, including repeated resources, before calling
  // the provider: a failed operation can have committed a partial hardware vote.
  Journal->Transactions[Journal->TransactionCount++]=Entry;
  Status=Journal->Provider.Apply(Journal->Provider.Context,Journal->Owner,Resource,Operation);
  if (CR_ERROR(Status)) Journal->Failed=TRUE;
  return Status;
}
CR_STATUS CrPepJournalCommit(CR_PEP_JOURNAL *Journal) {
  CR_STATUS Status=Check(Journal);if (CR_ERROR(Status)) return Status;
  if (!Journal->Active || Journal->Failed || Journal->Closing) return CR_NOT_READY;
  Journal->TransactionCount=0;Journal->OriginalOwnedCount=0;Journal->Active=FALSE;
  return CR_SUCCESS;
}
CR_STATUS CrPepJournalRollback(CR_PEP_JOURNAL *Journal) {
  CR_STATUS Status=Check(Journal);if (CR_ERROR(Status)) return Status;
  if (!Journal->Active) return CR_SUCCESS;
  // Until the entire reverse sequence succeeds, neither Commit nor Begin may
  // use this owner. Successful entries are popped; a failed entry is retained.
  Journal->Failed=TRUE;
  while (Journal->TransactionCount!=0) {
    CONST CR_PEP_JOURNAL_ENTRY *Entry=&Journal->Transactions[Journal->TransactionCount-1];
    Status=Journal->Provider.Restore(Journal->Provider.Context,Journal->Owner,Entry->Resource,&Entry->Before);
    if (CR_ERROR(Status)) return Status;
    --Journal->TransactionCount;
  }
  Journal->OwnedCount=Journal->OriginalOwnedCount;Journal->OriginalOwnedCount=0;
  Journal->Active=FALSE;Journal->Failed=FALSE;return CR_SUCCESS;
}
CR_STATUS CrPepJournalClose(CR_PEP_JOURNAL *Journal) {
  CR_STATUS Status=Check(Journal);if (CR_ERROR(Status)) return Status;
  Journal->Closing=TRUE;
  Status=CrPepJournalRollback(Journal);if (CR_ERROR(Status)) return Status;
  while (Journal->OwnedCount!=0) {
    CONST CR_PEP_JOURNAL_ENTRY *Entry=&Journal->Owned[Journal->OwnedCount-1];
    Status=Journal->Provider.Restore(Journal->Provider.Context,Journal->Owner,Entry->Resource,&Entry->Before);
    if (CR_ERROR(Status)) return Status;
    --Journal->OwnedCount;
  }
  Journal->Closing=FALSE;return CR_SUCCESS;
}
CR_STATUS CrPepJournalUninit(CR_PEP_JOURNAL *Journal) {
  CR_STATUS Status=Check(Journal);if (CR_ERROR(Status)) return Status;
  if (Journal->Active || Journal->OwnedCount!=0 || Journal->Closing) return CR_BUSY;
  CR_PEP_JOURNAL Empty={0};*Journal=Empty;return CR_SUCCESS;
}
STATIC CR_STATUS CheckOwner(CR_PEP_JOURNAL *Journal,VOID *Owner) {
  CR_STATUS Status=Check(Journal);if (CR_ERROR(Status)) return Status;
  return Owner==Journal->Owner ? CR_SUCCESS : CR_INVALID_PARAMETER;
}
STATIC CR_STATUS Validate(VOID *Context,VOID *Owner,CONST CR_PEP_RESOURCE_OP *Operation) {
  CR_STATUS Status=CheckOwner(Context,Owner);if (CR_ERROR(Status)) return Status;
  return CrPepJournalValidate(Context,Operation);
}
STATIC CR_STATUS Begin(VOID *Context,VOID *Owner) {
  CR_STATUS Status=CheckOwner(Context,Owner);if (CR_ERROR(Status)) return Status;
  return CrPepJournalBegin(Context);
}
STATIC CR_STATUS Apply(VOID *Context,VOID *Owner,CONST CR_PEP_RESOURCE_OP *Operation) {
  CR_STATUS Status=CheckOwner(Context,Owner);if (CR_ERROR(Status)) return Status;
  return CrPepJournalApply(Context,Operation);
}
STATIC CR_STATUS Commit(VOID *Context,VOID *Owner) {
  CR_STATUS Status=CheckOwner(Context,Owner);if (CR_ERROR(Status)) return Status;
  return CrPepJournalCommit(Context);
}
STATIC CR_STATUS Rollback(VOID *Context,VOID *Owner) {
  CR_STATUS Status=CheckOwner(Context,Owner);if (CR_ERROR(Status)) return Status;
  return CrPepJournalRollback(Context);
}
CR_STATUS CrPepJournalGetResourceIo(CR_PEP_JOURNAL *Journal,CR_PEP_RESOURCE_IO *Io) {
  CR_STATUS Status=Check(Journal);if (CR_ERROR(Status)) return Status;
  if (Io==NULL) return CR_INVALID_PARAMETER;
  CR_PEP_RESOURCE_IO New={Journal,Validate,Begin,Apply,Commit,Rollback};*Io=New;return CR_SUCCESS;
}
