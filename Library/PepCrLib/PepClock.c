/* Absolute owner clock/GDSC journal bridge. SPDX-License-Identifier: MIT */
#include <Library/PepClockLib.h>
#include <oskal/cr_mutex.h>
#include <oskal/cr_string.h>

STATIC BOOLEAN TextEqual(CR_PEP_TEXT A, CR_PEP_TEXT B) {
  if (A.Length != B.Length) return FALSE;
  for (UINTN Index = 0; Index < A.Length; ++Index)
    if (A.Data[Index] != B.Data[Index]) return FALSE;
  return TRUE;
}
STATIC CR_STATUS Check(CR_PEP_CLOCK_PROVIDER *Provider) {
  if (CR_ERROR(CrMutexCheckLevel())) return CR_INVALID_PARAMETER;
  return Provider != NULL && Provider->Initialized ? CR_SUCCESS : CR_INVALID_PARAMETER;
}
STATIC CR_STATUS Decode(CONST CR_PEP_CLOCK_BINDING *Binding,
                         CONST CR_PEP_RESOURCE_OP *Op, UINT64 *Rate) {
  if (Op == NULL || Op->Name.Data == NULL || Op->Name.Length == 0 ||
      Op->SecondaryName.Length != 0 || Op->DriverName.Length != 0 || Op->VoteType.Length != 0 ||
      !TextEqual(Binding->Name, Op->Name) ||
      Op->ArgumentCount == 0) return CR_UNSUPPORTED;
  *Rate = Binding->DefaultRateHz;
  if (Binding->Gdsc) {
    if (Op->Kind != CrPepFootswitch ||
        (Op->Arguments[0] != 1 && Op->Arguments[0] != 2) ||
        !((Op->ArgumentCount == 2 && Op->Arguments[1] == 1) ||
          (Op->ArgumentCount == 1 && Binding->AllowImplicitGdscMode)))
      return CR_UNSUPPORTED;
    return CR_SUCCESS;
  }
  if (Op->Kind != CrPepClock) return CR_UNSUPPORTED;
  if (Op->Arguments[0] == 1 || Op->Arguments[0] == 2)
    return Op->ArgumentCount == 1 ? CR_SUCCESS : CR_UNSUPPORTED;
  if (Op->Arguments[0] != 3 || Op->ArgumentCount != 3) return CR_UNSUPPORTED;
  for (UINTN Index = 0; Index < Binding->RateCount; ++Index) {
    CONST CR_PEP_CLOCK_RATE *Contract = &Binding->Rates[Index];
    if (Op->Arguments[1] == Contract->PolicyValue && Op->Arguments[2] == Contract->Selector) {
      *Rate = Contract->RateHz; return CR_SUCCESS;
    }
  }
  return CR_UNSUPPORTED;
}
STATIC CR_STATUS BindingFor(CR_PEP_CLOCK_PROVIDER *Provider, UINT64 Resource,
                             CONST CR_PEP_CLOCK_BINDING **Binding) {
  CR_STATUS Status = Check(Provider);
  if (CR_ERROR(Status)) return Status;
  if (Resource == 0 || Resource > Provider->Count) return CR_INVALID_PARAMETER;
  *Binding = &Provider->Bindings[(UINT32)Resource - 1];return CR_SUCCESS;
}
CR_STATUS CrPepClockProviderDescribe(CR_PEP_CLOCK_PROVIDER *Provider, CONST CR_PEP_RESOURCE_OP *Op,
                                      CONST CR_PEP_CLOCK_BINDING **Result, UINT64 *Rate) {
  if (Result == NULL || Rate == NULL) return CR_INVALID_PARAMETER;
  *Result = NULL;*Rate = 0;
  CR_STATUS Status = Check(Provider);
  if (CR_ERROR(Status)) return Status;
  if (Op == NULL || Op->Name.Data == NULL || Op->Name.Length == 0) return CR_INVALID_PARAMETER;
  for (UINTN Index = 0; Index < Provider->Count; ++Index) {
    CONST CR_PEP_CLOCK_BINDING *Binding = &Provider->Bindings[Index];
    if (!TextEqual(Binding->Name, Op->Name) ||
        ((Op->Kind == CrPepFootswitch) != (Binding->Gdsc != FALSE))) continue;
    Status = Decode(Binding, Op, Rate);
    if (CR_ERROR(Status)) return Status;
    *Result = Binding;return CR_SUCCESS;
  }
  return CR_NOT_FOUND;
}
STATIC CR_STATUS Resolve(VOID *Context, VOID *Owner, CONST CR_PEP_RESOURCE_OP *Op,
                         UINT64 *Resource) {
  if (Owner == NULL || Resource == NULL) return CR_INVALID_PARAMETER;
  CR_PEP_CLOCK_PROVIDER *Provider = Context;CONST CR_PEP_CLOCK_BINDING *Binding;UINT64 Rate;
  CR_STATUS Status = CrPepClockProviderDescribe(Provider, Op, &Binding, &Rate);
  if (CR_ERROR(Status)) return Status;
  Status = Provider->Io.Validate(Provider->Io.Context, Binding, Rate);
  if (CR_ERROR(Status)) return Status;
  CR_CLOCK_VOTE_VALUE Value;BOOLEAN Pending;
  Status = Provider->Io.Get(Provider->Io.Context, Owner, Binding, &Value, &Pending);
  if (CR_ERROR(Status)) return Status;
  if (Pending) return CR_NOT_READY;
  *Resource = (UINT64)(Binding - Provider->Bindings) + 1;return CR_SUCCESS;
}
STATIC VOID Encode(CR_PEP_JOURNAL_VALUE *Output, CONST CR_CLOCK_VOTE_VALUE *Value) {
  cr_memset(Output, 0, sizeof(*Output));
  Output->Words[0] = Value->Present;Output->Words[1] = Value->Enabled;Output->Words[2] = Value->RateHz;
}
STATIC CR_STATUS Capture(VOID *Context, VOID *Owner, UINT64 Resource, CR_PEP_JOURNAL_VALUE *Output) {
  CR_PEP_CLOCK_PROVIDER *Provider = Context;CONST CR_PEP_CLOCK_BINDING *Binding;
  CR_STATUS Status = BindingFor(Provider, Resource, &Binding);
  if (CR_ERROR(Status)) return Status;
  if (Owner == NULL || Output == NULL) return CR_INVALID_PARAMETER;
  CR_CLOCK_VOTE_VALUE Value;BOOLEAN Pending;
  Status = Provider->Io.Get(Provider->Io.Context, Owner, Binding, &Value, &Pending);
  if (!CR_ERROR(Status)) Encode(Output, &Value);
  return Status;
}
STATIC CR_STATUS Apply(VOID *Context, VOID *Owner, UINT64 Resource, CONST CR_PEP_RESOURCE_OP *Op) {
  CR_PEP_CLOCK_PROVIDER *Provider = Context;CONST CR_PEP_CLOCK_BINDING *Binding;
  CR_STATUS Status = BindingFor(Provider, Resource, &Binding);
  if (CR_ERROR(Status)) return Status;
  if (Owner == NULL || Op == NULL) return CR_INVALID_PARAMETER;
  UINT64 Rate;Status = Decode(Binding, Op, &Rate);if (CR_ERROR(Status)) return Status;
  CR_CLOCK_VOTE_VALUE Value;BOOLEAN Pending;
  Status = Provider->Io.Get(Provider->Io.Context, Owner, Binding, &Value, &Pending);
  if (CR_ERROR(Status)) return Status;
  if (Pending) return CR_NOT_READY;
  if (!Value.Present) Value.RateHz = Binding->DefaultRateHz;
  Value.Present = TRUE;
  if (Op->Arguments[0] == 3) Value.RateHz = Rate;
  else Value.Enabled = Op->Arguments[0] == 1;
  return Provider->Io.Set(Provider->Io.Context, Owner, Binding, &Value);
}
STATIC CR_STATUS Restore(VOID *Context, VOID *Owner, UINT64 Resource, CONST CR_PEP_JOURNAL_VALUE *Input) {
  CR_PEP_CLOCK_PROVIDER *Provider = Context;CONST CR_PEP_CLOCK_BINDING *Binding;
  CR_STATUS Status = BindingFor(Provider, Resource, &Binding);
  if (CR_ERROR(Status)) return Status;
  if (Owner == NULL || Input == NULL || Input->Words[0] > 1 || Input->Words[1] > 1 ||
      (!Input->Words[0] && (Input->Words[1] || Input->Words[2]))) return CR_INVALID_PARAMETER;
  for (UINTN Index = 3; Index < ARRAY_SIZE(Input->Words); ++Index)
    if (Input->Words[Index] != 0) return CR_INVALID_PARAMETER;
  CR_CLOCK_VOTE_VALUE Value = {.Present = (BOOLEAN)Input->Words[0],
    .Enabled = (BOOLEAN)Input->Words[1],.RateHz = Input->Words[2]};
  return Provider->Io.Set(Provider->Io.Context, Owner, Binding, &Value);
}
CR_STATUS CrPepClockProviderInit(CR_PEP_CLOCK_PROVIDER *Provider, CONST CR_PEP_CLOCK_IO *Io,
                                 CONST CR_PEP_CLOCK_BINDING *Bindings, UINT32 Count) {
  if (CR_ERROR(CrMutexCheckLevel()) || Provider == NULL || Io == NULL ||
      Io->Validate == NULL || Io->Get == NULL || Io->Set == NULL ||
      Bindings == NULL || Count == 0 || Count > 4096U) return CR_INVALID_PARAMETER;
  if (Provider->Initialized) return CR_ALREADY_STARTED;
  for (UINTN Index = 0; Index < Count; ++Index) {
    CONST CR_PEP_CLOCK_BINDING *Binding = &Bindings[Index];
    if (Binding->Name.Data == NULL || Binding->Name.Length == 0 || Binding->Name.Length > 255 ||
        Binding->HardwareName == NULL || Binding->HardwareName[0] == 0 || Binding->Gdsc > 1 ||
        Binding->AllowImplicitGdscMode > 1 || (!Binding->Gdsc && Binding->AllowImplicitGdscMode) ||
        Binding->Controller >= CC_TYPE_MAX || Binding->RateCount > 256 ||
        (Binding->RateCount != 0 && Binding->Rates == NULL) ||
        (Binding->Gdsc && (Binding->DefaultRateHz != 0 || Binding->RateCount != 0)))
      return CR_INVALID_PARAMETER;
    for (UINTN R = 0; R < Binding->RateCount; ++R) {
      if (Binding->Rates[R].RateHz == 0) return CR_INVALID_PARAMETER;
      for (UINTN Previous = 0; Previous < R; ++Previous)
        if (Binding->Rates[Previous].PolicyValue == Binding->Rates[R].PolicyValue &&
            Binding->Rates[Previous].Selector == Binding->Rates[R].Selector)
          return CR_INVALID_PARAMETER;
    }
    for (UINTN Previous = 0; Previous < Index; ++Previous) {
      CONST CR_PEP_CLOCK_BINDING *Other = &Bindings[Previous];
      if (Other->Gdsc == Binding->Gdsc &&
          (TextEqual(Other->Name, Binding->Name) ||
           (Other->Controller == Binding->Controller && cr_strcmp(Other->HardwareName, Binding->HardwareName) == 0)))
        return CR_INVALID_PARAMETER;
    }
  }
  Provider->Io = *Io;Provider->Bindings = Bindings;Provider->Count = Count;Provider->Initialized = TRUE;
  return CR_SUCCESS;
}
CR_STATUS CrPepClockProviderGetJournal(CR_PEP_CLOCK_PROVIDER *Provider, CR_PEP_JOURNAL_PROVIDER *Journal) {
  CR_STATUS Status = Check(Provider);if (CR_ERROR(Status)) return Status;
  if (Journal == NULL) return CR_INVALID_PARAMETER;
  *Journal = (CR_PEP_JOURNAL_PROVIDER){Provider,Resolve,Capture,Apply,Restore};return CR_SUCCESS;
}
CR_STATUS CrPepClockProviderUninit(CR_PEP_CLOCK_PROVIDER *Provider) {
  CR_STATUS Status = Check(Provider);if (CR_ERROR(Status)) return Status;
  cr_memset(Provider, 0, sizeof(*Provider));return CR_SUCCESS;
}
