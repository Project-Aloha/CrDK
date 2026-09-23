/** @file
  Host tests for the native ClockDxe adapter.

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

EFI_GUID gEfiClockProtocolGuid = EFI_CLOCK_PROTOCOL_GUID;
EFI_GUID gEfiClockCrProtocolGuid = EFI_CLOCK_CR_PROTOCOL_GUID;
EFI_BOOT_SERVICES *gBS;

STATIC EFI_STATUS mLocateStatus;
STATIC EFI_STATUS mInstallStatus;
STATIC EFI_STATUS mGetIdStatus;
STATIC EFI_STATUS mSetRateStatus;
STATIC EFI_STATUS mSelectExternalStatus;
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
STATIC UINTN      mEnableCalls;
STATIC UINTN      mDisableCalls;
STATIC UINTN      mIsEnabledCalls;
STATIC UINTN      mGetPowerIdCalls;
STATIC UINTN      mPowerEnableCalls;
STATIC UINTN      mPowerDisableCalls;
STATIC UINTN      mResetCalls;
STATIC UINTN      mLastId;
STATIC UINTN      mLastExternalId;
STATIC UINT32     mLastRate;
STATIC UINT32     mExternalSource;
STATIC EFI_CLOCK_FREQUENCY_TYPE mLastMatch;
STATIC EFI_CLOCK_RESET_TYPE     mLastReset;
STATIC CHAR8      mLastName[96];
STATIC EFI_CLOCK_PROTOCOL mMockNative;
STATIC BOOLEAN    mClockEnabled;
STATIC BOOLEAN    mEnableFailureLeavesEnabled;

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
  return 90;
}

STATIC
EFI_STATUS
EFIAPI
MockGetClockId (
  IN  EFI_CLOCK_PROTOCOL *This,
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
  IN EFI_CLOCK_PROTOCOL *This,
  IN UINTN               ClockId
  )
{
  assert (This == &mMockNative);
  if ((ClockId == 20) || (ClockId == 25)) {
    assert (mExternalSource == 0);
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
  IN EFI_CLOCK_PROTOCOL *This,
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
  IN  EFI_CLOCK_PROTOCOL *This,
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
  IN  EFI_CLOCK_PROTOCOL       *This,
  IN  UINTN                     ClockId,
  IN  UINT32                    Rate,
  IN  EFI_CLOCK_FREQUENCY_TYPE  Match,
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
  IN EFI_CLOCK_PROTOCOL *This,
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
  assert (Rate == 0 && Source == 0 && Divider == 0);
  assert (M == 0 && N == 0 && TwiceD == 0);
  mSelectExternalCalls++;
  mLastExternalId = ClockId;
  mExternalSource = Source;
  return mSelectExternalStatus;
}

STATIC
EFI_STATUS
EFIAPI
MockGetPowerId (
  IN  EFI_CLOCK_PROTOCOL *This,
  IN  CONST CHAR8        *Name,
  OUT UINTN              *PowerId
  )
{
  assert (This == &mMockNative && PowerId != NULL);
  assert (strcmp (Name, "gcc_pcie_0_gdsc") == 0);
  mGetPowerIdCalls++;
  *PowerId = 40;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MockEnablePower (
  IN EFI_CLOCK_PROTOCOL *This,
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
  IN EFI_CLOCK_PROTOCOL *This,
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
  IN EFI_CLOCK_PROTOCOL   *This,
  IN UINTN                 ClockId,
  IN EFI_CLOCK_RESET_TYPE  ResetType
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
  assert (Controller != NULL && Id != NULL && NativeId != NULL && Flags != NULL);
  if (EFI_ERROR (mResolveStatus)) {
    return mResolveStatus;
  }
  if (strcmp (Controller, "gcc") != 0) {
    return EFI_UNSUPPORTED;
  }

  *Flags = mResolveFlags;
  if ((Kind == CrDalClockResourceClock) &&
      ((strcmp (Id, "gcc_pcie_0_pipe_clk_src") == 0) ||
       (strcmp (Id, "gcc_pcie_0_pipe_clk") == 0))) {
    *NativeId = "gcc_pcie_0_pipe_clk";
    *Flags |= CR_DAL_CLOCK_RESOURCE_NO_RATE |
              CR_DAL_CLOCK_RESOURCE_EXTERNAL_SOURCE;
  } else if ((Kind == CrDalClockResourceClock) &&
             ((strcmp (Id, "gcc_pcie_1_phy_aux_clk_src") == 0) ||
              (strcmp (Id, "gcc_pcie_1_phy_aux_clk") == 0))) {
    *NativeId = "gcc_pcie_1_phy_aux_clk";
    *Flags |= CR_DAL_CLOCK_RESOURCE_NO_RATE |
              CR_DAL_CLOCK_RESOURCE_EXTERNAL_SOURCE;
  } else if ((Kind == CrDalClockResourcePowerDomain) &&
             (strcmp (Id, "pcie_0_gdsc") == 0)) {
    *NativeId = "gcc_pcie_0_gdsc";
  } else if ((Kind == CrDalClockResourceReset) &&
             (strcmp (Id, "gcc_pcie_0_bcr") == 0)) {
    *NativeId = "gcc_pcie_0_aux_clk";
  } else {
    *NativeId = Id;
  }
  if (mResolveNull) {
    *NativeId = NULL;
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MockLocateProtocol (
  IN  EFI_GUID *Protocol,
  IN  VOID     *Registration,
  OUT VOID    **Interface
  )
{
  assert (Protocol == &gEfiClockProtocolGuid && Registration == NULL);
  mLocateCalls++;
  *Interface = &mMockNative;
  return mLocateStatus;
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
  .LocateProtocol = MockLocateProtocol,
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
  mLocateStatus = EFI_SUCCESS;
  mInstallStatus = EFI_SUCCESS;
  mGetIdStatus = EFI_SUCCESS;
  mSetRateStatus = EFI_SUCCESS;
  mSelectExternalStatus = EFI_SUCCESS;
  mEnableStatus = EFI_SUCCESS;
  mDisableStatus = EFI_SUCCESS;
  mIsEnabledStatus = EFI_SUCCESS;
  mPowerEnableStatus = EFI_SUCCESS;
  mPowerDisableStatus = EFI_SUCCESS;
  mResetStatus = EFI_SUCCESS;
  mResolveStatus = EFI_SUCCESS;
  mResolveFlags = 0;
  mResolveNull = FALSE;
  mLocateCalls = 0;
  mInstallCalls = 0;
  mGetIdCalls = 0;
  mSetRateCalls = 0;
  mSelectExternalCalls = 0;
  mEnableCalls = 0;
  mDisableCalls = 0;
  mIsEnabledCalls = 0;
  mGetPowerIdCalls = 0;
  mPowerEnableCalls = 0;
  mPowerDisableCalls = 0;
  mResetCalls = 0;
  mLastId = 0;
  mLastExternalId = 0;
  mLastRate = 0;
  mExternalSource = 2;
  mLastMatch = EfiClockFrequencyHzAtLeast;
  mLastReset = EfiClockResetPulse;
  mClockEnabled = FALSE;
  mEnableFailureLeavesEnabled = FALSE;
  mVoteLock.Tpl = TPL_NOTIFY;
  mVoteLock.OwnerTpl = TPL_APPLICATION;
  mVoteLock.Lock = EfiLockReleased;
  memset (&mMockNative, 0, sizeof (mMockNative));
  mMockNative.Version = EFI_CLOCK_PROTOCOL_REVISION;
  mMockNative.GetClockID = MockGetClockId;
  mMockNative.EnableClock = MockEnableClock;
  mMockNative.DisableClock = MockDisableClock;
  mMockNative.IsClockEnabled = MockIsClockEnabled;
  mMockNative.SetClockFreqHz = MockSetRate;
  mMockNative.SelectExternalSource = MockSelectExternal;
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

  assert (offsetof (EFI_CLOCK_PROTOCOL, GetClockID) == sizeof (UINT64));
  assert (offsetof (EFI_CLOCK_PROTOCOL, ResetClock) ==
          sizeof (UINT64) + (15U * sizeof (VOID *)));
  assert (sizeof (EFI_CLOCK_PROTOCOL) ==
          sizeof (UINT64) + (16U * sizeof (VOID *)));

  ResetHarness ();
  mLocateStatus = EFI_NOT_FOUND;
  assert (ClockEntryPoint (Image, NULL) == EFI_NOT_FOUND);
  assert (mLocateCalls == 1 && mInstallCalls == 0 && mNativeClock == NULL);

  ResetHarness ();
  mMockNative.Version--;
  assert (ClockEntryPoint (Image, NULL) == EFI_INCOMPATIBLE_VERSION);
  assert (mInstallCalls == 0 && mNativeClock == NULL);

  ResetHarness ();
  mMockNative.ResetClock = NULL;
  assert (ClockEntryPoint (Image, NULL) == EFI_INCOMPATIBLE_VERSION);
  assert (mInstallCalls == 0 && mNativeClock == NULL);

  ResetHarness ();
  mInstallStatus = EFI_DEVICE_ERROR;
  assert (ClockEntryPoint (Image, NULL) == EFI_DEVICE_ERROR);
  assert (mInstallCalls == 1 && mNativeClock == NULL);

  ResetHarness ();
  assert (ClockEntryPoint (Image, NULL) == EFI_SUCCESS);
  assert (mLocateCalls == 1 && mInstallCalls == 1);
  assert (mNativeClock == &mMockNative);
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
  assert (mLastMatch == EfiClockFrequencyHzExact);
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
  assert (mSetRateCalls == 0 && mSelectExternalCalls == 1 &&
          mEnableCalls == 1 && mLastExternalId == 25 &&
          mExternalSource == 0);
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_1_phy_aux_clk", 20000000,
            TRUE
            ) == EFI_SUCCESS);
  assert (mSetRateCalls == 0 && mSelectExternalCalls == 1 &&
          mEnableCalls == 1);
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_1_phy_aux_clk", 0, FALSE
            ) == EFI_SUCCESS);
  assert (mDisableCalls == 0);
  assert (ProtocolSetClock (
            &mClockCrProtocol, "gcc", "gcc_pcie_1_phy_aux_clk_src", 0, FALSE
            ) == EFI_SUCCESS);
  assert (mDisableCalls == 1 && mLastId == 25);

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
  assert (mLastId == 10 && mLastReset == EfiClockResetAssert);
  assert (ProtocolSetReset (
            &mClockCrProtocol, "gcc", "gcc_pcie_0_phy_bcr", FALSE
            ) == EFI_SUCCESS);
  assert (strcmp (mLastName, "gcc_pcie_0_phy_bcr") == 0);
  assert (mLastId == 30 && mLastReset == EfiClockResetDeassert);

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

int
main (void)
{
  TestAbiAndEntryPoint ();
  TestClockVotesAndRates ();
  TestAliasesAndPowerDomains ();
  TestResetAndResolverErrors ();
  puts ("ClockCrDxe: native ABI, aliases, votes, reset, and rollback passed");
  return 0;
}
