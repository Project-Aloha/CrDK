/* Absolute owner leases over the shared regulator supply graph. MIT */
#include <Library/RpmhRailVoteLib.h>
#include <oskal/cr_mutex.h>
#include <oskal/cr_string.h>

STATIC BOOLEAN ModeValid(UINT8 Mode) {return Mode==2 || Mode==3 || Mode==4 || Mode==6 || Mode==7;}
STATIC CR_STATUS Check(CR_RPMH_RAIL_VOTER *Voter) {
  return !CR_ERROR(CrMutexCheckLevel()) && Voter!=NULL && Voter->Initialized ? CR_SUCCESS : CR_INVALID_PARAMETER;
}
STATIC BOOLEAN HasRail(CR_RPMH_RAIL_VOTER *Voter,CONST CR_RPMH_RAIL *Rail) {
  for (UINT32 I=0;I<Voter->RailCount;++I) if (&Voter->Rails[I]==Rail) return TRUE;
  return FALSE;
}
STATIC CR_RPMH_RAIL_ENTRY *Find(CR_RPMH_RAIL_VOTER *Voter,VOID *Owner,CR_RPMH_RAIL *Rail) {
  for (UINT32 I=0;I<Voter->Capacity;++I)
    if (Voter->Entries[I].Owner==Owner && Voter->Entries[I].Rail==Rail) return &Voter->Entries[I];
  return NULL;
}
STATIC BOOLEAN GraphPending(CONST CR_RPMH_RAIL *Rail) {
  UINT32 Depth=0;
  for (;Rail!=NULL && Depth<64;++Depth,Rail=Rail->Parent)
    if ((!Rail->Ready && Rail->ReferenceCount!=0) ||
        (Rail->ReferenceCount==0 && (Rail->VoltageRequested || Rail->ModeRequested ||
         Rail->EnableRequested || Rail->ParentOwned))) return TRUE;
  return Rail!=NULL;
}
STATIC BOOLEAN Overlap(UINTN A,UINTN As,UINTN B,UINTN Bs) {return A<B+Bs && B<A+As;}
CR_STATUS CrRpmhRailVotesInit(CR_RPMH_RAIL_VOTER *Voter,CONST CR_RPMH_RAIL_IO *Io,
    CR_RPMH_RAIL *Rails,UINT32 RailCount,CR_RPMH_RAIL_ENTRY *Entries,UINT32 Capacity) {
  if (CR_ERROR(CrMutexCheckLevel()) || Voter==NULL || Io==NULL || Io->GetAddress==NULL || Io->Write==NULL ||
      Rails==NULL || Entries==NULL || RailCount==0 || RailCount>64 || Capacity==0 || Capacity>65536 ||
      (UINTN)Rails%sizeof(VOID *) || (UINTN)Entries%sizeof(VOID *)) return CR_INVALID_PARAMETER;
  if (Voter->Initialized) return CR_ALREADY_STARTED;
  UINTN V=(UINTN)Voter,R=(UINTN)Rails,E=(UINTN)Entries;
  UINTN Rs=sizeof(*Rails)*RailCount,Es=sizeof(*Entries)*Capacity;
  if (V>~((UINTN)0)-sizeof(*Voter) || R>~((UINTN)0)-Rs || E>~((UINTN)0)-Es ||
      Overlap(V,sizeof(*Voter),R,Rs) || Overlap(V,sizeof(*Voter),E,Es) || Overlap(R,Rs,E,Es)) return CR_INVALID_PARAMETER;
  // Validate inventory membership before following any parent pointer.
  for (UINT32 I=0;I<RailCount;++I) {
    if (Rails[I].Name==NULL || Rails[I].Name[0]==0) return CR_INVALID_PARAMETER;
    if (Rails[I].Parent!=NULL) {
      BOOLEAN Found=FALSE;
      for (UINT32 P=0;P<RailCount;++P) if (&Rails[P]==Rails[I].Parent) {Found=TRUE;break;}
      if (!Found) return CR_INVALID_PARAMETER;
    }
    for (UINT32 P=0;P<I;++P)
      if (cr_strcmp(Rails[P].Name,Rails[I].Name)==0 || (Rails[I].Address!=0 && Rails[P].Address!=0 &&
         (Rails[I].Address>Rails[P].Address ? Rails[I].Address-Rails[P].Address : Rails[P].Address-Rails[I].Address)<=8U))
        return CR_INVALID_PARAMETER;
  }
  for (UINT32 I=0;I<RailCount;++I) {
    CR_STATUS Status=CrRpmhRailValidate(&Rails[I]);if (CR_ERROR(Status)) return Status;
  }
  CR_RPMH_RAIL_IO Copy=*Io;
  cr_memset(Entries,0,Es);
  Voter->Io=Copy;Voter->Rails=Rails;Voter->RailCount=RailCount;Voter->Entries=Entries;
  Voter->Capacity=Capacity;Voter->Initialized=TRUE;return CR_SUCCESS;
}
CR_STATUS CrRpmhRailVotesValidate(CR_RPMH_RAIL_VOTER *Voter,CR_RPMH_RAIL *Rail,UINT32 VoltageMv,UINT8 Mode) {
  CR_STATUS Status=Check(Voter);if (CR_ERROR(Status)) return Status;
  if (!HasRail(Voter,Rail) || VoltageMv>0x1fffU || (Mode!=0 && !ModeValid(Mode)) ||
      (VoltageMv!=0 && Mode==0)) return CR_INVALID_PARAMETER;
  Status=CrRpmhRailValidate(Rail);if (CR_ERROR(Status)) return Status;
  for (CONST CR_RPMH_RAIL *Node=Rail;Node!=NULL;Node=Node->Parent)
    if (Node->Address==0) return CR_NOT_READY;
  return CR_SUCCESS;
}
CR_STATUS CrRpmhRailVotesReserve(CR_RPMH_RAIL_VOTER *Voter,VOID *Owner,CR_RPMH_RAIL *Rail,
                                UINT32 VoltageMv,UINT8 Mode) {
  if (Owner==NULL) return CR_INVALID_PARAMETER;
  CR_STATUS Status=CrRpmhRailVotesValidate(Voter,Rail,VoltageMv,Mode);
  if (CR_ERROR(Status)) return Status;
  CR_RPMH_RAIL_ENTRY *Entry=Find(Voter,Owner,Rail);
  if (Entry==NULL) {
    for (UINT32 I=0;I<Voter->Capacity;++I)
      if (Voter->Entries[I].Owner==NULL) {Entry=&Voter->Entries[I];break;}
    if (Entry==NULL) return CR_OUT_OF_RESOURCES;
    Entry->Owner=Owner;Entry->Rail=Rail;
  }
  Entry->Reserved=TRUE;return CR_SUCCESS;
}
CR_STATUS CrRpmhRailVotesRequireReservation(CR_RPMH_RAIL_VOTER *Voter,VOID *Owner,CR_RPMH_RAIL *Rail) {
  CR_STATUS Status=Check(Voter);if (CR_ERROR(Status)) return Status;
  if (Owner==NULL || !HasRail(Voter,Rail)) return CR_INVALID_PARAMETER;
  CR_RPMH_RAIL_ENTRY *Entry=Find(Voter,Owner,Rail);
  return Entry!=NULL && Entry->Reserved ? CR_SUCCESS : CR_NOT_FOUND;
}
CR_STATUS CrRpmhRailVotesGet(CR_RPMH_RAIL_VOTER *Voter,VOID *Owner,CR_RPMH_RAIL *Rail,
                            CR_RPMH_RAIL_VALUE *Value,BOOLEAN *Pending) {
  CR_STATUS Status=Check(Voter);if (CR_ERROR(Status)) return Status;
  if (Owner==NULL || Value==NULL || Pending==NULL || !HasRail(Voter,Rail)) return CR_INVALID_PARAMETER;
  CR_RPMH_RAIL_ENTRY *Entry=Find(Voter,Owner,Rail);
  *Value=Entry!=NULL ? Entry->Value : (CR_RPMH_RAIL_VALUE){0};
  *Pending=(Entry!=NULL && Entry->RecoveryPending) || GraphPending(Rail);
  return CR_SUCCESS;
}
STATIC BOOLEAN SameValue(CONST CR_RPMH_RAIL_VALUE *A,CONST CR_RPMH_RAIL_VALUE *B) {
  return A->Present==B->Present && A->Enabled==B->Enabled && A->VoltageMv==B->VoltageMv && A->Mode==B->Mode;
}
STATIC CR_STATUS Withdraw(CR_RPMH_RAIL_VOTER *Voter,CR_RPMH_RAIL_ENTRY *Entry) {
  if (!Entry->Held) return CR_SUCCESS;
  CR_STATUS Status=CrRpmhRailRelease(&Voter->Io,Entry->Rail);
  if (CR_ERROR(Status)) {Entry->RecoveryPending=TRUE;return Status;}
  Entry->Held=FALSE;return CR_SUCCESS;
}
CR_STATUS CrRpmhRailVotesSet(CR_RPMH_RAIL_VOTER *Voter,VOID *Owner,CR_RPMH_RAIL *Rail,
                            CONST CR_RPMH_RAIL_VALUE *Input) {
  CR_STATUS Status=Check(Voter);if (CR_ERROR(Status)) return Status;
  if (Owner==NULL || Input==NULL || Input->Present>1 || Input->Enabled>1 ||
      (!Input->Present && (Input->Enabled || Input->VoltageMv || Input->Mode)) ||
      (Input->Enabled && (Input->VoltageMv==0 || !ModeValid(Input->Mode)))) return CR_INVALID_PARAMETER;
  CR_RPMH_RAIL_VALUE Value=*Input;
  Status=CrRpmhRailVotesValidate(Voter,Rail,Value.VoltageMv,Value.Mode);if (CR_ERROR(Status)) return Status;
  CR_RPMH_RAIL_ENTRY *Entry=Find(Voter,Owner,Rail);
  if (Entry==NULL) {
    if (!Value.Present) return CR_SUCCESS;
    for (UINT32 I=0;I<Voter->Capacity;++I)
      if (Voter->Entries[I].Owner==NULL) {Entry=&Voter->Entries[I];break;}
    if (Entry==NULL) return CR_OUT_OF_RESOURCES;
    Entry->Owner=Owner;Entry->Rail=Rail;
  }
  if (!Entry->RecoveryPending && SameValue(&Entry->Value,&Value)) {
    // An unchanged disabled/absent owner holds no cleanup lease. Foreign
    // uncertainty belongs to Get/readiness, not this owner's journal restore.
    return Value.Enabled && GraphPending(Rail) ? CR_NOT_READY : CR_SUCCESS;
  }
  if (Entry->Held && (Entry->RecoveryPending || !Value.Enabled ||
      Value.VoltageMv!=Entry->Value.VoltageMv || Value.Mode!=Entry->Value.Mode)) {
    Status=Withdraw(Voter,Entry);if (CR_ERROR(Status)) return Status;
  }
  if (Value.Enabled && !Entry->Held) {
    BOOLEAN Owned=FALSE;
    Status=CrRpmhRailAcquire(&Voter->Io,Rail,Value.VoltageMv,Value.Mode,&Owned);
    Entry->Held=Owned;
    if (CR_ERROR(Status)) {
      Entry->RecoveryPending=Owned || (Entry->Value.Enabled && !Entry->Held);
      if (!Entry->RecoveryPending && !Entry->Value.Present && !Entry->Reserved) cr_memset(Entry,0,sizeof(*Entry));
      return Status;
    }
  }
  Entry->Value=Value;Entry->RecoveryPending=FALSE;
  if (!Value.Present && !Entry->Reserved) cr_memset(Entry,0,sizeof(*Entry));
  return CR_SUCCESS;
}
STATIC CR_STATUS ReleaseOwners(CR_RPMH_RAIL_VOTER *Voter,VOID *Owner) {
  CR_STATUS Result=CR_SUCCESS;
  for (UINT32 I=Voter->Capacity;I!=0;--I) {
    CR_RPMH_RAIL_ENTRY *Entry=&Voter->Entries[I-1];
    if (Entry->Owner==NULL || (Owner!=NULL && Entry->Owner!=Owner)) continue;
    CR_STATUS Status=Withdraw(Voter,Entry);
    if (CR_ERROR(Status)) {if (!CR_ERROR(Result)) Result=Status;}
    else cr_memset(Entry,0,sizeof(*Entry));
  }
  return Result;
}
CR_STATUS CrRpmhRailVotesReleaseOwner(CR_RPMH_RAIL_VOTER *Voter,VOID *Owner) {
  CR_STATUS Status=Check(Voter);if (CR_ERROR(Status)) return Status;
  return Owner!=NULL ? ReleaseOwners(Voter,Owner) : CR_INVALID_PARAMETER;
}
CR_STATUS CrRpmhRailVotesDrain(CR_RPMH_RAIL_VOTER *Voter) {
  CR_STATUS Status=Check(Voter);return CR_ERROR(Status) ? Status : ReleaseOwners(Voter,NULL);
}
CR_STATUS CrRpmhRailVotesUninit(CR_RPMH_RAIL_VOTER *Voter) {
  CR_STATUS Status=Check(Voter);if (CR_ERROR(Status)) return Status;
  for (UINT32 I=0;I<Voter->Capacity;++I) if (Voter->Entries[I].Owner!=NULL) return CR_BUSY;
  cr_memset(Voter,0,sizeof(*Voter));return CR_SUCCESS;
}
