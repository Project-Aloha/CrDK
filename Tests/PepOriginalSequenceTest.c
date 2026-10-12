/* Actual original LPXC parser/journal sequence; callback model does not infer units. MIT */
#include <Library/PepJournalLib.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "PepFixture.h"
static UINT32 Owner,DelayCalls,Calls,Captures,Restores,FailDelay,FailRestore;
static UINT32 Order[64],OrderCount;
static BOOLEAN BadIdentity;
static CR_PEP_JOURNAL_VALUE Hardware[3],Baseline[3];
#ifdef _KERNEL_MODE
KIRQL KeGetCurrentIrql(VOID) {return PASSIVE_LEVEL;}
#else
VOID *EFIAPI SetMem(VOID *P,UINTN N,UINT8 V) {return memset(P,V,N);}
#endif
static BOOLEAN ExactOriginal(CONST CR_PEP_RESOURCE_OP *Op) {
  CONST CR_PEP_VALUE *Device=&TouchRoot.Package.Values[0];
  for(UINTN D=2;D<Device->Package.Count;++D) {
    CONST CR_PEP_VALUE *State=&Device->Package.Values[D];
    if(State->Package.Count<2||State->Package.Values[0].Type!=CrPepString||
       State->Package.Values[0].String.Length!=6||memcmp(State->Package.Values[0].String.Data,"DSTATE",6))continue;
    for(UINTN A=2;A<State->Package.Count;++A) {
      CR_PEP_RESOURCE_OP Original;assert(CrPepDecodeResource(&State->Package.Values[A],&Original)==CR_SUCCESS);
      if(Original.Kind==Op->Kind&&Original.ArgumentCount==Op->ArgumentCount&&
         !memcmp(Original.Arguments,Op->Arguments,Op->ArgumentCount*sizeof(UINT64)))return TRUE;
    }
  }
  return FALSE;
}
static CR_STATUS Resolve(VOID *C,VOID *O,CONST CR_PEP_RESOURCE_OP *Op,UINT64 *Resource) {
  assert(C==Hardware&&O==&Owner);
  if(BadIdentity){*Resource=Op->Kind==CrPepDelay ? 0 : CR_PEP_JOURNAL_TRANSIENT_RESOURCE;return CR_SUCCESS;}
  if(!ExactOriginal(Op))return CR_UNSUPPORTED;
  if(Op->Kind==CrPepDelay){*Resource=CR_PEP_JOURNAL_TRANSIENT_RESOURCE;return CR_SUCCESS;}
  assert(Op->Kind==CrPepTlmmGpio||Op->Kind==CrPepTlmmGpioV2);
  UINT64 Pin=Op->Arguments[Op->Kind==CrPepTlmmGpioV2 ? 1 : 0];
  if(Pin==80)*Resource=0;else if(Pin==54)*Resource=1;else {assert(Pin==55);*Resource=2;}
  return CR_SUCCESS;
}
static CR_STATUS Capture(VOID *C,VOID *O,UINT64 R,CR_PEP_JOURNAL_VALUE *V) {
  assert(C==Hardware&&O==&Owner&&R<3);++Captures;*V=Hardware[R];return CR_SUCCESS;
}
static CR_STATUS Apply(VOID *C,VOID *O,UINT64 R,CONST CR_PEP_RESOURCE_OP *Op) {
  assert(C==Hardware&&O==&Owner&&OrderCount<64);++Calls;
  if(Op->Kind==CrPepDelay) {
    assert(R==CR_PEP_JOURNAL_TRANSIENT_RESOURCE&&Op->ArgumentCount==1);
    /* Preserve raw original DELAY values. This fixture makes no time-unit claim. */
    Order[OrderCount++]=(UINT32)(1000+Op->Arguments[0]);
    return ++DelayCalls==FailDelay ? CR_TIMEOUT : CR_SUCCESS;
  }
  assert(R<3&&Op->ArgumentCount<=7);Order[OrderCount++]=(UINT32)R;
  memset(&Hardware[R],0,sizeof(Hardware[R]));
  memcpy(Hardware[R].Words,Op->Arguments,Op->ArgumentCount*sizeof(UINT64));
  Hardware[R].Words[7]=Op->Kind;return CR_SUCCESS;
}
static CR_STATUS Restore(VOID *C,VOID *O,UINT64 R,CONST CR_PEP_JOURNAL_VALUE *V) {
  assert(C==Hardware&&O==&Owner&&R<3&&OrderCount<64);Order[OrderCount++]=2000+(UINT32)R;
  Hardware[R]=*V;return ++Restores==FailRestore ? CR_DEVICE_ERROR : CR_SUCCESS;
}
static VOID Run(VOID) {
  CR_PEP_POLICY Policy;assert(CrPepPolicyInit(&Policy,&TouchRoot)==CR_SUCCESS);
  CR_PEP_DEVICE_INFO Info;assert(CrPepGetDeviceInfo(&Policy,0,&Info)==CR_SUCCESS&&Info.DStateMask==9);
  for(UINT32 R=0;R<3;++R)Baseline[R].Words[0]=100+R;
  memcpy(Hardware,Baseline,sizeof(Hardware));
  CR_PEP_JOURNAL J={0};CR_PEP_JOURNAL_ENTRY Transactions[4],Owned[3];
  CR_PEP_JOURNAL_PROVIDER P={Hardware,Resolve,Capture,Apply,Restore};
  assert(CrPepJournalInit(&J,&Owner,&P,Transactions,4,Owned,3)==CR_SUCCESS);
  CR_PEP_RESOURCE_IO Io;assert(CrPepJournalGetResourceIo(&J,&Io)==CR_SUCCESS);
  assert(CrPepValidateDeviceResources(&Policy,0,&Owner,&Io)==CR_SUCCESS&&!Calls&&!Captures);
  CR_PEP_RESULT Result;
  assert(CrPepApplyState(&Policy,0,CrPepDState,0,1,&Owner,&Io,&Result)==CR_NOT_FOUND&&!Calls);
  assert(CrPepApplyState(&Policy,0,CrPepDState,0,0,&Owner,&Io,&Result)==CR_SUCCESS);
  CONST UINT32 Forward[]={0,1,1005,1,1100,2};
  assert(Result.Applied==6&&Calls==6&&DelayCalls==2&&Captures==4&&J.OwnedCount==3&&J.TransactionCount==0);
  assert(OrderCount==6&&!memcmp(Order,Forward,sizeof(Forward)));
  assert(CrPepJournalClose(&J)==CR_SUCCESS&&Restores==3&&DelayCalls==2&&!memcmp(Hardware,Baseline,sizeof(Hardware)));
  /* A failed first delay prevents subsequent pin writes. Failed reverse write
   * retains the exact original records; retry does not execute the delay again. */
  OrderCount=Calls=DelayCalls=Captures=Restores=0;FailDelay=1;FailRestore=1;
  assert(CrPepApplyState(&Policy,0,CrPepDState,0,0,&Owner,&Io,&Result)==CR_TIMEOUT);
  assert(Result.Applied==2&&Result.RollbackStatus==CR_DEVICE_ERROR&&J.Active&&J.TransactionCount==2&&J.OwnedCount==2);
  CONST UINT32 Failed[]={0,1,1005,2001};
  assert(OrderCount==4&&!memcmp(Order,Failed,sizeof(Failed))&&Calls==3&&DelayCalls==1);
  assert(CrPepJournalRollback(&J)==CR_SUCCESS&&!J.Active&&!J.OwnedCount&&Restores==3&&Order[4]==2001&&Order[5]==2000);
  assert(!memcmp(Hardware,Baseline,sizeof(Hardware))&&DelayCalls==1);
  OrderCount=Calls=DelayCalls=Captures=Restores=0;FailDelay=2;FailRestore=0;
  assert(CrPepApplyState(&Policy,0,CrPepDState,0,0,&Owner,&Io,&Result)==CR_TIMEOUT&&Result.Applied==4&&Result.RollbackStatus==CR_SUCCESS);
  CONST UINT32 Second[]={0,1,1005,1,1100,2001,2001,2000};
  assert(OrderCount==8&&!memcmp(Order,Second,sizeof(Second))&&Captures==3&&Restores==3&&!J.OwnedCount);
  assert(!memcmp(Hardware,Baseline,sizeof(Hardware)));
  BadIdentity=TRUE;CR_PEP_RESOURCE_OP Delay={.Kind=CrPepDelay,.ArgumentCount=1,.Arguments={5}},Clock={.Kind=CrPepClock};
  UINT32 Before=Calls+Captures+Restores;
  assert(CrPepJournalValidate(&J,&Delay)==CR_COMPROMISED_DATA&&CrPepJournalValidate(&J,&Clock)==CR_COMPROMISED_DATA);
  assert(CrPepJournalBegin(&J)==CR_SUCCESS&&CrPepJournalApply(&J,&Delay)==CR_COMPROMISED_DATA);
  assert(CrPepJournalRollback(&J)==CR_SUCCESS&&Before==Calls+Captures+Restores&&CrPepJournalUninit(&J)==CR_SUCCESS);
  puts("Original LPXC: full-width V2/GPIO tuple parsing, raw delay ordering, transient identity, failed delay and retained reverse repair PASS");
}
int main(VOID) {Run();return 0;}
