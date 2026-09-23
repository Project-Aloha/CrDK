/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#undef NULL
#include "../Library/PciHostBridgeLib/PciHostBridgeLib.c"

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
STATIC BOOLEAN mFailClock;

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
MockClock (EFI_CLOCK_CR_PROTOCOL *This, CONST CHAR8 *Controller,
           CONST CHAR8 *Id, UINT64 Rate, BOOLEAN Enable)
{
  (VOID)This;
  (VOID)Controller;
  (VOID)Id;
  (VOID)Rate;
  return Enable && mFailClock ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC EFI_RPMH_CR_PROTOCOL mMockRpmh = {
  .RpmhSetVregVoltage = MockVoltage,
  .RpmhSetVregMode = MockMode,
  .RpmhEnableVreg = MockEnable
};

STATIC EFI_CLOCK_CR_PROTOCOL mMockClock = { .SetClock = MockClock };

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
  .ControllerCount = 1
};

STATIC VOID
ResetMock (
  VOID
  )
{
  memset (&mPcieResources, 0, sizeof (mPcieResources));
  mPcieResources.Interconnect = &mMockInterconnect;
  mPcieTarget = &mTarget;
  mAcquireCalls = 0;
  mSetCalls = 0;
  mReleaseCalls = 0;
  mFailAcquire = 0;
  mFailSet = 0;
  mFailReleaseMask = 0;
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

  puts ("Crane PCI resources: partial acquisition, shared votes and cleanup retries passed");
  return 0;
}
