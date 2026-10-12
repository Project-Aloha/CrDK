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
STATIC UINT32               mWrites;

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
  ++mWrites;

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
  mWrites = 0;
  RawWrite(TEST_DRV_BASE, 3U << 16);
  RawWrite(TEST_DRV_BASE + TEST_CHILD_CONFIG, 2U | (4U << 27));
}

STATIC VOID
InitContext(RpmhDeviceContext *Context)
{
  RpmhDeviceContext *ContextPointer;

  memset(Context, 0, sizeof(*Context));
  Context->drv_base_address = TEST_DRV_BASE;
  Context->drv_size = TEST_MMIO_SIZE;
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

STATIC VOID TestConfigBoundsBeforeWrites(VOID) {
  RpmhDeviceContext C = {0}, Saved;
  UINT8 Data[9] = {0,0,0x10,0,0,2,0,0,0};
  C.drv_base_address = TEST_DRV_BASE; C.drv_size = TEST_MMIO_SIZE;
  Saved = C;
  for (UINTN N=0; N<9; ++N) {
    assert(RpmhLibLoadConfig(&C,Data,N) == CR_INVALID_PARAMETER);
    assert(memcmp(&C,&Saved,sizeof(C)) == 0);
  }
  assert(RpmhLibLoadConfig(&C,Data,sizeof(Data)) == CR_SUCCESS);
  assert(C.drv_id == 0 && C.tcs_offset == TEST_TCS_OFFSET && C.tcs_config.active_tcs.tcs_count == 2);
  Saved = C;
  Data[5] = 33;
  assert(RpmhLibLoadConfig(&C,Data,sizeof(Data)) == CR_INVALID_PARAMETER);
  assert(memcmp(&C,&Saved,sizeof(C)) == 0); Data[5] = 2;
  Data[1] = 1;
  assert(RpmhLibLoadConfig(&C,Data,sizeof(Data)) == CR_INVALID_PARAMETER);
  assert(memcmp(&C,&Saved,sizeof(C)) == 0); Data[1] = 0;
  ResetMock(); C.drv_size = 15;
  RpmhDeviceContext *Pointer = &C;
  assert(RpmhLibInit(&Pointer) == CR_INVALID_PARAMETER && mWrites == 0 && mInterrupt == NULL);
  // Last command status word must fit in full, rather than only its start.
  C.drv_size = TEST_TCS_OFFSET + TEST_TCS_STRIDE + TEST_CMD_STATUS + 3*TEST_CMD_STRIDE + 3 + 4;
  assert(RpmhLibInit(&Pointer) == CR_INVALID_PARAMETER && mWrites == 0 && mInterrupt == NULL);
  ++C.drv_size;
  assert(RpmhLibInit(&Pointer) == CR_SUCCESS && mInterrupt != NULL);
  assert(RpmhLibDeinit(&C) == CR_SUCCESS);
  C.tcs_offset = 0xfffffffcU; C.drv_size = TEST_MMIO_SIZE; mWrites = 0;
  assert(RpmhLibInit(&Pointer) == CR_INVALID_PARAMETER && mWrites == 0 && mInterrupt == NULL);
}

STATIC VOID TestVrmSpanBeforeWrites(VOID) {
  RpmhDeviceContext C;ResetMock();InitContext(&C);
  UINT32 Before=mWrites;
  /* A mode at fffffffc+8 otherwise wraps to address4 and reaches hardware. */
  assert(RpmhSetVregMode(&C,0xfffffffcU,7)==CR_INVALID_PARAMETER);
  assert(RpmhEnableVreg(&C,0xfffffff8U,TRUE)==CR_INVALID_PARAMETER);
  assert(RpmhSetVregVoltage(&C,TEST_VRM_ADDRESS+1,900)==CR_INVALID_PARAMETER);
  /* These APIs take a base, not a wire command offset within its VRM slot. */
  for (UINT32 Offset=4;Offset<16;Offset+=4) {
    assert(RpmhSetVregVoltage(&C,TEST_VRM_ADDRESS+Offset,900)==CR_INVALID_PARAMETER);
    assert(RpmhEnableVreg(&C,TEST_VRM_ADDRESS+Offset,TRUE)==CR_INVALID_PARAMETER);
    assert(RpmhSetVregMode(&C,TEST_VRM_ADDRESS+Offset,7)==CR_INVALID_PARAMETER);
  }
  assert(RpmhSetVregVoltage(&C,TEST_VRM_ADDRESS,0x2000)==CR_INVALID_PARAMETER);
  assert(RpmhSetVregMode(&C,TEST_VRM_ADDRESS,8)==CR_INVALID_PARAMETER);
  assert(mWrites==Before && mTriggerCount[0]==0 && mTriggerCount[1]==0);
  assert(RpmhLibDeinit(&C)==CR_SUCCESS);
}

STATIC VOID TestControlTcsIsSeparate(VOID) {
  /* Kona reports DRV2 v2.0, 8 command TCS, 16 commands/TCS. The DT assigns
     active2/sleep3/wake3/control1: control must consume no ninth slot. */
  for (UINT32 Version=2;Version<=3;++Version) {
    RpmhDeviceContext C={0},*P=&C;
    UINT8 Data[9]={2,0,0x0d,0,0,2,3,3,1};
    /* Wire tcs_offset is little endian 0x00000d00. */
    ResetMock();RawWrite(TEST_DRV_BASE,Version<<16);
    RawWrite(TEST_DRV_BASE+TEST_CHILD_CONFIG,(8U<<12)|(16U<<27));
    C.drv_base_address=TEST_DRV_BASE;C.drv_size=TEST_MMIO_SIZE;
    assert(RpmhLibLoadConfig(&C,Data,sizeof Data)==CR_SUCCESS);
    UINT32 End=0xd00+7*0x2a0+(Version==2?0x40:0x44)+15*(Version==2?0x14:0x18)+4;
    C.drv_size=End;
    assert(RpmhLibInit(&P)==CR_SUCCESS&&C.NumCmdsPerTcs==16);
    assert(C.tcs_config.active_tcs.offset==0&&C.tcs_config.active_tcs.mask==0x03);
    assert(C.tcs_config.sleep_tcs.offset==2&&C.tcs_config.sleep_tcs.mask==0x1c);
    assert(C.tcs_config.wake_tcs.offset==5&&C.tcs_config.wake_tcs.mask==0xe0);
    assert(C.tcs_config.control_tcs.tcs_count==1&&!C.tcs_config.control_tcs.mask&&!C.tcs_config.control_tcs.offset);
    assert(RawRead(TEST_DRV_BASE+0xd00)==3);
    assert(RpmhLibDeinit(&C)==CR_SUCCESS);
    mWrites=0;C.drv_size=End-1;
    assert(RpmhLibInit(&P)==CR_INVALID_PARAMETER&&mWrites==0&&mInterrupt==NULL);
    C.drv_size=End;
    mWrites=0;C.tcs_config.wake_tcs.tcs_count=4;
    assert(RpmhLibInit(&P)==CR_DEVICE_ERROR&&mWrites==0&&mInterrupt==NULL);
    C.tcs_config.wake_tcs.tcs_count=3;C.tcs_config.control_tcs.tcs_count=4;
    assert(RpmhLibInit(&P)==CR_DEVICE_ERROR&&mWrites==0&&mInterrupt==NULL);
  }
  RpmhDeviceContext C={0};UINT8 Data[9]={0,0,0x10,0,0,2,15,15,1};
  assert(RpmhLibLoadConfig(&C,Data,sizeof Data)==CR_SUCCESS);
  Data[8]=4;assert(RpmhLibLoadConfig(&C,Data,sizeof Data)==CR_INVALID_PARAMETER);
}

int
main(void)
{
  TestControlTcsIsSeparate();
  TestVrmSpanBeforeWrites();
  TestConfigBoundsBeforeWrites();
  TestPollingCompletionAndCommandFlags();
  TestIrqAndPollingRetireOnlyOnce();
  TestCommandStatusFailureKeepsTcsOwned();
  TestCompletionDuringTimeoutHandoff();
  TestControlReadbackTimeoutIsReturned();
  TestTimeoutBlocksRollbackUntilLateCompletion();
  puts("RPMh: synchronous completion, failure retention and rollback ordering passed");
  return 0;
}
