/* Actual USB stopped-route DXE + SMMU core; modeled MMIO and allocator.
 * SPDX-License-Identifier: MIT */
#define CR_SMMU_REAL_CORE 1
#define main SmmuLegacyTestMain
#include "SmmuCrDxeTest.c"
#undef main
#define CrTargetGetSmmuContext GetActualSm8250Smmu
#define mSmmu mActualSm8250Target
#include "../Target/Sm8250/CrSmmuTarget.c"
#undef mSmmu
#undef CrTargetGetSmmuContext
#define CrTargetGetSmmuContext GetActualSm8350Smmu
#define mSmmu mActualSm8350Target
#define mSids mActualSm8350Sids
#define mCoherent mActualSm8350Coherent
#define mUfsSids mActualSm8350Ufs
#define mUsbDevices mActualSm8350Usb
#include "../Target/Sm8350/CrSmmuTarget.c"
#undef mUsbDevices
#undef mUfsSids
#undef mCoherent
#undef mSids
#undef mSmmu
#undef CrTargetGetSmmuContext
#define HW_BASE 0x15000000ULL
#define USB_BASE 0xa800000ULL
#define HW_CB(n) (0x80000U+(n)*4096U)
STATIC UINT32 Hardware[0x100000/4],Usb[0xcd00/4];
STATIC UINT32 Before[0x100000/4];
STATIC UINTN HardwareWrites,StopChecks,FailStopCheck,UsbReads;
STATIC UINT64 RejectHardware;
STATIC EFI_STATUS StopFailureStatus;
STATIC BOOLEAN HardwareTimeout;
STATIC UINT32 TestSmmuHardwareRead32(UINTN Address) {
  if(Address>=USB_BASE&&Address<USB_BASE+sizeof(Usb)){++UsbReads;return Usb[(Address-USB_BASE)/4];}
  assert(Address>=HW_BASE&&Address<HW_BASE+sizeof(Hardware));
  if(HardwareTimeout&&Address>=HW_BASE+HW_CB(0)&&(Address&0xfff)==0x7f4)return 1;
  return Hardware[(Address-HW_BASE)/4];
}
STATIC VOID TestSmmuHardwareWrite32(UINTN Address,UINT32 Value) {
  assert(Address>=HW_BASE&&Address<HW_BASE+sizeof(Hardware));++HardwareWrites;
  if(Address==RejectHardware)return;
  if(Address>=HW_BASE+HW_CB(0)&&(Address&0xfff)==0)Value&=~0x1000U;
  if(Address>=HW_BASE+HW_CB(0)&&(Address&0xfff)==0x10)Value|=0x60;
  Hardware[(Address-HW_BASE)/4]=Value;
}
STATIC EFI_STATUS EFIAPI NativeStopped(VOID *Context) {
  assert(Context==Usb);++StopChecks;
  return StopChecks==FailStopCheck?StopFailureStatus:EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ForeignStopped(VOID *Context) {return NativeStopped(Context);}
STATIC VOID Fixture(BOOLEAN Lahaina) {
  ResetState();memset(Hardware,0,sizeof(Hardware));memset(Usb,0,sizeof(Usb));
  Hardware[0x20/4]=0x4c017e57;Hardware[0x24/4]=0x6000004a;Hardware[0x28/4]=0x5111;Hardware[0]=0x270406;
  Hardware[(0x800+3*4)/4]=0x80000020;Hardware[(0xc00+3*4)/4]=3;
  Hardware[(0x1000+3*4)/4]=0x100ff;Hardware[(0x1800+3*4)/4]=1;Hardware[HW_CB(3)/4]=0xe0;
  Usb[0]=0x40;Usb[0x44/4]=1;Usb[0xc110/4]=1U<<12;Usb[0xc408/4]=1U<<31;
  Usb[0xc120/4]=0x33310000;Usb[0xc140/4]=2;
  memset(&mSmmu.Device,0,sizeof(mSmmu.Device));
  assert(SmmuProbe(&mSmmu.Device,HW_BASE,sizeof(Hardware),&mSmmuIoOps)==CR_SUCCESS);
  mSmmu.Target=Lahaina?GetActualSm8350Smmu():GetActualSm8250Smmu();
  mSmmu.Device.SctlrReadbackIgnoreMask=0x1000;mSmmu.Device.AddressBits=64;
  HardwareWrites=StopChecks=FailStopCheck=UsbReads=0;RejectHardware=0;HardwareTimeout=FALSE;StopFailureStatus=EFI_ACCESS_DENIED;
}
STATIC EFI_STATUS Acquire(VOID) {
#ifdef EFI_SMMU_CR_STOPPED_USB_REVISION
  return CrSmmuAcquireUsbStream(&mCrProtocol,USB_BASE,0x20,NativeStopped,Usb);
#else
  (VOID)NativeStopped;return CrSmmuAttach(&mCrProtocol,0,0xffff,0x20);
#endif
}
int main(void) {
  for(UINTN Board=0;Board<2;++Board) {
    Fixture(Board);assert(CrSmmuAttach(&mCrProtocol,0,0xffff,0x20)==EFI_ACCESS_DENIED);
    assert(!HardwareWrites&&mAllocateCalls==1&&mFreeCalls==1);
    assert(Acquire()==EFI_SUCCESS);
#ifdef EFI_SMMU_CR_STOPPED_USB_REVISION
    CR_SMMU_DOMAIN *D=FindDomainBySid(0x20);assert(D&&D->Domain.InheritedStreamOwned);
    VOID *Tables=D->Tables;UINTN W=HardwareWrites,A=mAllocateCalls;
    assert(Acquire()==EFI_SUCCESS&&HardwareWrites==W&&mAllocateCalls==A&&D->Tables==Tables);
    assert(CrSmmuAcquireUsbStream(&mCrProtocol,USB_BASE,0x20,ForeignStopped,Usb)==EFI_ACCESS_DENIED);
    assert(UsbReads&&StopChecks>5);
    assert(CrSmmuAcquireUsbStream(&mCrProtocol,USB_BASE,0,NativeStopped,Usb)==EFI_ACCESS_DENIED);
    assert(CrSmmuAcquireUsbStream(&mCrProtocol,USB_BASE+0x1000,0x20,NativeStopped,Usb)==EFI_UNSUPPORTED);
    CONST CrTargetSmmuContext *Actual=mSmmu.Target;mSmmu.Target=CrTargetGetActualSm8450SmmuContext();
    assert(Acquire()==EFI_UNSUPPORTED);mSmmu.Target=Actual;
    /* Every independently read controller fence must reject an active writer
     * before table allocation or any SMMU write. */
    CONST UINT32 Offsets[]={0,0x40,0x44,0xc110,0xc120,0xc140,0xc704,0xc708,0xc720,0xc400,0xc404,0xc408,0xc40c};
    for(UINTN I=0;I<ARRAY_SIZE(Offsets);++I) {
      Fixture(Board);Usb[Offsets[I]/4]^=Offsets[I]==0xc704?1U<<31:1;
      if(Offsets[I]==0)Usb[0]=0x19;
      if(Offsets[I]==0xc110)Usb[0xc110/4]=2U<<12;
      if(Offsets[I]==0xc120)Usb[0xc120/4]=0;
      if(Offsets[I]==0xc140)Usb[0xc140/4]=1;
      if(Offsets[I]==0xc408)Usb[0xc408/4]=0;
      assert(Acquire()!=EFI_SUCCESS&&!HardwareWrites&&!mAllocateCalls&&!mFreeCalls);
    }
    Fixture(Board);memcpy(Before,Hardware,sizeof(Hardware));
    assert(Acquire()==EFI_SUCCESS);D=FindDomainBySid(0x20);
    for(UINTN I=0;I<ARRAY_SIZE(Hardware);++I) {
      UINTN O=I*4,Private=HW_CB(D->Domain.Bank);
      if(O==0xc00+3*4||(O>=Private&&O<Private+4096)||
         O==0x1000+D->Domain.Bank*4||O==0x1800+D->Domain.Bank*4)continue;
      assert(Hardware[I]==Before[I]);
    }
    W=HardwareWrites;
    assert(SmmuDetach(&D->Domain)==CR_BUSY&&HardwareWrites==W);
    assert(SmmuReleaseToPhysicalAtExit(&D->Domain)==CR_BUSY&&HardwareWrites==W);
    for(UINTN I=0;I<3;++I) {
      Fixture(Board);
      if(I==0)Hardware[(0x800+3*4)/4]=0x80010020;
      if(I==1){Hardware[(0x800+4*4)/4]=0x80000020;Hardware[(0xc00+4*4)/4]=4;}
      if(I==2){Hardware[(0x800+4*4)/4]=0x80000099;Hardware[(0xc00+4*4)/4]=3;}
      assert(Acquire()!=EFI_SUCCESS&&!HardwareWrites);
    }
    Fixture(Board);FailStopCheck=1;StopFailureStatus=EFI_WARN_UNKNOWN_GLYPH;
    assert(Acquire()==EFI_NOT_READY&&!HardwareWrites&&!mAllocateCalls);
    for(UINTN I=0;I<5;++I) {
      Fixture(Board);
      if(I==0)FailStopCheck=4;
      if(I==1)RejectHardware=HW_BASE+0xc00+3*4;
      if(I==2)HardwareTimeout=TRUE;
      if(I==3)FailStopCheck=5;
      if(I==4){FailStopCheck=5;StopFailureStatus=EFI_WARN_UNKNOWN_GLYPH;}
      assert(EFI_ERROR(Acquire())&&HardwareWrites&&mAllocateCalls==1&&!mFreeCalls);
      assert(mSmmu.Domains[0].Poisoned&&mSmmu.Domains[0].Tables);
      Tables=mSmmu.Domains[0].Tables;W=HardwareWrites;A=mAllocateCalls;
      FailStopCheck=0;RejectHardware=0;HardwareTimeout=FALSE;
      assert(Acquire()==EFI_DEVICE_ERROR&&HardwareWrites==W&&mAllocateCalls==A&&!mFreeCalls&&mSmmu.Domains[0].Tables==Tables);
    }
#endif
  }
  puts("Stopped USB actual DXE+core: both targets, independent controller fences, foreign/shared routes, warning rejection and postwrite table pinning PASS");
  return 0;
}
