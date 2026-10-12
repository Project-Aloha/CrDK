/* Actual EFI frontend + ClockLib/SM8450 private display lease.
 * Firmware publication, MMIO, timing and TPL primitives are host boundaries.
 * SPDX-License-Identifier: MIT */
#define CR_CLOCK_EXTERNAL_LEASE_FIXTURE_ONLY
#include "ClockExternalLeaseTest.c"
#include "../Driver/ClockCrDxe/ClockCrDxe.c"
#include "../Target/Sm8450/CrClockTarget.c"
EFI_GUID gEfiClockCrProtocolGuid=EFI_CLOCK_CR_PROTOCOL_GUID;
EFI_GUID gEfiClockExternalCrProtocolGuid=EFI_CLOCK_EXTERNAL_CR_PROTOCOL_GUID;
EFI_BOOT_SERVICES *gBS;
INTN EFIAPI AsciiStrnCmp(CONST CHAR8 *A,CONST CHAR8 *B,UINTN N){return strncmp(A,B,N);}
static EFI_TPL CurrentTpl=TPL_APPLICATION;
static UINTN OuterLocks,ResetCalls;
EFI_TPL EFIAPI EfiGetCurrentTpl(VOID){return CurrentTpl;}
VOID EFIAPI EfiAcquireLock(EFI_LOCK *L){assert(L==&mVoteLock&&L->Lock==EfiLockReleased&&CurrentTpl<=L->Tpl);L->OwnerTpl=CurrentTpl;CurrentTpl=L->Tpl;L->Lock=EfiLockAcquired;++OuterLocks;}
VOID EFIAPI EfiReleaseLock(EFI_LOCK *L){assert(L==&mVoteLock&&L->Lock==EfiLockAcquired&&CurrentTpl==L->Tpl);CurrentTpl=L->OwnerTpl;L->Lock=EfiLockReleased;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN L){(void)L;return TRUE;}
CR_STATUS CrDalClockResolveResource(CONST CHAR8 *C,CONST CHAR8 *N,CR_DAL_CLOCK_RESOURCE_KIND K,CONST CHAR8 **Native,UINT32 *Flags){return CrTargetClockResolveResource(C,N,K,Native,Flags);}
CR_STATUS CrDalClockReset(CONST CHAR8 *C,CONST CHAR8 *N,BOOLEAN A){++ResetCalls;return CrSm8450ClockReset(mClockContext,C,N,A);}
static VOID *DxeFixture(VOID){
 assert(CurrentTpl==TPL_APPLICATION);VOID *S=Fixture(&mExternalLease);
 mClockContext=mExternalLease.External.Core;mNativeClock=&mCraneNativeClock;
 memset(mClockVotes,0,sizeof(mClockVotes));memset(mPowerDomainVotes,0,sizeof(mPowerDomainVotes));memset(mCraneRates,0,sizeof(mCraneRates));
 mVoteLock.Tpl=TPL_NOTIFY;mVoteLock.Lock=EfiLockReleased;return S;
}
static VOID DxeFinish(VOID *Storage,VOID *Owner){
 BOOLEAN Owned=FALSE;assert(mExternalProtocol.Reserve(&mExternalProtocol,Owner,&Owned)==EFI_SUCCESS&&Owned);
 Finish(&mExternalLease,Storage,Owner);
}
static VOID NoLegacyMutation(VOID){
 UINTN W=Writes,R=Reads,Resets=ResetCalls;CR_CLOCK_EXTERNAL *E=&mExternalLease.External;
 assert(mClockCrProtocol.SetClock(&mClockCrProtocol,"dispcc",E->Leaf[0]->Name,0,TRUE)==EFI_ACCESS_DENIED);
 assert(mClockCrProtocol.SetClock(&mClockCrProtocol,"dispcc",E->Access[0]->Name,0,FALSE)==EFI_ACCESS_DENIED);
 assert(mClockCrProtocol.SetGdsc(&mClockCrProtocol,"dispcc",E->Domain->Name,TRUE)==EFI_ACCESS_DENIED);
 assert(mClockCrProtocol.ConfigureExternalSource(&mClockCrProtocol,"dispcc",E->Source[0]->Name,1000,1,1,0,0,0)==EFI_ACCESS_DENIED);
 assert(mClockCrProtocol.SetDivider(&mClockCrProtocol,"dispcc",E->Leaf[2]->Name,2)==EFI_ACCESS_DENIED);
 /* The actual DSI parent is an RCG, so legacy SetParent rejects its type
  * before any mutation. Do not replace it with a fabricated MUX fixture. */
 assert(mClockCrProtocol.SetParent(&mClockCrProtocol,"dispcc",E->Leaf[0]->Name,1)==EFI_UNSUPPORTED);
 assert(mClockCrProtocol.SetReset(&mClockCrProtocol,"dispcc","unknown_display_reset",TRUE)==EFI_ACCESS_DENIED);
 assert(Writes==W&&Reads==R&&ResetCalls==Resets&&mVoteLock.Lock==EfiLockReleased&&CurrentTpl==TPL_APPLICATION);
}
static VOID DxeRetire(UINT32 Soc) {
 /* Actual EFI wrapper over an adapted SM8450 graph fixture. Separate
    ClockBootResourceTest composes the selected SM8250/SM8350 graphs. */
 VOID *Storage=DxeFixture();CR_CLOCK_EXTERNAL *E=&mExternalLease.External;
 ClockDriverContext *Core=E->Core;
 for(UINT32 I=0;I<2;++I) {
   ClockNode **L=I?E->SecondLeaf:E->Leaf,**S=I?E->SecondSource:E->Source;
   const UINT32 Gates[2][3]={{0x2028,0x202c,0x2004},{0x2030,0x2034,0x2008}};
   const UINT32 Sources[2][2]={{Soc==0x8250?0x2110:0x210c,Soc==0x8250?0x2098:0x2094},
     {Soc==0x8250?0x212c:0x2128,Soc==0x8250?0x20b0:0x20ac}};
   for(UINT32 J=0;J<3;++J)L[J]->EnableRegister=L[J]->HaltRegister=Gates[I][J];
   L[1]->DividerRegister=(Soc==0x8250?0x2128:0x2124)+I*0x1c;L[1]->DividerWidth=Soc==0x8250?2:4;
   for(UINT32 J=0;J<2;++J)S[J]->CmdRegister=Sources[I][J];
 }
 E->Domain->GdscRegister=0x3000;
 E->Access[2]->EnableRegister=E->Access[2]->HaltRegister=Soc==0x8250?0x2080:0x207c;
 memset(&mExternalLease,0,sizeof(mExternalLease));
 assert(CrClockExternalLeaseInitForSoc(&mExternalLease,Core,Soc)==CR_SUCCESS);
 E=&mExternalLease.External;DomainAddress=E->Domain->ParentController->Address+0x3000;
 *Reg(DomainAddress)=BIT(0)|BIT(10);int Owner,Foreign;BOOLEAN Owned;
 CurrentTpl=TPL_NOTIFY;
 assert(mExternalProtocol.Reserve(&mExternalProtocol,&Owner,&Owned)==EFI_SUCCESS&&Owned);
 CurrentTpl=TPL_APPLICATION;
 /* Actual owner-qualified BCR before functional programming. A register
    readback fault must retain the access lease and exact epoch. */
 UINTN ResetAddress=Core->ClockControllers[CC_TYPE_DISP_CC].Address+0x2000;
 *Reg(ResetAddress)=BIT(22);DropWriteAddress=ResetAddress;
 UINTN W0=Writes,R0=Reads;
 assert(mClockCrProtocol.SetReset(&mClockCrProtocol,"dispcc","disp_cc_mdss_core_bcr",TRUE)==EFI_ACCESS_DENIED&&Writes==W0&&Reads==R0);
 assert(mExternalProtocol.ResetCore(&mExternalProtocol,&Foreign,23,TRUE)==EFI_ACCESS_DENIED&&Writes==W0&&Reads==R0);
 assert(mExternalProtocol.ResetCore(&mExternalProtocol,&Owner,0,TRUE)==EFI_INVALID_PARAMETER&&Writes==W0&&Reads==R0);
 CurrentTpl=TPL_HIGH_LEVEL;
 assert(mExternalProtocol.ResetCore(&mExternalProtocol,&Owner,23,TRUE)==EFI_INVALID_PARAMETER);
 CurrentTpl=TPL_CALLBACK;
 assert(mExternalProtocol.ResetCore(&mExternalProtocol,&Owner,23,TRUE)==EFI_DEVICE_ERROR);
 assert(CurrentTpl==TPL_CALLBACK&&mVoteLock.Lock==EfiLockReleased&&E->CoreResetTouched&&!E->CoreResetComplete);
 assert(mExternalProtocol.ReleaseOwner(&mExternalProtocol,&Owner)==EFI_NOT_READY);CheckPrivate(&mExternalLease);
 W0=Writes;R0=Reads;
 assert(mExternalProtocol.ResetCore(&mExternalProtocol,&Owner,24,TRUE)==EFI_INVALID_PARAMETER&&Writes==W0&&Reads==R0);
 DropWriteAddress=0;
 assert(mExternalProtocol.ResetCore(&mExternalProtocol,&Owner,23,TRUE)==EFI_SUCCESS);
 assert(*Reg(ResetAddress)==(BIT(22)|BIT(0)));
 DropWriteAddress=ResetAddress;
 assert(mExternalProtocol.ResetCore(&mExternalProtocol,&Owner,23,FALSE)==EFI_DEVICE_ERROR);
 assert(mExternalProtocol.CheckCoreResetReleased(&mExternalProtocol,&Owner,23)==EFI_NOT_READY);
 DropWriteAddress=0;
 assert(mExternalProtocol.ResetCore(&mExternalProtocol,&Owner,23,FALSE)==EFI_SUCCESS);
 assert(*Reg(ResetAddress)==BIT(22));
 assert(mExternalProtocol.CheckCoreResetReleased(&mExternalProtocol,&Owner,23)==EFI_SUCCESS);
 CONST CHAR8 *Functional[]={"disp_cc_mdss_mdp_clk","disp_cc_mdss_mdp_lut_clk","disp_cc_mdss_vsync_clk",
   "disp_cc_mdss_esc0_clk","disp_cc_mdss_esc1_clk","disp_cc_mdss_mdp_clk_src",
   "disp_cc_mdss_vsync_clk_src","disp_cc_mdss_esc0_clk_src","disp_cc_mdss_esc1_clk_src"};
 CONST UINT32 Offsets[]={0x200c,0x201c,0x2024,0x2038,0x203c,
   Soc==0x8250?0x20c8:0x20c4,Soc==0x8250?0x20f8:0x20f4,
   Soc==0x8250?0x2148:0x2144,Soc==0x8250?0x2160:0x215c};
 ClockNode *Functions[9];
 for(UINT32 I=0;I<9;++I){
   assert(GetClockNode(Core,Functional[I],NULL,NULL,&Functions[I])==CR_SUCCESS);
   if(I<5)Functions[I]->EnableRegister=Functions[I]->HaltRegister=Offsets[I];
   else {Functions[I]->CmdRegister=Offsets[I];Functions[I]->MNDWidth=0;Functions[I]->HwClockCtrl=I==5;}
   *Reg(Functions[I]->ParentController->Address+Offsets[I])=BIT(22)|(I<5?BIT(0):BIT(1));
 }
 Functions[0]->ReferenceCount=1;W0=Writes;R0=Reads;
 assert(mExternalProtocol.RetireInheritedFunctional(&mExternalProtocol,&Owner,23)==EFI_ACCESS_DENIED&&Writes==W0&&Reads==R0+1);
 Functions[0]->ReferenceCount=0;
 /* A missing ordinary branch HALT remains a physical stop failure;
    aggregate functional ROOT_OFF alone is not local force ownership. */
 BlockDisable=Functions[0]->ParentController->Address+Offsets[0];
 EFI_STATUS RetireStatus=mExternalProtocol.RetireInheritedFunctional(&mExternalProtocol,&Owner,23);
 if(RetireStatus!=EFI_TIMEOUT)fprintf(stderr,"functional retirement status=%llx expected timeout\n",(unsigned long long)RetireStatus);
 assert(RetireStatus==EFI_TIMEOUT);
 assert(mExternalProtocol.ReleaseOwner(&mExternalProtocol,&Owner)==EFI_NOT_READY);CheckPrivate(&mExternalLease);
 BlockDisable=0;
 assert(mExternalProtocol.RetireInheritedFunctional(&mExternalProtocol,&Owner,23)==EFI_SUCCESS);
 for(UINT32 I=0;I<9;++I)assert((*Reg(Functions[I]->ParentController->Address+Offsets[I])&(BIT(22)|BIT(31)))==(BIT(22)|BIT(31)));
 /* Completed bootstrap operations never reset or stop a new pipeline. */
 Functions[0]->ReferenceCount=1;*Reg(Functions[0]->ParentController->Address+Offsets[0])|=BIT(0);W0=Writes;
 assert(mExternalProtocol.ResetCore(&mExternalProtocol,&Owner,23,TRUE)==EFI_SUCCESS&&Writes==W0);
 assert(mExternalProtocol.RetireInheritedFunctional(&mExternalProtocol,&Owner,23)==EFI_SUCCESS&&Writes==W0);
 Functions[0]->ReferenceCount=0;*Reg(Functions[0]->ParentController->Address+Offsets[0])&=~BIT(0);
 CurrentTpl=TPL_APPLICATION;
 UINTN W=Writes,R=Reads;
 assert(mExternalProtocol.RetireInheritedDsi(NULL,&Owner,23)==EFI_INVALID_PARAMETER&&Reads==R&&Writes==W);
 assert(mExternalProtocol.RetireInheritedDsi(&mExternalProtocol,&Foreign,23)==EFI_ACCESS_DENIED&&Reads==R&&Writes==W);
 CurrentTpl=TPL_HIGH_LEVEL;
 assert(mExternalProtocol.CheckInheritedDsiOff(&mExternalProtocol,&Owner,23)==EFI_INVALID_PARAMETER&&Reads==R&&Writes==W);
 CurrentTpl=TPL_APPLICATION;
 assert(mExternalProtocol.CheckInheritedDsiOff(&mExternalProtocol,&Owner,23)==EFI_NOT_READY&&Writes==W);
 for(UINT32 I=0;I<2;++I) {
   ClockNode **L=I?E->SecondLeaf:E->Leaf,**S=I?E->SecondSource:E->Source;
   for(UINT32 J=0;J<3;++J)*Reg(Gate(L[J]))=BIT(0);
   for(UINT32 J=0;J<2;++J)*Reg(S[J]->ParentController->Address+S[J]->CmdRegister)=BIT(1);
 }
 BlockDisable=E->SecondSource[1]->ParentController->Address+E->SecondSource[1]->CmdRegister;
 assert(mExternalProtocol.RetireInheritedDsi(&mExternalProtocol,&Owner,23)==EFI_TIMEOUT);
 assert(mExternalProtocol.ReleaseOwner(&mExternalProtocol,&Owner)==EFI_NOT_READY);CheckPrivate(&mExternalLease);
 W=Writes;R=Reads;
 assert(mExternalProtocol.RetireInheritedDsi(&mExternalProtocol,&Owner,24)==EFI_INVALID_PARAMETER&&Reads==R&&Writes==W);
 BlockDisable=0;
 assert(mExternalProtocol.RetireInheritedDsi(&mExternalProtocol,&Owner,23)==EFI_SUCCESS);
 CurrentTpl=TPL_NOTIFY;
 W=Writes;assert(mExternalProtocol.CheckInheritedDsiOff(&mExternalProtocol,&Owner,23)==EFI_SUCCESS&&Writes==W);
 CurrentTpl=TPL_APPLICATION;
 assert(mExternalProtocol.RetireInheritedDsi(&mExternalProtocol,&Owner,23)==EFI_SUCCESS&&Writes==W);
 assert(mExternalProtocol.ReleaseOwner(&mExternalProtocol,&Owner)==EFI_SUCCESS);
 for(UINT32 I=0;I<3;++I)assert(!mExternalLease.AccessOwned[I]);
 assert(!mExternalLease.DomainOwned);assert(CrClockExternalUninit(E)==CR_SUCCESS);
 free(Storage);Active=NULL;
}
int main(void){
 (void)Dispose;int Owner,Foreign;BOOLEAN Owned,On,Pending;UINT64 Incarnation;CR_CLOCK_DSI_PLAN P,Q;
 VOID *S=DxeFixture();UINTN W=Writes,R=Reads;
 assert(mExternalProtocol.Reserve(NULL,&Owner,&Owned)==EFI_INVALID_PARAMETER&&!Owned&&Writes==W&&Reads==R);
 EFI_CLOCK_EXTERNAL_CR_PROTOCOL Copy=mExternalProtocol;
 assert(Copy.Reserve(&Copy,&Owner,&Owned)==EFI_INVALID_PARAMETER&&!Owned&&Writes==W&&Reads==R);
 assert(mExternalProtocol.Reserve(&mExternalProtocol,&Owner,NULL)==EFI_INVALID_PARAMETER);
 CurrentTpl=TPL_HIGH_LEVEL;Owned=TRUE;
 assert(mExternalProtocol.Reserve(&mExternalProtocol,&Owner,&Owned)==EFI_INVALID_PARAMETER&&!Owned);
 assert(mExternalProtocol.SetPlan(&mExternalProtocol,&Owner,0,NULL,FALSE,&Owned)==EFI_INVALID_PARAMETER&&!Owned);
 assert(mExternalProtocol.GetPlan(&mExternalProtocol,&Owner,0,&Q,&On,&Pending,&Incarnation)==EFI_INVALID_PARAMETER);
 assert(mExternalProtocol.ReleaseOwner(&mExternalProtocol,&Owner)==EFI_INVALID_PARAMETER&&Reads==R&&Writes==W&&!OuterLocks);CurrentTpl=TPL_APPLICATION;
 assert(mExternalProtocol.Reserve(&mExternalProtocol,&Owner,&Owned)==EFI_SUCCESS&&Owned);CheckPrivate(&mExternalLease);NoLegacyMutation();
 assert(mClockCrProtocol.SetReset(&mClockCrProtocol,"gcc","gcc_pcie_1_bcr",TRUE)==EFI_SUCCESS&&ResetCalls==1);
 assert(mClockCrProtocol.SetReset(&mClockCrProtocol,"gcc","gcc_pcie_1_bcr",FALSE)==EFI_SUCCESS&&ResetCalls==2);
 W=Writes;R=Reads;
 assert(mExternalProtocol.Reserve(&mExternalProtocol,&Foreign,&Owned)==EFI_ACCESS_DENIED&&!Owned&&Reads==R&&Writes==W);
 assert(mExternalProtocol.ReleaseOwner(&mExternalProtocol,&Foreign)==EFI_ACCESS_DENIED&&Reads==R&&Writes==W);
 for(UINT32 I=0;I<2;++I){P=Plan(I);assert(mExternalProtocol.SetPlan(&mExternalProtocol,&Owner,I,&P,TRUE,&Owned)==EFI_SUCCESS&&Owned);assert(mExternalProtocol.GetPlan(&mExternalProtocol,&Owner,I,&Q,&On,&Pending,&Incarnation)==EFI_SUCCESS&&On&&!Pending&&Incarnation==1&&CrClockDsiPlanEqual(&P,&Q));}
 NoLegacyMutation();W=Writes;R=Reads;P=Plan(0);
 assert(mExternalProtocol.SetPlan(&mExternalProtocol,&Foreign,0,&P,TRUE,&Owned)==EFI_ACCESS_DENIED&&!Owned&&Reads==R&&Writes==W);
 assert(mExternalProtocol.SetPlan(&mExternalProtocol,&Owner,2,&P,TRUE,&Owned)==EFI_INVALID_PARAMETER&&!Owned&&Writes==W);
 assert(mExternalProtocol.ReleaseOwner(&mExternalProtocol,&Owner)==EFI_SUCCESS&&!mExternalLease.External.Owner);DxeFinish(S,&Owner);
 /* Real partial prerequisite acquire, no duplicate vote, anonymous exclusion,
  * and successful exact Release after the faulty gate becomes available. */
 S=DxeFixture();BlockEnable=Gate(mExternalLease.External.Access[2]);
 assert(mExternalProtocol.Reserve(&mExternalProtocol,&Owner,&Owned)==EFI_TIMEOUT&&Owned&&mExternalLease.CleanupPending);NoLegacyMutation();W=Writes;R=Reads;
 assert(mExternalProtocol.Reserve(&mExternalProtocol,&Owner,&Owned)==EFI_NOT_READY&&Owned&&Writes==W&&Reads==R);
 assert(mExternalProtocol.Reserve(&mExternalProtocol,&Foreign,&Owned)==EFI_ACCESS_DENIED&&!Owned&&Writes==W);
 BlockEnable=0;assert(mExternalProtocol.ReleaseOwner(&mExternalProtocol,&Owner)==EFI_SUCCESS);DxeFinish(S,&Owner);
 /* Real failed leaf release retains prerequisites and exact token. */
 S=DxeFixture();assert(mExternalProtocol.Reserve(&mExternalProtocol,&Owner,&Owned)==EFI_SUCCESS&&Owned);P=Plan(0);
 assert(mExternalProtocol.SetPlan(&mExternalProtocol,&Owner,0,&P,TRUE,&Owned)==EFI_SUCCESS&&Owned);BlockDisable=Gate(mExternalLease.External.Leaf[1]);
 assert(mExternalProtocol.ReleaseOwner(&mExternalProtocol,&Owner)==EFI_TIMEOUT&&mExternalLease.External.Owner==&Owner&&mExternalLease.DomainOwned);NoLegacyMutation();
 assert(mExternalProtocol.GetPlan(&mExternalProtocol,&Owner,0,&Q,&On,&Pending,&Incarnation)==EFI_SUCCESS&&Pending);
 BlockDisable=0;assert(mExternalProtocol.ReleaseOwner(&mExternalProtocol,&Owner)==EFI_SUCCESS);DxeFinish(S,&Owner);
 DxeRetire(0x8250);DxeRetire(0x8350);
 puts("Actual EFI external-clock wrappers/ClockLib: identity/TPL, plans, real acquire/release faults, both-SoC inherited retirement with retained access PASS");return 0;
}
