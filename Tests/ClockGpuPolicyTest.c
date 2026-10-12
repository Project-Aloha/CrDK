/* Focused generic GPU clock policy tests; real core/voter. SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <assert.h>
static uintptr_t NeverWrite;
static void ObserveWrite(uintptr_t Address,uint32_t Old) { (void)Old;assert(Address!=NeverWrite); }
#define CR_CLOCK_TEST_WRITE_CALLBACK ObserveWrite
#define main ClockOwnershipFixtureMain
#include "ClockOwnershipTest.c"
#undef main

static ClockNode First,Second,Ro,Leaf;
static ClockNode *RoParents[2],*LeafParents[1],*PolicyNodes[4];
static UINT64 RoRates[2],LeafRates[1];
static CR_CLOCK_VOTER Votes;
static CR_CLOCK_VOTE_ENTRY Slots[4];
static UINT32 Owner;
static void Setup(void) {
  Reset();Controller.MemMapped=TRUE;
  First=(ClockNode){.Type=CLOCK_NODE_TYPE_BRANCH_2,.Name="fixed300",.ParentController=&Controller,
    .FixedRateHz=300000000,.EnableRegister=0x100,.EnableMsk=1,.HaltRegister=0x104};
  Second=(ClockNode){.Type=CLOCK_NODE_TYPE_BRANCH_2,.Name="fixed150",.ParentController=&Controller,
    .FixedRateHz=150000000,.EnableRegister=0x110,.EnableMsk=1,.HaltRegister=0x114};
  RoParents[0]=&First;RoParents[1]=&Second;RoRates[0]=300000000;RoRates[1]=150000000;
  Ro=(ClockNode){.Type=CLOCK_NODE_TYPE_READ_ONLY_DIVIDER,.Name="ro",.ParentController=&Controller,
    .ParentCount=2,.Parents=RoParents,.ParentRates=RoRates,.DividerRegister=0x180,
    .DividerWidth=4,.DividerReadOnly=TRUE,.DividerInputRateHz=150000000};
  LeafParents[0]=&Ro;LeafRates[0]=75000000;
  Leaf=(ClockNode){.Type=CLOCK_NODE_TYPE_BRANCH_2,.Name="leaf",.ParentController=&Controller,
    .ParentCount=1,.Parents=LeafParents,.ParentRates=LeafRates,.FixedRateHz=75000000,
    .EnableRegister=0x120,.EnableMsk=1,.HaltRegister=0x124};
  PolicyNodes[0]=&First;PolicyNodes[1]=&Second;PolicyNodes[2]=&Ro;PolicyNodes[3]=&Leaf;
  Controller.Clks=PolicyNodes;Controller.ClkCount=4;Active=&Context;NeverWrite=TEST_BASE+0x180;
  *Reg(NeverWrite)=1;
  memset(&Votes,0,sizeof(Votes));assert(CrClockVotesInit(&Votes,&Context,Slots,4)==CR_SUCCESS);
}
static void EdgesAndNoIoValidate(void) {
  Setup();UINTN R=Reads,W=Writes;
  assert(CrClockVotesValidate(&Votes,&Ro,75000000)==CR_SUCCESS);
  assert(CrClockVotesValidate(&Votes,&Ro,50000000)==CR_SUCCESS);
  assert(CrClockVotesValidate(&Votes,&Ro,75000001)==CR_UNSUPPORTED);
  assert(CrClockVotesValidate(&Votes,&Leaf,75000001)==CR_UNSUPPORTED);
  assert(R==Reads && W==Writes);
  assert(ClockEnable(&Context,&Leaf,75000001,TRUE)==CR_UNSUPPORTED && Reads==R && Writes==W);
  assert(ClockEnable(&Context,&Leaf,75000000,TRUE)==CR_SUCCESS);
  assert(First.ReferenceCount==1 && First.ActiveRateHz==300000000);
  assert(Second.ReferenceCount==1 && Second.ActiveRateHz==150000000);
  assert(Ro.ReferenceCount==1 && Ro.ActiveDivider==2 && Ro.ActiveRateHz==75000000);
  assert(Leaf.ReferenceCount==1 && Ro.ParentsHeld==2);
  assert(ClockEnable(&Context,&Leaf,75000000,FALSE)==CR_SUCCESS);
  assert(!Ro.ReferenceCount && !First.ReferenceCount && !Second.ReferenceCount && !Ro.ActiveDivider);
  R=Reads;W=Writes;RoRates[0]=299999999;
  assert(CrClockVotesValidate(&Votes,&Ro,75000000)==CR_UNSUPPORTED && R==Reads && W==Writes);
  RoRates[0]=300000000;Ro.ParentRates=(CONST UINT64 *)((UINTN)RoRates+1);
  assert(CrClockVotesValidate(&Votes,&Ro,75000000)==CR_INVALID_PARAMETER && R==Reads && W==Writes);
  Ro.ParentRates=RoRates;Ro.DividerWidth=0;
  assert(CrClockVotesValidate(&Votes,&Ro,75000000)==CR_INVALID_PARAMETER && R==Reads && W==Writes);
  assert(ClockEnable(&Context,&Ro,75000000,TRUE)==CR_INVALID_PARAMETER && R==Reads && W==Writes);
  Setup();Leaf.Parents[0]=&First;Leaf.ParentRates=NULL;First.FixedRateHz=75000000;
  assert(ClockEnable(&Context,&Leaf,75000000,TRUE)==CR_SUCCESS && First.ActiveRateHz==75000000);
  assert(ClockEnable(&Context,&Leaf,75000000,FALSE)==CR_SUCCESS);
  Leaf.ParentRates=LeafRates;LeafRates[0]=0;
  assert(ClockEnable(&Context,&Leaf,75000000,TRUE)==CR_SUCCESS && First.ActiveRateHz==75000000);
  assert(ClockEnable(&Context,&Leaf,75000000,FALSE)==CR_SUCCESS);
}
static void ChangedSharedDivider(void) {
  Setup();assert(ClockEnable(&Context,&Leaf,75000000,TRUE)==CR_SUCCESS);
  *Reg(NeverWrite)=2;UINTN W=Writes;
  assert(ClockEnable(&Context,&Leaf,75000000,TRUE)==CR_UNSUPPORTED && Writes==W);
  assert(Leaf.ReferenceCount==1 && Ro.ReferenceCount==1);
  *Reg(NeverWrite)=1;
  assert(ClockEnable(&Context,&Leaf,75000000,TRUE)==CR_SUCCESS && Leaf.ReferenceCount==2 && Writes==W);
  assert(ClockEnable(&Context,&Leaf,75000000,FALSE)==CR_SUCCESS);
  assert(ClockEnable(&Context,&Leaf,75000000,FALSE)==CR_SUCCESS);
  Setup();Ro.ParentCount=0;Ro.DividerInputRateHz=1;*Reg(NeverWrite)=0;
  assert(ClockEnable(&Context,&Ro,1,TRUE)==CR_SUCCESS && Ro.ActiveDivider==1);
  *Reg(NeverWrite)=1;W=Writes;
  assert(ClockEnable(&Context,&Ro,1,TRUE)==CR_NOT_READY && Writes==W && Ro.ReferenceCount==1);
  /* Cleanup ignores enable-rate policy and changed read-only hardware. */
  Ro.FixedRateHz=77;Ro.DividerRegister=0xfffffff0U;
  assert(ClockEnable(&Context,&Ro,999,FALSE)==CR_SUCCESS && Writes==W && !Ro.ReferenceCount);
}
static void RepeatedVote(void) {
  Setup();CR_CLOCK_VOTE_VALUE V={.Present=TRUE,.Enabled=TRUE,.RateHz=75000000};
  assert(CrClockVotesSet(&Votes,&Owner,&Leaf,&V)==CR_SUCCESS);
  *Reg(NeverWrite)=2;UINTN W=Writes;
  assert(CrClockVotesSet(&Votes,&Owner,&Leaf,&V)==CR_UNSUPPORTED && Writes==W);
  assert(Leaf.ReferenceCount==1 && Ro.ReferenceCount==1);
  *Reg(NeverWrite)=1;
  assert(CrClockVotesSet(&Votes,&Owner,&Leaf,&V)==CR_SUCCESS && Writes==W && Leaf.ReferenceCount==1);
  assert(CrClockVotesReleaseOwner(&Votes,&Owner)==CR_SUCCESS);
}
static void PartialCleanup(void) {
  Setup();assert(ClockEnable(&Context,&Second,150000000,TRUE)==CR_SUCCESS);
  /* Current encoded divisor rejects a different possible reservation rate. */
  BlockDisable=TEST_BASE+First.EnableRegister;
  BOOLEAN Owned=FALSE;
  assert(ClockAcquire(&Context,&Ro,50000000,&Owned)==CR_UNSUPPORTED && Owned);
  assert(Ro.ReleasePending && Ro.ReferenceCount==1 && Ro.ParentsHeld==1 && !Ro.GateHeld);
  assert(First.ReferenceCount==1 && Second.ReferenceCount==1);
  UINTN W=Writes;assert(ClockAcquire(&Context,&Ro,75000000,&Owned)==CR_NOT_READY && !Owned && Writes==W);
  BlockDisable=0;Ro.FixedRateHz=1;Ro.DividerRegister=0xfffffff0;
  assert(ClockEnable(&Context,&Ro,123,FALSE)==CR_SUCCESS);
  assert(!Ro.ReferenceCount && !Ro.ParentsHeld && !First.ReferenceCount && Second.ReferenceCount==1);
  assert(ClockEnable(&Context,&Second,150000000,FALSE)==CR_SUCCESS);
}
static void AonPolicy(void) {
  Setup();Leaf.KeepEnabled=TRUE;
  assert(ClockEnable(&Context,&Leaf,75000000,TRUE)==CR_SUCCESS);
  UINTN W=Writes;
  assert(ClockEnable(&Context,&Leaf,75000000,FALSE)==CR_SUCCESS);
  /* Only source parents were stopped. The AON leaf control stays asserted. */
  assert((*Reg(TEST_BASE+Leaf.EnableRegister)&1)==1 && Writes>W);
  assert(!Leaf.ReferenceCount && !Ro.ReferenceCount && !First.ReferenceCount && !Second.ReferenceCount);
  Setup();Leaf.ParentCount=0;Leaf.KeepEnabled=TRUE;
  assert(ClockEnable(&Context,&Leaf,75000000,TRUE)==CR_SUCCESS);W=Writes;UINTN R=Reads,D=Delays;
  assert(ClockEnable(&Context,&Leaf,1,FALSE)==CR_SUCCESS && Writes==W && Reads==R && Delays==D);
  Setup();Leaf.KeepEnabled=TRUE;assert(ClockEnable(&Context,&Leaf,75000000,TRUE)==CR_SUCCESS);
  BlockDisable=TEST_BASE+First.EnableRegister;
  assert(ClockEnable(&Context,&Leaf,75000000,FALSE)==CR_TIMEOUT);
  assert(Leaf.ReleasePending && !Leaf.GateHeld && Leaf.ReferenceCount==1 && First.ReferenceCount==1);
  BlockDisable=0;W=Writes;
  assert(ClockEnable(&Context,&Leaf,1,FALSE)==CR_SUCCESS && !First.ReferenceCount);
  assert((*Reg(TEST_BASE+Leaf.EnableRegister)&1)==1 && Writes>W);
}
static void RcgAndCeil(void) {
  Setup();ClockRcgFreqTable F[]={CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(0,0,19200000,1,0,0)};
  ClockNode N={.Type=CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2,.Name="fixedrcg",
    .ParentController=&Controller,.CmdRegister=0x200,.HIDWidth=5,.FreqTable=F,.FreqCount=1,.FixedRateHz=19200000};
  UINTN R=Reads,W=Writes;
  assert(ClockRcg2SetRate(&Context,&N,1,CLOCK_RCG2_POLICY_CEIL)==CR_UNSUPPORTED && Reads==R && Writes==W);
  Ro.ParentCount=0;Ro.DividerInputRateHz=CR_MAX_UINT64;Ro.DividerWidth=16;
  *Reg(NeverWrite)=65535;UINT64 Rate=CR_MAX_UINT64/65536U+1U;
  R=Reads;W=Writes;
  assert(CrClockVotesValidate(&Votes,&Ro,Rate)==CR_SUCCESS && Reads==R && Writes==W);
  assert(ClockEnable(&Context,&Ro,Rate,TRUE)==CR_SUCCESS && Ro.ActiveDivider==65536U);
  assert(ClockEnable(&Context,&Ro,0,FALSE)==CR_SUCCESS);
  Ro.DividerInputRateHz=17;Ro.DividerWidth=3;
  for (UINT32 Div=1;Div<=8;++Div) {
    *Reg(NeverWrite)=Div-1U;Rate=17/Div+(17%Div!=0);
    R=Reads;W=Writes;assert(CrClockVotesValidate(&Votes,&Ro,Rate)==CR_SUCCESS && Reads==R && Writes==W);
    assert(ClockEnable(&Context,&Ro,Rate,TRUE)==CR_SUCCESS && Ro.ActiveDivider==Div);
    assert(ClockEnable(&Context,&Ro,Rate,FALSE)==CR_SUCCESS && Writes==W);
  }
  Ro.RequiredDivider=9;assert(CrClockVotesValidate(&Votes,&Ro,3)==CR_INVALID_PARAMETER);
  Setup();Ro.DividerReadOnly=FALSE;W=Writes;
  assert(ClockSetDivider(&Context,&Ro,2)==CR_SUCCESS && Writes==W);
  assert(ClockSetDivider(&Context,&Ro,3)==CR_UNSUPPORTED && Writes==W);
  Ro.DividerRegister=0;NeverWrite=TEST_BASE;*Reg(NeverWrite)=3;
  assert(ClockSetDivider(&Context,&Ro,4)==CR_SUCCESS && Writes==W);
  assert(ClockSetDivider(&Context,&Ro,3)==CR_UNSUPPORTED && Writes==W);
}
int main(void) {
  EdgesAndNoIoValidate();ChangedSharedDivider();RepeatedVote();PartialCleanup();AonPolicy();RcgAndCeil();
  printf("generic GPU clock policies PASS: edge rates/fixed rate/RO rounding+snapshot/AON/retained cleanup\n");return 0;
}
