/* Actual display group DXE/core with modeled MMIO and page services.
 * SPDX-License-Identifier: MIT */
#define CR_SMMU_REAL_CORE 1
#define CR_SMMU_DISPLAY_TARGET_OVERRIDE 1
#define main SmmuLegacyTestMain
#include "SmmuCrDxeTest.c"
#undef main
#define HW_BASE 0x15000000ULL
#define HW_CB(n) (0x80000U+(n)*4096U)
STATIC UINT32 Hardware[0x100000/4],Before[0x100000/4];
STATIC UINTN HardwareReads,HardwareWrites,StopChecks,FailStopCheck;
STATIC UINT64 RejectHardware;
STATIC BOOLEAN HardwareTimeout;
STATIC EFI_STATUS StopFailure;
STATIC CR_MDSS_TARGET DisplayTarget;
STATIC BOOLEAN CaptureTrace;
STATIC UINT32 CaptureBank,CaptureStream,CaptureEvents[8],CaptureEventCount;
CONST CR_MDSS_TARGET *CrDalGetMdssContext(VOID) {return &DisplayTarget;}
STATIC UINT32 TestSmmuHardwareRead32(UINTN A) {
  ++HardwareReads;
  assert(A>=HW_BASE&&A<HW_BASE+sizeof(Hardware));
  if(HardwareTimeout&&A>=HW_BASE+HW_CB(0)&&(A&0xfff)==0x7f4)return 1;
  return Hardware[(A-HW_BASE)/4];
}
STATIC VOID TestSmmuHardwareWrite32(UINTN A,UINT32 V) {
  assert(A>=HW_BASE&&A<HW_BASE+sizeof(Hardware));++HardwareWrites;
  if(A==RejectHardware)return;
  if(CaptureTrace){
    UINT32 Event=0;
    if(A==HW_BASE+0xc00+CaptureStream*4&&((V>>16)&3)==2)Event=1;
    if(A==HW_BASE+HW_CB(CaptureBank)&&V==0)Event=2;
    if(A==HW_BASE+HW_CB(CaptureBank)+0x610){assert(V==0);Event=3;}
    if(A==HW_BASE+HW_CB(CaptureBank)+0x7f0)Event=4;
    if(A==HW_BASE+0x800+CaptureStream*4&&!(V&0x80000000U))Event=5;
    if(Event){assert(CaptureEventCount<ARRAY_SIZE(CaptureEvents));CaptureEvents[CaptureEventCount++]=Event;}
  }
  if(A>=HW_BASE+HW_CB(0)&&(A&0xfff)==0)V&=~0x1000U;
  if(A>=HW_BASE+HW_CB(0)&&(A&0xfff)==0x10)V|=0x60U;
  Hardware[(A-HW_BASE)/4]=V;
}
STATIC EFI_STATUS EFIAPI NativeStopped(VOID *C,UINT64 G) {
  assert(C==&DisplayTarget&&G==71&&mSmmu.Lock.Lock==EfiLockReleased);
  ++StopChecks;return StopChecks==FailStopCheck?StopFailure:EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ForeignStopped(VOID *C,UINT64 G) {return NativeStopped(C,G);}
STATIC VOID Fixture(UINT32 Soc) {
  ResetState();
#ifndef EXPECT_UNTYPED_ATTACH
  memset(&mDisplayGroup,0,sizeof(mDisplayGroup));memset(mDisplayBuffers,0,sizeof(mDisplayBuffers));
#endif
  memset(Hardware,0,sizeof(Hardware));
  Hardware[0x20/4]=0x4c017e57;Hardware[0x24/4]=0x6000004a;Hardware[0x28/4]=0x5111;
  Hardware[0]=Soc==0x8250?0x270406:0x200406;
  /* One inherited masked route covers all four display SIDs, sharing CB3. */
  Hardware[(0x800+3*4)/4]=0x84020820;Hardware[(0xc00+3*4)/4]=0x303;
  Hardware[(0x1000+3*4)/4]=0x1f300;Hardware[(0x1800+3*4)/4]=1;
  Hardware[HW_CB(3)/4]=0x1027;Hardware[(HW_CB(3)+0x10)/4]=0x38011;
  Hardware[(HW_CB(3)+0x24)/4]=7U<<16;Hardware[(HW_CB(3)+0x2c)/4]=7U<<16;
  Hardware[(HW_CB(3)+0x30)/4]=0x80201c;
  memset(&mSmmu.Device,0,sizeof(mSmmu.Device));
  assert(SmmuProbe(&mSmmu.Device,HW_BASE,sizeof(Hardware),&mSmmuIoOps)==CR_SUCCESS);
  mValidationTarget.Base=HW_BASE;mValidationTarget.Size=sizeof(Hardware);
  mValidationTarget.SegmentCount=3;mValidationTarget.MaximumDomains=14;mValidationTarget.SctlrReadbackIgnoreMask=0x1000;
  mSmmu.Target=&mValidationTarget;mSmmu.Device.SctlrReadbackIgnoreMask=0x1000;
  memset(&DisplayTarget,0,sizeof(DisplayTarget));DisplayTarget.Revision=1;DisplayTarget.Size=sizeof(DisplayTarget);
  DisplayTarget.Soc=Soc;DisplayTarget.SmmuBase=HW_BASE;DisplayTarget.RequiresStoppedDisplay=TRUE;
  DisplayTarget.Streams[0]=0x820;DisplayTarget.Streams[1]=0x822;DisplayTarget.Streams[2]=0xc20;DisplayTarget.Streams[3]=0xc22;
  memcpy(Before,Hardware,sizeof(Hardware));HardwareReads=HardwareWrites=StopChecks=FailStopCheck=0;RejectHardware=0;HardwareTimeout=FALSE;StopFailure=EFI_ACCESS_DENIED;
  CaptureTrace=FALSE;CaptureEventCount=0;
}
#ifndef EXPECT_UNTYPED_ATTACH
STATIC VOID DisabledEmptyFixture(UINT32 Soc,UINT32 Stream,UINT32 Bank)
{
  Fixture(Soc);
  Hardware[(0x800+3*4)/4]=0;Hardware[HW_CB(3)/4]=0;
  /* Literal Kona capture: AArch64 stage1 CB4 is referenced but disabled,
     has empty TTBRs and ASID0. CONTEXTIDR is not its AArch64 ASID. */
  Hardware[(0x800+Stream*4)/4]=0x84020820;Hardware[(0xc00+Stream*4)/4]=Bank;
  Hardware[(0x1000+Bank*4)/4]=0x1f000;Hardware[(0x1800+Bank*4)/4]=1;
  Hardware[HW_CB(Bank)/4]=0xe0;Hardware[(HW_CB(Bank)+0x10)/4]=0x4002c074;
  Hardware[(HW_CB(Bank)+0x20)/4]=Hardware[(HW_CB(Bank)+0x24)/4]=0;
  Hardware[(HW_CB(Bank)+0x28)/4]=Hardware[(HW_CB(Bank)+0x2c)/4]=0;
  Hardware[(HW_CB(Bank)+0x30)/4]=0;Hardware[(HW_CB(Bank)+0x34)/4]=0x2a66b5aa;
  /* TLBI readback starts nonzero; successful retirement must submit zero. */
  Hardware[(HW_CB(Bank)+0x610)/4]=0x55;
  memcpy(Before,Hardware,sizeof(Hardware));CaptureBank=Bank;CaptureStream=Stream;
}
STATIC UINTN CoreCounts[5],CoreFaultNth,CoreFaultKind;
STATIC BOOLEAN CorePostedFailure;
STATIC CR_SMMU_DISPLAY_GROUP_CORE Core;
STATIC BOOLEAN CoreFault(UINTN Kind)
{ return ++CoreCounts[Kind]==CoreFaultNth&&Kind==CoreFaultKind; }
STATIC CR_STATUS CoreReady(VOID *C,UINT64 G)
{
  assert(C==&Core&&G==97);return CoreFault(0)?EFI_WARN_UNKNOWN_GLYPH:CR_SUCCESS;
}
STATIC CR_STATUS CoreRead(VOID *C,UINT32 O,UINT32 *V)
{
  assert(C==&Core);if(CoreFault(1))return EFI_WARN_UNKNOWN_GLYPH;
  *V=TestSmmuHardwareRead32(HW_BASE+O);return CR_SUCCESS;
}
STATIC CR_STATUS CoreWrite(VOID *C,UINT32 O,UINT32 V)
{
  assert(C==&Core);BOOLEAN Fail=CoreFault(2);
  if(!Fail||CorePostedFailure)TestSmmuHardwareWrite32(HW_BASE+O,V);
  return Fail?EFI_WARN_UNKNOWN_GLYPH:CR_SUCCESS;
}
STATIC CR_STATUS CoreFence(VOID *C)
{ assert(C==&Core);return CoreFault(3)?EFI_WARN_UNKNOWN_GLYPH:CR_SUCCESS; }
STATIC CR_STATUS CoreDelay(VOID *C,UINT32 U)
{ assert(C==&Core&&U&&U<=1024);return CoreFault(4)?EFI_WARN_UNKNOWN_GLYPH:CR_SUCCESS; }
STATIC VOID CoreFixture(UINT32 Soc)
{
  Fixture(Soc);memset(&Core,0,sizeof(Core));memset(CoreCounts,0,sizeof(CoreCounts));
  CoreFaultNth=0;CorePostedFailure=FALSE;
  CR_SMMU_DISPLAY_GROUP_CORE_IO Io={&Core,CoreReady,CoreRead,CoreWrite,CoreFence,CoreDelay};
  assert(CrSmmuDisplayGroupInit(&Core,&mSmmu.Device,&Io,DisplayTarget.Streams,97)==CR_SUCCESS);
  assert(CrSmmuDisplayGroupObserve(&Core)==CR_SUCCESS);
  memset(CoreCounts,0,sizeof(CoreCounts));
}
STATIC VOID TestPortableRetries(UINT32 Soc)
{
  CoreFixture(Soc);assert(CrSmmuDisplayGroupRetire(&Core)==CR_SUCCESS);
  UINTN Counts[5],Cases=0;memcpy(Counts,CoreCounts,sizeof(Counts));
  for(UINTN Kind=0;Kind<4;++Kind)for(UINTN N=1;N<=Counts[Kind];++N)
    for(UINTN Posted=0;Posted<(Kind==2?2U:1U);++Posted){
      CoreFixture(Soc);CoreFaultKind=Kind;CoreFaultNth=N;CorePostedFailure=Posted;
      assert(CrSmmuDisplayGroupRetire(&Core)==CR_NOT_READY&&!Core.Retired);
      if(Core.Reserved)assert(CrSmmuDisplayGroupAbort(&Core)==CR_BUSY);
      CR_SMMU_DISPLAY_GROUP_CORE Copy=Core;
      assert(CrSmmuDisplayGroupRetire(&Copy)==CR_INVALID_PARAMETER);
      CoreFaultNth=0;
      assert(CrSmmuDisplayGroupRetire(&Core)==CR_SUCCESS&&Core.Retired);
      UINTN Writes=HardwareWrites;
      assert(CrSmmuDisplayGroupCheckRetired(&Core)==CR_SUCCESS&&HardwareWrites==Writes);
      assert(mSmmu.Device.ReservedStreams[3]&&mSmmu.Device.ReservedBanks[3]&&mSmmu.Device.ReservedAsids[7]);
      assert(Hardware[0]==Before[0]&&Hardware[(HW_CB(3)+0x20)/4]==Before[(HW_CB(3)+0x20)/4]);
      ++Cases;
    }
  CoreFixture(Soc);CoreFaultKind=4;CoreFaultNth=1;HardwareTimeout=TRUE;
  assert(CrSmmuDisplayGroupRetire(&Core)==CR_NOT_READY&&Core.Reserved&&!Core.Failed);
  CoreFaultNth=0;HardwareTimeout=FALSE;
  assert(CrSmmuDisplayGroupRetire(&Core)==CR_SUCCESS);
  CoreFixture(Soc);CoreFaultKind=3;CoreFaultNth=1;
  assert(CrSmmuDisplayGroupRetire(&Core)==CR_NOT_READY&&Core.Touched);
  CoreFaultNth=0;Hardware[0]^=4;UINTN Writes=HardwareWrites;
  assert(CrSmmuDisplayGroupRetire(&Core)!=CR_SUCCESS&&HardwareWrites==Writes&&Core.Reserved);
  printf("Portable display group SoC=%x: %zu retained callback failure/retry cases PASS\n",Soc,(size_t)(Cases+2));
}
STATIC EFI_PHYSICAL_ADDRESS FramePhysical;
STATIC UINT64 BufferAttributes[2];
STATIC EFI_STATUS EFIAPI BufferDescriptor(EFI_PHYSICAL_ADDRESS Address,
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D)
{
  EFI_STATUS S=MockGetMemorySpaceDescriptor(Address,D);if(S!=EFI_SUCCESS)return S;
  BOOLEAN Frame=FramePhysical&&Address>=FramePhysical&&Address<FramePhysical+8192;
  D->GcdMemoryType=Frame?EfiGcdMemoryTypeReserved:EfiGcdMemoryTypeSystemMemory;
  D->Attributes=BufferAttributes[Frame?0:1];return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI BufferAttributesSet(EFI_PHYSICAL_ADDRESS Address,UINT64 Bytes,UINT64 Attributes)
{
  EFI_STATUS S=MockSetMemorySpaceAttributes(Address,Bytes,Attributes);
  if(S==EFI_SUCCESS)BufferAttributes[Address==FramePhysical?0:1]=Attributes;
  return S;
}
STATIC VOID BuffersFixture(VOID)
{
  FramePhysical=0;BufferAttributes[0]=BufferAttributes[1]=EFI_MEMORY_WB|EFI_MEMORY_XP;
  gDS->GetMemorySpaceDescriptor=BufferDescriptor;gDS->SetMemorySpaceAttributes=BufferAttributesSet;
}
STATIC VOID TestDecodedInventory(UINT32 Soc)
{
  Fixture(Soc);
  /* Move the inherited route and bank, use a different decoded ASID and
   * retain an unrelated live bank. No CB/SMR/ASID index is a board constant. */
  Hardware[(0x800+8*4)/4]=Hardware[(0x800+3*4)/4];
  Hardware[(0xc00+8*4)/4]=0x309;Hardware[(0x800+3*4)/4]=0;
  Hardware[(0x1000+9*4)/4]=Hardware[(0x1000+3*4)/4];
  Hardware[(0x1800+9*4)/4]=1;
  memcpy(&Hardware[HW_CB(9)/4],&Hardware[HW_CB(3)/4],4096);
  Hardware[HW_CB(3)/4]=0;
  Hardware[(HW_CB(9)+0x24)/4]=0x117U<<16;
  Hardware[(HW_CB(9)+0x2c)/4]=0x117U<<16;
  Hardware[HW_CB(10)/4]=1;Hardware[(0x1000+10*4)/4]=0x1f300;
  Hardware[(0x1800+10*4)/4]=1;Hardware[(HW_CB(10)+0x10)/4]=0x38001;
  /* With TCR2.AS=0, only the low eight TTBR ASID bits are active. The
   * ignored high byte cannot conceal a foreign effective ASID 1. */
  Hardware[(HW_CB(10)+0x24)/4]=0x101U<<16;
  Hardware[(HW_CB(10)+0x2c)/4]=0x201U<<16;
  memcpy(Before,Hardware,sizeof(Hardware));
  EFI_SMMU_CR_DISPLAY_GROUP G={0};
  assert(CrSmmuAcquireDisplayGroup(&mCrProtocol,Soc,71,NativeStopped,&DisplayTarget,&G)==EFI_SUCCESS);
  assert(mDisplayGroup.Core.Before.Streams[0]==8&&mDisplayGroup.Core.Before.OwnedBanks[0]==9&&
         mDisplayGroup.Core.Before.Asids[0]==0x117);
  for(UINT32 I=0;I<4;++I){
    CR_SMMU_DOMAIN *D=FindDomainBySid(DisplayTarget.Streams[I]);
    assert(D&&D->Domain.Asid!=1);
  }
  assert(Hardware[0]==Before[0]&&Hardware[HW_CB(9)/4]==0&&
         Hardware[(HW_CB(9)+0x610)/4]==0x117);
  assert(memcmp(&Hardware[HW_CB(10)/4],&Before[HW_CB(10)/4],4096)==0);
  assert(Hardware[(0x1000+10*4)/4]==Before[(0x1000+10*4)/4]&&
         Hardware[(0x1800+10*4)/4]==Before[(0x1800+10*4)/4]);
}
STATIC VOID TestDisabledEmptyInventory(UINT32 Soc)
{
  for(UINT32 Moved=0;Moved<2;++Moved){
    UINT32 Stream=Moved?8:4,Bank=Moved?9:4;
    DisabledEmptyFixture(Soc,Stream,Bank);CaptureTrace=TRUE;
    EFI_SMMU_CR_DISPLAY_GROUP G={0};
    EFI_STATUS CapturedStatus=CrSmmuAcquireDisplayGroup(&mCrProtocol,Soc,71,NativeStopped,&DisplayTarget,&G);
    if(CapturedStatus!=EFI_SUCCESS)fprintf(stderr,"disabled empty AArch64 bank status=%llx\n",(unsigned long long)CapturedStatus);
    assert(CapturedStatus==EFI_SUCCESS);
    CR_SMMU_DISPLAY_GROUP_CORE *B=&mDisplayGroup.Core;
    assert(B->Before.StreamCount==1&&B->Before.BankCount==1&&B->Before.Streams[0]==Stream&&
      B->Before.OwnedBanks[0]==Bank&&B->Before.Asids[0]==0&&B->Retired);
    assert(mSmmu.Device.ReservedStreams[Stream]&&mSmmu.Device.ReservedBanks[Bank]&&mSmmu.Device.ReservedAsids[0]);
    assert(CaptureEventCount==5);
    for(UINT32 I=0;I<5;++I)assert(CaptureEvents[I]==I+1);
    assert(Hardware[0]==Before[0]&&Hardware[(0x800+Stream*4)/4]==0x04020820&&
      Hardware[(0xc00+Stream*4)/4]==0x20000&&Hardware[HW_CB(Bank)/4]==0&&
      Hardware[(HW_CB(Bank)+0x610)/4]==0);
    /* Retirement preserves every inherited translation/configuration word. */
    STATIC CONST UINT32 Stable[]={4,0x10,0x20,0x24,0x28,0x2c,0x30,0x34,0x38,0x3c};
    for(UINT32 I=0;I<ARRAY_SIZE(Stable);++I)
      assert(Hardware[(HW_CB(Bank)+Stable[I])/4]==Before[(HW_CB(Bank)+Stable[I])/4]);
    assert(Hardware[(0x1000+Bank*4)/4]==Before[(0x1000+Bank*4)/4]&&
      Hardware[(0x1800+Bank*4)/4]==Before[(0x1800+Bank*4)/4]);
    for(UINT32 I=0;I<4;++I){CR_SMMU_DOMAIN *D=FindDomainBySid(DisplayTarget.Streams[I]);
      assert(D&&D->Domain.Bank!=Bank&&D->Domain.Asid&&D->Domain.InheritedStreamOwned);}
    UINTN W=HardwareWrites,A=mAllocateCalls;
    assert(CrSmmuAcquireDisplayGroup(&mCrProtocol,Soc,71,NativeStopped,&DisplayTarget,&G)==EFI_SUCCESS&&
      HardwareWrites==W&&mAllocateCalls==A&&CaptureEventCount==5);
  }
  /* Zero is narrowly supported for a disabled, empty AArch64 context.
     Active/nonempty/unknown formats and sharing remain prewrite refusals. */
  for(UINT32 Case=0;Case<10;++Case){
    DisabledEmptyFixture(Soc,4,4);EFI_SMMU_CR_DISPLAY_GROUP G={0};
    if(Case==0)Hardware[HW_CB(4)/4]|=1;
    if(Case==1)Hardware[(HW_CB(4)+0x20)/4]=0x200000;
    if(Case==2)Hardware[(HW_CB(4)+0x28)/4]=0x200000;
    if(Case==3)Hardware[(0x1800+4*4)/4]=0;
    if(Case==4)Hardware[(0x1000+4*4)/4]=0x2f000;
    if(Case==5){Hardware[(0x800+7*4)/4]=0x80000099;Hardware[(0xc00+7*4)/4]=4;}
    if(Case==6){Hardware[HW_CB(10)/4]=1;Hardware[(0x1000+10*4)/4]=0x1f000;
      Hardware[(0x1800+10*4)/4]=1;Hardware[(HW_CB(10)+0x10)/4]=0x10;}
    if(Case==7)mSmmu.Device.ReservedAsids[0]=TRUE;
    if(Case==8){FailStopCheck=1;StopFailure=EFI_WARN_UNKNOWN_GLYPH;}
    if(Case==9){Hardware[(0x800+4*4)/4]|=1U<<18;}
    assert(CrSmmuAcquireDisplayGroup(&mCrProtocol,Soc,71,NativeStopped,&DisplayTarget,&G)!=EFI_SUCCESS&&
      !HardwareWrites&&!mAllocateCalls);
  }
  /* A posted fence failure retains ASID0 and exact identity for retry;
     reactivation between retries is rejected before another write. */
  for(UINT32 Drift=0;Drift<2;++Drift){
    DisabledEmptyFixture(Soc,4,4);memset(&Core,0,sizeof(Core));memset(CoreCounts,0,sizeof(CoreCounts));
    CoreFaultNth=0;CorePostedFailure=FALSE;
    CR_SMMU_DISPLAY_GROUP_CORE_IO Io={&Core,CoreReady,CoreRead,CoreWrite,CoreFence,CoreDelay};
    assert(CrSmmuDisplayGroupInit(&Core,&mSmmu.Device,&Io,DisplayTarget.Streams,97)==CR_SUCCESS);
    assert(CrSmmuDisplayGroupObserve(&Core)==CR_SUCCESS&&!HardwareWrites);
    memset(CoreCounts,0,sizeof(CoreCounts));CoreFaultKind=3;CoreFaultNth=1;
    assert(CrSmmuDisplayGroupRetire(&Core)==CR_NOT_READY&&Core.Touched&&Core.Reserved&&!Core.Retired);
    assert(mSmmu.Device.ReservedAsids[0]&&CrSmmuDisplayGroupAbort(&Core)==CR_BUSY);
    CoreFaultNth=0;UINTN W=HardwareWrites;
    if(Drift){Hardware[HW_CB(4)/4]|=1;
      assert(CrSmmuDisplayGroupRetire(&Core)!=CR_SUCCESS&&HardwareWrites==W);}
    else {assert(CrSmmuDisplayGroupRetire(&Core)==CR_SUCCESS&&Core.Retired);
      W=HardwareWrites;assert(CrSmmuDisplayGroupCheckRetired(&Core)==CR_SUCCESS&&HardwareWrites==W);}
  }
  /* ASID0 must survive an uncertain write that actually reached hardware. */
  DisabledEmptyFixture(Soc,4,4);memset(&Core,0,sizeof(Core));memset(CoreCounts,0,sizeof(CoreCounts));
  CoreFaultNth=0;CorePostedFailure=FALSE;
  CR_SMMU_DISPLAY_GROUP_CORE_IO Io={&Core,CoreReady,CoreRead,CoreWrite,CoreFence,CoreDelay};
  assert(CrSmmuDisplayGroupInit(&Core,&mSmmu.Device,&Io,DisplayTarget.Streams,97)==CR_SUCCESS);
  assert(CrSmmuDisplayGroupObserve(&Core)==CR_SUCCESS);
  memset(CoreCounts,0,sizeof(CoreCounts));CoreFaultKind=2;CoreFaultNth=1;CorePostedFailure=TRUE;
  assert(CrSmmuDisplayGroupRetire(&Core)==CR_NOT_READY&&Core.Touched&&Core.Reserved&&!Core.Retired);
  assert(Hardware[(0xc00+4*4)/4]==0x20000&&mSmmu.Device.ReservedAsids[0]&&
    CrSmmuDisplayGroupAbort(&Core)==CR_BUSY);
  CoreFaultNth=0;assert(CrSmmuDisplayGroupRetire(&Core)==CR_SUCCESS&&Core.Retired);
  /* The actual DXE keeps the zero-tag journal on checked-write, stopped
     callback and terminal sync failures. A timeout never becomes a retry. */
  for(UINT32 Case=0;Case<3;++Case){
    DisabledEmptyFixture(Soc,4,4);EFI_SMMU_CR_DISPLAY_GROUP G={0};
    if(Case==0)RejectHardware=HW_BASE+0xc00+4*4;
    if(Case==1)HardwareTimeout=TRUE;
    if(Case==2)FailStopCheck=2;
    assert(CrSmmuAcquireDisplayGroup(&mCrProtocol,Soc,71,NativeStopped,&DisplayTarget,&G)!=EFI_SUCCESS&&
      G.Token&&HardwareWrites&&mSmmu.Device.ReservedStreams[4]&&mSmmu.Device.ReservedBanks[4]&&
      mSmmu.Device.ReservedAsids[0]);
    UINTN W=HardwareWrites,A=mAllocateCalls;VOID *Token=G.Token;
    RejectHardware=0;HardwareTimeout=FALSE;FailStopCheck=0;
    if(Case==1)assert(CrSmmuAcquireDisplayGroup(&mCrProtocol,Soc,71,NativeStopped,&DisplayTarget,&G)!=EFI_SUCCESS&&
      G.Token==Token&&HardwareWrites==W&&mAllocateCalls==A&&!mFreeCalls);
    else assert(CrSmmuAcquireDisplayGroup(&mCrProtocol,Soc,71,NativeStopped,&DisplayTarget,&G)==EFI_SUCCESS&&
      G.Token==Token&&!mFreeCalls);
    assert(mSmmu.Device.ReservedAsids[0]);
  }
  printf("Display ASID0 SoC=%x: captured/moved disabled empty banks, ordered retirement, strict refusals and retained retry PASS\n",Soc);
}
STATIC VOID TestForeignAsidSelection(UINT32 Soc)
{
  /* A1 selects the ASID independently of which TTBR supplies a translation.
     The inactive tag may be zero; the selected tag must remain exclusive. */
  UINT32 Cases=0;
  for(UINT32 Format=0;Format<3;++Format)
    for(UINT32 A1=0;A1<2;++A1)
      for(UINT32 Disabled=0;Disabled<2;++Disabled)
        for(UINT32 Conflict=0;Conflict<2;++Conflict){
    DisabledEmptyFixture(Soc,4,4);
    Hardware[HW_CB(10)/4]=Disabled?0:1;
    Hardware[(0x800+10*4)/4]=0x80000099;Hardware[(0xc00+10*4)/4]=10;
    Hardware[(0x1000+10*4)/4]=0x1f000;
    Hardware[(0x1800+10*4)/4]=Format==2?0:1;
    Hardware[(HW_CB(10)+0x10)/4]=Format==1?0x10:0;
    Hardware[(HW_CB(10)+0x30)/4]=(A1?1U<<22:0)|(Format==2?1U<<31:0)|
      (A1?1U<<23:1U<<7); /* Disable the selected TTBR walk, not its ASID. */
    UINT32 Selected=Conflict?0:(Format==1?0x100:9);
    /* In 8-bit format even ignored high tag bits cannot hide ASID zero. */
    if(Conflict&&Format!=1)Selected=0x100;
    Hardware[(HW_CB(10)+(A1?0x2c:0x24))/4]=Selected<<16;
    Hardware[(HW_CB(10)+(A1?0x24:0x2c))/4]=(Conflict?9U:0U)<<16;
    memcpy(Before,Hardware,sizeof(Hardware));
    EFI_SMMU_CR_DISPLAY_GROUP G={0};
    EFI_STATUS S=CrSmmuAcquireDisplayGroup(&mCrProtocol,Soc,71,NativeStopped,&DisplayTarget,&G);
    if(Conflict){
      assert(S==EFI_ACCESS_DENIED&&!HardwareWrites&&!mAllocateCalls);
      assert(mDisplayGroup.Core.Before.Conflict==CrSmmuDisplaySharedAsid&&
        mDisplayGroup.Core.Before.ConflictIndex==10&&mDisplayGroup.Core.Before.ConflictAsid==0);
    }
    else assert((S==EFI_SUCCESS)&&"inactive foreign TTBR tag must not claim ASID zero");
    assert(memcmp(&Hardware[HW_CB(10)/4],&Before[HW_CB(10)/4],4096)==0);
    assert(Hardware[(0x800+10*4)/4]==Before[(0x800+10*4)/4]&&
      Hardware[(0xc00+10*4)/4]==Before[(0xc00+10*4)/4]);
    ++Cases;
  }
  /* An A1 change after inventory must be caught before local TLBI writes. */
  DisabledEmptyFixture(Soc,4,4);
  Hardware[HW_CB(10)/4]=1;Hardware[(0x1000+10*4)/4]=0x1f000;
  Hardware[(0x1800+10*4)/4]=1;Hardware[(HW_CB(10)+0x24)/4]=9U<<16;
  memset(&Core,0,sizeof(Core));memset(CoreCounts,0,sizeof(CoreCounts));CoreFaultNth=0;
  CR_SMMU_DISPLAY_GROUP_CORE_IO Io={&Core,CoreReady,CoreRead,CoreWrite,CoreFence,CoreDelay};
  assert(CrSmmuDisplayGroupInit(&Core,&mSmmu.Device,&Io,DisplayTarget.Streams,97)==CR_SUCCESS);
  assert(CrSmmuDisplayGroupObserve(&Core)==CR_SUCCESS&&!HardwareWrites);
  Hardware[(HW_CB(10)+0x30)/4]=1U<<22;
  assert(CrSmmuDisplayGroupRetire(&Core)==CR_BUSY&&!HardwareWrites);
  printf("Display foreign ASID selection SoC=%x: %u format/width/A1/live/reference cases and drift refusal PASS\n",Soc,Cases);
}
STATIC VOID TestForeignEmptyBank(UINT32 Soc)
{
  for(UINT32 Case=0;Case<8;++Case){
    DisabledEmptyFixture(Soc,4,4);
    /* Literal foreign CB0 from the new Kona capture. Preserve its route. */
    Hardware[0x800/4]=0x80000099;Hardware[0xc00/4]=0;
    Hardware[0x1000/4]=0x1f000;Hardware[0x1800/4]=1;
    Hardware[HW_CB(0)/4]=0xe0;Hardware[(HW_CB(0)+0x10)/4]=0x40028072;
    Hardware[(HW_CB(0)+0x34)/4]=0x284a5499;
    if(Case==1)Hardware[HW_CB(0)/4]|=1;
    if(Case==2)Hardware[(HW_CB(0)+0x20)/4]=0x200000;
    if(Case==3)Hardware[(HW_CB(0)+0x28)/4]=0x200000;
    if(Case==4)Hardware[0x1000/4]=0x2f000;
    if(Case==5){Hardware[0x1800/4]=0;Hardware[(HW_CB(0)+0x34)/4]=0;}
    if(Case==6)Hardware[0xc00/4]=4;
    if(Case==7)mSmmu.Device.ReservedBanks[0]=TRUE;
    memcpy(Before,Hardware,sizeof(Hardware));
    EFI_SMMU_CR_DISPLAY_GROUP G={0};
    EFI_STATUS S=CrSmmuAcquireDisplayGroup(&mCrProtocol,Soc,71,NativeStopped,&DisplayTarget,&G);
    if(Case==0)assert((S==EFI_SUCCESS)&&"disabled empty foreign context is not a live ASID0 owner");
    else assert(S!=EFI_SUCCESS&&!HardwareWrites&&!mAllocateCalls);
    assert(memcmp(&Hardware[HW_CB(0)/4],&Before[HW_CB(0)/4],4096)==0);
    assert(Hardware[0x800/4]==Before[0x800/4]&&Hardware[0xc00/4]==Before[0xc00/4]);
  }
  for(UINT32 Drift=0;Drift<3;++Drift){
    DisabledEmptyFixture(Soc,4,4);
    Hardware[0x800/4]=0x80000099;Hardware[0xc00/4]=0;
    Hardware[0x1000/4]=0x1f000;Hardware[0x1800/4]=1;Hardware[HW_CB(0)/4]=0xe0;
    memset(&Core,0,sizeof(Core));memset(CoreCounts,0,sizeof(CoreCounts));CoreFaultNth=0;
    CR_SMMU_DISPLAY_GROUP_CORE_IO Io={&Core,CoreReady,CoreRead,CoreWrite,CoreFence,CoreDelay};
    assert(CrSmmuDisplayGroupInit(&Core,&mSmmu.Device,&Io,DisplayTarget.Streams,97)==CR_SUCCESS);
    assert(CrSmmuDisplayGroupObserve(&Core)==CR_SUCCESS&&!HardwareWrites);
    if(Drift==0)Hardware[HW_CB(0)/4]|=1;
    if(Drift==1)Hardware[(HW_CB(0)+0x28)/4]=0x200000;
    if(Drift==2)mSmmu.Device.ReservedBanks[0]=TRUE;
    assert(CrSmmuDisplayGroupRetire(&Core)!=CR_SUCCESS&&!HardwareWrites);
  }
  printf("Display empty foreign context SoC=%x: exact capture, active/nonempty/unknown/reserved refusals and drift PASS\n",Soc);
}
STATIC VOID TestExistingDomain(UINT32 Soc)
{
  Fixture(Soc);STATIC CONST UINT16 Allowed[]={0x1c00};
  mValidationTarget.DefaultStreamIds=Allowed;mValidationTarget.DefaultStreamCount=1;
  assert(CrSmmuAttach(&mCrProtocol,0,0x100,Allowed[0])==EFI_SUCCESS);
  CR_SMMU_DOMAIN *Foreign=FindDomainBySid(Allowed[0]);assert(Foreign);
  UINT32 Bank=Foreign->Domain.Bank,Stream=Foreign->Domain.Stream;
  /* Real Qcom readback also has nonsemantic bits 5 and 6. Retirement never
   * adopts old translation policy; it only decodes ASID and preserves the
   * full owned configuration byte-for-byte through its stop transaction. */
  Hardware[(HW_CB(3)+0x10)/4]|=0x60;
  memcpy(Before,Hardware,sizeof(Hardware));
  EFI_SMMU_CR_DISPLAY_GROUP G={0};
  assert(CrSmmuAcquireDisplayGroup(&mCrProtocol,Soc,71,NativeStopped,&DisplayTarget,&G)==EFI_SUCCESS);
  assert(memcmp(&Hardware[HW_CB(Bank)/4],&Before[HW_CB(Bank)/4],4096)==0);
  assert(Hardware[(0x800+Stream*4)/4]==Before[(0x800+Stream*4)/4]&&
         Hardware[(0xc00+Stream*4)/4]==Before[(0xc00+Stream*4)/4]);
  assert(Hardware[(HW_CB(3)+0x10)/4]==Before[(HW_CB(3)+0x10)/4]);
}
STATIC EFI_STATUS EFIAPI WarningTableAllocate(EFI_ALLOCATE_TYPE Type,EFI_MEMORY_TYPE MemoryType,
  UINTN Pages,EFI_PHYSICAL_ADDRESS *Address)
{
  EFI_STATUS S=MockAllocatePages(Type,MemoryType,Pages,Address);assert(S==EFI_SUCCESS);
  memset((VOID *)(UINTN)*Address,0x5a,Pages*SMMU_PAGE_SIZE);return EFI_WARN_UNKNOWN_GLYPH;
}
STATIC VOID TestTableWarning(UINT32 Soc)
{
  Fixture(Soc);gBS->AllocatePages=WarningTableAllocate;
  EFI_SMMU_CR_DISPLAY_GROUP G={0};UINTN Free=mFreeCalls;
  assert(CrSmmuAcquireDisplayGroup(&mCrProtocol,Soc,71,NativeStopped,&DisplayTarget,&G)!=EFI_SUCCESS);
  assert(G.Token&&mDisplayGroup.Failed&&mSmmu.Domains[0].Poisoned&&mFreeCalls==Free);
  for(UINTN I=0;I<SMMU_TABLE_PAGES*SMMU_PAGE_SIZE;++I)assert(mPagePool[I]==0x5a);
  gBS->AllocatePages=MockAllocatePages;
  UINTN Writes=HardwareWrites,Alloc=mAllocateCalls;
  assert(CrSmmuAcquireDisplayGroup(&mCrProtocol,Soc,71,NativeStopped,&DisplayTarget,&G)!=EFI_SUCCESS);
  assert(HardwareWrites==Writes&&mAllocateCalls==Alloc&&mFreeCalls==Free);
}
STATIC VOID TestBuffers(UINT32 Soc)
{
  Fixture(Soc);BuffersFixture();EFI_SMMU_CR_DISPLAY_GROUP Group={0};
  assert(CrSmmuAcquireDisplayGroup(&mCrProtocol,Soc,71,NativeStopped,&DisplayTarget,&Group)==EFI_SUCCESS);
  assert(MockAllocatePages(AllocateAnyPages,EfiReservedMemoryType,2,&FramePhysical)==EFI_SUCCESS);
  EFI_SMMU_CR_DISPLAY_LEASE Frame={0},Command={0};
  assert(CrSmmuBindDisplayFrame(&mCrProtocol,&Group,(VOID *)(UINTN)FramePhysical,FramePhysical,6000,&Frame)==EFI_SUCCESS);
  assert(CrSmmuAllocateDisplayCommand(&mCrProtocol,&Group,256,&Command)==EFI_SUCCESS);
  UINTN UnpreparedFree=mFreeCalls;
  assert(CrSmmuFreeBuffer(&mIoMmuProtocol,1,(VOID *)(UINTN)mDisplayBuffers[1].Address)==EFI_ACCESS_DENIED&&mFreeCalls==UnpreparedFree);
  assert(CrSmmuPrepareDisplayBuffer(&mCrProtocol,&Group,&Frame)==EFI_SUCCESS);
  assert(CrSmmuPrepareDisplayBuffer(&mCrProtocol,&Group,&Command)==EFI_SUCCESS);
  assert(CrSmmuCheckDisplayBuffer(&mCrProtocol,&Group,&Frame)==EFI_SUCCESS);
  assert(CrSmmuCheckDisplayBuffer(&mCrProtocol,&Group,&Command)==EFI_SUCCESS);
  UINTN WritesBeforeReactivation=HardwareWrites;
  Hardware[HW_CB(3)/4]=1;
  assert(CrSmmuCheckDisplayBuffer(&mCrProtocol,&Group,&Frame)!=EFI_SUCCESS&&HardwareWrites==WritesBeforeReactivation);
  Hardware[HW_CB(3)/4]=0;
  assert(CrSmmuCheckDisplayBuffer(&mCrProtocol,&Group,&Frame)==EFI_SUCCESS);
  EFI_SMMU_CR_DISPLAY_BUFFER_INFO I;
  UINTN Reads=HardwareReads,Stops=StopChecks,Writes=HardwareWrites;
  assert(CrSmmuQueryDisplayBuffer(&mCrProtocol,&Group,&Frame,&I)==EFI_SUCCESS);
  assert(I.Flags==(CR_SMMU_DISPLAY_BORROWED_FRAME|CR_SMMU_DISPLAY_CPU_READY|CR_SMMU_DISPLAY_MAPPING|CR_SMMU_DISPLAY_READY|CR_SMMU_DISPLAY_OWNER_READY));
  assert(HardwareReads==Reads&&HardwareWrites==Writes&&StopChecks==Stops);
  DisplayTarget.Interrupt^=1;
  assert(CrSmmuQueryDisplayBuffer(&mCrProtocol,&Group,&Frame,&I)==EFI_SUCCESS&&!(I.Flags&CR_SMMU_DISPLAY_OWNER_READY));
  DisplayTarget.Interrupt^=1;
  CR_SMMU_DOMAIN *Poison=FindDomainBySid(0x820);assert(Poison);Poison->Poisoned=TRUE;
  assert(CrSmmuQueryDisplayBuffer(&mCrProtocol,&Group,&Frame,&I)==EFI_SUCCESS&&!(I.Flags&CR_SMMU_DISPLAY_OWNER_READY));
  Poison->Poisoned=FALSE;
  assert(CrSmmuQueryDisplayBuffer(&mCrProtocol,&Group,&Frame,&I)==EFI_SUCCESS&&(I.Flags&CR_SMMU_DISPLAY_OWNER_READY));
  assert(HardwareReads==Reads&&HardwareWrites==Writes&&StopChecks==Stops);
  assert(I.HostAddress==FramePhysical&&I.Bytes==6000&&I.PageBytes==8192&&I.GrantedMask==15);
  CR_SMMU_MAPPING *Map=mDisplayBuffers[0].Mapping;
  assert(CrSmmuSetAttributeById(&mIoMmuProtocol,HW_BASE,0x820,Map,EDKII_IOMMU_ACCESS_READ)==EFI_ACCESS_DENIED);
  assert(CrSmmuSetAttribute(&mIoMmuProtocol,mHandleSegment0,Map,EDKII_IOMMU_ACCESS_READ)==EFI_ACCESS_DENIED);
  assert(CrSmmuUnmap(&mIoMmuProtocol,Map)==EFI_ACCESS_DENIED);
  assert(CrSmmuPreserveReadOnlyMapping(&mCrProtocol,Map)==EFI_ACCESS_DENIED);
  EFI_PHYSICAL_ADDRESS Dma=0;VOID *Mapping=NULL;UINTN Bytes=6000;
  assert(CrSmmuMap(&mIoMmuProtocol,EdkiiIoMmuOperationBusMasterRead64,(VOID *)(UINTN)FramePhysical,&Bytes,&Dma,&Mapping)==EFI_ACCESS_DENIED);
  assert(!Dma&&!Mapping);
  assert(CrSmmuPreserveDisplayFrame(&mCrProtocol,&Group,&Command)==EFI_UNSUPPORTED);
  assert(CrSmmuPreserveDisplayFrame(&mCrProtocol,&Group,&Frame)==EFI_SUCCESS&&Map->PreserveAtExit);
  EFI_SMMU_CR_DISPLAY_LEASE Copy=Frame;assert(CrSmmuCheckDisplayBuffer(&mCrProtocol,&Group,&Copy)==EFI_INVALID_PARAMETER);
  BufferAttributes[0]=EFI_MEMORY_WB|EFI_MEMORY_XP;
  assert(CrSmmuCheckDisplayBuffer(&mCrProtocol,&Group,&Frame)==EFI_ACCESS_DENIED);
  BufferAttributes[0]=EFI_MEMORY_WC|EFI_MEMORY_XP;
  Map->DomainAccess[7]=EDKII_IOMMU_ACCESS_READ;
  assert(CrSmmuCheckDisplayBuffer(&mCrProtocol,&Group,&Frame)!=EFI_SUCCESS);Map->DomainAccess[7]=0;
  UINTN Free=mFreeCalls;assert(CrSmmuReleaseDisplayBuffer(&mCrProtocol,&Group,&Command)==EFI_SUCCESS&&mFreeCalls==Free+1);
  Free=mFreeCalls;assert(CrSmmuReleaseDisplayBuffer(&mCrProtocol,&Group,&Frame)==EFI_SUCCESS&&mFreeCalls==Free);
  assert(!Frame.Token&&!Frame.Generation&&BufferAttributes[0]==(EFI_MEMORY_WB|EFI_MEMORY_XP));
  for(UINT32 Case=0;Case<4;++Case){
    Fixture(Soc);BuffersFixture();memset(&Group,0,sizeof(Group));memset(&Frame,0,sizeof(Frame));
    assert(CrSmmuAcquireDisplayGroup(&mCrProtocol,Soc,71,NativeStopped,&DisplayTarget,&Group)==EFI_SUCCESS);
    assert(MockAllocatePages(AllocateAnyPages,EfiReservedMemoryType,2,&FramePhysical)==EFI_SUCCESS);
    if(Case==0){mFailMemoryAttributeCall=1;}
    EFI_STATUS S=CrSmmuBindDisplayFrame(&mCrProtocol,&Group,(VOID *)(UINTN)FramePhysical,FramePhysical,6000,&Frame);
    if(Case==0){assert(S!=EFI_SUCCESS&&Frame.Token);mFailMemoryAttributeCall=0;assert(CrSmmuReleaseDisplayBuffer(&mCrProtocol,&Group,&Frame)==EFI_SUCCESS);continue;}
    assert(S==EFI_SUCCESS);
    if(Case==1)HardwareTimeout=TRUE;
    S=CrSmmuPrepareDisplayBuffer(&mCrProtocol,&Group,&Frame);
    if(Case==1)assert(S!=EFI_SUCCESS&&mDisplayBuffers[0].Mapping);else assert(S==EFI_SUCCESS);
    if(Case==2)HardwareTimeout=TRUE;
    if(Case==3){FailStopCheck=StopChecks+1;StopFailure=EFI_WARN_UNKNOWN_GLYPH;}
    UINTN W=HardwareWrites;Free=mFreeCalls;
    assert(CrSmmuReleaseDisplayBuffer(&mCrProtocol,&Group,&Frame)!=EFI_SUCCESS&&Frame.Token&&mFreeCalls==Free);
    assert(mDisplayBuffers[0].Mapping&&mDisplayBuffers[0].CleanupOnly);
    if(Case==3){assert(HardwareWrites==W);FailStopCheck=0;assert(CrSmmuReleaseDisplayBuffer(&mCrProtocol,&Group,&Frame)==EFI_SUCCESS);}
  }
}
STATIC VOID TestCommandFailures(UINT32 Soc)
{
  for(UINT32 Case=0;Case<3;++Case){
    Fixture(Soc);BuffersFixture();EFI_SMMU_CR_DISPLAY_GROUP G={0};EFI_SMMU_CR_DISPLAY_LEASE C={0};
    assert(CrSmmuAcquireDisplayGroup(&mCrProtocol,Soc,71,NativeStopped,&DisplayTarget,&G)==EFI_SUCCESS);
    UINTN Free=mFreeCalls;
    if(Case==0)mAllocateStatus=EFI_OUT_OF_RESOURCES;
    if(Case==1)gBS->AllocatePages=WarningTableAllocate;
    EFI_STATUS S=CrSmmuAllocateDisplayCommand(&mCrProtocol,&G,256,&C);
    if(Case<2){
      assert(S!=EFI_SUCCESS&&C.Token);EFI_SMMU_CR_DISPLAY_BUFFER_INFO I;
      assert(CrSmmuQueryDisplayBuffer(&mCrProtocol,&G,&C,&I)==EFI_SUCCESS);
      assert((I.Flags&CR_SMMU_DISPLAY_CLEANUP_ONLY)&&!(I.Flags&CR_SMMU_DISPLAY_CPU_READY));
      if(Case==0){mAllocateStatus=EFI_SUCCESS;assert(CrSmmuReleaseDisplayBuffer(&mCrProtocol,&G,&C)==EFI_SUCCESS&&!C.Token);}
      else{
        assert(I.Flags&CR_SMMU_DISPLAY_ALLOCATION_UNCERTAIN);
        assert(CrSmmuReleaseDisplayBuffer(&mCrProtocol,&G,&C)!=EFI_SUCCESS&&C.Token);
      }
      assert(mFreeCalls==Free);continue;
    }
    assert(S==EFI_SUCCESS);mFreeStatus=EFI_DEVICE_ERROR;
    assert(CrSmmuReleaseDisplayBuffer(&mCrProtocol,&G,&C)==EFI_DEVICE_ERROR&&C.Token);
    mFreeStatus=EFI_SUCCESS;
    assert(CrSmmuReleaseDisplayBuffer(&mCrProtocol,&G,&C)==EFI_SUCCESS&&!C.Token);
    assert(mFreeCalls==Free+2);
  }
}
typedef struct {EFI_SMMU_CR_DISPLAY_GROUP *Group;EFI_SMMU_CR_DISPLAY_LEASE *Frame,*Command;} EXIT_BUFFERS;
STATIC EFI_STATUS EFIAPI ExitPrepareCheck(VOID *Context)
{
  EXIT_BUFFERS *E=Context;assert(mSmmu.AfterExitBootServices&&!mSmmu.ExitComplete);
  UINTN Writes=HardwareWrites,Alloc=mAllocateCalls,Free=mFreeCalls;
  assert(CrSmmuCheckDisplayBuffer(&mCrProtocol,E->Group,E->Frame)==EFI_SUCCESS);
  assert(CrSmmuCheckDisplayBuffer(&mCrProtocol,E->Group,E->Command)==EFI_SUCCESS);
  EFI_SMMU_CR_DISPLAY_BUFFER_INFO I;
  assert(CrSmmuQueryDisplayBuffer(&mCrProtocol,E->Group,E->Frame,&I)==EFI_SUCCESS&&I.Revision==1);
  assert(CrSmmuPrepareDisplayBuffer(&mCrProtocol,E->Group,E->Frame)!=EFI_SUCCESS);
  assert(CrSmmuReleaseDisplayBuffer(&mCrProtocol,E->Group,E->Command)!=EFI_SUCCESS);
  assert(CrSmmuPreserveDisplayFrame(&mCrProtocol,E->Group,E->Frame)!=EFI_SUCCESS);
  assert(CrSmmuAcquireDisplayGroup(&mCrProtocol,DisplayTarget.Soc,71,NativeStopped,&DisplayTarget,E->Group)!=EFI_SUCCESS);
  assert(HardwareWrites==Writes&&mAllocateCalls==Alloc&&mFreeCalls==Free);
  return EFI_SUCCESS;
}
STATIC VOID TestExitPreparation(UINT32 Soc)
{
  Fixture(Soc);BuffersFixture();EFI_SMMU_CR_DISPLAY_GROUP G={0};
  EFI_SMMU_CR_DISPLAY_LEASE Frame={0},Command={0};
  assert(CrSmmuAcquireDisplayGroup(&mCrProtocol,Soc,71,NativeStopped,&DisplayTarget,&G)==EFI_SUCCESS);
  assert(MockAllocatePages(AllocateAnyPages,EfiReservedMemoryType,2,&FramePhysical)==EFI_SUCCESS);
  assert(CrSmmuBindDisplayFrame(&mCrProtocol,&G,(VOID *)(UINTN)FramePhysical,FramePhysical,6000,&Frame)==EFI_SUCCESS);
  assert(CrSmmuAllocateDisplayCommand(&mCrProtocol,&G,256,&Command)==EFI_SUCCESS);
  assert(CrSmmuPrepareDisplayBuffer(&mCrProtocol,&G,&Frame)==EFI_SUCCESS);
  assert(CrSmmuPrepareDisplayBuffer(&mCrProtocol,&G,&Command)==EFI_SUCCESS);
  assert(CrSmmuPreserveDisplayFrame(&mCrProtocol,&G,&Frame)==EFI_SUCCESS);
  EXIT_BUFFERS E={&G,&Frame,&Command};
  assert(CrSmmuSetExitPrepareObserver(&mCrProtocol,G.Token,ExitPrepareCheck,&E)==EFI_SUCCESS);
  UINTN Free=mFreeCalls;CrSmmuExitBootServices(NULL,NULL);
  assert(mSmmu.ExitComplete&&mSmmu.ExitStatus==EFI_SUCCESS&&mFreeCalls==Free);
  for(UINT32 I=0;I<CR_SMMU_MAX_DOMAINS;++I)if(mSmmu.Domains[I].InUse){
    assert(mSmmu.Domains[I].PreservedAtExit&&mDisplayBuffers[0].Mapping->DomainAccess[I]==EDKII_IOMMU_ACCESS_READ);
    assert(mDisplayBuffers[1].Mapping->DomainAccess[I]==0);
  }
  EFI_SMMU_CR_DISPLAY_BUFFER_INFO Info;
  assert(CrSmmuCheckDisplayBuffer(&mCrProtocol,&G,&Frame)!=EFI_SUCCESS);
  assert(CrSmmuQueryDisplayBuffer(&mCrProtocol,&G,&Frame,&Info)!=EFI_SUCCESS);
}
#endif
STATIC EFI_STATUS Acquire(EFI_SMMU_CR_DISPLAY_GROUP *G) {
#ifdef EXPECT_UNTYPED_ATTACH
  (VOID)G;return CrSmmuAttach(&mCrProtocol,0,0xffff,0x820);
#else
  return CrSmmuAcquireDisplayGroup(&mCrProtocol,DisplayTarget.Soc,71,NativeStopped,&DisplayTarget,G);
#endif
}
#ifndef CR_SMMU_DISPLAY_TEST_NO_MAIN
int main(void) {
  for(UINT32 Soc=0x8250;Soc<=0x8350;Soc+=0x100) {
    Fixture(Soc);EFI_SMMU_CR_DISPLAY_GROUP G={0};assert(Acquire(&G)==EFI_SUCCESS);
#ifndef EXPECT_UNTYPED_ATTACH
    assert(G.Token&&G.Generation&&StopChecks>2&&HardwareWrites);
    UINTN W=HardwareWrites,A=mAllocateCalls;assert(Acquire(&G)==EFI_SUCCESS&&HardwareWrites==W&&mAllocateCalls==A);
    EFI_SMMU_CR_DISPLAY_GROUP Copy=G;assert(Acquire(&Copy)==EFI_ACCESS_DENIED);
    assert(CrSmmuAcquireDisplayGroup(&mCrProtocol,Soc,71,ForeignStopped,&DisplayTarget,&G)==EFI_ACCESS_DENIED);
    assert(CrSmmuAttach(&mCrProtocol,0,0xffff,0x820)==EFI_UNSUPPORTED);
    assert(Hardware[(0x800+3*4)/4]==0x04020820&&Hardware[HW_CB(3)/4]==0);
    assert(mSmmu.Device.ReservedStreams[3]&&mSmmu.Device.ReservedBanks[3]&&mSmmu.Device.ReservedAsids[7]);
    for(UINTN I=0;I<4;++I){CR_SMMU_DOMAIN *D=FindDomainBySid(DisplayTarget.Streams[I]);assert(D&&D->Domain.InheritedStreamOwned);}
    TestDecodedInventory(Soc);
    TestDisabledEmptyInventory(Soc);
    TestForeignAsidSelection(Soc);
    TestForeignEmptyBank(Soc);
    TestExistingDomain(Soc);
    TestTableWarning(Soc);
    TestPortableRetries(Soc);
    TestBuffers(Soc);
    TestCommandFailures(Soc);
    TestExitPreparation(Soc);
    for(UINTN Case=0;Case<8;++Case){
      Fixture(Soc);memset(&G,0,sizeof(G));
      if(Case==0)Hardware[(0x800+3*4)/4]|=1U<<18;
      if(Case==1){Hardware[(0x800+4*4)/4]=0x80000099;Hardware[(0xc00+4*4)/4]=3;}
      if(Case==2)Hardware[(0x1000+3*4)/4]=0;
      if(Case==3)Hardware[(HW_CB(3)+0x2c)/4]=8U<<16;
      if(Case==4){Hardware[(0x800+4*4)/4]=0x80000099;Hardware[(0xc00+4*4)/4]=4;Hardware[(0x1000+4*4)/4]=0x1f300;Hardware[(0x1800+4*4)/4]=1;Hardware[(HW_CB(4)+0x10)/4]=0x38011;Hardware[(HW_CB(4)+0x24)/4]=7U<<16;Hardware[(HW_CB(4)+0x2c)/4]=7U<<16;}
      if(Case==5)Hardware[0]&=~0x400U;
      if(Case==6){FailStopCheck=1;StopFailure=EFI_WARN_UNKNOWN_GLYPH;}
      if(Case==7)DisplayTarget.Soc=0x8450;
      assert(Acquire(&G)!=EFI_SUCCESS&&!HardwareWrites&&!mAllocateCalls);
    }
    for(UINTN Case=0;Case<3;++Case){
      Fixture(Soc);memset(&G,0,sizeof(G));
      if(Case==0)RejectHardware=HW_BASE+0xc00+3*4;
      if(Case==1)HardwareTimeout=TRUE;
      if(Case==2)FailStopCheck=2;
      assert(Acquire(&G)!=EFI_SUCCESS&&G.Token&&HardwareWrites);
      W=HardwareWrites;A=mAllocateCalls;VOID *Token=G.Token;
      RejectHardware=0;HardwareTimeout=FALSE;FailStopCheck=0;
      if(Case==1)
        assert(Acquire(&G)!=EFI_SUCCESS&&G.Token==Token&&HardwareWrites==W&&mAllocateCalls==A&&!mFreeCalls);
      else
        assert(Acquire(&G)==EFI_SUCCESS&&G.Token==Token&&!mFreeCalls);
    }
#endif
  }
  puts("Display group actual DXE/core: both SoCs, shared masked route, malformed/shared banks, owner binding, typed READ buffers and terminal failure journals PASS");
  return 0;
}
#endif
