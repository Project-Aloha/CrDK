/* Absolute BUSARB journal bridge; hardware logic lives in ICBLib. MIT */
#include <Library/PepBandwidthLib.h>
#include <oskal/cr_string.h>

STATIC BOOLEAN TextEqual(CR_PEP_TEXT A, CR_PEP_TEXT B) {
  if (A.Length != B.Length) return FALSE;
  for (UINTN I = 0; I < A.Length; ++I) if (A.Data[I] != B.Data[I]) return FALSE;
  return TRUE;
}
STATIC CR_STATUS Check(CR_PEP_BANDWIDTH_PROVIDER *Provider) {
  return !CR_ERROR(CrMutexCheckLevel()) && Provider != NULL && Provider->Initialized ?
    CR_SUCCESS : CR_INVALID_PARAMETER;
}
STATIC CR_STATUS Decode(CONST CR_PEP_BANDWIDTH_BINDING *Binding,
                         CONST CR_PEP_RESOURCE_OP *Op, CR_ICB_VOTE_VALUE *Value) {
  if (Op == NULL || Op->Kind != CrPepBusArb || Op->Name.Data == NULL ||
      Op->SecondaryName.Data == NULL || Op->DriverName.Length != 0 || Op->VoteType.Length != 0 ||
      Op->ArgumentCount != 3 || Op->Arguments[0] != 3 ||
      !TextEqual(Binding->Master, Op->Name) || !TextEqual(Binding->Slave, Op->SecondaryName))
    return CR_UNSUPPORTED;
  if (Op->Arguments[2] < Op->Arguments[1]) return CR_INVALID_PARAMETER;
  UINT64 Scale = Binding->PolicyUnitsPerKbyte;
  *Value = (CR_ICB_VOTE_VALUE){.Present = TRUE,
    .Average = Op->Arguments[1] / Scale + (Op->Arguments[1] % Scale != 0),
    .Peak = Op->Arguments[2] / Scale + (Op->Arguments[2] % Scale != 0)};
  return CR_SUCCESS;
}
STATIC CR_STATUS BindingFor(CR_PEP_BANDWIDTH_PROVIDER *Provider, UINT64 Resource,
                             CONST CR_PEP_BANDWIDTH_BINDING **Binding) {
  CR_STATUS Status = Check(Provider);if (CR_ERROR(Status)) return Status;
  if (Resource == 0 || Resource > Provider->Count) return CR_INVALID_PARAMETER;
  *Binding = &Provider->Bindings[(UINT32)Resource - 1];return CR_SUCCESS;
}
CR_STATUS CrPepBandwidthProviderDescribe(CR_PEP_BANDWIDTH_PROVIDER *Provider, CONST CR_PEP_RESOURCE_OP *Op,
                                         CONST CR_PEP_BANDWIDTH_BINDING **Result, CR_ICB_VOTE_VALUE *Value) {
  if (Result == NULL || Value == NULL) return CR_INVALID_PARAMETER;
  *Result = NULL;cr_memset(Value, 0, sizeof(*Value));
  CR_STATUS Status = Check(Provider);if (CR_ERROR(Status)) return Status;
  if (Op == NULL || Op->Name.Data == NULL ||
      Op->SecondaryName.Data == NULL) return CR_INVALID_PARAMETER;
  for (UINT32 I = 0; I < Provider->Count; ++I) {
    CONST CR_PEP_BANDWIDTH_BINDING *Binding = &Provider->Bindings[I];
    if (!TextEqual(Binding->Master, Op->Name) || !TextEqual(Binding->Slave, Op->SecondaryName)) continue;
    Status = Decode(Binding, Op, Value);if (CR_ERROR(Status)) return Status;
    *Result = Binding;return CR_SUCCESS;
  }
  return CR_NOT_FOUND;
}
STATIC CR_STATUS Resolve(VOID *Context, VOID *Owner, CONST CR_PEP_RESOURCE_OP *Op, UINT64 *Resource) {
  if (Owner == NULL || Resource == NULL) return CR_INVALID_PARAMETER;
  CR_PEP_BANDWIDTH_PROVIDER *Provider = Context;CONST CR_PEP_BANDWIDTH_BINDING *Binding;
  CR_ICB_VOTE_VALUE Value;BOOLEAN Pending;
  CR_STATUS Status = CrPepBandwidthProviderDescribe(Provider, Op, &Binding, &Value);
  if (CR_ERROR(Status)) return Status;
  Status = Provider->Io.Validate(Provider->Io.Context, Owner, &Binding->Path);
  if (CR_ERROR(Status)) return Status;
  Status = Provider->Io.Get(Provider->Io.Context, Owner, &Binding->Path, &Value, &Pending);
  if (CR_ERROR(Status)) return Status;
  if (Pending) return CR_NOT_READY;
  *Resource = (UINT64)(Binding - Provider->Bindings) + 1;return CR_SUCCESS;
}
STATIC CR_STATUS Capture(VOID *Context, VOID *Owner, UINT64 Resource, CR_PEP_JOURNAL_VALUE *Output) {
  CR_PEP_BANDWIDTH_PROVIDER *Provider = Context;CONST CR_PEP_BANDWIDTH_BINDING *Binding;
  CR_STATUS Status = BindingFor(Provider, Resource, &Binding);if (CR_ERROR(Status)) return Status;
  if (Owner == NULL || Output == NULL) return CR_INVALID_PARAMETER;
  CR_ICB_VOTE_VALUE Value;BOOLEAN Pending;
  Status = Provider->Io.Get(Provider->Io.Context, Owner, &Binding->Path, &Value, &Pending);
  if (!CR_ERROR(Status)) {
    cr_memset(Output, 0, sizeof(*Output));
    Output->Words[0] = Value.Present;Output->Words[1] = Value.Average;Output->Words[2] = Value.Peak;
  }
  return Status;
}
STATIC CR_STATUS Apply(VOID *Context, VOID *Owner, UINT64 Resource, CONST CR_PEP_RESOURCE_OP *Op) {
  CR_PEP_BANDWIDTH_PROVIDER *Provider = Context;CONST CR_PEP_BANDWIDTH_BINDING *Binding;
  CR_STATUS Status = BindingFor(Provider, Resource, &Binding);if (CR_ERROR(Status)) return Status;
  if (Owner == NULL) return CR_INVALID_PARAMETER;
  CR_ICB_VOTE_VALUE Value;
  Status = Decode(Binding, Op, &Value);if (CR_ERROR(Status)) return Status;
  return Provider->Io.Set(Provider->Io.Context, Owner, &Binding->Path, &Value);
}
STATIC CR_STATUS Restore(VOID *Context, VOID *Owner, UINT64 Resource, CONST CR_PEP_JOURNAL_VALUE *Input) {
  CR_PEP_BANDWIDTH_PROVIDER *Provider = Context;CONST CR_PEP_BANDWIDTH_BINDING *Binding;
  CR_STATUS Status = BindingFor(Provider, Resource, &Binding);if (CR_ERROR(Status)) return Status;
  if (Owner == NULL || Input == NULL || Input->Words[0] > 1 || Input->Words[2] < Input->Words[1] ||
      (!Input->Words[0] && (Input->Words[1] || Input->Words[2]))) return CR_INVALID_PARAMETER;
  for (UINTN I = 3; I < ARRAY_SIZE(Input->Words); ++I)
    if (Input->Words[I] != 0) return CR_INVALID_PARAMETER;
  CR_ICB_VOTE_VALUE Value = {.Present = (BOOLEAN)Input->Words[0],.Average = Input->Words[1],.Peak = Input->Words[2]};
  return Provider->Io.Set(Provider->Io.Context, Owner, &Binding->Path, &Value);
}
CR_STATUS CrPepBandwidthProviderInit(CR_PEP_BANDWIDTH_PROVIDER *Provider, CONST CR_PEP_BANDWIDTH_IO *Io,
                                     CONST CR_PEP_BANDWIDTH_BINDING *Bindings, UINT32 Count) {
  if (CR_ERROR(CrMutexCheckLevel()) || Provider == NULL || Io == NULL || Io->Validate == NULL ||
      Io->Get == NULL || Io->Set == NULL || Bindings == NULL || Count == 0 || Count > 4096U)
    return CR_INVALID_PARAMETER;
  if (Provider->Initialized) return CR_ALREADY_STARTED;
  for (UINT32 I = 0; I < Count; ++I) {
    CONST CR_PEP_BANDWIDTH_BINDING *Binding = &Bindings[I];
    if (Binding->Master.Data == NULL || Binding->Master.Length == 0 || Binding->Master.Length > 255 ||
        Binding->Slave.Data == NULL || Binding->Slave.Length == 0 || Binding->Slave.Length > 255 ||
        Binding->Path.Provider == NULL || Binding->Path.Provider[0] == 0 ||
        (Binding->PolicyUnitsPerKbyte != 1 && Binding->PolicyUnitsPerKbyte != 1000)) return CR_INVALID_PARAMETER;
    for (UINT32 P = 0; P < I; ++P) {
      CONST CR_PEP_BANDWIDTH_BINDING *Other = &Bindings[P];
      if ((TextEqual(Other->Master, Binding->Master) && TextEqual(Other->Slave, Binding->Slave)) ||
          (Other->Path.SourceId == Binding->Path.SourceId && Other->Path.DestinationId == Binding->Path.DestinationId &&
           cr_strcmp(Other->Path.Provider, Binding->Path.Provider) == 0)) return CR_INVALID_PARAMETER;
    }
  }
  Provider->Io = *Io;Provider->Bindings = Bindings;Provider->Count = Count;Provider->Initialized = TRUE;
  return CR_SUCCESS;
}
CR_STATUS CrPepBandwidthProviderGetJournal(CR_PEP_BANDWIDTH_PROVIDER *Provider, CR_PEP_JOURNAL_PROVIDER *Journal) {
  CR_STATUS Status = Check(Provider);if (CR_ERROR(Status)) return Status;
  if (Journal == NULL) return CR_INVALID_PARAMETER;
  *Journal = (CR_PEP_JOURNAL_PROVIDER){Provider,Resolve,Capture,Apply,Restore};return CR_SUCCESS;
}
CR_STATUS CrPepBandwidthProviderUninit(CR_PEP_BANDWIDTH_PROVIDER *Provider) {
  CR_STATUS Status = Check(Provider);if (CR_ERROR(Status)) return Status;
  cr_memset(Provider, 0, sizeof(*Provider));return CR_SUCCESS;
}
