/** @file
  Host tests for the Crane-owned clock backend.

  SPDX-License-Identifier: MIT
**/
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#undef NULL
#include <Uefi.h>

#include "../Driver/ClockCrDxe/ClockCrDxe.c"
#include "../Library/Sm8450ClockLib/Sm8450Clock.c"
#include "../Target/Sm8450/CrClockTarget.c"
#include "../Library/ClockLib/clock_config.c"

EFI_GUID gEfiClockCrProtocolGuid = EFI_CLOCK_CR_PROTOCOL_GUID;
EFI_GUID gEfiClockExternalCrProtocolGuid = EFI_CLOCK_EXTERNAL_CR_PROTOCOL_GUID;
EFI_GUID gEfiRpmhCrProtocolGuid = EFI_RPMH_CR_PROTOCOL_GUID;
EFI_BOOT_SERVICES *gBS;
STATIC EFI_TPL mTestTpl = TPL_APPLICATION;
EFI_TPL EFIAPI EfiGetCurrentTpl(VOID) { return mTestTpl; }
UINT64 EFIAPI GetPerformanceCounter(VOID) { static UINT64 Counter; return ++Counter; }
/* This adapter-only fixture supplies no qualified native display target.
 * Actual selected target/core behavior lives in the composition suites. */
CONST CR_MDSS_TARGET *CrDalGetMdssContext(VOID) { return NULL; }
UINTN EFIAPI MicroSecondDelay(UINTN Microseconds) {
  /* Inherited retirement is outside this vote-mock fixture's contract. */
  (VOID)Microseconds;assert(0);return 0;
}
INTN EFIAPI AsciiStrnCmp(CONST CHAR8 *A, CONST CHAR8 *B, UINTN N)
{ return strncmp(A, B, N); }

/* ClockCrDxe now owns the backend through ClockLib.  Keep this harness
 * self-contained: these lightweight hooks model the CrDAL/ClockLib entry
 * points used only while the test switches the adapter to its vote mock. */
STATIC ClockDriverContext mTestClockContext;
STATIC ClockNode *mLookupNode;
STATIC UINTN mMuxAddress, mMuxWrites;
STATIC UINT32 mMuxValue;
STATIC BOOLEAN mDropMuxWrite;
STATIC UINTN mPixelConfig;
STATIC UINT32 mPixelValues[4];
STATIC UINTN mPixelWrites;
STATIC BOOLEAN mCoreRetainFailure;
STATIC UINTN mCoreDisables;

ClockDriverContext *
CrDalGetClockContext (
  VOID
  )
{
  return &mTestClockContext;
}

CR_STATUS
ClockLibInit (
  IN OUT ClockDriverContext **ClockContext
  )
{
  return (ClockContext != NULL) ? CR_SUCCESS : CR_INVALID_PARAMETER;
}

CR_STATUS
CrDalClockInit (
  IN ClockDriverContext *ClockContext
  )
{
  return (ClockContext != NULL) ? CR_SUCCESS : CR_INVALID_PARAMETER;
}

CR_STATUS
GetClockNode (
  IN ClockDriverContext *ClockContext,
  IN CONST CHAR8        *ClockName,
  IN OUT ClockControllerType *ControllerType,
  IN OUT UINT32         *ClockId,
  OUT ClockNode         **TargetClockNode
  )
{
  (VOID)ClockContext;
  (VOID)ClockName;
  (VOID)ControllerType;
  (VOID)ClockId;
  if (TargetClockNode != NULL) {
    *TargetClockNode = mLookupNode;
  }
  return mLookupNode == NULL ? CR_NOT_FOUND : CR_SUCCESS;
}

CR_STATUS
GetGdscNode (
  IN ClockDriverContext *ClockContext,
  IN CONST CHAR8        *GdscName,
  OUT ClockNode         **TargetGdscNode
  )
{
  (VOID)ClockContext;
  (VOID)GdscName;
  if (TargetGdscNode != NULL) {
    *TargetGdscNode = NULL;
  }
  return CR_NOT_FOUND;
}

CR_STATUS
ClockEnable (
  IN ClockDriverContext *ClockContext,
  IN ClockNode          *TargetClockNode,
  IN UINT64             RateHz,
  IN BOOLEAN            Enable
  )
{
  (VOID)ClockContext;
  (VOID)RateHz;
  if (mCoreRetainFailure && Enable) {
    TargetClockNode->ReferenceCount = 1;
    TargetClockNode->ReleasePending = TRUE;
    return CR_TIMEOUT;
  }
  if (!Enable) {
    ++mCoreDisables;
    TargetClockNode->ReferenceCount = 0;
    TargetClockNode->ReleasePending = FALSE;
  }
  return CR_SUCCESS;
}

CR_STATUS ClockAcquire (ClockDriverContext *Context, ClockNode *Node,
  UINT64 Rate, BOOLEAN *Owned)
{
  UINT32 Before = Node->ReferenceCount;
  CR_STATUS Status = ClockEnable (Context, Node, Rate, TRUE);
  *Owned = !CR_ERROR (Status) || Node->ReferenceCount > Before;
  return Status;
}
CR_STATUS ClockAcquireExternal (ClockDriverContext *Context, ClockNode *Node,
  UINT64 Rate, UINT32 Source, BOOLEAN *Owned)
{
  *Owned = FALSE;
  CR_STATUS Status = ClockSelectExternalSource (Context, Node, Source, 0, 0, 0, 0);
  if (CR_ERROR (Status)) return Status;
  return ClockAcquire (Context, Node, Rate, Owned);
}
CR_STATUS ClockGdscSet (ClockDriverContext *Context, ClockNode *Node, BOOLEAN Enable)
{
  return ClockEnable (Context, Node, 0, Enable);
}
CR_STATUS ClockGdscAcquire (ClockDriverContext *Context, ClockNode *Node, BOOLEAN *Owned)
{
  return ClockAcquire (Context, Node, 0, Owned);
}
/* This adapter-only fixture models its core contract; the native ownership
   and SM8250 native composition fixtures execute real ClockCheckAcquired. */
CR_STATUS ClockCheckAcquired (ClockDriverContext *Context, ClockNode *Node, UINT64 Rate)
{
  (VOID)Rate;
  return Context != NULL && Node != NULL ? CR_SUCCESS : CR_INVALID_PARAMETER;
}

CR_STATUS
ClockRcg2SetRate (
  IN ClockDriverContext *ClockContext,
  IN ClockNode          *TargetClockNode,
  IN UINT64             RateHz,
  IN UINT8              Policy
  )
{
  (VOID)ClockContext;
  (VOID)TargetClockNode;
  (VOID)RateHz;
  (VOID)Policy;
  return CR_SUCCESS;
}

CR_STATUS
ClockRcg2UpdateConfig (
  IN ClockDriverContext *ClockContext,
  IN ClockNode          *TargetClockNode
  )
{
  (VOID)ClockContext;
  (VOID)TargetClockNode;
  return CR_SUCCESS;
}

CR_STATUS
ClockGdscEnable (
  IN ClockDriverContext *ClockContext,
  IN ClockNode          *Gdsc
  )
{
  (VOID)ClockContext;
  (VOID)Gdsc;
  return CR_SUCCESS;
}

CR_STATUS
ClockGdscDisable (
  IN ClockDriverContext *ClockContext,
  IN ClockNode          *Gdsc
  )
{
  (VOID)ClockContext;
  (VOID)Gdsc;
  return CR_SUCCESS;
}

CR_STATUS
CrDalClockReset (
  IN CONST CHAR8 *Controller,
  IN CONST CHAR8 *Id,
  IN BOOLEAN      Assert
  )
{
  (VOID)Controller;
  (VOID)Id;
  (VOID)Assert;
  return CR_SUCCESS;
}

UINT32 EFIAPI MmioRead32 (IN UINTN Address) {
  if (mPixelConfig && Address >= mPixelConfig && Address < mPixelConfig + 16)
    return mPixelValues[(Address - mPixelConfig) / 4];
  return Address == mMuxAddress ? mMuxValue : 0;
}
UINT32 EFIAPI MmioWrite32 (IN UINTN Address, IN UINT32 Value) {
  if (mPixelConfig && Address >= mPixelConfig && Address < mPixelConfig + 16) {
    mPixelValues[(Address - mPixelConfig) / 4] = Value;
    ++mPixelWrites;
  }
  if (Address == mMuxAddress) {
    ++mMuxWrites;
    if (!mDropMuxWrite) mMuxValue = Value;
  }
  return Value;
}
VOID EFIAPI MemoryFence (VOID) { }

STATIC EFI_STATUS mInstallStatus;
STATIC EFI_STATUS mGetIdStatus;
STATIC EFI_STATUS mSetRateStatus;
STATIC EFI_STATUS mSelectExternalStatus;
STATIC EFI_STATUS mSetDividerStatus;
STATIC EFI_STATUS mEnableStatus;
STATIC EFI_STATUS mDisableStatus;
STATIC EFI_STATUS mIsEnabledStatus;
STATIC EFI_STATUS mPowerEnableStatus;
STATIC EFI_STATUS mPowerDisableStatus;
STATIC EFI_STATUS mResetStatus;
STATIC EFI_STATUS mResolveStatus;
STATIC UINT32     mResolveFlags;
STATIC BOOLEAN    mResolveNull;
STATIC UINTN      mLocateCalls;
STATIC UINTN      mInstallCalls;
STATIC UINTN      mGetIdCalls;
STATIC UINTN      mSetRateCalls;
STATIC UINTN      mSelectExternalCalls;
STATIC UINTN      mSetDividerCalls;
STATIC UINTN      mEnableCalls;
STATIC UINTN      mDisableCalls;
STATIC UINTN      mIsEnabledCalls;
STATIC UINTN      mGetPowerIdCalls;
STATIC UINTN      mPowerEnableCalls;
STATIC UINTN      mPowerDisableCalls;
STATIC UINTN      mResetCalls;
STATIC UINTN      mLastId;
STATIC UINTN      mLastExternalId;
STATIC UINTN      mLastDividerId;
STATIC UINT32     mLastRate;
STATIC UINT32     mExternalRate;
STATIC UINT32     mExternalSource;
STATIC UINT32     mExternalDivider;
STATIC UINT32     mExternalM;
STATIC UINT32     mExternalN;
STATIC UINT32     mExternalTwiceD;
STATIC UINT32     mLastDivider;
STATIC CRANE_CLOCK_FREQUENCY_TYPE mLastMatch;
STATIC CRANE_CLOCK_RESET_TYPE     mLastReset;
STATIC CHAR8      mLastName[96];
STATIC CRANE_CLOCK_BACKEND mMockNative;
STATIC BOOLEAN    mClockEnabled;
STATIC BOOLEAN    mEnableFailureLeavesEnabled;

INTN
EFIAPI
AsciiStrCmp (
  IN CONST CHAR8 *FirstString,
  IN CONST CHAR8 *SecondString
  )
{
  return strcmp (FirstString, SecondString);
}

VOID
EFIAPI
DebugPrint (
  IN UINTN        ErrorLevel,
  IN CONST CHAR8 *Format,
  ...
  )
{
  (VOID)ErrorLevel;
  (VOID)Format;
}

BOOLEAN
EFIAPI
DebugPrintEnabled (
  VOID
  )
{
  return TRUE;
}

BOOLEAN
EFIAPI
DebugPrintLevelEnabled (
  IN CONST UINTN ErrorLevel
  )
{
  (VOID)ErrorLevel;
  return TRUE;
}

VOID
EFIAPI
EfiAcquireLock (
  IN EFI_LOCK *Lock
  )
{
  assert (Lock != NULL && Lock->Lock == EfiLockReleased);
  Lock->Lock = EfiLockAcquired;
}

VOID
EFIAPI
EfiReleaseLock (
  IN EFI_LOCK *Lock
  )
{
  assert (Lock != NULL && Lock->Lock == EfiLockAcquired);
  Lock->Lock = EfiLockReleased;
}

STATIC
UINTN
IdForName (
  IN CONST CHAR8 *Name
  )
{
  if (strcmp (Name, "gcc_pcie_0_aux_clk") == 0) {
    return 10;
  }
  if (strcmp (Name, "gcc_pcie_0_pipe_clk") == 0) {
    return 20;
  }
  if (strcmp (Name, "gcc_pcie_1_phy_aux_clk") == 0) {
    return 25;
  }
  if (strcmp (Name, "gcc_pcie_0_phy_bcr") == 0) {
    return 30;
  }
  if (strcmp (Name, "disp_cc_mdss_byte0_clk") == 0) {
    return 50;
  }
  if (strcmp (Name, "disp_cc_mdss_pclk0_clk") == 0) {
    return 51;
  }
  if (strcmp (Name, "disp_cc_mdss_byte0_intf_clk") == 0) {
    return 52;
  }
  return 90;
}

STATIC
EFI_STATUS
EFIAPI
MockGetClockId (
  IN  CRANE_CLOCK_BACKEND *This,
  IN  CONST CHAR8        *Name,
  OUT UINTN              *ClockId
  )
{
  assert (This == &mMockNative && Name != NULL && ClockId != NULL);
  mGetIdCalls++;
  strncpy (mLastName, Name, sizeof (mLastName) - 1);
  mLastName[sizeof (mLastName) - 1] = '\0';
  *ClockId = IdForName (Name);
  return mGetIdStatus;
}

STATIC
EFI_STATUS
EFIAPI
MockEnableClock (
  IN CRANE_CLOCK_BACKEND *This,
  IN UINTN               ClockId
  )
{
  assert (This == &mMockNative);
  if (ClockId == 20) {
    assert (mExternalSource == 0);
  }
  if (ClockId == 25) {
    /* AUX enable preserves either legal bootloader/Linux mux parent. */
    assert ((mExternalSource == 0) || (mExternalSource == 2));
  }
  mEnableCalls++;
  mLastId = ClockId;
  if (!EFI_ERROR (mEnableStatus) || mEnableFailureLeavesEnabled) {
    mClockEnabled = TRUE;
  }
  return mEnableStatus;
}

STATIC
EFI_STATUS
EFIAPI
MockDisableClock (
  IN CRANE_CLOCK_BACKEND *This,
  IN UINTN               ClockId
  )
{
  assert (This == &mMockNative);
  mDisableCalls++;
  mLastId = ClockId;
  if (!EFI_ERROR (mDisableStatus)) {
    mClockEnabled = FALSE;
  }
  return mDisableStatus;
}

STATIC
EFI_STATUS
EFIAPI
MockIsClockEnabled (
  IN  CRANE_CLOCK_BACKEND *This,
  IN  UINTN               ClockId,
  OUT BOOLEAN            *IsEnabled
  )
{
  assert (This == &mMockNative && IsEnabled != NULL);
  mIsEnabledCalls++;
  mLastId = ClockId;
  *IsEnabled = mClockEnabled;
  return mIsEnabledStatus;
}

STATIC
EFI_STATUS
EFIAPI
MockSetRate (
  IN  CRANE_CLOCK_BACKEND       *This,
  IN  UINTN                     ClockId,
  IN  UINT32                    Rate,
  IN  CRANE_CLOCK_FREQUENCY_TYPE  Match,
  OUT UINT32                   *ResultRate OPTIONAL
  )
{
  assert (This == &mMockNative);
  mSetRateCalls++;
  mLastId = ClockId;
  mLastRate = Rate;
  mLastMatch = Match;
  if (ResultRate != NULL) {
    *ResultRate = Rate;
  }
  return mSetRateStatus;
}

STATIC
EFI_STATUS
EFIAPI
MockSelectExternal (
  IN CRANE_CLOCK_BACKEND *This,
  IN UINTN               ClockId,
  IN UINT32              Rate,
  IN UINT32              Source,
  IN UINT32              Divider,
  IN UINT32              M,
  IN UINT32              N,
  IN UINT32              TwiceD
  )
{
  assert (This == &mMockNative);
  assert (mVoteLock.Lock == EfiLockAcquired);
  mSelectExternalCalls++;
  mLastExternalId = ClockId;
  mExternalRate = Rate;
  mExternalSource = Source;
  mExternalDivider = Divider;
  mExternalM = M;
  mExternalN = N;
  mExternalTwiceD = TwiceD;
  return mSelectExternalStatus;
}

STATIC
EFI_STATUS
EFIAPI
MockSetDivider (
  IN CRANE_CLOCK_BACKEND *This,
  IN UINTN               ClockId,
  IN UINT32              Divider
  )
{
  assert (This == &mMockNative);
  assert (mVoteLock.Lock == EfiLockAcquired);
  mSetDividerCalls++;
  mLastDividerId = ClockId;
  mLastDivider = Divider;
  return mSetDividerStatus;
}

STATIC
EFI_STATUS
EFIAPI
MockGetPowerId (
  IN  CRANE_CLOCK_BACKEND *This,
  IN  CONST CHAR8        *Name,
  OUT UINTN              *PowerId
  )
{
  assert (This == &mMockNative && PowerId != NULL);
  assert (strcmp (Name, "pcie_0_gdsc") == 0);
  mGetPowerIdCalls++;
  *PowerId = 40;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MockEnablePower (
  IN CRANE_CLOCK_BACKEND *This,
  IN UINTN               PowerId
  )
{
  assert (This == &mMockNative && PowerId == 40);
  mPowerEnableCalls++;
  return mPowerEnableStatus;
}

STATIC
EFI_STATUS
EFIAPI
MockDisablePower (
  IN CRANE_CLOCK_BACKEND *This,
  IN UINTN               PowerId
  )
{
  assert (This == &mMockNative && PowerId == 40);
  mPowerDisableCalls++;
  return mPowerDisableStatus;
}

STATIC
EFI_STATUS
EFIAPI
MockResetClock (
  IN CRANE_CLOCK_BACKEND   *This,
  IN UINTN                 ClockId,
  IN CRANE_CLOCK_RESET_TYPE  ResetType
  )
{
  assert (This == &mMockNative);
  mResetCalls++;
  mLastId = ClockId;
  mLastReset = ResetType;
  return mResetStatus;
}

CR_STATUS
CrDalClockResolveResource (
  IN  CONST CHAR8                 *Controller,
  IN  CONST CHAR8                 *Id,
  IN  CR_DAL_CLOCK_RESOURCE_KIND   Kind,
  OUT CONST CHAR8                **NativeId,
  OUT UINT32                      *Flags
  )
{
  EFI_STATUS Status;

  assert (Controller != NULL && Id != NULL && NativeId != NULL && Flags != NULL);
  if (EFI_ERROR (mResolveStatus)) {
    return mResolveStatus;
  }
  Status = CrTargetClockResolveResource (
             Controller, Id, Kind, NativeId, Flags);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  *Flags |= mResolveFlags;
  if (mResolveNull) {
    *NativeId = NULL;
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MockInstallMultiple (
  IN OUT EFI_HANDLE *Handle,
  ...
  )
{
  assert (Handle != NULL);
  mInstallCalls++;
  return mInstallStatus;
}

STATIC EFI_BOOT_SERVICES mBootServices = {
  .InstallMultipleProtocolInterfaces = MockInstallMultiple,
};

STATIC
VOID
ResetHarness (
  VOID
  )
{
  memset (mClockVotes, 0, sizeof (mClockVotes));
  memset (mPowerDomainVotes, 0, sizeof (mPowerDomainVotes));
  memset (mLastName, 0, sizeof (mLastName));
  mInstallStatus = EFI_SUCCESS;
  mGetIdStatus = EFI_SUCCESS;
  mSetRateStatus = EFI_SUCCESS;
  mSelectExternalStatus = EFI_SUCCESS;
  mSetDividerStatus = EFI_SUCCESS;
  mEnableStatus = EFI_SUCCESS;
  mDisableStatus = EFI_SUCCESS;
  mIsEnabledStatus = EFI_SUCCESS;
  mPowerEnableStatus = EFI_SUCCESS;
  mPowerDisableStatus = EFI_SUCCESS;
  mResetStatus = EFI_SUCCESS;
  mResolveStatus = EFI_SUCCESS;
  mResolveFlags = 0;
  mResolveNull = FALSE;
  mLookupNode = NULL;
  mCoreRetainFailure = FALSE; mCoreDisables = 0;
  mMuxAddress = mMuxWrites = mMuxValue = 0;
  mDropMuxWrite = FALSE;
  mLocateCalls = 0;
  mInstallCalls = 0;
  mGetIdCalls = 0;
  mSetRateCalls = 0;
  mSelectExternalCalls = 0;
  mSetDividerCalls = 0;
  mEnableCalls = 0;
  mDisableCalls = 0;
  mIsEnabledCalls = 0;
  mGetPowerIdCalls = 0;
  mPowerEnableCalls = 0;
  mPowerDisableCalls = 0;
  mResetCalls = 0;
  mLastId = 0;
  mLastExternalId = 0;
  mLastDividerId = 0;
  mLastRate = 0;
  mExternalRate = 0;
  mExternalSource = 2;
  mExternalDivider = 0;
  mExternalM = 0;
  mExternalN = 0;
  mExternalTwiceD = 0;
  mLastDivider = 0;
  mLastMatch = CraneClockFrequencyHzAtMost;
  mLastReset = CraneClockResetAssert;
  mClockEnabled = FALSE;
  mEnableFailureLeavesEnabled = FALSE;
  mVoteLock.Tpl = TPL_NOTIFY;
  mVoteLock.OwnerTpl = TPL_APPLICATION;
  mVoteLock.Lock = EfiLockReleased;
  memset (&mMockNative, 0, sizeof (mMockNative));
  mMockNative.Version = CRANE_CLOCK_BACKEND_REVISION;
  mMockNative.GetClockID = MockGetClockId;
  mMockNative.EnableClock = MockEnableClock;
  mMockNative.DisableClock = MockDisableClock;
  mMockNative.IsClockEnabled = MockIsClockEnabled;
  mMockNative.SetClockFreqHz = MockSetRate;
  mMockNative.SelectExternalSource = MockSelectExternal;
  mMockNative.SetClockDivider = MockSetDivider;
  mMockNative.GetClockPowerDomainID = MockGetPowerId;
  mMockNative.EnableClockPowerDomain = MockEnablePower;
  mMockNative.DisableClockPowerDomain = MockDisablePower;
  mMockNative.ResetClock = MockResetClock;
  mNativeClock = &mMockNative;
  gBS = &mBootServices;
}

STATIC
VOID
TestAbiAndEntryPoint (
  VOID
  )
{
  EFI_HANDLE Image = NULL;

  assert (offsetof (CRANE_CLOCK_BACKEND, GetClockID) == sizeof (UINT64));
  assert (offsetof (CRANE_CLOCK_BACKEND, ResetClock) ==
          sizeof (UINT64) + (13U * sizeof (VOID *)));
  assert (sizeof (CRANE_CLOCK_BACKEND) ==
          sizeof (UINT64) + (14U * sizeof (VOID *)));

  ResetHarness ();
  mInstallStatus = EFI_DEVICE_ERROR;
  assert (ClockEntryPoint (Image, NULL) == EFI_DEVICE_ERROR);
  assert (mInstallCalls == 1 && mLocateCalls == 0 && mNativeClock == NULL);

  ResetHarness ();
  assert (ClockEntryPoint (Image, NULL) == EFI_SUCCESS);
  assert (mLocateCalls == 0 && mInstallCalls == 1);
  /* EntryPoint selected the Crane backend.  The remaining tests explicitly
   * replace it with the mock to exercise vote rollback without MMIO. */
  assert (mNativeClock == &mCraneNativeClock);
  mNativeClock = &mMockNative;
}

STATIC
VOID
TestClockVotesAndRates (
  VOID
  )
{
  ResetHarness ();
  assert (ProtocolSetClock (NULL, "gcc", "clock", 0, TRUE) ==
          EFI_INVALID_PARAMETER);
  mNativeClock = NULL;
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_0_aux_clk", 19200000, TRUE
            ) == EFI_NOT_READY);
  mNativeClock = &mMockNative;

  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_0_aux_clk", 19200000, TRUE
            ) == EFI_SUCCESS);
  assert (mGetIdCalls == 1 && mSetRateCalls == 1 && mEnableCalls == 1);
  assert (mLastId == 10 && mLastRate == 19200000);
  assert (mLastMatch == CraneClockFrequencyHzExact);
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_0_aux_clk", 19200000, TRUE
            ) == EFI_SUCCESS);
  assert (mGetIdCalls == 2 && mSetRateCalls == 1 && mEnableCalls == 1);
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_0_aux_clk", 100000000, TRUE
            ) == EFI_ACCESS_DENIED);
  assert (mSetRateCalls == 1 && mEnableCalls == 1);
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_0_aux_clk", 0, FALSE
            ) == EFI_SUCCESS);
  assert (mDisableCalls == 0);
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_0_aux_clk", 0, FALSE
            ) == EFI_SUCCESS);
  assert (mDisableCalls == 1);
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_0_aux_clk", 0, FALSE
            ) == EFI_SUCCESS);
  assert (mDisableCalls == 1);

  ResetHarness ();
  mSetRateStatus = EFI_DEVICE_ERROR;
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_0_aux_clk", 19200000, TRUE
            ) == EFI_DEVICE_ERROR);
  assert (mEnableCalls == 0);
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_0_aux_clk", 0, FALSE
            ) == EFI_SUCCESS);
  assert (mDisableCalls == 0);
  mSetRateStatus = EFI_SUCCESS;
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_0_aux_clk", 19200000, TRUE
            ) == EFI_SUCCESS);

  mDisableStatus = EFI_DEVICE_ERROR;
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_0_aux_clk", 0, FALSE
            ) == EFI_DEVICE_ERROR);
  mDisableStatus = EFI_SUCCESS;
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_0_aux_clk", 0, FALSE
            ) == EFI_SUCCESS);
  assert (mDisableCalls == 2);

  ResetHarness ();
  mEnableStatus = EFI_DEVICE_ERROR;
  mEnableFailureLeavesEnabled = TRUE;
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_0_aux_clk", 19200000, TRUE
            ) == EFI_DEVICE_ERROR);
  assert (mIsEnabledCalls == 2 && mClockEnabled);
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_0_aux_clk", 0, FALSE
            ) == EFI_DEVICE_ERROR);
  assert (mDisableCalls == 0);
  mClockEnabled = FALSE;
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_0_aux_clk", 0, FALSE
            ) == EFI_SUCCESS);

  ResetHarness ();
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_0_aux_clk",
            (UINT64)MAX_UINT32 + 1U, TRUE
            ) == EFI_INVALID_PARAMETER);
  assert (mSetRateCalls == 0 && mEnableCalls == 0);
}

STATIC VOID TestNativeRetainedCleanup (VOID)
{
  ClockNode Node = {.Type = CLOCK_NODE_TYPE_BRANCH_2};
  ResetHarness ();
  mNativeClock = &mCraneNativeClock;
  mClockContext = &mTestClockContext;
  mLookupNode = &Node; mCoreRetainFailure = TRUE;
  assert (ProtocolSetClock (&mClockCrProtocol, "gcc", "gcc_usb3_prim_phy_aux_clk",
                            19200000, TRUE) == EFI_TIMEOUT);
  assert (Node.ReferenceCount == 1 && Node.ReleasePending);
  BOOLEAN Owned = TRUE;
  assert (ProtocolAcquireClock (&mClockCrProtocol, "gcc", "gcc_usb3_prim_phy_aux_clk",
                                 19200000, &Owned) == EFI_TIMEOUT && !Owned);
  mCoreRetainFailure = FALSE;
  /* A known native cleanup reference must be actively drained. Merely
     polling IsEnabled keeps the failed owner alive forever. */
  assert (ProtocolSetClock (&mClockCrProtocol, "gcc", "gcc_usb3_prim_phy_aux_clk",
                            0, FALSE) == EFI_SUCCESS);
  assert (mCoreDisables == 1 && Node.ReferenceCount == 0 && !Node.ReleasePending);
  ResetHarness ();
  assert (ProtocolAcquireClock (&mClockCrProtocol, "gcc", "gcc_usb3_prim_phy_aux_clk",
                                 19200000, &Owned) == EFI_SUCCESS && Owned);
  assert (ProtocolAcquireClock (&mClockCrProtocol, "gcc", "gcc_usb3_prim_phy_aux_clk",
                                 100000000, &Owned) == EFI_ACCESS_DENIED && !Owned);
  assert (mDisableCalls == 0);
  mDisableStatus = EFI_TIMEOUT;
  assert (ProtocolSetClock (&mClockCrProtocol, "gcc", "gcc_usb3_prim_phy_aux_clk",
                            0, FALSE) == EFI_TIMEOUT);
  assert (ProtocolAcquireClock (&mClockCrProtocol, "gcc", "gcc_usb3_prim_phy_aux_clk",
                                 19200000, &Owned) == EFI_TIMEOUT && !Owned);
  mDisableStatus = EFI_SUCCESS;
  assert (ProtocolSetClock (&mClockCrProtocol, "gcc", "gcc_usb3_prim_phy_aux_clk",
                            0, FALSE) == EFI_SUCCESS);
}

STATIC
VOID
TestAliasesAndPowerDomains (
  VOID
  )
{
  ResetHarness ();
  assert (mExternalSource == 2);
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_0_pipe_clk_src", 250000000,
            TRUE
            ) == EFI_SUCCESS);
  assert (strcmp (mLastName, "gcc_pcie_0_pipe_clk") == 0);
  assert (mSetRateCalls == 0 && mSelectExternalCalls == 1 &&
          mEnableCalls == 1 && mLastExternalId == 20 &&
          mExternalSource == 0);
  assert (mExternalRate == 0 && mExternalDivider == 0 && mExternalM == 0 &&
          mExternalN == 0 && mExternalTwiceD == 0);
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_0_pipe_clk", 250000000,
            TRUE
            ) == EFI_SUCCESS);
  assert (mSetRateCalls == 0 && mSelectExternalCalls == 1 &&
          mEnableCalls == 1);
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_0_pipe_clk", 0, FALSE
            ) == EFI_SUCCESS);
  assert (mDisableCalls == 0);
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_0_pipe_clk_src", 0, FALSE
            ) == EFI_SUCCESS);
  assert (mDisableCalls == 1 && mLastId == 20);

  ResetHarness ();
  assert (mExternalSource == 2);
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_1_phy_aux_clk_src", 20000000,
            TRUE
            ) == EFI_SUCCESS);
  assert (strcmp (mLastName, "gcc_pcie_1_phy_aux_clk") == 0);
  assert (mSetRateCalls == 0 && mSelectExternalCalls == 0 &&
          mEnableCalls == 1 && mExternalSource == 2);
  assert (mExternalRate == 0 && mExternalDivider == 0 && mExternalM == 0 &&
          mExternalN == 0 && mExternalTwiceD == 0);
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_1_phy_aux_clk", 20000000,
            TRUE
            ) == EFI_SUCCESS);
  assert (mSetRateCalls == 0 && mSelectExternalCalls == 0 &&
          mEnableCalls == 1 && mExternalSource == 2);
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_1_phy_aux_clk", 0, FALSE
            ) == EFI_SUCCESS);
  assert (mDisableCalls == 0);
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_1_phy_aux_clk_src", 0, FALSE
            ) == EFI_SUCCESS);
  assert (mDisableCalls == 1 && mLastId == 25);

  ResetHarness ();
  mExternalSource = 0;
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_1_phy_aux_clk", 20000000,
            TRUE
            ) == EFI_SUCCESS);
  assert (mSelectExternalCalls == 0 && mExternalSource == 0 &&
          mEnableCalls == 1);

  ResetHarness ();
  assert (ProtocolSetGdsc (
            &mClockCrProtocol, "gcc", "pcie_0_gdsc", TRUE
            ) == EFI_SUCCESS);
  assert (ProtocolSetGdsc (
            &mClockCrProtocol, "gcc", "pcie_0_gdsc", TRUE
            ) == EFI_SUCCESS);
  assert (mGetPowerIdCalls == 2 && mPowerEnableCalls == 1);
  assert (ProtocolSetGdsc (
            &mClockCrProtocol, "gcc", "pcie_0_gdsc", FALSE
            ) == EFI_SUCCESS);
  assert (mPowerDisableCalls == 0);
  mPowerDisableStatus = EFI_DEVICE_ERROR;
  assert (ProtocolSetGdsc (
            &mClockCrProtocol, "gcc", "pcie_0_gdsc", FALSE
            ) == EFI_DEVICE_ERROR);
  mPowerDisableStatus = EFI_SUCCESS;
  assert (ProtocolSetGdsc (
            &mClockCrProtocol, "gcc", "pcie_0_gdsc", FALSE
            ) == EFI_SUCCESS);
  assert (mPowerDisableCalls == 2);
}

STATIC
VOID
TestResetAndResolverErrors (
  VOID
  )
{
  ResetHarness ();
  assert (ProtocolSetReset (
            &mClockCrProtocol, "gcc", "gcc_pcie_0_bcr", TRUE
            ) == EFI_SUCCESS);
  assert (strcmp (mLastName, "gcc_pcie_0_aux_clk") == 0);
  assert (mLastId == 10 && mLastReset == CraneClockResetAssert);
  assert (ProtocolSetReset (
            &mClockCrProtocol, "gcc", "gcc_pcie_0_phy_bcr", FALSE
            ) == EFI_SUCCESS);
  assert (strcmp (mLastName, "gcc_pcie_0_phy_bcr") == 0);
  assert (mLastId == 30 && mLastReset == CraneClockResetDeassert);

  mResolveFlags = BIT2;
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_0_aux_clk", 0, TRUE
            ) == EFI_COMPROMISED_DATA);
  mResolveFlags = 0;
  mResolveNull = TRUE;
  assert (ProtocolSetGdsc (
            &mClockCrProtocol, "gcc", "pcie_0_gdsc", TRUE
            ) == EFI_COMPROMISED_DATA);
  mResolveNull = FALSE;
  mResolveStatus = EFI_NOT_FOUND;
  assert (ProtocolSetReset (
            &mClockCrProtocol, "gcc", "gcc_pcie_0_bcr", TRUE
            ) == EFI_NOT_FOUND);
  assert (ProtocolSetClock (
            &mClockCrProtocol, "rpmhcc", "clock", 0, TRUE
            ) == EFI_NOT_FOUND);
}

STATIC
VOID
TestSm8450UfsAndGdscResources (
  VOID
  )
{
  CONST CHAR8       *Name;
  UINT32             Flags;
  ClockDriverContext *Context;
  ClockNode          *Node;

  assert (CrTargetClockResolveResource (
            "gcc", "gcc_aggre_ufs_phy_axi_clk", CrDalClockResourceClock,
            &Name, &Flags) == EFI_SUCCESS);
  assert (strcmp (Name, "gcc_aggre_ufs_phy_axi_clk") == 0 && Flags == 0);

  Context = CrTargetGetClockContext ();
  assert (Context != NULL && Context->ClockControllers != NULL);
  Node = Context->ClockControllers[CC_TYPE_GCC].Clks[
           GCC_AGGRE_UFS_PHY_AXI_CLK];
  assert (Node != NULL && strcmp (Node->Name, "gcc_aggre_ufs_phy_axi_clk") == 0);
  assert (Node->HaltRegister == 0x870d4 && Node->EnableRegister == 0x870d4);
  assert (Node->EnableMsk == BIT (0) && Node->HwcgRegister == 0x870d4 &&
          Node->HwcgMsk == BIT (1));

  Node = Context->ClockControllers[CC_TYPE_GCC].Clks[
           GCC_PCIE_0_PIPE_CLK_SRC];
  assert (Node != NULL && Node->Type == CLOCK_NODE_TYPE_PHY_MUX &&
          Node->MuxRegister == 0x7b060);
  Node = Context->ClockControllers[CC_TYPE_GCC].Clks[
           GCC_PCIE_0_PIPE_CLK];
  assert (Node != NULL && Node->ParentCount == 1 &&
          Node->Parents[0] == Context->ClockControllers[CC_TYPE_GCC].Clks[
                                  GCC_PCIE_0_PIPE_CLK_SRC]);
  Node = Context->ClockControllers[CC_TYPE_GCC].Clks[
           GCC_PCIE_1_PHY_AUX_CLK_SRC];
  assert (Node != NULL && Node->Type == CLOCK_NODE_TYPE_MUX &&
          Node->MuxRegister == 0x9d080);
  assert (CrTargetClockResolveResource (
            "gcc", "gcc_pcie_1_phy_aux_clk_src", CrDalClockResourceClock,
            &Name, &Flags) == EFI_SUCCESS);
  assert (strcmp (Name, "gcc_pcie_1_phy_aux_clk") == 0 &&
          Flags == CR_DAL_CLOCK_RESOURCE_NO_RATE);
  assert (CrTargetClockResolveResource (
            "gcc", "gcc_pcie_1_phy_aux_clk", CrDalClockResourceClock,
            &Name, &Flags) == EFI_SUCCESS);
  assert (strcmp (Name, "gcc_pcie_1_phy_aux_clk") == 0 &&
          Flags == CR_DAL_CLOCK_RESOURCE_NO_RATE);
  Node = Context->ClockControllers[CC_TYPE_GCC].Clks[
           GCC_PCIE_1_PIPE_CLK];
  assert (Node != NULL && Node->ParentCount == 1 &&
          Node->Parents[0] == Context->ClockControllers[CC_TYPE_GCC].Clks[
                                  GCC_PCIE_1_PIPE_CLK_SRC]);

  assert (CrTargetClockResolveResource (
            "gcc", "pcie_0_gdsc", CrDalClockResourcePowerDomain,
            &Name, &Flags) == EFI_SUCCESS);
  assert (strcmp (Name, "pcie_0_gdsc") == 0 && Flags == 0);
  assert (CrTargetClockResolveResource (
            "gcc", "pcie_1_gdsc", CrDalClockResourcePowerDomain,
            &Name, &Flags) == EFI_SUCCESS);
  assert (strcmp (Name, "pcie_1_gdsc") == 0 && Flags == 0);
}

STATIC
VOID
TestNativeMuxSelector (
  VOID
  )
{
  ClockDriverContext *Context;
  ClockNode          *Node;

  Context = CrTargetGetClockContext ();
  assert (Context != NULL && Context->ClockControllers != NULL);
  Node = Context->ClockControllers[CC_TYPE_GCC].Clks[
           GCC_PCIE_1_PHY_AUX_CLK];
  assert (Node != NULL && Node->Type == CLOCK_NODE_TYPE_BRANCH_2 &&
          Node->ParentCount == 1 && Node->Parents != NULL &&
          Node->Parents[0]->Type == CLOCK_NODE_TYPE_MUX);

  /* The leaf aliases the regular mux source; source selection must program
   * the mux without trying to gate it. */
  mClockContext = Context;
  CrLockInit(&Context->Lock);Context->LockInitialized=TRUE;
  mMuxAddress = Node->Parents[0]->ParentController->Address +
                Node->Parents[0]->MuxRegister;
  mMuxValue = 0xa5a50000U;
  assert (CraneClockSelectSource (
            &mCraneNativeClock, (UINTN)Node, 0, 0, 0, 0, 0, 0) ==
          EFI_SUCCESS);
  assert (CraneClockSelectSource (
            &mCraneNativeClock, (UINTN)Node, 0, 2, 0, 0, 0, 0) ==
          EFI_SUCCESS);
  assert (CraneClockSelectSource (
            &mCraneNativeClock, (UINTN)Node, 0, 1, 0, 0, 0, 0) ==
          EFI_UNSUPPORTED);
  assert (mMuxValue == 0xa5a50002U);

  ResetHarness ();
  mClockContext = Context;
  CrLockInit(&Context->Lock);Context->LockInitialized=TRUE;
  mNativeClock = &mCraneNativeClock;
  mLookupNode = Node;
  mMuxAddress = Node->Parents[0]->ParentController->Address +
                Node->Parents[0]->MuxRegister;
  mMuxValue = 0xa5a50000U;
  Node->ReferenceCount = Node->Parents[0]->ReferenceCount = 1;
  assert (mClockCrProtocol.SetParent (
            &mClockCrProtocol, "gcc", "gcc_pcie_1_phy_aux_clk", 2) == EFI_SUCCESS);
  assert (mMuxValue == 0xa5a50002U && mMuxWrites == 1);
  assert (mClockCrProtocol.SetParent (
            &mClockCrProtocol, "gcc", "gcc_pcie_1_phy_aux_clk", 0) == EFI_SUCCESS);
  assert (mMuxValue == 0xa5a50000U && mMuxWrites == 2);
  Node->Parents[0]->ReferenceCount = 2;
  assert (mClockCrProtocol.SetParent (
            &mClockCrProtocol, "gcc", "gcc_pcie_1_phy_aux_clk", 2) == EFI_ACCESS_DENIED);
  assert (mMuxWrites == 2);
  Node->Parents[0]->ReferenceCount = 1;
  CLOCK_CR_VOTE *Vote = FindVote (mClockVotes, CLOCK_CR_MAX_CLOCK_VOTES,
                                 (UINTN)Node, TRUE);
  Vote->NativeId = (UINTN)Node;
  Vote->References = 2;
  assert (mClockCrProtocol.SetParent (
            &mClockCrProtocol, "gcc", "gcc_pcie_1_phy_aux_clk", 2) == EFI_ACCESS_DENIED);
  Vote->References = 1;
  Vote->Uncertain = TRUE;
  assert (mClockCrProtocol.SetParent (
            &mClockCrProtocol, "gcc", "gcc_pcie_1_phy_aux_clk", 2) == EFI_ACCESS_DENIED);
  Vote->Uncertain = FALSE;
  mDropMuxWrite = TRUE;
  assert (mClockCrProtocol.SetParent (
            &mClockCrProtocol, "gcc", "gcc_pcie_1_phy_aux_clk", 2) == EFI_DEVICE_ERROR);
  assert (mMuxValue == 0xa5a50000U && mMuxWrites == 4); /* failed selection + rollback */
  assert (Node->ReferenceCount == 1 && Node->Parents[0]->ReferenceCount == 1);
  Node->ReferenceCount = Node->Parents[0]->ReferenceCount = 0;
  {
    ClockNode Unsupported = { .Type = CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2 };
    mLookupNode = &Unsupported;
    assert (mClockCrProtocol.SetParent (
              &mClockCrProtocol, "gcc", "gcc_pcie_1_phy_aux_clk", 2) == EFI_UNSUPPORTED);
    Unsupported.Type = CLOCK_NODE_TYPE_PHY_MUX;
    assert (mClockCrProtocol.SetParent (
              &mClockCrProtocol, "gcc", "gcc_pcie_1_phy_aux_clk", 2) == EFI_UNSUPPORTED);
    assert (mMuxWrites == 4);
  }
}

STATIC
VOID
TestDisplayResourceResolution (
  VOID
  )
{
  CONST CHAR8 *Name;
  UINT32       Flags;

  assert (CrTargetClockResolveResource (
            "dispcc", "disp_cc_mdss_byte0_clk", CrDalClockResourceClock,
            &Name, &Flags) == EFI_SUCCESS);
  assert (strcmp (Name, "disp_cc_mdss_byte0_clk") == 0 && Flags == 0);
  assert (CrTargetClockResolveResource (
            "dispcc", "mdss_gdsc", CrDalClockResourcePowerDomain,
            &Name, &Flags) == EFI_SUCCESS);
  assert (strcmp (Name, "mdss_gdsc") == 0 && Flags == 0);
  assert (CrTargetClockResolveResource (
            "rpmhcc", "disp_cc_mdss_byte0_clk", CrDalClockResourceClock,
            &Name, &Flags) == EFI_UNSUPPORTED);
  assert (Name == NULL && Flags == 0);
  assert (CrTargetClockResolveResource (
            "dispcc", "disp_cc_mdss_byte0_clk", CrDalClockResourceMax,
            &Name, &Flags) == EFI_INVALID_PARAMETER);
}

STATIC
VOID
TestDisplaySourceAndDivider (
  VOID
  )
{
  ResetHarness ();
  assert (mClockCrProtocol.ConfigureExternalSource (
            &mClockCrProtocol, "dispcc", "disp_cc_mdss_pclk0_clk",
            148500000, 2, 3, 5, 7, 14) == EFI_SUCCESS);
  assert (strcmp (mLastName, "disp_cc_mdss_pclk0_clk") == 0);
  assert (mSelectExternalCalls == 1 && mLastExternalId == 51);
  assert (mExternalRate == 148500000 && mExternalSource == 2 &&
          mExternalDivider == 3 && mExternalM == 5 && mExternalN == 7 &&
          mExternalTwiceD == 14);
  assert (mClockCrProtocol.SetDivider (
            &mClockCrProtocol, "dispcc", "disp_cc_mdss_byte0_intf_clk",
            2) == EFI_SUCCESS);
  assert (strcmp (mLastName, "disp_cc_mdss_byte0_intf_clk") == 0);
  assert (mSetDividerCalls == 1 && mLastDividerId == 52 && mLastDivider == 2);
  assert (mGetIdCalls == 2 && mIsEnabledCalls == 2);
  assert (mEnableCalls == 0 && mDisableCalls == 0 && mSetRateCalls == 0);
  assert (FindVote (mClockVotes, CLOCK_CR_MAX_CLOCK_VOTES, 51, FALSE) == NULL);
  assert (FindVote (mClockVotes, CLOCK_CR_MAX_CLOCK_VOTES, 52, FALSE) == NULL);
  assert (mVoteLock.Lock == EfiLockReleased);
}

STATIC
VOID
TestReconfigurationWhileEnabled (
  VOID
  )
{
  ResetHarness ();
  /* An enabled clock outside this adapter also blocks reconfiguration. */
  mClockEnabled = TRUE;
  assert (ProtocolConfigureExternalSource (
            &mClockCrProtocol, "dispcc", "disp_cc_mdss_byte0_clk",
            111375000, 1, 1, 0, 0, 0) == EFI_ACCESS_DENIED);
  assert (ProtocolSetDivider (
            &mClockCrProtocol, "dispcc", "disp_cc_mdss_byte0_clk",
            2) == EFI_ACCESS_DENIED);
  assert (mIsEnabledCalls == 2);
  assert (mSelectExternalCalls == 0 && mSetDividerCalls == 0);
  assert (mVoteLock.Lock == EfiLockReleased);

  ResetHarness ();
  assert (ProtocolSetClock (
            &mClockCrProtocol, "dispcc", "disp_cc_mdss_byte0_clk", 0,
            TRUE) == EFI_SUCCESS);
  assert (mIsEnabledCalls == 1);
  /* Local ownership still blocks a transient hardware-disabled state. */
  mClockEnabled = FALSE;
  assert (ProtocolConfigureExternalSource (
            &mClockCrProtocol, "dispcc", "disp_cc_mdss_byte0_clk",
            111375000, 1, 1, 0, 0, 0) == EFI_ACCESS_DENIED);
  assert (ProtocolSetDivider (
            &mClockCrProtocol, "dispcc", "disp_cc_mdss_byte0_clk",
            2) == EFI_ACCESS_DENIED);
  assert (mSelectExternalCalls == 0 && mSetDividerCalls == 0);
  assert (mIsEnabledCalls == 1 && mVoteLock.Lock == EfiLockReleased);

  assert (ProtocolSetClock (
            &mClockCrProtocol, "dispcc", "disp_cc_mdss_byte0_clk", 0,
            FALSE) == EFI_SUCCESS);
  assert (ProtocolConfigureExternalSource (
            &mClockCrProtocol, "dispcc", "disp_cc_mdss_byte0_clk",
            111375000, 1, 1, 0, 0, 0) == EFI_SUCCESS);
  assert (ProtocolSetDivider (
            &mClockCrProtocol, "dispcc", "disp_cc_mdss_byte0_clk",
            2) == EFI_SUCCESS);
  assert (mSelectExternalCalls == 1 && mSetDividerCalls == 1);
}

STATIC
VOID
TestReconfigurationValidation (
  VOID
  )
{
  CONST struct {
    EFI_CLOCK_CR_PROTOCOL *Protocol;
    CONST CHAR8           *Controller;
    CONST CHAR8           *Id;
    UINT32                 Divider;
  } Invalid[] = {
    { NULL, "dispcc", "disp_cc_mdss_byte0_clk", 1 },
    { &mClockCrProtocol, NULL, "disp_cc_mdss_byte0_clk", 1 },
    { &mClockCrProtocol, "dispcc", NULL, 1 },
    { &mClockCrProtocol, "dispcc", "disp_cc_mdss_byte0_clk", 0 },
  };
  UINTN Index;

  ResetHarness ();
  for (Index = 0; Index < ARRAY_SIZE (Invalid); Index++) {
    assert (ProtocolConfigureExternalSource (
              Invalid[Index].Protocol, Invalid[Index].Controller,
              Invalid[Index].Id, 111375000, 1, Invalid[Index].Divider,
              0, 0, 0) == EFI_INVALID_PARAMETER);
    assert (ProtocolSetDivider (
              Invalid[Index].Protocol, Invalid[Index].Controller,
              Invalid[Index].Id, Invalid[Index].Divider
              ) == EFI_INVALID_PARAMETER);
  }
  mNativeClock = NULL;
  assert (ProtocolConfigureExternalSource (
            &mClockCrProtocol, "dispcc", "disp_cc_mdss_byte0_clk",
            111375000, 1, 1, 0, 0, 0) == EFI_NOT_READY);
  assert (ProtocolSetDivider (
            &mClockCrProtocol, "dispcc", "disp_cc_mdss_byte0_clk",
            2) == EFI_NOT_READY);
  assert (mGetIdCalls == 0 && mIsEnabledCalls == 0);
  assert (mSelectExternalCalls == 0 && mSetDividerCalls == 0);
}

STATIC
VOID
TestReconfigurationFailurePropagation (
  VOID
  )
{
  CONST EFI_STATUS Failures[] = {
    EFI_NOT_FOUND, EFI_COMPROMISED_DATA, EFI_NOT_FOUND, EFI_DEVICE_ERROR,
  };
  UINTN Index;

  for (Index = 0; Index < ARRAY_SIZE (Failures); Index++) {
    ResetHarness ();
    switch (Index) {
      case 0: mResolveStatus = Failures[Index]; break;
      case 1: mResolveFlags = BIT2; break;
      case 2: mGetIdStatus = Failures[Index]; break;
      case 3: mIsEnabledStatus = Failures[Index]; break;
    }
    assert (ProtocolConfigureExternalSource (
              &mClockCrProtocol, "dispcc", "disp_cc_mdss_byte0_clk",
              111375000, 1, 1, 0, 0, 0) == Failures[Index]);
    assert (ProtocolSetDivider (
              &mClockCrProtocol, "dispcc", "disp_cc_mdss_byte0_clk",
              2) == Failures[Index]);
    assert (mSelectExternalCalls == 0 && mSetDividerCalls == 0);
    assert (mVoteLock.Lock == EfiLockReleased);
  }

  ResetHarness ();
  mSelectExternalStatus = EFI_DEVICE_ERROR;
  mSetDividerStatus = EFI_TIMEOUT;
  assert (ProtocolConfigureExternalSource (
            &mClockCrProtocol, "dispcc", "disp_cc_mdss_byte0_clk",
            111375000, 1, 1, 0, 0, 0) == EFI_DEVICE_ERROR);
  assert (ProtocolSetDivider (
            &mClockCrProtocol, "dispcc", "disp_cc_mdss_byte0_clk",
            2) == EFI_TIMEOUT);
  assert (mSelectExternalCalls == 1 && mSetDividerCalls == 1);
  assert (mVoteLock.Lock == EfiLockReleased);
  mSelectExternalStatus = EFI_SUCCESS;
  mSetDividerStatus = EFI_SUCCESS;
  assert (ProtocolConfigureExternalSource (
            &mClockCrProtocol, "dispcc", "disp_cc_mdss_byte0_clk",
            111375000, 1, 1, 0, 0, 0) == EFI_SUCCESS);
  assert (ProtocolSetDivider (
            &mClockCrProtocol, "dispcc", "disp_cc_mdss_byte0_clk",
            2) == EFI_SUCCESS);
}

SPIN_LOCK *EFIAPI InitializeSpinLock(SPIN_LOCK *Lock) { *Lock=1;return Lock; }
SPIN_LOCK *EFIAPI AcquireSpinLock(SPIN_LOCK *Lock) { assert(*Lock==1);*Lock=2;return Lock; }
SPIN_LOCK *EFIAPI ReleaseSpinLock(SPIN_LOCK *Lock) { assert(*Lock==2);*Lock=1;return Lock; }
int
main (void)
{
  TestAbiAndEntryPoint ();
  TestClockVotesAndRates ();
  TestNativeRetainedCleanup ();
  TestAliasesAndPowerDomains ();
  TestResetAndResolverErrors ();
  TestSm8450UfsAndGdscResources ();
  TestNativeMuxSelector ();
  {
    ClockDriverContext *Context = CrTargetGetClockContext();
    ClockNode *Node = &DispDpPixel;
    mClockContext = Context;
    CrLockInit(&Context->Lock);Context->LockInitialized=TRUE;
    mPixelConfig = 0x0AF081C0;
    mPixelValues[0] = 0xF0003000;
    assert(CraneClockSelectSource(&mCraneNativeClock, (UINTN)Node,
             148500000, 2, 2, 11, 100, 100) == EFI_SUCCESS);
    assert(mPixelValues[0] == 0xF0002201 && mPixelValues[1] == 11);
    assert(mPixelValues[2] == 0xFFA6 && mPixelValues[3] == 0xFF9B);
    UINTN Writes = mPixelWrites;
    assert(CraneClockSelectSource(&mCraneNativeClock, (UINTN)Node,
             148500000, 2, 2, 0, 100, 100) == EFI_INVALID_PARAMETER);
    assert(CraneClockSelectSource(&mCraneNativeClock, (UINTN)Node,
             148500000, 2, 2, 11, 65536, 100) == EFI_INVALID_PARAMETER);
    assert(mPixelWrites == Writes);
    assert(CraneClockSelectSource(&mCraneNativeClock, (UINTN)Node,
             148500000, 2, 2, 0, 0, 0) == EFI_SUCCESS);
    assert(mPixelValues[0] == 0xF0000201);
    Node = &DispDpLinkIntf;
    mMuxAddress = 0x0AF081B4; mMuxValue = 3; mMuxWrites = 0;
    assert(CraneClockSetDivider(&mCraneNativeClock, (UINTN)Node, 4) == EFI_SUCCESS);
    assert(CraneClockSetDivider(&mCraneNativeClock, (UINTN)Node, 1) == EFI_UNSUPPORTED);
    assert(mMuxWrites == 0);
    mPixelConfig = 0;
  }
  TestDisplayResourceResolution ();
  TestDisplaySourceAndDivider ();
  TestReconfigurationWhileEnabled ();
  TestReconfigurationValidation ();
  TestReconfigurationFailurePropagation ();
  puts ("ClockCrDxe: native ABI, SM8450 resources, votes, reset, source/divider, and rollback passed");
  return 0;
}
