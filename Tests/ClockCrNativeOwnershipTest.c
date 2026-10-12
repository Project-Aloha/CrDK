/** @file Native protocol/core ownership failure regressions. SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include <Uefi.h>

/* Execute the native frontend and portable implementations together. Only
 * firmware publication, MMIO, timing and lock primitives are host boundaries. */
#include "../Driver/ClockCrDxe/ClockCrDxe.c"
#include "../Library/ClockLib/clock.c"
#include "../Library/ClockLib/gdsc.c"
#include "../Library/ClockLib/clock_config.c"
#include "../Library/Sm8450ClockLib/Sm8450Clock.c"
#include "../Target/Sm8450/CrClockTarget.c"

EFI_GUID gEfiClockCrProtocolGuid = EFI_CLOCK_CR_PROTOCOL_GUID;
EFI_BOOT_SERVICES *gBS;
#define TEST_BASE 0x20000000U
typedef struct { UINTN Address; UINT32 Value; } TEST_REGISTER;
static TEST_REGISTER Registers[64];
static UINTN RegisterCount, Writes, Delays, SpinDepth;
static EFI_TPL CurrentTpl = TPL_APPLICATION;
static UINTN BlockEnable, BlockDisable;
static BOOLEAN AutoPower, AutoRoot;
static ClockController Controller;
static ClockDriverContext Context;

static UINT32 *Reg(UINTN Address)
{
  for (UINTN I=0; I<RegisterCount; ++I)
    if (Registers[I].Address==Address) return &Registers[I].Value;
  assert(RegisterCount<ARRAY_SIZE(Registers));
  Registers[RegisterCount].Address=Address;
  Registers[RegisterCount].Value=0;
  return &Registers[RegisterCount++].Value;
}
UINT32 EFIAPI MmioRead32(UINTN Address) { return *Reg(Address); }
UINT32 EFIAPI MmioWrite32(UINTN Address, UINT32 Value)
{
  assert(SpinDepth!=0); ++Writes;
  UINT32 Old=*Reg(Address); *Reg(Address)=Value;
  if (Address==TEST_BASE+0x40) {
    *Reg(Address)=(Value&~BIT(31)) | (Old&BIT(31));
    if (AutoPower) {
      if (Value&BIT(0)) *Reg(Address)&=~BIT(31);
      else *Reg(Address)|=BIT(31);
    }
  }
  for (UINTN I=0; I<Controller.ClkCount; ++I) {
    ClockNode *Node=Controller.Clks[I];
    if (Node->Type==CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2 &&
        Address==TEST_BASE+Node->CmdRegister && AutoRoot) {
      *Reg(Address)&=~(BIT(0)|BIT(31));
      if ((Value&BIT(1))==0) *Reg(Address)|=BIT(31);
    }
    if ((Node->Type==CLOCK_NODE_TYPE_BRANCH || Node->Type==CLOCK_NODE_TYPE_BRANCH_2) &&
        Address==TEST_BASE+Node->EnableRegister) {
      UINT32 *Halt=Reg(TEST_BASE+Node->HaltRegister);
      if (Value&Node->EnableMsk) {
        if (Address==BlockEnable) *Halt|=BIT(31);
        else *Halt&=~BIT(31);
      } else {
        if (Address==BlockDisable) *Halt&=~BIT(31);
        else *Halt|=BIT(31);
      }
    }
  }
  return *Reg(Address);
}
UINTN EFIAPI MicroSecondDelay(UINTN Us) { Delays+=Us;return Us; }
VOID EFIAPI MemoryFence(VOID) {}
VOID EFIAPI DebugPrint(UINTN L, CONST CHAR8 *F, ...) { (void)L;(void)F; }
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return TRUE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN L) { (void)L;return TRUE; }
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A, CONST CHAR8 *B) { return strcmp(A,B); }
INTN EFIAPI AsciiStrnCmp(CONST CHAR8 *A, CONST CHAR8 *B, UINTN N) { return strncmp(A,B,N); }
VOID *EFIAPI SetMem(VOID *B, UINTN L, UINT8 V) { return memset(B,V,L); }
SPIN_LOCK *EFIAPI InitializeSpinLock(SPIN_LOCK *L) { *L=1;return L; }
SPIN_LOCK *EFIAPI AcquireSpinLock(SPIN_LOCK *L)
{ assert(*L==1 && SpinDepth==0);*L=2;++SpinDepth;return L; }
SPIN_LOCK *EFIAPI ReleaseSpinLock(SPIN_LOCK *L)
{ assert(*L==2 && SpinDepth==1);*L=1;--SpinDepth;return L; }
VOID EFIAPI EfiAcquireLock(EFI_LOCK *L)
{
  assert(L->Lock==EfiLockReleased && CurrentTpl<=L->Tpl && SpinDepth==0);
  L->OwnerTpl=CurrentTpl;CurrentTpl=L->Tpl;L->Lock=EfiLockAcquired;
}
VOID EFIAPI EfiReleaseLock(EFI_LOCK *L)
{
  assert(L->Lock==EfiLockAcquired && CurrentTpl==L->Tpl && SpinDepth==0);
  CurrentTpl=L->OwnerTpl;L->Lock=EfiLockReleased;
}
CR_STATUS CrDalClockResolveResource(CONST CHAR8 *C, CONST CHAR8 *N,
  CR_DAL_CLOCK_RESOURCE_KIND K, CONST CHAR8 **Native, UINT32 *Flags)
{ return CrTargetClockResolveResource(C,N,K,Native,Flags); }
CR_STATUS CrDalClockReset(CONST CHAR8 *C, CONST CHAR8 *N, BOOLEAN A)
{ return CrSm8450ClockReset(&Context,C,N,A); }

static void Reset(ClockNode **Nodes, UINT16 Count, ClockNode **Domains, UINT16 DomainCount)
{
  assert(CurrentTpl==TPL_APPLICATION && SpinDepth==0);
  RegisterCount=Writes=Delays=0;BlockEnable=BlockDisable=0;
  AutoPower=AutoRoot=TRUE;
  Controller=(ClockController){.Address=TEST_BASE,.Size=0x1000,
    .Clks=Nodes,.ClkCount=Count,.Gdscs=Domains,.GdscCount=DomainCount};
  Context=(ClockDriverContext){.ClockControllerCount=1,.ClockControllers=&Controller};
  CrLockInit(&Context.Lock);Context.LockInitialized=TRUE;
  for (UINTN I=0; I<Count; ++I) Nodes[I]->ParentController=&Controller;
  for (UINTN I=0; I<DomainCount; ++I) Domains[I]->ParentController=&Controller;
  memset(mClockVotes,0,sizeof(mClockVotes));
  memset(mPowerDomainVotes,0,sizeof(mPowerDomainVotes));
  memset(mCraneRates,0,sizeof(mCraneRates));
  mVoteLock.Tpl=TPL_NOTIFY;mVoteLock.Lock=EfiLockReleased;
  mClockContext=&Context;mNativeClock=&mCraneNativeClock;
}
static void Idle(void)
{ assert(CurrentTpl==TPL_APPLICATION && SpinDepth==0 && mVoteLock.Lock==EfiLockReleased); }
static ClockNode Branch(CONST CHAR8 *Name, UINT32 Offset)
{
  return (ClockNode){.Type=CLOCK_NODE_TYPE_BRANCH_2,.Name=Name,
    .EnableRegister=Offset,.EnableMsk=BIT(0),.HaltRegister=Offset+4,
    .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT};
}

static void ForeignRate(void)
{
  ClockRcgFreqTable Rates[]={
    CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(0,0,19200000U,1,0,0),
    CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(0,1,38400000U,1,0,0)};
  ClockNode Root={.Type=CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2,.Name="review_rcg",
    .CmdRegister=0x100,.HIDWidth=5,.FreqCount=2,.FreqTable=Rates};
  ClockNode *Nodes[]={&Root};Reset(Nodes,ARRAY_SIZE(Nodes),NULL,0);
  BOOLEAN Owned=FALSE;
  assert(ClockAcquire(&Context,&Root,19200000,&Owned)==CR_SUCCESS && Owned);
  UINTN W=Writes;UINT32 Cfg=*Reg(TEST_BASE+0x104);
  assert(ProtocolAcquireClock(&mClockCrProtocol,"gcc",Root.Name,38400000,&Owned)==EFI_ACCESS_DENIED);
  assert(!Owned && Root.ReferenceCount==1 && Root.ActiveRateHz==19200000);
  assert(Writes==W && *Reg(TEST_BASE+0x104)==Cfg);
  assert(ProtocolAcquireClock(&mClockCrProtocol,"gcc",Root.Name,19200000,&Owned)==EFI_SUCCESS && Owned);
  assert(Root.ReferenceCount==2 && Writes==W);
  assert(ProtocolSetClock(&mClockCrProtocol,"gcc",Root.Name,0,FALSE)==EFI_SUCCESS);
  assert(Root.ReferenceCount==1 && Writes==W);
  assert(ClockEnable(&Context,&Root,0,FALSE)==CR_SUCCESS && Root.ReferenceCount==0);
  Idle();
}

static void ForeignPending(void)
{
  ClockNode Parent=Branch("parent",0x100),Leaf=Branch("leaf",0x120);
  ClockNode *Parents[]={&Parent};Leaf.Parents=Parents;Leaf.ParentCount=1;
  ClockNode *Nodes[]={&Parent,&Leaf};Reset(Nodes,ARRAY_SIZE(Nodes),NULL,0);
  BlockEnable=TEST_BASE+Leaf.EnableRegister;BlockDisable=TEST_BASE+Parent.EnableRegister;
  BOOLEAN Owned=FALSE;
  assert(ClockAcquire(&Context,&Leaf,0,&Owned)==CR_TIMEOUT && Owned);
  assert(Leaf.ReferenceCount==1 && Leaf.ReleasePending && !Leaf.GateHeld && Leaf.ParentsHeld==1);
  UINTN W=Writes;
  assert(ProtocolAcquireClock(&mClockCrProtocol,"gcc",Leaf.Name,0,&Owned)==EFI_NOT_READY && !Owned);
  assert(ProtocolSetClock(&mClockCrProtocol,"gcc",Leaf.Name,0,FALSE)==EFI_SUCCESS);
  assert(Writes==W && Leaf.ReferenceCount==1 && Parent.ReferenceCount==1);
  BlockDisable=0;
  assert(ClockEnable(&Context,&Leaf,0,FALSE)==CR_SUCCESS);
  assert(!Leaf.ReferenceCount && !Leaf.ParentsHeld && !Parent.ReferenceCount);
  Idle();
}

static void ForeignExternalSource(void)
{
  for (UINTN Case=0; Case<3; ++Case) {
    ClockNode Mux={.Type=CLOCK_NODE_TYPE_MUX,.Name="foreign_pipe_source",
      .MuxRegister=0x180,.MuxMask=BIT(0),.MuxPhyValue=0,.MuxRefValue=BIT(0)};
    ClockNode Leaf=Branch("gcc_pcie_0_pipe_clk",0x120);
    ClockNode *Parents[]={&Mux};Leaf.Parents=Parents;Leaf.ParentCount=1;
    ClockNode *Nodes[]={&Mux,&Leaf};Reset(Nodes,ARRAY_SIZE(Nodes),NULL,0);
    *Reg(TEST_BASE+Mux.MuxRegister)=Mux.MuxRefValue;
    BOOLEAN Owned=FALSE;
    if (Case==0) {
      assert(ClockAcquire(&Context,&Leaf,0,&Owned)==CR_SUCCESS && Owned);
    } else if (Case==1) {
      assert(ClockAcquire(&Context,&Mux,0,&Owned)==CR_SUCCESS && Owned);
    } else {
      BlockEnable=BlockDisable=TEST_BASE+Leaf.EnableRegister;
      assert(ClockAcquire(&Context,&Leaf,0,&Owned)==CR_TIMEOUT && Owned);
      assert(Leaf.ReleasePending && Leaf.ReferenceCount==1);
    }
    UINTN W=Writes;UINT32 Selector=*Reg(TEST_BASE+Mux.MuxRegister);
    UINT32 LeafRefs=Leaf.ReferenceCount,MuxRefs=Mux.ReferenceCount;
    /* The automatic PHY selector belongs to this acquire. A fresh native
     * protocol owner must neither reconfigure a foreign leaf/mux nor clear
     * its exact retained cleanup reference. */
    assert(EFI_ERROR(ProtocolAcquireClock(&mClockCrProtocol,"gcc",Leaf.Name,0,&Owned)) && !Owned);
    assert(Writes==W && *Reg(TEST_BASE+Mux.MuxRegister)==Selector);
    assert(Leaf.ReferenceCount==LeafRefs && Mux.ReferenceCount==MuxRefs);
    assert(ProtocolSetClock(&mClockCrProtocol,"gcc",Leaf.Name,0,FALSE)==EFI_SUCCESS && Writes==W);
    BlockDisable=0;
    assert(ClockEnable(&Context,Case==1?&Mux:&Leaf,0,FALSE)==CR_SUCCESS);
    assert(!Leaf.ReferenceCount && !Mux.ReferenceCount);
    Idle();
  }
}

static void NativeCleanup(BOOLEAN FirstEnableFailure)
{
  ClockNode Parent={.Type=CLOCK_NODE_TYPE_MUX,.Name="foreign_mux"};
  ClockNode Leaf=Branch("leaf",0x120);ClockNode *Parents[]={&Parent};
  Leaf.Parents=Parents;Leaf.ParentCount=1;
  ClockNode *Nodes[]={&Parent,&Leaf};Reset(Nodes,ARRAY_SIZE(Nodes),NULL,0);
  BOOLEAN Owned=FALSE;
  assert(ClockAcquire(&Context,&Parent,0,&Owned)==CR_SUCCESS && Owned);
  if (FirstEnableFailure) BlockEnable=BlockDisable=TEST_BASE+Leaf.EnableRegister;
  EFI_STATUS S=ProtocolAcquireClock(&mClockCrProtocol,"gcc",Leaf.Name,0,&Owned);
  assert(S==(FirstEnableFailure?EFI_TIMEOUT:EFI_SUCCESS) && Owned);
  assert(Leaf.ReferenceCount==1 && Leaf.ParentsHeld==1 && Parent.ReferenceCount==2);
  BlockDisable=TEST_BASE+Leaf.EnableRegister;
  assert(ProtocolSetClock(&mClockCrProtocol,"gcc",Leaf.Name,0,FALSE)==EFI_TIMEOUT);
  assert(Leaf.ReleasePending && Leaf.ReferenceCount==1 && Parent.ReferenceCount==2);
  UINTN W=Writes;
  assert(ProtocolAcquireClock(&mClockCrProtocol,"gcc",Leaf.Name,0,&Owned)==EFI_TIMEOUT && !Owned);
  assert(Writes==W);
  BlockDisable=0;
  assert(ProtocolSetClock(&mClockCrProtocol,"gcc",Leaf.Name,0,FALSE)==EFI_SUCCESS);
  assert(!Leaf.ReferenceCount && !Leaf.ReleasePending && !Leaf.ParentsHeld && Parent.ReferenceCount==1);
  W=Writes;
  assert(ProtocolSetClock(&mClockCrProtocol,"gcc",Leaf.Name,0,FALSE)==EFI_SUCCESS && Writes==W);
  assert(ClockEnable(&Context,&Parent,0,FALSE)==CR_SUCCESS && !Parent.ReferenceCount);
  Idle();
}

static ClockNode Domain(BOOLEAN ConfigPoll)
{
  return (ClockNode){.Type=CLOCK_NODE_TYPE_GDSC,.Name="domain",.GdscRegister=0x40,
    .PwrStsFlag=CLOCK_NODE_GDSC_PWR_STS_RET_ON,
    .GdscFlags=CLOCK_GDSC_FLAG_HW_CTL | (ConfigPoll?CLOCK_GDSC_FLAG_POLL_CFG_GDSC_REG:0)};
}
static void GdscCleanup(void)
{
  ClockNode Power=Domain(TRUE);ClockNode *Domains[]={&Power};Reset(NULL,0,Domains,1);
  *Reg(TEST_BASE+0x40)=BIT(0);AutoPower=FALSE;
  BOOLEAN Owned=FALSE;
  assert(ProtocolAcquireGdsc(&mClockCrProtocol,"gcc",Power.Name,&Owned)==EFI_TIMEOUT && Owned);
  assert(Power.ReferenceCount==1 && Power.ReleasePending);
  UINTN W=Writes;
  assert(ProtocolAcquireGdsc(&mClockCrProtocol,"gcc",Power.Name,&Owned)==EFI_TIMEOUT && !Owned);
  assert(Writes==W);
  assert(ProtocolSetGdsc(&mClockCrProtocol,"gcc",Power.Name,FALSE)==EFI_TIMEOUT);
  assert(Power.ReferenceCount==1 && Power.ReleasePending);
  *Reg(TEST_BASE+0x44)=BIT(15);
  assert(ProtocolSetGdsc(&mClockCrProtocol,"gcc",Power.Name,FALSE)==EFI_SUCCESS);
  assert(!Power.ReferenceCount && !Power.ReleasePending);
  Idle();
}
static void InheritedGdsc(void)
{
  ClockNode Power=Domain(FALSE);ClockNode *Domains[]={&Power};Reset(NULL,0,Domains,1);
  *Reg(TEST_BASE+0x40)=BIT(31)|BIT(1)|BIT(10);
  BOOLEAN Owned=FALSE;
  assert(ProtocolAcquireGdsc(&mClockCrProtocol,"gcc",Power.Name,&Owned)==EFI_SUCCESS && Owned);
  /* A subsequent consumer reset changes status without withdrawing the
   * inherited software request. Releasing our owner must preserve it. */
  *Reg(TEST_BASE+0x40)&=~BIT(31);AutoPower=FALSE;
  UINTN D=Delays;
  assert(ProtocolSetGdsc(&mClockCrProtocol,"gcc",Power.Name,FALSE)==EFI_SUCCESS);
  assert(!Power.ReferenceCount && !Power.ReleasePending && Delays==D);
  assert((*Reg(TEST_BASE+0x40)&(BIT(0)|BIT(1)|BIT(10)))==(BIT(1)|BIT(10)));
  Idle();
}
int main(void)
{
  ForeignRate();ForeignPending();ForeignExternalSource();
  NativeCleanup(TRUE);NativeCleanup(FALSE);
  GdscCleanup();InheritedGdsc();
  puts("ClockCr native/core: foreign ownership, failed acquire/release, retry and inherited GDSC passed");
  return 0;
}

/* Native OS boundary used by the typed external-clock lease adapter. */
EFI_TPL EFIAPI EfiGetCurrentTpl(VOID) { return CurrentTpl; }
