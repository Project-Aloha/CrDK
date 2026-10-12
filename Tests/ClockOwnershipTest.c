/** @file Clock graph and GDSC ownership regressions. SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifndef _KERNEL_MODE
#undef NULL
#include <Uefi.h>
#endif
#include <Library/PepClockLib.h>
#ifdef CR_CLOCK_SOURCE_PATH
#include CR_CLOCK_SOURCE_PATH
#else
#include "../Library/ClockLib/clock.c"
#endif
#include "../Library/ClockLib/gdsc.c"
#include "../Library/ClockLib/clock_config.c"
#include "../Library/ClockLib/clock_votes.c"
#include "../Library/Sm8450ClockLib/Sm8450Clock.c"

#define TEST_BASE 0x20000000U
static ClockDriverContext *Active;
static BOOLEAN AutoPower=TRUE, AutoClock=TRUE;
static UINTN BlockEnable,BlockDisable;
#ifdef _KERNEL_MODE
static KIRQL TestIrql;
#endif
static UINTN Writes, Reads, Delays;
typedef struct { UINTN Address; UINT32 Value; } REG;
static REG Regs[512];
static UINTN RegCount;
static UINT32 *Reg(UINTN Address) {
  for (UINTN I=0;I<RegCount;++I) if (Regs[I].Address==Address) return &Regs[I].Value;
  assert(RegCount<ARRAY_SIZE(Regs)); Regs[RegCount].Address=Address; Regs[RegCount].Value=0;
  return &Regs[RegCount++].Value;
}
static UINT32 Read(UINTN Address) { ++Reads; return *Reg(Address); }
static UINT32 Write(UINTN Address,UINT32 Value) {
  ++Writes; UINT32 Old=*Reg(Address); *Reg(Address)=Value;
  if (Address==TEST_BASE+0x40) {
    *Reg(Address)=(Value&~BIT(31)) | (Old&BIT(31));
    if (AutoPower) {
      if (Value&BIT(0)) *Reg(Address)&=~BIT(31);
      else *Reg(Address)|=BIT(31);
    }
  }
  if (Active!=NULL && AutoClock) {
    for (UINTN C=0;C<Active->ClockControllerCount;++C) {
      ClockController *Controller=&Active->ClockControllers[C];
      for (UINTN I=0;I<Controller->ClkCount;++I) {
        ClockNode *Node=Controller->Clks[I]; if (Node==NULL) continue;
        if (Node->Type==CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2 &&
            Address==Controller->Address+Node->CmdRegister) {
          *Reg(Address)&=~(BIT(0)|BIT(31));
          if ((Value&BIT(1))==0 && Address!=BlockDisable) *Reg(Address)|=BIT(31);
        }
        if ((Node->Type==CLOCK_NODE_TYPE_BRANCH || Node->Type==CLOCK_NODE_TYPE_BRANCH_2) &&
            Node->HaltRegister!=0 &&
            Address==Controller->Address+Node->EnableRegister) {
          UINTN Halt=Controller->Address+Node->HaltRegister;
          if (Value&Node->EnableMsk) {
            if (Address==BlockEnable) *Reg(Halt)|=BIT(31);
            else *Reg(Halt)&=~BIT(31);
          } else {
            if (Address==BlockDisable) *Reg(Halt)&=~BIT(31);
            else *Reg(Halt)|=BIT(31);
          }
        }
      }
    }
  }
#ifdef CR_CLOCK_TEST_WRITE_CALLBACK
  CR_CLOCK_TEST_WRITE_CALLBACK(Address,Old);
#endif
  return *Reg(Address);
}
#ifdef _KERNEL_MODE
UINT32 CrMmioRead32(UINTN A) { return Read(A); }
UINT32 CrMmioWrite32(UINTN A,UINT32 V) { return Write(A,V); }
UINT32 CrMmioUpdateBits32(UINTN A,UINT32 M,UINT32 V) { return Write(A,(Read(A)&~M)|(V&M)); }
VOID KeInitializeSpinLock(KSPIN_LOCK *L) { *L=1; }
VOID KeAcquireSpinLock(KSPIN_LOCK *L,KIRQL *Irql) { assert(*L==1);*L=2;*Irql=PASSIVE_LEVEL; }
VOID KeReleaseSpinLock(KSPIN_LOCK *L,KIRQL Irql) { assert(*L==2 && Irql==PASSIVE_LEVEL);*L=1; }
KIRQL KeGetCurrentIrql(VOID) { return TestIrql; }
VOID KeStallExecutionProcessor(ULONG Us) { Delays+=Us; }
NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE M,BOOLEAN A,LARGE_INTEGER *I)
{ (void)M;(void)A;Delays+=(UINTN)(-I->QuadPart/10);return STATUS_SUCCESS; }
#else
UINT32 EFIAPI MmioRead32(UINTN A) { return Read(A); }
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V) { return Write(A,V); }
UINTN EFIAPI MicroSecondDelay(UINTN Us) { Delays+=Us;return Us; }
VOID EFIAPI MemoryFence(VOID) {}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...) { (void)Level;(void)Format; }
SPIN_LOCK *EFIAPI InitializeSpinLock(SPIN_LOCK *L) { *L=1;return L; }
SPIN_LOCK *EFIAPI AcquireSpinLock(SPIN_LOCK *L) { assert(*L==1);*L=2;return L; }
SPIN_LOCK *EFIAPI ReleaseSpinLock(SPIN_LOCK *L) { assert(*L==2);*L=1;return L; }
VOID *EFIAPI SetMem(VOID *B,UINTN L,UINT8 V) { return memset(B,V,L); }
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B) { return strcmp(A,B); }
#endif
static ClockController Controller={.Address=TEST_BASE,.Size=0x1000};
static ClockDriverContext Context={.ClockControllerCount=1,.ClockControllers=&Controller};
static ClockNode Domain={.Type=CLOCK_NODE_TYPE_GDSC,.Name="test",.GdscRegister=0x40,
  .GdscFlags=CLOCK_GDSC_FLAG_HW_CTL,.PwrStsFlag=CLOCK_NODE_GDSC_PWR_STS_RET_ON,
  .ParentController=&Controller};
static void Reset(void) {
  Active=NULL; RegCount=Writes=Reads=Delays=0; AutoPower=AutoClock=TRUE;
  BlockEnable=BlockDisable=0;
  Domain.ReferenceCount=0;Domain.RestoreControlEnabled=FALSE;
  Domain.RestoreHwControlled=FALSE;Domain.ReleasePending=FALSE;
  Domain.GdscFlags=CLOCK_GDSC_FLAG_HW_CTL;Domain.CxcCount=0;Domain.CxcRegisters=NULL;
  Controller.Size=0x1000;CrLockInit(&Context.Lock);Context.LockInitialized=TRUE;
}
static void PowerOwnership(void) {
  Reset();*Reg(TEST_BASE+0x40)=BIT(0)|BIT(10);
  assert(ClockGdscSet(&Context,&Domain,TRUE)==CR_SUCCESS);
  assert(Domain.ReferenceCount==1 && !Domain.RestoreControlEnabled);
  UINTN W=Writes;
  assert(ClockGdscSet(&Context,&Domain,TRUE)==CR_SUCCESS && Domain.ReferenceCount==2 && Writes==W);
  assert(ClockGdscSet(&Context,&Domain,FALSE)==CR_SUCCESS && Domain.ReferenceCount==1 && Writes==W);
  assert(ClockGdscSet(&Context,&Domain,FALSE)==CR_SUCCESS && Domain.ReferenceCount==0);
  assert((*Reg(TEST_BASE+0x40)&(BIT(0)|BIT(1)|BIT(10)|BIT(31)))==(BIT(0)|BIT(10)));

  Reset();*Reg(TEST_BASE+0x40)=BIT(31)|BIT(10)|BIT(1);
  assert(ClockGdscSet(&Context,&Domain,TRUE)==CR_SUCCESS);
  *Reg(TEST_BASE+0x40)&=~BIT(31);AutoPower=FALSE; UINTN D=Delays;
  assert(ClockGdscSet(&Context,&Domain,FALSE)==CR_SUCCESS && Delays==D);
  assert((*Reg(TEST_BASE+0x40)&(BIT(0)|BIT(1)|BIT(10)))==(BIT(1)|BIT(10)));

  Reset();*Reg(TEST_BASE+0x40)=BIT(0);AutoPower=FALSE;
  assert(ClockGdscSet(&Context,&Domain,TRUE)==CR_TIMEOUT);
  assert(Domain.ReferenceCount==0 && !Domain.ReleasePending && (*Reg(TEST_BASE+0x40)&BIT(0)));

  Reset();*Reg(TEST_BASE+0x40)=BIT(0)|BIT(31);AutoPower=FALSE;
  assert(ClockGdscSet(&Context,&Domain,TRUE)==CR_SUCCESS);
  assert(ClockGdscSet(&Context,&Domain,FALSE)==CR_TIMEOUT);
  assert(Domain.ReferenceCount==1 && Domain.ReleasePending);
  W=Writes;
  assert(ClockGdscSet(&Context,&Domain,TRUE)==CR_NOT_READY && Writes==W);
  AutoPower=TRUE;
  assert(ClockGdscSet(&Context,&Domain,FALSE)==CR_SUCCESS && Domain.ReferenceCount==0 && !Domain.ReleasePending);

  BOOLEAN Owned = TRUE;
  Reset();Domain.ReferenceCount=CR_MAX_UINT32;
  assert(ClockGdscAcquire(&Context,&Domain,&Owned)==CR_OUT_OF_RESOURCES && !Owned && Writes==0);
  Reset();*Reg(TEST_BASE+0x40)=BIT(0);AutoPower=FALSE;
  assert(ClockGdscAcquire(&Context,&Domain,&Owned)==CR_TIMEOUT && !Owned && !Domain.ReferenceCount);
  Reset();*Reg(TEST_BASE+0x40)=BIT(0);AutoPower=FALSE;
  Domain.GdscFlags |= CLOCK_GDSC_FLAG_POLL_CFG_GDSC_REG;
  assert(ClockGdscAcquire(&Context,&Domain,&Owned)==CR_TIMEOUT && Owned && Domain.ReleasePending);
  W=Writes;
  assert(ClockGdscAcquire(&Context,&Domain,&Owned)==CR_NOT_READY && !Owned && W==Writes);
  *Reg(TEST_BASE+0x44)=BIT(15);
  assert(ClockGdscSet(&Context,&Domain,FALSE)==CR_SUCCESS && !Domain.ReferenceCount && !Domain.ReleasePending);
}
static void MappingBounds(void) {
  Reset();Domain.GdscFlags|=CLOCK_GDSC_FLAG_POLL_CFG_GDSC_REG;Controller.Size=0x47;
  assert(ClockGdscSet(&Context,&Domain,TRUE)==CR_INVALID_PARAMETER && Writes==0 && Reads==0);
  assert(ClockGdscEnable(&Context,&Domain)==CR_INVALID_PARAMETER && Writes==0);
  Controller.Size=0x1000;Domain.CxcCount=1;
  assert(ClockGdscSet(&Context,&Domain,TRUE)==CR_INVALID_PARAMETER && Writes==0);
  UINT32 Cxc=0x1000;Domain.CxcRegisters=&Cxc;
  assert(ClockGdscSet(&Context,&Domain,TRUE)==CR_INVALID_PARAMETER && Writes==0);
  Domain.CxcCount=0;Domain.GdscRegister=0xfffffffcU;
  assert(ClockGdscSet(&Context,&Domain,TRUE)==CR_INVALID_PARAMETER && Writes==0);
  Domain.GdscRegister=0x40;
  assert(ClockGdscSet(&Context,&Domain,2)==CR_INVALID_PARAMETER && Writes==0);
}
static void GraphRelease(void) {
  Reset();
  VOID *Storage=calloc(1,CrSm8450ClockStateSize()); assert(Storage!=NULL);
  ClockDriverContext *Core=NULL;
  assert(CrSm8450ClockCreate(Storage,CrSm8450ClockStateSize(),&Core)==CR_SUCCESS);
  CR_CLOCK_MAPPING Mappings[CC_TYPE_MAX];
  for (UINTN C=0;C<CC_TYPE_MAX;++C) {
    UINT64 Physical;UINT32 Size;
    assert(CrSm8450ClockResource((ClockControllerType)C,&Physical,&Size)==CR_SUCCESS);
    Mappings[C]=(CR_CLOCK_MAPPING){.Address=0x30000000U+C*0x1000000U,.Size=Size};
  }
  assert(CrSm8450ClockBind(Core,Mappings,CC_TYPE_MAX)==CR_SUCCESS);
  CrLockInit(&Core->Lock);Core->LockInitialized=TRUE;Active=Core;
  ClockNode *Master,*Aggre,*Source;
  assert(GetClockNode(Core,"gcc_usb30_prim_master_clk",NULL,NULL,&Master)==CR_SUCCESS);
  assert(GetClockNode(Core,"gcc_aggre_usb3_prim_axi_clk",NULL,NULL,&Aggre)==CR_SUCCESS);
  Source=Master->Parents[0];
  assert(Source==Aggre->Parents[0]);
  CR_CLOCK_VOTER Voter={0};CR_CLOCK_VOTE_ENTRY Entries[8];UINT32 Owners[2];
  CR_CLOCK_VOTE_VALUE On={.RateHz=200000000,.Present=TRUE,.Enabled=TRUE};
  assert(CrClockVotesInit(&Voter,Core,Entries,8)==CR_SUCCESS);
  UINTN W=Writes,R=Reads;
  assert(CrClockVotesValidate(&Voter,Master,200000000)==CR_SUCCESS);
  assert(CrClockVotesValidate(&Voter,Master,199999999)==CR_UNSUPPORTED && W==Writes && R==Reads);
  assert(CrClockVotesSet(&Voter,&Owners[0],Master,&On)==CR_SUCCESS);
  assert(CrClockVotesSet(&Voter,&Owners[1],Aggre,&On)==CR_SUCCESS && Source->ReferenceCount==2);
  assert(CrClockVotesReleaseOwner(&Voter,&Owners[0])==CR_SUCCESS && Source->ReferenceCount==1 && Aggre->ReferenceCount==1);
  assert(CrClockVotesDrain(&Voter)==CR_SUCCESS && !Source->ReferenceCount);
  assert(CrClockVotesUninit(&Voter)==CR_SUCCESS);
  assert(ClockEnable(Core,Master,200000000,TRUE)==CR_SUCCESS);
  assert(ClockEnable(Core,Master,200000000,TRUE)==CR_SUCCESS);
  assert(ClockEnable(Core,Aggre,200000000,TRUE)==CR_SUCCESS);
  assert(Master->ReferenceCount==2 && Source->ReferenceCount==2);
  AutoClock=FALSE; Master->HwcgRegister=0;
  assert(CrSm8450ClockRelease(Core)==CR_TIMEOUT);
  assert(Master->ReferenceCount==1 && Source->ReferenceCount!=0);
  AutoClock=TRUE;
  assert(CrSm8450ClockRelease(Core)==CR_SUCCESS);
  SM8450_CLOCK_STATE *State=Storage;
  for (UINTN I=0;I<State->NodeCount;++I) assert(State->Nodes[I].ReferenceCount==0);
  ClockControllerType Bad=(ClockControllerType)-1;UINT32 Id=0;ClockNode *Node=Master;
  assert(GetClockNode(Core,NULL,&Bad,&Id,&Node)==CR_INVALID_PARAMETER && Node==NULL);
  free(Storage);Active=NULL;
}
static void ParentRetryOwnership(void) {
  Reset();
  ClockNode Parent={.Type=CLOCK_NODE_TYPE_BRANCH_2,.Name="parent",.ParentController=&Controller,
    .EnableRegister=0x100,.EnableMsk=BIT(0),.HaltRegister=0x104,.HaltCheckFlag=CLOCK_NODE_BRANCH_HALT};
  ClockNode Shared={.Type=CLOCK_NODE_TYPE_MUX,.Name="shared",.ParentController=&Controller};
  ClockNode *Parents[]={&Parent,&Shared};
  ClockNode Leaf={.Type=CLOCK_NODE_TYPE_BRANCH_2,.Name="leaf",.ParentController=&Controller,
    .ParentCount=2,.Parents=Parents,.EnableRegister=0x120,.EnableMsk=BIT(0),
    .HaltRegister=0x124,.HaltCheckFlag=CLOCK_NODE_BRANCH_HALT_SKIP};
  ClockNode *Nodes[]={&Parent,&Shared,&Leaf};Controller.Clks=Nodes;Controller.ClkCount=3;Active=&Context;
  assert(ClockEnable(&Context,&Shared,19200000,TRUE)==CR_SUCCESS); // Foreign owner's reference.
  assert(ClockEnable(&Context,&Leaf,19200000,TRUE)==CR_SUCCESS);
  assert(Parent.ReferenceCount==1 && Shared.ReferenceCount==2 && Leaf.ReferenceCount==1);
  BlockDisable=TEST_BASE+Parent.EnableRegister;
  assert(ClockEnable(&Context,&Leaf,19200000,FALSE)==CR_TIMEOUT);
  assert(Parent.ReferenceCount==1 && Shared.ReferenceCount==1 && Leaf.ReferenceCount==1);
  BlockDisable=0;
  assert(ClockEnable(&Context,&Leaf,19200000,FALSE)==CR_SUCCESS);
  assert(Parent.ReferenceCount==0 && Shared.ReferenceCount==1 && Leaf.ReferenceCount==0);
  // An incomplete final release must not admit a new reference to an off leaf.
  assert(ClockEnable(&Context,&Leaf,19200000,TRUE)==CR_SUCCESS);BlockDisable=TEST_BASE+Parent.EnableRegister;
  assert(ClockEnable(&Context,&Leaf,19200000,FALSE)==CR_TIMEOUT);UINTN W=Writes;
  assert(ClockEnable(&Context,&Leaf,19200000,TRUE)==CR_NOT_READY && Writes==W && Leaf.ReferenceCount==1);
  BlockDisable=0;assert(ClockEnable(&Context,&Leaf,19200000,FALSE)==CR_SUCCESS && Shared.ReferenceCount==1);
  // A failed cold enable can also have a failed parent rollback. Its node must
  // retain the exact parent reference so Disable can complete that cleanup.
  Leaf.HaltCheckFlag=CLOCK_NODE_BRANCH_HALT;
  BlockEnable=TEST_BASE+Leaf.EnableRegister;BlockDisable=TEST_BASE+Parent.EnableRegister;
  BOOLEAN Owned = FALSE;
  assert(ClockAcquire(&Context,&Leaf,19200000,&Owned)==CR_TIMEOUT && Owned);
  assert(Leaf.ReferenceCount==1 && Leaf.ReleasePending && Parent.ReferenceCount==1 && Shared.ReferenceCount==1);
  W=Writes;assert(ClockAcquire(&Context,&Leaf,19200000,&Owned)==CR_NOT_READY && !Owned && Writes==W);
  BlockEnable=BlockDisable=0;
  assert(ClockEnable(&Context,&Leaf,19200000,FALSE)==CR_SUCCESS && !Leaf.ReleasePending && Parent.ReferenceCount==0 && Shared.ReferenceCount==1);
  // The failing parent can retain its own cleanup reference before its child
  // ever writes a gate. The ancestor owns that acquired prefix, not the later
  // unacquired shared parent, and retry skips the untouched child gate.
  BlockEnable=BlockDisable=TEST_BASE+Parent.EnableRegister;
  assert(ClockEnable(&Context,&Leaf,19200000,TRUE)==CR_TIMEOUT);
  assert(Leaf.ReferenceCount==1 && Leaf.ReleasePending && Leaf.ParentsHeld==1 && !Leaf.GateHeld &&
         Parent.ReferenceCount==1 && Parent.ReleasePending && Shared.ReferenceCount==1);
  BlockEnable=BlockDisable=0;
  assert(ClockEnable(&Context,&Leaf,19200000,FALSE)==CR_SUCCESS && !Leaf.ReleasePending &&
         !Leaf.ParentsHeld && !Parent.ReferenceCount && !Parent.ReleasePending && Shared.ReferenceCount==1);
  assert(ClockEnable(&Context,&Shared,19200000,FALSE)==CR_SUCCESS);
  Controller.Clks=NULL;Controller.ClkCount=0;Active=NULL;
}
static void AbsoluteVotes(void) {
  Reset();Controller.MemMapped=TRUE;
  ClockNode Parent={.Type=CLOCK_NODE_TYPE_BRANCH_2,.Name="parent",.ParentController=&Controller,
    .EnableRegister=0x100,.EnableMsk=BIT(0),.HaltRegister=0x104,.HaltCheckFlag=CLOCK_NODE_BRANCH_HALT};
  ClockNode Shared={.Type=CLOCK_NODE_TYPE_MUX,.Name="shared",.ParentController=&Controller,
    .MuxRegister=0x180,.MuxMask=3,.MuxRefValue=2};
  ClockNode *Parents[]={&Parent,&Shared};
  ClockNode Leaf={.Type=CLOCK_NODE_TYPE_BRANCH_2,.Name="leaf",.ParentController=&Controller,
    .ParentCount=2,.Parents=Parents,.EnableRegister=0x120,.EnableMsk=BIT(0),
    .HaltRegister=0x124,.HaltCheckFlag=CLOCK_NODE_BRANCH_HALT};
  ClockNode *Nodes[]={&Parent,&Shared,&Leaf};Controller.Clks=Nodes;Controller.ClkCount=3;Active=&Context;
  UINT32 Owners[4]={0};CR_CLOCK_VOTER Voter={0};CR_CLOCK_VOTE_ENTRY Entries[3];
  CR_CLOCK_VOTE_VALUE On={.RateHz=19200000,.Present=TRUE,.Enabled=TRUE},Off=On,Absent={0},Value;
  BOOLEAN Pending;Off.Enabled=FALSE;
  assert(CrClockVotesInit(&Voter,&Context,(CR_CLOCK_VOTE_ENTRY *)((UINT8 *)Entries+1),3)==CR_INVALID_PARAMETER);
  assert(CrClockVotesInit(&Voter,&Context,Entries,3)==CR_SUCCESS);
  assert(CrClockVotesInit(&Voter,&Context,Entries,3)==CR_BUSY);
  UINTN W=Writes,R=Reads;
  assert(CrClockVotesValidate(&Voter,&Leaf,On.RateHz)==CR_SUCCESS && W==Writes && R==Reads);
  assert(CrClockVotesGet(&Voter,&Owners[0],&Leaf,&Value,&Pending)==CR_SUCCESS && !Value.Present && !Pending);
  assert(CrClockVotesSet(&Voter,&Owners[0],&Leaf,&On)==CR_SUCCESS);
  assert(Leaf.ReferenceCount==1 && Parent.ReferenceCount==1 && Shared.ReferenceCount==1);
  W=Writes;assert(CrClockVotesSet(&Voter,&Owners[0],&Leaf,&On)==CR_SUCCESS && Writes==W && Leaf.ReferenceCount==1);
  Parent.ReleasePending=TRUE;
  assert(CrClockVotesGet(&Voter,&Owners[0],&Leaf,&Value,&Pending)==CR_SUCCESS && Pending);
  assert(CrClockVotesSet(&Voter,&Owners[0],&Leaf,&On)==CR_NOT_READY && Writes==W && Leaf.ReferenceCount==1);
  Parent.ReleasePending=FALSE;
  assert(CrClockVotesSet(&Voter,&Owners[1],&Leaf,&On)==CR_SUCCESS && Leaf.ReferenceCount==2 && Parent.ReferenceCount==1);
  assert(CrClockVotesSet(&Voter,&Owners[2],&Leaf,&Off)==CR_SUCCESS && Leaf.ReferenceCount==2 && Writes==W);
  assert(CrClockVotesSet(&Voter,&Owners[3],&Leaf,&On)==CR_OUT_OF_RESOURCES && Leaf.ReferenceCount==2 && Writes==W);
  assert(CrClockVotesSet(&Voter,&Owners[3],&Leaf,&Absent)==CR_SUCCESS && Leaf.ReferenceCount==2);
  CR_CLOCK_VOTE_VALUE Other=On;Other.RateHz=24000000;
  assert(CrClockVotesSet(&Voter,&Owners[0],&Leaf,&Other)==CR_BUSY && Leaf.ReferenceCount==1);
  assert(CrClockVotesGet(&Voter,&Owners[0],&Leaf,&Value,&Pending)==CR_SUCCESS && Value.Enabled && Value.RateHz==On.RateHz && Pending);
  assert(CrClockVotesSet(&Voter,&Owners[0],&Leaf,&On)==CR_SUCCESS && Leaf.ReferenceCount==2);
  assert(CrClockVotesReleaseOwner(&Voter,&Owners[0])==CR_SUCCESS && Leaf.ReferenceCount==1 && Parent.ReferenceCount==1);
  assert(CrClockVotesUninit(&Voter)==CR_BUSY);
  assert(CrClockVotesDrain(&Voter)==CR_SUCCESS && !Leaf.ReferenceCount && !Parent.ReferenceCount && !Shared.ReferenceCount);

  // Retrying a failed final parent withdrawal preserves a foreign direct vote.
  assert(ClockEnable(&Context,&Shared,On.RateHz,TRUE)==CR_SUCCESS);
  assert(CrClockVotesSet(&Voter,&Owners[0],&Leaf,&On)==CR_SUCCESS);
  BlockDisable=TEST_BASE+Parent.EnableRegister;
  assert(CrClockVotesSet(&Voter,&Owners[0],&Leaf,&Absent)==CR_TIMEOUT && Shared.ReferenceCount==1);
  assert(CrClockVotesGet(&Voter,&Owners[0],&Leaf,&Value,&Pending)==CR_SUCCESS && Pending && Value.Enabled);
  assert(CrClockVotesUninit(&Voter)==CR_BUSY);
  BlockDisable=0;
  assert(CrClockVotesSet(&Voter,&Owners[0],&Leaf,&Absent)==CR_SUCCESS && Shared.ReferenceCount==1 && !Leaf.ReferenceCount);
  BlockEnable=TEST_BASE+Leaf.EnableRegister;BlockDisable=TEST_BASE+Parent.EnableRegister;
  assert(CrClockVotesSet(&Voter,&Owners[0],&Leaf,&On)==CR_TIMEOUT && Leaf.ReferenceCount==1);
  assert(CrClockVotesGet(&Voter,&Owners[0],&Leaf,&Value,&Pending)==CR_SUCCESS && !Value.Present && Pending);
  assert(CrClockVotesDrain(&Voter)==CR_TIMEOUT);
  BlockEnable=BlockDisable=0;
  assert(CrClockVotesSet(&Voter,&Owners[0],&Leaf,&Absent)==CR_SUCCESS && !Leaf.ReferenceCount && Shared.ReferenceCount==1);
  assert(ClockEnable(&Context,&Shared,On.RateHz,FALSE)==CR_SUCCESS);

  // GDSC withdrawal is per owner and retains failed final restoration.
  CR_CLOCK_VOTE_VALUE Power={.Present=TRUE,.Enabled=TRUE};*Reg(TEST_BASE+0x40)=BIT(0);
  assert(CrClockVotesValidate(&Voter,&Domain,1)==CR_INVALID_PARAMETER);
  W=Writes;R=Reads;Domain.GdscFlags|=CLOCK_GDSC_FLAG_POLL_CFG_GDSC_REG;Controller.Size=0x47;
  assert(CrClockVotesValidate(&Voter,&Domain,0)==CR_INVALID_PARAMETER && W==Writes && R==Reads);
  Controller.Size=0x1000;Domain.GdscFlags&=~CLOCK_GDSC_FLAG_POLL_CFG_GDSC_REG;
  assert(CrClockVotesSet(&Voter,&Owners[0],&Domain,&Power)==CR_SUCCESS);
  assert(CrClockVotesSet(&Voter,&Owners[1],&Domain,&Power)==CR_SUCCESS && Domain.ReferenceCount==2);
  assert(CrClockVotesReleaseOwner(&Voter,&Owners[0])==CR_SUCCESS && Domain.ReferenceCount==1);
  AutoPower=FALSE;assert(CrClockVotesDrain(&Voter)==CR_TIMEOUT && Domain.ReferenceCount==1);
  AutoPower=TRUE;assert(CrClockVotesDrain(&Voter)==CR_SUCCESS && !Domain.ReferenceCount);
#ifdef _KERNEL_MODE
  TestIrql=DISPATCH_LEVEL;W=Writes;R=Reads;
  assert(CrClockVotesSet(&Voter,&Owners[0],&Leaf,&On)==CR_INVALID_PARAMETER);
  assert(CrClockVotesDrain(&Voter)==CR_INVALID_PARAMETER && W==Writes && R==Reads);
  TestIrql=PASSIVE_LEVEL;
#endif
  Parents[0]=&Leaf;W=Writes;R=Reads;
  assert(CrClockVotesValidate(&Voter,&Leaf,On.RateHz)==CR_INVALID_PARAMETER && W==Writes && R==Reads);
  Parents[0]=&Parent;
  // Prove permanent capacity before publication and retain it across absence.
  assert(CrClockVotesRequireReservation(&Voter,&Owners[0],&Leaf)==CR_NOT_FOUND);
  for (UINTN O=0;O<3;++O) {
    assert(CrClockVotesReserve(&Voter,&Owners[O],&Leaf,On.RateHz)==CR_SUCCESS);
    assert(CrClockVotesReserve(&Voter,&Owners[O],&Leaf,On.RateHz)==CR_SUCCESS);
  }
  assert(CrClockVotesReserve(&Voter,&Owners[3],&Domain,1)==CR_INVALID_PARAMETER);
  assert(CrClockVotesReserve(&Voter,&Owners[3],&Domain,0)==CR_OUT_OF_RESOURCES && W==Writes && R==Reads);
  assert(CrClockVotesUninit(&Voter)==CR_BUSY);
  assert(CrClockVotesSet(&Voter,&Owners[0],&Leaf,&On)==CR_SUCCESS);
  assert(CrClockVotesSet(&Voter,&Owners[0],&Leaf,&Absent)==CR_SUCCESS);
  assert(CrClockVotesRequireReservation(&Voter,&Owners[0],&Leaf)==CR_SUCCESS);
  W=Writes;R=Reads;
  assert(CrClockVotesReserve(&Voter,&Owners[3],&Leaf,On.RateHz)==CR_OUT_OF_RESOURCES && W==Writes && R==Reads);
  BlockEnable=TEST_BASE+Leaf.EnableRegister;BlockDisable=TEST_BASE+Parent.EnableRegister;
  assert(CrClockVotesSet(&Voter,&Owners[0],&Leaf,&On)==CR_TIMEOUT);
  assert(CrClockVotesReleaseOwner(&Voter,&Owners[0])==CR_TIMEOUT);
  assert(CrClockVotesRequireReservation(&Voter,&Owners[0],&Leaf)==CR_SUCCESS);
  BlockEnable=BlockDisable=0;
  assert(CrClockVotesSet(&Voter,&Owners[0],&Leaf,&Absent)==CR_SUCCESS);
  assert(CrClockVotesRequireReservation(&Voter,&Owners[0],&Leaf)==CR_SUCCESS);
  assert(CrClockVotesReleaseOwner(&Voter,&Owners[0])==CR_SUCCESS);
  assert(CrClockVotesRequireReservation(&Voter,&Owners[0],&Leaf)==CR_NOT_FOUND);
  assert(CrClockVotesReserve(&Voter,&Owners[3],&Domain,0)==CR_SUCCESS);
  assert(CrClockVotesDrain(&Voter)==CR_SUCCESS);
  assert(CrClockVotesUninit(&Voter)==CR_SUCCESS);
  Controller.Clks=NULL;Controller.ClkCount=0;Controller.MemMapped=FALSE;Active=NULL;
}
static CR_CLOCK_VOTER *JournalVoter;
static ClockNode *JournalLeaf;
static BOOLEAN JournalWriteFailure;
static ClockNode *JournalNode(CONST CR_PEP_CLOCK_BINDING *Binding) {
  assert(Binding->Controller==CC_TYPE_GCC);
  return Binding->Gdsc ? &Domain : JournalLeaf;
}
static CR_STATUS JournalValidate(VOID *Io,CONST CR_PEP_CLOCK_BINDING *Binding,UINT64 RateHz) {
  assert(Io==JournalVoter);return CrClockVotesValidate(Io,JournalNode(Binding),RateHz);
}
static CR_STATUS JournalGet(VOID *Io,VOID *Owner,CONST CR_PEP_CLOCK_BINDING *Binding,
                            CR_CLOCK_VOTE_VALUE *Value,BOOLEAN *Pending) {
  assert(Io==JournalVoter);
  CR_STATUS Status=CrClockVotesRequireReservation(Io,Owner,JournalNode(Binding));
  return CR_ERROR(Status) ? Status : CrClockVotesGet(Io,Owner,JournalNode(Binding),Value,Pending);
}
static CR_STATUS JournalSet(VOID *Io,VOID *Owner,CONST CR_PEP_CLOCK_BINDING *Binding,
                            CONST CR_CLOCK_VOTE_VALUE *Value) {
  assert(Io==JournalVoter);
  CR_STATUS Status=CrClockVotesSet(Io,Owner,JournalNode(Binding),Value);
  if (!CR_ERROR(Status) && JournalWriteFailure) {JournalWriteFailure=FALSE;return CR_DEVICE_ERROR;}
  return Status;
}
static void ClockJournal(void) {
  Reset();Controller.MemMapped=TRUE;
  ClockNode Parent={.Type=CLOCK_NODE_TYPE_BRANCH_2,.Name="parent",.ParentController=&Controller,
    .EnableRegister=0x100,.EnableMsk=BIT(0),.HaltRegister=0x104,.HaltCheckFlag=CLOCK_NODE_BRANCH_HALT};
  ClockNode *Parents[]={&Parent};
  ClockNode Leaf={.Type=CLOCK_NODE_TYPE_BRANCH_2,.Name="leaf",.ParentController=&Controller,
    .ParentCount=1,.Parents=Parents,.EnableRegister=0x120,.EnableMsk=BIT(0),
    .HaltRegister=0x124,.HaltCheckFlag=CLOCK_NODE_BRANCH_HALT};
  ClockNode *Nodes[]={&Parent,&Leaf};Controller.Clks=Nodes;Controller.ClkCount=2;Active=&Context;
  CR_CLOCK_VOTER Voter={0};CR_CLOCK_VOTE_ENTRY Votes[8];UINT32 Owners[2];
  assert(CrClockVotesInit(&Voter,&Context,Votes,8)==CR_SUCCESS);JournalVoter=&Voter;JournalLeaf=&Leaf;
  CONST CR_PEP_CLOCK_RATE Rates[]={{24000000,1,24000000},{19200000,1,19200000}};
  CR_PEP_CLOCK_BINDING Bindings[]={
    {.Name={"leaf",4},.HardwareName="leaf",.Controller=CC_TYPE_GCC,.DefaultRateHz=19200000,.Rates=Rates,.RateCount=2},
    {.Name={"domain",6},.HardwareName="test",.Controller=CC_TYPE_GCC,.Gdsc=TRUE,.AllowImplicitGdscMode=TRUE}
  };
  CR_PEP_CLOCK_IO Io={&Voter,JournalValidate,JournalGet,JournalSet};CR_PEP_CLOCK_PROVIDER Provider={0};
  CR_PEP_CLOCK_BINDING Duplicates[]={Bindings[0],Bindings[0]};UINTN W=Writes,R=Reads;
  assert(CrPepClockProviderInit(&Provider,&Io,Bindings,0)==CR_INVALID_PARAMETER);
  assert(CrPepClockProviderInit(&Provider,&Io,Duplicates,2)==CR_INVALID_PARAMETER);
  assert(CrPepClockProviderInit(&Provider,&Io,Bindings,2)==CR_SUCCESS && W==Writes && R==Reads);
  CR_PEP_JOURNAL_PROVIDER Backend;assert(CrPepClockProviderGetJournal(&Provider,&Backend)==CR_SUCCESS);
  CR_PEP_JOURNAL Journals[2]={0};CR_PEP_JOURNAL_ENTRY Tx[2][8],Owned[2][4];
  for(UINTN O=0;O<2;++O)
    assert(CrPepJournalInit(&Journals[O],&Owners[O],&Backend,Tx[O],8,Owned[O],4)==CR_SUCCESS);
  CR_PEP_RESOURCE_OP On={.Kind=CrPepClock,.Name={"leaf",4},.Arguments={1},.ArgumentCount=1};
  CR_PEP_RESOURCE_OP Off=On;Off.Arguments[0]=2;
  CR_PEP_RESOURCE_OP Rate=On;Rate.Arguments[0]=3;Rate.Arguments[1]=24000000;Rate.Arguments[2]=1;Rate.ArgumentCount=3;
  CR_PEP_RESOURCE_OP Power={.Kind=CrPepFootswitch,.Name={"domain",6},.Arguments={1,1},.ArgumentCount=2};
  CR_PEP_RESOURCE_OP Bad=On;Bad.Arguments[0]=0;
  assert(CrPepJournalValidate(&Journals[0],&Bad)==CR_UNSUPPORTED && W==Writes && R==Reads);
  Bad=Rate;Bad.Arguments[2]=9;assert(CrPepJournalValidate(&Journals[0],&Bad)==CR_UNSUPPORTED);
  assert(CrPepJournalValidate(&Journals[0],&On)==CR_NOT_FOUND);
  CONST CR_PEP_CLOCK_BINDING *Description=NULL;UINT64 DescribedRate;
  assert(CrPepClockProviderDescribe(&Provider,&Rate,&Description,&DescribedRate)==CR_SUCCESS &&
         Description==&Bindings[0] && DescribedRate==24000000 && W==Writes && R==Reads);
  assert(CrPepClockProviderDescribe(&Provider,&Bad,&Description,&DescribedRate)==CR_UNSUPPORTED && Description==NULL);
  for (UINTN O=0;O<2;++O) {
    assert(CrClockVotesReserve(&Voter,&Owners[O],&Leaf,19200000)==CR_SUCCESS);
    assert(CrClockVotesReserve(&Voter,&Owners[O],&Leaf,24000000)==CR_SUCCESS);
    assert(CrClockVotesReserve(&Voter,&Owners[O],&Domain,0)==CR_SUCCESS);
  }
  assert(W==Writes && R==Reads);
  Bad=Power;Bad.Arguments[1]=2;assert(CrPepJournalValidate(&Journals[0],&Bad)==CR_UNSUPPORTED);
  // Actual Kodiak USB has {name,One}; display uses {name,One,One}. Both
  // identify the same whole-domain gate only when the target qualifies it.
  Bad=Power;Bad.ArgumentCount=1;
  assert(CrPepJournalValidate(&Journals[0],&Bad)==CR_SUCCESS && W==Writes && R==Reads);
  Bindings[1].AllowImplicitGdscMode=FALSE;
  assert(CrPepJournalValidate(&Journals[0],&Bad)==CR_UNSUPPORTED);
  Bindings[1].AllowImplicitGdscMode=TRUE;
  Power=Bad;
  Bad=On;Bad.Name.Data=NULL;assert(CrPepJournalValidate(&Journals[0],&Bad)==CR_INVALID_PARAMETER);
  assert(CrPepJournalBegin(&Journals[0])==CR_SUCCESS);
  assert(CrPepJournalApply(&Journals[0],&On)==CR_SUCCESS);
  assert(CrPepJournalApply(&Journals[0],&On)==CR_SUCCESS && Leaf.ReferenceCount==1);
  assert(CrPepJournalCommit(&Journals[0])==CR_SUCCESS);
  assert(CrPepJournalBegin(&Journals[1])==CR_SUCCESS);
  assert(CrPepJournalApply(&Journals[1],&On)==CR_SUCCESS && Leaf.ReferenceCount==2);
  assert(CrPepJournalCommit(&Journals[1])==CR_SUCCESS);
  assert(CrPepJournalClose(&Journals[0])==CR_SUCCESS && Leaf.ReferenceCount==1);
  assert(CrPepJournalClose(&Journals[1])==CR_SUCCESS && !Leaf.ReferenceCount);
  assert(CrPepJournalValidate(&Journals[0],&On)==CR_SUCCESS);

  // A mode-specific rate tuple sets the disabled demand before CLOCK1. No
  // hidden unit conversion occurs, and repeated clock ops share one binding.
  assert(CrPepJournalBegin(&Journals[0])==CR_SUCCESS);
  assert(CrPepJournalApply(&Journals[0],&Rate)==CR_SUCCESS && !Leaf.ReferenceCount);
  assert(CrPepJournalApply(&Journals[0],&On)==CR_SUCCESS && Leaf.ActiveRateHz==24000000 && Leaf.ReferenceCount==1);
  assert(CrPepJournalApply(&Journals[0],&Off)==CR_SUCCESS && !Leaf.ReferenceCount);
  assert(CrPepJournalRollback(&Journals[0])==CR_SUCCESS && !Leaf.ReferenceCount && !Journals[0].OwnedCount);

  // Late failure after a real successful hardware write is fully undone.
  *Reg(TEST_BASE+0x40)=BIT(0);
  assert(CrPepJournalBegin(&Journals[0])==CR_SUCCESS);
  assert(CrPepJournalApply(&Journals[0],&On)==CR_SUCCESS);
  assert(CrPepJournalApply(&Journals[0],&Power)==CR_SUCCESS);
  JournalWriteFailure=TRUE;
  assert(CrPepJournalApply(&Journals[0],&Rate)==CR_DEVICE_ERROR && Leaf.ActiveRateHz==24000000);
  AutoPower=FALSE;
  assert(CrPepJournalRollback(&Journals[0])==CR_TIMEOUT && Journals[0].TransactionCount==2 && Leaf.ReferenceCount==1);
  assert(CrPepJournalBegin(&Journals[0])==CR_BUSY);
  AutoPower=TRUE;
  assert(CrPepJournalRollback(&Journals[0])==CR_SUCCESS && !Leaf.ReferenceCount && !Domain.ReferenceCount);

  assert(CrPepJournalBegin(&Journals[0])==CR_SUCCESS);
  assert(CrPepJournalApply(&Journals[0],&On)==CR_SUCCESS);
  assert(CrPepJournalApply(&Journals[0],&Power)==CR_SUCCESS);
  assert(CrPepJournalCommit(&Journals[0])==CR_SUCCESS);
  BlockDisable=TEST_BASE+Parent.EnableRegister;
  assert(CrPepJournalClose(&Journals[0])==CR_TIMEOUT && Journals[0].OwnedCount==1 && Leaf.ReferenceCount==1);
  assert(CrPepJournalValidate(&Journals[1],&On)==CR_NOT_READY);
  BlockDisable=0;assert(CrPepJournalClose(&Journals[0])==CR_SUCCESS && !Leaf.ReferenceCount);
  for(UINTN O=0;O<2;++O) assert(CrPepJournalUninit(&Journals[O])==CR_SUCCESS);
  assert(CrPepClockProviderUninit(&Provider)==CR_SUCCESS);
  assert(CrClockVotesUninit(&Voter)==CR_BUSY);
  assert(CrClockVotesDrain(&Voter)==CR_SUCCESS);
  assert(CrClockVotesUninit(&Voter)==CR_SUCCESS);
  JournalVoter=NULL;JournalLeaf=NULL;Controller.Clks=NULL;Controller.ClkCount=0;Controller.MemMapped=FALSE;Active=NULL;
}
static void ConfigurationOwnership(void) {
  Reset();
  VOID *Storage=calloc(1,CrSm8450ClockStateSize()); assert(Storage!=NULL);
  ClockDriverContext *Core=NULL;
  assert(CrSm8450ClockCreate(Storage,CrSm8450ClockStateSize(),&Core)==CR_SUCCESS);
  CR_CLOCK_MAPPING Mappings[CC_TYPE_MAX];
  for (UINTN C=0;C<CC_TYPE_MAX;++C) {
    UINT64 Physical;UINT32 Size;
    assert(CrSm8450ClockResource((ClockControllerType)C,&Physical,&Size)==CR_SUCCESS);
    /* A translated Windows mapping must not be truncated to UINT32. */
    Mappings[C]=(CR_CLOCK_MAPPING){.Address=(UINTN)0x12300000000ULL+C*0x1000000U,.Size=Size};
  }
  assert(CrSm8450ClockBind(Core,Mappings,CC_TYPE_MAX)==CR_SUCCESS);
  CrLockInit(&Core->Lock);Core->LockInitialized=TRUE;Active=Core;
  ClockNode *Pixel,*Byte,*ByteIntf,*LinkIntf,*Utmi,*Aux;
  assert(GetClockNode(Core,"disp_cc_mdss_dptx0_pixel0_clk",NULL,NULL,&Pixel)==CR_SUCCESS);
  assert(GetClockNode(Core,"disp_cc_mdss_byte0_clk",NULL,NULL,&Byte)==CR_SUCCESS);
  assert(GetClockNode(Core,"disp_cc_mdss_byte0_intf_clk",NULL,NULL,&ByteIntf)==CR_SUCCESS);
  assert(ByteIntf->Parents[0]==Byte->Parents[0]);
  assert(GetClockNode(Core,"disp_cc_mdss_dptx0_link_intf_clk",NULL,NULL,&LinkIntf)==CR_SUCCESS);
  assert(GetClockNode(Core,"gcc_usb30_prim_mock_utmi_clk",NULL,NULL,&Utmi)==CR_SUCCESS);
  assert(GetClockNode(Core,"gcc_pcie_1_phy_aux_clk",NULL,NULL,&Aux)==CR_SUCCESS);
  ClockNode *Source=Pixel->Parents[0];
  UINTN Config=Source->ParentController->Address+Source->CmdRegister+4U;
  assert(Config>0xffffffffU);
  *Reg(Config)=0xa5a50013U;*Reg(Config+4)=0x11223344U;
  *Reg(Config+8)=0x33445566U;*Reg(Config+12)=0x55667788U;
  assert(ClockSelectExternalSource(Core,Pixel,1,3,2,5,6)==CR_SUCCESS);
  assert(*Reg(Config+4)==0x11220002U && *Reg(Config+8)==0x3344fffcU &&
         *Reg(Config+12)==0x5566fff9U);
  UINT32 Expected=(0xa5a50013U&~(UINT32)(31U|CLOCK_NODE_RCG_CMD_REGISTER_CFG_SRC_SEL_MSK|
                    CLOCK_NODE_RCG_CMD_REGISTER_CFG_MODE_MSK)) |
                   2U | (UINT32)SET_FIELD(1,CLOCK_NODE_RCG_CMD_REGISTER_CFG_SRC_SEL_MSK) |
                   CLOCK_NODE_RCG_CMD_REGISTER_CFG_MODE_DUAL_EDGE_MSK;
  assert(*Reg(Config)==Expected && Source->ExternalConfigPending);
  assert(ClockSetDivider(Core,Pixel,4)==CR_SUCCESS);
  Expected=(Expected&~31U)|3U; assert(*Reg(Config)==Expected);
  assert(ClockEnable(Core,Pixel,27000000,TRUE)==CR_SUCCESS);
  assert(*Reg(Config)==Expected && !Source->ExternalConfigPending);
  UINTN W=Writes,R=Reads;
  assert(ClockSelectExternalSource(Core,Pixel,1,2,0,0,0)==CR_BUSY && Writes==W && Reads==R);
  assert(ClockEnable(Core,Pixel,27000000,FALSE)==CR_SUCCESS);
  assert(ClockSelectExternalSource(Core,Pixel,2,2,0,0,0)==CR_SUCCESS);
  assert((*Reg(Config)&CLOCK_NODE_RCG_CMD_REGISTER_CFG_MODE_MSK)==0);
  W=Writes;R=Reads;
  assert(ClockSelectExternalSource(Core,Pixel,8,2,0,0,0)==CR_INVALID_PARAMETER);
  assert(ClockSelectExternalSource(Core,Pixel,1,0,0,0,0)==CR_INVALID_PARAMETER);
  assert(ClockSelectExternalSource(Core,Pixel,1,33,0,0,0)==CR_INVALID_PARAMETER);
  assert(ClockSelectExternalSource(Core,Pixel,1,2,6,5,6)==CR_INVALID_PARAMETER);
  assert(ClockSelectExternalSource(Core,Pixel,1,2,1,65536,6)==CR_INVALID_PARAMETER);
  assert(ClockSelectExternalSource(Core,Pixel,1,2,1,5,65536)==CR_INVALID_PARAMETER);
  assert(Writes==W && Reads==R);
  UINT32 Size=Source->ParentController->Size;
  Source->ParentController->Size=Source->CmdRegister+16U;
  assert(ClockSelectExternalSource(Core,Pixel,1,2,0,0,0)==CR_UNSUPPORTED);
  assert(ClockSetDivider(Core,Pixel,2)==CR_UNSUPPORTED && Writes==W && Reads==R);
  Source->ParentController->Size=Size;

  UINTN Divider=LinkIntf->ParentController->Address+LinkIntf->DividerRegister;
  *Reg(Divider)=0xf0000003U;W=Writes;
  assert(ClockSetDivider(Core,LinkIntf,4)==CR_SUCCESS && Writes==W);
  assert(ClockSetDivider(Core,LinkIntf,2)==CR_UNSUPPORTED && Writes==W);
  Divider=Utmi->ParentController->Address+Utmi->DividerRegister;
  *Reg(Divider)=0xf0000001U;R=Reads;
  assert(ClockSetDivider(Core,Utmi,2)==CR_UNSUPPORTED && Reads==R && Writes==W);
  assert(ClockSetDivider(Core,Utmi,1)==CR_UNSUPPORTED && Writes==W);
  *Reg(Divider)=0xf0000000U;
  assert(ClockSetDivider(Core,Utmi,1)==CR_SUCCESS && Writes==W);
  Divider=ByteIntf->ParentController->Address+ByteIntf->DividerRegister;
  *Reg(Divider)=0xf0000000U;
  assert(ClockSetDivider(Core,ByteIntf,4)==CR_SUCCESS && *Reg(Divider)==0xf0000003U);
  ByteIntf->ReferenceCount=1;W=Writes;R=Reads;
  assert(ClockSetDivider(Core,ByteIntf,2)==CR_BUSY && Reads==R && Writes==W);
  ByteIntf->ReferenceCount=0;

  ClockNode *Mux=Aux->Parents[0];
  Aux->ReferenceCount=2;Mux->ReferenceCount=1;W=Writes;R=Reads;
  assert(ClockSelectExternalSource(Core,Mux,2,0,0,0,0)==CR_BUSY && Reads==R && Writes==W);
  Aux->ReferenceCount=Mux->ReferenceCount=0;
  assert(ClockSelectExternalSource(Core,Mux,2,0,0,0,0)==CR_SUCCESS);

  UINT32 Old[4]; for (UINTN I=0;I<4;++I) Old[I]=*Reg(Config+4U*I);
  BOOLEAN Pending=Source->ExternalConfigPending;AutoClock=FALSE;
  assert(ClockSelectExternalSource(Core,Pixel,1,3,2,5,6)==CR_TIMEOUT);
  for (UINTN I=0;I<4;++I) assert(*Reg(Config+4U*I)==Old[I]);
  assert(Source->ReleasePending && Source->ExternalConfigPending==Pending);
  assert(CrSm8450ClockRelease(Core)==CR_TIMEOUT && Source->ReleasePending);
  assert(ClockEnable(Core,Pixel,27000000,TRUE)==CR_TIMEOUT && Pixel->ReferenceCount==0);
  AutoClock=TRUE;
  assert(CrSm8450ClockRelease(Core)==CR_SUCCESS && !Source->ReleasePending);
  assert(ClockSelectExternalSource(Core,Pixel,1,3,2,5,6)==CR_SUCCESS);
  assert(ClockEnable(Core,Pixel,27000000,TRUE)==CR_SUCCESS);
  assert(CrSm8450ClockRelease(Core)==CR_SUCCESS);
  free(Storage);Active=NULL;
}
int main(void) {
  PowerOwnership();MappingBounds();GraphRelease();ParentRetryOwnership();AbsoluteVotes();ClockJournal();ConfigurationOwnership();
  puts("Clock ownership: absolute per-owner clock/GDSC votes, foreign references, conflicting-rate recovery, retained parent/gate retry, mapping bounds, fractional clocks/dividers and graph teardown passed");
  return 0;
}
