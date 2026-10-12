/* Real ClockLib private access/domain lease failures. SPDX-License-Identifier: MIT */
#include <stdint.h>
static uintptr_t DomainAddress, DropWriteAddress;
#define CR_CLOCK_TEST_WRITE_CALLBACK(A,Old) do { \
  if ((A)==DomainAddress) { \
    *Reg(A)=(*Reg(A)&~BIT(31))|((Old)&BIT(31)); \
    if(AutoPower) {if(*Reg(A)&BIT(0))*Reg(A)&=~BIT(31);else *Reg(A)|=BIT(31);} \
  } \
  if ((A)==DropWriteAddress) *Reg(A)=(Old); \
} while(0)
#define CR_CLOCK_EXTERNAL_FIXTURE_ONLY
#include "ClockExternalTest.c"
#include <Library/ClockExternalLease.h>
#ifndef _KERNEL_MODE
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
#endif
static VOID *Fixture(CR_CLOCK_EXTERNAL_LEASE *L) {
  CR_CLOCK_EXTERNAL E;DomainAddress=DropWriteAddress=0;
  VOID *S=ExternalFixture(&E);memset(L,0,sizeof(*L));
  assert(CrClockExternalLeaseInit(L,E.Core)==CR_SUCCESS);
  assert(CrClockExternalUninit(&E)==CR_SUCCESS);
  DomainAddress=L->External.Domain->ParentController->Address+L->External.Domain->GdscRegister;
  *Reg(DomainAddress)=BIT(0)|BIT(10);
  return S;
}
static VOID CheckPrivate(CR_CLOCK_EXTERNAL_LEASE *L) {
  assert(L->Ready && !L->CleanupPending && L->DomainOwned && L->External.Domain->ReferenceCount);
  for(UINT32 I=0;I<3;++I)assert(L->AccessOwned[I] && L->External.Access[I]->ReferenceCount);
}
static VOID Finish(CR_CLOCK_EXTERNAL_LEASE *L,VOID *S,VOID *O) {
  assert(CrClockExternalLeaseRelease(L,O)==CR_SUCCESS);
  assert(!L->External.Owner && !L->Ready && !L->CleanupPending && !L->DomainOwned);
  for(UINT32 I=0;I<3;++I)assert(!L->AccessOwned[I]);
  SM8450_CLOCK_STATE *State=S;
  for(UINTN I=0;I<State->NodeCount;++I)assert(State->Nodes[I].ReferenceCount==0);
  assert(CrClockExternalUninit(&L->External)==CR_SUCCESS);free(S);Active=NULL;
}
static UINTN Gate(ClockNode *N){return N->ParentController->Address+N->EnableRegister;}
static CR_CLOCK_DSI_PLAN Plan(UINT32 I) {
  return (CR_CLOCK_DSI_PLAN){1,I,74250000,55687500,111375000,2,3,1,0};
}
#ifndef CR_CLOCK_EXTERNAL_LEASE_FIXTURE_ONLY
int main(void) {
  (void)Dispose; /* The imported older fixture intentionally uses synthetic refs. */
  CR_CLOCK_EXTERNAL_LEASE L;VOID *S=Fixture(&L);int O,F;BOOLEAN Owned,On,Pending;
  UINT64 Incarnation;CR_CLOCK_DSI_PLAN P,Q;
  assert(CrClockExternalLeaseReserve(&L,&O,&Owned)==CR_SUCCESS&&Owned);CheckPrivate(&L);
  UINTN W=Writes,R=Reads;
  assert(CrClockExternalLeaseReserve(&L,&O,&Owned)==CR_SUCCESS&&Owned&&W==Writes&&R==Reads);
  assert(CrClockExternalLeaseReserve(&L,&F,&Owned)==CR_BUSY&&!Owned&&W==Writes);
  assert(CrClockExternalLeaseRelease(&L,&F)==CR_BUSY&&W==Writes);
  for(UINT32 I=0;I<3;++I)assert(CrClockExternalLeaseGuard(&L,L.External.Access[I])==CR_BUSY);
  assert(CrClockExternalLeaseGuard(&L,L.External.Access[2]->Parents[0])==CR_BUSY);
  assert(CrClockExternalLeaseGuard(&L,L.External.Domain)==CR_BUSY);
  assert(CrClockExternalLeaseGuard(&L,L.External.Leaf[0])==CR_BUSY);
  assert(CrClockExternalLeaseGuard(&L,L.External.SecondSource[1])==CR_BUSY);
  ClockNode *Other=NULL;
  assert(GetClockNode(L.External.Core,"gcc_usb30_prim_master_clk",NULL,NULL,&Other)==CR_SUCCESS);
  assert(CrClockExternalLeaseGuard(&L,Other)==CR_SUCCESS&&W==Writes&&R==Reads);
  for(UINT32 I=0;I<2;++I) {
    P=Plan(I);assert(CrClockExternalLeaseSetPlan(&L,&O,I,&P,TRUE,&Owned)==CR_SUCCESS&&Owned);
    assert(CrClockExternalLeaseGetPlan(&L,&O,I,&Q,&On,&Pending,&Incarnation)==CR_SUCCESS&&On&&!Pending&&Incarnation==1);
    assert(CrClockDsiPlanEqual(&P,&Q));
  }
#ifdef _KERNEL_MODE
  TestIrql=DISPATCH_LEVEL;W=Writes;R=Reads;
  assert(CrClockExternalLeaseReserve(&L,&O,&Owned)==CR_INVALID_PARAMETER&&!Owned);
  assert(CrClockExternalLeaseRelease(&L,&O)==CR_INVALID_PARAMETER);
  assert(CrClockExternalLeaseGuard(&L,Other)==CR_INVALID_PARAMETER);
  assert(CrClockExternalLeaseSetPlan(&L,&O,0,NULL,FALSE,&Owned)==CR_INVALID_PARAMETER&&!Owned);
  assert(CrClockExternalLeaseGetPlan(&L,&O,0,&Q,&On,&Pending,&Incarnation)==CR_INVALID_PARAMETER&&W==Writes&&R==Reads);
  TestIrql=PASSIVE_LEVEL;
#endif
  assert(CrClockExternalLeaseRelease(&L,&O)==CR_SUCCESS);
  assert(CrClockExternalLeaseGuard(&L,L.External.Access[0])==CR_SUCCESS);
  assert(CrClockExternalLeaseReserve(&L,&O,&Owned)==CR_SUCCESS&&Owned&&L.Incarnation==2);
  Finish(&L,S,&O);
  /* Shared preexisting votes survive all private releases. */
  S=Fixture(&L);
  for(UINT32 I=0;I<3;++I)assert(ClockAcquire(L.External.Core,L.External.Access[I],I==2?19200000:0,&Owned)==CR_SUCCESS&&Owned);
  assert(ClockGdscAcquire(L.External.Core,L.External.Domain,&Owned)==CR_SUCCESS&&Owned);
  assert(CrClockExternalLeaseReserve(&L,&O,&Owned)==CR_SUCCESS&&Owned);
  for(UINT32 I=0;I<3;++I)assert(L.External.Access[I]->ReferenceCount==2);
  assert(L.External.Domain->ReferenceCount==2);
  assert(CrClockExternalLeaseRelease(&L,&O)==CR_SUCCESS);
  for(UINT32 I=0;I<3;++I)assert(L.External.Access[I]->ReferenceCount==1);
  assert(L.External.Domain->ReferenceCount==1);
  assert(ClockGdscSet(L.External.Core,L.External.Domain,FALSE)==CR_SUCCESS);
  for(UINT32 I=3;I-- >0;)assert(ClockEnable(L.External.Core,L.External.Access[I],0,FALSE)==CR_SUCCESS);
  assert(CrClockExternalLeaseReserve(&L,&O,&Owned)==CR_SUCCESS&&Owned);Finish(&L,S,&O);
  /* Fail each real prerequisite acquisition; retry must be explicit cleanup. */
  for(UINT32 I=0;I<4;++I) {
    S=Fixture(&L);
    if(I<2)L.External.Access[I]->ReferenceCount=CR_MAX_UINT32;
    else if(I==2)BlockEnable=Gate(L.External.Access[I]);else AutoPower=FALSE;
    CR_STATUS Status=CrClockExternalLeaseReserve(&L,&O,&Owned);
    assert(Status==(I<2?CR_OUT_OF_RESOURCES:CR_TIMEOUT)&&Owned&&L.External.Owner==&O&&L.CleanupPending&&!L.Ready);
    W=Writes;R=Reads;
    assert(CrClockExternalLeaseReserve(&L,&O,&Owned)==CR_NOT_READY&&Owned&&Writes==W&&Reads==R);
    assert(CrClockExternalLeaseReserve(&L,&F,&Owned)==CR_BUSY&&!Owned&&Writes==W);
    P=Plan(0);assert(CrClockExternalLeaseSetPlan(&L,&O,0,&P,TRUE,&Owned)==CR_NOT_READY&&!Owned&&Writes==W);
    assert(CrClockExternalLeaseGuard(&L,L.External.Access[0])==CR_BUSY);
    if(I<2)L.External.Access[I]->ReferenceCount=0;
    BlockEnable=0;AutoPower=TRUE;Finish(&L,S,&O);
  }
  /* Each failed prerequisite withdrawal retains the token and retry refs. */
  for(UINT32 I=0;I<4;++I) {
    S=Fixture(&L);
    if(I<2)*Reg(Gate(L.External.Access[I]))|=L.External.Access[I]->EnableMsk;
    assert(CrClockExternalLeaseReserve(&L,&O,&Owned)==CR_SUCCESS&&Owned);
    if(I<2) {
      DropWriteAddress=Gate(L.External.Access[I]);
      *Reg(DropWriteAddress)&=~L.External.Access[I]->EnableMsk;
    } else if(I==2)BlockDisable=Gate(L.External.Access[I]);else AutoPower=FALSE;
    assert(CrClockExternalLeaseRelease(&L,&O)==(I<2?CR_DEVICE_ERROR:CR_TIMEOUT)&&L.External.Owner==&O&&L.CleanupPending);
    W=Writes;
    assert(CrClockExternalLeaseReserve(&L,&F,&Owned)==CR_BUSY&&!Owned&&Writes==W);
    assert(CrClockExternalLeaseGuard(&L,L.External.Access[0])==CR_BUSY);
    assert(CrClockExternalLeaseGetPlan(&L,&O,0,&Q,&On,&Pending,&Incarnation)==CR_NOT_READY);
    DropWriteAddress=0;BlockDisable=0;AutoPower=TRUE;Finish(&L,S,&O);
  }
  /* Group rollback failures must keep every prerequisite for recovery. */
  for(UINT32 I=0;I<2;++I)for(UINT32 J=0;J<3;++J) {
    S=Fixture(&L);assert(CrClockExternalLeaseReserve(&L,&O,&Owned)==CR_SUCCESS&&Owned);
    ClockNode **Leaves=I?L.External.SecondLeaf:L.External.Leaf;
    BlockEnable=Gate(Leaves[J]);P=Plan(I);
    assert(CrClockExternalLeaseSetPlan(&L,&O,I,&P,TRUE,&Owned)==CR_TIMEOUT&&Owned);CheckPrivate(&L);
    BlockEnable=0;Finish(&L,S,&O);
    S=Fixture(&L);assert(CrClockExternalLeaseReserve(&L,&O,&Owned)==CR_SUCCESS&&Owned);
    Leaves=I?L.External.SecondLeaf:L.External.Leaf;P=Plan(I);
    assert(CrClockExternalLeaseSetPlan(&L,&O,I,&P,TRUE,&Owned)==CR_SUCCESS&&Owned);
    BlockDisable=Gate(Leaves[J]);
    assert(CrClockExternalLeaseRelease(&L,&O)==CR_TIMEOUT&&L.External.Owner==&O);
    assert(L.DomainOwned&&L.External.Domain->ReferenceCount);
    for(UINT32 K=0;K<3;++K)assert(L.AccessOwned[K]&&L.External.Access[K]->ReferenceCount);
    BlockDisable=0;Finish(&L,S,&O);
  }
  for(UINT32 I=0;I<2;++I)for(UINT32 Offset=4;Offset<=16;Offset+=4) {
    S=Fixture(&L);assert(CrClockExternalLeaseReserve(&L,&O,&Owned)==CR_SUCCESS&&Owned);
    ClockNode *Source=I?L.External.SecondSource[1]:L.External.Source[1];
    UINTN Address=Source->ParentController->Address+Source->CmdRegister+Offset;
    *Reg(Address)=0x554400a5;P=Plan(I);
    assert(CrClockExternalLeaseSetPlan(&L,&O,I,&P,TRUE,&Owned)==CR_SUCCESS&&Owned);
    DropWriteAddress=Address;
    assert(CrClockExternalLeaseRelease(&L,&O)==CR_DEVICE_ERROR&&L.External.Owner==&O&&L.DomainOwned);
    for(UINT32 K=0;K<3;++K)assert(L.AccessOwned[K]);
    DropWriteAddress=0;Finish(&L,S,&O);assert(*Reg(Address)==0x554400a5);
  }
  S=Fixture(&L);L.Incarnation=CR_MAX_UINT64;W=Writes;R=Reads;
  assert(CrClockExternalLeaseReserve(&L,&O,&Owned)==CR_OUT_OF_RESOURCES&&!Owned&&!L.External.Owner&&Writes==W&&Reads==R);
  L.Incarnation=0;assert(CrClockExternalLeaseReserve(&L,&O,&Owned)==CR_SUCCESS&&Owned);Finish(&L,S,&O);
  puts("External private lease: shared references, exclusive/idle guards, incarnation, 8 access faults, 12 gate faults, 8 restore faults and retained cleanup PASS");
  return 0;
}

#endif
