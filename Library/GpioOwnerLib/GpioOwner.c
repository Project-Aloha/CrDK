/* Exact logical owner retention; hardware lives in GPIO/PMIC cores. MIT */
#include <Library/GpioOwnerLib.h>
#include <oskal/cr_mutex.h>
#include <oskal/cr_string.h>
STATIC BOOLEAN KeyEqual(CONST CR_GPIO_OWNER_KEY *A,CONST CR_GPIO_OWNER_KEY *B) {return A->Kind==B->Kind&&A->Controller==B->Controller&&A->Pin==B->Pin;}
STATIC BOOLEAN ConfigEqual(CONST CR_GPIO_OWNER_CONFIG *A,CONST CR_GPIO_OWNER_CONFIG *B) {return cr_memcmp(A,B,sizeof(*A))==0;}
STATIC CR_STATUS Check(CR_GPIO_OWNER_BANK *B) {return !CR_ERROR(CrMutexCheckLevel())&&B!=NULL&&B->Initialized ? CR_SUCCESS : CR_INVALID_PARAMETER;}
STATIC CR_STATUS Find(CR_GPIO_OWNER_BANK *B,CONST CR_GPIO_OWNER_KEY *Key,UINT32 *Index) {
  if(Key==NULL)return CR_INVALID_PARAMETER;
  for(UINT32 I=0;I<B->ResourceCount;++I)if(KeyEqual(Key,&B->Resources[I].Key)){*Index=I;return CR_SUCCESS;}
  return CR_NOT_FOUND;
}
STATIC CR_GPIO_OWNER_ENTRY *Entry(CR_GPIO_OWNER_BANK *B,VOID *Owner,UINT32 R) {for(UINT32 I=0;I<B->Capacity;++I)if(B->Entries[I].Owner==Owner&&B->Entries[I].Resource==R)return &B->Entries[I];return NULL;}
STATIC BOOLEAN Any(CR_GPIO_OWNER_BANK *B,UINT32 R) {for(UINT32 I=0;I<B->Capacity;++I)if(B->Entries[I].Owner!=NULL&&B->Entries[I].Resource==R)return TRUE;return FALSE;}
STATIC CR_STATUS Aggregate(CR_GPIO_OWNER_BANK *B,UINT32 R,CR_GPIO_OWNER_ENTRY *Replace,CONST CR_GPIO_OWNER_VALUE *Value,CR_GPIO_OWNER_CONFIG *Output) {
  BOOLEAN Have=FALSE;*Output=B->Resources[R].Baseline;
  for(UINT32 I=0;I<B->Capacity;++I) {
    CR_GPIO_OWNER_ENTRY *E=&B->Entries[I];if(E->Owner==NULL||E->Resource!=R)continue;
    CONST CR_GPIO_OWNER_VALUE *V=E==Replace ? Value : &E->Value;if(!V->Present)continue;
    if(Have&&!ConfigEqual(Output,&V->Config))return CR_BUSY;
    *Output=V->Config;Have=TRUE;
  }
  return CR_SUCCESS;
}
CR_STATUS CrGpioOwnerInit(CR_GPIO_OWNER_BANK *B,CONST CR_GPIO_OWNER_IO *Io,CR_GPIO_OWNER_RESOURCE *R,UINT32 Count,CR_GPIO_OWNER_ENTRY *E,UINT32 Capacity) {
  if(CR_ERROR(CrMutexCheckLevel())||B==NULL||Io==NULL||Io->Validate==NULL||Io->Claim==NULL||Io->Unclaim==NULL||Io->Write==NULL||R==NULL||Count==0||Count>512||E==NULL||Capacity==0||Capacity>4096)return CR_INVALID_PARAMETER;
  if(B->Initialized)return CR_ALREADY_STARTED;
  for(UINT32 I=0;I<Count;++I) {
    if(R[I].Key.Kind>CrGpioOwnerPmic||R[I].Claimed||R[I].ClaimPending||R[I].Pending)return CR_INVALID_PARAMETER;
    for(UINT32 P=0;P<I;++P)if(KeyEqual(&R[P].Key,&R[I].Key))return CR_INVALID_PARAMETER;
    CR_STATUS S=Io->Validate(Io->Context,&R[I].Key,&R[I].Baseline);if(CR_ERROR(S))return S;
  }
  for(UINT32 I=0;I<Capacity;++I)if(E[I].Owner!=NULL)return CR_INVALID_PARAMETER;
  cr_memset(E,0,sizeof(*E)*Capacity);
  *B=(CR_GPIO_OWNER_BANK){*Io,R,Count,E,Capacity,TRUE};return CR_SUCCESS;
}
CR_STATUS CrGpioOwnerValidate(CR_GPIO_OWNER_BANK *B,CONST CR_GPIO_OWNER_KEY *Key,CONST CR_GPIO_OWNER_CONFIG *C) {
  CR_STATUS S=Check(B);if(CR_ERROR(S)||C==NULL)return CR_ERROR(S)?S:CR_INVALID_PARAMETER;UINT32 R;S=Find(B,Key,&R);if(CR_ERROR(S))return S;
  return B->Io.Validate(B->Io.Context,Key,C);
}
CR_STATUS CrGpioOwnerReserve(CR_GPIO_OWNER_BANK *B,VOID *Owner,CONST CR_GPIO_OWNER_KEY *Key) {
  CR_STATUS S=Check(B);if(CR_ERROR(S)||Owner==NULL)return CR_ERROR(S)?S:CR_INVALID_PARAMETER;
  UINT32 R;S=Find(B,Key,&R);if(CR_ERROR(S))return S;
  CR_GPIO_OWNER_ENTRY *Existing=Entry(B,Owner,R);
  if(Existing!=NULL) {
    if(Existing->Closing)return CR_NOT_READY;
    if(!B->Resources[R].ClaimPending)return CR_SUCCESS;
    S=B->Io.Claim(B->Io.Context,Key);if(!CR_ERROR(S))B->Resources[R].ClaimPending=FALSE;return S;
  }
  CR_GPIO_OWNER_ENTRY *Free=NULL;for(UINT32 I=0;I<B->Capacity;++I)if(B->Entries[I].Owner==NULL){Free=&B->Entries[I];break;}
  if(Free==NULL)return CR_OUT_OF_RESOURCES;
  /* Record first: a fallible metadata Claim may have committed its class
   * fence. Retain this identity and force Unclaim on failed setup cleanup. */
  cr_memset(Free,0,sizeof(*Free));Free->Owner=Owner;Free->Resource=R;
  if(!B->Resources[R].Claimed) {
    B->Resources[R].Claimed=TRUE;
    B->Resources[R].ClaimPending=TRUE;
    S=B->Io.Claim(B->Io.Context,Key);if(CR_ERROR(S))return S;
    B->Resources[R].ClaimPending=FALSE;
  }
  if(B->Resources[R].ClaimPending)return CR_NOT_READY;
  return CR_SUCCESS;
}
CR_STATUS CrGpioOwnerGet(CR_GPIO_OWNER_BANK *B,VOID *Owner,CONST CR_GPIO_OWNER_KEY *Key,CR_GPIO_OWNER_VALUE *V,BOOLEAN *Pending) {
  CR_STATUS S=Check(B);if(CR_ERROR(S)||Owner==NULL||V==NULL||Pending==NULL)return CR_ERROR(S)?S:CR_INVALID_PARAMETER;
  UINT32 R;S=Find(B,Key,&R);if(CR_ERROR(S))return S;CR_GPIO_OWNER_ENTRY *E=Entry(B,Owner,R);if(E==NULL)return CR_NOT_FOUND;
  *V=E->Value;*Pending=B->Resources[R].Pending||B->Resources[R].ClaimPending||E->Closing;return B->Io.Validate(B->Io.Context,Key,&B->Resources[R].Baseline);
}
CR_STATUS CrGpioOwnerSet(CR_GPIO_OWNER_BANK *B,VOID *Owner,CONST CR_GPIO_OWNER_KEY *Key,CONST CR_GPIO_OWNER_VALUE *V) {
  CR_STATUS S=Check(B);if(CR_ERROR(S)||Owner==NULL||V==NULL||V->Present>1)return CR_ERROR(S)?S:CR_INVALID_PARAMETER;
  CR_GPIO_OWNER_CONFIG Zero={0};if(!V->Present&&!ConfigEqual(&V->Config,&Zero))return CR_INVALID_PARAMETER;
  UINT32 R;S=Find(B,Key,&R);if(CR_ERROR(S))return S;CR_GPIO_OWNER_ENTRY *E=Entry(B,Owner,R);if(E==NULL)return CR_NOT_FOUND;
  if(B->Resources[R].ClaimPending || (E->Closing&&V->Present))return CR_NOT_READY;
  S=B->Io.Validate(B->Io.Context,Key,V->Present?&V->Config:&B->Resources[R].Baseline);if(CR_ERROR(S))return S;
  if(B->Resources[R].Pending&&!E->RecoveryPending)return CR_NOT_READY;
  CR_GPIO_OWNER_CONFIG Desired;S=Aggregate(B,R,E,V,&Desired);if(CR_ERROR(S))return S;
  if(E->RecoveryPending) {
    CR_GPIO_OWNER_CONFIG Old;S=Aggregate(B,R,NULL,NULL,&Old);if(CR_ERROR(S))return S;
    S=B->Io.Write(B->Io.Context,Key,&Old);if(CR_ERROR(S))return S;
    E->RecoveryPending=FALSE;B->Resources[R].Pending=FALSE;
  }
  S=B->Io.Write(B->Io.Context,Key,&Desired);
  if(CR_ERROR(S)){E->RecoveryPending=TRUE;B->Resources[R].Pending=TRUE;return S;}
  E->Value=*V;return CR_SUCCESS;
}
CR_STATUS CrGpioOwnerRelease(CR_GPIO_OWNER_BANK *B,VOID *Owner) {
  CR_STATUS S=Check(B);if(CR_ERROR(S)||Owner==NULL)return CR_ERROR(S)?S:CR_INVALID_PARAMETER;
  for(UINT32 I=B->Capacity;I>0;--I) {
    CR_GPIO_OWNER_ENTRY *E=&B->Entries[I-1];if(E->Owner!=Owner)continue;UINT32 R=E->Resource;E->Closing=TRUE;
    if(E->Value.Present||E->RecoveryPending) {CR_GPIO_OWNER_VALUE Absent={0};S=CrGpioOwnerSet(B,Owner,&B->Resources[R].Key,&Absent);if(CR_ERROR(S))return S;}
    /* Untouched absence never repairs a foreign writer's pending state. */
    BOOLEAN Last=TRUE;for(UINT32 J=0;J<B->Capacity;++J)if(&B->Entries[J]!=E&&B->Entries[J].Owner!=NULL&&B->Entries[J].Resource==R){Last=FALSE;break;}
    if(Last&&B->Resources[R].Claimed) {S=B->Io.Unclaim(B->Io.Context,&B->Resources[R].Key);if(CR_ERROR(S))return S;B->Resources[R].Claimed=FALSE;B->Resources[R].ClaimPending=FALSE;}
    cr_memset(E,0,sizeof(*E));
  }
  return CR_SUCCESS;
}
CR_STATUS CrGpioOwnerDrain(CR_GPIO_OWNER_BANK *B) {
  CR_STATUS S=Check(B);if(CR_ERROR(S))return S;
  for(UINT32 I=0;I<B->Capacity;++I)if(B->Entries[I].Owner!=NULL){S=CrGpioOwnerRelease(B,B->Entries[I].Owner);if(CR_ERROR(S))return S;}
  return CR_SUCCESS;
}
CR_STATUS CrGpioOwnerUninit(CR_GPIO_OWNER_BANK *B) {
  CR_STATUS S=Check(B);if(CR_ERROR(S))return S;
  for(UINT32 I=0;I<B->ResourceCount;++I)if(B->Resources[I].Claimed||B->Resources[I].ClaimPending||B->Resources[I].Pending||Any(B,I))return CR_BUSY;
  cr_memset(B,0,sizeof(*B));return CR_SUCCESS;
}
