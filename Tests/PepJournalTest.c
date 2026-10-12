/* Actual portable journal with shared-vote/partial-write fault model. MIT */
#include <Library/PepJournalLib.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "PepFixture.h"

#define RESOURCES 128U
typedef struct { CR_PEP_RESOURCE_KIND Kind;CR_PEP_TEXT Name,Secondary; } KEY;
static KEY Keys[RESOURCES];static UINT32 KeyCount,Owners[2];
static CR_PEP_JOURNAL_VALUE Votes[2][RESOURCES];
static UINT64 Hardware[RESOURCES];
static UINT32 Resolved,Captured,Written,Restored,FailWriteAt,FailRestoreAt,FailCaptureAt;
static UINT32 RestoreOrder[512],RestoreCount;
static CR_PEP_JOURNAL_VALUE RestoreValues[512];
#ifdef _KERNEL_MODE
static KIRQL Irql;
KIRQL KeGetCurrentIrql(VOID) { return Irql; }
#else
VOID *EFIAPI SetMem(VOID *Buffer,UINTN Length,UINT8 Value) { return memset(Buffer,Value,Length); }
#endif
static BOOLEAN TextEqual(CR_PEP_TEXT A,CR_PEP_TEXT B) {
  return A.Length==B.Length && (A.Length==0 || !memcmp(A.Data,B.Data,A.Length));
}
static UINT32 OwnerIndex(VOID *Owner) {
  assert(Owner==&Owners[0] || Owner==&Owners[1]);return Owner==&Owners[0] ? 0U : 1U;
}
static UINT32 Index(UINT64 Resource) {
  assert((Resource&0xffffffffffffff00ULL)==0x1122334400000000ULL);
  UINT32 I=(UINT32)(Resource&255);assert(I<KeyCount);return I;
}
static VOID Update(UINT32 I) {
  Hardware[I]=Votes[0][I].Words[0]>Votes[1][I].Words[0] ? Votes[0][I].Words[0] : Votes[1][I].Words[0];
}
static CR_STATUS Resolve(VOID *Context,VOID *Owner,CONST CR_PEP_RESOURCE_OP *Op,UINT64 *Resource) {
  (VOID)Context;(VOID)OwnerIndex(Owner);++Resolved;
  if (Op->ArgumentCount>5) return CR_UNSUPPORTED;
  for (UINT32 I=0;I<KeyCount;++I) if (Keys[I].Kind==Op->Kind && TextEqual(Keys[I].Name,Op->Name) &&
      TextEqual(Keys[I].Secondary,Op->SecondaryName)) {
    *Resource=0x1122334400000000ULL|I;return CR_SUCCESS;
  }
  return CR_UNSUPPORTED;
}
static CR_STATUS Capture(VOID *Context,VOID *Owner,UINT64 Resource,CR_PEP_JOURNAL_VALUE *Value) {
  (VOID)Context;UINT32 O=OwnerIndex(Owner),I=Index(Resource);++Captured;
  if (FailCaptureAt==Captured) return CR_DEVICE_ERROR;
  *Value=Votes[O][I];return CR_SUCCESS;
}
static CR_STATUS Apply(VOID *Context,VOID *Owner,UINT64 Resource,CONST CR_PEP_RESOURCE_OP *Op) {
  (VOID)Context;UINT32 O=OwnerIndex(Owner),I=Index(Resource);++Written;
  CR_PEP_JOURNAL_VALUE New={0};
  for (UINT32 A=0;A<Op->ArgumentCount;++A) New.Words[A]=Op->Arguments[A];
  New.Words[5]=Op->ArgumentCount;New.Words[6]=(UINT64)Op->Kind;
  Votes[O][I]=New;Update(I); // Model a hardware commit preceding an error.
  return FailWriteAt==Written ? CR_DEVICE_ERROR : CR_SUCCESS;
}
static CR_STATUS Restore(VOID *Context,VOID *Owner,UINT64 Resource,CONST CR_PEP_JOURNAL_VALUE *Value) {
  (VOID)Context;UINT32 O=OwnerIndex(Owner),I=Index(Resource);++Restored;
  assert(RestoreCount<512);RestoreOrder[RestoreCount]=I;RestoreValues[RestoreCount++]=*Value;
  Votes[O][I]=*Value;Update(I); // Failed restore can also have changed hardware.
  return FailRestoreAt==Restored ? CR_DEVICE_ERROR : CR_SUCCESS;
}
static CONST CR_PEP_JOURNAL_PROVIDER Provider={NULL,Resolve,Capture,Apply,Restore};
static CR_PEP_RESOURCE_OP Operation(UINT32 I,UINT64 Value) {
  CR_PEP_RESOURCE_OP Op={0};Op.Kind=Keys[I].Kind;Op.Name=Keys[I].Name;
  Op.SecondaryName=Keys[I].Secondary;Op.ArgumentCount=1;Op.Arguments[0]=Value;return Op;
}
static VOID Init(CR_PEP_JOURNAL *J,UINT32 O,CR_PEP_JOURNAL_ENTRY *Tx,UINT32 Tc,
                 CR_PEP_JOURNAL_ENTRY *Owned,UINT32 Oc) {
  assert(CrPepJournalInit(J,&Owners[O],&Provider,Tx,Tc,Owned,Oc)==CR_SUCCESS);
}
static VOID Write(CR_PEP_JOURNAL *J,UINT32 I,UINT64 Value) {
  CR_PEP_RESOURCE_OP Op=Operation(I,Value);assert(CrPepJournalApply(J,&Op)==CR_SUCCESS);
}
static VOID Synthetic(VOID) {
  KeyCount=3;for (UINT32 I=0;I<3;++I) {Keys[I].Kind=CrPepClock;Keys[I].Name=(CR_PEP_TEXT){&"abc"[I],1};}
  Votes[0][0].Words[0]=11;Votes[1][1].Words[0]=900;
  for (UINT32 I=0;I<3;++I) Update(I);
  CR_PEP_JOURNAL J={0},Other={0};CR_PEP_JOURNAL_ENTRY Tx[8],Owned[3],Ot[4],Oo[3];
  assert(CrPepJournalInit(&J,&Owners[0],&Provider,Tx,8,Tx,3)==CR_INVALID_PARAMETER && !J.Initialized);
  assert(CrPepJournalInit(&J,&Owners[0],&Provider,Tx,8,Tx+1,3)==CR_INVALID_PARAMETER);
  assert(CrPepJournalInit(&J,&Owners[0],&Provider,Tx,0,Owned,3)==CR_INVALID_PARAMETER);
  assert(CrPepJournalInit(&J,&Owners[0],&Provider,(CR_PEP_JOURNAL_ENTRY *)((UINT8 *)Tx+1),1,Owned,3)==CR_INVALID_PARAMETER);
  UINTN Top=(~(UINTN)0)-_Alignof(CR_PEP_JOURNAL_ENTRY)+1;
  assert(CrPepJournalInit(&J,&Owners[0],&Provider,(CR_PEP_JOURNAL_ENTRY *)Top,1,Owned,3)==CR_INVALID_PARAMETER);
  assert(!Resolved && !Captured && !Written && !Restored);
  Init(&J,0,Tx,8,Owned,3);Init(&Other,1,Ot,4,Oo,3);
  assert(CrPepJournalInit(&J,&Owners[0],&Provider,Tx,8,Owned,3)==CR_ALREADY_STARTED);
  CR_PEP_RESOURCE_IO Io;assert(CrPepJournalGetResourceIo(&J,&Io)==CR_SUCCESS);
  CR_PEP_RESOURCE_OP Op=Operation(0,123);
  assert(Io.Validate(Io.Context,&Owners[1],&Op)==CR_INVALID_PARAMETER && !Resolved);
  assert(Io.Begin(Io.Context,&Owners[1])==CR_INVALID_PARAMETER);
  assert(CrPepJournalValidate(&J,&Op)==CR_SUCCESS && !Captured && !Written);
  assert(CrPepJournalApply(&J,&Op)==CR_NOT_READY);
  assert(CrPepJournalBegin(&J)==CR_SUCCESS);Write(&J,2,40);
  assert(CrPepJournalCommit(&J)==CR_SUCCESS && J.OwnedCount==1 && !J.TransactionCount);
  assert(CrPepJournalUninit(&J)==CR_BUSY);
  assert(CrPepJournalBegin(&J)==CR_SUCCESS);Write(&J,0,20);Write(&J,0,30);
  FailWriteAt=Written+1;Op=Operation(1,2000);
  assert(CrPepJournalApply(&J,&Op)==CR_DEVICE_ERROR && J.TransactionCount==3 && Hardware[1]==2000);
  // Another owner changes its vote while this owner's rollback is pending.
  assert(CrPepJournalBegin(&Other)==CR_SUCCESS);Write(&Other,0,333);assert(CrPepJournalCommit(&Other)==CR_SUCCESS);
  FailRestoreAt=Restored+2;
  assert(CrPepJournalRollback(&J)==CR_DEVICE_ERROR && J.TransactionCount==2 && J.Failed && J.Active);
  assert(Hardware[1]==900 && Hardware[0]==333 && J.OwnedCount==3);
  assert(CrPepJournalCommit(&J)==CR_NOT_READY && CrPepJournalBegin(&J)==CR_BUSY && CrPepJournalUninit(&J)==CR_BUSY);
  assert(CrPepJournalApply(&J,&Op)==CR_NOT_READY);
  assert(CrPepJournalRollback(&J)==CR_SUCCESS && J.OwnedCount==1 && !J.Active);
  assert(RestoreCount==4 && RestoreOrder[0]==1 && RestoreOrder[1]==0 && RestoreOrder[2]==0 && RestoreOrder[3]==0);
  assert(RestoreValues[1].Words[0]==20 && RestoreValues[2].Words[0]==20 && RestoreValues[3].Words[0]==11);
  assert(Votes[0][0].Words[0]==11 && Votes[0][1].Words[0]==0 && Hardware[0]==333 && Hardware[1]==900);
  FailRestoreAt=Restored+1;
  assert(CrPepJournalClose(&J)==CR_DEVICE_ERROR && J.Closing && J.OwnedCount==1);
  assert(CrPepJournalBegin(&J)==CR_BUSY && CrPepJournalUninit(&J)==CR_BUSY);
  assert(CrPepJournalClose(&J)==CR_SUCCESS && !J.Closing && !J.OwnedCount);
  UINT32 Before=Restored;assert(CrPepJournalClose(&J)==CR_SUCCESS && Restored==Before);
  assert(CrPepJournalClose(&Other)==CR_SUCCESS && Hardware[0]==11 && Hardware[1]==900);
  assert(CrPepJournalUninit(&Other)==CR_SUCCESS);
  // Capture failure has no attempted write, and does not add ownership.
  assert(CrPepJournalBegin(&J)==CR_SUCCESS);FailCaptureAt=Captured+1;Before=Written;
  assert(CrPepJournalApply(&J,&Op)==CR_DEVICE_ERROR && Written==Before && !J.OwnedCount && !J.TransactionCount);
  assert(CrPepJournalRollback(&J)==CR_SUCCESS);FailCaptureAt=0;
#ifdef _KERNEL_MODE
  CR_PEP_JOURNAL Saved=J;Before=Resolved+Captured+Written+Restored;Irql=DISPATCH_LEVEL;
  assert(CR_ERROR(CrPepJournalBegin(&J)) && CR_ERROR(CrPepJournalValidate(&J,&Op)) &&
         CR_ERROR(CrPepJournalApply(&J,&Op)) && CR_ERROR(CrPepJournalCommit(&J)) &&
         CR_ERROR(CrPepJournalRollback(&J)) && CR_ERROR(CrPepJournalClose(&J)) &&
         CR_ERROR(CrPepJournalUninit(&J)) && CR_ERROR(CrPepJournalGetResourceIo(&J,&Io)));
  assert(!memcmp(&Saved,&J,sizeof(J)) && Before==Resolved+Captured+Written+Restored);Irql=PASSIVE_LEVEL;
#endif
  assert(CrPepJournalUninit(&J)==CR_SUCCESS);
  // Capacity is checked before capture/write. Repeated resources consume one
  // lifetime entry but one undo record for every attempted operation.
  Init(&J,0,Tx,2,Owned,1);assert(CrPepJournalBegin(&J)==CR_SUCCESS);Write(&J,0,50);Write(&J,0,50);
  Before=Captured;Op=Operation(0,60);
  assert(CrPepJournalApply(&J,&Op)==CR_OUT_OF_RESOURCES && Captured==Before);
  assert(CrPepJournalRollback(&J)==CR_SUCCESS && Hardware[0]==11);
  assert(CrPepJournalBegin(&J)==CR_SUCCESS);Write(&J,0,60);Op=Operation(1,1000);Before=Captured;
  assert(CrPepJournalApply(&J,&Op)==CR_OUT_OF_RESOURCES && Captured==Before && Hardware[1]==900);
  assert(CrPepJournalRollback(&J)==CR_SUCCESS && CrPepJournalUninit(&J)==CR_SUCCESS);
}
static VOID AddFixtureKeys(CONST CR_PEP_VALUE *Value) {
  CR_PEP_RESOURCE_OP Op;
  if (CrPepDecodeResource(Value,&Op)==CR_SUCCESS) {
    for (UINT32 I=0;I<KeyCount;++I) if (Keys[I].Kind==Op.Kind && TextEqual(Keys[I].Name,Op.Name) &&
        TextEqual(Keys[I].Secondary,Op.SecondaryName)) return;
    assert(KeyCount<RESOURCES);Keys[KeyCount++]=(KEY){Op.Kind,Op.Name,Op.SecondaryName};return;
  }
  if (Value->Type==CrPepPackage)
    for (UINTN I=0;I<Value->Package.Count;++I) AddFixtureKeys(&Value->Package.Values[I]);
}
static VOID ActualKodiak(VOID) {
  memset(Keys,0,sizeof(Keys));memset(Votes,0,sizeof(Votes));memset(Hardware,0,sizeof(Hardware));KeyCount=0;
  FailWriteAt=FailRestoreAt=FailCaptureAt=0;RestoreCount=0;AddFixtureKeys(&KodiakRoot);
  for (UINT32 I=0;I<KeyCount;++I) {Votes[1][I].Words[0]=900;Update(I);}
  CR_PEP_POLICY Policy;assert(CrPepPolicyInit(&Policy,&KodiakRoot)==CR_SUCCESS);
  CR_PEP_JOURNAL J={0};
  CR_PEP_JOURNAL_ENTRY *Tx=calloc(512,sizeof(*Tx)),*Owned=calloc(RESOURCES,sizeof(*Owned));assert(Tx && Owned);
  Init(&J,0,Tx,512,Owned,RESOURCES);CR_PEP_RESOURCE_IO Io;assert(CrPepJournalGetResourceIo(&J,&Io)==CR_SUCCESS);
  UINT32 Operations=0;
  for (UINT32 D=0;D<5;++D) {
    CR_PEP_DEVICE_INFO Info;assert(CrPepGetDeviceInfo(&Policy,D,&Info)==CR_SUCCESS);
    assert(CrPepValidateDeviceResources(&Policy,D,&Owners[0],&Io)==CR_SUCCESS);
    for (UINT32 S=0;S<4;++S) if (Info.DStateMask&(1U<<S)) {
      CR_PEP_RESULT Result;assert(CrPepApplyState(&Policy,D,CrPepDState,0,S,&Owners[0],&Io,&Result)==CR_SUCCESS);
      Operations+=Result.Applied;
    }
    for (UINT32 C=0;C<32;++C) if (Info.ComponentMask&(1U<<C)) {
      CR_PEP_COMPONENT_INFO Component;assert(CrPepGetComponentInfo(&Policy,D,C,&Component)==CR_SUCCESS);
      for (UINT32 Kind=0;Kind<2;++Kind) for (UINT32 S=0;S<64;++S)
        if ((Kind ? Component.PStateMask : Component.FStateMask)&(1ULL<<S)) {
          CR_PEP_RESULT Result;
          assert(CrPepApplyState(&Policy,D,Kind ? CrPepPState : CrPepFState,C,S,&Owners[0],&Io,&Result)==CR_SUCCESS);
          Operations+=Result.Applied;
        }
    }
    assert(CrPepJournalClose(&J)==CR_SUCCESS && !J.OwnedCount);
    for (UINT32 I=0;I<KeyCount;++I) assert(Votes[0][I].Words[0]==0 && Hardware[I]==900);
  }
  assert(Operations==239 && CrPepJournalUninit(&J)==CR_SUCCESS);free(Tx);free(Owned);
  printf("PEP journal: actual Kodiak five devices/all239operations, %u immutable model bindings and foreign votes preserved\n",KeyCount);
}
int main(void) {
  Synthetic();ActualKodiak();
  puts("PEP journal: partial write/restore, reverse repeated-resource undo, retained close, capacity, owner and context checks passed");
  return 0;
}
