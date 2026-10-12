/** Exercise the MU UIC command path with a write-one-to-clear IS model. */
#include <assert.h>
#include <stdio.h>
#undef NULL
#include <Uefi.h>
#include "UfsPassThruHci.c"

STATIC UINT32 mStatus, mArgs[3], mAck, mResult;
STATIC UINT32 mBusyStatusReads, mStatusReads, mArgWrites;
STATIC BOOLEAN mReadFailure, mWriteFailure;
STATIC BOOLEAN mPowerPending, mPowerNoCompletion, mPowerNoUccs;
STATIC UINT32 mPowerReads, mUpmcrs = 1, mPowerDispatches;
EDKII_UFS_HC_PLATFORM_PROTOCOL *mUfsHcPlatform;
STATIC UINT32 mHce = 1, mLinkAttempts, mLinkFailures, mResets, mPreLinks;
STATIC BOOLEAN mRecoveryFailure, mHceInitializing, mStopStuck;
STATIC BOOLEAN mDevicePresent = TRUE;
STATIC EFI_STATUS EFIAPI PlatformCallback (EFI_HANDLE Handle,
  EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE Phase, VOID *Interface)
{
  if (Phase == EdkiiUfsHcPreLinkStartup) mPreLinks++;
  if (Phase == EdkiiUfsHcPreHce) {
    if (!mUfsHcPlatform->SkipHceReenable) assert (mHce == 0);
    if (mRecoveryFailure) return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}
VOID EFIAPI DebugPrint (UINTN Level, CONST CHAR8 *Format, ...) { }
BOOLEAN EFIAPI DebugPrintEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled (CONST UINTN Level) { return FALSE; }
BOOLEAN EFIAPI DebugCodeEnabled (VOID) { return FALSE; }
UINT64 EFIAPI MultU64x32 (UINT64 Value, UINT32 Factor) { return Value * Factor; }
UINT64 EFIAPI DivU64x32 (UINT64 Dividend, UINT32 Divisor) { return Dividend / Divisor; }
UINTN EFIAPI MicroSecondDelay (UINTN Delay) {
  if (mHceInitializing && Delay >= 1000) mHceInitializing = FALSE;
  return Delay;
}
STATIC EFI_STATUS EFIAPI Read (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This,
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH Width, UINT64 Offset, UINTN Count, VOID *Buffer)
{
  UINT32 Value;
  if (mReadFailure) return EFI_DEVICE_ERROR;
  if (Offset == UFS_HC_IS_OFFSET) {
    if (mPowerPending && !mPowerNoCompletion && ++mPowerReads >= 4) {
      mStatus |= BIT4; mPowerPending = FALSE;
    }
    Value = mStatus;
  }
  else if (Offset == UFS_HC_ENABLE_OFFSET) { assert (!mHceInitializing); Value = mHce; }
  else if (Offset == UFS_HC_STATUS_OFFSET) {
    mStatusReads++;
    Value = (mDevicePresent ? UFS_HC_HCS_DP : 0) | (mUpmcrs << 8);
    if (mBusyStatusReads != 0) mBusyStatusReads--;
    else Value |= UFS_HC_HCS_UCRDY;
  }
  else { assert (Offset >= UFS_HC_UCMD_ARG1_OFFSET && Offset <= UFS_HC_UCMD_ARG3_OFFSET);
    Value = mArgs[(Offset - UFS_HC_UCMD_ARG1_OFFSET) / 4]; }
  *(UINT32 *)Buffer = Value;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Write (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This,
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH Width, UINT64 Offset, UINTN Count, VOID *Buffer)
{
  UINT32 Value = *(UINT32 *)Buffer;
  if (mWriteFailure) return EFI_DEVICE_ERROR;
  if (Offset == UFS_HC_IS_OFFSET) { mAck = Value; mStatus &= ~Value; }
  else if (Offset == UFS_HC_ENABLE_OFFSET) {
    if (Value == 0) mResets++;
    if (!(mStopStuck && Value == 0)) mHce = Value;
    mHceInitializing = (Value != 0);
  } else if (Offset == UFS_HC_UTMRLRSR_OFFSET || Offset == UFS_HC_UTRLRSR_OFFSET) {
    assert (Value == 0);
  } else if (Offset == UFS_HC_UIC_CMD_OFFSET) {
    assert (Value == UfsUicDmeGet || Value == UfsUicDmeSet || Value == UfsUicDmeLinkStartup);
    if (Value == UfsUicDmeSet && (mArgs[0] >> 16) == 0x1571) {
      assert (!(mStatus & BIT4)); /* Stale UPMS must be acknowledged before dispatch. */
      mPowerPending = TRUE; mPowerReads = 0; mPowerDispatches++;
    }
    if (Value == UfsUicDmeLinkStartup) {
      mLinkAttempts++;
      mArgs[1] = (mLinkAttempts <= mLinkFailures) ? 1 : 0;
    } else mArgs[1] = mResult;
    mArgs[2] = 0x12345678;
    if (!mPowerNoUccs) mStatus |= UFS_HC_IS_UCCS;
  } else { assert (Offset >= UFS_HC_UCMD_ARG1_OFFSET && Offset <= UFS_HC_UCMD_ARG3_OFFSET);
    assert (mBusyStatusReads == 0 && mStatusReads != 0);
    mArgWrites++;
    mArgs[(Offset - UFS_HC_UCMD_ARG1_OFFSET) / 4] = Value; }
  return EFI_SUCCESS;
}
int main (void)
{
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL Host = { .Read = Read, .Write = Write };
  UFS_PASS_THRU_PRIVATE_DATA Private = { .UfsHostController = &Host };
  EDKII_UIC_COMMAND Command = { .Opcode = UfsUicDmeGet, .Arg1 = 0x410000 };
  UINT32 Unrelated = UFS_HC_IS_ULSS | BIT0 | BIT2;
  mStatus = Unrelated | UFS_HC_IS_UCCS;
  assert (UfsExecUicCommands (&Private, &Command) == EFI_SUCCESS);
  assert (mAck == UFS_HC_IS_UCCS && (mStatus & Unrelated) == Unrelated);
  assert (Command.Arg3 == 0x12345678 && Command.Arg2 == 0);
  mBusyStatusReads = 2; mStatusReads = 0; mArgWrites = 0;
  assert (UfsExecUicCommands (&Private, &Command) == EFI_SUCCESS);
  assert (mStatusReads == 3 && mArgWrites == 3);
  mAck = 0; mStatus = Unrelated;
  assert (UfsExecUicCommands (&Private, &Command) == EFI_SUCCESS && mAck == 0);
  assert ((mStatus & Unrelated) == Unrelated);
  mResult = 1;
  assert (UfsExecUicCommands (&Private, &Command) == EFI_DEVICE_ERROR);
  mReadFailure = TRUE;
  assert (UfsExecUicCommands (&Private, &Command) == EFI_DEVICE_ERROR);
  mReadFailure = FALSE; mWriteFailure = TRUE;
  assert (UfsExecUicCommands (&Private, &Command) == EFI_DEVICE_ERROR);
  mWriteFailure = FALSE;
  /* UCCS acceptance precedes a separately delayed power-mode completion. */
  Command.Opcode = UfsUicDmeSet; Command.Arg1 = 0x15710000; Command.Arg3 = 0x11;
  mResult = 0; mStatus = Unrelated | BIT4 | UFS_HC_IS_UCCS;
  assert (UfsExecUicCommands (&Private, &Command) == EFI_SUCCESS);
  assert (mPowerDispatches == 1 && mPowerReads >= 4 && !(mStatus & BIT4));
  assert ((mStatus & Unrelated) == Unrelated && mAck == BIT4);
  mUpmcrs = 0;
  assert (UfsExecUicCommands (&Private, &Command) == EFI_DEVICE_ERROR);
  mUpmcrs = 2;
  assert (UfsExecUicCommands (&Private, &Command) == EFI_DEVICE_ERROR);
  mUpmcrs = 1; mPowerNoCompletion = TRUE;
  mStatus = Unrelated | BIT4;
  assert (UfsExecUicCommands (&Private, &Command) == EFI_TIMEOUT);
  assert ((mStatus & Unrelated) == Unrelated);
  mPowerNoCompletion = FALSE; mPowerNoUccs = TRUE; mStatus = Unrelated;
  assert (UfsExecUicCommands (&Private, &Command) == EFI_TIMEOUT);
  mPowerNoUccs = FALSE; mPowerPending = FALSE; mResult = 1;
  assert (UfsExecUicCommands (&Private, &Command) == EFI_DEVICE_ERROR);
  mPowerPending = FALSE; mResult = 0;
  /* Ordinary DME_SET must not consume another owner's asynchronous event. */
  Command.Arg1 = 0x15830000; mStatus = Unrelated | BIT4;
  assert (UfsExecUicCommands (&Private, &Command) == EFI_SUCCESS);
  assert (mStatus & BIT4);
  EDKII_UFS_HC_PLATFORM_PROTOCOL Platform = {
    .Version = 3, .Callback = PlatformCallback, .SkipHceReenable = FALSE
  };
  mUfsHcPlatform = &Platform;
  mLinkFailures = 1;
  assert (UfsDeviceDetection (&Private) == EFI_SUCCESS);
  assert (mLinkAttempts == 2 && mResets == 1 && mPreLinks == 2);
  mLinkAttempts = mResets = mPreLinks = 0; mLinkFailures = 3;
  assert (UfsDeviceDetection (&Private) == EFI_DEVICE_ERROR);
  assert (mLinkAttempts == 3 && mResets == 2 && mPreLinks == 3);
  mLinkAttempts = mResets = mPreLinks = 0; mRecoveryFailure = TRUE;
  assert (UfsDeviceDetection (&Private) == EFI_DEVICE_ERROR);
  assert (mLinkAttempts == 1 && mResets == 1 && mPreLinks == 1 && mHce == 0);
  mRecoveryFailure = FALSE;
  /* SkipHceReenable retains the platform-owned enabled controller. */
  mHce = 1; Platform.SkipHceReenable = TRUE; mResets = 0;
  assert (UfsEnableHostController (&Private) == EFI_SUCCESS);
  assert (mHce == 1 && mResets == 0);
  Platform.SkipHceReenable = FALSE;
  mLinkAttempts = mResets = mPreLinks = 0; Platform.SkipLinkStartup = TRUE;
  assert (UfsDeviceDetection (&Private) == EFI_SUCCESS);
  assert (mLinkAttempts == 0 && mResets == 0 && mPreLinks == 1);
  Platform.SkipLinkStartup = FALSE;
  mDevicePresent = FALSE; mStatus = 0; mLinkFailures = 0;
  mLinkAttempts = mResets = mPreLinks = 0;
  assert (UfsDeviceDetection (&Private) == EFI_NOT_FOUND);
  assert (mLinkAttempts == 1 && mResets == 0 && mPreLinks == 1);
  mHce = 1; mStopStuck = TRUE;
  assert (UfsControllerStop (&Private) == EFI_DEVICE_ERROR && mHce == 1);
  mStopStuck = FALSE;
  assert (UfsControllerStop (&Private) == EFI_SUCCESS && mHce == 0);
  assert (UfsControllerStop (&Private) == EFI_SUCCESS && mHce == 0);
  puts ("MU UFS UIC tests passed: link retry/HCE recovery, bounded failures, skip link, asynchronous power completion/status/timeouts, W1C preservation and MMIO errors");
  return 0;
}
