/* Qualified ARC NPA journal bridge; metadata owns virtual-level conversion. MIT */
#include <Library/PepNpaLib.h>
#include <oskal/common.h>
#include <oskal/cr_mutex.h>
#include <oskal/cr_string.h>

STATIC BOOLEAN TextEqual(CR_PEP_TEXT A, CR_PEP_TEXT B) {
  if (A.Length != B.Length) return FALSE;
  for (UINTN I=0; I<A.Length; ++I) if (A.Data[I]!=B.Data[I]) return FALSE;
  return TRUE;
}
STATIC BOOLEAN TextValid(CR_PEP_TEXT Text) {
  if (Text.Data==NULL || Text.Length==0 || Text.Length>255) return FALSE;
  for (UINTN I=0; I<Text.Length; ++I) if (Text.Data[I]==0) return FALSE;
  return TRUE;
}
STATIC BOOLEAN ValueValid(CONST CR_PEP_NPA_VALUE *Value) {
  return Value->Present<=1 && (Value->Present || Value->State==0);
}
STATIC CR_STATUS Check(CR_PEP_NPA_PROVIDER *Provider) {
  return !CR_ERROR(CrMutexCheckLevel()) && Provider!=NULL && Provider->Initialized ?
    CR_SUCCESS : CR_INVALID_PARAMETER;
}
STATIC CR_STATUS Decode(CONST CR_PEP_NPA_BINDING *Binding, CONST CR_PEP_RESOURCE_OP *Op,
                         CONST CR_PEP_NPA_PROFILE **Result) {
  if (Op==NULL || Op->Kind!=CrPepNpa || !TextValid(Op->Name) ||
      Op->SecondaryName.Length!=0 || Op->DriverName.Length!=0 || Op->VoteType.Length!=0 ||
      Op->ArgumentCount!=2 || Op->Arguments[0]!=1 || !TextEqual(Binding->Name,Op->Name))
    return CR_UNSUPPORTED;
  for (UINT32 I=0; I<Binding->ProfileCount; ++I) {
    CONST CR_PEP_NPA_PROFILE *Profile=&Binding->Profiles[I];
    if (Profile->Arguments[0]==Op->Arguments[0] && Profile->Arguments[1]==Op->Arguments[1]) {
      *Result=Profile; return CR_SUCCESS;
    }
  }
  return CR_UNSUPPORTED;
}
CR_STATUS CrPepNpaProviderDescribe(CR_PEP_NPA_PROVIDER *Provider, CONST CR_PEP_RESOURCE_OP *Op,
    CONST CR_PEP_NPA_BINDING **Binding, CONST CR_PEP_NPA_PROFILE **Profile) {
  if (Binding==NULL || Profile==NULL) return CR_INVALID_PARAMETER;
  *Binding=NULL; *Profile=NULL;
  CR_STATUS Status=Check(Provider); if (CR_ERROR(Status)) return Status;
  if (Op==NULL || !TextValid(Op->Name)) return CR_INVALID_PARAMETER;
  for (UINT32 I=0; I<Provider->Count; ++I) {
    CONST CR_PEP_NPA_BINDING *Candidate=&Provider->Bindings[I];
    if (!TextEqual(Candidate->Name,Op->Name)) continue;
    Status=Decode(Candidate,Op,Profile);
    if (!CR_ERROR(Status)) *Binding=Candidate;
    return Status;
  }
  return CR_NOT_FOUND;
}
STATIC CR_STATUS BindingFor(CR_PEP_NPA_PROVIDER *Provider, UINT64 Resource,
                             CONST CR_PEP_NPA_BINDING **Binding) {
  CR_STATUS Status=Check(Provider); if (CR_ERROR(Status)) return Status;
  if (Resource==0 || Resource>Provider->Count) return CR_INVALID_PARAMETER;
  *Binding=&Provider->Bindings[(UINT32)Resource-1]; return CR_SUCCESS;
}
STATIC CR_STATUS ResolveValue(CR_PEP_NPA_PROVIDER *Provider, CONST CR_PEP_NPA_BINDING *Binding,
                               CONST CR_PEP_NPA_PROFILE *Profile, CR_PEP_NPA_VALUE *Value) {
  cr_memset(Value,0,sizeof(*Value));
  CR_STATUS Status=Provider->Io.ResolveLevel(Provider->Io.Context,Binding,Profile->Arguments[1],Value);
  if (!CR_ERROR(Status) && (!ValueValid(Value) || !Value->Present)) return CR_COMPROMISED_DATA;
  return Status;
}
STATIC CR_STATUS Resolve(VOID *Context, VOID *Owner, CONST CR_PEP_RESOURCE_OP *Op, UINT64 *Resource) {
  if (Owner==NULL || Resource==NULL) return CR_INVALID_PARAMETER;
  CR_PEP_NPA_PROVIDER *Provider=Context;
  CONST CR_PEP_NPA_BINDING *Binding; CONST CR_PEP_NPA_PROFILE *Profile;
  CR_STATUS Status=CrPepNpaProviderDescribe(Provider,Op,&Binding,&Profile);
  if (CR_ERROR(Status)) return Status;
  CR_PEP_NPA_VALUE Value; BOOLEAN Pending=FALSE;
  Status=ResolveValue(Provider,Binding,Profile,&Value); if (CR_ERROR(Status)) return Status;
  Status=Provider->Io.Get(Provider->Io.Context,Owner,Binding,&Value,&Pending);
  if (CR_ERROR(Status)) return Status;
  if (!ValueValid(&Value)) return CR_COMPROMISED_DATA;
  if (Pending) return CR_NOT_READY;
  *Resource=(UINT64)(Binding-Provider->Bindings)+1; return CR_SUCCESS;
}
STATIC CR_STATUS Capture(VOID *Context, VOID *Owner, UINT64 Resource, CR_PEP_JOURNAL_VALUE *Output) {
  CR_PEP_NPA_PROVIDER *Provider=Context; CONST CR_PEP_NPA_BINDING *Binding;
  CR_STATUS Status=BindingFor(Provider,Resource,&Binding); if (CR_ERROR(Status)) return Status;
  if (Owner==NULL || Output==NULL) return CR_INVALID_PARAMETER;
  CR_PEP_NPA_VALUE Value; BOOLEAN Pending=FALSE;
  Status=Provider->Io.Get(Provider->Io.Context,Owner,Binding,&Value,&Pending);
  if (CR_ERROR(Status)) return Status;
  if (!ValueValid(&Value)) return CR_COMPROMISED_DATA;
  if (Pending) return CR_NOT_READY;
  cr_memset(Output,0,sizeof(*Output)); Output->Words[0]=Value.Present; Output->Words[1]=Value.State;
  return CR_SUCCESS;
}
STATIC CR_STATUS Apply(VOID *Context, VOID *Owner, UINT64 Resource, CONST CR_PEP_RESOURCE_OP *Op) {
  CR_PEP_NPA_PROVIDER *Provider=Context; CONST CR_PEP_NPA_BINDING *Binding;
  CR_STATUS Status=BindingFor(Provider,Resource,&Binding); if (CR_ERROR(Status)) return Status;
  if (Owner==NULL) return CR_INVALID_PARAMETER;
  CONST CR_PEP_NPA_PROFILE *Profile;
  Status=Decode(Binding,Op,&Profile); if (CR_ERROR(Status)) return Status;
  CR_PEP_NPA_VALUE Value;
  Status=ResolveValue(Provider,Binding,Profile,&Value); if (CR_ERROR(Status)) return Status;
  return Provider->Io.Set(Provider->Io.Context,Owner,Binding,&Value);
}
STATIC CR_STATUS Restore(VOID *Context, VOID *Owner, UINT64 Resource, CONST CR_PEP_JOURNAL_VALUE *Input) {
  CR_PEP_NPA_PROVIDER *Provider=Context; CONST CR_PEP_NPA_BINDING *Binding;
  CR_STATUS Status=BindingFor(Provider,Resource,&Binding); if (CR_ERROR(Status)) return Status;
  if (Owner==NULL || Input==NULL || Input->Words[0]>1) return CR_INVALID_PARAMETER;
  for (UINTN I=2; I<ARRAY_SIZE(Input->Words); ++I) if (Input->Words[I]!=0) return CR_INVALID_PARAMETER;
  CR_PEP_NPA_VALUE Value={.Present=(BOOLEAN)Input->Words[0],.State=Input->Words[1]};
  if (!ValueValid(&Value)) return CR_INVALID_PARAMETER;
  return Provider->Io.Set(Provider->Io.Context,Owner,Binding,&Value);
}
CR_STATUS CrPepNpaProviderInit(CR_PEP_NPA_PROVIDER *Provider, CONST CR_PEP_NPA_IO *Io,
    CONST CR_PEP_NPA_BINDING *Bindings, UINT32 Count) {
  if (CR_ERROR(CrMutexCheckLevel()) || Provider==NULL || Io==NULL || Io->ResolveLevel==NULL ||
      Io->Get==NULL || Io->Set==NULL || Bindings==NULL || Count==0 || Count>4096) return CR_INVALID_PARAMETER;
  if (Provider->Initialized) return CR_ALREADY_STARTED;
  for (UINT32 I=0; I<Count; ++I) {
    CONST CR_PEP_NPA_BINDING *Binding=&Bindings[I];
    if (!TextValid(Binding->Name) || Binding->HardwareName==NULL || Binding->HardwareName[0]==0 ||
        (Binding->Kind!=CrPepNpaArcPowerLevel && Binding->Kind!=CrPepNpaArcXoLevel) ||
        Binding->Profiles==NULL || Binding->ProfileCount==0 || Binding->ProfileCount>256)
      return CR_INVALID_PARAMETER;
    for (UINT32 P=0; P<I; ++P)
      if (TextEqual(Bindings[P].Name,Binding->Name) || cr_strcmp(Bindings[P].HardwareName,Binding->HardwareName)==0)
        return CR_INVALID_PARAMETER;
    for (UINT32 P=0; P<Binding->ProfileCount; ++P) {
      if (Binding->Profiles[P].Arguments[0]!=1) return CR_INVALID_PARAMETER;
      for (UINT32 Previous=0; Previous<P; ++Previous)
        if (Binding->Profiles[P].Arguments[1]==Binding->Profiles[Previous].Arguments[1])
          return CR_INVALID_PARAMETER;
    }
  }
  Provider->Io=*Io; Provider->Bindings=Bindings; Provider->Count=Count; Provider->Initialized=TRUE;
  return CR_SUCCESS;
}
CR_STATUS CrPepNpaProviderGetJournal(CR_PEP_NPA_PROVIDER *Provider, CR_PEP_JOURNAL_PROVIDER *Journal) {
  CR_STATUS Status=Check(Provider); if (CR_ERROR(Status)) return Status;
  if (Journal==NULL) return CR_INVALID_PARAMETER;
  *Journal=(CR_PEP_JOURNAL_PROVIDER){Provider,Resolve,Capture,Apply,Restore}; return CR_SUCCESS;
}
CR_STATUS CrPepNpaProviderUninit(CR_PEP_NPA_PROVIDER *Provider) {
  CR_STATUS Status=Check(Provider); if (CR_ERROR(Status)) return Status;
  cr_memset(Provider,0,sizeof(*Provider)); return CR_SUCCESS;
}
STATIC CONST CR_PEP_NPA_PROFILE CxProfiles[]={{{1,0}},{{1,256}}};
STATIC CONST CR_PEP_NPA_PROFILE XoProfiles[]={{{1,0}},{{1,128}}};
STATIC CONST CR_PEP_NPA_BINDING KodiakBindings[]={
  {{"/arc/client/rail_cx",19},"cx.lvl",CrPepNpaArcPowerLevel,CxProfiles,ARRAY_SIZE(CxProfiles)},
  {{"/arc/client/rail_xo",19},"xo.lvl",CrPepNpaArcXoLevel,XoProfiles,ARRAY_SIZE(XoProfiles)}
};
VOID CrPepNpaGetKodiakBindings(CONST CR_PEP_NPA_BINDING **Bindings, UINT32 *Count) {
  if (Bindings!=NULL) *Bindings=KodiakBindings;
  if (Count!=NULL) *Count=ARRAY_SIZE(KodiakBindings);
}
