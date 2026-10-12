/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#undef NULL
#include "../Library/PciHostBridgeLib/PciHostBridgeLib.c"

ClockDriverContext *CrDalGetClockContext(VOID) { return NULL; }
CR_STATUS CrDalClockResolveResource(CONST CHAR8 *C, CONST CHAR8 *Id,
    CR_DAL_CLOCK_RESOURCE_KIND Kind, CONST CHAR8 **Native, UINT32 *Flags) {
  (VOID)C; (VOID)Id; (VOID)Kind; (VOID)Native; (VOID)Flags;
  return CR_NOT_FOUND;
}

/* The rollback test embeds PciHostBridgeLib but does not link the hardware
   PcieLib implementation.  Supply the small public accessor used by the
   retained-resource bookkeeping helper. */
CR_STATUS
PcieGetPortInfo (
  IN PcieDeviceContext       *Context,
  IN UINT16                   PortIndex,
  OUT CONST PciePortRuntime **Port
  )
{
  if ((Context == NULL) || (Port == NULL) || !Context->Initialized ||
      (PortIndex >= Context->Target->ControllerCount)) {
    return CR_INVALID_PARAMETER;
  }
  *Port = &Context->Ports[PortIndex];
  return CR_SUCCESS;
}

STATIC UINTN mAcquireCalls;
STATIC UINTN mSetCalls;
STATIC UINTN mReleaseCalls;
STATIC UINTN mFailAcquire;
STATIC UINTN mFailSet;
STATIC UINT32 mFailReleaseMask;
STATIC UINTN mRailStep;
STATIC UINTN mFailRailStep;
STATIC UINTN mRailReleases;
STATIC BOOLEAN mFailRailRelease;
STATIC BOOLEAN mRejectRailAcquire;
STATIC BOOLEAN mFailClock;
STATIC UINTN mParentCalls;
STATIC EFI_STATUS mParentStatus;
STATIC UINTN mAttachCalls;
STATIC UINTN mBlockCalls;
STATIC UINTN mFailAttach;
STATIC EFI_STATUS mAttachError;
STATIC EFI_STATUS mBlockError;
STATIC GpioConfigParams mGpioWritten;
STATIC EFI_STATUS mGpioWriteStatus;
STATIC EFI_STATUS mGpioReadStatus;
STATIC UINTN mGpioReads;
STATIC UINTN mGpioMismatch;
STATIC GpioValueType mGpioInput;

BOOLEAN EFIAPI
DebugPrintEnabled (VOID)
{
  return FALSE;
}

BOOLEAN EFIAPI
DebugPrintLevelEnabled (UINTN ErrorLevel)
{
  (VOID)ErrorLevel;
  return TRUE;
}

VOID EFIAPI
DebugPrint (UINTN ErrorLevel, CONST CHAR8 *Format, ...)
{
  (VOID)ErrorLevel;
  (VOID)Format;
}

INTN EFIAPI
AsciiStrCmp (IN CONST CHAR8 *Left, IN CONST CHAR8 *Right)
{
  return strcmp (Left, Right);
}

VOID *EFIAPI
ZeroMem (OUT VOID *Buffer, IN UINTN Length)
{
  return memset (Buffer, 0, Length);
}

VOID *EFIAPI
AllocateZeroPool (IN UINTN Size)
{
  return calloc (1, Size);
}

UINT16 EFIAPI
SetDevicePathNodeLength (IN OUT VOID *Node, IN UINTN Length)
{
  EFI_DEVICE_PATH_PROTOCOL *Path = Node;
  assert (Length <= MAX_UINT16);
  Path->Length[0] = (UINT8)Length;
  Path->Length[1] = (UINT8)(Length >> 8);
  return (UINT16)Length;
}

STATIC EFI_STATUS EFIAPI
MockVoltage (EFI_RPMH_CR_PROTOCOL *This, CONST CHAR8 *Name, UINT32 Voltage)
{
  (VOID)This;
  assert (strcmp (Name, "ldob6") == 0 && Voltage == 1200);
  return ++mRailStep == mFailRailStep ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
MockMode (EFI_RPMH_CR_PROTOCOL *This, CONST CHAR8 *Name, UINT8 Mode)
{
  (VOID)This;
  assert (strcmp (Name, "ldob6") == 0 && Mode == 7);
  return ++mRailStep == mFailRailStep ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
MockEnable (EFI_RPMH_CR_PROTOCOL *This, CONST CHAR8 *Name, BOOLEAN Enable)
{
  (VOID)This;
  assert (strcmp (Name, "ldob6") == 0);
  if (!Enable) {
    mRailReleases++;
    return mFailRailRelease ? EFI_DEVICE_ERROR : EFI_SUCCESS;
  }
  return ++mRailStep == mFailRailStep ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
MockAcquireRail (EFI_RPMH_CR_PROTOCOL *This, CONST CHAR8 *Name,
                 UINT32 Voltage, UINT8 Mode, BOOLEAN *Owned)
{
  EFI_STATUS Status;

  *Owned = FALSE;
  if (mRejectRailAcquire) {
    return EFI_ACCESS_DENIED;
  }
  *Owned = TRUE;
  Status = MockVoltage (This, Name, Voltage);
  if (!EFI_ERROR (Status)) {
    Status = MockMode (This, Name, Mode);
  }
  if (!EFI_ERROR (Status)) {
    Status = MockEnable (This, Name, TRUE);
  }
  return Status;
}

STATIC EFI_STATUS EFIAPI
MockClock (EFI_CLOCK_CR_PROTOCOL *This, CONST CHAR8 *Controller,
           CONST CHAR8 *Id, UINT64 Rate, BOOLEAN Enable)
{
  (VOID)This;
  (VOID)Controller;
  (VOID)Id;
  (VOID)Rate;
  return Enable && mFailClock ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI MockAcquireClock (EFI_CLOCK_CR_PROTOCOL *This,
  CONST CHAR8 *Controller, CONST CHAR8 *Id, UINT64 Rate, BOOLEAN *Owned)
{
  /* This fixture exercises the partial-commit case. */
  *Owned = TRUE;
  return MockClock (This, Controller, Id, Rate, TRUE);
}

STATIC EFI_RPMH_CR_PROTOCOL mMockRpmh = {
  .Revision = EFI_RPMH_CR_PROTOCOL_REVISION,
  .RpmhSetVregVoltage = MockVoltage,
  .RpmhSetVregMode = MockMode,
  .RpmhEnableVreg = MockEnable,
  .AcquireVreg = MockAcquireRail
};

STATIC UINTN mRetainedCalls;
STATIC EFI_STATUS mRetainedResult;
STATIC EFI_STATUS mFloorResult;
STATIC UINTN mFloorCalls;
STATIC EFI_STATUS EFIAPI MockBootFloor (EFI_RPMH_CR_PROTOCOL *This)
{
  assert (This == &mMockRpmh);
  ++mFloorCalls;
  return mFloorResult;
}
STATIC VOID TestPcieBootFloor (VOID)
{
  mPcieResources.Rpmh = &mMockRpmh;
  mMockRpmh.EnsureBootFloor = MockBootFloor;
  mFloorCalls = 0; mFloorResult = EFI_NOT_READY;
  CRANE_PCIE_RESOURCE_CONTEXT Before = mPcieResources;
  assert (EnsurePcieBootFloor () == EFI_NOT_READY);
  assert (!memcmp (&Before, &mPcieResources, sizeof (Before)));
  mFloorResult = EFI_WARN_WRITE_FAILURE;
  assert (EnsurePcieBootFloor () == EFI_NOT_READY);
  mFloorResult = EFI_SUCCESS;
  assert (EnsurePcieBootFloor () == EFI_SUCCESS && mFloorCalls == 3);
  mMockRpmh.EnsureBootFloor = NULL;
  assert (EnsurePcieBootFloor () == EFI_UNSUPPORTED && mFloorCalls == 3);
}
STATIC EFI_STATUS EFIAPI
MockRetained (EFI_RPMH_CR_PROTOCOL *This, CONST CHAR8 *Name)
{
  assert (This == &mMockRpmh);
  assert (!strcmp (Name, "rfclka1") || !strcmp (Name, "rfclka5") || !strcmp (Name, "smpb12"));
  ++mRetainedCalls;
  return mRetainedResult;
}

STATIC VOID TestEnableOnlySupply (VOID)
{
  CONST CHAR8 *Names[] = {"rfclka1", "rfclka5", "smpb12"};
  mPcieResources.Rpmh = &mMockRpmh;
  mMockRpmh.EnsureRetainedVregEnabled = MockRetained;
  mRailStep = mRailReleases = mRetainedCalls = 0;
  for (UINTN I = 0; I < ARRAY_SIZE (Names); ++I) {
    PcieTargetSupply Supply = {"enable-only", "rpmh-enable", Names[I], 0, 0, 0};
    mRetainedResult = CR_DEVICE_ERROR;
    assert (CranePcieSetSupply (NULL, &Supply, TRUE) == CR_DEVICE_ERROR);
    mRetainedResult = CR_SUCCESS;
    assert (CranePcieSetSupply (NULL, &Supply, TRUE) == CR_SUCCESS);
    assert (CranePcieSetSupply (NULL, &Supply, FALSE) == CR_SUCCESS);
    Supply.VoltageMv = 1200;
    assert (CranePcieSetSupply (NULL, &Supply, TRUE) == CR_UNSUPPORTED);
    Supply.VoltageMv = 0; Supply.Mode = 7;
    assert (CranePcieSetSupply (NULL, &Supply, TRUE) == CR_UNSUPPORTED);
  }
  assert (mRetainedCalls == 6 && !mRailStep && !mRailReleases);
  mMockRpmh.EnsureRetainedVregEnabled = NULL;
}

STATIC EFI_STATUS EFIAPI
MockParent (EFI_CLOCK_CR_PROTOCOL *This, CONST CHAR8 *Controller,
            CONST CHAR8 *Id, UINT32 Source)
{
  (VOID)This;
  assert (strcmp (Controller, "gcc") == 0);
  assert (strcmp (Id, "gcc_pcie_1_phy_aux_clk") == 0);
  assert (Source == 2);
  ++mParentCalls;
  return mParentStatus;
}

STATIC EFI_CLOCK_CR_PROTOCOL mMockClock = {
  .Revision = EFI_CLOCK_CR_PROTOCOL_REVISION,
  .SetClock = MockClock,
  .AcquireClock = MockAcquireClock,
  .SetParent = MockParent
};

STATIC EFI_STATUS EFIAPI
MockGpioInit (EFI_GPIO_CR_PROTOCOL *This, GpioConfigParams *Config)
{
  (VOID)This;
  assert (Config->PinNumber >= 97 && Config->PinNumber <= 99);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
MockGpioWrite (EFI_GPIO_CR_PROTOCOL *This, GpioConfigParams *Config)
{
  (VOID)This;
  mGpioWritten = *Config;
  return mGpioWriteStatus;
}

STATIC EFI_STATUS EFIAPI
MockGpioRead (EFI_GPIO_CR_PROTOCOL *This, GpioConfigParams *Config)
{
  (VOID)This;
  assert (Config->PinNumber == mGpioWritten.PinNumber);
  mGpioReads++;
  *Config = mGpioWritten;
  if (!Config->OutputEnable) {
    Config->OutputValue = mGpioInput;
  }
  switch (mGpioMismatch) {
    case 1: Config->FunctionSel ^= 1; break;
    case 2: Config->OutputEnable = !Config->OutputEnable; break;
    case 3: Config->Pull ^= 1; break;
    case 4: Config->DriveStrength ^= 1; break;
    case 5: Config->OutputValue ^= 1; break;
    default: break;
  }
  return mGpioReadStatus;
}

STATIC EFI_GPIO_CR_PROTOCOL mMockGpio = {
  .Revision = EFI_GPIO_CR_PROTOCOL_REVISION,
  .InitGpioConfigParams = MockGpioInit,
  .ConfigGpio = MockGpioWrite,
  .GetGpioConfig = MockGpioRead
};

STATIC CONST PcieTargetIommuMap mIommuMaps[] = {
  { 0x0000, "apps_smmu", 0x1c00, 1 },
  { 0x0100, "apps_smmu", 0x1c01, 1 }
};

STATIC EFI_STATUS EFIAPI
MockAttach (
  IN EFI_SMMU_CR_PROTOCOL *This,
  IN UINT16 Segment,
  IN UINT16 Bdf,
  IN UINT16 Sid
  )
{
  (VOID)This;
  assert (Segment == 0);
  assert (mAttachCalls < ARRAY_SIZE (mIommuMaps));
  assert (Bdf == mIommuMaps[mAttachCalls].Bdf);
  assert (Sid == mIommuMaps[mAttachCalls].StreamId);
  mAttachCalls++;
  return mAttachCalls == mFailAttach ? mAttachError : EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
MockBlockSegment (
  IN EFI_SMMU_CR_PROTOCOL *This,
  IN UINT16 Segment
  )
{
  (VOID)This;
  assert (Segment == 0);
  mBlockCalls++;
  return mBlockError;
}

STATIC EFI_SMMU_CR_PROTOCOL mMockSmmu = {
  .Revision = EFI_SMMU_CR_PROTOCOL_REVISION,
  .Attach = MockAttach,
  .BlockSegment = MockBlockSegment
};

STATIC EFI_STATUS EFIAPI
MockAcquire (
  IN EFI_INTERCONNECT_CR_PROTOCOL *This,
  IN CONST CHAR8 *Provider,
  IN UINT32 Source,
  IN UINT32 Destination,
  OUT INTERCONNECT_PATH_HANDLE *Path
  )
{
  (VOID)This;
  (VOID)Provider;
  (VOID)Source;
  (VOID)Destination;
  mAcquireCalls++;
  if (mAcquireCalls == mFailAcquire) {
    return EFI_DEVICE_ERROR;
  }
  *Path = (INTERCONNECT_PATH_HANDLE)mAcquireCalls;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
MockSet (
  IN EFI_INTERCONNECT_CR_PROTOCOL *This,
  IN INTERCONNECT_PATH_HANDLE Path,
  IN UINT64 Average,
  IN UINT64 Peak
  )
{
  (VOID)This;
  (VOID)Path;
  (VOID)Average;
  (VOID)Peak;
  mSetCalls++;
  return mSetCalls == mFailSet ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
MockRelease (
  IN EFI_INTERCONNECT_CR_PROTOCOL *This,
  IN INTERCONNECT_PATH_HANDLE Path
  )
{
  (VOID)This;
  mReleaseCalls++;
  return (mFailReleaseMask & (1U << Path)) != 0 ?
         EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC EFI_INTERCONNECT_CR_PROTOCOL mMockInterconnect = {
  .Revision = EFI_INTERCONNECT_CR_PROTOCOL_REVISION,
  .AcquirePath = MockAcquire,
  .SetBandwidth = MockSet,
  .ReleasePath = MockRelease
};

STATIC CONST PcieTargetController mController = {
  .Domain = 0,
  .IommuMapOffset = 0,
  .IommuMapCount = ARRAY_SIZE (mIommuMaps),
  .Interconnect = {
    .Provider = "qcom,sm8450-pcie",
    .MemSource = 0x1000,
    .MemDestination = 0x1100,
    .CpuSource = 0x2000,
    .CpuDestination = 0x2100,
    .MemPeak = 100,
    .CpuPeak = 10
  }
};

STATIC CONST PcieTargetContext mTarget = {
  .Controllers = &mController,
  .ControllerCount = 1,
  .IommuMaps = mIommuMaps,
  .IommuMapCount = ARRAY_SIZE (mIommuMaps)
};

STATIC VOID
ResetMock (
  VOID
  )
{
  memset (&mPcieResources, 0, sizeof (mPcieResources));
  mPcieResources.Interconnect = &mMockInterconnect;
  mPcieResources.Smmu = &mMockSmmu;
  mPcieTarget = &mTarget;
  mAcquireCalls = 0;
  mSetCalls = 0;
  mReleaseCalls = 0;
  mFailAcquire = 0;
  mFailSet = 0;
  mFailReleaseMask = 0;
  mAttachCalls = 0;
  mBlockCalls = 0;
  mFailAttach = 0;
  mAttachError = EFI_ACCESS_DENIED;
  mBlockError = EFI_SUCCESS;
  mRejectRailAcquire = FALSE;
}

STATIC UINTN mDiagnosticCalls;
STATIC VOID
MockDiagnose (CONST PcieIoOps *Io, CONST PcieTargetController *Controller)
{
  assert (Io != NULL && Controller->Domain == 1);
  ++mDiagnosticCalls;
}

STATIC VOID
TestSupplyDiagnostics (VOID)
{
  CONST PcieTargetSupply Supplies[] = {
    { "pll", "rpmh", "ldob6", 1200, 0, 7 },
    { "qref", "rpmh", "ldob5", 880, 0, 7 }
  };
  PcieTargetController Controller = mController;
  PcieTargetContext Target = mTarget;
  PcieIoOps Io = { 0 };
  CRANE_PCIE_RESOURCE_CONTEXT Before;
  CRANE_PCIE_RESOURCE_REF *Ref;

  ResetMock ();
  Controller.Domain = 1;
  Controller.SupplyCount = ARRAY_SIZE (Supplies);
  Target.Supplies = Supplies;
  Target.SupplyCount = ARRAY_SIZE (Supplies);
  mPcieTarget = &Target;
  mPcieResources.Rpmh = &mMockRpmh;
  mRailStep = mRailReleases = mFailRailStep = 0;
  assert (CranePcieSetSupply (NULL, &Supplies[0], TRUE) == CR_SUCCESS);
  assert (CranePcieSetSupply (NULL, &Supplies[0], TRUE) == CR_SUCCESS);
  Ref = FindResourceRef (mPcieResources.SupplyRefs, "ldob6", FALSE);
  assert (Ref != NULL && Ref->Count == 2 && Ref->Ready && Ref->Owned &&
          Ref->VoltageMv == 1200 && Ref->Mode == 7);
  Before = mPcieResources;
  mPciePlatformDiagnoseLink = MockDiagnose;
  mDiagnosticCalls = 0;
  CranePcieDiagnoseLink (&Io, &Controller);
  assert (mDiagnosticCalls == 1 && mRailStep == 3 && mRailReleases == 0);
  assert (memcmp (&Before, &mPcieResources, sizeof (Before)) == 0);
  /* A missing cache entry stays missing; malformed target slices cannot
     delegate a potentially unsafe platform snapshot. */
  assert (FindResourceRef (mPcieResources.SupplyRefs, "ldob5", FALSE) == NULL);
  Controller.SupplyOffset = MAX_UINT16;
  CranePcieDiagnoseLink (&Io, &Controller);
  assert (mDiagnosticCalls == 1);
  Controller.SupplyOffset = 0;
  Controller.SupplyCount = PCIE_MAX_RESOURCES + 1;
  CranePcieDiagnoseLink (&Io, &Controller);
  assert (mDiagnosticCalls == 1);
  assert (memcmp (&Before, &mPcieResources, sizeof (Before)) == 0);
  mPciePlatformDiagnoseLink = NULL;
  assert (CranePcieSetSupply (NULL, &Supplies[0], FALSE) == CR_SUCCESS);
  assert (CranePcieSetSupply (NULL, &Supplies[0], FALSE) == CR_SUCCESS);
  ResetMock ();
}

STATIC VOID
TestPcieGpioReadback (
  VOID
  )
{
  CONST PcieTargetGpio Perst = {
    "perst", "tlmm", 97, "GPIO_ACTIVE_LOW", 0, 2, PCIE_GPIO_PULL_DOWN
  };
  CONST PcieTargetGpio ClkReq = {
    "clkreq", "tlmm", 98, "GPIO_ACTIVE_LOW", 1, 2, PCIE_GPIO_PULL_UP
  };
  UINTN Fault;

  ResetMock ();
  mPcieResources.Gpio = &mMockGpio;
  mGpioReads = 0;
  assert (CranePcieSetGpio (NULL, &Perst, PCIE_GPIO_OUTPUT, TRUE) == CR_SUCCESS);
  assert (mGpioWritten.OutputEnable && mGpioWritten.OutputValue == GPIO_VALUE_LOW);
  assert (CranePcieSetGpio (NULL, &Perst, PCIE_GPIO_OUTPUT, FALSE) == CR_SUCCESS);
  assert (mGpioWritten.OutputValue == GPIO_VALUE_HIGH && mGpioReads == 2);
  for (mGpioInput = GPIO_VALUE_LOW; mGpioInput <= GPIO_VALUE_HIGH; mGpioInput++) {
    assert (CranePcieSetGpio (NULL, &ClkReq, PCIE_GPIO_INPUT, FALSE) == CR_SUCCESS);
    assert (!mGpioWritten.OutputEnable && mGpioWritten.FunctionSel == GPIO_FUNC_1);
  }
  /* Board pinctrl has no authority to choose direction or output level. */
  for (UINTN Direction = 0; Direction <= 1; ++Direction) {
    for (UINTN Level = 0; Level <= 1; ++Level) {
      mGpioWritten.PinNumber = Perst.Pin;
      mGpioWritten.OutputEnable = (BOOLEAN)Direction;
      mGpioWritten.OutputValue = (GpioValueType)Level;
      mGpioInput = (GpioValueType)Level;
      assert (CranePcieSetGpio (NULL, &Perst, PCIE_GPIO_PRESERVE, FALSE) == CR_SUCCESS);
      assert (mGpioWritten.OutputEnable == Direction && mGpioWritten.OutputValue == Level);
      assert (mGpioWritten.FunctionSel == Perst.Function && mGpioWritten.Pull == GPIO_PULL_DOWN);
    }
  }
  for (Fault = 1; Fault <= 5; Fault++) {
    mGpioMismatch = Fault;
    assert (CranePcieSetGpio (NULL, &Perst, PCIE_GPIO_OUTPUT, FALSE) == CR_DEVICE_ERROR);
  }
  mGpioMismatch = 0;
  mGpioWriteStatus = EFI_ACCESS_DENIED;
  mGpioReads = 0;
  assert (CranePcieSetGpio (NULL, &Perst, PCIE_GPIO_OUTPUT, FALSE) == EFI_ACCESS_DENIED);
  assert (mGpioReads == 0);
  mGpioWriteStatus = EFI_SUCCESS;
  mGpioReadStatus = EFI_DEVICE_ERROR;
  assert (CranePcieSetGpio (NULL, &Perst, PCIE_GPIO_OUTPUT, FALSE) == EFI_DEVICE_ERROR);
  mGpioReadStatus = EFI_SUCCESS;
  mMockGpio.GetGpioConfig = NULL;
  assert (CranePcieSetGpio (NULL, &Perst, PCIE_GPIO_OUTPUT, FALSE) == CR_UNSUPPORTED);
  mMockGpio.GetGpioConfig = MockGpioRead;
}

STATIC VOID
TestIommuRollback (
  VOID
  )
{
  /* An early attach rejection can leave no domain for BlockSegment.  Keep
     that rejection visible and do not retry an unowned revoke at teardown. */
  ResetMock ();
  mFailAttach = 1;
  mBlockError = EFI_NOT_FOUND;
  assert (CranePcieSetIommu (NULL, &mController, TRUE) == EFI_ACCESS_DENIED);
  assert (mAttachCalls == 1 && mBlockCalls == 1);
  assert (!mPcieResources.IommuAttached[0]);
  assert (CranePcieSetIommu (NULL, &mController, FALSE) == EFI_SUCCESS);
  assert (mBlockCalls == 1);

  /* A failure after an earlier BDF attached still reports the original
     error when revoking the partial segment succeeds. */
  ResetMock ();
  mFailAttach = 2;
  mAttachError = EFI_OUT_OF_RESOURCES;
  assert (CranePcieSetIommu (NULL, &mController, TRUE) == EFI_OUT_OF_RESOURCES);
  assert (mAttachCalls == 2 && mBlockCalls == 1);
  assert (!mPcieResources.IommuAttached[0]);
  assert (CranePcieSetIommu (NULL, &mController, FALSE) == EFI_SUCCESS);
  assert (mBlockCalls == 1);

  /* A real revoke error must not replace Attach's diagnosis or discard the
     retained ownership: teardown retries it until the SMMU confirms release. */
  ResetMock ();
  mFailAttach = 2;
  mAttachError = EFI_OUT_OF_RESOURCES;
  mBlockError = EFI_TIMEOUT;
  assert (CranePcieSetIommu (NULL, &mController, TRUE) == EFI_OUT_OF_RESOURCES);
  assert (mAttachCalls == 2 && mBlockCalls == 1);
  assert (mPcieResources.IommuAttached[0]);
  assert (CranePcieSetIommu (NULL, &mController, FALSE) == EFI_TIMEOUT);
  assert (mBlockCalls == 2 && mPcieResources.IommuAttached[0]);
  mBlockError = EFI_SUCCESS;
  assert (CranePcieSetIommu (NULL, &mController, FALSE) == EFI_SUCCESS);
  assert (mBlockCalls == 3 && !mPcieResources.IommuAttached[0]);
  assert (CranePcieSetIommu (NULL, &mController, FALSE) == EFI_SUCCESS);
  assert (mBlockCalls == 3);

  /* NOT_FOUND during teardown of a successfully attached segment is still
     an unexpected revoke failure, rather than proof that ownership is gone. */
  ResetMock ();
  assert (CranePcieSetIommu (NULL, &mController, TRUE) == EFI_SUCCESS);
  assert (mAttachCalls == 2 && mPcieResources.IommuAttached[0]);
  mBlockError = EFI_NOT_FOUND;
  assert (CranePcieSetIommu (NULL, &mController, FALSE) == EFI_NOT_FOUND);
  assert (mBlockCalls == 1 && mPcieResources.IommuAttached[0]);
  mBlockError = EFI_SUCCESS;
  assert (CranePcieSetIommu (NULL, &mController, FALSE) == EFI_SUCCESS);
  assert (mBlockCalls == 2 && !mPcieResources.IommuAttached[0]);
}

STATIC VOID
RetryCleanup (
  VOID
  )
{
  UINTN Acquires = mAcquireCalls;

  assert (CranePcieSetInterconnect (NULL, &mController, TRUE) == CR_BUSY);
  assert (mAcquireCalls == Acquires);
  mFailReleaseMask = 0;
  assert (CranePcieSetInterconnect (NULL, &mController, FALSE) == CR_SUCCESS);
  assert (mPcieResources.MemPaths[0] == 0);
  assert (mPcieResources.CpuPaths[0] == 0);
  assert (!mPcieResources.InterconnectReady[0]);
}

STATIC VOID
TestRetainedPortResourceMask (
  VOID
  )
{
  PcieTargetController  Controller;
  PcieTargetContext     Target;
  PcieDeviceContext     Context;

  memset (&Controller, 0, sizeof (Controller));
  memset (&Target, 0, sizeof (Target));
  memset (&Context, 0, sizeof (Context));
  Target.Controllers       = &Controller;
  Target.ControllerCount   = 1;
  Context.Target           = &Target;
  Context.Initialized      = TRUE;
  mPcieTarget             = &Target;
  mPcieContext            = &Context;

  /* This models PcieInitializePort() returning its original error after
     ReleasePortResources() retained a PHY clock vote. */
  Context.Ports[0].PhyClocksEnabled = TRUE;
  RecordRetainedPcieResources (1U);
  assert (Context.ResourceMask == 1U);

  Context.Ports[0].PhyClocksEnabled = FALSE;
  Context.Ports[0].EndpointPowerManaged = TRUE;
  Context.ResourceMask = 0;
  RecordRetainedPcieResources (1U);
  assert (Context.ResourceMask == 1U);
  Context.Ports[0].EndpointPowerManaged = FALSE;
  Context.ResourceMask              = 0;
  RecordRetainedPcieResources (1U);
  assert (Context.ResourceMask == 0U);

  mPcieContext = NULL;
  mPcieTarget  = NULL;
}

int
main (
  void
  )
{
  CONST PcieTargetSupply Supply = { "pll", "rpmh", "ldob6", 1200, 0, 7 };
  CONST PcieTargetClock Clock = {
    .Controller = "gcc", .Id = "gcc_test_clk",
    .Provider = PCIE_CLOCK_PROVIDER_GCC
  };
  UINTN Step;
  TestPcieGpioReadback ();
  {
    CONST PcieTargetPhy Phy = {
      .AuxMuxController = "gcc", .AuxMuxId = "gcc_pcie_1_phy_aux_clk"
    };
    assert (CranePcieSetClockParent (NULL, &Phy, 2) == CR_UNSUPPORTED);
    mPcieResources.ClockProtocol = &mMockClock;
    mMockClock.Revision = EFI_CLOCK_CR_PROTOCOL_REVISION - 1;
    assert (CranePcieSetClockParent (NULL, &Phy, 2) == CR_UNSUPPORTED);
    mMockClock.Revision = EFI_CLOCK_CR_PROTOCOL_REVISION;
    mMockClock.SetParent = NULL;
    assert (CranePcieSetClockParent (NULL, &Phy, 2) == CR_UNSUPPORTED);
    mMockClock.SetParent = MockParent;
    assert (CranePcieSetClockParent (NULL, &Phy, 2) == CR_SUCCESS);
    mParentStatus = EFI_ACCESS_DENIED;
    assert (CranePcieSetClockParent (NULL, &Phy, 2) == EFI_ACCESS_DENIED);
    assert (mParentCalls == 2);
    mParentStatus = EFI_SUCCESS;
  }
  {
    CONST PcieTargetRange Ranges[] = {
      { PCIE_RANGE_IO, 0, 0x60200000, 0x100000 },
      { PCIE_RANGE_MEM32, 0x60300000, 0x60300000, 0x3d00000 },
      { PCIE_RANGE_MEM64, 0x100000000, 0x100000000, 0x10000000 }
    };
    PcieTargetController Controller = {
      .Domain = 1, .BusStart = 0, .BusEnd = 1,
      .RangeOffset = 0, .RangeCount = 2
    };
    CONST PcieTargetContext Target = {
      .Controllers = &Controller, .ControllerCount = 1,
      .Ranges = Ranges, .RangeCount = 3
    };
    PCI_ROOT_BRIDGE Bridge;

    mPcieTarget = &Target;
    assert (BuildRootBridge (0, &Bridge) == EFI_SUCCESS);
    assert (!Bridge.ResourceAssigned);
    assert (Bridge.AllocationAttributes == EFI_PCI_HOST_BRIDGE_COMBINE_MEM_PMEM);
    assert (Bridge.Io.Base - Bridge.Io.Translation == 0x60200000);
    assert (Bridge.Mem.Base == 0x60300000 && Bridge.Mem.Limit == 0x63ffffff);
    assert (Bridge.PMem.Base > Bridge.PMem.Limit);
    free (Bridge.DevicePath);
    Controller.RangeCount = 3;
    assert (BuildRootBridge (0, &Bridge) == EFI_SUCCESS);
    assert (Bridge.AllocationAttributes ==
            (EFI_PCI_HOST_BRIDGE_COMBINE_MEM_PMEM | EFI_PCI_HOST_BRIDGE_MEM64_DECODE));
    free (Bridge.DevicePath);
  }

  for (Step = 1; Step <= 3; Step++) {
    ResetMock ();
    mPcieResources.Rpmh = &mMockRpmh;
    mRailStep = mRailReleases = 0;
    mFailRailStep = Step;
    mFailRailRelease = TRUE;
    assert (CranePcieSetSupply (NULL, &Supply, TRUE) == EFI_DEVICE_ERROR);
    assert (CranePcieSetSupply (NULL, &Supply, FALSE) == EFI_DEVICE_ERROR);
    /* Another port must not borrow a voltage-only or uncertain enable vote.
       Its failed acquisition still receives a reference for safe rollback. */
    assert (CranePcieSetSupply (NULL, &Supply, TRUE) == CR_BUSY);
    assert (mRailStep == Step);
    assert (CranePcieSetSupply (NULL, &Supply, FALSE) == CR_SUCCESS);
    assert (mRailReleases == 1);
    mFailRailRelease = FALSE;
    assert (CranePcieSetSupply (NULL, &Supply, FALSE) == CR_SUCCESS);
    assert (mRailReleases == 2);
    assert (CranePcieSetSupply (NULL, &Supply, FALSE) == CR_SUCCESS);
    assert (mRailReleases == 2);
  }

  ResetMock ();
  mPcieResources.Rpmh = &mMockRpmh;
  mRailStep = mRailReleases = mFailRailStep = 0;
  mRejectRailAcquire = TRUE;
  assert (CranePcieSetSupply (NULL, &Supply, TRUE) == EFI_ACCESS_DENIED);
  assert (CranePcieSetSupply (NULL, &Supply, FALSE) == CR_SUCCESS);
  assert (mRailStep == 0 && mRailReleases == 0);

  ResetMock ();
  mPcieResources.Rpmh = &mMockRpmh;
  mRailStep = mRailReleases = mFailRailStep = 0;
  assert (CranePcieSetSupply (NULL, &Supply, TRUE) == CR_SUCCESS);
  assert (CranePcieSetSupply (NULL, &Supply, TRUE) == CR_SUCCESS);
  assert (mRailStep == 3);
  assert (CranePcieSetSupply (NULL, &Supply, FALSE) == CR_SUCCESS);
  assert (mRailReleases == 0);
  assert (CranePcieSetSupply (NULL, &Supply, FALSE) == CR_SUCCESS);
  assert (mRailReleases == 1);

  ResetMock ();
  mPcieResources.ClockProtocol = &mMockClock;
  mFailClock = TRUE;
  assert (CranePcieSetClock (NULL, &Clock, TRUE) == EFI_DEVICE_ERROR);
  assert (CranePcieSetClock (NULL, &Clock, TRUE) == CR_BUSY);
  assert (CranePcieSetClock (NULL, &Clock, FALSE) == CR_SUCCESS);
  assert (CranePcieSetClock (NULL, &Clock, FALSE) == CR_SUCCESS);
  mFailClock = FALSE;
  assert (CranePcieSetClock (NULL, &Clock, TRUE) == CR_SUCCESS);
  assert (CranePcieSetClock (NULL, &Clock, FALSE) == CR_SUCCESS);

  ResetMock ();
  mFailSet = 1;
  mFailReleaseMask = 1U << 1;
  assert (CranePcieSetInterconnect (NULL, &mController, TRUE) == CR_DEVICE_ERROR);
  assert (mPcieResources.MemPaths[0] == 1);
  assert (mPcieResources.CpuPaths[0] == 0);
  RetryCleanup ();

  ResetMock ();
  mFailAcquire = 2;
  mFailReleaseMask = 1U << 1;
  assert (CranePcieSetInterconnect (NULL, &mController, TRUE) == CR_DEVICE_ERROR);
  assert (mPcieResources.MemPaths[0] == 1);
  assert (mPcieResources.CpuPaths[0] == 0);
  RetryCleanup ();

  /* Both rollback operations can fail after the CPU bandwidth vote fails.
   * A complete pair of retained handles is not an initialized path pair. */
  ResetMock ();
  mFailSet = 2;
  mFailReleaseMask = (1U << 1) | (1U << 2);
  assert (CranePcieSetInterconnect (NULL, &mController, TRUE) == CR_DEVICE_ERROR);
  assert (mPcieResources.MemPaths[0] == 1);
  assert (mPcieResources.CpuPaths[0] == 2);
  assert (!mPcieResources.InterconnectReady[0]);
  RetryCleanup ();

  ResetMock ();
  assert (CranePcieSetInterconnect (NULL, &mController, TRUE) == CR_SUCCESS);
  assert (mPcieResources.InterconnectReady[0]);
  assert (CranePcieSetInterconnect (NULL, &mController, TRUE) == CR_SUCCESS);
  assert (mAcquireCalls == 2 && mSetCalls == 2);
  mFailReleaseMask = 1U << 2;
  assert (CranePcieSetInterconnect (NULL, &mController, FALSE) == CR_DEVICE_ERROR);
  assert (mPcieResources.MemPaths[0] == 0);
  assert (mPcieResources.CpuPaths[0] == 2);
  RetryCleanup ();

  TestIommuRollback ();
  TestSupplyDiagnostics ();
  TestRetainedPortResourceMask ();
  TestEnableOnlySupply ();
  TestPcieBootFloor ();

  puts ("Crane PCI resources: partial acquisition, shared votes, SMMU errors and cleanup retries passed");
  return 0;
}
