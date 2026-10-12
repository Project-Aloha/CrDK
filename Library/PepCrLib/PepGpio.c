/* Per-owner GPIO journal over an explicit original-AML tuple profile. MIT */
#include <Library/PepGpioLib.h>
#include <oskal/cr_mutex.h>
#include <oskal/cr_string.h>
STATIC CR_STATUS Check(CR_PEP_GPIO_PROVIDER *P) {return !CR_ERROR(CrMutexCheckLevel())&&P!=NULL&&P->Initialized ? CR_SUCCESS : CR_INVALID_PARAMETER;}
STATIC BOOLEAN TextEqual(CR_PEP_TEXT A,CR_PEP_TEXT B) {return A.Length==B.Length&&(A.Length==0||(A.Data!=NULL&&B.Data!=NULL&&cr_memcmp(A.Data,B.Data,A.Length)==0));}
STATIC BOOLEAN Match(CONST CR_PEP_GPIO_PROFILE *P,CONST CR_PEP_RESOURCE_OP *O) {
  if(P->Kind!=O->Kind||P->ArgumentCount!=O->ArgumentCount||!TextEqual(P->Command,O->Name)||O->SecondaryName.Length||O->DriverName.Length||O->VoteType.Length)return FALSE;
  for(UINT32 I=0;I<P->ArgumentCount;++I)if(P->Arguments[I]!=O->Arguments[I])return FALSE;
  return TRUE;
}
CR_STATUS CrPepGpioProviderDescribe(CR_PEP_GPIO_PROVIDER *P,CONST CR_PEP_RESOURCE_OP *O,CONST CR_PEP_GPIO_BINDING **Binding,CR_GPIO_OWNER_CONFIG *C) {
  CR_STATUS S=Check(P);if(CR_ERROR(S)||O==NULL||Binding==NULL||C==NULL)return CR_ERROR(S)?S:CR_INVALID_PARAMETER;
  *Binding=NULL;
  for(UINT32 I=0;I<P->Count;++I)for(UINT32 J=0;J<P->Bindings[I].ProfileCount;++J)if(Match(&P->Bindings[I].Profiles[J],O)){*Binding=&P->Bindings[I];*C=P->Bindings[I].Profiles[J].Config;return CR_SUCCESS;}
  return CR_UNSUPPORTED;
}
STATIC CR_STATUS BindingFor(CR_PEP_GPIO_PROVIDER *P,UINT64 R,CONST CR_PEP_GPIO_BINDING **B) {CR_STATUS S=Check(P);if(CR_ERROR(S))return S;if(R==0||R>P->Count)return CR_INVALID_PARAMETER;*B=&P->Bindings[(UINT32)R-1];return CR_SUCCESS;}
STATIC CR_STATUS Resolve(VOID *Context,VOID *Owner,CONST CR_PEP_RESOURCE_OP *O,UINT64 *R) {
  CR_PEP_GPIO_PROVIDER *P=Context;CONST CR_PEP_GPIO_BINDING *B;CR_GPIO_OWNER_CONFIG C;
  if(Owner==NULL||R==NULL)return CR_INVALID_PARAMETER;CR_STATUS S=CrPepGpioProviderDescribe(P,O,&B,&C);if(CR_ERROR(S))return S;
  S=P->Io.Validate(P->Io.Context,&B->Key,&C);if(CR_ERROR(S))return S;CR_GPIO_OWNER_VALUE V;BOOLEAN Pending;
  S=P->Io.Get(P->Io.Context,Owner,&B->Key,&V,&Pending);if(CR_ERROR(S))return S;if(Pending)return CR_NOT_READY;
  *R=(UINT64)(B-P->Bindings)+1;return CR_SUCCESS;
}
STATIC VOID Encode(CR_PEP_JOURNAL_VALUE *Out,CONST CR_GPIO_OWNER_VALUE *V) {
  cr_memset(Out,0,sizeof(*Out));Out->Words[0]=V->Present;
  CONST UINT8 *Bytes=(CONST UINT8 *)&V->Config;
  for(UINTN I=0;I<sizeof(V->Config);++I)Out->Words[1+I/8]|=(UINT64)Bytes[I]<<((I%8)*8);
}
STATIC CR_STATUS Capture(VOID *Context,VOID *Owner,UINT64 R,CR_PEP_JOURNAL_VALUE *Out) {
  CR_PEP_GPIO_PROVIDER *P=Context;CONST CR_PEP_GPIO_BINDING *B;CR_STATUS S=BindingFor(P,R,&B);if(CR_ERROR(S)||Out==NULL||Owner==NULL)return CR_ERROR(S)?S:CR_INVALID_PARAMETER;
  CR_GPIO_OWNER_VALUE V;BOOLEAN Pending;S=P->Io.Get(P->Io.Context,Owner,&B->Key,&V,&Pending);if(!CR_ERROR(S))Encode(Out,&V);return S;
}
STATIC CR_STATUS Apply(VOID *Context,VOID *Owner,UINT64 R,CONST CR_PEP_RESOURCE_OP *O) {
  CR_PEP_GPIO_PROVIDER *P=Context;CONST CR_PEP_GPIO_BINDING *B,*Desired;CR_STATUS S=BindingFor(P,R,&B);if(CR_ERROR(S))return S;
  CR_GPIO_OWNER_VALUE V={.Present=TRUE};S=CrPepGpioProviderDescribe(P,O,&Desired,&V.Config);if(CR_ERROR(S))return S;if(Desired!=B)return CR_INVALID_PARAMETER;
  return P->Io.Set(P->Io.Context,Owner,&B->Key,&V);
}
STATIC CR_STATUS Restore(VOID *Context,VOID *Owner,UINT64 R,CONST CR_PEP_JOURNAL_VALUE *In) {
  CR_PEP_GPIO_PROVIDER *P=Context;CONST CR_PEP_GPIO_BINDING *B;CR_STATUS S=BindingFor(P,R,&B);if(CR_ERROR(S)||In==NULL||Owner==NULL)return CR_ERROR(S)?S:CR_INVALID_PARAMETER;
  if(In->Words[0]>1 || (In->Words[2]>>16)!=0)return CR_INVALID_PARAMETER;
  for(UINT32 I=3;I<8;++I)if(In->Words[I])return CR_INVALID_PARAMETER;
  CR_GPIO_OWNER_VALUE V={.Present=(BOOLEAN)In->Words[0]};UINT8 *Bytes=(UINT8 *)&V.Config;
  for(UINTN I=0;I<sizeof(V.Config);++I)Bytes[I]=(UINT8)(In->Words[1+I/8]>>((I%8)*8));
  if(!V.Present&&(In->Words[1]||In->Words[2]))return CR_INVALID_PARAMETER;
  return P->Io.Set(P->Io.Context,Owner,&B->Key,&V);
}
CR_STATUS CrPepGpioProviderInit(CR_PEP_GPIO_PROVIDER *P,CONST CR_PEP_GPIO_IO *Io,CONST CR_PEP_GPIO_BINDING *Bindings,UINT32 Count) {
  if(CR_ERROR(CrMutexCheckLevel())||P==NULL||Io==NULL||Io->Validate==NULL||Io->Get==NULL||Io->Set==NULL||Bindings==NULL||Count==0||Count>512)return CR_INVALID_PARAMETER;
  if(P->Initialized)return CR_ALREADY_STARTED;
  for(UINT32 I=0;I<Count;++I) {
    CONST CR_PEP_GPIO_BINDING *B=&Bindings[I];if(B->Key.Kind>CrGpioOwnerPmic||B->Profiles==NULL||B->ProfileCount==0||B->ProfileCount>256)return CR_INVALID_PARAMETER;
    for(UINT32 K=0;K<I;++K) {
      CONST CR_PEP_GPIO_BINDING *Other=&Bindings[K];
      if(Other->Key.Kind==B->Key.Kind&&Other->Key.Controller==B->Key.Controller&&Other->Key.Pin==B->Key.Pin)return CR_INVALID_PARAMETER;
    }
    for(UINT32 J=0;J<B->ProfileCount;++J) {
      CONST CR_PEP_GPIO_PROFILE *F=&B->Profiles[J];UINT32 N=F->ArgumentCount;
      if(!((F->Kind==CrPepTlmmGpio&&(N==6||N==7)&&F->Command.Length==0&&B->Key.Kind==CrGpioOwnerTlmm)||
           (F->Kind==CrPepTlmmGpioV2&&(N==7||N==8)&&F->Command.Length==0&&B->Key.Kind==CrGpioOwnerTlmm)||
           (F->Kind==CrPepPmicGpio&&N==7&&F->Command.Data!=NULL&&F->Command.Length!=0&&B->Key.Kind==CrGpioOwnerPmic)))return CR_INVALID_PARAMETER;
      for(UINT32 K=0;K<=I;++K)for(UINT32 L=0;L<Bindings[K].ProfileCount;++L) {
        if(K==I&&L>=J)break;
        CONST CR_PEP_GPIO_PROFILE *Other=&Bindings[K].Profiles[L];
        CR_PEP_RESOURCE_OP Op={.Kind=Other->Kind,.Name=Other->Command,.ArgumentCount=Other->ArgumentCount};
        for(UINT32 A=0;A<Other->ArgumentCount;++A)Op.Arguments[A]=Other->Arguments[A];
        if(Match(F,&Op))return CR_INVALID_PARAMETER;
      }
    }
  }
  *P=(CR_PEP_GPIO_PROVIDER){*Io,Bindings,Count,TRUE};return CR_SUCCESS;
}
CR_STATUS CrPepGpioProviderGetJournal(CR_PEP_GPIO_PROVIDER *P,CR_PEP_JOURNAL_PROVIDER *J) {CR_STATUS S=Check(P);if(CR_ERROR(S)||J==NULL)return CR_ERROR(S)?S:CR_INVALID_PARAMETER;*J=(CR_PEP_JOURNAL_PROVIDER){P,Resolve,Capture,Apply,Restore};return CR_SUCCESS;}
CR_STATUS CrPepGpioProviderUninit(CR_PEP_GPIO_PROVIDER *P) {CR_STATUS S=Check(P);if(CR_ERROR(S))return S;cr_memset(P,0,sizeof(*P));return CR_SUCCESS;}
