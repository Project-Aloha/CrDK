/* Allocation-free reserved owner votes. SPDX-License-Identifier: MIT */
#include <Library/InterconnectVoteLib.h>
#include <oskal/cr_string.h>

STATIC CR_STATUS Enter(CR_ICB_VOTER *Voter) {
  if (CR_ERROR(CrMutexCheckLevel()) || Voter == NULL || !Voter->Initialized)
    return CR_INVALID_PARAMETER;
  return CrMutexAcquire(&Voter->Lock);
}
STATIC BOOLEAN PathValid(CONST CR_ICB_PATH *Path) {
  return Path != NULL && Path->Provider != NULL && Path->Provider[0] != 0;
}
STATIC BOOLEAN SamePath(CONST CR_ICB_PATH *Left, CONST CR_ICB_PATH *Right) {
  return Left->SourceId == Right->SourceId && Left->DestinationId == Right->DestinationId &&
         cr_strcmp(Left->Provider, Right->Provider) == 0;
}
STATIC CR_ICB_VOTE_ENTRY *Find(CR_ICB_VOTER *Voter, VOID *Owner, CONST CR_ICB_PATH *Path) {
  for (UINT32 I = 0; I < Voter->Capacity; ++I)
    if (Voter->Entries[I].Owner == Owner && SamePath(&Voter->Entries[I].Path, Path))
      return &Voter->Entries[I];
  return NULL;
}
STATIC BOOLEAN Overlaps(UINTN Start, UINTN Length, CONST VOID *Other, UINTN Size) {
  UINTN Address = (UINTN)Other;
  return Address > ~((UINTN)0) - Size || (Start < Address + Size && Address < Start + Length);
}
CR_STATUS CrIcbVotesInit(CR_ICB_VOTER *Voter, InterconnectDeviceContext *Core,
                         CR_ICB_VOTE_ENTRY *Entries, UINT32 Capacity) {
  if (CR_ERROR(CrMutexCheckLevel()) || Voter == NULL || Core == NULL || Entries == NULL ||
      Capacity == 0 || Capacity > INTERCONNECT_MAX_PATHS ||
      (UINTN)Entries % sizeof(VOID *) != 0) return CR_INVALID_PARAMETER;
  if (Voter->Initialized) return CR_ALREADY_STARTED;
  UINTN Bytes = sizeof(*Entries) * Capacity;
  if ((UINTN)Entries > ~((UINTN)0) - Bytes ||
      Overlaps((UINTN)Entries, Bytes, Voter, sizeof(*Voter)) ||
      Overlaps((UINTN)Entries, Bytes, Core, sizeof(*Core)) ||
      Overlaps((UINTN)Voter, sizeof(*Voter), Core, sizeof(*Core))) return CR_INVALID_PARAMETER;
  if (!Core->Initialized || Core->DeinitPending || Core->RollbackPending) return CR_NOT_READY;
  cr_memset(Entries, 0, Bytes);
  Voter->Core = Core;Voter->Entries = Entries;Voter->Capacity = Capacity;
  CrMutexInit(&Voter->Lock);Voter->Initialized = TRUE;
  return CR_SUCCESS;
}
CR_STATUS CrIcbVotesReserve(CR_ICB_VOTER *Voter, VOID *Owner, CONST CR_ICB_PATH *Path) {
  if (Owner == NULL || !PathValid(Path)) return CR_INVALID_PARAMETER;
  CR_STATUS Status = Enter(Voter);if (CR_ERROR(Status)) return Status;
  Status = InterconnectValidatePath(Voter->Core, Path->Provider, Path->SourceId, Path->DestinationId);
  if (!CR_ERROR(Status) && Find(Voter, Owner, Path) == NULL) {
    CR_ICB_VOTE_ENTRY *Slot = NULL;
    for (UINT32 I = 0; I < Voter->Capacity; ++I)
      if (Voter->Entries[I].Owner == NULL) {Slot = &Voter->Entries[I];break;}
    if (Slot == NULL) Status = CR_OUT_OF_RESOURCES;
    else {
      INTERCONNECT_PATH_HANDLE Handle;
      Status = InterconnectAcquirePath(Voter->Core, Path->Provider, Path->SourceId, Path->DestinationId, &Handle);
      if (!CR_ERROR(Status)) {
        Slot->Owner = Owner;Slot->Path = *Path;Slot->Handle = Handle;
      }
    }
  }
  CrMutexRelease(&Voter->Lock);return Status;
}
CR_STATUS CrIcbVotesValidate(CR_ICB_VOTER *Voter, VOID *Owner, CONST CR_ICB_PATH *Path) {
  if (Owner == NULL || !PathValid(Path)) return CR_INVALID_PARAMETER;
  CR_STATUS Status = Enter(Voter);if (CR_ERROR(Status)) return Status;
  if (Find(Voter, Owner, Path) == NULL) Status = CR_NOT_FOUND;
  else Status = InterconnectValidatePath(Voter->Core, Path->Provider, Path->SourceId, Path->DestinationId);
  CrMutexRelease(&Voter->Lock);return Status;
}
CR_STATUS CrIcbVotesGet(CR_ICB_VOTER *Voter, VOID *Owner, CONST CR_ICB_PATH *Path,
                        CR_ICB_VOTE_VALUE *Value, BOOLEAN *Pending) {
  if (Owner == NULL || !PathValid(Path) || Value == NULL || Pending == NULL) return CR_INVALID_PARAMETER;
  CR_STATUS Status = Enter(Voter);if (CR_ERROR(Status)) return Status;
  CR_ICB_VOTE_ENTRY *Entry = Find(Voter, Owner, Path);
  if (Entry == NULL) Status = CR_NOT_FOUND;
  else {
    UINT64 Average, Peak;
    Status = InterconnectGetPathVote(Voter->Core, Entry->Handle, &Average, &Peak, Pending);
    if (!CR_ERROR(Status)) {
      if (Average != Entry->Value.Average || Peak != Entry->Value.Peak) Status = CR_COMPROMISED_DATA;
      else *Value = Entry->Value;
    }
  }
  CrMutexRelease(&Voter->Lock);return Status;
}
CR_STATUS CrIcbVotesSet(CR_ICB_VOTER *Voter, VOID *Owner, CONST CR_ICB_PATH *Path,
                        CONST CR_ICB_VOTE_VALUE *Input) {
  if (Owner == NULL || !PathValid(Path) || Input == NULL || Input->Present > 1 ||
      Input->Peak < Input->Average || (!Input->Present && (Input->Average || Input->Peak)))
    return CR_INVALID_PARAMETER;
  CR_ICB_VOTE_VALUE Value = *Input;
  CR_STATUS Status = Enter(Voter);if (CR_ERROR(Status)) return Status;
  CR_ICB_VOTE_ENTRY *Entry = Find(Voter, Owner, Path);
  if (Entry == NULL) Status = CR_NOT_FOUND;
  else {
    Status = InterconnectSetBandwidth(Voter->Core, Entry->Handle, Value.Average, Value.Peak);
    if (!CR_ERROR(Status)) Entry->Value = Value;
  }
  CrMutexRelease(&Voter->Lock);return Status;
}
STATIC CR_STATUS Release(CR_ICB_VOTER *Voter, VOID *Owner) {
  CR_STATUS Result = CR_SUCCESS;
  for (UINT32 I = Voter->Capacity; I != 0; --I) {
    CR_ICB_VOTE_ENTRY *Entry = &Voter->Entries[I - 1];
    if (Entry->Owner == NULL || (Owner != NULL && Entry->Owner != Owner)) continue;
    CR_STATUS Status = InterconnectReleasePath(Voter->Core, Entry->Handle);
    if (CR_ERROR(Status)) {if (!CR_ERROR(Result)) Result = Status;}
    else cr_memset(Entry, 0, sizeof(*Entry));
  }
  return Result;
}
CR_STATUS CrIcbVotesReleaseOwner(CR_ICB_VOTER *Voter, VOID *Owner) {
  if (Owner == NULL) return CR_INVALID_PARAMETER;
  CR_STATUS Status = Enter(Voter);if (CR_ERROR(Status)) return Status;
  Status = Release(Voter, Owner);CrMutexRelease(&Voter->Lock);return Status;
}
CR_STATUS CrIcbVotesDrain(CR_ICB_VOTER *Voter) {
  CR_STATUS Status = Enter(Voter);if (CR_ERROR(Status)) return Status;
  Status = Release(Voter, NULL);CrMutexRelease(&Voter->Lock);return Status;
}
CR_STATUS CrIcbVotesUninit(CR_ICB_VOTER *Voter) {
  CR_STATUS Status = Enter(Voter);if (CR_ERROR(Status)) return Status;
  for (UINT32 I = 0; I < Voter->Capacity; ++I)
    if (Voter->Entries[I].Owner != NULL) {Status = CR_BUSY;break;}
  CrMutexRelease(&Voter->Lock);
  if (!CR_ERROR(Status)) cr_memset(Voter, 0, sizeof(*Voter));
  return Status;
}
CR_STATUS CrIcbVotesRetireAfterDeinit(CR_ICB_VOTER *Voter) {
  CR_STATUS Status = Enter(Voter);if (CR_ERROR(Status)) return Status;
  if (Voter->Core->Initialized || Voter->Core->DeinitPending || Voter->Core->RollbackPending) Status = CR_BUSY;
  else cr_memset(Voter->Entries, 0, sizeof(*Voter->Entries) * Voter->Capacity);
  CrMutexRelease(&Voter->Lock);
  if (!CR_ERROR(Status)) cr_memset(Voter, 0, sizeof(*Voter));
  return Status;
}
