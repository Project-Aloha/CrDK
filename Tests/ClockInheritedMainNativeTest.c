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
#include "../Target/Sm8250/CrClockTarget.c"

EFI_GUID gEfiClockCrProtocolGuid = EFI_CLOCK_CR_PROTOCOL_GUID;
EFI_BOOT_SERVICES *gBS;
#define TEST_BASE 0x20000000U
typedef struct { UINTN Address; UINT32 Value; } TEST_REGISTER;
static TEST_REGISTER Registers[512];
static UINTN RegisterCount, Writes, Delays, SpinDepth;
static EFI_TPL CurrentTpl = TPL_APPLICATION;
static UINTN BlockEnable, BlockDisable;
static BOOLEAN AutoPower, AutoRoot;
static ClockDriverContext *Active;

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
  for (UINTN C=0; Active&&C<Active->ClockControllerCount; ++C) {
   ClockController *Controller=&Active->ClockControllers[C];
   for (UINTN I=0; I<Controller->ClkCount; ++I) {
    ClockNode *Node=Controller->Clks[I];if(!Node)continue;
    if (Node->Type==CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2 &&
        Address==Controller->Address+Node->CmdRegister && AutoRoot) {
      *Reg(Address)&=~(BIT(0)|BIT(31));
      if ((Value&BIT(1))==0) *Reg(Address)|=BIT(31);
    }
    if ((Node->Type==CLOCK_NODE_TYPE_BRANCH || Node->Type==CLOCK_NODE_TYPE_BRANCH_2) &&
        Address==Controller->Address+Node->EnableRegister) {
      UINT32 *Halt=Reg(Controller->Address+Node->HaltRegister);
      if (Value&Node->EnableMsk) {
        if (Address==BlockEnable) *Halt|=BIT(31);
        else *Halt&=~BIT(31);
      } else {
        if (Address==BlockDisable) *Halt&=~BIT(31);
        else *Halt|=BIT(31);
      }
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
{ return CrTargetClockReset(C,N,A); }


EFI_TPL EFIAPI EfiGetCurrentTpl(VOID) { return CurrentTpl; }
CR_STATUS MapDeviceIORegion(UINTN Address,UINTN Size) {
 assert((Address==0x100000&&Size==0x1f0000)||(Address==0xaf00000&&Size==0x20000));return CR_SUCCESS;
}
CR_STATUS UnMapMemRegion(UINTN Address,UINTN Size){assert(Address&&Size);return CR_SUCCESS;}
static ClockNode *Node(CONST CHAR8 *Name){ClockNode *N=NULL;assert(GetClockNode(Active,Name,NULL,NULL,&N)==CR_SUCCESS);return N;}
static void Setup(void){
 RegisterCount=Writes=Delays=0;BlockEnable=BlockDisable=0;AutoPower=AutoRoot=TRUE;
 Active=CrTargetGetClockContext();assert(ClockLibInit(&Active)==CR_SUCCESS);assert(CrTargetClockInit(Active)==CR_SUCCESS);
 memset(mClockVotes,0,sizeof(mClockVotes));memset(mPowerDomainVotes,0,sizeof(mPowerDomainVotes));memset(mCraneRates,0,sizeof(mCraneRates));
 mVoteLock.Tpl=TPL_NOTIFY;mVoteLock.Lock=EfiLockReleased;mClockContext=Active;mNativeClock=&mCraneNativeClock;
}
static void Inherited(UINT32 Outputs){
 const UINTN B=0xaf01000;
 *Reg(B)=BIT(31)|BIT(2)|BIT(0);*Reg(B+4)=31;*Reg(B+0x40)=0x4000;
 *Reg(B+0x0c)=0x20000|Outputs;*Reg(B+0x38)=1;
}
static void AcquirePair(UINT32 Outputs){
 Inherited(Outputs);UINTN W=Writes;BOOLEAN Owned=FALSE;UINT32 Result=0;
 ClockNode *M=Node("disp_cc_mdss_mdp_clk"),*L=Node("disp_cc_mdss_mdp_lut_clk"),*P=Node("disp_cc_pll1");
 /* Real SetClockFreqHz is staging only. No MMIO before the core ownership lock. */
 assert(CraneClockSetFreq(&mCraneNativeClock,(UINTN)M,200000000,CraneClockFrequencyHzExact,&Result)==EFI_SUCCESS&&Result==200000000&&Writes==W);
 assert(ProtocolAcquireClock(&mClockCrProtocol,"dispcc",M->Name,200000000,&Owned)==EFI_SUCCESS&&Owned);
 assert(M->ReferenceCount==1&&M->ActiveRateHz==200000000&&P->ReferenceCount==1);
 assert((*Reg(0xaf020cc)&0x71f)==0x405);
 assert(ProtocolAcquireClock(&mClockCrProtocol,"dispcc",L->Name,200000000,&Owned)==EFI_SUCCESS&&Owned&&P->ReferenceCount==2);
 W=Writes;assert(ProtocolAcquireClock(&mClockCrProtocol,"dispcc",M->Name,200000000,&Owned)==EFI_SUCCESS&&Owned&&Writes==W);
 assert(ProtocolSetClock(&mClockCrProtocol,"dispcc",M->Name,0,FALSE)==EFI_SUCCESS);
 assert(ProtocolSetClock(&mClockCrProtocol,"dispcc",L->Name,0,FALSE)==EFI_SUCCESS);
 assert(ProtocolSetClock(&mClockCrProtocol,"dispcc",M->Name,0,FALSE)==EFI_SUCCESS);
 assert(!P->ReferenceCount&&!M->ReferenceCount&&!L->ReferenceCount);
 assert(*Reg(0xaf0100c)==(0x20000|Outputs));
}
static void Refused(UINT32 Offset,UINT32 Value){
 Inherited(1);*Reg(0xaf01000+Offset)=Value;UINTN W=Writes;BOOLEAN Owned=TRUE;
 assert(ProtocolAcquireClock(&mClockCrProtocol,"dispcc","disp_cc_mdss_mdp_clk",200000000,&Owned)==EFI_NOT_READY&&!Owned&&Writes==W);
 assert(!Node("disp_cc_pll1")->ReferenceCount&&!Node("disp_cc_mdss_mdp_clk")->ReferenceCount);
}
int main(void){
 Setup();
 AcquirePair(1);AcquirePair(3);AcquirePair(5);AcquirePair(7);
 Refused(0x0c,0x20006); /* EVEN/ODD enable does not enable MAIN. */
 Refused(0,BIT(2)|BIT(0));Refused(0,BIT(31)|BIT(0));Refused(0,BIT(31)|BIT(2));
 Refused(0,BIT(31)|BIT(2)|BIT(0)|BIT(20));Refused(4,30);Refused(0x40,0);Refused(0x38,0);
 Inherited(1);BOOLEAN Owned=FALSE;assert(ProtocolAcquireClock(&mClockCrProtocol,"dispcc","disp_cc_mdss_mdp_clk",200000000,&Owned)==EFI_SUCCESS);
 UINTN W=Writes;*Reg(0xaf0100c)&=~BIT(0);
 assert(ProtocolAcquireClock(&mClockCrProtocol,"dispcc","disp_cc_mdss_mdp_clk",200000000,&Owned)==EFI_NOT_READY&&!Owned&&Writes==W);
 *Reg(0xaf0100c)|=BIT(0);assert(ProtocolSetClock(&mClockCrProtocol,"dispcc","disp_cc_mdss_mdp_clk",0,FALSE)==EFI_SUCCESS);
 assert(CurrentTpl==TPL_APPLICATION&&!SpinDepth&&mVoteLock.Lock==EfiLockReleased);
 puts("PASS: actual ClockCrDxe + SM8250 target + shared core 200MHz staging/MAIN-only inherited source, MDP/LUT sharing, unrelated output retention and live source refusals");return 0;
}
