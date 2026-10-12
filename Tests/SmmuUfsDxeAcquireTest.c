/* SPDX-License-Identifier: MIT
 * Actual DXE ownership/table lifetime composed with actual SMMU core.
 * Only BootServices, memory attributes and physical MMIO are modeled. */
#define CR_SMMU_REAL_CORE 1
#define main SmmuLegacyTestMain
#include "SmmuCrDxeTest.c"
#undef main
#define CrTargetGetSmmuContext GetActualSm8250Smmu
#define mSmmu mActualSm8250Target
#include "../Target/Sm8250/CrSmmuTarget.c"
#undef mSmmu
#undef CrTargetGetSmmuContext
#define HW_BASE 0x15000000ULL
#define HCI_BASE 0x1d84000ULL
#define HW_CB(n) (0x80000U+(n)*4096U)
STATIC UINT32 Hardware[0x100000/4], Hci[0x3000/4];
STATIC UINTN HardwareWrites, StopChecks, FailStopCheck;
STATIC UINT64 RejectHardware;
STATIC BOOLEAN HardwareTimeout;
STATIC EFI_STATUS StopFailureStatus=EFI_ACCESS_DENIED;
STATIC UINT32 TestSmmuHardwareRead32(UINTN Address) {
  if(Address>=HCI_BASE&&Address<HCI_BASE+sizeof(Hci))return Hci[(Address-HCI_BASE)/4];
  assert(Address>=HW_BASE&&Address<HW_BASE+sizeof(Hardware));
  if(HardwareTimeout&&Address>=HW_BASE+HW_CB(0)&&(Address&0xfff)==0x7f4)return 1;
  return Hardware[(Address-HW_BASE)/4];
}
STATIC VOID TestSmmuHardwareWrite32(UINTN Address,UINT32 Value) {
  assert(Address>=HW_BASE&&Address<HW_BASE+sizeof(Hardware));HardwareWrites++;
  if(Address==RejectHardware)return;
  if(Address>=HW_BASE+HW_CB(0)&&(Address&0xfff)==0)Value&=~0x1000U;
  if(Address>=HW_BASE+HW_CB(0)&&(Address&0xfff)==0x10)Value|=0x60;
  Hardware[(Address-HW_BASE)/4]=Value;
}
STATIC EFI_STATUS EFIAPI NativeStopped(VOID *Context) {
  assert(Context==Hci);StopChecks++;
  return StopChecks==FailStopCheck?StopFailureStatus:EFI_SUCCESS;
}
STATIC VOID Fixture(VOID) {
  /* Reset denotes a new modeled controller lifetime, not timeout recovery. */
  ResetState();memset(Hardware,0,sizeof(Hardware));memset(Hci,0,sizeof(Hci));
  Hardware[0x20/4]=0x4c017e57;Hardware[0x24/4]=0x6000004a;
  Hardware[0x28/4]=0x5111;Hardware[0]=0x270406;
  Hardware[(0x800+3*4)/4]=0x800000e0;Hardware[(0xc00+3*4)/4]=3;
  Hardware[(0x800+4*4)/4]=0x800004e0;Hardware[(0xc00+4*4)/4]=4;
  for(UINTN Bank=3;Bank<=4;Bank++) {
    Hardware[(0x1000+Bank*4)/4]=0x100ff;Hardware[(0x1800+Bank*4)/4]=1;
    Hardware[HW_CB(Bank)/4]=0xe0;
  }
  memset(&mSmmu.Device,0,sizeof(mSmmu.Device));
  assert(SmmuProbe(&mSmmu.Device,HW_BASE,sizeof(Hardware),&mSmmuIoOps)==CR_SUCCESS);
  mSmmu.Target=GetActualSm8250Smmu();
  mSmmu.Device.SctlrReadbackIgnoreMask=0x1000;
  /* Host-address allocation emulates identity-accessible table RAM, not the
     board's 36-bit allocator proof. Core fixture separately uses low real PAs. */
  mSmmu.Device.AddressBits=64;
  HardwareWrites=StopChecks=FailStopCheck=0;RejectHardware=0;HardwareTimeout=FALSE;
  StopFailureStatus=EFI_ACCESS_DENIED;
}
STATIC EFI_STATUS AcquireSid(UINT16 Sid) {
  return CrSmmuAcquireUfsStream(&mCrProtocol,HCI_BASE,Sid,NativeStopped,Hci);
}
int main(void) {
  Fixture();assert(CrSmmuAttach(&mCrProtocol,0,0xffff,0xe0)==EFI_ACCESS_DENIED);
  assert(!HardwareWrites&&mAllocateCalls==1&&mFreeCalls==1);
  assert(AcquireSid(0xe0)==EFI_SUCCESS);
  CR_SMMU_DOMAIN *E0=FindDomainBySid(0xe0);assert(E0&&E0->Domain.InheritedStreamOwned);
  UINTN Writes=HardwareWrites,Alloc=mAllocateCalls,Free=mFreeCalls;
  VOID *SavedTables=E0->Tables;
  assert(AcquireSid(0xe0)==EFI_SUCCESS&&HardwareWrites==Writes&&mAllocateCalls==Alloc);
  Hci[0x34/4]=1;assert(AcquireSid(0xe0)==EFI_ACCESS_DENIED);
  assert(HardwareWrites==Writes&&mAllocateCalls==Alloc&&mFreeCalls==Free&&E0->Tables==SavedTables);
  Hci[0x34/4]=0;
  FailStopCheck=StopChecks+1;
  assert(AcquireSid(0x4e0)==EFI_ACCESS_DENIED&&!FindDomainBySid(0x4e0));
  assert(E0->Tables==SavedTables&&mAllocateCalls==Alloc&&mFreeCalls==Free);
  FailStopCheck=0;assert(AcquireSid(0x4e0)==EFI_SUCCESS);
  assert(FindDomainBySid(0x4e0)->Tables!=SavedTables&&E0->Tables==SavedTables);
  assert(mAllocateCalls==Alloc+1&&mFreeCalls==Free);
  assert(AcquireSid(0x1c00)==EFI_ACCESS_DENIED);
  assert(CrSmmuAcquireUfsStream(&mCrProtocol,HCI_BASE+0x1000,0xe0,NativeStopped,Hci)==EFI_UNSUPPORTED);
  CONST CrTargetSmmuContext *Actual=mSmmu.Target;
  mSmmu.Target=CrTargetGetActualSm8450SmmuContext();
  assert(AcquireSid(0xe0)==EFI_UNSUPPORTED);mSmmu.Target=Actual;
  for(UINTN I=0;I<6;I++) {
    static CONST UINT16 Offsets[]={0x24,0x34,0x60,0x80,0x58,0x78};
    Fixture();Hci[Offsets[I]/4]=1;assert(AcquireSid(0xe0)==EFI_ACCESS_DENIED);
    assert(!HardwareWrites&&!mAllocateCalls&&!mFreeCalls);
  }
  /* Postwrite loss, S2CR rejected write and TLB timeout must pin tables and
     reject retry; their route can still reference those tables. */
  for(UINTN I=0;I<5;I++) {
    Fixture();
    if(I==0)FailStopCheck=4;
    if(I==1)RejectHardware=HW_BASE+0xc00+3*4;
    if(I==2)HardwareTimeout=TRUE;
    if(I==3)FailStopCheck=5;
    if(I==4){FailStopCheck=5;StopFailureStatus=EFI_WARN_UNKNOWN_GLYPH;}
    assert(EFI_ERROR(AcquireSid(0xe0))&&HardwareWrites&&mAllocateCalls==1&&!mFreeCalls);
    assert(mSmmu.Domains[0].Poisoned&&mSmmu.Domains[0].Tables);
    VOID *Pinned=mSmmu.Domains[0].Tables;Alloc=mAllocateCalls;Writes=HardwareWrites;
    FailStopCheck=0;RejectHardware=0;HardwareTimeout=FALSE;
    assert(AcquireSid(0xe0)==EFI_DEVICE_ERROR&&mAllocateCalls==Alloc&&!mFreeCalls);
    assert(mSmmu.Domains[0].Tables==Pinned&&HardwareWrites==Writes);
  }
  Fixture();FailStopCheck=1;StopFailureStatus=EFI_WARN_UNKNOWN_GLYPH;
  assert(AcquireSid(0xe0)==EFI_NOT_READY&&!HardwareWrites&&!mAllocateCalls);
  Fixture();assert(AcquireSid(0xe0)==EFI_SUCCESS);
  CR_SMMU_DOMAIN *Retained=FindDomainBySid(0xe0);
  Writes=HardwareWrites;Alloc=mAllocateCalls;SavedTables=Retained->Tables;
  Hardware[(0xc00+3*4)/4]^=1;
  assert(AcquireSid(0xe0)==EFI_DEVICE_ERROR&&Retained->Poisoned&&Retained->Tables==SavedTables);
  assert(HardwareWrites==Writes&&mAllocateCalls==Alloc&&!mFreeCalls);
  puts("Stopped UFS DXE+core: actual target admission, HCI fences, two-SID retry and poisoned-table retention PASS");
  return 0;
}
