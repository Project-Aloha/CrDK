/* Absolute per-owner PMIC journal; hardware logic lives in RpmhLib. MIT */
#include <Library/PepRailLib.h>
#include <oskal/common.h>
#include <oskal/cr_mutex.h>
#include <oskal/cr_string.h>

STATIC BOOLEAN TextEqual(CR_PEP_TEXT A,CR_PEP_TEXT B) {
  if (A.Length!=B.Length) return FALSE;
  for (UINTN I=0;I<A.Length;++I) if (A.Data[I]!=B.Data[I]) return FALSE;
  return TRUE;
}
STATIC BOOLEAN TextValid(CR_PEP_TEXT Text) {
  if (Text.Data==NULL || Text.Length==0 || Text.Length>255) return FALSE;
  for (UINTN I=0;I<Text.Length;++I) if (Text.Data[I]==0) return FALSE;
  return TRUE;
}
STATIC BOOLEAN ValueValid(CONST CR_RPMH_RAIL_VALUE *Value) {
  BOOLEAN Mode=Value->Mode==2 || Value->Mode==3 || Value->Mode==4 || Value->Mode==6 || Value->Mode==7;
  return Value->Present<=1 && Value->Enabled<=1 && Value->VoltageMv<=0x1fffU &&
    (Value->Mode==0 || Mode) && (!Value->Enabled || (Value->Present && Value->VoltageMv!=0 && Mode)) &&
    (Value->VoltageMv==0 || Mode) && (Value->Present || (!Value->Enabled && !Value->VoltageMv && !Value->Mode));
}
STATIC CR_STATUS Check(CR_PEP_RAIL_PROVIDER *Provider) {
  return !CR_ERROR(CrMutexCheckLevel()) && Provider!=NULL && Provider->Initialized ? CR_SUCCESS : CR_INVALID_PARAMETER;
}
STATIC CR_STATUS Decode(CONST CR_PEP_RAIL_BINDING *Binding,CONST CR_PEP_RESOURCE_OP *Op,
                         CONST CR_RPMH_RAIL_VALUE **Value) {
  if (Op==NULL || Op->Kind!=CrPepVreg || Op->Name.Data==NULL || Op->SecondaryName.Length!=0 ||
      Op->ArgumentCount!=5 || Op->Arguments[0]!=1 || !TextEqual(Binding->Name,Op->Name)) return CR_UNSUPPORTED;
  if (Op->DriverName.Length==0 && Op->VoteType.Length==0) {
    if (!Binding->AllowImplicitQualifier) return CR_UNSUPPORTED;
  } else if (Op->DriverName.Data==NULL || Op->VoteType.Data==NULL ||
      !TextEqual(Binding->DriverName,Op->DriverName) || !TextEqual(Binding->VoteType,Op->VoteType)) return CR_UNSUPPORTED;
  for (UINT32 I=0;I<Binding->ProfileCount;++I) {
    BOOLEAN Equal=TRUE;
    for (UINT32 A=0;A<5;++A) if (Binding->Profiles[I].Arguments[A]!=Op->Arguments[A]) {Equal=FALSE;break;}
    if (Equal) {*Value=&Binding->Profiles[I].Value;return CR_SUCCESS;}
  }
  return CR_UNSUPPORTED;
}
STATIC CR_STATUS BindingFor(CR_PEP_RAIL_PROVIDER *Provider,UINT64 Resource,CONST CR_PEP_RAIL_BINDING **Binding) {
  CR_STATUS Status=Check(Provider);if (CR_ERROR(Status)) return Status;
  if (Resource==0 || Resource>Provider->Count) return CR_INVALID_PARAMETER;
  *Binding=&Provider->Bindings[(UINT32)Resource-1];return CR_SUCCESS;
}
CR_STATUS CrPepRailProviderDescribe(CR_PEP_RAIL_PROVIDER *Provider,CONST CR_PEP_RESOURCE_OP *Op,
                                    CONST CR_PEP_RAIL_BINDING **Result,CR_RPMH_RAIL_VALUE *Value) {
  if (Result==NULL || Value==NULL) return CR_INVALID_PARAMETER;
  *Result=NULL;cr_memset(Value,0,sizeof(*Value));
  CR_STATUS Status=Check(Provider);if (CR_ERROR(Status)) return Status;
  if (Op==NULL || Op->Name.Data==NULL) return CR_INVALID_PARAMETER;
  for (UINT32 I=0;I<Provider->Count;++I) {
    CONST CR_PEP_RAIL_BINDING *Binding=&Provider->Bindings[I];
    if (!TextEqual(Binding->Name,Op->Name)) continue;
    CONST CR_RPMH_RAIL_VALUE *Desired;
    Status=Decode(Binding,Op,&Desired);if (CR_ERROR(Status)) return Status;
    *Value=*Desired;*Result=Binding;return CR_SUCCESS;
  }
  return CR_NOT_FOUND;
}
STATIC CR_STATUS Resolve(VOID *Context,VOID *Owner,CONST CR_PEP_RESOURCE_OP *Op,UINT64 *Resource) {
  if (Owner==NULL || Resource==NULL) return CR_INVALID_PARAMETER;
  CR_PEP_RAIL_PROVIDER *Provider=Context;CONST CR_PEP_RAIL_BINDING *Binding;
  CR_RPMH_RAIL_VALUE Value;BOOLEAN Pending;
  CR_STATUS Status=CrPepRailProviderDescribe(Provider,Op,&Binding,&Value);if (CR_ERROR(Status)) return Status;
  Status=Provider->Io.Validate(Provider->Io.Context,Binding,&Value);if (CR_ERROR(Status)) return Status;
  Status=Provider->Io.Get(Provider->Io.Context,Owner,Binding,&Value,&Pending);if (CR_ERROR(Status)) return Status;
  if (Pending) return CR_NOT_READY;
  *Resource=(UINT64)(Binding-Provider->Bindings)+1;return CR_SUCCESS;
}
STATIC CR_STATUS Capture(VOID *Context,VOID *Owner,UINT64 Resource,CR_PEP_JOURNAL_VALUE *Output) {
  CR_PEP_RAIL_PROVIDER *Provider=Context;CONST CR_PEP_RAIL_BINDING *Binding;
  CR_STATUS Status=BindingFor(Provider,Resource,&Binding);if (CR_ERROR(Status)) return Status;
  if (Owner==NULL || Output==NULL) return CR_INVALID_PARAMETER;
  CR_RPMH_RAIL_VALUE Value;BOOLEAN Pending;
  Status=Provider->Io.Get(Provider->Io.Context,Owner,Binding,&Value,&Pending);
  if (!CR_ERROR(Status)) {
    cr_memset(Output,0,sizeof(*Output));Output->Words[0]=Value.Present;Output->Words[1]=Value.Enabled;
    Output->Words[2]=Value.VoltageMv;Output->Words[3]=Value.Mode;
  }
  return Status;
}
STATIC CR_STATUS Apply(VOID *Context,VOID *Owner,UINT64 Resource,CONST CR_PEP_RESOURCE_OP *Op) {
  CR_PEP_RAIL_PROVIDER *Provider=Context;CONST CR_PEP_RAIL_BINDING *Binding;
  CR_STATUS Status=BindingFor(Provider,Resource,&Binding);if (CR_ERROR(Status)) return Status;
  if (Owner==NULL) return CR_INVALID_PARAMETER;
  CONST CR_RPMH_RAIL_VALUE *Value;
  Status=Decode(Binding,Op,&Value);if (CR_ERROR(Status)) return Status;
  return Provider->Io.Set(Provider->Io.Context,Owner,Binding,Value);
}
STATIC CR_STATUS Restore(VOID *Context,VOID *Owner,UINT64 Resource,CONST CR_PEP_JOURNAL_VALUE *Input) {
  CR_PEP_RAIL_PROVIDER *Provider=Context;CONST CR_PEP_RAIL_BINDING *Binding;
  CR_STATUS Status=BindingFor(Provider,Resource,&Binding);if (CR_ERROR(Status)) return Status;
  if (Owner==NULL || Input==NULL || Input->Words[0]>1 || Input->Words[1]>1 ||
      Input->Words[2]>0x1fffU || Input->Words[3]>0xffU) return CR_INVALID_PARAMETER;
  for (UINTN I=4;I<ARRAY_SIZE(Input->Words);++I) if (Input->Words[I]!=0) return CR_INVALID_PARAMETER;
  CR_RPMH_RAIL_VALUE Value={.Present=(BOOLEAN)Input->Words[0],.Enabled=(BOOLEAN)Input->Words[1],
    .VoltageMv=(UINT32)Input->Words[2],.Mode=(UINT8)Input->Words[3]};
  if (!ValueValid(&Value)) return CR_INVALID_PARAMETER;
  return Provider->Io.Set(Provider->Io.Context,Owner,Binding,&Value);
}
CR_STATUS CrPepRailProviderInit(CR_PEP_RAIL_PROVIDER *Provider,CONST CR_PEP_RAIL_IO *Io,
                                CONST CR_PEP_RAIL_BINDING *Bindings,UINT32 Count) {
  if (CR_ERROR(CrMutexCheckLevel()) || Provider==NULL || Io==NULL || Io->Validate==NULL || Io->Get==NULL ||
      Io->Set==NULL || Bindings==NULL || Count==0 || Count>4096) return CR_INVALID_PARAMETER;
  if (Provider->Initialized) return CR_ALREADY_STARTED;
  for (UINT32 I=0;I<Count;++I) {
    CONST CR_PEP_RAIL_BINDING *Binding=&Bindings[I];
    if (!TextValid(Binding->Name) || !TextValid(Binding->DriverName) || !TextValid(Binding->VoteType) ||
        Binding->HardwareName==NULL || Binding->HardwareName[0]==0 || Binding->AllowImplicitQualifier>1 ||
        Binding->Profiles==NULL || Binding->ProfileCount==0 || Binding->ProfileCount>256) return CR_INVALID_PARAMETER;
    for (UINT32 P=0;P<I;++P)
      if (TextEqual(Bindings[P].Name,Binding->Name) || cr_strcmp(Bindings[P].HardwareName,Binding->HardwareName)==0)
        return CR_INVALID_PARAMETER;
    for (UINT32 P=0;P<Binding->ProfileCount;++P) {
      CONST CR_PEP_RAIL_PROFILE *Profile=&Binding->Profiles[P];
      if (Profile->Arguments[0]!=1 || !Profile->Value.Present || !ValueValid(&Profile->Value)) return CR_INVALID_PARAMETER;
      for (UINT32 Previous=0;Previous<P;++Previous) {
        BOOLEAN Equal=TRUE;
        for (UINT32 A=0;A<5;++A) if (Profile->Arguments[A]!=Binding->Profiles[Previous].Arguments[A]) {Equal=FALSE;break;}
        if (Equal) return CR_INVALID_PARAMETER;
      }
    }
  }
  Provider->Io=*Io;Provider->Bindings=Bindings;Provider->Count=Count;Provider->Initialized=TRUE;return CR_SUCCESS;
}
CR_STATUS CrPepRailProviderGetJournal(CR_PEP_RAIL_PROVIDER *Provider,CR_PEP_JOURNAL_PROVIDER *Journal) {
  CR_STATUS Status=Check(Provider);if (CR_ERROR(Status)) return Status;
  if (Journal==NULL) return CR_INVALID_PARAMETER;
  *Journal=(CR_PEP_JOURNAL_PROVIDER){Provider,Resolve,Capture,Apply,Restore};return CR_SUCCESS;
}
CR_STATUS CrPepRailProviderUninit(CR_PEP_RAIL_PROVIDER *Provider) {
  CR_STATUS Status=Check(Provider);if (CR_ERROR(Status)) return Status;
  cr_memset(Provider,0,sizeof(*Provider));return CR_SUCCESS;
}
