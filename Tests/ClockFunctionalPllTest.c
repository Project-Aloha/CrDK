/* Selected targets and actual ClockLib source/ClockExternal lease. Hardware
 * LOCK, global branch HALT and supplier acknowledgements are independent.
 * SPDX-License-Identifier: MIT */
#include <stdint.h>
static void PllWrite(uintptr_t Address,uint32_t Old);
#define CR_CLOCK_TEST_WRITE_CALLBACK(A,O) PllWrite((A),(O))
#define CR_CLOCK_TARGET_DISPLAY_MAIN PriorDisplayMain
#include "ClockTargetDisplayTest.c"
#include <Library/ClockExternalLease.h>
#ifdef TEST_FRONTEND
#include "../Driver/ClockCrDxe/ClockCrDxe.c"
EFI_GUID gEfiClockCrProtocolGuid=EFI_CLOCK_CR_PROTOCOL_GUID;
EFI_GUID gEfiClockExternalCrProtocolGuid=EFI_CLOCK_EXTERNAL_CR_PROTOCOL_GUID;
EFI_BOOT_SERVICES *gBS;
static EFI_TPL PllTpl=TPL_APPLICATION;
EFI_TPL EFIAPI EfiGetCurrentTpl(VOID){return PllTpl;}
VOID EFIAPI EfiAcquireLock(EFI_LOCK *L){assert(L==&mVoteLock&&L->Lock==EfiLockReleased&&PllTpl<=L->Tpl);L->OwnerTpl=PllTpl;PllTpl=L->Tpl;L->Lock=EfiLockAcquired;}
VOID EFIAPI EfiReleaseLock(EFI_LOCK *L){assert(L==&mVoteLock&&L->Lock==EfiLockAcquired&&PllTpl==L->Tpl);PllTpl=L->OwnerTpl;L->Lock=EfiLockReleased;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN L){(VOID)L;return TRUE;}
INTN EFIAPI AsciiStrnCmp(CONST CHAR8 *A,CONST CHAR8 *B,UINTN N){return strncmp(A,B,N);}
CR_STATUS CrDalClockResolveResource(CONST CHAR8 *C,CONST CHAR8 *N,CR_DAL_CLOCK_RESOURCE_KIND K,CONST CHAR8 **Native,UINT32 *Flags){return CrTargetClockResolveResource(C,N,K,Native,Flags);}
CR_STATUS CrDalClockReset(CONST CHAR8 *C,CONST CHAR8 *N,BOOLEAN A){return CrTargetClockReset(C,N,A);}
#define Lease mExternalLease
#define CrClockExternalLeasePrepareFunctionalPll(L,O,G,S,C) mExternalProtocol.PrepareFunctionalPll(&mExternalProtocol,(O),(G),(S),(C))
#define CrClockExternalLeaseCheckFunctionalPll(L,O,G) mExternalProtocol.CheckFunctionalPll(&mExternalProtocol,(O),(G))
#define CrClockExternalLeaseRelease(L,O) mExternalProtocol.ReleaseOwner(&mExternalProtocol,(O))
#else
static CR_CLOCK_EXTERNAL_LEASE Lease;
#endif
static UINTN B,Power,PllWrites,Drop,Runs;
#ifndef TEST_BEFORE
static UINTN FailAfter;
static BOOLEAN SupplyLost,QuietLost,FsmLost,SupplyReady=TRUE;
static int Foreign;
#endif
static BOOLEAN LockNever,LockStale;
static int Owner;
static void PllWrite(uintptr_t A,uint32_t Old) {
  if(A==Power){*Reg(A)=(*Reg(A)&~BIT(31))|(Old&BIT(31));
    if(AutoPower){if(*Reg(A)&1)*Reg(A)&=~BIT(31);else *Reg(A)|=BIT(31);}}
  if(B&&A>=B+0x1000&&A<=B+0x1040){
    ++PllWrites;
    if(A==Drop)*Reg(A)=Old;
    if(A==B+0x1000)*Reg(A)=(*Reg(A)&~BIT(31))|(Old&BIT(31));
    if(LockStale)*Reg(B+0x1000)|=BIT(31);
    if(A==B+0x1038&&A!=Drop){
      if(*Reg(A)==1){++Runs;if(!LockNever)*Reg(B+0x1000)|=BIT(31)|BIT(11);}
      else if(!LockStale)*Reg(B+0x1000)&=~BIT(31);
    }
  }
}
#ifndef TEST_BEFORE
static CR_STATUS Supply(VOID *Context) {
  assert(Context==&Owner&&Lease.External.Core->Lock==1);
  if(!SupplyReady||(SupplyLost&&PllWrites>=FailAfter))return CR_NOT_READY;
  if(FsmLost&&PllWrites>=FailAfter)*Reg(B+0x100c)|=BIT(21);
  if(QuietLost&&PllWrites>=FailAfter)*Reg(B+0x2014)&=~BIT(31);
  return CR_SUCCESS;
}
#endif
static void Setup(BOOLEAN Live) {
  Reset();ClockDriverContext *C=CrTargetGetClockContext();
#ifdef _KERNEL_MODE
  C->ClockControllers[CC_TYPE_GCC].MemMapped=TRUE;C->ClockControllers[CC_TYPE_DISP_CC].MemMapped=TRUE;
#endif
  assert(ClockLibInit(&C)==CR_SUCCESS);Active=C;assert(CrTargetClockInit(C)==CR_SUCCESS);
#ifdef TEST_FRONTEND
  mClockContext=C;mNativeClock=&mCraneNativeClock;mVoteLock.Tpl=TPL_NOTIFY;mVoteLock.Lock=EfiLockReleased;
#endif
  B=C->ClockControllers[CC_TYPE_DISP_CC].Address;
  assert(CrClockExternalLeaseInitForSoc(&Lease,C,TEST_SOC)==CR_SUCCESS);
  Power=B+Lease.External.Domain->GdscRegister;*Reg(Power)=BIT(0)|BIT(10);
  BOOLEAN Owned;assert(CrClockExternalLeaseReserve(&Lease,&Owner,&Owned)==CR_SUCCESS&&Owned);
  assert(CrClockExternalLeaseResetCore(&Lease,&Owner,9,TRUE)==CR_SUCCESS);
  assert(CrClockExternalLeaseResetCore(&Lease,&Owner,9,FALSE)==CR_SUCCESS);
  assert(CrClockExternalLeaseRetireInheritedFunctional(&Lease,&Owner,9)==CR_SUCCESS);
  *Reg(B+0x2014)=BIT(31);*Reg(B+0x20dc)=BIT(31);
  if(Live)PllState(B);else {*Reg(B+0x1000)=0;*Reg(B+0x1004)=0;
    *Reg(B+0x1008)=0;*Reg(B+0x1040)=0;*Reg(B+0x100c)=1;*Reg(B+0x1038)=0;}
  PllWrites=Runs=0;
}
static CR_STATUS Prepare(void) {
#ifdef TEST_BEFORE
  BOOLEAN Owned=FALSE;
  return ClockAcquire(Active,DisplayNode(Active,"disp_cc_mdss_mdp_clk"),200000000,&Owned);
#else
  return CrClockExternalLeasePrepareFunctionalPll(&Lease,&Owner,9,Supply,&Owner);
#endif
}
static void Status(CR_STATUS S,CR_STATUS E) {
  if(S!=E)fprintf(stderr,"status=%llx expected=%llx writes=%zu\n",(unsigned long long)S,(unsigned long long)E,(size_t)PllWrites);
  assert(S==E);
}
int main(int argc,char **argv) {
  assert(argc==2);CONST CHAR8 *Case=argv[1];BOOLEAN Live=!strncmp(Case,"live",4);
  Setup(Live);
#ifdef TEST_BEFORE
  Status(Prepare(),Live||TEST_SOC==0x8350?CR_SUCCESS:CR_NOT_READY);
  puts("saved source actual PLL path control");return 0;
#else
  UINTN W=Writes;
#ifdef TEST_FRONTEND
  if(!strcmp(Case,"frontend-admission")){
    EFI_CLOCK_EXTERNAL_CR_PROTOCOL Copy=mExternalProtocol;UINTN PW=PllWrites;
    Status(Copy.PrepareFunctionalPll(&Copy,&Owner,9,Supply,&Owner),CR_INVALID_PARAMETER);
    PllTpl=TPL_HIGH_LEVEL;Status(Prepare(),CR_INVALID_PARAMETER);PllTpl=TPL_APPLICATION;
    assert(PllWrites==PW&&mVoteLock.Lock==EfiLockReleased);puts("actual EFI identity/TPL refusal PASS");return 0;
  }
#endif
  if(!strcmp(Case,"foreign")){Status(CrClockExternalLeasePrepareFunctionalPll(&Lease,&Foreign,9,Supply,&Owner),CR_BUSY);assert(Writes==W);return 0;}
  if(!strcmp(Case,"generation")){Status(CrClockExternalLeasePrepareFunctionalPll(&Lease,&Owner,10,Supply,&Owner),CR_INVALID_PARAMETER);assert(Writes==W);return 0;}
  if(!strcmp(Case,"no-floor"))SupplyReady=FALSE;
  if(!strcmp(Case,"fsm"))*Reg(B+0x100c)|=BIT(21);
  if(!strcmp(Case,"partial-config"))*Reg(B+0x1004)=7;
  if(!strcmp(Case,"active-op"))*Reg(B+0x1038)=1;
  if(!strcmp(Case,"active-out"))*Reg(B+0x1000)=1;
  if(!strcmp(Case,"foreign-ref"))DisplayNode(Active,"disp_cc_pll1")->ReferenceCount=1;
  if(!strcmp(Case,"mdp-live"))*Reg(B+0x200c)&=~BIT(31);
  if(!strcmp(Case,"lut-live"))*Reg(B+0x201c)&=~BIT(31);
  if(!strcmp(Case,"rot-live"))*Reg(B+0x2014)&=~BIT(31);
  if(!strcmp(Case,"rot-force"))*Reg(B+0x20dc)|=2;
  if(!strcmp(Case,"rot-parent")){*Reg(B+0x20dc)=0;*Reg(B+0x20e0)=0x405;}
  if(!strcmp(Case,"ahb-parent"))*Reg(B+Lease.External.Access[2]->Parents[0]->CmdRegister+4)=0x405;
  if(!strcmp(Case,"lock-never"))LockNever=TRUE;
  if(!strcmp(Case,"lock-stale"))LockStale=TRUE;
  if(!strncmp(Case,"drop-",5))Drop=B+0x1000+(UINTN)strtoul(Case+5,NULL,16);
  if(!strcmp(Case,"retry-calibrated")){SupplyLost=TRUE;FailAfter=20;}
  if(!strcmp(Case,"floor-loss")){SupplyLost=TRUE;FailAfter=3;}
  if(!strcmp(Case,"fsm-loss")){FsmLost=TRUE;FailAfter=3;}
  if(!strcmp(Case,"quiet-loss")){QuietLost=TRUE;FailAfter=3;}
  if(!strcmp(Case,"live-drift"))*Reg(B+0x1040)=0;
  if(!strcmp(Case,"live-fsm"))*Reg(B+(TEST_SOC==0x8250?0x1000:0x100c))|=TEST_SOC==0x8250?BIT(20):BIT(21);
  CR_STATUS Expected=CR_SUCCESS;
  if(!strcmp(Case,"no-floor")||!strcmp(Case,"live-drift")||!strcmp(Case,"live-fsm"))Expected=CR_NOT_READY;
  else if(!Live&&TEST_SOC==0x8250)Expected=CR_NOT_READY;
  else if(!strcmp(Case,"fsm")||!strcmp(Case,"partial-config")||!strcmp(Case,"active-op")||!strcmp(Case,"active-out"))Expected=CR_NOT_READY;
  else if(!strcmp(Case,"foreign-ref")||!strcmp(Case,"mdp-live")||!strcmp(Case,"lut-live")||!strcmp(Case,"rot-live")||!strcmp(Case,"rot-force")||!strcmp(Case,"rot-parent")||!strcmp(Case,"ahb-parent"))Expected=CR_BUSY;
  else if(LockNever||LockStale)Expected=CR_TIMEOUT;
  else if(Drop)Expected=CR_DEVICE_ERROR;
  else if(SupplyLost||FsmLost)Expected=CR_NOT_READY;
  else if(QuietLost)Expected=CR_BUSY;
  Status(Prepare(),Expected);
  if(!strcmp(Case,"retry-calibrated")&&TEST_SOC==0x8350){
    assert(Lease.External.FunctionalPll.Pending&&(*Reg(B+0x1000)&BIT(11)));SupplyLost=FALSE;
    Status(Prepare(),CR_SUCCESS);assert(Lease.External.FunctionalPll.Ready&&Runs>=3);
    Status(CrClockExternalLeaseRelease(&Lease,&Owner),CR_SUCCESS);puts("owned calibrated standby retry PASS");return 0;
  }
  if(Expected!=CR_SUCCESS){
    if(Lease.External.FunctionalPll.Touched){
      assert(CrClockExternalNeedsAccess(&Lease.External));
      assert(CrClockExternalLeaseGuard(&Lease,DisplayNode(Active,"disp_cc_mdss_mdp_clk"))==CR_BUSY);
      UINTN PW=PllWrites;Status(CrClockExternalLeaseRelease(&Lease,&Owner),Expected==CR_BUSY?CR_BUSY:(SupplyLost||FsmLost)?CR_NOT_READY:LockStale?CR_TIMEOUT:CR_SUCCESS);
      if(SupplyLost||QuietLost||FsmLost||LockStale){assert(Lease.External.Owner==&Owner&&Lease.DomainOwned&&Lease.AccessOwned[2]);assert(PllWrites>=PW);}
      Drop=0;LockNever=LockStale=SupplyLost=QuietLost=FsmLost=FALSE;*Reg(B+0x100c)&=~BIT(21);*Reg(B+0x2014)|=BIT(31);*Reg(B+0x1000)&=~BIT(31);
      if(Lease.External.Owner)Status(CrClockExternalLeaseRelease(&Lease,&Owner),CR_SUCCESS);
    }else assert(PllWrites==0);
    puts("cold source refusal/retained fault control PASS");return 0;
  }
  assert(Live?PllWrites==0:Runs==2);
  Status(CrClockExternalLeaseCheckFunctionalPll(&Lease,&Owner,9),CR_SUCCESS);
  UINTN PW=PllWrites;Status(Prepare(),CR_SUCCESS);assert(PllWrites==PW);
  if(!Live){assert(*Reg(B+0x1020)==0x2a9a699c&&*Reg(B+0x102c)==0x01800000);assert(!(*Reg(B+0x1000)&BIT(23)));}
  BOOLEAN Owned;ClockNode *Mdp=DisplayNode(Active,"disp_cc_mdss_mdp_clk");
  Status(ClockAcquire(Active,Mdp,200000000,&Owned),CR_SUCCESS);assert(Owned);
  if(!Live){PW=PllWrites;Status(CrClockExternalLeaseRelease(&Lease,&Owner),CR_BUSY);assert(PllWrites==PW&&Lease.DomainOwned);
    *Reg(B+0x1020)=0;Status(CrClockExternalLeaseCheckFunctionalPll(&Lease,&Owner,9),CR_NOT_READY);*Reg(B+0x1020)=0x2a9a699c;}
  Status(ClockEnable(Active,Mdp,0,FALSE),CR_SUCCESS);
  if(TEST_SOC==0x8350&&!strncmp(Case,"release-drop-",13)){
    Drop=B+0x1000+(UINTN)strtoul(Case+13,NULL,16);UINTN PW=PllWrites;
    Status(CrClockExternalLeaseRelease(&Lease,&Owner),CR_DEVICE_ERROR);
    assert(PllWrites>PW&&Lease.External.FunctionalPll.Pending&&Lease.External.Owner==&Owner&&Lease.DomainOwned&&Lease.AccessOwned[2]);
    Drop=0;
  }
  Status(CrClockExternalLeaseRelease(&Lease,&Owner),CR_SUCCESS);
  if(!Live)assert(!(*Reg(B+0x1000)&1)&&!(*Reg(B+0x100c)&7)&&!*Reg(B+0x1038));
  puts("selected target cold/inherited PLL ownership PASS");return 0;
#endif
}
