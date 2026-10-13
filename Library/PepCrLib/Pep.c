/* Kodiak DEVICE/COMPONENT/D/F/P-state programs with transactional providers.
 * SPDX-License-Identifier: MIT */
#include <Library/PepCrLib.h>
#include <oskal/cr_string.h>

#define PEP_MAX_DEVICES 256U
#define PEP_MAX_RECORDS 64U
#define PEP_MAX_OPERATIONS 256U

STATIC BOOLEAN TextValid(CR_PEP_TEXT T) {
  if (T.Data == NULL || T.Length == 0 || T.Length > 255U) return FALSE;
  for (UINTN I = 0; I < T.Length; ++I)
    if (T.Data[I] == 0) return FALSE;
  return TRUE;
}
STATIC BOOLEAN TextIs(CR_PEP_TEXT T, CONST CHAR8 *S) {
  UINTN I = 0;
  if (!TextValid(T)) return FALSE;
  while (S[I] != 0) {
    if (I == T.Length || T.Data[I] != S[I]) return FALSE;
    ++I;
  }
  return I == T.Length;
}
/* DEVICE names are absolute ACPI paths. ASL short NameSegs and the padded
 * BIOS paths delivered by PoFx identify the same object (ACPI 6.5 19.3.1).
 * Only device identity uses this comparison; resource names stay literal. */
STATIC BOOLEAN IdSegment(CR_PEP_TEXT Id, UINTN *Position, UINT32 *Segment) {
  UINT32 Name = 0x5f5f5f5fU;
  UINTN Count = 0;
  while (*Position < Id.Length && Id.Data[*Position] != '.') {
    CHAR8 C = Id.Data[(*Position)++];
    if (C >= 'a' && C <= 'z') C -= 'a' - 'A';
    if (Count == 4 || !(C == '_' || (C >= 'A' && C <= 'Z') ||
                       (Count != 0 && C >= '0' && C <= '9'))) return FALSE;
    Name = (Name & ~(0xffU << (8 * Count))) | ((UINT32)C << (8 * Count));
    ++Count;
  }
  if (Count == 0) return FALSE;
  if (*Position < Id.Length && ++*Position == Id.Length) return FALSE;
  *Segment = Name;
  return TRUE;
}
STATIC BOOLEAN IdValid(CR_PEP_TEXT Id) {
  if (!TextValid(Id) || Id.Length < 2 || Id.Data[0] != '\\') return FALSE;
  UINTN Position = 1; UINT32 Segment;
  while (Position < Id.Length)
    if (!IdSegment(Id, &Position, &Segment)) return FALSE;
  return TRUE;
}
STATIC BOOLEAN IdEqual(CR_PEP_TEXT A, CR_PEP_TEXT B) {
  if (!TextValid(A) || !TextValid(B) || A.Length < 2 || B.Length < 2 ||
      A.Data[0] != '\\' || B.Data[0] != '\\') return FALSE;
  UINTN APosition = 1, BPosition = 1;
  while (APosition < A.Length && BPosition < B.Length) {
    UINT32 ASegment, BSegment;
    if (!IdSegment(A, &APosition, &ASegment) ||
        !IdSegment(B, &BPosition, &BSegment) || ASegment != BSegment) return FALSE;
  }
  return APosition == A.Length && BPosition == B.Length;
}
STATIC BOOLEAN Package(CONST CR_PEP_VALUE *V, UINTN Min, UINTN Max) {
  return V != NULL && V->Type == CrPepPackage &&
    V->Package.Count >= Min && V->Package.Count <= Max &&
    (V->Package.Count == 0 || V->Package.Values != NULL);
}
STATIC BOOLEAN Tag(CONST CR_PEP_VALUE *V, CONST CHAR8 *S) {
  return Package(V, 1, PEP_MAX_OPERATIONS + 2U) &&
    V->Package.Values[0].Type == CrPepString &&
    TextIs(V->Package.Values[0].String, S);
}
STATIC BOOLEAN Integer(CONST CR_PEP_VALUE *V, UINT64 Max) {
  return V->Type == CrPepInteger && V->Integer <= Max;
}

CR_STATUS CrPepDecodeResource(CONST CR_PEP_VALUE *Record, CR_PEP_RESOURCE_OP *Op) {
  CONST CR_PEP_VALUE *P;
  UINTN Count, Name = 0, Secondary = 0, Qualifier = 0;
  if (Op == NULL || !Package(Record, 2, 3)) return CR_INVALID_PARAMETER;
  cr_memset(Op, 0, sizeof(*Op));
  if (Tag(Record,"TLMMGPIO_V2")) {
    if (!Package(Record,3,3) || !Integer(&Record->Package.Values[1],CR_MAX_UINT64) ||
        !Package(&Record->Package.Values[2],6,7)) return CR_INVALID_PARAMETER;
    Op->Kind=CrPepTlmmGpioV2;
    Op->Arguments[Op->ArgumentCount++]=Record->Package.Values[1].Integer;
    P=Record->Package.Values[2].Package.Values;Count=Record->Package.Values[2].Package.Count;
    for (UINTN I=0;I<Count;++I) {
      if (!Integer(&P[I],CR_MAX_UINT64)) return CR_INVALID_PARAMETER;
      Op->Arguments[Op->ArgumentCount++]=P[I].Integer;
    }
    return CR_SUCCESS;
  }
  if (!Package(Record,2,2)) return CR_INVALID_PARAMETER;
  if (Tag(Record,"DELAY") || Tag(Record,"TLMMGPIO")) {
    BOOLEAN Delay=Tag(Record,"DELAY");
    if (!Package(&Record->Package.Values[1],Delay ? 1 : 6,Delay ? 1 : 7)) return CR_INVALID_PARAMETER;
    Op->Kind=Delay ? CrPepDelay : CrPepTlmmGpio;
    P=Record->Package.Values[1].Package.Values;Count=Record->Package.Values[1].Package.Count;
    for (UINTN I=0;I<Count;++I) {
      if (!Integer(&P[I],CR_MAX_UINT64)) return CR_INVALID_PARAMETER;
      Op->Arguments[Op->ArgumentCount++]=P[I].Integer;
    }
    return CR_SUCCESS;
  }
  if (!Package(&Record->Package.Values[1],2,8)) return CR_INVALID_PARAMETER;
  P = Record->Package.Values[1].Package.Values;
  Count = Record->Package.Values[1].Package.Count;
  if (Tag(Record, "CLOCK")) {
    Op->Kind = CrPepClock;
    if (!Integer(&P[1], 8) ||
        !((Count == 2 && (P[1].Integer == 1 || P[1].Integer == 2)) ||
          (Count == 4 && (P[1].Integer == 3 || P[1].Integer == 8))))
      return CR_UNSUPPORTED;
  } else if (Tag(Record, "FOOTSWITCH")) {
    Op->Kind = CrPepFootswitch;
    if ((Count != 2 && Count != 3) || !Integer(&P[1], 2) || P[1].Integer == 0)
      return CR_UNSUPPORTED;
  } else if (Tag(Record, "PMICVREGVOTE")) {
    Op->Kind = CrPepVreg;
    if ((Count != 6 && Count != 8) || !Integer(&P[1], 1) || P[1].Integer != 1)
      return CR_UNSUPPORTED;
    if (Count == 8) {
      if (P[6].Type != CrPepString || !TextValid(P[6].String) ||
          P[7].Type != CrPepString || !TextValid(P[7].String)) return CR_INVALID_PARAMETER;
      Op->DriverName = P[6].String;Op->VoteType = P[7].String;Qualifier = 6;
    }
  } else if (Tag(Record, "NPARESOURCE")) {
    Op->Kind = CrPepNpa; Name = 1;
    if (Count != 3 || !Integer(&P[0], 1) || P[0].Integer != 1)
      return CR_UNSUPPORTED;
  } else if (Tag(Record,"PMICGPIO")) {
    Op->Kind=CrPepPmicGpio;
    if (Count!=8) return CR_UNSUPPORTED;
  } else if (Tag(Record, "BUSARB")) {
    Op->Kind = CrPepBusArb; Name = 1; Secondary = 2;
    if (Count != 5 || !Integer(&P[0], 3) || P[0].Integer != 3)
      return CR_UNSUPPORTED;
  } else return CR_UNSUPPORTED;
  if (P[Name].Type != CrPepString || !TextValid(P[Name].String))
    return CR_INVALID_PARAMETER;
  Op->Name = P[Name].String;
  if (Secondary != 0) {
    if (P[Secondary].Type != CrPepString || !TextValid(P[Secondary].String))
      return CR_INVALID_PARAMETER;
    Op->SecondaryName = P[Secondary].String;
  }
  for (UINTN I = 0; I < Count; ++I) {
    if (I == Name || (Secondary != 0 && I == Secondary) || (Qualifier != 0 && I >= Qualifier)) continue;
    if (!Integer(&P[I], CR_MAX_UINT64)) return CR_INVALID_PARAMETER;
    Op->Arguments[Op->ArgumentCount++] = P[I].Integer;
  }
  return CR_SUCCESS;
}

STATIC CR_STATUS ValidateState(CONST CR_PEP_VALUE *Record, UINT64 MaxState) {
  CR_PEP_RESOURCE_OP Op;
  if (!Package(Record, 2, PEP_MAX_OPERATIONS + 2U) ||
      !Integer(&Record->Package.Values[1], MaxState)) return CR_INVALID_PARAMETER;
  for (UINTN I = 2; I < Record->Package.Count; ++I) {
    CR_STATUS Status = CrPepDecodeResource(&Record->Package.Values[I], &Op);
    if (CR_ERROR(Status)) return Status;
  }
  return CR_SUCCESS;
}
STATIC CR_STATUS ValidateComponent(CONST CR_PEP_VALUE *Record) {
  UINT64 FMask = 0, PMask = 0;
  if (!Package(Record, 2, PEP_MAX_RECORDS) ||
      !Integer(&Record->Package.Values[1], 31)) return CR_INVALID_PARAMETER;
  for (UINTN I = 2; I < Record->Package.Count; ++I) {
    CONST CR_PEP_VALUE *S = &Record->Package.Values[I];
    UINT64 *Mask;
    CR_STATUS Status;
    if (Tag(S, "FSTATE")) Mask = &FMask;
    else if (Tag(S, "PSTATE")) Mask = &PMask;
    else return CR_UNSUPPORTED;
    Status = ValidateState(S, 63);
    if (CR_ERROR(Status)) return Status;
    UINT64 Bit = 1ULL << S->Package.Values[1].Integer;
    if ((*Mask & Bit) != 0) return CR_INVALID_PARAMETER;
    *Mask |= Bit;
  }
  return CR_SUCCESS;
}

CR_STATUS CrPepPolicyInit(CR_PEP_POLICY *Policy, CONST CR_PEP_VALUE *Root) {
  if (Policy == NULL) return CR_INVALID_PARAMETER;
  Policy->Root = NULL;
  if (!Package(Root, 0, PEP_MAX_DEVICES)) return CR_INVALID_PARAMETER;
  for (UINTN I = 0; I < Root->Package.Count; ++I) {
    CONST CR_PEP_VALUE *D = &Root->Package.Values[I], *V;
    UINT64 DMask = 0, CMask = 0;
    UINT32 Abandon = 0;
    BOOLEAN HasAbandon = FALSE;
    if (!Tag(D, "DEVICE") || !Package(D, 2, PEP_MAX_RECORDS))
      return CR_INVALID_PARAMETER;
    V = D->Package.Values;
    if (V[1].Type != CrPepString || !IdValid(V[1].String)) return CR_INVALID_PARAMETER;
    for (UINTN J = 0; J < I; ++J)
      if (IdEqual(V[1].String, Root->Package.Values[J].Package.Values[1].String))
        return CR_INVALID_PARAMETER;
    for (UINTN J = 2; J < D->Package.Count; ++J) {
      CONST CR_PEP_VALUE *R = &V[J];
      CR_STATUS Status;
      UINT64 *Mask, Bit;
      if (Tag(R, "DSTATE")) {
        Status = ValidateState(R, 3); Mask = &DMask;
      } else if (Tag(R, "COMPONENT")) {
        Status = ValidateComponent(R); Mask = &CMask;
      } else if (Tag(R, "ABANDON_DSTATE")) {
        if (!Package(R, 2, 2) || HasAbandon ||
            !Integer(&R->Package.Values[1], 3)) return CR_INVALID_PARAMETER;
        HasAbandon = TRUE; Abandon = (UINT32)R->Package.Values[1].Integer;
        continue;
      } else return CR_UNSUPPORTED;
      if (CR_ERROR(Status)) return Status;
      Bit = 1ULL << R->Package.Values[1].Integer;
      if ((*Mask & Bit) != 0) return CR_INVALID_PARAMETER;
      *Mask |= Bit;
    }
    if (HasAbandon && (DMask & (1ULL << Abandon)) == 0)
      return CR_INVALID_PARAMETER;
  }
  Policy->Root = Root;
  return CR_SUCCESS;
}

CR_STATUS CrPepFindDevice(CONST CR_PEP_POLICY *Policy, CR_PEP_TEXT Id, UINT32 *Index) {
  if (Policy == NULL || Policy->Root == NULL || Index == NULL || !TextValid(Id))
    return CR_INVALID_PARAMETER;
  for (UINTN I = 0; I < Policy->Root->Package.Count; ++I) {
    if (IdEqual(Id, Policy->Root->Package.Values[I].Package.Values[1].String)) {
      *Index = (UINT32)I; return CR_SUCCESS;
    }
  }
  return CR_NOT_FOUND;
}
STATIC CONST CR_PEP_VALUE *Device(CONST CR_PEP_POLICY *Policy, UINT32 Index) {
  if (Policy == NULL || Policy->Root == NULL || Index >= Policy->Root->Package.Count)
    return NULL;
  return &Policy->Root->Package.Values[Index];
}
STATIC CONST CR_PEP_VALUE *Find(CONST CR_PEP_VALUE *Record, CONST CHAR8 *Name,
                                UINT32 Number) {
  for (UINTN I = 2; I < Record->Package.Count; ++I) {
    CONST CR_PEP_VALUE *V = &Record->Package.Values[I];
    if (Tag(V, Name) && V->Package.Values[1].Integer == Number) return V;
  }
  return NULL;
}
CR_STATUS CrPepGetAbandonState(CONST CR_PEP_POLICY *Policy, UINT32 Index, UINT32 *State) {
  CONST CR_PEP_VALUE *D = Device(Policy, Index);
  if (D == NULL || State == NULL) return CR_INVALID_PARAMETER;
  for (UINTN I = 2; I < D->Package.Count; ++I) {
    CONST CR_PEP_VALUE *V = &D->Package.Values[I];
    if (Tag(V, "ABANDON_DSTATE")) {
      *State = (UINT32)V->Package.Values[1].Integer; return CR_SUCCESS;
    }
  }
  return CR_NOT_FOUND;
}
CR_STATUS CrPepGetDeviceInfo(CONST CR_PEP_POLICY *Policy, UINT32 Index,
                             CR_PEP_DEVICE_INFO *Info) {
  CONST CR_PEP_VALUE *D = Device(Policy, Index);
  if (D == NULL || Info == NULL) return CR_INVALID_PARAMETER;
  cr_memset(Info, 0, sizeof(*Info));
  Info->Id = D->Package.Values[1].String;
  for (UINTN I = 2; I < D->Package.Count; ++I) {
    CONST CR_PEP_VALUE *V = &D->Package.Values[I];
    UINT32 Number = (UINT32)V->Package.Values[1].Integer;
    if (Tag(V, "DSTATE")) Info->DStateMask |= 1U << Number;
    else if (Tag(V, "COMPONENT")) Info->ComponentMask |= 1U << Number;
    else if (Tag(V, "ABANDON_DSTATE")) {
      Info->HasAbandonState = TRUE; Info->AbandonState = Number;
    }
  }
  return CR_SUCCESS;
}
CR_STATUS CrPepGetComponentInfo(CONST CR_PEP_POLICY *Policy, UINT32 Index,
                                UINT32 Component, CR_PEP_COMPONENT_INFO *Info) {
  CONST CR_PEP_VALUE *D = Device(Policy, Index), *C;
  if (D == NULL || Info == NULL || Component >= 32U) return CR_INVALID_PARAMETER;
  C = Find(D, "COMPONENT", Component);
  if (C == NULL) return CR_NOT_FOUND;
  cr_memset(Info, 0, sizeof(*Info));
  for (UINTN I = 2; I < C->Package.Count; ++I) {
    CONST CR_PEP_VALUE *V = &C->Package.Values[I];
    UINT64 Bit = 1ULL << V->Package.Values[1].Integer;
    if (Tag(V, "FSTATE")) Info->FStateMask |= Bit;
    else if (Tag(V, "PSTATE")) Info->PStateMask |= Bit;
  }
  return CR_SUCCESS;
}
STATIC CONST CR_PEP_VALUE *StateRecord(CONST CR_PEP_VALUE *D,
                                      CR_PEP_TRANSITION Transition) {
  CONST CR_PEP_VALUE *S = NULL;
  CR_PEP_STATE_KIND Kind = Transition.Kind;
  UINT32 Component = Transition.Component, State = Transition.State;
  if (Kind == CrPepDState) S = Find(D, "DSTATE", State);
  else if (Kind == CrPepFState || Kind == CrPepPState) {
    CONST CR_PEP_VALUE *C = Find(D, "COMPONENT", Component);
    if (C != NULL) S = Find(C, Kind == CrPepFState ? "FSTATE" : "PSTATE", State);
  }
  return S;
}
STATIC CR_STATUS ValidateResources(CONST CR_PEP_VALUE *S, VOID *Owner,
                                   CONST CR_PEP_RESOURCE_IO *Io) {
  CR_PEP_RESOURCE_OP Op;
  for (UINTN I = 2; I < S->Package.Count; ++I) {
    CR_STATUS Status = CrPepDecodeResource(&S->Package.Values[I], &Op);
    if (!CR_ERROR(Status)) Status = Io->Validate(Io->Context, Owner, &Op);
    if (CR_ERROR(Status)) return Status;
  }
  return CR_SUCCESS;
}
CR_STATUS CrPepValidateDeviceResources(CONST CR_PEP_POLICY *Policy, UINT32 Index,
                                      VOID *Owner, CONST CR_PEP_RESOURCE_IO *Io) {
  CONST CR_PEP_VALUE *D = Device(Policy, Index);
  if (D == NULL || Owner == NULL || Io == NULL || Io->Validate == NULL)
    return CR_INVALID_PARAMETER;
  for (UINTN I = 2; I < D->Package.Count; ++I) {
    CONST CR_PEP_VALUE *V = &D->Package.Values[I];
    CR_STATUS Status;
    if (Tag(V, "ABANDON_DSTATE")) continue;
    if (Tag(V, "COMPONENT")) {
      for (UINTN J = 2; J < V->Package.Count; ++J) {
        Status = ValidateResources(&V->Package.Values[J], Owner, Io);
        if (CR_ERROR(Status)) return Status;
      }
    } else {
      Status = ValidateResources(V, Owner, Io);
      if (CR_ERROR(Status)) return Status;
    }
  }
  return CR_SUCCESS;
}
CR_STATUS CrPepValidateStates(CONST CR_PEP_POLICY *Policy, UINT32 Index,
                             CONST CR_PEP_TRANSITION *Transitions, UINT32 Count,
                             VOID *Owner, CONST CR_PEP_RESOURCE_IO *Io) {
  CONST CR_PEP_VALUE *D = Device(Policy, Index);
  if (D == NULL || Transitions == NULL || Count == 0 || Count > 65U ||
      Owner == NULL || Io == NULL || Io->Validate == NULL) return CR_INVALID_PARAMETER;
  // Missing or invalid later transitions reject the whole batch before even
  // invoking a provider's pure preflight callback.
  for (UINT32 I = 0; I < Count; ++I) {
    if ((UINT32)Transitions[I].Kind > CrPepPState) return CR_INVALID_PARAMETER;
    if (StateRecord(D, Transitions[I]) == NULL) return CR_NOT_FOUND;
  }
  for (UINT32 I = 0; I < Count; ++I) {
    CR_STATUS Status = ValidateResources(StateRecord(D, Transitions[I]), Owner, Io);
    if (CR_ERROR(Status)) return Status;
  }
  return CR_SUCCESS;
}
CR_STATUS CrPepApplyStates(CONST CR_PEP_POLICY *Policy, UINT32 Index,
                          CONST CR_PEP_TRANSITION *Transitions, UINT32 Count,
                          VOID *Owner, CONST CR_PEP_RESOURCE_IO *Io, CR_PEP_RESULT *Result) {
  CR_STATUS Status;
  if (Result == NULL) return CR_INVALID_PARAMETER;
  Result->Status = CR_INVALID_PARAMETER;
  Result->RollbackStatus = CR_SUCCESS; Result->Applied = 0;
  if (Io == NULL || Io->Begin == NULL || Io->Apply == NULL ||
      Io->Commit == NULL || Io->Rollback == NULL) return Result->Status;
  Status = CrPepValidateStates(Policy, Index, Transitions, Count, Owner, Io);
  if (CR_ERROR(Status)) { Result->Status = Status; return Status; }
  Status = Io->Begin(Io->Context, Owner);
  if (CR_ERROR(Status)) { Result->Status = Status; return Status; }
  CONST CR_PEP_VALUE *D = Device(Policy, Index);
  for (UINT32 T = 0; T < Count && !CR_ERROR(Status); ++T) {
    CONST CR_PEP_VALUE *S = StateRecord(D, Transitions[T]);
    for (UINTN I = 2; I < S->Package.Count; ++I) {
      CR_PEP_RESOURCE_OP Op;
      Status = CrPepDecodeResource(&S->Package.Values[I], &Op);
      if (!CR_ERROR(Status)) Status = Io->Apply(Io->Context, Owner, &Op);
      if (CR_ERROR(Status)) break;
      ++Result->Applied;
    }
  }
  if (!CR_ERROR(Status)) Status = Io->Commit(Io->Context, Owner);
  Result->Status = Status;
  if (CR_ERROR(Status)) Result->RollbackStatus = Io->Rollback(Io->Context, Owner);
  return Status;
}
CR_STATUS CrPepApplyState(CONST CR_PEP_POLICY *Policy, UINT32 Index,
                         CR_PEP_STATE_KIND Kind, UINT32 Component, UINT32 State,
                         VOID *Owner, CONST CR_PEP_RESOURCE_IO *Io, CR_PEP_RESULT *Result) {
  CR_PEP_TRANSITION Transition = {Kind, Component, State};
  return CrPepApplyStates(Policy, Index, &Transition, 1, Owner, Io, Result);
}
