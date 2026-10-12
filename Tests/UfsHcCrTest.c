/** UFS resource sequencing, DMA contract and failed rollback tests. */
#ifndef _PCD_VALUE_PcdUfsLegacyCoherentDmaHandoff
#define _PCD_VALUE_PcdUfsLegacyCoherentDmaHandoff FALSE
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
#include "../Target/Sm8450/CrUfsTarget.c"
#include "../Driver/UfsHcCrDxe/UfsHcCrDxe.c"
STATIC CR_TARGET_UFS_CONTEXT mTestTarget;
STATIC BOOLEAN mMissingTarget;
CONST CR_TARGET_UFS_CONTEXT *CrDalGetUfsContext(VOID) { return mMissingTarget ? NULL : &mTestTarget; }
#define CR_UFS_NON_LANE_CLOCKS 7U

EFI_GUID gEfiClockCrProtocolGuid = { .Data1 = 1 };
EFI_GUID gEfiRpmhCrProtocolGuid = { .Data1 = 2 };
EFI_GUID gEfiGpioCrProtocolGuid = { .Data1 = 3 };
EFI_GUID gEfiInterconnectCrProtocolGuid = { .Data1 = 4 };
EFI_GUID gEfiSmmuCrProtocolGuid = { .Data1 = 5 };
EFI_GUID gEdkiiIoMmuProtocolGuid = { .Data1 = 6 };
EFI_BOOT_SERVICES *gBS;
STATIC EFI_CLOCK_CR_PROTOCOL mClock;
STATIC EFI_RPMH_CR_PROTOCOL mRpmh;
STATIC EFI_GPIO_CR_PROTOCOL mGpio;
STATIC EFI_INTERCONNECT_CR_PROTOCOL mIcb;
STATIC EFI_SMMU_CR_PROTOCOL mSmmu;
STATIC EDKII_IOMMU_PROTOCOL mIommu;
STATIC UINT32 mMmio[0x4000 / 4];
STATIC BOOLEAN mDomain, mCoreReset, mPhyReady, mResetReleased, mQrefEnabled;
STATIC BOOLEAN mClockOn[ARRAY_SIZE (mClocks)];
STATIC BOOLEAN mRailVote[ARRAY_SIZE (mRails)];
STATIC UINTN mPathCount, mAttachCount, mCalibrations, mAccessCount;
STATIC UINTN mClockEnables, mCoreResetCount;
STATIC INTN mFailVoltage, mFailClock;
STATIC BOOLEAN mFailPhy, mFailPath2, mFailRelease, mMissingClock;
STATIC BOOLEAN mFailResetDeassert;
STATIC BOOLEAN mFailGrant, mFailUnmap, mPartialMap;
STATIC UINTN mOutstandingMaps, mAllocCalls, mFreeCalls, mPools;
STATIC UINT64 mDmaAccess;
STATIC EDKII_IOMMU_OPERATION mDmaOperation;
STATIC UINT64 mAllocAttributes;
STATIC EFI_STATUS mAllocStatus;
STATIC EFI_ALLOCATE_TYPE mAllocType;
STATIC UINT32 mUicCoreCycles, mUic40nsCycles, mUicLcc;
STATIC BOOLEAN mFailUic;
STATIC UINT32 mHsCaps[4], mHsValues[0x10000];
STATIC UINTN mHsCount, mHsFailAt, mHsPowerChanges, mRefSettled;
STATIC BOOLEAN mFailRefGate;
STATIC EFI_STATUS mHsFailureStatus=EFI_DEVICE_ERROR;
STATIC UINT32 mPhyStage;
STATIC BOOLEAN mBootFloor;
STATIC EFI_STATUS mResetReadStatus;
STATIC BOOLEAN mBadResetConfig,mDropResetHigh,mDropResetLow;
STATIC EFI_STATUS mBootFloorStatus, mGrantStatus, mMapStatus, mUnmapStatus;
STATIC UINTN mGrantCalls, mGrantFailAt, mGrantMask, mAttachFailAt;
STATIC UINTN mAcquireUfsCalls, mAcquiredSids;
STATIC UINT16 mStoppedBusyOffset;
STATIC UINT32 mSavedHwVersion, mSavedHciVersion;
STATIC EFI_STATUS EFIAPI BootFloor(EFI_RPMH_CR_PROTOCOL *This) {
  assert(!mDomain);
  for(UINTN I=0;I<ARRAY_SIZE(mClockOn);I++) assert(!mClockOn[I]);
  mBootFloor = mBootFloorStatus == EFI_SUCCESS;
  return mBootFloorStatus;
}



STATIC EFI_STATUS EFIAPI ExecUic (
  EDKII_UFS_HC_DRIVER_INTERFACE *This, EDKII_UIC_COMMAND *Command)
{
  UINT32 *Value;
  UINT16 Attribute = Command->Arg1 >> 16;
  if (Attribute == 0x1587 || Attribute == 0x1581 || Attribute == 0x1561 ||
      (Command->Opcode == 2 && Attribute != 0xd002 && Attribute != 0x9007 && Attribute != 0x155e)) {
    mHsCount++;
    if (mHsFailAt == mHsCount) return mHsFailureStatus;
    if (Attribute == 0x1587) { Command->Arg3=mHsCaps[Command->Opcode==3]; return EFI_SUCCESS; }
    if (Attribute == 0x1581) { Command->Arg3=mHsCaps[2]; return EFI_SUCCESS; }
    if (Attribute == 0x1561) { Command->Arg3=mHsCaps[3]; return EFI_SUCCESS; }
    assert(Command->Opcode==2 && mRefSettled);
    assert(mMmio[CR_UFS_CFG1/4] & CR_UFS_DEV_REFCLK_EN);
    mHsValues[Attribute]=Command->Arg3;
    if (Attribute==0x1571) {
      assert(mHsValues[0x1584]==1 && mHsValues[0x1569]==1 && mHsValues[0x156a]==2);
      assert(mHsValues[0x15b0]==8191 && mHsValues[0x15b1]==65535 && mHsValues[0x15b2]==32767);
      assert(mHsValues[0x15b3]==8191 && mHsValues[0x15b4]==65535 && mHsValues[0x15b5]==32767);
      assert(mHsValues[0xd041]==8191 && mHsValues[0xd042]==65535 && mHsValues[0xd043]==32767);
      mHsPowerChanges++;
    }
    return EFI_SUCCESS;
  }
  if (mFailUic) return EFI_DEVICE_ERROR;
  assert (Command->Arg2 == 0);
  switch (Command->Arg1) {
    case 0x00410000: Command->Arg3 = 1; return EFI_SUCCESS;
    case 0xd0020000: Value = &mUicCoreCycles; break;
    case 0x90070000: Value = &mUic40nsCycles; break;
    case 0x155e0000: Value = &mUicLcc; break;
    default: assert (FALSE); return EFI_DEVICE_ERROR;
  }
  if (Command->Opcode == 1) Command->Arg3 = *Value;
  else { assert (Command->Opcode == 2); *Value = Command->Arg3; }
  return EFI_SUCCESS;
}

VOID EFIAPI DebugPrint (UINTN Level, CONST CHAR8 *Format, ...) { }
BOOLEAN EFIAPI DebugPrintEnabled (VOID) { return TRUE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled (CONST UINTN Level) { return TRUE; }
VOID *EFIAPI ZeroMem (VOID *Buffer, UINTN Size) { return memset (Buffer, 0, Size); }
VOID *EFIAPI AllocatePool (UINTN Size) { mPools++; return malloc (Size); }
VOID *EFIAPI AllocateZeroPool (UINTN Size) { mPools++; return calloc (1, Size); }
VOID EFIAPI FreePool (VOID *Buffer) { assert (mPools); mPools--; free (Buffer); }
VOID EFIAPI MemoryFence (VOID) { }
UINTN EFIAPI MicroSecondDelay (UINTN Delay) { if (Delay==50 && (mMmio[CR_UFS_CFG1/4]&CR_UFS_DEV_REFCLK_EN)) mRefSettled++; return Delay; }

STATIC UINTN MmioIndex (UINTN Address)
{
  assert (mDomain && mClockOn[2] && !mCoreReset);
  assert (Address >= CR_UFS_HCI_BASE && Address < CR_UFS_HCI_BASE + 0x4000);
  /* Linux QMP power_on enables ref/ref_aux/qref before PCS access.  A
     running prior firmware clock must not hide a missing cold-init vote. */
  if (Address >= CR_UFS_PHY_BASE) assert (mQrefEnabled);
  mAccessCount++;
  return (Address - CR_UFS_HCI_BASE) / 4;
}
UINT32 EFIAPI MmioRead32 (UINTN Address)
{
  UINTN Index = MmioIndex (Address);
  if (Address == CR_UFS_PHY_BASE + CR_UFS_PHY_READY) {
    mPhyReady = !mFailPhy;
    return mPhyReady ? 1 : 0;
  }
  if (mFailRefGate && Address==CR_UFS_HCI_BASE+CR_UFS_CFG1) return mMmio[Index]&~CR_UFS_DEV_REFCLK_EN;
  return mMmio[Index];
}
UINT32 EFIAPI MmioWrite32 (UINTN Address, UINT32 Value)
{
  UINTN Index = MmioIndex (Address);
  if (Address == CR_UFS_PHY_BASE + 0x94) {
    assert ((mMmio[CR_UFS_CFG1 / 4] & CR_UFS_PHY_SOFT_RESET) != 0);
    assert (!mClockOn[CR_UFS_NON_LANE_CLOCKS]);
    mCalibrations++; mPhyStage=0;
  }
  if (Address==CR_UFS_PHY_BASE+0x484 && Value==0xf5) { assert(mPhyStage==0); mPhyStage=1; }
  if (Address==CR_UFS_PHY_BASE+0x484 && Value==0xe5) { assert(mPhyStage==1); mPhyStage=2; }
  if (Address==CR_UFS_PHY_BASE+0x10c && Value==6) { assert(mPhyStage==(mUfs.MaxHsGear==4 ? 2U : 1U)); mPhyStage=3; }
  if (Address==CR_UFS_PHY_BASE+CR_UFS_PHY_START && (Value&1)) {
    assert(mPhyStage==3);
    assert(mMmio[(CR_UFS_PHY_BASE-CR_UFS_HCI_BASE+0x10c)/4]==6);
    if(mUfs.MaxHsGear==4) {
      assert(mMmio[(CR_UFS_PHY_BASE-CR_UFS_HCI_BASE+0x648)/4]==0x81);
      assert(mMmio[(CR_UFS_PHY_BASE-CR_UFS_HCI_BASE+0xa48)/4]==0x81);
      assert(mMmio[(CR_UFS_PHY_BASE-CR_UFS_HCI_BASE+0xc60)/4]==0x0a);
    }
  }
  mMmio[Index] = Value;
  return Value;
}
UINT32 EFIAPI MmioOr32 (UINTN Address, UINT32 Bits) {
  return MmioWrite32 (Address, MmioRead32 (Address) | Bits);
}
UINT32 EFIAPI MmioAnd32 (UINTN Address, UINT32 Bits) {
  return MmioWrite32 (Address, MmioRead32 (Address) & Bits);
}
UINT8 EFIAPI MmioRead8 (UINTN Address) { return (UINT8)MmioRead32 (Address); }
UINT16 EFIAPI MmioRead16 (UINTN Address) { return (UINT16)MmioRead32 (Address); }
UINT64 EFIAPI MmioRead64 (UINTN Address) { return MmioRead32 (Address); }
UINT8 EFIAPI MmioWrite8 (UINTN Address, UINT8 Value) { return (UINT8)MmioWrite32 (Address, Value); }
UINT16 EFIAPI MmioWrite16 (UINTN Address, UINT16 Value) { return (UINT16)MmioWrite32 (Address, Value); }
UINT64 EFIAPI MmioWrite64 (UINTN Address, UINT64 Value) { return MmioWrite32 (Address, (UINT32)Value); }

STATIC EFI_STATUS EFIAPI Locate (EFI_GUID *Guid, VOID *Registration, VOID **Interface)
{
  switch (Guid->Data1) {
    case 1: *Interface = mMissingClock ? NULL : &mClock; break;
    case 2: *Interface = &mRpmh; break;
    case 3: *Interface = &mGpio; break;
    case 4: *Interface = &mIcb; break;
    case 5: *Interface = &mSmmu; break;
    case 6: *Interface = &mIommu; break;
    default: assert (FALSE);
  }
  return *Interface ? EFI_SUCCESS : EFI_NOT_FOUND;
}
STATIC EFI_STATUS EFIAPI GpioSet (EFI_GPIO_CR_PROTOCOL *This, UINT16 Pin, UINT8 Value)
{
  assert (Pin == mTestTarget.DeviceReset.PinNumber);
  if (Value != 0) {
    assert (mPhyReady && mAttachCount && mClockOn[8]);
  }
  if (!((Value && mDropResetHigh) || (!Value && mDropResetLow))) mResetReleased = Value != 0;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI GetResetConfigMock (EFI_GPIO_CR_PROTOCOL *This, GpioConfigParams *Config)
{
  assert(Config->PinNumber == mTestTarget.DeviceReset.PinNumber);
  if(mResetReadStatus != EFI_SUCCESS) return mResetReadStatus;
  *Config=mTestTarget.DeviceReset;
  Config->OutputValue=mResetReleased ? GPIO_VALUE_HIGH : GPIO_VALUE_LOW;
  if(mBadResetConfig) Config->Pull=GPIO_PULL_UNCHANGE;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI GpioConfig (EFI_GPIO_CR_PROTOCOL *This, GpioConfigParams *Config)
{
  assert (Config->PinNumber == mTestTarget.DeviceReset.PinNumber && Config->OutputEnable);
  assert (Config->FunctionSel == GPIO_FUNC_UNCHANGE);
  assert (Config->Pull == mTestTarget.DeviceReset.Pull);
  assert (Config->DriveStrength == mTestTarget.DeviceReset.DriveStrength);
  assert (Config->OutputValue == GPIO_VALUE_LOW);
  return GpioSet (This, Config->PinNumber, 0);
}
STATIC EFI_STATUS EFIAPI Gdsc (EFI_CLOCK_CR_PROTOCOL *This, CONST CHAR8 *Controller,
                              CONST CHAR8 *Id, BOOLEAN Enable)
{
  if (Enable) assert(mBootFloor);
  mDomain = Enable;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Reset (EFI_CLOCK_CR_PROTOCOL *This, CONST CHAR8 *Controller,
                               CONST CHAR8 *Id, BOOLEAN Assert)
{
  assert (strcmp (Id, "gcc_ufs_phy_bcr") == 0);
  assert (mDomain && mClockOn[2] && !mResetReleased);
  if (!Assert && mFailResetDeassert) return EFI_DEVICE_ERROR;
  mCoreReset = Assert;
  if (Assert) { UINT32 Param=mMmio[CR_UFS_PARAM0/4]; mCoreResetCount++; memset (mMmio, 0, sizeof (mMmio)); mMmio[CR_UFS_PARAM0/4]=Param; mMmio[0xe4/4]=mSavedHwVersion; mMmio[0x08/4]=mSavedHciVersion; }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Clock (EFI_CLOCK_CR_PROTOCOL *This, CONST CHAR8 *Controller,
                               CONST CHAR8 *Id, UINT64 Rate, BOOLEAN Enable)
{
  UINTN Index;
  for (Index = 0; Index < ARRAY_SIZE (mClocks); Index++) {
    if (strcmp (Id, mTestTarget.Clocks[Index].Name) == 0) break;
  }
  assert (Index < ARRAY_SIZE (mClocks));
  if (Enable) {
    if (Index <= 2) assert(Rate==300000000);
    if (Index >= CR_UFS_NON_LANE_CLOCKS) assert (mPhyReady);
    mClockEnables++;
    mClockOn[Index] = TRUE;
    if (strcmp (Id, mTestTarget.Clocks[6].Name) == 0) mQrefEnabled = TRUE;
    if ((INTN)Index == mFailClock) return EFI_DEVICE_ERROR;
  } else {
    mClockOn[Index] = FALSE;
    if (strcmp (Id, mTestTarget.Clocks[6].Name) == 0) mQrefEnabled = FALSE;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI AcquireClockMock (EFI_CLOCK_CR_PROTOCOL *P,
  CONST CHAR8 *C, CONST CHAR8 *N, UINT64 R, BOOLEAN *Owned)
{
  EFI_STATUS Status = Clock(P, C, N, R, TRUE);
  *Owned = TRUE; /* This wire mock asserts the gate before its injected error. */
  return Status;
}
STATIC EFI_STATUS EFIAPI AcquireGdscMock (EFI_CLOCK_CR_PROTOCOL *P,
  CONST CHAR8 *C, CONST CHAR8 *N, BOOLEAN *Owned)
{
  EFI_STATUS Status = Gdsc(P, C, N, TRUE);
  *Owned = !EFI_ERROR(Status);
  return Status;
}
STATIC UINTN RailIndex (CONST CHAR8 *Id)
{
  UINTN Index;
  for (Index = 0; Index < ARRAY_SIZE (mRails); Index++)
    if (strcmp (Id, mTestTarget.Rails[Index].Name) == 0) return Index;
  assert (FALSE); return 0;
}
STATIC EFI_STATUS EFIAPI Voltage (EFI_RPMH_CR_PROTOCOL *This, CONST CHAR8 *Id, UINT32 Value)
{
  UINTN Index = RailIndex (Id);
  assert (Value == mTestTarget.Rails[Index].VoltageMv);
  mRailVote[Index] = TRUE;
  return (INTN)Index == mFailVoltage ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Mode (EFI_RPMH_CR_PROTOCOL *This, CONST CHAR8 *Id, UINT8 Value)
{ assert (Value == 7); return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI Rail (EFI_RPMH_CR_PROTOCOL *This, CONST CHAR8 *Id, BOOLEAN Enable)
{
  if (!Enable && mFailRelease) return EFI_DEVICE_ERROR;
  mRailVote[RailIndex (Id)] = Enable;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI AcquireRail (EFI_RPMH_CR_PROTOCOL *This, CONST CHAR8 *Id,
                                     UINT32 VoltageMv, UINT8 RequestedMode, BOOLEAN *Owned)
{
  EFI_STATUS Status;
  *Owned = TRUE;
  Status = Voltage (This, Id, VoltageMv);
  if (!EFI_ERROR (Status)) Status = Mode (This, Id, RequestedMode);
  if (!EFI_ERROR (Status)) Status = Rail (This, Id, TRUE);
  return Status;
}
STATIC EFI_STATUS EFIAPI Acquire (EFI_INTERCONNECT_CR_PROTOCOL *This, CONST CHAR8 *Id,
                                 UINT32 Src, UINT32 Dst, INTERCONNECT_PATH_HANDLE *Path)
{
  if (Src == 0x3002 && mFailPath2) return EFI_DEVICE_ERROR;
  *Path = ++mPathCount;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Bandwidth (EFI_INTERCONNECT_CR_PROTOCOL *This,
                                    INTERCONNECT_PATH_HANDLE Path, UINT64 Avg, UINT64 Peak)
{ assert (Path != INTERCONNECT_INVALID_PATH); return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI Release (EFI_INTERCONNECT_CR_PROTOCOL *This, INTERCONNECT_PATH_HANDLE Path)
{ assert (mPathCount); mPathCount--; return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI Attach (EFI_SMMU_CR_PROTOCOL *This, UINT16 Segment, UINT16 Bdf, UINT16 Sid)
{
  assert(mAttachCount < mTestTarget.SidCount && Sid == mTestTarget.Sids[mAttachCount] && !mResetReleased);
  if (mAttachFailAt == mAttachCount+1) return EFI_DEVICE_ERROR;
  mAttachCount++; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI AcquireUfs (EFI_SMMU_CR_PROTOCOL *This, UINT64 Base, UINT16 Sid,
    EFI_SMMU_CR_UFS_VERIFY_STOPPED Check, VOID *Context)
{
  assert(This==&mSmmu&&Base==mTestTarget.HciBase&&Check&&Context==&mUfs);
  mAcquireUfsCalls++;
  if(mStoppedBusyOffset!=MAX_UINT16)mMmio[mStoppedBusyOffset/4]=1;
  EFI_STATUS Status=Check(Context);
  if(Status!=EFI_SUCCESS)return Status;
  UINTN Index;
  for(Index=0;Index<mTestTarget.SidCount;Index++)if(mTestTarget.Sids[Index]==Sid)break;
  assert(Index<mTestTarget.SidCount);
  if(mAcquiredSids&(1U<<Index))return EFI_SUCCESS;
  Status=Attach(This,0,0xffff,Sid);
  if(Status==EFI_SUCCESS)mAcquiredSids|=1U<<Index;
  return Status;
}
STATIC EFI_STATUS EFIAPI DmaAllocate (EDKII_IOMMU_PROTOCOL *This, EFI_ALLOCATE_TYPE Type,
                  EFI_MEMORY_TYPE MemoryType, UINTN Pages, VOID **Address, UINT64 Attributes)
{
  mAllocType = Type; mAllocAttributes = Attributes; mAllocCalls++;
  if (mAllocStatus != EFI_SUCCESS) { *Address = NULL; return mAllocStatus; }
  *Address = calloc (Pages, EFI_PAGE_SIZE); return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI DmaFree (EDKII_IOMMU_PROTOCOL *This, UINTN Pages, VOID *Address)
{ if (mOutstandingMaps) return EFI_ACCESS_DENIED; mFreeCalls++; free (Address); return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI DmaMap (EDKII_IOMMU_PROTOCOL *This, EDKII_IOMMU_OPERATION Op,
                               VOID *Address, UINTN *Length, EFI_PHYSICAL_ADDRESS *Device, VOID **Map)
{
  if (mPartialMap) *Length /= 2;
  mDmaOperation = Op; mOutstandingMaps++; *Map = (VOID *)0x55; *Device = 0x40001000;
  return mMapStatus;
}
STATIC EFI_STATUS EFIAPI DmaGrant (EDKII_IOMMU_PROTOCOL *This, UINT64 Base, UINT32 Sid,
                                 VOID *Map, UINT64 Access)
{
  UINTN Index;
  assert(Base == mTestTarget.SmmuBase && Map==(VOID *)0x55);
  for(Index=0;Index<mTestTarget.SidCount;Index++) if(Sid==mTestTarget.Sids[Index]) break;
  assert(Index<mTestTarget.SidCount);
  mDmaAccess=Access; mGrantCalls++; mGrantMask|=1U<<Index;
  if(mGrantFailAt==mGrantCalls) return mGrantStatus;
  return mFailGrant ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI DmaUnmap (EDKII_IOMMU_PROTOCOL *This, VOID *Map)
{
  if (mFailUnmap) return EFI_DEVICE_ERROR;
  if (mUnmapStatus != EFI_SUCCESS) return mUnmapStatus;
  mGrantMask=0;
  assert (mOutstandingMaps); mOutstandingMaps--; return EFI_SUCCESS;
}
STATIC VOID Setup (VOID)
{
  STATIC EFI_BOOT_SERVICES Services;
  assert (!mPools);
  memset (&mUfs, 0, sizeof (mUfs));
  mTestTarget=mUfsTarget; mMissingTarget=FALSE;
  mUfs.MemPath = mUfs.CfgPath = INTERCONNECT_INVALID_PATH;
  memset (mMmio, 0, sizeof (mMmio));
  memset (mClockOn, 0, sizeof (mClockOn));
  memset (mRailVote, 0, sizeof (mRailVote));
  mClock.Revision=EFI_CLOCK_CR_PROTOCOL_REVISION; mClock.AcquireClock=AcquireClockMock; mClock.AcquireGdsc=AcquireGdscMock;
  mClock.SetClock = Clock; mClock.SetGdsc = Gdsc; mClock.SetReset = Reset;
  mRpmh.Revision = EFI_RPMH_CR_PROTOCOL_REVISION; mRpmh.EnsureBootFloor=BootFloor;
  mRpmh.RpmhEnableVreg = Rail; mRpmh.RpmhSetVregVoltage = Voltage;
  mRpmh.RpmhSetVregMode = Mode; mRpmh.AcquireVreg = AcquireRail;
  mGpio.SetIoValue = GpioSet;
  mGpio.ConfigGpio = GpioConfig; mGpio.GetGpioConfig=GetResetConfigMock;
  mIcb.AcquirePath = Acquire; mIcb.SetBandwidth = Bandwidth; mIcb.ReleasePath = Release;
  mSmmu.Attach = Attach;
  mSmmu.Revision=EFI_SMMU_CR_PROTOCOL_REVISION;mSmmu.AcquireUfsStream=AcquireUfs;
  mIommu.Revision = EDKII_IOMMU_PROTOCOL_REVISION;
  mIommu.AllocateBuffer = DmaAllocate; mIommu.FreeBuffer = DmaFree;
  mIommu.Map = DmaMap; mIommu.Unmap = DmaUnmap; mIommu.SetAttributeById = DmaGrant;
  Services.LocateProtocol = Locate; gBS = &Services;
  mDomain = mCoreReset = mPhyReady = mResetReleased = mQrefEnabled = FALSE;
  mPathCount = mAttachCount = mCalibrations = mAccessCount = 0;
  mClockEnables = mCoreResetCount = 0;
  mFailVoltage = mFailClock = -1;
  mFailPhy = mFailPath2 = mFailRelease = mMissingClock = FALSE;
  mFailResetDeassert = FALSE;
  mFailGrant = mFailUnmap = mPartialMap = FALSE;
  mOutstandingMaps = mAllocCalls = mFreeCalls = 0;
  mAllocStatus = EFI_SUCCESS;
  mMmio[CR_UFS_PARAM0/4]=4U<<4;
  mHsCaps[0]=mHsCaps[1]=4; mHsCaps[2]=mHsCaps[3]=2;
  memset(mHsValues,0,sizeof(mHsValues));
  mHsCount=mHsFailAt=mHsPowerChanges=mRefSettled=0;
  mFailRefGate=FALSE;
  mBootFloor=FALSE; mBootFloorStatus=EFI_SUCCESS;
  mResetReadStatus=EFI_SUCCESS; mBadResetConfig=mDropResetHigh=mDropResetLow=FALSE;
  mGrantCalls=mGrantFailAt=mGrantMask=mAttachFailAt=0;
  mAcquireUfsCalls=mAcquiredSids=0;mStoppedBusyOffset=MAX_UINT16;
  mGrantStatus=EFI_DEVICE_ERROR; mMapStatus=mUnmapStatus=EFI_SUCCESS;
  mSavedHwVersion=4U<<28; mSavedHciVersion=0x30000;
  mUicLcc=1;

}
STATIC VOID AssertClean (VOID)
{
  UINTN Index;
  assert (!mDomain && !mResetReleased && !mPathCount && !mUfs.ResourcesReady);
  for (Index = 0; Index < ARRAY_SIZE (mClockOn); Index++) assert (!mClockOn[Index]);
  for (Index = 0; Index < ARRAY_SIZE (mRailVote); Index++) assert (!mRailVote[Index]);
}
STATIC UINTN mFaultCalls;
STATIC EFI_STATUS EFIAPI FaultDumpMock (EFI_SMMU_CR_PROTOCOL *This,
                                       UINT16 Segment, UINT16 Bdf, UINT16 Sid)
{
  assert(This==&mSmmu && Segment==0 && Bdf==0xFFFF && Sid==0xE0);
  assert(mUfs.ResourcesReady && mOutstandingMaps==1 && mMmio[0x34/4]==1);
  mFaultCalls++; return EFI_SUCCESS;
}
STATIC VOID TestFatalSnapshot (VOID)
{
  UINT32 Is=0, Value=1; UINTN Length=16, Accesses;
  EFI_PHYSICAL_ADDRESS Device; VOID *Buffer, *Mapping;
  for (unsigned Old=0; Old<2; Old++) {
    Setup(); mFaultCalls=0;
    mSmmu.Revision=Old ? EFI_SMMU_CR_EXIT_UFS_COHERENT_REVISION : EFI_SMMU_CR_PROTOCOL_REVISION;
    mSmmu.DumpFault=FaultDumpMock;
    assert(CrUfsAllocateBuffer(&mUfs.Host,AllocateAnyPages,EfiBootServicesData,1,&Buffer,0)==EFI_SUCCESS);
    assert(CrUfsMap(&mUfs.Host,EdkiiUfsHcOperationBusMasterCommonBuffer,Buffer,&Length,&Device,&Mapping)==EFI_SUCCESS);
    mMmio[0x34/4]=1; mMmio[0x20/4]=0x405; /* Completion plus generic UIC. */
    assert(CrUfsRead(&mUfs.Host,EfiUfsHcWidthUint32,0x20,1,&Is)==EFI_SUCCESS && !mUfs.FatalError && !mFaultCalls);
    mMmio[0x20/4]=0x20405;
    UINT32 SavedMmio[ARRAY_SIZE(mMmio)]; memcpy(SavedMmio,mMmio,sizeof(mMmio));
    UINTN SavedPools=mPools;
    assert(CrUfsRead(&mUfs.Host,EfiUfsHcWidthUint32,0x20,1,&Is)==EFI_SUCCESS && Is==0x20405);
    assert(mUfs.FatalError==0x20000 && mFaultCalls==!Old);
    assert(!memcmp(SavedMmio,mMmio,sizeof(mMmio)) && mPools==SavedPools);
    assert(CrUfsRead(&mUfs.Host,EfiUfsHcWidthUint32,0x20,1,&Is)==EFI_SUCCESS && mFaultCalls==!Old);
    Accesses=mAccessCount;
    assert(CrUfsWrite(&mUfs.Host,EfiUfsHcWidthUint32,0x58,1,&Value)==EFI_DEVICE_ERROR);
    assert(CrUfsWrite(&mUfs.Host,EfiUfsHcWidthUint32,0x34,1,&Value)==EFI_DEVICE_ERROR);
    assert(CrUfsPlatformCallback((VOID *)1,EdkiiUfsHcPreHce,NULL)==EFI_DEVICE_ERROR);
    assert(mAccessCount==Accesses);
    VOID *Extra=NULL; UINTN Bytes=16;
    assert(CrUfsMap(&mUfs.Host,EdkiiUfsHcOperationBusMasterRead,Buffer,&Bytes,&Device,&Extra)==EFI_DEVICE_ERROR && !Extra && !Device);
    assert(CrUfsAllocateBuffer(&mUfs.Host,AllocateAnyPages,EfiBootServicesData,1,&Extra,0)==EFI_DEVICE_ERROR && !Extra);
    Value=0;
    assert(CrUfsWrite(&mUfs.Host,EfiUfsHcWidthUint32,0x34,1,&Value)==EFI_SUCCESS);
    assert(CrUfsUnmap(&mUfs.Host,Mapping)==EFI_SUCCESS);
    assert(CrUfsFreeBuffer(&mUfs.Host,1,Buffer)==EFI_SUCCESS);
    assert(CrUfsReleaseResources()==EFI_SUCCESS); AssertClean(); Value=1;
  }
}

STATIC VOID TestCoherentAllocation (VOID)
{
  VOID *Buffer;
  UINTN Calls;
  Setup ();
  assert (mTestTarget.DmaCoherent && CrTargetUfsValid (&mTestTarget));
  mTestTarget.DmaCoherent = 2;
  assert (!CrTargetUfsValid (&mTestTarget));
  mTestTarget.DmaCoherent = TRUE;
  // MU's default Attributes=0 must still allocate WB on the coherent target.
  assert (CrUfsAllocateBuffer (&mUfs.Host, AllocateAnyPages, EfiBootServicesData,
            1, &Buffer, 0) == EFI_SUCCESS);
  assert (mAllocAttributes == EDKII_IOMMU_ATTRIBUTE_MEMORY_CACHED);
  assert (CrUfsFreeBuffer (&mUfs.Host, 1, Buffer) == EFI_SUCCESS);
  Calls = mAllocCalls;
  assert (CrUfsAllocateBuffer (&mUfs.Host, AllocateAnyPages, EfiBootServicesData,
            1, &Buffer, EDKII_IOMMU_ATTRIBUTE_MEMORY_WRITE_COMBINE) == EFI_UNSUPPORTED);
  assert (Buffer == NULL && mAllocCalls == Calls);
  assert (CrUfsAllocateBuffer (&mUfs.Host, AllocateAnyPages, EfiBootServicesData,
            1, &Buffer, EDKII_IOMMU_ATTRIBUTE_MEMORY_WRITE_COMBINE |
                        EDKII_IOMMU_ATTRIBUTE_MEMORY_CACHED) == EFI_UNSUPPORTED);
  assert (Buffer == NULL && mAllocCalls == Calls);
  for (UINTN Failure = 0; Failure < 2; Failure++) {
    mAllocStatus = Failure ? EFI_OUT_OF_RESOURCES : EFI_UNSUPPORTED;
    Calls = mAllocCalls;
    assert (CrUfsAllocateBuffer (&mUfs.Host, AllocateAnyPages, EfiBootServicesData,
              1, &Buffer, 0) == mAllocStatus);
    assert (Buffer == NULL && mAllocCalls == Calls + 1);
    assert (mAllocAttributes == EDKII_IOMMU_ATTRIBUTE_MEMORY_CACHED);
  }
  assert (CrUfsReleaseResources () == EFI_SUCCESS); AssertClean ();
  Setup ();
  mTestTarget.DmaCoherent = FALSE;
  assert (CrUfsAllocateBuffer (&mUfs.Host, AllocateAnyPages, EfiBootServicesData,
            1, &Buffer, 0) == EFI_SUCCESS);
  assert (mAllocAttributes == 0);
  assert (CrUfsFreeBuffer (&mUfs.Host, 1, Buffer) == EFI_SUCCESS);
  assert (CrUfsReleaseResources () == EFI_SUCCESS); AssertClean ();
}

int main (void)
{
  UINTN Base, Length, Index;
  UINT32 Value;
  VOID *Buffer, *Mapping;
  EFI_PHYSICAL_ADDRESS Device;
  EDKII_UFS_HC_DRIVER_INTERFACE Driver = { .UfsExecUicCommand = ExecUic };
  TestCoherentAllocation ();
  Setup ();
  assert (CrUfsGetMmioBar (&mUfs.Host, &Base) == EFI_SUCCESS);
  assert (Base == CR_UFS_HCI_BASE && mUfs.ResourcesReady && mResetReleased);
  assert (mCoreResetCount == 1 && mCalibrations == 1);
  assert (mMmio[(CR_UFS_PHY_BASE - CR_UFS_HCI_BASE + 0x884) / 4] == 0xe5);
  assert (mMmio[(CR_UFS_PHY_BASE - CR_UFS_HCI_BASE + 0xb20) / 4] == 0x24);
  assert (CrUfsRead (&mUfs.Host, EfiUfsHcWidthUint32, 0, 1, &Value) == EFI_SUCCESS);
  assert (CrUfsPlatformCallback ((VOID *)1, EdkiiUfsHcPreHce, NULL) == EFI_SUCCESS);
  assert (mClockEnables == ARRAY_SIZE (mClocks) && mAttachCount == 1);
  /* Seed every timing bit, including bit 6, and unrelated upper bits.
     Reprogramming must replace the whole field while preserving others. */
  mUicCoreCycles = MAX_UINT32; mUic40nsCycles = MAX_UINT32; mUicLcc = 1;
  mMmio[(CR_UFS_PHY_BASE - CR_UFS_HCI_BASE + 0xd48) / 4] = MAX_UINT32;
  assert (CrUfsPlatformCallback ((VOID *)1, EdkiiUfsHcPostHce, &Driver) == EFI_SUCCESS);
  assert (CrUfsPlatformCallback ((VOID *)1, EdkiiUfsHcPreLinkStartup, &Driver) == EFI_SUCCESS);
  assert (mUicCoreCycles == ((MAX_UINT32 & ~(0x0fff0000U | BIT8)) | (300U << 16)));
  assert (mUic40nsCycles == ((MAX_UINT32 & ~0x7fU) | 12));
  assert (mUicLcc == 0 && mMmio[0xc0 / 4] == 300);
  assert (mMmio[(CR_UFS_PHY_BASE - CR_UFS_HCI_BASE + 0xd48) / 4] == MAX_UINT32);
  assert (CrUfsPlatformCallback ((VOID *)1, EdkiiUfsHcPostLinkStartup, &Driver) == EFI_SUCCESS);
  assert (mMmio[(CR_UFS_PHY_BASE - CR_UFS_HCI_BASE + 0xd48) / 4] == BIT1);
  assert(mHsValues[0x1583]==4 && mHsValues[0x1568]==4 && mHsValues[0x15d4]==1);
  assert(mHsValues[0x1580]==2 && mHsValues[0x1560]==2 && mHsValues[0x1571]==0x11);
  assert(mHsPowerChanges==1);
  mHsCaps[1]=3; mHsCaps[2]=1; mHsCaps[3]=4;
  assert (CrUfsPlatformCallback ((VOID *)1, EdkiiUfsHcPostLinkStartup, &Driver) == EFI_UNSUPPORTED);
  mHsCaps[3]=1;
  assert (CrUfsPlatformCallback ((VOID *)1, EdkiiUfsHcPostLinkStartup, &Driver) == EFI_SUCCESS);
  assert(mHsValues[0x1583]==3 && mHsValues[0x1568]==3 && mHsValues[0x15d4]==3);
  assert(mHsValues[0x1580]==1 && mHsValues[0x1560]==1);
  /* The Linux link contract rejects zero lanes and a report beyond this
     SM8450 controller's two QMP data lanes instead of silently clamping it. */
  mHsCaps[2]=0; mHsCaps[3]=0;
  assert (CrUfsPlatformCallback ((VOID *)1, EdkiiUfsHcPostLinkStartup, &Driver) == EFI_UNSUPPORTED);
  mHsCaps[2]=3; mHsCaps[3]=3;
  assert (CrUfsPlatformCallback ((VOID *)1, EdkiiUfsHcPostLinkStartup, &Driver) == EFI_UNSUPPORTED);
  mHsCaps[2]=mHsCaps[3]=2;
  assert(CrUfsPlatformCallback((VOID *)1,EdkiiUfsHcPostLinkStartup,NULL)==EFI_UNSUPPORTED);
  /* Inject an error at every capability, setup, timeout and final power command. */
  for(Index=1; Index<=22; Index++) {
    mHsCount=0; mHsFailAt=Index; mHsPowerChanges=0;
    assert(CrUfsPlatformCallback((VOID *)1,EdkiiUfsHcPostLinkStartup,&Driver)==EFI_DEVICE_ERROR);
    assert(!mHsPowerChanges && mHsCount==Index);
  }
  mHsCount=0; mHsFailAt=22; mHsFailureStatus=EFI_TIMEOUT;
  assert(CrUfsPlatformCallback((VOID *)1,EdkiiUfsHcPostLinkStartup,&Driver)==EFI_TIMEOUT);
  assert(!mHsPowerChanges); mHsFailureStatus=EFI_DEVICE_ERROR;
  mHsFailAt=0;
  mHsCaps[0]=1;
  assert(CrUfsNegotiateHs(&Driver)==EFI_SUCCESS && mHsValues[0x1568]==1 && mHsValues[0x1583]==1);
  mHsCaps[0]=4;
  for(Index=0;Index<4;Index++) {
    UINT32 Saved=mHsCaps[Index]; mHsCaps[Index]=0;
    assert(CrUfsPlatformCallback((VOID *)1,EdkiiUfsHcPostLinkStartup,&Driver)==EFI_UNSUPPORTED);
    mHsCaps[Index]=Saved;
  }
  mFailRefGate=TRUE;
  assert(CrUfsPlatformCallback((VOID *)1,EdkiiUfsHcPostLinkStartup,&Driver)==EFI_DEVICE_ERROR);
  mFailRefGate=FALSE;
  mBootFloor=FALSE; mBootFloorStatus=EFI_SUCCESS;
  mResetReadStatus=EFI_SUCCESS; mBadResetConfig=mDropResetHigh=mDropResetLow=FALSE;
  mGrantCalls=mGrantFailAt=mGrantMask=mAttachFailAt=0;
  mGrantStatus=EFI_DEVICE_ERROR; mMapStatus=mUnmapStatus=EFI_SUCCESS;
  mSavedHwVersion=4U<<28; mSavedHciVersion=0x30000;
  mUicLcc=1;
  mFailUic = TRUE;
  assert (CrUfsPlatformCallback ((VOID *)1, EdkiiUfsHcPostHce, &Driver) == EFI_DEVICE_ERROR);
  assert (CrUfsPlatformCallback ((VOID *)1, EdkiiUfsHcPreLinkStartup, &Driver) == EFI_DEVICE_ERROR);
  mFailUic = FALSE;
  /* A failed link makes MU call PreHce again. The HCI and PHY need a fresh
     calibration, while the SMMU SID remains attached to its first domain. */
  assert (CrUfsPlatformCallback ((VOID *)1, EdkiiUfsHcPreHce, &Driver) == EFI_SUCCESS);
  /* Normal link retry powers down the PHY before clock teardown; only the
     fresh initialization asserts the BCR reset again. */
  assert (mCoreResetCount == 2 && mCalibrations == 2);
  assert (mAttachCount == 1 && mPathCount == 2 && mUfs.ResourcesReady);
  assert (mClockEnables == 2 * ARRAY_SIZE (mClocks) && mResetReleased);
  assert (CrUfsRead (&mUfs.Host, (EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH)-1, 0, 1, &Value) == EFI_INVALID_PARAMETER);
  assert (CrUfsRead (&mUfs.Host, EfiUfsHcWidthUint32, CR_UFS_HCI_LENGTH, 1, &Value) == EFI_UNSUPPORTED);

  assert (CrUfsAllocateBuffer (&mUfs.Host, AllocateAddress, EfiBootServicesData, 1,
                              &Buffer, EDKII_IOMMU_ATTRIBUTE_MEMORY_CACHED) == EFI_SUCCESS);
  assert (mAllocType == AllocateAnyPages && mAllocAttributes == EDKII_IOMMU_ATTRIBUTE_MEMORY_CACHED);
  for (Index = 0; Index < 3; Index++) {
    Length = 8;
    assert (CrUfsMap (&mUfs.Host, Index, Buffer, &Length, &Device, &Mapping) == EFI_SUCCESS);
    assert (mDmaOperation == EdkiiIoMmuOperationBusMasterRead64 + Index);
    assert (mDmaAccess == (Index == 0 ? EDKII_IOMMU_ACCESS_READ :
                          Index == 1 ? EDKII_IOMMU_ACCESS_WRITE : 3));
    assert (CrUfsUnmap (&mUfs.Host, Mapping) == EFI_SUCCESS);
  }
  mPartialMap = TRUE; Length = 8;
  assert (CrUfsMap (&mUfs.Host, 0, Buffer, &Length, &Device, &Mapping) == EFI_OUT_OF_RESOURCES);
  assert (!Mapping && !Device && !Length && !mOutstandingMaps && !mUfs.PendingMapping);
  mFailUnmap = TRUE; Length = 8;
  assert (CrUfsMap (&mUfs.Host, 0, Buffer, &Length, &Device, &Mapping) == EFI_OUT_OF_RESOURCES);
  assert (!Mapping && !Device && !Length && mUfs.PendingMapping && mOutstandingMaps == 1);
  assert (CrUfsFreeBuffer (&mUfs.Host, 1, Buffer) == EFI_DEVICE_ERROR && !mFreeCalls);
  mPartialMap = mFailUnmap = FALSE;
  assert (CrUfsRetryPendingMapping () == EFI_SUCCESS);
  assert (!mOutstandingMaps && !mPools);
  mFailGrant = mFailUnmap = TRUE; Length = 8;
  assert (CrUfsMap (&mUfs.Host, 0, Buffer, &Length, &Device, &Mapping) == EFI_DEVICE_ERROR);
  assert (!Mapping && !Device && mUfs.PendingMapping && mOutstandingMaps == 1);
  assert (CrUfsFreeBuffer (&mUfs.Host, 1, Buffer) == EFI_DEVICE_ERROR && !mFreeCalls);
  mFailUnmap = FALSE;
  assert (CrUfsFreeBuffer (&mUfs.Host, 1, Buffer) == EFI_SUCCESS);
  assert (!mOutstandingMaps && !mPools);
  /* Failed caller-visible Unmap tokens must never be autoretried/freed. */
  {
    VOID *First, *Second;
    UINTN Frees = mFreeCalls;
    mFailGrant = FALSE;
    assert (CrUfsAllocateBuffer (&mUfs.Host, AllocateAnyPages,
      EfiBootServicesData, 1, &Buffer, 0) == EFI_SUCCESS);
    Length = 16;
    assert (CrUfsMap (&mUfs.Host, EdkiiUfsHcOperationBusMasterCommonBuffer,
      Buffer, &Length, &Device, &First) == EFI_SUCCESS);
    Length = 16;
    assert (CrUfsMap (&mUfs.Host, EdkiiUfsHcOperationBusMasterRead,
      Buffer, &Length, &Device, &Second) == EFI_SUCCESS);
    mFailUnmap = TRUE;
    assert (CrUfsUnmap (&mUfs.Host, First) == EFI_DEVICE_ERROR);
    assert (CrUfsUnmap (&mUfs.Host, Second) == EFI_DEVICE_ERROR);
    assert (CrUfsUnmap (&mUfs.Host, First) == EFI_DEVICE_ERROR);
    assert (mPools == 2 && mOutstandingMaps == 2);
    assert (!mUfs.PendingMapping);
    assert (CrUfsRetryPendingMapping () == EFI_SUCCESS);
    assert (CrUfsInitializeResources () == EFI_SUCCESS);
    assert (CrUfsFreeBuffer (&mUfs.Host, 1, Buffer) == EFI_ACCESS_DENIED);
    assert (mFreeCalls == Frees && mPools == 2 && mOutstandingMaps == 2);
    mFailUnmap = FALSE;
    /* Only explicit owner retries revoke these mappings. */
    assert (CrUfsUnmap (&mUfs.Host, First) == EFI_SUCCESS);
    assert (mPools == 1 && mOutstandingMaps == 1);
    assert (CrUfsFreeBuffer (&mUfs.Host, 1, Buffer) == EFI_ACCESS_DENIED);
    assert (CrUfsUnmap (&mUfs.Host, Second) == EFI_SUCCESS);
    assert (CrUfsFreeBuffer (&mUfs.Host, 1, Buffer) == EFI_SUCCESS);
    assert (mFreeCalls == Frees + 1 && !mPools && !mOutstandingMaps);
    assert (!mUfs.PendingMapping);
    assert (CrUfsRetryPendingMapping () == EFI_SUCCESS && !mPools);
  }
  assert (CrUfsReleaseResources () == EFI_SUCCESS); AssertClean ();
  assert (CrUfsInitializeResources () == EFI_SUCCESS && mAttachCount == 1 && mCalibrations == 3);

  Setup (); mMissingClock = TRUE;
  assert (CrUfsGetMmioBar (&mUfs.Host, &Base) == EFI_NOT_READY && Base == 0);
  assert (!mAccessCount && !mPathCount);
  Setup (); mFailPath2 = TRUE;
  assert (CrUfsInitializeResources () == EFI_DEVICE_ERROR); AssertClean ();
  for (Index = 0; Index < ARRAY_SIZE (mRails); Index++) {
    Setup (); mFailVoltage = (INTN)Index;
    assert (CrUfsInitializeResources () == EFI_DEVICE_ERROR); AssertClean ();
  }
  for (Index = 0; Index < ARRAY_SIZE (mClocks); Index++) {
    Setup (); mFailClock = (INTN)Index;
    assert (CrUfsInitializeResources () == EFI_DEVICE_ERROR); AssertClean ();
  }
  Setup (); mFailPhy = TRUE;
  assert (CrUfsInitializeResources () == EFI_TIMEOUT); AssertClean ();
  /* A failed reset deassert must leave the HCI held in reset while rollback
     removes clocks, rails and the power domain. */
  Setup (); mFailResetDeassert = TRUE;
  assert (CrUfsInitializeResources () == EFI_DEVICE_ERROR && !mUfs.CleanupPending);
  assert (mCoreReset);
  Setup (); mFailVoltage = 2; mFailRelease = TRUE;
  assert (CrUfsInitializeResources () == EFI_DEVICE_ERROR && mUfs.CleanupPending);
  assert (mDomain && mPathCount == 2); /* prerequisites retained for retry */
  mFailRelease = FALSE; mFailVoltage = -1;
  assert (CrUfsInitializeResources () == EFI_SUCCESS && !mUfs.CleanupPending);
  Setup ();
  assert (CrUfsInitializeResources () == EFI_SUCCESS);
  assert (CrUfsPlatformCallback ((VOID *)1, EdkiiUfsHcPreHce, &Driver) == EFI_SUCCESS);
  mFailPhy = TRUE;
  assert (CrUfsPlatformCallback ((VOID *)1, EdkiiUfsHcPreHce, &Driver) == EFI_TIMEOUT);
  AssertClean ();
  mFailPhy = FALSE;
  assert (CrUfsPlatformCallback ((VOID *)1, EdkiiUfsHcPreHce, &Driver) == EFI_SUCCESS);
  assert (mCoreResetCount == 3 && mCalibrations == 3 && mAttachCount == 1);
  Setup (); mMmio[CR_UFS_PARAM0/4]=3U<<4;
  assert(CrUfsInitializeResources()==EFI_SUCCESS && mUfs.MaxHsGear==3);
  assert(mMmio[(CR_UFS_PHY_BASE-CR_UFS_HCI_BASE+0x484)/4]==0xf5);
  assert(CrUfsNegotiateHs(&Driver)==EFI_SUCCESS && mHsValues[0x1568]==3 && mHsValues[0x15d4]==3);
  assert(CrUfsReleaseResources()==EFI_SUCCESS); AssertClean();
  Setup (); mMmio[CR_UFS_PARAM0/4]=0;
  assert(CrUfsInitializeResources()==EFI_UNSUPPORTED); AssertClean();
  /* Invalid metadata is rejected before GPIO, votes, clocks or MMIO. */
  for(Index=0;Index<13;Index++) {
    Setup();
    switch(Index) {
      case 0: mMissingTarget=TRUE; break;
      case 1: mTestTarget.Revision++; break;
      case 2: mTestTarget.Size--; break;
      case 3: mTestTarget.PhyBase=mTestTarget.HciBase; break;
      case 4: mTestTarget.ClockCount++; break;
      case 5: mTestTarget.RailCount=CR_TARGET_UFS_MAX_RAILS+1; break;
      case 6: mTestTarget.SidCount=3; break;
      case 7: mTestTarget.Base.Count=257; break;
      case 8: mTestTarget.PhyReady=mTestTarget.PhyStart; break;
      case 9: mTestTarget.PrePhyClockCount=8; break;
      case 10: mTestTarget.AllowedHciMajors=1U<<3; break;
      case 11: mTestTarget.RxInterfaceCount=1; mTestTarget.RxInterfaceOffsets[0]=mTestTarget.PhyReady; break;
      case 12: mTestTarget.SmmuBase=mTestTarget.HciBase; break;
    }
    assert(CrUfsGetMmioBar(&mUfs.Host,&Base)==EFI_UNSUPPORTED);
    assert(!mAccessCount && !mPathCount && !mClockEnables && !mAttachCount && !mDomain);
  }
  for(Index=0;Index<4;Index++) {
    Setup(); mSavedHwVersion=Index==0 ? 0 : Index==1 ? MAX_UINT32 : (Index==2 ? 3U : 5U)<<28;
    assert(CrUfsInitializeResources()==EFI_UNSUPPORTED);
    assert(!mCalibrations && !mAttachCount); AssertClean();
  }
  Setup(); mBootFloorStatus=EFI_WARN_UNKNOWN_GLYPH;
  assert(CrUfsInitializeResources()==EFI_DEVICE_ERROR && !mClockEnables && !mCalibrations); AssertClean();

  Setup(); mBadResetConfig=TRUE;
  assert(CrUfsInitializeResources()==EFI_DEVICE_ERROR && !mClockEnables && !mAccessCount && !mPathCount);
  mBadResetConfig=FALSE;
  assert(CrUfsInitializeResources()==EFI_SUCCESS);
  mDropResetLow=TRUE;
  assert(CrUfsReleaseResources()==EFI_DEVICE_ERROR && mUfs.CleanupPending && mDomain);
  mDropResetLow=FALSE;
  assert(CrUfsReleaseResources()==EFI_SUCCESS); AssertClean();
  Setup(); mResetReadStatus=EFI_WARN_UNKNOWN_GLYPH;
  assert(CrUfsInitializeResources()==EFI_DEVICE_ERROR && !mClockEnables && !mAccessCount);
  Setup(); mDropResetHigh=TRUE;
  assert(CrUfsInitializeResources()==EFI_DEVICE_ERROR); AssertClean();
  /* Two streams share one mapping token; failed second grant revokes BOTH,
     and incomplete revoke keeps storage unavailable until explicit retry. */
  STATIC CONST UINT16 DualSids[]={0xe0,0x4e0};
  Setup(); mTestTarget.Sids=DualSids; mTestTarget.SidCount=2; mAttachFailAt=2;
  assert(CrUfsInitializeResources()==EFI_DEVICE_ERROR && mUfs.SmmuAttached==1); AssertClean();
  mAttachFailAt=0;
  assert(CrUfsInitializeResources()==EFI_SUCCESS && mAttachCount==2 && mUfs.SmmuAttached==2);
  assert(CrUfsAllocateBuffer(&mUfs.Host,AllocateAnyPages,EfiBootServicesData,1,&Buffer,0)==EFI_SUCCESS);
  Length=32;
  assert(CrUfsMap(&mUfs.Host,0,Buffer,&Length,&Device,&Mapping)==EFI_SUCCESS);
  assert(mGrantMask==3 && mGrantCalls==2);
  assert(CrUfsUnmap(&mUfs.Host,Mapping)==EFI_SUCCESS && !mGrantMask);
  for(Index=0;Index<2;Index++) {
    Length=32; mGrantFailAt=mGrantCalls+2; mGrantStatus=Index ? EFI_WARN_UNKNOWN_GLYPH : EFI_DEVICE_ERROR;
    mUnmapStatus=EFI_WARN_UNKNOWN_GLYPH;
    assert(CrUfsMap(&mUfs.Host,0,Buffer,&Length,&Device,&Mapping)==EFI_DEVICE_ERROR);
    assert(!Mapping && !Device && mGrantMask==3 && mUfs.PendingMapping);
    assert(CrUfsFreeBuffer(&mUfs.Host,1,Buffer)==EFI_DEVICE_ERROR && mOutstandingMaps==1);
    mUnmapStatus=EFI_SUCCESS;
    assert(CrUfsRetryPendingMapping()==EFI_SUCCESS && !mGrantMask && !mPools && !mOutstandingMaps);
  }
  mGrantFailAt=0; mMapStatus=EFI_WARN_UNKNOWN_GLYPH; mFailUnmap=TRUE; Length=32;
  assert(CrUfsMap(&mUfs.Host,0,Buffer,&Length,&Device,&Mapping)==EFI_DEVICE_ERROR);
  assert(!Mapping && !Device && mUfs.PendingMapping && mOutstandingMaps==1);
  mMapStatus=EFI_SUCCESS; mFailUnmap=FALSE;
  assert(CrUfsFreeBuffer(&mUfs.Host,1,Buffer)==EFI_SUCCESS && !mPools);
  assert(CrUfsReleaseResources()==EFI_SUCCESS); AssertClean();

  /* Android conditional linecfg uses standard HCI VERSION independently of
     Qualcomm major; bit31 remains unchanged when no skip-ICE quirk exists. */
  for(Index=0;Index<5;Index++) {
    Setup(); mTestTarget.LineCfgPolicy=CrUfsLineCfgAndroidConditional;
    mTestTarget.SkipIceForNonScsi=FALSE;
    mTestTarget.RxInterfaceCount=2; mTestTarget.RxInterfaceOffsets[0]=0x734; mTestTarget.RxInterfaceOffsets[1]=0xb34;
    mSavedHciVersion=Index==0 ? 0x10000 : Index==1 ? 0x10100 : Index==2 ? 0x30000 : Index==3 ? 0x100 : 0x110;
    assert(CrUfsInitializeResources()==EFI_SUCCESS);
    assert(!(mMmio[(0x3000+0x734)/4]&BIT5) && !(mMmio[(0x3000+0xb34)/4]&BIT5));
    mMmio[(0x3000+0xd48)/4]=BIT7|BIT1;
    assert(CrUfsPlatformCallback((VOID *)1,EdkiiUfsHcPreLinkStartup,&Driver)==EFI_SUCCESS);
    assert(mMmio[(0x3000+0xd48)/4]==BIT7 && !(mMmio[CR_UFS_CFG1/4]&BIT31));
    assert(mUicLcc==(Index<2 ? 1U : 0U));
    assert(CrUfsPlatformCallback((VOID *)1,EdkiiUfsHcPostLinkStartup,&Driver)==EFI_SUCCESS);
    assert(mMmio[(0x3000+0xd48)/4]==(BIT7|(Index<2 ? BIT1 : 0)));
    assert(CrUfsReleaseResources()==EFI_SUCCESS); AssertClean();
    assert((mMmio[(0x3000+0x734)/4]&BIT5) && (mMmio[(0x3000+0xb34)/4]&BIT5));
  }
  TestFatalSnapshot ();
  /* Actual new producer branch: stop proof runs with all resources owned,
     E0 survives a failed 4E0 attach, and retry revalidates both before reset. */
  Setup();mTestTarget.AcquireInheritedSmmu=TRUE;mTestTarget.Sids=DualSids;
  mTestTarget.SidCount=2;mAttachFailAt=2;
  assert(CrUfsInitializeResources()==EFI_DEVICE_ERROR&&mUfs.SmmuAttached==1&&mAttachCount==1);
  assert(!mResetReleased&&mAcquiredSids==1);AssertClean();
  mAttachFailAt=0;
  assert(CrUfsInitializeResources()==EFI_SUCCESS&&mAttachCount==2&&mUfs.SmmuAttached==2);
  assert(mAcquireUfsCalls==5&&mAcquiredSids==3&&mResetReleased);
  assert(CrUfsReleaseResources()==EFI_SUCCESS);AssertClean();
  for(Index=0;Index<6;Index++) {
    STATIC CONST UINT16 BusyOffsets[]={0x24,0x34,0x60,0x80,0x58,0x78};
    Setup();mTestTarget.AcquireInheritedSmmu=TRUE;mStoppedBusyOffset=BusyOffsets[Index];
    assert(CrUfsInitializeResources()==EFI_ACCESS_DENIED&&!mAttachCount&&!mResetReleased);
    AssertClean();
  }
  Setup();mTestTarget.AcquireInheritedSmmu=TRUE;mSmmu.AcquireUfsStream=NULL;
  assert(CrUfsInitializeResources()==EFI_UNSUPPORTED&&!mAttachCount&&!mResetReleased);AssertClean();
  Setup();mTestTarget.AcquireInheritedSmmu=TRUE;mSmmu.Revision=EFI_SMMU_CR_FAULT_DIAGNOSTIC_REVISION;
  assert(CrUfsInitializeResources()==EFI_UNSUPPORTED&&!mAttachCount&&!mResetReleased);AssertClean();
  puts ("UFS target/multistream tests passed; UFS host tests passed: cold G4/RateB overlay ordering, HS capability negotiation, all UIC failures, refclk settle, rollback/retry and DMA contract");
  return 0;
}
