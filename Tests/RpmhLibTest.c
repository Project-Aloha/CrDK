/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#undef NULL
#include <Uefi.h>
#include <Library/rpmh.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

#define TEST_DRV_BASE          0x00100000U
#define TEST_TCS_OFFSET        0x00001000U
#define TEST_MMIO_SIZE         0x00004000U
#define TEST_TCS_STRIDE        0x000002A0U
#define TEST_CMD_STRIDE        0x00000018U
#define TEST_IRQ_ENABLE        0x00000000U
#define TEST_IRQ_STATUS        0x00000004U
#define TEST_IRQ_CLEAR         0x00000008U
#define TEST_CHILD_CONFIG      0x0000000CU
#define TEST_CMD_WAIT          0x00000020U
#define TEST_CONTROL           0x00000024U
#define TEST_CMD_ENABLE        0x0000002CU
#define TEST_CMD_MSGID         0x00000034U
#define TEST_CMD_ADDR          0x00000038U
#define TEST_CMD_STATUS        0x00000040U
#define TEST_MSG_RESPONSE      BIT(8)
#define TEST_STATUS_ISSUED     BIT(8)
#define TEST_STATUS_COMPLETED  BIT(16)
#define TEST_CONTROL_ENABLE    BIT(16)
#define TEST_CONTROL_TRIGGER   BIT(24)
#define TEST_VRM_ADDRESS       0x00040020U

typedef enum {
  MockCompleteByPolling,
  MockCompleteWithIrqRace,
  MockNeverComplete,
  MockIncompleteStatus,
  MockCompleteAtTimeoutHandoff,
  MockControlReadbackTimeout
} MOCK_COMPLETION_MODE;

STATIC UINT32               mMmio[TEST_MMIO_SIZE / sizeof(UINT32)];
STATIC MOCK_COMPLETION_MODE mMode;
STATIC CR_INTERRUPT_CONFIG *mInterrupt;
STATIC UINT32               mTriggerCount[2];
STATIC UINT32               mTriggerEnabled[2];
STATIC UINT32               mTriggerWait[2];
STATIC UINT32               mTriggerMsgId[2][4];
STATIC UINT32               mIrqClearWrites;
STATIC UINT32               mRaceCallbacks;
STATIC UINT32               mHandoffCallbacks;
STATIC BOOLEAN              mRaceArmed;
STATIC BOOLEAN              mHandoffArmed;
STATIC BOOLEAN              mInsideRaceCallback;
STATIC UINT64               mDelayUs;

STATIC VOID
RaiseCompletion(UINT32 Tcs, BOOLEAN Completed);

STATIC UINTN
RegisterIndex(UINTN Address)
{
  assert(Address >= TEST_DRV_BASE);
  assert(Address < TEST_DRV_BASE + TEST_MMIO_SIZE);
  assert(((Address - TEST_DRV_BASE) & 3U) == 0);
  return (Address - TEST_DRV_BASE) / sizeof(UINT32);
}

STATIC UINTN
TcsAddress(UINT32 Tcs, UINT32 Register)
{
  return TEST_DRV_BASE + TEST_TCS_OFFSET + TEST_TCS_STRIDE * Tcs + Register;
}

STATIC UINTN
TcsCommandAddress(UINT32 Tcs, UINT32 Command, UINT32 Register)
{
  return TcsAddress(Tcs, Register + TEST_CMD_STRIDE * Command);
}

STATIC UINT32
RawRead(UINTN Address)
{
  return mMmio[RegisterIndex(Address)];
}

STATIC VOID
RawWrite(UINTN Address, UINT32 Value)
{
  mMmio[RegisterIndex(Address)] = Value;
}

UINT32
CrMmioRead32(IN UINTN Address)
{
  if ((Address == TcsAddress(0, TEST_IRQ_STATUS)) && mHandoffArmed &&
      mDelayUs >= 1000 && !mInsideRaceCallback) {
    assert(mInterrupt != NULL && mInterrupt->Handler != NULL);
    mHandoffArmed = FALSE;
    RaiseCompletion(0, TRUE);
    mInsideRaceCallback = TRUE;
    mHandoffCallbacks++;
    mInterrupt->Handler(mInterrupt->Param);
    mInsideRaceCallback = FALSE;
  }
  if ((Address == TcsAddress(0, TEST_IRQ_STATUS)) && mRaceArmed &&
      !mInsideRaceCallback) {
    assert(mInterrupt != NULL && mInterrupt->Handler != NULL);
    mRaceArmed = FALSE;
    mInsideRaceCallback = TRUE;
    mRaceCallbacks++;
    mInterrupt->Handler(mInterrupt->Param);
    mInsideRaceCallback = FALSE;
  }
  return RawRead(Address);
}

STATIC VOID
RecordTriggeredTcs(UINT32 Tcs)
{
  UINT32 Command;
  UINT32 Enabled;

  Enabled = RawRead(TcsAddress(Tcs, TEST_CMD_ENABLE));
  mTriggerEnabled[Tcs] = Enabled;
  mTriggerWait[Tcs] = RawRead(TcsAddress(Tcs, TEST_CMD_WAIT));
  for (Command = 0; Command < 4; Command++) {
    mTriggerMsgId[Tcs][Command] =
        RawRead(TcsCommandAddress(Tcs, Command, TEST_CMD_MSGID));
    if ((Enabled & BIT(Command)) == 0) {
      continue;
    }
  }
}

STATIC VOID
RaiseCompletion(UINT32 Tcs, BOOLEAN Completed)
{
  UINT32 Command;
  UINT32 Enabled;
  UINT32 Status;

  Enabled = RawRead(TcsAddress(Tcs, TEST_CMD_ENABLE));
  for (Command = 0; Command < 4; Command++) {
    if ((Enabled & BIT(Command)) == 0) {
      continue;
    }
    Status = TEST_STATUS_ISSUED;
    if (Completed) {
      Status |= TEST_STATUS_COMPLETED;
    }
    RawWrite(TcsCommandAddress(Tcs, Command, TEST_CMD_STATUS), Status);
  }
  RawWrite(TcsAddress(0, TEST_IRQ_STATUS),
           RawRead(TcsAddress(0, TEST_IRQ_STATUS)) | (UINT32)BIT(Tcs));
}

UINT32
CrMmioWrite32(IN UINTN Address, IN UINT32 Value)
{
  UINT32 Tcs;

  if (Address == TcsAddress(0, TEST_IRQ_CLEAR)) {
    mIrqClearWrites++;
    RawWrite(TcsAddress(0, TEST_IRQ_STATUS),
             RawRead(TcsAddress(0, TEST_IRQ_STATUS)) & ~Value);
    return Value;
  }

  for (Tcs = 0; Tcs < 2; Tcs++) {
    if (Address != TcsAddress(Tcs, TEST_CONTROL)) {
      continue;
    }
    if (mMode == MockControlReadbackTimeout) {
      return Value;
    }
    RawWrite(Address, Value);
    if ((Value & (TEST_CONTROL_ENABLE | TEST_CONTROL_TRIGGER)) ==
        (TEST_CONTROL_ENABLE | TEST_CONTROL_TRIGGER)) {
      mTriggerCount[Tcs]++;
      RecordTriggeredTcs(Tcs);
      if (mMode == MockCompleteByPolling ||
          mMode == MockCompleteWithIrqRace ||
          mMode == MockIncompleteStatus) {
        RaiseCompletion(Tcs, mMode != MockIncompleteStatus);
      }
      if (mMode == MockCompleteWithIrqRace) {
        mRaceArmed = TRUE;
      } else if (mMode == MockCompleteAtTimeoutHandoff) {
        mHandoffArmed = TRUE;
      }
    }
    return Value;
  }

  RawWrite(Address, Value);
  return Value;
}

VOID
CrTestSleep(IN UINT64 Microseconds)
{
  mDelayUs += Microseconds;
}

VOID EFIAPI
DebugPrint(IN UINTN ErrorLevel, IN CONST CHAR8 *Format, ...)
{
  (VOID)ErrorLevel;
  (VOID)Format;
}

UINT32 EFIAPI
InterlockedCompareExchange32(
    IN OUT volatile UINT32 *Value,
    IN UINT32 CompareValue,
    IN UINT32 ExchangeValue)
{
  UINT32 Expected = CompareValue;

  __atomic_compare_exchange_n(
      Value, &Expected, ExchangeValue, FALSE, __ATOMIC_SEQ_CST,
      __ATOMIC_SEQ_CST);
  return Expected;
}

SPIN_LOCK *EFIAPI
InitializeSpinLock(OUT SPIN_LOCK *SpinLock)
{
  *SpinLock = 1;
  return SpinLock;
}

SPIN_LOCK *EFIAPI
AcquireSpinLock(IN OUT SPIN_LOCK *SpinLock)
{
  assert(*SpinLock == 1);
  *SpinLock = 2;
  return SpinLock;
}

SPIN_LOCK *EFIAPI
ReleaseSpinLock(IN OUT SPIN_LOCK *SpinLock)
{
  assert(*SpinLock == 2);
  *SpinLock = 1;
  return SpinLock;
}

CR_STATUS
CrRegisterInterrupt(CR_INTERRUPT_CONFIG *InterruptConfig)
{
  assert(InterruptConfig != NULL);
  assert(mInterrupt == NULL);
  mInterrupt = InterruptConfig;
  return CR_SUCCESS;
}

CR_STATUS
CrUnregisterInterrupt(CR_INTERRUPT_CONFIG *InterruptConfig)
{
  if (InterruptConfig == NULL || mInterrupt != InterruptConfig) {
    return CR_NOT_FOUND;
  }
  mInterrupt = NULL;
  return CR_SUCCESS;
}

STATIC VOID
ResetMock(VOID)
{
  memset(mMmio, 0, sizeof(mMmio));
  memset(mTriggerCount, 0, sizeof(mTriggerCount));
  memset(mTriggerEnabled, 0, sizeof(mTriggerEnabled));
  memset(mTriggerWait, 0, sizeof(mTriggerWait));
  memset(mTriggerMsgId, 0, sizeof(mTriggerMsgId));
  mMode = MockCompleteByPolling;
  mInterrupt = NULL;
  mIrqClearWrites = 0;
  mRaceCallbacks = 0;
  mHandoffCallbacks = 0;
  mRaceArmed = FALSE;
  mHandoffArmed = FALSE;
  mInsideRaceCallback = FALSE;
  mDelayUs = 0;
  RawWrite(TEST_DRV_BASE, 3U << 16);
  RawWrite(TEST_DRV_BASE + TEST_CHILD_CONFIG, 2U | (4U << 27));
}

STATIC VOID
InitContext(RpmhDeviceContext *Context)
{
  RpmhDeviceContext *ContextPointer;

  memset(Context, 0, sizeof(*Context));
  Context->drv_base_address = TEST_DRV_BASE;
  Context->tcs_offset = TEST_TCS_OFFSET;
  Context->drv_id = 0;
  Context->tcs_config.active_tcs.tcs_count = 2;
  ContextPointer = Context;
  assert(RpmhLibInit(&ContextPointer) == CR_SUCCESS);
  assert(ContextPointer == Context);
  assert(Context->NumCmdsPerTcs == 4);
  assert(Context->tcs_config.active_tcs.mask == 3);
  assert(RawRead(TcsAddress(0, TEST_IRQ_ENABLE)) == 3);
  mIrqClearWrites = 0;
}

STATIC VOID
TestPollingCompletionAndCommandFlags(VOID)
{
  RpmhDeviceContext Context;
  RpmhTcsCmd Commands[2] = {
      {TEST_VRM_ADDRESS, 900, 1, 0},
      {TEST_VRM_ADDRESS + 4, 1, 0, 0},
  };

  ResetMock();
  InitContext(&Context);
  assert(RpmhWrite(&Context, Commands, 2) == CR_SUCCESS);
  assert(Context.TcsBusy == 0);
  assert(mTriggerCount[0] == 1 && mTriggerCount[1] == 0);
  assert(mTriggerEnabled[0] == (BIT(0) | BIT(1)));
  assert(mTriggerWait[0] == BIT(0));
  assert((mTriggerMsgId[0][0] & TEST_MSG_RESPONSE) != 0);
  assert((mTriggerMsgId[0][1] & TEST_MSG_RESPONSE) != 0);
  assert(mIrqClearWrites == 1);
  assert(RpmhLibDeinit(&Context) == CR_SUCCESS);
}

STATIC VOID
TestIrqAndPollingRetireOnlyOnce(VOID)
{
  RpmhDeviceContext Context;
  RpmhTcsCmd Command = {TEST_VRM_ADDRESS, 900, 0, 0};

  ResetMock();
  InitContext(&Context);
  mMode = MockCompleteWithIrqRace;
  assert(RpmhWrite(&Context, &Command, 1) == CR_SUCCESS);
  assert(mRaceCallbacks == 1);
  assert(mIrqClearWrites == 1);
  assert(Context.TcsBusy == 0);
  assert(Context.TcsRetiring == 0);
  assert(RpmhLibDeinit(&Context) == CR_SUCCESS);
}

STATIC VOID
TestCommandStatusFailureKeepsTcsOwned(VOID)
{
  RpmhDeviceContext Context;
  RpmhTcsCmd Command = {TEST_VRM_ADDRESS, 900, 0, 1};

  ResetMock();
  InitContext(&Context);
  mMode = MockIncompleteStatus;
  assert(RpmhWrite(&Context, &Command, 1) == CR_DEVICE_ERROR);
  assert(Context.TcsBusy == BIT(0));
  assert(Context.TcsFailed == BIT(0));
  assert(RawRead(TcsAddress(0, TEST_CMD_ENABLE)) == BIT(0));
  assert(RpmhLibDeinit(&Context) == CR_SUCCESS);
}

STATIC VOID
TestCompletionDuringTimeoutHandoff(VOID)
{
  RpmhDeviceContext Context;
  RpmhTcsCmd Command = {TEST_VRM_ADDRESS, 900, 0, 1};

  ResetMock();
  InitContext(&Context);
  mMode = MockCompleteAtTimeoutHandoff;
  assert(RpmhWrite(&Context, &Command, 1) == CR_SUCCESS);
  assert(mDelayUs == 1000);
  assert(mHandoffCallbacks == 1);
  assert(Context.TcsBusy == 0);
  assert(mIrqClearWrites == 1);
  assert(RpmhLibDeinit(&Context) == CR_SUCCESS);
}

STATIC VOID
TestControlReadbackTimeoutIsReturned(VOID)
{
  RpmhDeviceContext Context;
  RpmhTcsCmd Command = {TEST_VRM_ADDRESS, 900, 0, 1};

  ResetMock();
  InitContext(&Context);
  mMode = MockControlReadbackTimeout;
  assert(RpmhWrite(&Context, &Command, 1) == CR_TIMEOUT);
  assert(Context.TcsBusy == BIT(0));
  assert(Context.TcsFailed == BIT(0));
  assert(mTriggerCount[0] == 0 && mDelayUs == 1000);
  assert(RpmhLibDeinit(&Context) == CR_SUCCESS);
}

STATIC VOID
TestTimeoutBlocksRollbackUntilLateCompletion(VOID)
{
  RpmhDeviceContext Context;

  ResetMock();
  InitContext(&Context);
  mMode = MockNeverComplete;
  assert(RpmhEnableVreg(&Context, TEST_VRM_ADDRESS, TRUE) == CR_TIMEOUT);
  assert(Context.TcsBusy == BIT(0));
  assert(mTriggerCount[0] == 1 && mTriggerCount[1] == 0);

  /* Enable, voltage and mode offsets share one VRM resource.  The free TCS1
   * must not carry a rollback past the timed-out request in TCS0. */
  assert(RpmhEnableVreg(&Context, TEST_VRM_ADDRESS, FALSE) == CR_BUSY);
  assert(RpmhSetVregVoltage(&Context, TEST_VRM_ADDRESS, 0) == CR_BUSY);
  assert(RpmhSetVregMode(&Context, TEST_VRM_ADDRESS, 0) == CR_BUSY);
  assert(mTriggerCount[0] == 1 && mTriggerCount[1] == 0);
  assert(RawRead(TcsAddress(1, TEST_CMD_ENABLE)) == 0);

  RawWrite(TcsCommandAddress(0, 0, TEST_CMD_STATUS),
           TEST_STATUS_ISSUED | TEST_STATUS_COMPLETED);
  RawWrite(TcsAddress(0, TEST_IRQ_STATUS), BIT(0));
  assert(mInterrupt != NULL && mInterrupt->Handler != NULL);
  mInterrupt->Handler(mInterrupt->Param);
  assert(Context.TcsBusy == 0);
  assert(RawRead(TcsAddress(0, TEST_CMD_ENABLE)) == 0);

  mMode = MockCompleteByPolling;
  assert(RpmhEnableVreg(&Context, TEST_VRM_ADDRESS, FALSE) == CR_SUCCESS);
  assert(mTriggerCount[0] == 2 && mTriggerCount[1] == 0);
  assert(Context.TcsBusy == 0);
  assert(RpmhLibDeinit(&Context) == CR_SUCCESS);
}

int
main(void)
{
  TestPollingCompletionAndCommandFlags();
  TestIrqAndPollingRetireOnlyOnce();
  TestCommandStatusFailureKeepsTcsOwned();
  TestCompletionDuringTimeoutHandoff();
  TestControlReadbackTimeoutIsReturned();
  TestTimeoutBlocksRollbackUntilLateCompletion();
  puts("RPMh: synchronous completion, failure retention and rollback ordering passed");
  return 0;
}
