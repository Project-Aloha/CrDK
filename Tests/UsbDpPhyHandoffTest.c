/* Actual native PHY frontend and pointer-free MDSH core. SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <stdarg.h>
#define main ExistingPhyMain
#include "UsbDpPhyTest.c"
#undef main

EFI_GUID gEfiMdssHandoffProtocolGuid = EFI_MDSS_HANDOFF_PROTOCOL_GUID;
EFI_GUID gEfiUsbDpPhyCrProtocolGuid = EFI_USB_DP_PHY_CR_PROTOCOL_GUID;
EFI_GUID gEfiClockCrProtocolGuid = EFI_CLOCK_CR_PROTOCOL_GUID;
EFI_GUID gEfiRpmhCrProtocolGuid = EFI_RPMH_CR_PROTOCOL_GUID;
EFI_GUID gEfiI2cCrProtocolGuid = {0};
STATIC UINTN mBootCalls, mFreed, mRegistered, mUninstalled, mClockAcquires, mFailClock;
STATIC EFI_STATUS mRegisterStatus, mUninstallStatus, mInstallStatus;
STATIC EFI_MDSS_HANDOFF_FREEZE mRegisteredFreeze;
STATIC EFI_MDSS_HANDOFF_SNAPSHOT mRegisteredSnapshot;
STATIC VOID *mRegisteredContext;
STATIC CR_MDSS_HANDOFF_PARTICIPANT mSnapshot, mUnchanged;

VOID *EFIAPI ZeroMem(VOID *P, UINTN N) { return memset(P, 0, N); }
VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N) { return memcpy(D, S, N); }
UINTN EFIAPI AsciiStrLen(CONST CHAR8 *S) { return strlen(S); }
VOID EFIAPI FreePool(VOID *P) { ++mFreed; free(P); }

STATIC EFI_STATUS EFIAPI SetParticipant(EFI_MDSS_HANDOFF_PROTOCOL *This, UINT32 Owner,
  VOID *Context, EFI_MDSS_HANDOFF_FREEZE Freeze, EFI_MDSS_HANDOFF_SNAPSHOT Snapshot)
{
  assert(This == mHandoff && Owner == CR_MDSS_NATIVE_PHY && Context == &mProtocol);
  assert(Freeze != NULL && Snapshot != NULL);
  ++mBootCalls; ++mRegistered;
  if (mRegisterStatus != EFI_SUCCESS) return mRegisterStatus;
  mRegisteredContext = Context; mRegisteredFreeze = Freeze; mRegisteredSnapshot = Snapshot;
  return EFI_SUCCESS;
}
STATIC EFI_MDSS_HANDOFF_PROTOCOL mHandoffMock = {
  EFI_MDSS_HANDOFF_PROTOCOL_REVISION, SetParticipant
};
STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *Guid, VOID *Registration, VOID **Interface)
{
  assert(Registration == NULL); ++mBootCalls;
  if (Guid == &gEfiMdssHandoffProtocolGuid) *Interface = &mHandoffMock;
  else if (Guid == &gEfiClockCrProtocolGuid) *Interface = &mClockMock;
  else { assert(Guid == &gEfiRpmhCrProtocolGuid); *Interface = &mRailMock; }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI LocateHandles(EFI_LOCATE_SEARCH_TYPE Type, EFI_GUID *Guid,
  VOID *Key, UINTN *Count, EFI_HANDLE **Handles)
{
  assert(Type == ByProtocol && Guid == &gEfiI2cCrProtocolGuid && Key == NULL);
  ++mBootCalls; *Count = 1; *Handles = malloc(sizeof(**Handles));
  assert(*Handles != NULL); **Handles = &mI2cMock; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Handle(EFI_HANDLE Device, EFI_GUID *Guid, VOID **Interface)
{
  assert(Device == &mI2cMock && Guid == &gEfiI2cCrProtocolGuid); ++mBootCalls;
  *Interface = &mI2cMock; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Install(EFI_HANDLE *HandleOut, ...)
{
  va_list Args; va_start(Args, HandleOut);
  assert(va_arg(Args, EFI_GUID *) == &gEfiUsbDpPhyCrProtocolGuid);
  assert(va_arg(Args, VOID *) == &mProtocol && va_arg(Args, VOID *) == NULL);
  va_end(Args); ++mBootCalls; return mInstallStatus;
}
STATIC EFI_STATUS EFIAPI Uninstall(EFI_HANDLE Device, ...)
{
  assert(Device == (EFI_HANDLE)(UINTN)1); ++mBootCalls; ++mUninstalled;
  return mUninstallStatus;
}
STATIC EFI_BOOT_SERVICES mBoot = {
  .RaiseTPL = RaiseTpl, .RestoreTPL = RestoreTpl,
  .LocateProtocol = Locate, .LocateHandleBuffer = LocateHandles, .HandleProtocol = Handle,
  .InstallMultipleProtocolInterfaces = Install, .UninstallMultipleProtocolInterfaces = Uninstall
};
STATIC EFI_STATUS EFIAPI AcquireCommittedClock(EFI_CLOCK_CR_PROTOCOL *P,
  CONST CHAR8 *C, CONST CHAR8 *N, UINT64 R, BOOLEAN *Owned)
{
  EFI_STATUS S = Clock(P, C, N, R, TRUE);
  *Owned = TRUE;
  return ++mClockAcquires == mFailClock ? EFI_TIMEOUT : S;
}
/* A distinct simulated image for each case; no production reset/unfreeze API. */
STATIC VOID NewImage(VOID)
{
  memset(&mPhy, 0, sizeof(mPhy));
  memset(mOwned, 0, sizeof(mOwned)); memset(mReady, 0, sizeof(mReady));
  memset(&mFsa, 0, sizeof(mFsa)); memset(&mRoute, 0, sizeof(mRoute));
  mCommonReady = mCommonTouched = mUsbTouched = mDpTouched = mUsbSs = mFrozen = FALSE;
  mCommonStatus = EFI_SUCCESS; memset(mClientStatus, 0, sizeof(mClientStatus));
  mDpRate = mDpLanes = 0;
  for (UINTN I = 0; I < ARRAY_SIZE(mClocks); ++I) { mClocks[I].Owned = FALSE; mClocks[I].Status = EFI_SUCCESS; }
  for (UINTN I = 0; I < ARRAY_SIZE(mCommonRails); ++I) { mCommonRails[I].Owned = FALSE; mCommonRails[I].Status = EFI_SUCCESS; }
  for (UINTN I = 0; I < ARRAY_SIZE(mUsbRails); ++I) { mUsbRails[I].Owned = FALSE; mUsbRails[I].Status = EFI_SUCCESS; }
  Clear(); mI2cMock.Instance = 6; mClockMock.AcquireClock = AcquireCommittedClock;
  mBootCalls = mFreed = mRegistered = mUninstalled = mClockAcquires = mFailClock = 0;
  mRegisterStatus = mUninstallStatus = mInstallStatus = EFI_SUCCESS;
  mRegisteredContext = NULL; mRegisteredFreeze = NULL; mRegisteredSnapshot = NULL;
  gBS = &mBoot;
}
STATIC VOID FrozenSnapshot(VOID)
{
  UINTN Writes = mWrites, Resets = mResets, I2cCalls = mI2cCalls;
  UINTN BootCalls = mBootCalls, Acquires = mAcquireCalls, Clocks = mClockAcquires, Delays = mDelays;
  VOID *Map = (VOID *)(UINTN)1;
  EFI_TPL OldTpl = RaiseTpl(TPL_NOTIFY);
  assert(HandoffFreeze(&mProtocol) == EFI_SUCCESS);
  assert(HandoffFreeze(&mProtocol) == EFI_SUCCESS);
  memset(&mSnapshot, 0xa5, sizeof(mSnapshot));
  assert(HandoffSnapshot(&mProtocol, &mSnapshot, &Map) == EFI_SUCCESS && Map == NULL);
  assert(mSnapshot.Owner == CR_MDSS_NATIVE_PHY);
  assert(mWrites == Writes && mResets == Resets && mI2cCalls == I2cCalls && mBootCalls == BootCalls);
  assert(mAcquireCalls == Acquires && mClockAcquires == Clocks && mDelays == Delays);
  BOOLEAN Owned = TRUE; EFI_USB_TYPEC_STATE S = State(TRUE, FALSE);
  assert(Acquire(&mProtocol, UsbDpPhyUsb, &S, TRUE, &Owned) == EFI_ACCESS_DENIED && !Owned);
  assert(PowerDp(&mProtocol, 2700, 2) == EFI_ACCESS_DENIED);
  assert(Levels(&mProtocol, 0, 0) == EFI_ACCESS_DENIED);
  /* This is the late DWC3 EBS release: its exact lease remains retained. */
  assert(Release(&mProtocol, UsbDpPhyUsb) == EFI_ACCESS_DENIED);
  assert(Release(&mProtocol, UsbDpPhyDp) == EFI_ACCESS_DENIED);
  mUnchanged = mSnapshot;
  assert(HandoffSnapshot(&mProtocol, &mSnapshot, &Map) == EFI_SUCCESS);
  assert(!memcmp(&mSnapshot, &mUnchanged, sizeof(mSnapshot)));
  assert(mWrites == Writes && mResets == Resets && mI2cCalls == I2cCalls && mBootCalls == BootCalls);
  RestoreTpl(OldTpl);
  CR_MDSS_NATIVE_HANDOFF R = {0};
  assert(CrMdssHandoffInit(&R, 1, 2, 0xfc400000, 8192, 31, 31) == CR_SUCCESS);
  assert(CrMdssHandoffAppend(&R, &mSnapshot) == CR_SUCCESS);
  assert(!memcmp(&R.Phy, &mSnapshot.Phy, sizeof(R.Phy)));
}
STATIC EFI_STATUS ColdEntry(VOID)
{
  /* Clear() prepares the core for direct protocol fixture calls. Entry
     itself is exercised from a distinct cold image with zero core storage. */
  assert(!mPhy.Owner[0] && !mPhy.Owner[1]);
  memset(&mPhy, 0, sizeof(mPhy));
  return UsbDpPhyCrEntryPoint((EFI_HANDLE)(UINTN)1, NULL);
}
int main(void)
{
  assert(ExistingPhyMain() == 0);
  NewImage(); VOID *Map = (VOID *)(UINTN)1;
  memset(&mSnapshot, 0xa5, sizeof(mSnapshot)); mUnchanged = mSnapshot;
  assert(HandoffSnapshot(&mProtocol, &mSnapshot, &Map) == EFI_NOT_READY);
  assert(!memcmp(&mSnapshot, &mUnchanged, sizeof(mSnapshot)) && Map == (VOID *)(UINTN)1);
  assert(HandoffFreeze(NULL) == EFI_INVALID_PARAMETER && !mFrozen);
  FrozenSnapshot(); assert(mSnapshot.Phy.State == CR_MDSS_NATIVE_ABSENT && !mSnapshot.ClockCount && !mSnapshot.RailCount);

  /* A real higher-TPL observer preempts an admitted callback-TPL operation.
     Close future admissions, but do not bless an active journal or lower TPL. */
  NewImage(); EfiAcquireLock(&mLock);
  EFI_TPL PreviousTpl = RaiseTpl(TPL_NOTIFY);
  memset(&mSnapshot, 0xa5, sizeof(mSnapshot)); mUnchanged = mSnapshot;
  Map = (VOID *)(UINTN)1;
  assert(HandoffFreeze(&mProtocol) == EFI_NOT_READY && mFrozen && mCurrentTpl == TPL_NOTIFY);
  assert(HandoffSnapshot(&mProtocol, &mSnapshot, &Map) == EFI_NOT_READY);
  assert(!memcmp(&mSnapshot, &mUnchanged, sizeof(mSnapshot)) && Map == (VOID *)(UINTN)1);
  RestoreTpl(PreviousTpl); EfiReleaseLock(&mLock);
  FrozenSnapshot();

  NewImage(); assert(ColdEntry() == EFI_SUCCESS);
  assert(mRegistered == 1 && mRegisteredContext == &mProtocol && mFreed == 1 && mLock.Tpl == TPL_CALLBACK);
  EFI_USB_TYPEC_STATE S = State(TRUE, TRUE); BOOLEAN Owned;
  assert(Acquire(&mProtocol, UsbDpPhyDp, &S, FALSE, &Owned) == EFI_SUCCESS && Owned);
  assert(Acquire(&mProtocol, UsbDpPhyUsb, &S, TRUE, &Owned) == EFI_SUCCESS && Owned);
  assert(PowerDp(&mProtocol, 5400, 1) == EFI_SUCCESS);
  FrozenSnapshot();
  assert(mSnapshot.Phy.State == CR_MDSS_NATIVE_LIVE && mSnapshot.Phy.UsbOwners == 1 && mSnapshot.Phy.DpOwners == 1);
  assert(mSnapshot.Phy.Orientation == UsbTypeCReverse && mSnapshot.Phy.LaneCount == 1 && mSnapshot.Phy.LinkRate == 5400);
  assert(mSnapshot.Phy.ReadyMask == 31 && mSnapshot.Phy.TouchedMask == 15 && !mSnapshot.Phy.Status);
  assert(mSnapshot.ClockCount == 4 && mSnapshot.RailCount == 6);
  for (UINTN I = 0; I < 4; ++I) assert(mSnapshot.Clocks[I].Owner == CR_MDSS_NATIVE_PHY && !strcmp(mSnapshot.Clocks[I].Name, mClocks[I].Name) && mSnapshot.Clocks[I].RateHz == mClocks[I].Rate);
  for (UINTN I = 0; I < 6; ++I) assert(mSnapshot.Rails[I].Owner == CR_MDSS_NATIVE_PHY);
  assert(mRegisteredFreeze(mRegisteredContext) == EFI_SUCCESS);
  assert(mRegisteredSnapshot(mRegisteredContext, &mSnapshot, &Map) == EFI_SUCCESS);

  for (UINTN I = 1; I <= 4; ++I) {
    NewImage(); mFailClock = I;
    assert(Acquire(&mProtocol, UsbDpPhyDp, &S, FALSE, &Owned) == EFI_TIMEOUT && Owned);
    FrozenSnapshot();
    assert(mSnapshot.Phy.State == CR_MDSS_NATIVE_UNCERTAIN && mSnapshot.Phy.Status == EFI_TIMEOUT);
    assert(mSnapshot.ClockCount == I && mSnapshot.RailCount == 3 && mSnapshot.Clocks[I-1].Flags == 3);
  }
  NewImage(); mFailAcquire = 2;
  assert(Acquire(&mProtocol, UsbDpPhyDp, &S, FALSE, &Owned) == EFI_DEVICE_ERROR && Owned);
  FrozenSnapshot(); assert(mSnapshot.RailCount == 2 && mSnapshot.Rails[1].Flags == 3);
  assert(mSnapshot.Phy.Status == EFI_DEVICE_ERROR && !mSnapshot.Phy.ReadyMask && !mSnapshot.Phy.TouchedMask);

  for (UINTN I = 4; I <= 6; ++I) {
    NewImage(); mFailAcquire = I;
    assert(Acquire(&mProtocol, UsbDpPhyUsb, &S, TRUE, &Owned) == EFI_DEVICE_ERROR && Owned);
    FrozenSnapshot();
    assert(mSnapshot.ClockCount == 4 && mSnapshot.RailCount == I && mSnapshot.Rails[I-1].Flags == 3);
    assert(mSnapshot.Phy.UsbOwners == 1 && !mSnapshot.Phy.DpOwners && mSnapshot.Phy.Status == EFI_DEVICE_ERROR);
    assert(mSnapshot.Phy.ReadyMask == (CR_MDSS_NATIVE_PHY_COMMON | CR_MDSS_NATIVE_PHY_FSA));
  }

  NewImage(); assert(Acquire(&mProtocol, UsbDpPhyDp, &S, FALSE, &Owned) == EFI_SUCCESS);
  mPllFail = TRUE; assert(PowerDp(&mProtocol, 5400, 2) == EFI_TIMEOUT);
  FrozenSnapshot(); assert(mSnapshot.Phy.State == CR_MDSS_NATIVE_UNCERTAIN && mSnapshot.Phy.Status == EFI_TIMEOUT);
  assert(!mSnapshot.Phy.LinkRate && mSnapshot.Phy.LaneCount == 2 && mSnapshot.Phy.DpOwners == 1);

  NewImage(); assert(Acquire(&mProtocol, UsbDpPhyDp, &S, FALSE, &Owned) == EFI_SUCCESS);
  mFailRelease = TRUE; assert(Release(&mProtocol, UsbDpPhyDp) == EFI_DEVICE_ERROR);
  FrozenSnapshot(); assert(mSnapshot.ClockCount == 4 && mSnapshot.Clocks[3].Flags == 3 && mSnapshot.RailCount == 3);
  assert(!(mSnapshot.Phy.ReadyMask & CR_MDSS_NATIVE_PHY_COMMON) && !mSnapshot.Phy.Orientation);

  NewImage(); mFailReads = TRUE;
  assert(Acquire(&mProtocol, UsbDpPhyDp, &S, FALSE, &Owned) == EFI_DEVICE_ERROR);
  FrozenSnapshot(); assert(!mSnapshot.ClockCount && mSnapshot.RailCount == 3 && !mSnapshot.Phy.TouchedMask);

  for (UINTN I = 0; I < 3; ++I) {
    CONST UINTN Offsets[] = {0x6C, 0x70, 0x74};
    NewImage(); mFailTuneOffset = Offsets[I];
    assert(Acquire(&mProtocol, UsbDpPhyUsb, &S, TRUE, &Owned) == EFI_DEVICE_ERROR && Owned);
    FrozenSnapshot();
    assert(mSnapshot.Phy.State == CR_MDSS_NATIVE_UNCERTAIN && mSnapshot.Phy.Status == EFI_DEVICE_ERROR);
    assert(mSnapshot.Phy.TouchedMask & CR_MDSS_NATIVE_PHY_USB);
    assert(!(mSnapshot.Phy.ReadyMask & (CR_MDSS_NATIVE_PHY_USB | CR_MDSS_NATIVE_PHY_USB_SS)));
    assert(mSnapshot.ClockCount == 4 && mSnapshot.RailCount == 6 && mSnapshot.Phy.UsbOwners == 1);
  }

  NewImage(); mFailI2c = 2; mFailI2cCommits = TRUE;
  assert(Acquire(&mProtocol, UsbDpPhyDp, &S, FALSE, &Owned) == EFI_DEVICE_ERROR);
  FrozenSnapshot(); assert(mSnapshot.Phy.TouchedMask == CR_MDSS_NATIVE_PHY_FSA && !mSnapshot.Phy.ReadyMask);

  NewImage(); mRegisterStatus = EFI_DEVICE_ERROR;
  assert(ColdEntry() == EFI_DEVICE_ERROR && mUninstalled == 1);
  NewImage(); mRegisterStatus = EFI_DEVICE_ERROR; mUninstallStatus = EFI_ACCESS_DENIED;
  assert(ColdEntry() == EFI_SUCCESS && mUninstalled == 1 && !mRegisteredContext);
  NewImage(); mRegisterStatus = EFI_WARN_UNKNOWN_GLYPH;
  assert(ColdEntry() == EFI_NOT_READY && mUninstalled == 1);
  NewImage(); mInstallStatus = EFI_DEVICE_ERROR;
  assert(ColdEntry() == EFI_DEVICE_ERROR && !mRegistered);
  puts("Actual PHY handoff: immutable freeze, live/partial/error journals, raw masks, and resident-registration failures passed");
  return 0;
}
