/* Per-owner absolute votes. SPDX-License-Identifier: MIT */
#include <Library/ClockVoteLib.h>
#include "clock_internal.h"
#include <oskal/cr_string.h>
#include <oskal/cr_mutex.h>

STATIC CR_STATUS Check(CR_CLOCK_VOTER *Voter) {
  if (CR_ERROR(CrMutexCheckLevel())) return CR_INVALID_PARAMETER;
  if (Voter == NULL || !Voter->Initialized || Voter->Core == NULL ||
      !Voter->Core->LockInitialized || Voter->Entries == NULL || Voter->Capacity == 0)
    return CR_INVALID_PARAMETER;
  return CR_SUCCESS;
}

STATIC BOOLEAN Fits(CONST ClockController *Controller, UINT64 Offset) {
  return Controller != NULL && Controller->MemMapped && Controller->Address != 0 &&
    (Controller->Address & 3U) == 0 && Controller->Size >= 4 &&
    Offset <= Controller->Size - 4U &&
    Controller->Address <= (UINTN)-1 - Controller->Size;
}

STATIC BOOLEAN KnownController(CR_CLOCK_VOTER *Voter, ClockController *Controller) {
  for (UINTN Index = 0; Index < Voter->Core->ClockControllerCount; ++Index)
    if (Controller == &Voter->Core->ClockControllers[Index]) return TRUE;
  return FALSE;
}

/* Only immutable descriptions are inspected. Bound depth and fanout before
   walking parent pointers; a cycle reaches the depth bound without MMIO. */
STATIC CR_STATUS ValidateClock(CR_CLOCK_VOTER *Voter, ClockNode *Node,
                               UINT64 RateHz, UINT32 Depth, UINT32 *Budget) {
  if (Node == NULL || Depth == 32 || *Budget == 0 ||
      !KnownController(Voter, Node->ParentController))
    return CR_INVALID_PARAMETER;
  --*Budget;
  if (CR_ERROR(ClockParentDescription(Node))) return CR_INVALID_PARAMETER;
  if (Node->FixedRateHz && Node->FixedRateHz!=RateHz) return CR_UNSUPPORTED;
  if (Node->KeepEnabled>1 || (Node->KeepEnabled &&
      Node->Type!=CLOCK_NODE_TYPE_BRANCH && Node->Type!=CLOCK_NODE_TYPE_BRANCH_2))
    return CR_INVALID_PARAMETER;
  if (Node->Type == CLOCK_NODE_TYPE_BRANCH || Node->Type == CLOCK_NODE_TYPE_BRANCH_2) {
    if (Node->EnableRegister == 0 || Node->EnableMsk == 0 ||
        !Fits(Node->ParentController, Node->EnableRegister) ||
        !Fits(Node->ParentController, Node->HaltRegister) ||
        (Node->HwcgRegister != 0 && !Fits(Node->ParentController, Node->HwcgRegister)))
      return CR_INVALID_PARAMETER;
  } else if (Node->Type == CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2) {
    if (!Fits(Node->ParentController, Node->CmdRegister) ||
        !Fits(Node->ParentController, (UINT64)Node->CmdRegister + Node->CmdRCfgOffset + 0x10U) ||
        Node->HIDWidth == 0 || Node->HIDWidth > 8 || Node->MNDWidth > 16 ||
        Node->FreqTable == NULL || Node->FreqCount == 0) return CR_INVALID_PARAMETER;
    BOOLEAN Found = FALSE;
    for (UINTN Index = 0; Index < Node->FreqCount; ++Index)
      if (Node->FreqTable[Index].FrequencyHz == RateHz) Found = TRUE;
    if (!Found) return CR_UNSUPPORTED;
  } else if (Node->Type == CLOCK_NODE_TYPE_MUX || Node->Type == CLOCK_NODE_TYPE_PHY_MUX) {
    if (Node->MuxMask == 0 || !Fits(Node->ParentController, Node->MuxRegister))
      return CR_INVALID_PARAMETER;
  } else if (Node->Type == CLOCK_NODE_TYPE_READ_ONLY_DIVIDER) {
    if (!Node->DividerWidth || Node->DividerWidth>16 ||
        Node->DividerReadOnly!=TRUE || !Node->DividerInputRateHz ||
        (Node->DividerRegister&3U) || !Fits(Node->ParentController,Node->DividerRegister) ||
        (Node->RequiredDivider && Node->RequiredDivider>(UINT32)BIT(Node->DividerWidth)))
      return CR_INVALID_PARAMETER;
    /* Reserve checks possible immutable divisor rates, not a hardware value.
       The first/shared acquisition reads the actual register under core lock. */
    BOOLEAN Found=FALSE;
    UINT32 Max=(UINT32)BIT(Node->DividerWidth);
    for (UINT32 Divider=1;Divider<=Max;++Divider) {
      if (Node->RequiredDivider && Divider!=Node->RequiredDivider) continue;
      UINT64 Rate=Node->DividerInputRateHz/Divider+(Node->DividerInputRateHz%Divider!=0);
      if (Rate==RateHz) {Found=TRUE;break;}
    }
    if (!Found) return CR_UNSUPPORTED;
  } else if (Node->Type == CLOCK_NODE_TYPE_FIXED_PLL) {
    CR_FIXED_PLL_DESCRIPTION D = Node->FixedPll.Description;
    if (RateHz != D.FrequencyHz || !Node->ParentController->MemMapped)
      return CR_UNSUPPORTED;
    D.Base = Node->ParentController->Address;
    D.Size = Node->ParentController->Size;
    CR_FIXED_PLL_VOTE Pure = {0};
    CR_STATUS Status = CrFixedPllInit(&Pure, &D);
    if (CR_ERROR(Status)) return Status;
  } else {
    return CR_UNSUPPORTED;
  }
  for (UINTN Index = 0; Index < Node->ParentCount; ++Index) {
    if (!ClockNeedsParent(Node,Index,RateHz)) continue;
    UINT64 ParentRate=ClockParentRate(Node,Index,RateHz);
    CR_STATUS Status = ValidateClock(Voter, Node->Parents[Index], ParentRate, Depth + 1, Budget);
    if (CR_ERROR(Status)) return Status;
  }
  return CR_SUCCESS;
}

CR_STATUS CrClockVotesValidate(CR_CLOCK_VOTER *Voter, ClockNode *Node, UINT64 RateHz) {
  CR_STATUS Status = Check(Voter);
  if (CR_ERROR(Status)) return Status;
  if (Node == NULL || !KnownController(Voter, Node->ParentController))
    return CR_INVALID_PARAMETER;
  if (Node->Type != CLOCK_NODE_TYPE_GDSC) {
    UINT32 Budget = 512;
    return ValidateClock(Voter, Node, RateHz, 0, &Budget);
  }
  if (RateHz != 0 || !Fits(Node->ParentController, Node->GdscRegister))
    return CR_INVALID_PARAMETER;
  return ClockGdscValidate(Voter->Core, Node);
}

CR_STATUS CrClockVotesInit(CR_CLOCK_VOTER *Voter, ClockDriverContext *Core,
                          CR_CLOCK_VOTE_ENTRY *Entries, UINT32 Capacity) {
  if (CR_ERROR(CrMutexCheckLevel()) || Voter == NULL || Core == NULL ||
      !Core->LockInitialized || Core->ClockControllers == NULL ||
      Core->ClockControllerCount == 0 || Core->ClockControllerCount > CC_TYPE_MAX ||
      Entries == NULL || Capacity == 0 || Capacity > 65536U ||
      ((UINTN)Entries & (sizeof(UINTN) - 1U)) != 0 ||
      (UINTN)Entries > (UINTN)-1 - (UINTN)Capacity * sizeof(*Entries))
    return CR_INVALID_PARAMETER;
  UINTN Start = (UINTN)Entries, End = Start + (UINTN)Capacity * sizeof(*Entries);
  if ((UINTN)Voter > (UINTN)-1 - sizeof(*Voter) ||
      (Start < (UINTN)Voter + sizeof(*Voter) && (UINTN)Voter < End))
    return CR_INVALID_PARAMETER;
  UINTN C = (UINTN)Core, Cs = sizeof(*Core);
  UINTN Controllers = (UINTN)Core->ClockControllers;
  UINTN ControllerSize = Core->ClockControllerCount * sizeof(*Core->ClockControllers);
  if (C > (UINTN)-1 - Cs || Controllers > (UINTN)-1 - ControllerSize ||
      (Start < C + Cs && C < End) ||
      (Start < Controllers + ControllerSize && Controllers < End))
    return CR_INVALID_PARAMETER;
  if (Voter->Initialized || Voter->Core != NULL || Voter->Entries != NULL || Voter->Capacity != 0)
    return CR_BUSY;
  cr_memset(Entries, 0, (UINTN)Capacity * sizeof(*Entries));
  Voter->Core = Core; Voter->Entries = Entries; Voter->Capacity = Capacity;
  Voter->Initialized = TRUE;
  return CR_SUCCESS;
}

STATIC CR_CLOCK_VOTE_ENTRY *Find(CR_CLOCK_VOTER *Voter, VOID *Owner, ClockNode *Node) {
  for (UINTN Index = 0; Index < Voter->Capacity; ++Index)
    if (Voter->Entries[Index].Owner == Owner && Voter->Entries[Index].Node == Node)
      return &Voter->Entries[Index];
  return NULL;
}

CR_STATUS CrClockVotesReserve(CR_CLOCK_VOTER *Voter, VOID *Owner, ClockNode *Node, UINT64 RateHz) {
  if (Owner == NULL) return CR_INVALID_PARAMETER;
  CR_STATUS Status = CrClockVotesValidate(Voter, Node, RateHz);
  if (CR_ERROR(Status)) return Status;
  CR_CLOCK_VOTE_ENTRY *Entry = Find(Voter, Owner, Node);
  if (Entry == NULL) {
    for (UINT32 Index = 0; Index < Voter->Capacity; ++Index)
      if (Voter->Entries[Index].Owner == NULL) { Entry = &Voter->Entries[Index]; break; }
    if (Entry == NULL) return CR_OUT_OF_RESOURCES;
    Entry->Owner = Owner; Entry->Node = Node;
  }
  Entry->Reserved = TRUE;
  return CR_SUCCESS;
}

CR_STATUS CrClockVotesRequireReservation(CR_CLOCK_VOTER *Voter, VOID *Owner, ClockNode *Node) {
  CR_STATUS Status = Check(Voter);
  if (CR_ERROR(Status)) return Status;
  if (Owner == NULL || Node == NULL) return CR_INVALID_PARAMETER;
  CR_CLOCK_VOTE_ENTRY *Entry = Find(Voter, Owner, Node);
  return Entry != NULL && Entry->Reserved ? CR_SUCCESS : CR_NOT_FOUND;
}

STATIC BOOLEAN PendingParents(ClockNode *Node, UINT64 RateHz, UINT32 Depth, UINT32 *Budget) {
  if (Node == NULL || Depth == 32 || *Budget == 0 ||
      CR_ERROR(ClockParentDescription(Node))) return TRUE;
  --*Budget;
  if (Node->ReleasePending) return TRUE;
  for (UINTN Index = 0; Index < Node->ParentCount; ++Index) {
    if (!ClockNeedsParent(Node,Index,RateHz)) continue;
    if (PendingParents(Node->Parents[Index], ClockParentRate(Node,Index,RateHz),
                       Depth + 1, Budget)) return TRUE;
  }
  return FALSE;
}

CR_STATUS CrClockVotesGet(CR_CLOCK_VOTER *Voter, VOID *Owner, ClockNode *Node,
                         CR_CLOCK_VOTE_VALUE *Value, BOOLEAN *RecoveryPending) {
  CR_STATUS Status = Check(Voter);
  if (CR_ERROR(Status)) return Status;
  if (Owner == NULL || Node == NULL || Value == NULL || RecoveryPending == NULL)
    return CR_INVALID_PARAMETER;
  CR_CLOCK_VOTE_ENTRY *Entry = Find(Voter, Owner, Node);
  cr_memset(Value, 0, sizeof(*Value));
  UINT32 Budget = 512;
  UINT64 RateHz=Node->ReferenceCount ? Node->ActiveRateHz :
    Entry!=NULL ? Entry->Value.RateHz : 0;
  *RecoveryPending = PendingParents(Node, RateHz, 0, &Budget);
  if (Entry != NULL) {
    *Value = Entry->Value;
    *RecoveryPending = *RecoveryPending || Entry->RecoveryPending;
  }
  return CR_SUCCESS;
}

STATIC CR_STATUS Withdraw(CR_CLOCK_VOTER *Voter, CR_CLOCK_VOTE_ENTRY *Entry) {
  if (!Entry->Held) { Entry->RecoveryPending = FALSE; return CR_SUCCESS; }
  CR_STATUS Status = Entry->Node->Type == CLOCK_NODE_TYPE_GDSC ?
    ClockGdscSet(Voter->Core, Entry->Node, FALSE) :
    ClockEnable(Voter->Core, Entry->Node, Entry->Node->ActiveRateHz, FALSE);
  if (CR_ERROR(Status)) { Entry->RecoveryPending = TRUE; return Status; }
  Entry->Held = FALSE; Entry->RecoveryPending = FALSE;
  return CR_SUCCESS;
}

CR_STATUS CrClockVotesSet(CR_CLOCK_VOTER *Voter, VOID *Owner, ClockNode *Node,
                         CONST CR_CLOCK_VOTE_VALUE *Value) {
  CR_STATUS Status = Check(Voter);
  if (CR_ERROR(Status)) return Status;
  if (Owner == NULL || Node == NULL || Value == NULL || Value->Present > 1 ||
      Value->Enabled > 1 || (!Value->Present && (Value->Enabled || Value->RateHz != 0)))
    return CR_INVALID_PARAMETER;
  /* Removal remains possible even if this graph is no longer ready for a
     fresh enable. The adapter must keep mappings valid through withdrawal. */
  if (Value->Present) {
    Status = CrClockVotesValidate(Voter, Node, Value->RateHz);
    if (CR_ERROR(Status)) return Status;
  }
  CR_CLOCK_VOTE_ENTRY *Entry = Find(Voter, Owner, Node);
  if (Entry == NULL) {
    if (!Value->Present) return CR_SUCCESS;
    for (UINTN Index = 0; Index < Voter->Capacity; ++Index) {
      if (Voter->Entries[Index].Owner == NULL) { Entry = &Voter->Entries[Index]; break; }
    }
    if (Entry == NULL) return CR_OUT_OF_RESOURCES;
    Entry->Owner = Owner; Entry->Node = Node;
  }
  BOOLEAN Same = Entry->Value.Present == Value->Present &&
    Entry->Value.Enabled == Value->Enabled && Entry->Value.RateHz == Value->RateHz;
  if (Same && Value->Enabled && Entry->Held && !Entry->RecoveryPending) {
    UINT32 Budget = 512;
    if (PendingParents(Node, Value->RateHz, 0, &Budget)) return CR_NOT_READY;
    if (Node->Type!=CLOCK_NODE_TYPE_GDSC) {
      Status=ClockCheckAcquired(Voter->Core,Node,Value->RateHz);
      if (CR_ERROR(Status)) return Status;
    }
  }
  if (Entry->RecoveryPending || (Entry->Held && !Same)) {
    Status = Withdraw(Voter, Entry);
    if (CR_ERROR(Status)) return Status;
  }
  if (Value->Enabled && !Entry->Held) {
    UINT32 Before = Node->ReferenceCount;
    Status = Node->Type == CLOCK_NODE_TYPE_GDSC ? ClockGdscSet(Voter->Core, Node, TRUE) :
      ClockEnable(Voter->Core, Node, Value->RateHz, TRUE);
    if (CR_ERROR(Status)) {
      /* The core can retain one cleanup reference after a failed acquire. */
      Entry->Held = Node->ReferenceCount > Before;
      /* A rate replacement may have already withdrawn the old enabled vote.
         Retain uncertainty even when the rejected new rate acquired nothing. */
      Entry->RecoveryPending = Entry->Held || Entry->Value.Enabled;
      return Status;
    }
    Entry->Held = TRUE;
  }
  Entry->Value = *Value;
  if (!Value->Present && !Entry->Reserved) cr_memset(Entry, 0, sizeof(*Entry));
  return CR_SUCCESS;
}

STATIC CR_STATUS Release(CR_CLOCK_VOTER *Voter, VOID *Owner) {
  CR_STATUS Status = Check(Voter);
  if (CR_ERROR(Status)) return Status;
  /* Stop clock dependencies before dropping power-domain references. Entry
     slots can be reused, so reverse slot order alone is not a power order. */
  for (UINT32 Domains = 0; Domains != 2; ++Domains) {
    for (UINTN Index = Voter->Capacity; Index != 0; --Index) {
      CR_CLOCK_VOTE_ENTRY *Entry = &Voter->Entries[Index - 1];
      if (Entry->Owner == NULL || (Owner != NULL && Entry->Owner != Owner) ||
          ((Entry->Node->Type == CLOCK_NODE_TYPE_GDSC) != (Domains != 0))) continue;
      Status = Withdraw(Voter, Entry);
      if (CR_ERROR(Status)) return Status;
      cr_memset(Entry, 0, sizeof(*Entry));
    }
  }
  return CR_SUCCESS;
}
CR_STATUS CrClockVotesReleaseOwner(CR_CLOCK_VOTER *Voter, VOID *Owner) {
  if (Owner == NULL) return CR_INVALID_PARAMETER;
  return Release(Voter, Owner);
}
CR_STATUS CrClockVotesDrain(CR_CLOCK_VOTER *Voter) { return Release(Voter, NULL); }
CR_STATUS CrClockVotesUninit(CR_CLOCK_VOTER *Voter) {
  CR_STATUS Status = Check(Voter);
  if (CR_ERROR(Status)) return Status;
  for (UINTN Index = 0; Index < Voter->Capacity; ++Index)
    if (Voter->Entries[Index].Owner != NULL) return CR_BUSY;
  cr_memset(Voter, 0, sizeof(*Voter));
  return CR_SUCCESS;
}
