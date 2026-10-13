/* OS-independent supply leases. SPDX-License-Identifier: MIT */
#include <Library/RpmhRailLib.h>
#include <oskal/common.h>
#include <oskal/cr_mutex.h>

STATIC BOOLEAN ModeValid(UINT32 Mode) {
  return Mode == 2 || Mode == 3 || Mode == 4 || Mode == 6 || Mode == 7;
}
CR_STATUS CrRpmhRailValidate(CONST CR_RPMH_RAIL *Rail) {
  if (CR_ERROR(CrMutexCheckLevel()) || Rail == NULL) return CR_INVALID_PARAMETER;
  CONST CR_RPMH_RAIL *Seen[64];UINT32 Count = 0;
  for (CONST CR_RPMH_RAIL *Node = Rail; Node != NULL; Node = Node->Parent) {
    if (Count == ARRAY_SIZE(Seen)) return CR_COMPROMISED_DATA;
    if (Node->Name == NULL || Node->Name[0] == 0 ||
        (Node->Address!=0 && ((Node->Address&15U)!=0 || Node->Address>0xfffffff7U)) ||
        (Node->Parent != NULL && (Node->ParentMv == 0 || Node->ParentMv > 0x1fffU || !ModeValid(Node->ParentMode))))
      return CR_COMPROMISED_DATA;
    for (UINT32 I = 0; I < Count; ++I)
      if (Seen[I] == Node || (Node->Address!=0 && Seen[I]->Address!=0 &&
        (Node->Address>Seen[I]->Address ? Node->Address-Seen[I]->Address : Seen[I]->Address-Node->Address)<=8U))
        return CR_COMPROMISED_DATA;
    Seen[Count++] = Node;
  }
  return CR_SUCCESS;
}
STATIC CR_STATUS Check(CONST CR_RPMH_RAIL_IO *Io, CR_RPMH_RAIL *Rail) {
  if (Io == NULL || Io->GetAddress == NULL || Io->Write == NULL) return CR_INVALID_PARAMETER;
  return CrRpmhRailValidate(Rail);
}
CR_STATUS CrRpmhRailVote(CONST CR_RPMH_RAIL_IO *Io, CR_RPMH_RAIL *Rail,
                         CR_RPMH_RAIL_VOTE_KIND Kind, UINT32 Value) {
  CR_STATUS Status = Check(Io, Rail);if (CR_ERROR(Status)) return Status;
  BOOLEAN *Requested;
  if (Kind == CrRpmhRailVoltage) {
    if (Value > 0x1fffU) return CR_INVALID_PARAMETER;
    Requested = &Rail->VoltageRequested;
  } else if (Kind == CrRpmhRailMode) {
    if (Value != 0 && !ModeValid(Value)) return CR_INVALID_PARAMETER;
    Requested = &Rail->ModeRequested;
  } else if (Kind == CrRpmhRailEnable) {
    if (Value > 1) return CR_INVALID_PARAMETER;
    Requested = &Rail->EnableRequested;
  } else return CR_INVALID_PARAMETER;
  if (Rail->Address == 0) {
    UINT32 Address = 0;
    Status = Io->GetAddress(Io->Context, Rail->Name, &Address);
    if (CR_ERROR(Status)) return Status;
    if (Address == 0) return CR_NOT_FOUND;
    Rail->Address = Address;
    // Lazy resolution must not turn a child into an alias of its input supply.
    Status = CrRpmhRailValidate(Rail);
    if (CR_ERROR(Status)) {Rail->Address = 0;return Status;}
  }
  if (Value != 0) *Requested = TRUE;
  Status = Io->Write(Io->Context, Rail, Kind, Value);
  if (!CR_ERROR(Status) && Value == 0) *Requested = FALSE;
  return Status;
}
STATIC CR_STATUS Release(CONST CR_RPMH_RAIL_IO *Io, CR_RPMH_RAIL *Rail) {
  if (Rail->ReferenceCount > 1) {--Rail->ReferenceCount;return CR_SUCCESS;}
  Rail->ReferenceCount = 0;Rail->Ready = FALSE;
  CR_STATUS Status;
  if (Rail->EnableRequested) {
    Status = CrRpmhRailVote(Io, Rail, CrRpmhRailEnable, 0);if (CR_ERROR(Status)) return Status;
  }
  if (Rail->ModeRequested) {
    Status = CrRpmhRailVote(Io, Rail, CrRpmhRailMode, 0);if (CR_ERROR(Status)) return Status;
  }
  if (Rail->VoltageRequested) {
    Status = CrRpmhRailVote(Io, Rail, CrRpmhRailVoltage, 0);if (CR_ERROR(Status)) return Status;
  }
  if (Rail->ParentOwned) {
    if (Rail->Parent == NULL) return CR_COMPROMISED_DATA;
    Status = Release(Io, Rail->Parent);if (CR_ERROR(Status)) return Status;
    Rail->ParentOwned = FALSE;
  }
  return CR_SUCCESS;
}
CR_STATUS CrRpmhRailRelease(CONST CR_RPMH_RAIL_IO *Io, CR_RPMH_RAIL *Rail) {
  CR_STATUS Status = Check(Io, Rail);if (CR_ERROR(Status)) return Status;
  return Release(Io, Rail);
}
STATIC CR_STATUS Acquire(CONST CR_RPMH_RAIL_IO *Io, CR_RPMH_RAIL *Rail,
                          UINT32 VoltageMv, UINT8 Mode, BOOLEAN *Owned) {
  *Owned = FALSE;
  if (Rail->ReferenceCount == ~((UINTN)0)) return CR_OUT_OF_RESOURCES;
  if (Rail->ReferenceCount != 0) {
    if (!Rail->Ready || !Rail->VoltageRequested || !Rail->ModeRequested || !Rail->EnableRequested ||
        Rail->VoltageMv != VoltageMv || Rail->Mode != Mode) return CR_BUSY;
    ++Rail->ReferenceCount;*Owned = TRUE;return CR_SUCCESS;
  }
  if (Rail->VoltageRequested || Rail->ModeRequested || Rail->EnableRequested || Rail->ParentOwned)
    return CR_NOT_READY;
  Rail->ReferenceCount = 1;Rail->Ready = FALSE;Rail->VoltageMv = 0;Rail->Mode = 0;*Owned = TRUE;
  CR_STATUS Status;
  if (Rail->Parent != NULL) {
    Status = Acquire(Io, Rail->Parent, Rail->ParentMv, Rail->ParentMode, &Rail->ParentOwned);
    if (CR_ERROR(Status)) return Status;
  }
  Status = CrRpmhRailVote(Io, Rail, CrRpmhRailVoltage, VoltageMv);
  if (CR_ERROR(Status)) return Status;
  Rail->VoltageMv = VoltageMv;
  Status = CrRpmhRailVote(Io, Rail, CrRpmhRailMode, Mode);if (CR_ERROR(Status)) return Status;
  Rail->Mode = Mode;
  Status = CrRpmhRailVote(Io, Rail, CrRpmhRailEnable, 1);
  if (!CR_ERROR(Status)) Rail->Ready = TRUE;
  return Status;
}
CR_STATUS CrRpmhRailAcquire(CONST CR_RPMH_RAIL_IO *Io, CR_RPMH_RAIL *Rail,
                            UINT32 VoltageMv, UINT8 Mode, BOOLEAN *Owned) {
  if (Owned == NULL) return CR_INVALID_PARAMETER;
  *Owned = FALSE;
  CR_STATUS Status = Check(Io, Rail);if (CR_ERROR(Status)) return Status;
  if (VoltageMv == 0 || VoltageMv > 0x1fffU || !ModeValid(Mode)) return CR_INVALID_PARAMETER;
  return Acquire(Io, Rail, VoltageMv, Mode, Owned);
}

/* Resume only a reference belonging to the retained baseline journal or one
 * of its owned parent edges. Reissue exact positive votes after an uncertain
 * completion; a timeout never authorizes a disable or another acquisition. */
STATIC CR_STATUS RetainReference(CONST CR_RPMH_RAIL_IO *Io, CR_RPMH_RAIL *Rail,
                                UINT32 VoltageMv, UINT8 Mode, BOOLEAN *Owned) {
  CR_STATUS Status;
  if (!*Owned) {
    if (Rail->ReferenceCount == ~((UINTN)0)) return CR_OUT_OF_RESOURCES;
    if (Rail->ReferenceCount) {
      if (!Rail->Ready || !Rail->VoltageRequested || !Rail->ModeRequested ||
          !Rail->EnableRequested || Rail->VoltageMv!=VoltageMv || Rail->Mode!=Mode)
        return CR_BUSY;
      ++Rail->ReferenceCount; *Owned=TRUE; return CR_SUCCESS;
    }
    if (Rail->VoltageRequested || Rail->ModeRequested || Rail->EnableRequested || Rail->ParentOwned)
      return CR_NOT_READY;
    Rail->ReferenceCount=1; Rail->Ready=FALSE;
    Rail->VoltageMv=0; Rail->Mode=0; *Owned=TRUE;
  }
  if (Rail->Ready) {
    return Rail->ReferenceCount && Rail->VoltageRequested && Rail->ModeRequested &&
           Rail->EnableRequested && Rail->VoltageMv==VoltageMv && Rail->Mode==Mode ?
           CR_SUCCESS : CR_COMPROMISED_DATA;
  }
  if (Rail->ReferenceCount!=1 || (Rail->VoltageMv && Rail->VoltageMv!=VoltageMv) ||
      (Rail->Mode && Rail->Mode!=Mode)) return CR_COMPROMISED_DATA;
  if (Rail->Parent) {
    Status=RetainReference(Io,Rail->Parent,Rail->ParentMv,Rail->ParentMode,&Rail->ParentOwned);
    if (Status!=CR_SUCCESS) return Status;
  }
  Status=CrRpmhRailVote(Io,Rail,CrRpmhRailVoltage,VoltageMv);
  if (Status!=CR_SUCCESS) return CR_ERROR(Status)?Status:CR_DEVICE_ERROR;
  Rail->VoltageMv=VoltageMv;
  Status=CrRpmhRailVote(Io,Rail,CrRpmhRailMode,Mode);
  if (Status!=CR_SUCCESS) return CR_ERROR(Status)?Status:CR_DEVICE_ERROR;
  Rail->Mode=Mode;
  Status=CrRpmhRailVote(Io,Rail,CrRpmhRailEnable,1);
  if (Status!=CR_SUCCESS) return CR_ERROR(Status)?Status:CR_DEVICE_ERROR;
  Rail->Ready=TRUE;
  return CR_SUCCESS;
}
CR_STATUS CrRpmhRailRetain(CONST CR_RPMH_RAIL_IO *Io, CR_RPMH_RAIL *Rail,
                          UINT32 VoltageMv, UINT8 Mode, CR_RPMH_RAIL_RETAINED *Owner) {
  if (!Owner || !VoltageMv || VoltageMv>0x1fffU || !ModeValid(Mode)) return CR_INVALID_PARAMETER;
  CR_STATUS Status=Check(Io,Rail);
  if (Status!=CR_SUCCESS) return CR_ERROR(Status)?Status:CR_DEVICE_ERROR;
  if (Owner->Rail) {
    if (Owner->Rail!=Rail || Owner->VoltageMv!=VoltageMv || Owner->Mode!=Mode)
      return CR_BUSY;
  } else {
    if (Owner->Owned || Owner->VoltageMv || Owner->Mode) return CR_COMPROMISED_DATA;
    Owner->Rail=Rail; Owner->VoltageMv=VoltageMv; Owner->Mode=Mode;
  }
  return RetainReference(Io,Rail,VoltageMv,Mode,&Owner->Owned);
}
