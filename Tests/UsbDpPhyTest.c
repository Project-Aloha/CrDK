/** Independent register/resource failure fixtures. SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#ifndef CR_USB_DP_PHY_SOURCE
#define CR_USB_DP_PHY_SOURCE "../Driver/UsbDpPhyCrDxe/UsbDpPhyCrDxe.c"
#endif
#include CR_USB_DP_PHY_SOURCE

/* The native adapter's single authoritative portable journal. */
#define mOwned mPhy.Owner
#define mReady mPhy.Ready
#define mCommonReady mPhy.CommonReady
#define mCommonTouched mPhy.CommonTouched
#define mUsbTouched mPhy.UsbTouched
#define mDpTouched mPhy.DpTouched
#define mUsbSs mPhy.UsbSs
#define mFrozen mPhy.Frozen
#define mCommonStatus mPhy.CommonStatus
#define mClientStatus mPhy.ClientStatus
#define mRoute mPhy.Route
#define mDpRate mPhy.DpRate
#define mDpLanes mPhy.DpLanes
#define mClocks mPhy.Clocks
#define mCommonRails (*(CR_USB_DP_PHY_RAIL (*)[3])(&mPhy.Rails[0]))
#define mUsbRails (*(CR_USB_DP_PHY_RAIL (*)[3])(&mPhy.Rails[3]))

STATIC UINT32 mRegisters[0xC00], mHsRegisters[0x100];
STATIC UINT8 mFsaRegs[32];
STATIC UINTN mWrites, mResets, mDelays, mI2cCalls, mFailI2c, mAcquireCalls, mFailAcquire;
STATIC BOOLEAN mPllFail, mFailRelease;
STATIC BOOLEAN mComboReset, mUsb3Reset;
STATIC BOOLEAN mFailFrequency, mWrongFrequency;
STATIC BOOLEAN mWarnFrequency, mWarnI2c;
STATIC UINTN mWarnI2cAt;
STATIC BOOLEAN mFailReads, mFailI2cCommits;
STATIC UINTN mFrequencyCalls, mFailFrequencyAt;
STATIC struct { UINT8 Reg, Value; } mSwitchWrites[256];
STATIC UINTN mSwitchCount;
STATIC UINTN mTuneWrites, mFailTuneOffset;
EFI_BOOT_SERVICES *gBS;
VOID EFIAPI DebugPrint(UINTN L, CONST CHAR8 *F, ...) { (VOID)L; (VOID)F; }
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L) { (VOID)L; return FALSE; }
VOID EFIAPI DebugAssert(CONST CHAR8 *F, UINTN L, CONST CHAR8 *D)
{ fprintf(stderr, "%s:%zu %s\n", F, (size_t)L, D); assert(0); }
BOOLEAN EFIAPI DebugAssertEnabled(VOID) { return TRUE; }
UINTN EFIAPI MicroSecondDelay(UINTN Delay) { ++mDelays; return Delay; }
VOID EFIAPI MemoryFence(VOID) {}
VOID *EFIAPI SetMem(VOID *P, UINTN N, UINT8 V) { return memset(P, V, N); }
SPIN_LOCK *EFIAPI InitializeSpinLock(SPIN_LOCK *L) { *L = 0; return L; }
SPIN_LOCK *EFIAPI AcquireSpinLock(SPIN_LOCK *L) { assert(!*L); *L = 1; return L; }
SPIN_LOCK *EFIAPI ReleaseSpinLock(SPIN_LOCK *L) { assert(*L == 1); *L = 0; return L; }
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *P, UINT32 Old, UINT32 New)
{ UINT32 V = *P; if (V == Old) *P = New; return V; }
STATIC EFI_TPL mCurrentTpl = TPL_APPLICATION;
STATIC EFI_TPL EFIAPI RaiseTpl(EFI_TPL Tpl)
{ EFI_TPL Old = mCurrentTpl; assert(Tpl >= Old); mCurrentTpl = Tpl; return Old; }
STATIC VOID EFIAPI RestoreTpl(EFI_TPL Tpl)
{ assert(Tpl <= mCurrentTpl); mCurrentTpl = Tpl; }
STATIC EFI_BOOT_SERVICES mTplBoot = {.RaiseTPL = RaiseTpl, .RestoreTPL = RestoreTpl};
EFI_LOCK *EFIAPI EfiInitializeLock(EFI_LOCK *Lock, EFI_TPL Tpl)
{ Lock->Tpl = Tpl; Lock->OwnerTpl = TPL_APPLICATION; Lock->Lock = EfiLockReleased; return Lock; }
VOID EFIAPI EfiAcquireLock(EFI_LOCK *Lock)
{ assert(Lock->Lock == EfiLockReleased); Lock->OwnerTpl = RaiseTpl(Lock->Tpl); Lock->Lock = EfiLockAcquired; }
VOID EFIAPI EfiReleaseLock(EFI_LOCK *Lock)
{ assert(Lock->Lock == EfiLockAcquired); Lock->Lock = EfiLockReleased; RestoreTpl(Lock->OwnerTpl); }
UINT32 EFIAPI MmioRead32(UINTN Address)
{
  if (Address == 0x088EA178) return mPllFail ? 0 : 1; /* COM_C_READY_STATUS */
  if (Address == 0x088EA140) return mPllFail ? 0 : 3; /* COM_CMN_STATUS */
  if (Address == 0x088EAADC) return mPllFail ? 0 : 3;
  if (Address >= 0x088E8000 && Address < 0x088EB000)
    return mRegisters[(Address - 0x088E8000) / 4];
  assert(Address >= 0x088E3000 && Address < 0x088E3400);
  UINTN Offset = Address - 0x088E3000;
  UINT32 Value = mHsRegisters[Offset / 4];
  /* A write can commit but its qualified readback still disagree. */
  return Offset == mFailTuneOffset && (Value & 0xFFU) ? Value ^ 1U : Value;
}
UINT32 EFIAPI MmioWrite32(UINTN Address, UINT32 Value)
{
  ++mWrites;
  if (Address >= 0x088E8000 && Address < 0x088EB000)
    mRegisters[(Address - 0x088E8000) / 4] = Value;
  else {
    assert(Address >= 0x088E3000 && Address < 0x088E3400);
    UINTN Offset = Address - 0x088E3000;
    if (Offset == 0x6C || Offset == 0x70 || Offset == 0x74) {
      assert(mHsRegisters[0x50 / 4] & BIT1); /* POR still asserted */
      assert(mHsRegisters[0x94 / 4] & BIT1); /* common override still asserted */
      ++mTuneWrites;
    }
    mHsRegisters[(Address - 0x088E3000) / 4] = Value;
  }
  return Value;
}
UINT32 EFIAPI MmioOr32(UINTN A, UINT32 O) { return MmioWrite32(A, MmioRead32(A) | O); }
UINT32 EFIAPI MmioAnd32(UINTN A, UINT32 M) { return MmioWrite32(A, MmioRead32(A) & M); }
UINT32 EFIAPI MmioAndThenOr32(UINTN A, UINT32 M, UINT32 O)
{ return MmioWrite32(A, (MmioRead32(A) & M) | O); }
STATIC EFI_STATUS EFIAPI Clock(EFI_CLOCK_CR_PROTOCOL *T, CONST CHAR8 *C, CONST CHAR8 *N, UINT64 R, BOOLEAN E)
{
  (VOID)T; (VOID)R; assert(strcmp(C,"gcc") == 0 && N != NULL);
  if (E && strcmp(N, "gcc_usb3_prim_phy_aux_clk") == 0 && (mComboReset || mUsb3Reset)) return EFI_TIMEOUT;
  return !E && mFailRelease ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Reset(EFI_CLOCK_CR_PROTOCOL *T, CONST CHAR8 *C, CONST CHAR8 *N, BOOLEAN A)
{
  (VOID)T; assert(strcmp(C,"gcc") == 0 && N != NULL);
  if (strcmp(N, "gcc_usb3_dp_phy_prim_bcr") == 0) mComboReset = A;
  if (strcmp(N, "gcc_usb3_phy_prim_bcr") == 0) mUsb3Reset = A;
  ++mResets; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Rail(EFI_RPMH_CR_PROTOCOL *T, CONST CHAR8 *N, UINT32 V, UINT8 M, BOOLEAN *O)
{ (VOID)T; assert(N != NULL && V != 0 && (M == 6 || M == 7)); *O = TRUE; return ++mAcquireCalls == mFailAcquire ? EFI_DEVICE_ERROR : EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI Unrail(EFI_RPMH_CR_PROTOCOL *T, CONST CHAR8 *N, BOOLEAN E)
{ (VOID)T; assert(N != NULL && !E); return mFailRelease ? EFI_DEVICE_ERROR : EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI I2c(EFI_I2C_CR_PROTOCOL *T, UINTN Addr, EFI_I2C_REQUEST_PACKET *P, EFI_EVENT E, EFI_STATUS *S)
{
  (VOID)S; assert(Addr == 0x42 && E == NULL && T->BusFrequencyKHz == 100);
  BOOLEAN Failed = ++mI2cCalls == mFailI2c;
  if (Failed && !mFailI2cCommits) return EFI_DEVICE_ERROR;
  assert(P->Operation[0].Buffer != NULL);
  UINT8 *Data = P->Operation[0].Buffer;
  assert(Data[0] < 32);
  if (P->OperationCount == 2) {
    if (mFailReads || Failed) return EFI_DEVICE_ERROR;
    assert(P->Operation[0].LengthInBytes == 1 && P->Operation[1].LengthInBytes == 1);
    assert(P->Operation[1].Flags == I2C_FLAG_READ);
    P->Operation[1].Buffer[0] = mFsaRegs[Data[0]];
  } else {
    assert(P->OperationCount == 1 && P->Operation[0].LengthInBytes == 2);
    mFsaRegs[Data[0]] = Data[1];
    assert(mSwitchCount < ARRAY_SIZE(mSwitchWrites));
    mSwitchWrites[mSwitchCount].Reg = Data[0]; mSwitchWrites[mSwitchCount++].Value = Data[1];
  }
  return Failed ? EFI_DEVICE_ERROR : (mWarnI2c || mI2cCalls == mWarnI2cAt) ? EFI_WARN_UNKNOWN_GLYPH : EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Frequency(EFI_I2C_CR_PROTOCOL *T, UINTN *Hz)
{
  assert(*Hz == 100000);
  if (mWarnFrequency) return EFI_WARN_UNKNOWN_GLYPH;
  if (++mFrequencyCalls == mFailFrequencyAt || mFailFrequency) return EFI_DEVICE_ERROR;
  if (mWrongFrequency) { *Hz = 400000; return EFI_SUCCESS; }
  T->BusFrequencyKHz = 100;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI AcquireClockMock(EFI_CLOCK_CR_PROTOCOL *P,
  CONST CHAR8 *C, CONST CHAR8 *N, UINT64 R, BOOLEAN *Owned)
{
  EFI_STATUS Status = Clock(P, C, N, R, TRUE);
  *Owned = !EFI_ERROR(Status);
  return Status;
}
STATIC EFI_CLOCK_CR_PROTOCOL mClockMock = {.Revision=EFI_CLOCK_CR_PROTOCOL_REVISION, .SetClock = Clock, .SetReset = Reset, .AcquireClock=AcquireClockMock};
STATIC EFI_RPMH_CR_PROTOCOL mRailMock = {.AcquireVreg = Rail, .RpmhEnableVreg = Unrail};
STATIC EFI_I2C_CR_PROTOCOL mI2cMock = {.StartRequest = I2c, .SetBusFrequency = Frequency};
STATIC EFI_USB_TYPEC_STATE State(BOOLEAN Dp, BOOLEAN Reverse)
{
  EFI_USB_TYPEC_STATE S = {0};
  S.PdReady = S.Connected = TRUE;
  S.Orientation = Reverse ? UsbTypeCReverse : UsbTypeCNormal;
  S.DpActive = Dp; S.DpLanes = Dp ? 2 : 0; S.PinAssignment = Dp ? 4 : 0; S.Usb3 = TRUE;
  return S;
}
STATIC VOID Clear(VOID)
{
  gBS = &mTplBoot; mCurrentTpl = TPL_APPLICATION; EfiInitializeLock(&mLock, TPL_CALLBACK);
  assert(!mOwned[0] && !mOwned[1]);
  assert(!mPhy.SwitchTouched && !mCommonReady && !mCommonTouched);
  for (UINTN I = 0; I < ARRAY_SIZE(mCommonRails); ++I) assert(!mCommonRails[I].Owned);
  for (UINTN I = 0; I < ARRAY_SIZE(mClocks); ++I) assert(!mClocks[I].Owned);
  memset(mRegisters,0,sizeof(mRegisters)); memset(mHsRegisters,0,sizeof(mHsRegisters));
  memset(mFsaRegs,0,sizeof(mFsaRegs)); mFsaRegs[0] = 9;
  mFsa = (CR_FSA4480){.Bus = &mI2cMock};
  mClock = &mClockMock; mRpmh = &mRailMock;
  memset(&mPhy, 0, sizeof(mPhy)); assert(InitializeCore() == EFI_SUCCESS);
  mWrites = mResets = mDelays = mI2cCalls = mAcquireCalls = mFailAcquire = mFailI2c = mSwitchCount = 0;
  mPllFail = mFailRelease = FALSE;
  mComboReset = mUsb3Reset = FALSE;
  mFailFrequency = mWrongFrequency = FALSE; mI2cMock.BusFrequencyKHz = 400;
  mWarnFrequency = mWarnI2c = FALSE;
  mWarnI2cAt = 0;
  mFailReads = mFailI2cCommits = FALSE;
  mFrequencyCalls = mFailFrequencyAt = 0;
  mTuneWrites = mFailTuneOffset = 0;
}
STATIC VOID CheckHsTune(VOID)
{
  assert(mTuneWrites == 3);
  assert(mHsRegisters[0x6C / 4] == 0x765432E6);
  assert(mHsRegisters[0x70 / 4] == 0xFEDCBA0C);
  assert(mHsRegisters[0x74 / 4] == 0xABCDEF17);
  assert(!(mHsRegisters[0x50 / 4] & BIT1));
  assert(!(mHsRegisters[0x94 / 4] & BIT1));
}
STATIC VOID SeedHsTune(VOID)
{
  mHsRegisters[0x6C / 4] = 0x76543200;
  mHsRegisters[0x70 / 4] = 0xFEDCBA00;
  mHsRegisters[0x74 / 4] = 0xABCDEF00;
}
int main(void)
{
  BOOLEAN Owned;
  Clear();
  EFI_USB_TYPEC_STATE S = State(TRUE, FALSE);
  assert(Acquire(&mProtocol, UsbDpPhyDp, &S, FALSE, &Owned) == EFI_SUCCESS && Owned);
  assert(mFsaRegs[4] == 0xF8 && mFsaRegs[5] == 0x18);
  assert(!mTuneWrites); /* acquiring DP must not rewrite the independent HS PHY */
  UINTN Resets = mResets;
  SeedHsTune();
  assert(Acquire(&mProtocol, UsbDpPhyUsb, &S, TRUE, &Owned) == EFI_SUCCESS && Owned);
  CheckHsTune();
  assert(mResets == Resets + 2); /* Only independent HS reset, no common reset. */
  EFI_USB_TYPEC_STATE Flipped = State(TRUE, TRUE);
  assert(Release(&mProtocol, UsbDpPhyDp) == EFI_SUCCESS);
  assert(Acquire(&mProtocol, UsbDpPhyDp, &Flipped, FALSE, &Owned) == EFI_NOT_READY && !Owned);
  assert(Release(&mProtocol, UsbDpPhyUsb) == EFI_SUCCESS);
  assert(Acquire(&mProtocol, UsbDpPhyDp, &Flipped, FALSE, &Owned) == EFI_SUCCESS);
  assert(mFsaRegs[4] == 0xF8 && mFsaRegs[5] == 0x78);
  assert(PowerDp(&mProtocol, 2700, 2) == EFI_SUCCESS);
  assert(MmioRead32(0x088EAA1C) == 0x4C && MmioRead32(0x088EAA18) == 0x6D);
  assert(Levels(&mProtocol, 3, 1) == EFI_INVALID_PARAMETER);
  assert(Levels(&mProtocol, 1, 1) == EFI_SUCCESS);
  assert(MmioRead32(0x088EA214) == 0x3E); /* TX_DRV_LVL v4 */
  mPllFail = TRUE;
  assert(PowerDp(&mProtocol, 5400, 2) == EFI_TIMEOUT);
  assert(Levels(&mProtocol, 0, 0) == EFI_NOT_READY);
  mPllFail = FALSE; mFailRelease = TRUE;
  assert(Release(&mProtocol, UsbDpPhyDp) == EFI_DEVICE_ERROR && mOwned[UsbDpPhyDp]);
  assert(Acquire(&mProtocol, UsbDpPhyUsb, &S, TRUE, &Owned) == EFI_NOT_READY && !Owned);
  mFailRelease = FALSE;
  assert(Release(&mProtocol, UsbDpPhyDp) == EFI_SUCCESS);
  Clear();
  mFailAcquire = 1;
  assert(Acquire(&mProtocol, UsbDpPhyDp, &S, FALSE, &Owned) == EFI_DEVICE_ERROR && Owned);
  assert(mI2cCalls == 0 && mWrites == 0);
  assert(Release(&mProtocol, UsbDpPhyDp) == EFI_SUCCESS && mI2cCalls == 0);
  Clear();
  mFailI2c = 14; /* readback after defaults, retaining the switch supply */
  assert(Acquire(&mProtocol, UsbDpPhyDp, &S, FALSE, &Owned) == EFI_DEVICE_ERROR && Owned);
  assert(!mCommonReady && mCommonRails[0].Owned);
  assert(Release(&mProtocol, UsbDpPhyDp) == EFI_SUCCESS);
  Clear();
  S.DpLanes = 4; S.PinAssignment = 3; S.Usb3 = FALSE;
  assert(Acquire(&mProtocol, UsbDpPhyDp, &S, FALSE, &Owned) == EFI_SUCCESS);
  assert(mFsaRegs[4] == 0xE0 && mFsaRegs[5] == 0);
  assert(Acquire(&mProtocol, UsbDpPhyUsb, &S, TRUE, &Owned) == EFI_UNSUPPORTED && !Owned);
  SeedHsTune();
  assert(Acquire(&mProtocol, UsbDpPhyUsb, &S, FALSE, &Owned) == EFI_SUCCESS);
  CheckHsTune();
  Resets = mResets;
  assert(Release(&mProtocol, UsbDpPhyUsb) == EFI_SUCCESS && mCommonReady && mResets == Resets + 1);
  assert(PowerDp(&mProtocol, 1620, 4) == EFI_SUCCESS);
  assert(Release(&mProtocol, UsbDpPhyDp) == EFI_SUCCESS);
  /* Probe-only failure must not alter an inherited route or need a live bus
     to retire the rails. This reproduces the unplug cleanup retry loop. */
  Clear(); mFailReads = TRUE;
  mFsaRegs[4] = 0xE0; mFsaRegs[5] = 0x60;
  assert(Acquire(&mProtocol, UsbDpPhyDp, &S, FALSE, &Owned) == EFI_DEVICE_ERROR && Owned);
  assert(mI2cCalls == 1 && mSwitchCount == 0 && mWrites == 0);
  assert(Release(&mProtocol, UsbDpPhyDp) == EFI_SUCCESS);
  assert(mI2cCalls == 1 && mSwitchCount == 0);
  assert(mFsaRegs[4] == 0xE0 && mFsaRegs[5] == 0x60);
  Clear(); mFsaRegs[0] = 0xFF;
  assert(Acquire(&mProtocol, UsbDpPhyDp, &S, FALSE, &Owned) == EFI_NOT_FOUND && Owned);
  assert(Release(&mProtocol, UsbDpPhyDp) == EFI_SUCCESS);
  assert(mI2cCalls == 1 && mSwitchCount == 0);
  /* Frequency selection may fail after ID discovery but before the first
     data write. No switch cleanup is owned in that case either. */
  Clear(); mFailFrequencyAt = 2;
  assert(Acquire(&mProtocol, UsbDpPhyDp, &S, FALSE, &Owned) == EFI_DEVICE_ERROR && Owned);
  mFailFrequency = TRUE;
  assert(Release(&mProtocol, UsbDpPhyDp) == EFI_SUCCESS);
  assert(mI2cCalls == 1 && mSwitchCount == 0);
  /* Once a write was dispatched, an error cannot prove it did not commit.
     Exercise each default/route write and verification boundary with both
     pre-commit and post-commit bus failure outcomes. */
  for (UINTN Committed = 0; Committed != 2; ++Committed) {
    for (UINTN Failure = 2; Failure <= 20; ++Failure) {
      Clear(); mFailI2c = Failure; mFailI2cCommits = (BOOLEAN)Committed;
      assert(Acquire(&mProtocol, UsbDpPhyDp, &S, FALSE, &Owned) == EFI_DEVICE_ERROR && Owned);
      assert(mWrites == 0 && mCommonRails[0].Owned);
      mFailFrequency = TRUE;
      assert(Release(&mProtocol, UsbDpPhyDp) == EFI_DEVICE_ERROR && mOwned[UsbDpPhyDp]);
      assert(mCommonRails[0].Owned);
      mFailFrequency = FALSE;
      assert(Release(&mProtocol, UsbDpPhyDp) == EFI_SUCCESS);
      assert(mFsaRegs[4] == 0x98 && mFsaRegs[5] == 0x18);
    }
  }
  Clear(); mFailFrequency = TRUE;
  assert(Acquire(&mProtocol, UsbDpPhyDp, &S, FALSE, &Owned) == EFI_DEVICE_ERROR && Owned);
  assert(mI2cCalls == 0 && mWrites == 0);
  assert(Release(&mProtocol, UsbDpPhyDp) == EFI_SUCCESS);
  assert(mI2cCalls == 0);
  for (UINTN I = 0; I < 3; ++I) {
    CONST UINTN Offsets[] = {0x6C, 0x70, 0x74};
    Clear(); mFailTuneOffset = Offsets[I];
    assert(Acquire(&mProtocol, UsbDpPhyUsb, &S, FALSE, &Owned) == EFI_DEVICE_ERROR && Owned);
    assert(mTuneWrites == I + 1 && mUsbTouched && !mReady[UsbDpPhyUsb]);
    assert(mHsRegisters[0x50 / 4] & BIT1);
    assert(mHsRegisters[0x94 / 4] & BIT1);
    for (UINTN R = 0; R < ARRAY_SIZE(mUsbRails); ++R) assert(mUsbRails[R].Owned);
    mFailRelease = TRUE;
    assert(Release(&mProtocol, UsbDpPhyUsb) == EFI_DEVICE_ERROR && mOwned[UsbDpPhyUsb]);
    mFailRelease = FALSE;
    assert(Release(&mProtocol, UsbDpPhyUsb) == EFI_SUCCESS && !mOwned[UsbDpPhyUsb]);
  }
  Clear(); mWrongFrequency = TRUE;
  assert(Acquire(&mProtocol, UsbDpPhyDp, &S, FALSE, &Owned) == EFI_PROTOCOL_ERROR && Owned);
  assert(mI2cCalls == 0 && mWrites == 0);
  assert(Release(&mProtocol, UsbDpPhyDp) == EFI_SUCCESS);
  assert(mI2cCalls == 0);
  EFI_USB_TYPEC_STATE WarnState = State(TRUE, FALSE);
  Clear(); mWarnI2cAt = 2;
  assert(Acquire(&mProtocol, UsbDpPhyDp, &WarnState, FALSE, &Owned) == EFI_NOT_READY && Owned);
  assert(mI2cCalls == 2 && mSwitchCount == 1 && mPhy.SwitchTouched);
  mWarnI2c = TRUE;
  assert(Release(&mProtocol, UsbDpPhyDp) == EFI_NOT_READY && mOwned[UsbDpPhyDp]);
  assert(mCommonRails[0].Owned && mPhy.SwitchTouched);
  mWarnI2c = FALSE;
  assert(Release(&mProtocol, UsbDpPhyDp) == EFI_SUCCESS);
  Clear(); mWarnFrequency = TRUE;
  assert(Acquire(&mProtocol, UsbDpPhyDp, &WarnState, FALSE, &Owned) == EFI_NOT_READY && Owned);
  assert(mI2cCalls == 0 && mSwitchCount == 0 && !mPhy.SwitchTouched);
  assert(Release(&mProtocol, UsbDpPhyDp) == EFI_SUCCESS);
  Clear(); mWarnI2c = TRUE;
  assert(Acquire(&mProtocol, UsbDpPhyDp, &WarnState, FALSE, &Owned) == EFI_NOT_READY && Owned);
  assert(mI2cCalls == 1 && mSwitchCount == 0 && !mPhy.SwitchTouched);
  assert(Release(&mProtocol, UsbDpPhyDp) == EFI_SUCCESS);
  puts("USB/DP PHY: orientation, probe-only cleanup, partial writes and retained failures passed");
  return 0;
}
