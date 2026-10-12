/* Actual ClockLib/SM8450 external group fault tests. MIT */
#define main ClockPriorOwnershipRegression
#include "ClockOwnershipTest.c"
#undef main
#include <Library/ClockExternalLib.h>
static VOID *ExternalFixture(CR_CLOCK_EXTERNAL *E) {
  Reset();VOID *Storage=calloc(1,CrSm8450ClockStateSize());assert(Storage);
  ClockDriverContext *Core=NULL;assert(CrSm8450ClockCreate(Storage,CrSm8450ClockStateSize(),&Core)==CR_SUCCESS);
  CR_CLOCK_MAPPING Mappings[CC_TYPE_MAX];
  for(UINTN C=0;C<CC_TYPE_MAX;++C){UINT64 Physical;UINT32 Size;assert(CrSm8450ClockResource((ClockControllerType)C,&Physical,&Size)==CR_SUCCESS);Mappings[C]=(CR_CLOCK_MAPPING){0x30000000U+C*0x1000000U,Size};}
  assert(CrSm8450ClockBind(Core,Mappings,CC_TYPE_MAX)==CR_SUCCESS);
  CrLockInit(&Core->Lock);Core->LockInitialized=TRUE;Active=Core;memset(E,0,sizeof(*E));
  UINTN R=Reads,W=Writes;assert(CrClockExternalInit(E,Core)==CR_SUCCESS&&Reads==R&&Writes==W);
  return Storage;
}
static VOID Access(CR_CLOCK_EXTERNAL *E,BOOLEAN On) {for(UINT32 I=0;I<3;++I)E->Access[I]->ReferenceCount=On;E->Domain->ReferenceCount=On;}
static VOID Dispose(CR_CLOCK_EXTERNAL *E,VOID *Storage,int *O) {
  assert(CrClockExternalReleaseOwner(E,O)==CR_SUCCESS);Access(E,FALSE);
  assert(CrClockExternalUninit(E)==CR_SUCCESS);free(Storage);Active=NULL;
}
#ifndef CR_CLOCK_EXTERNAL_FIXTURE_ONLY
int main(void) {
  CR_CLOCK_EXTERNAL E;VOID *Storage=ExternalFixture(&E);int O,Foreign;UINT32 V;BOOLEAN On,Pending;
  E.Leaf[0]->ReferenceCount=1;UINTN R=Reads,W=Writes;
  assert(CrClockExternalReserve(&E,&O)==CR_BUSY&&Reads==R&&Writes==W);E.Leaf[0]->ReferenceCount=0;
  E.Source[0]->ExternalConfigPending=TRUE;assert(CrClockExternalReserve(&E,&O)==CR_BUSY&&Reads==R);E.Source[0]->ExternalConfigPending=FALSE;
  assert(CrClockExternalReserve(&E,&O)==CR_SUCCESS&&Reads==R&&Writes==W);
  assert(CrClockExternalReserve(&E,&Foreign)==CR_BUSY);
  assert(CrClockExternalGuard(&E,E.Leaf[0])==CR_BUSY&&CrClockExternalGuard(&E,E.Source[1])==CR_BUSY);
  assert(CrClockExternalGuard(&E,E.Access[0])==CR_SUCCESS&&Reads==R&&Writes==W);
  assert(CrClockExternalRead(&E,&O,0x9000,&V)==CR_NOT_READY&&Reads==R);
  UINT32 FullSize=E.Core->ClockControllers[CC_TYPE_DISP_CC].Size;
  E.Core->ClockControllers[CC_TYPE_DISP_CC].Size=0x9003;
  assert(CrClockExternalRead(&E,&O,0x9000,&V)==CR_NOT_READY&&Reads==R&&Writes==W);
  E.Core->ClockControllers[CC_TYPE_DISP_CC].Size=FullSize;
  UINTN FullCount=E.Core->ClockControllerCount;E.Core->ClockControllerCount=CC_TYPE_DISP_CC;
  assert(CrClockExternalRead(&E,&O,0x9000,&V)==CR_INVALID_PARAMETER&&Reads==R);
  E.Core->ClockControllerCount=FullCount;
  Access(&E,TRUE);assert(CrClockExternalRead(&E,&O,0x8134,&V)==CR_UNSUPPORTED&&Reads==R);
  assert(CrClockExternalSet(&E,&O,148500001,111375000,TRUE)==CR_UNSUPPORTED&&Reads==R&&Writes==W);
  E.Source[1]->ReferenceCount=1;
  assert(CrClockExternalSet(&E,&O,148500000,111375000,TRUE)==CR_BUSY&&Reads==R&&Writes==W&&!E.RecoveryPending);
  E.Source[1]->ReferenceCount=0;
  UINTN Base=E.Source[0]->ParentController->Address;
  *Reg(Base+0x802c)=1;W=Writes;
  assert(CrClockExternalSet(&E,&O,148500000,111375000,TRUE)==CR_NOT_READY&&Writes==W&&!E.RecoveryPending);
  *Reg(Base+0x802c)=0;*Reg(Base+0x80bc)=BIT(1);
  assert(CrClockExternalSet(&E,&O,148500000,111375000,TRUE)==CR_NOT_READY&&Writes==W&&!E.RecoveryPending);
  *Reg(Base+0x80bc)=0;
  *Reg(Base+0x8138)=0xa5a50013;*Reg(Base+0x80c0)=0x87653005;
  *Reg(Base+0x80c4)=0xaaaabbcc;*Reg(Base+0x80c8)=0xbbbbccdd;*Reg(Base+0x80cc)=0xccccddee;
  *Reg(Base+0x814c)=0xabcd0003;
  assert(CrClockExternalSet(&E,&O,148500000,111375000,TRUE)==CR_SUCCESS);
  assert((*Reg(Base+0x8138)&0x371f)==0x201&&(*Reg(Base+0x80c0)&0x371f)==0x101&&(*Reg(Base+0x814c)&15)==1);
  assert(CrClockExternalGet(&E,&O,&On,&Pending)==CR_SUCCESS&&On&&!Pending);
  W=Writes;assert(CrClockExternalSet(&E,&O,148500000,111375000,TRUE)==CR_SUCCESS&&Writes==W);
  assert(CrClockExternalSet(&E,&Foreign,0,0,FALSE)==CR_BUSY&&Writes==W);
  BlockDisable=Base+0x8004;assert(CrClockExternalSet(&E,&O,0,0,FALSE)==CR_TIMEOUT&&E.Owner==&O&&E.RecoveryPending&&E.Held[2]);
  assert(CrClockExternalGet(&E,&O,&On,&Pending)==CR_SUCCESS&&Pending);
  assert(CrClockExternalSet(&E,&O,148500000,111375000,TRUE)==CR_NOT_READY);
  BlockDisable=0;assert(CrClockExternalSet(&E,&O,0,0,FALSE)==CR_SUCCESS);
  assert(*Reg(Base+0x8138)==0xa5a50013&&*Reg(Base+0x80c0)==0x87653005&&*Reg(Base+0x80c4)==0xaaaabbcc&&*Reg(Base+0x80c8)==0xbbbbccdd&&*Reg(Base+0x80cc)==0xccccddee&&*Reg(Base+0x814c)==0xabcd0003);
  assert(CrClockExternalGet(&E,&O,&On,&Pending)==CR_SUCCESS&&!On&&!Pending);Dispose(&E,Storage,&O);
  for(UINT32 Failure=0;Failure<3;++Failure) {
    Storage=ExternalFixture(&E);assert(CrClockExternalReserve(&E,&O)==CR_SUCCESS);Access(&E,TRUE);
    BlockEnable=E.Leaf[Failure]->ParentController->Address+E.Leaf[Failure]->EnableRegister;
    assert(CrClockExternalSet(&E,&O,148500000,111375000,TRUE)==CR_TIMEOUT&&E.Owner==&O&&E.RecoveryPending);
    BlockEnable=0;assert(CrClockExternalSet(&E,&O,0,0,FALSE)==CR_SUCCESS);Dispose(&E,Storage,&O);
  }
  Storage=ExternalFixture(&E);assert(CrClockExternalReserve(&E,&O)==CR_SUCCESS);Access(&E,TRUE);AutoClock=FALSE;
  assert(CrClockExternalSet(&E,&O,148500000,111375000,TRUE)==CR_TIMEOUT&&E.SourceSaved[0]);
  assert(CrClockExternalReleaseOwner(&E,&O)==CR_TIMEOUT&&E.Owner==&O&&E.SourceSaved[0]);
  AutoClock=TRUE;assert(CrClockExternalSet(&E,&O,0,0,FALSE)==CR_SUCCESS);Dispose(&E,Storage,&O);
  Storage=ExternalFixture(&E);assert(CrClockExternalReserve(&E,&O)==CR_SUCCESS);Access(&E,TRUE);
  assert(CrClockExternalSet(&E,&O,148500000,111375000,TRUE)==CR_SUCCESS);
  W=Writes;Access(&E,FALSE);assert(CrClockExternalSet(&E,&O,0,0,FALSE)==CR_NOT_READY&&Writes==W&&E.Owner==&O);
  Access(&E,TRUE);assert(CrClockExternalDrain(&E)==CR_SUCCESS&&E.Owner==NULL);Access(&E,FALSE);assert(CrClockExternalUninit(&E)==CR_SUCCESS);free(Storage);Active=NULL;
  puts("Clock external: actual DSI source/divider, pure reservation, foreign exclusion, full snapshot restore and retained retries passed");return 0;
}

#endif
